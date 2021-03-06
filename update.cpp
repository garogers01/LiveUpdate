/**
 * Master thesis
 * by Alf-Andre Walla 2016-2017
 *
**/
#include "liveupdate.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "elf.h"
#include "storage.hpp"
#include <kernel/os.hpp>
#include <hw/devices.hpp>

//#define LPRINT(x, ...) printf(x, ##__VA_ARGS__);
#define LPRINT(x, ...) /** x **/

static const int SECT_SIZE   = 512;
static const int ELF_MINIMUM = 164;

extern "C"
void solo5_exec(const char*, size_t);
static void* HOTSWAP_AREA = (void*) 0x8000;
extern "C" void  hotswap(const char*, int, char*, uintptr_t, void*);
extern "C" char  __hotswap_length;
extern "C" void  hotswap64(char*, const char*, int, uintptr_t, void*);
extern uint32_t  hotswap64_len;
extern "C" void* __os_store_soft_reset(const void*, size_t);
// kernel area
extern char _ELF_START_;
extern char _end;
// heap area
extern char* heap_begin;
extern char* heap_end;
// turn this off to reduce liveupdate times at the cost of extra checks
bool LIVEUPDATE_PERFORM_SANITY_CHECKS = true;

using namespace liu;

static size_t update_store_data(void* location, LiveUpdate::storage_func, const buffer_t*);

template <typename Class>
inline bool validate_header(const Class* hdr)
{
    return hdr->e_ident[0] == 0x7F &&
           hdr->e_ident[1] == 'E'  &&
           hdr->e_ident[2] == 'L'  &&
           hdr->e_ident[3] == 'F';
}

void LiveUpdate::begin(void*        location,
                       buffer_t     blob,
                       storage_func storage_callback)
{
  LPRINT("LiveUpdate::begin(%p, %p:%d, ...)\n", location, blob.data(), (int) blob.size());
  // 1. turn off interrupts
  asm volatile("cli");

  // use area provided to us directly, which we will assume
  // is far enough into heap to not get overwritten by hotswap.
  // even then, it's still guaranteed to work: the copy mechanism
  // is implemented in hotswap.cpp and copies forwards. the
  // blobs are separated by at least one old kernel size and
  // some early heap allocations, which is at least 1mb, while
  // the copy mechanism just copies single bytes.
  const char* update_area  = blob.data();
  char* storage_area = (char*) location;

  // validate not overwriting heap, kernel area and other things
  if (storage_area < (char*) 0x200) {
    throw std::runtime_error("LiveUpdate storage area is (probably) a null pointer");
  }
  if (storage_area >= &_ELF_START_ && storage_area < &_end) {
    throw std::runtime_error("LiveUpdate storage area is inside kernel area");
  }
  if (storage_area >= heap_begin && storage_area < heap_end) {
    throw std::runtime_error("LiveUpdate storage area is inside the heap area");
  }
  if (storage_area >= (char*) OS::heap_max()) {
    throw std::runtime_error("LiveUpdate storage area is outside physical memory");
  }
  if (storage_area >= (char*) OS::heap_max() - 0x10000) {
    throw std::runtime_error("LiveUpdate storage area needs at least 64kb memory");
  }

  // search for ELF header
  LPRINT("* Looking for ELF header at %p\n", update_area);
  const char* binary  = &update_area[0];
  const auto* hdr = (const Elf32_Ehdr*) binary;
  if (!validate_header<Elf32_Ehdr>(hdr))
  {
    /// try again with 1 sector offset (skip bootloader)
    binary   = &update_area[SECT_SIZE];
    hdr      = (const Elf32_Ehdr*) binary;

    if (!validate_header<Elf32_Ehdr>(hdr))
    {
      /// failed to find elf header at sector 0 and 1
      /// simply return
      throw std::runtime_error("Could not find any ELF header in blob");
    }
  }
  LPRINT("* Found ELF header\n");

  size_t    expected_total = 0;
  uintptr_t start_offset = 0;

  const char* bin_data  = nullptr;
  int         bin_len   = 0;
  char*       phys_base = nullptr;

  if (hdr->e_ident[EI_CLASS] == ELFCLASS32)
  {
    /// note: this assumes section headers are at the end
    expected_total =
        hdr->e_shnum * hdr->e_shentsize +
        hdr->e_shoff;
    /// program entry point
    start_offset = hdr->e_entry;
    // get offsets for the new service from program header
    auto* phdr = (Elf32_Phdr*) &binary[hdr->e_phoff];
    bin_data  = &binary[phdr->p_offset];
    bin_len   = phdr->p_filesz;
    phys_base = (char*) (uintptr_t) phdr->p_paddr;
  }
  else {
    auto* ehdr = (Elf64_Ehdr*) hdr;
    /// note: this assumes section headers are at the end
    expected_total =
        ehdr->e_shnum * ehdr->e_shentsize +
        ehdr->e_shoff;
    /// program entry point
    start_offset = ehdr->e_entry;
    // get offsets for the new service from program header
    auto* phdr = (Elf64_Phdr*) &binary[ehdr->e_phoff];
    bin_data  = &binary[phdr->p_offset];
    bin_len   = phdr->p_filesz;
    phys_base = (char*) phdr->p_paddr;
  }

  if (blob.size() < expected_total || expected_total < ELF_MINIMUM)
  {
    fprintf(stderr,
        "*** There was a mismatch between blob length and expected ELF file size:\n");
    fprintf(stderr,
        "EXPECTED: %u byte\n",  (uint32_t) expected_total);
    fprintf(stderr,
        "ACTUAL:   %u bytes\n", (uint32_t) blob.size());
    throw std::runtime_error("ELF file was incomplete");
  }
  LPRINT("* Validated ELF header\n");

  // _start() entry point
  LPRINT("* _start is located at %#x\n", start_offset);

  // save ourselves if function passed
  update_store_data(storage_area, storage_callback, &blob);

  // 2. flush all devices with flush() interface
  hw::Devices::flush_all();
  // 3. deactivate all PCI devices and mask all MSI-X vectors
  // NOTE: there are some nasty side effects from calling this
  //hw::Devices::deactivate_all();

  // store soft-resetting stuff
#ifdef PLATFORM_x86_solo5
  void* sr_data = nullptr;
#else
  extern const std::pair<const char*, size_t> get_rollback_location();
  const auto rollback = get_rollback_location();
  void* sr_data = __os_store_soft_reset(rollback.first, rollback.second);
#endif

  // get offsets for the new service from program header
  if (bin_data == nullptr ||
      phys_base == nullptr || bin_len <= 64) {
    throw std::runtime_error("ELF program header malformed");
  }

  //char* phys_base = (char*) (start_offset & 0xffff0000);
  LPRINT("* Physical base address is %p...\n", phys_base);

  // replace ourselves and reset by jumping to _start
  LPRINT("* Replacing self with %d bytes and jumping to %#x\n", bin_len, start_offset);

#ifdef PLATFORM_x86_solo5
  solo5_exec(blob.data(), blob.size());
  throw std::runtime_error("solo5_exec returned");
#else
# ifdef ARCH_i686
    // copy hotswapping function to sweet spot
    memcpy(HOTSWAP_AREA, (void*) &hotswap, &__hotswap_length - (char*) &hotswap);
    /// the end
    ((decltype(&hotswap)) HOTSWAP_AREA)(bin_data, bin_len, phys_base, start_offset, sr_data);
# elif defined(ARCH_x86_64)
    // copy hotswapping function to sweet spot
    memcpy(HOTSWAP_AREA, (void*) &hotswap64, hotswap64_len);
    /// the end
    ((decltype(&hotswap64)) HOTSWAP_AREA)(phys_base, bin_data, bin_len, start_offset, sr_data);
# else
#    error "Unimplemented architecture"
# endif
#endif
}
void LiveUpdate::restore_environment()
{
  // enable interrupts again
  asm volatile("sti");
}
size_t LiveUpdate::store(void* location, storage_func func)
{
  return update_store_data(location, func, nullptr);
}

size_t LiveUpdate::stored_data_length(void* location)
{
  auto* storage = (storage_header*) location;
  
  if (LIVEUPDATE_PERFORM_SANITY_CHECKS)
  {
    /// sanity check
    if (storage->validate() == false)
        throw std::runtime_error("Failed sanity check on LiveUpdate storage area");
  }

  /// return length of the whole area
  return storage->total_bytes();
}

size_t update_store_data(void* location, LiveUpdate::storage_func func, const buffer_t* blob)
{
  // create storage header in the fixed location
  new (location) storage_header();
  auto* storage = (storage_header*) location;

  /// callback for storing stuff, if provided
  if (func != nullptr)
  {
    Storage wrapper {*storage};
    func(wrapper, blob);
  }

  /// finalize
  storage->finalize();

  /// return length (and perform sanity check)
  return LiveUpdate::stored_data_length(location);
}

/// struct Storage

void Storage::put_marker(uid id)
{
  hdr.add_marker(id);
}
void Storage::add_int(uid id, int value)
{
  hdr.add_int(id, value);
}
void Storage::add_string(uid id, const std::string& str)
{
  hdr.add_string(id, str);
}
void Storage::add_buffer(uid id, const buffer_t& blob)
{
  hdr.add_buffer(id, blob.data(), blob.size());
}
void Storage::add_buffer(uid id, const void* buf, size_t len)
{
  hdr.add_buffer(id, (const char*) buf, len);
}
void Storage::add_vector(uid id, const void* buf, size_t count, size_t esize)
{
  hdr.add_vector(id, buf, count, esize);
}
void Storage::add_string_vector(uid id, const std::vector<std::string>& vec)
{
  hdr.add_string_vector(id, vec);
}

#include "serialize_tcp.hpp"
void Storage::add_connection(uid id, Connection_ptr conn)
{
  hdr.add_struct(TYPE_TCP, id,
  [&conn] (char* location) -> int {
    // return size of all the serialized data
    return conn->serialize_to(location);
  });
}
