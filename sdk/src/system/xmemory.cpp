/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>

#if REX_PLATFORM_MAC
#include <sys/mman.h>
#endif

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/stream.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

// TODO(benvanik): move xbox.h out
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(protect_zero, true, "Memory", "Protect the zero page from reads and writes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(protect_on_release, false, "Memory",
                    "Protect released memory to prevent accesses");

REXCVAR_DEFINE_BOOL(scribble_heap, false, "Memory", "Scribble 0xCD into all allocated heap memory");

namespace rex::memory {

uint32_t get_page_count(uint32_t value, uint32_t page_size, uint32_t page_size_shift) {
  return rex::round_up(value, page_size) >> page_size_shift;
}

/**
 * Memory map:
 * 0x00000000 - 0x3FFFFFFF (1024mb) - virtual 4k pages
 * 0x40000000 - 0x7FFFFFFF (1024mb) - virtual 64k pages
 * 0x80000000 - 0x8BFFFFFF ( 192mb) - xex 64k pages
 * 0x8C000000 - 0x8FFFFFFF (  64mb) - xex 64k pages (encrypted)
 * 0x90000000 - 0x9FFFFFFF ( 256mb) - xex 4k pages
 * 0xA0000000 - 0xBFFFFFFF ( 512mb) - physical 64k pages
 * 0xC0000000 - 0xDFFFFFFF          - physical 16mb pages
 * 0xE0000000 - 0xFFFFFFFF          - physical 4k pages
 *
 * We use the host OS to create an entire addressable range for this. That way
 * we don't have to emulate a TLB. It'd be really cool to pass through page
 * sizes or use madvice to let the OS know what to expect.
 *
 * We create our own heap of committed memory that lives at
 * memory_HEAP_LOW to memory_HEAP_HIGH - all normal user allocations
 * come from there. Since the Xbox has no paging, we know that the size of
 * this heap will never need to be larger than ~512MB (realistically, smaller
 * than that). We place it far away from the XEX data and keep the memory
 * around it uncommitted so that we have some warning if things go astray.
 *
 * For XEX/GPU/etc data we allow placement allocations (base_address != 0) and
 * commit the requested memory as needed. This bypasses the standard heap, but
 * XEXs should never be overwriting anything so that's fine. We can also query
 * for previous commits and assert that we really isn't committing twice.
 *
 * GPU memory is mapped onto the lower 512mb of the virtual 4k range (0).
 * So 0xA0000000 = 0x00000000. A more sophisticated allocator could handle
 * this.
 */

static memory::Memory* active_memory_ = nullptr;

void CrashDump() {
  static std::atomic<int> in_crash_dump(0);
  if (in_crash_dump.fetch_add(1)) {
    // rex::FatalError(
    //     "Hard crash: the memory system crashed while dumping a crash dump.");
    return;
  }
  active_memory_->DumpMap();
  --in_crash_dump;
}

Memory::Memory() {
  system_page_size_ = uint32_t(rex::memory::page_size());
  system_allocation_granularity_ = uint32_t(rex::memory::allocation_granularity());
  assert_zero(active_memory_);
  active_memory_ = this;
}

Memory::~Memory() {
  assert_true(active_memory_ == this);
  active_memory_ = nullptr;

  // Uninstall the MMIO handler, as we won't be able to service more requests.
  mmio_handler_.reset();

  for (auto invalidation_callback : physical_memory_invalidation_callbacks_) {
    delete invalidation_callback;
  }

  heaps_.v00000000.Dispose();
  heaps_.v40000000.Dispose();
  heaps_.v80000000.Dispose();
  heaps_.v90000000.Dispose();
  heaps_.vA0000000.Dispose();
  heaps_.vC0000000.Dispose();
  heaps_.vE0000000.Dispose();
  heaps_.physical.Dispose();

  // Unmap all views and close mapping.
  if (mapping_ != rex::memory::kFileMappingHandleInvalid) {
    UnmapViews();
    rex::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_base_ = nullptr;
    mapping_ = rex::memory::kFileMappingHandleInvalid;
  }

  virtual_membase_ = nullptr;
  physical_membase_ = nullptr;
}

bool Memory::Initialize() {
  file_name_ = fmt::format("xenia_memory_{}", chrono::Clock::QueryHostTickCount());

  // Create main page file-backed mapping. This is all reserved but
  // uncommitted (so it shouldn't expand page file).
  // Round up to allocation granularity to accommodate platforms with large pages.
  const size_t mapping_size =
      rex::round_up(static_cast<size_t>(0x120000000ull) + system_allocation_granularity_,
                    static_cast<size_t>(system_allocation_granularity_));
  mapping_ = rex::memory::CreateFileMappingHandle(file_name_, mapping_size,
                                                  rex::memory::PageAccess::kReadWrite, false);
  if (mapping_ == rex::memory::kFileMappingHandleInvalid) {
    REXSYS_ERROR("Unable to reserve the 4gb guest address space.");
    assert_always();
    return false;
  }

#if REX_PLATFORM_MAC
  // On macOS, reserve a contiguous host range first, then carve guest views
  // into it with MAP_SHARED|MAP_FIXED so all views share the same backing fd.
  if (MapViewsMac()) {
    REXSYS_ERROR("Unable to find a continuous block in the 64bit address space.");
    assert_always();
    return false;
  }
  mapping_base_ = views_.all_views[0];
#elif REX_PLATFORM_SWITCH
  // Horizon does not allow fixed addresses: the kernel chooses the guest window,
  // which CreateFileMappingHandle reserved. We ask where it ended up and carve the
  // views inside it, like macOS but without mmap. Trying 1<<32, 1<<33... as Linux
  // does would always fail.
  mapping_base_ = static_cast<uint8_t*>(rex::memory::GetFileMappingBase(mapping_));
  if (!mapping_base_ || MapViews(mapping_base_)) {
    REXSYS_ERROR("Unable to map the guest views into the Switch guest window.");
    assert_always();
    return false;
  }
#else
  // Attempt to create our views. This may fail at the first address
  // we pick, so try a few times.
  mapping_base_ = 0;
  for (size_t n = 32; n < 64; n++) {
    auto mapping_base = reinterpret_cast<uint8_t*>(1ull << n);
    if (!MapViews(mapping_base)) {
      mapping_base_ = mapping_base;
      break;
    }
  }
  if (!mapping_base_) {
    REXSYS_ERROR("Unable to find a continuous block in the 64bit address space.");
    assert_always();
    return false;
  }
#endif
  virtual_membase_ = mapping_base_;
  physical_membase_ = mapping_base_ + 0x100000000ull;

  REXSYS_INFO("Guest memory arena mapped: virtual base 0x{:016X}, physical base 0x{:016X}",
              reinterpret_cast<uintptr_t>(virtual_membase_),
              reinterpret_cast<uintptr_t>(physical_membase_));

  // Prepare virtual heaps.
  heaps_.v00000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestVirtual, 0x00000000,
                              0x40000000, 4096);
  heaps_.v40000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestVirtual, 0x40000000,
                              0x40000000 - 0x01000000, 64 * 1024);
  heaps_.v80000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestXex, 0x80000000,
                              0x10000000, 64 * 1024);
  heaps_.v90000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestXex, 0x90000000,
                              0x10000000, 4096);

  // Prepare physical heaps.
  heaps_.physical.Initialize(this, physical_membase_, memory::HeapType::kGuestPhysical, 0x00000000,
                             0x20000000, 4096);
  heaps_.vA0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xA0000000,
                              0x20000000, 64 * 1024, &heaps_.physical);
  heaps_.vC0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xC0000000,
                              0x20000000, 16 * 1024 * 1024, &heaps_.physical);
  heaps_.vE0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xE0000000,
                              0x1FD00000, 4096, &heaps_.physical);

  // Protect the first and last 64kb of memory.
  heaps_.v00000000.AllocFixed(0x00000000, 0x10000, 0x10000,
                              memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                              !REXCVAR_GET(protect_zero)
                                  ? memory::kMemoryProtectRead | memory::kMemoryProtectWrite
                                  : memory::kMemoryProtectNoAccess);
  heaps_.physical.AllocFixed(0x1FFF0000, 0x10000, 0x10000, memory::kMemoryAllocationReserve,
                             memory::kMemoryProtectNoAccess);

  // GPU writeback.
  // 0xC... is physical, 0x7F... is virtual. We may need to overlay these.
  heaps_.vC0000000.AllocFixed(0xC0000000, 0x01000000, 32,
                              memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                              memory::kMemoryProtectRead | memory::kMemoryProtectWrite);

  // Pre-commit the physical memory range so the GPU can access it
  // without page faults. Reference: xenia-canary 5f5be0668.
#if REX_PLATFORM_SWITCH
  // Not on Switch. It is 512 MB, and on Horizon each chunk is also mapped into the
  // five views the 360 has over physical memory (0x7F..., 0xA0..., 0xC0...,
  // 0xE0... and raw physical), so the process ends up accounting for more than
  // 2 GB and the kernel refuses the next allocation. Measured: the game stayed
  // black after the EA logo with "no se pudo confirmar 0x6A0000 bytes; respaldo
  // 578 MB, mapeado 2139 MB contando espejos" (2001-0103).
  //
  // Instead, physical memory is committed as it is touched: the first access to an
  // uncommitted page lands in the exception handler, which commits 1 MB and
  // retries the instruction (exception_handler_switch.cpp, step 0).
#else
  rex::memory::AllocFixed(heaps_.physical.TranslateRelative(0), heaps_.physical.heap_size(),
                          rex::memory::AllocationType::kCommit,
                          rex::memory::PageAccess::kReadWrite);
#endif

  // Install MMIO handler for physical address translation and MMIO ranges
  mmio_handler_ = runtime::MMIOHandler::Install(
      virtual_membase_, physical_membase_, physical_membase_ + 0x1FFFFFFF, HostToGuestVirtualThunk,
      this, AccessViolationCallbackThunk, this);
  if (!mmio_handler_) {
    REXSYS_ERROR("Unable to install MMIO handlers");
    assert_always();
    return false;
  }
  REXSYS_DEBUG("Installed MMIO handler for physical address translation");

  // ?
  uint32_t unk_phys_alloc;
  heaps_.vA0000000.Alloc(0x340000, 64 * 1024, memory::kMemoryAllocationReserve,
                         memory::kMemoryProtectNoAccess, true, &unk_phys_alloc);

  // Allocate region at start of XEX range. Title 544307D5 explicitly
  // accesses 0x8000001C and expects a specific constant value.
  // Reference: xenia-canary 78f97f8ff.
  uint32_t unknown_xex_range;
  heaps_.v80000000.Alloc(0x40000, 4 * 1024, memory::kMemoryAllocationCommit,
                         memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false,
                         &unknown_xex_range);

  uint32_t value_to_write = rex::byte_swap(uint32_t(0x2a6e3f38));
  std::memcpy(TranslateVirtual(0x80000000 + 0x1C), &value_to_write, sizeof(uint32_t));

  return true;
}

static const struct {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
} map_info[] = {
    // (1024mb) - virtual 4k pages
    {
        0x00000000,
        0x3FFFFFFF,
        0x0000000000000000ull,
    },
    // (1024mb) - virtual 64k pages (cont)
    {
        0x40000000,
        0x7EFFFFFF,
        0x0000000040000000ull,
    },
    //   (16mb) - GPU writeback + 15mb of XPS?
    {
        0x7F000000,
        0x7FFFFFFF,
        0x0000000100000000ull,
    },
    //  (256mb) - xex 64k pages
    {
        0x80000000,
        0x8FFFFFFF,
        0x0000000080000000ull,
    },
    //  (256mb) - xex 4k pages
    {
        0x90000000,
        0x9FFFFFFF,
        0x0000000080000000ull,
    },
    //  (512mb) - physical 64k pages
    {
        0xA0000000,
        0xBFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 16mb pages
    {
        0xC0000000,
        0xDFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 4k pages
    {
        0xE0000000,
        0xFFFFFFFF,
        0x0000000100001000ull,
    },
    //          - physical raw
    {
        0x100000000,
        0x11FFFFFFF,
        0x0000000100000000ull,
    },
};
#if REX_PLATFORM_MAC
int Memory::MapViewsMac() {
  assert_true(rex::countof(map_info) == rex::countof(views_.all_views));

  // macOS does not guarantee that a non-MAP_FIXED mmap will honor a requested
  // address. Reserve a contiguous range first, then MAP_FIXED each view within
  // it so the guest layout is stable across runs.
  const size_t total_size =
      static_cast<size_t>(map_info[rex::countof(map_info) - 1].virtual_address_end -
                          map_info[0].virtual_address_start + 1);

  void* reserved_base = mmap(nullptr, total_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserved_base == MAP_FAILED) {
    REXSYS_ERROR("MapViewsMac: reserve failed: {}", std::strerror(errno));
    return 1;
  }

  uint8_t* mapping_base = reinterpret_cast<uint8_t*>(reserved_base);
  uint64_t granularity_mask = ~uint64_t(system_allocation_granularity_ - 1);

  for (size_t n = 0; n < rex::countof(map_info); n++) {
    size_t view_size = static_cast<size_t>(map_info[n].virtual_address_end -
                                           map_info[n].virtual_address_start + 1);
    size_t file_offset = static_cast<size_t>(map_info[n].target_address & granularity_mask);
    void* target_address = mapping_base + map_info[n].virtual_address_start;
    void* result = mmap(target_address, view_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                        mapping_, file_offset);
    if (result == MAP_FAILED || result != target_address) {
      int err = errno;
      REXSYS_ERROR(
          "MapViewsMac: map failed view {} addr 0x{:016X} size 0x{:X} offset 0x{:X} err {} ({})", n,
          reinterpret_cast<uintptr_t>(target_address), view_size, file_offset, err,
          std::strerror(err));
      munmap(reserved_base, total_size);
      for (auto& view : views_.all_views) {
        view = nullptr;
      }
      return 1;
    }
    views_.all_views[n] = reinterpret_cast<uint8_t*>(result);
  }

  return 0;
}
#endif  // REX_PLATFORM_MAC

int Memory::MapViews(uint8_t* mapping_base) {
  assert_true(rex::countof(map_info) == rex::countof(views_.all_views));
  // 0xE0000000 4 KB offset is emulated via host_address_offset and on the CPU
  // side if system allocation granularity is bigger than 4 KB.
  uint64_t granularity_mask = ~uint64_t(system_allocation_granularity_ - 1);
  for (size_t n = 0; n < rex::countof(map_info); n++) {
    views_.all_views[n] = reinterpret_cast<uint8_t*>(rex::memory::MapFileView(
        mapping_, mapping_base + map_info[n].virtual_address_start,
        map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1,
        rex::memory::PageAccess::kReadWrite, map_info[n].target_address & granularity_mask));
    if (!views_.all_views[n]) {
      // Failed, so bail and try again.
      UnmapViews();
      return 1;
    }
  }
  return 0;
}

void Memory::UnmapViews() {
  for (size_t n = 0; n < rex::countof(views_.all_views); n++) {
    if (views_.all_views[n]) {
      size_t length = map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1;
      rex::memory::UnmapFileView(mapping_, views_.all_views[n], length);
    }
  }
}

void Memory::Reset() {
  heaps_.v00000000.Reset();
  heaps_.v40000000.Reset();
  heaps_.v80000000.Reset();
  heaps_.v90000000.Reset();
  heaps_.physical.Reset();
}

const BaseHeap* Memory::LookupHeap(uint32_t address) const {
  if (address < 0x40000000) {
    return &heaps_.v00000000;
  } else if (address < 0x7F000000) {
    return &heaps_.v40000000;
  } else if (address < 0x80000000) {
    return nullptr;
  } else if (address < 0x90000000) {
    return &heaps_.v80000000;
  } else if (address < 0xA0000000) {
    return &heaps_.v90000000;
  } else if (address < 0xC0000000) {
    return &heaps_.vA0000000;
  } else if (address < 0xE0000000) {
    return &heaps_.vC0000000;
  } else if (address < 0xFFD00000) {
    return &heaps_.vE0000000;
  } else {
    return nullptr;
  }
}

BaseHeap* Memory::LookupHeapByType(bool physical, uint32_t page_size) {
  if (physical) {
    if (page_size <= 4096) {
      return &heaps_.vE0000000;
    } else if (page_size <= 64 * 1024) {
      return &heaps_.vA0000000;
    } else {
      return &heaps_.vC0000000;
    }
  } else {
    if (page_size <= 4096) {
      return &heaps_.v00000000;
    } else {
      return &heaps_.v40000000;
    }
  }
}

VirtualHeap* Memory::GetPhysicalHeap() {
  return &heaps_.physical;
}

uint32_t Memory::HostToGuestVirtual(const void* host_address) const {
  size_t virtual_address =
      reinterpret_cast<size_t>(host_address) - reinterpret_cast<size_t>(virtual_membase_);
  uint32_t vE0000000_host_offset = heaps_.vE0000000.host_address_offset();
  size_t vE0000000_host_base = size_t(heaps_.vE0000000.heap_base()) + vE0000000_host_offset;
  if (virtual_address >= vE0000000_host_base &&
      virtual_address <= (vE0000000_host_base + (heaps_.vE0000000.heap_size() - 1))) {
    virtual_address -= vE0000000_host_offset;
  }
  return uint32_t(virtual_address);
}

uint32_t Memory::HostToGuestVirtualThunk(const void* context, const void* host_address) {
  return reinterpret_cast<const memory::Memory*>(context)->HostToGuestVirtual(host_address);
}

uint32_t Memory::GetPhysicalAddress(uint32_t address) const {
  const BaseHeap* heap = LookupHeap(address);
  if (!heap || heap->heap_type() != memory::HeapType::kGuestPhysical) {
    return UINT32_MAX;
  }
  return static_cast<const PhysicalHeap*>(heap)->GetPhysicalAddress(address);
}

void Memory::Zero(uint32_t address, uint32_t size) {
  std::memset(TranslateVirtual(address), 0, size);
}

void Memory::Fill(uint32_t address, uint32_t size, uint8_t value) {
  std::memset(TranslateVirtual(address), value, size);
}

void Memory::Copy(uint32_t dest, uint32_t src, uint32_t size) {
  uint8_t* pdest = TranslateVirtual(dest);
  const uint8_t* psrc = TranslateVirtual(src);
  std::memcpy(pdest, psrc, size);
}

uint32_t Memory::SearchAligned(uint32_t start, uint32_t end, const uint32_t* values,
                               size_t value_count) {
  assert_true(start <= end);
  auto p = TranslateVirtual<const uint32_t*>(start);
  auto pe = TranslateVirtual<const uint32_t*>(end);
  while (p != pe) {
    if (*p == values[0]) {
      const uint32_t* pc = p + 1;
      size_t matched = 1;
      for (size_t n = 1; n < value_count; n++, pc++) {
        if (*pc != values[n]) {
          break;
        }
        matched++;
      }
      if (matched == value_count) {
        return HostToGuestVirtual(p);
      }
    }
    p++;
  }
  return 0;
}

bool Memory::AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask, uint32_t size,
                                   void* context, runtime::MMIOReadCallback read_callback,
                                   runtime::MMIOWriteCallback write_callback) {
  // AllocFixed rejects requests that don't cover whole host pages, so that a
  // guest heap can never widen an mprotect over a neighbouring guest page that
  // shares the host page. An MMIO window has no such neighbours - it owns its
  // entire range - so rounding out to host page bounds is safe here, and it is
  // required because callers describe these windows with mask-style sizes such
  // as 0xFFFF, which is one byte short of 64 KB and therefore never a multiple
  // of the 16 KB host page on Apple Silicon.
  const size_t host_page_size = rex::memory::page_size();
  uint8_t* const range_start = TranslateVirtual(virtual_address);
  uint8_t* const aligned_start = reinterpret_cast<uint8_t*>(
      reinterpret_cast<uintptr_t>(range_start) & ~(uintptr_t(host_page_size) - 1));
  const size_t aligned_length =
      rex::round_up(size_t(range_start - aligned_start) + size, host_page_size);
  if (!rex::memory::AllocFixed(aligned_start, aligned_length, rex::memory::AllocationType::kCommit,
                               rex::memory::PageAccess::kNoAccess)) {
    REXSYS_ERROR("Unable to map range; commit/protect failed");
    return false;
  }
  // The MMIO registration keeps the caller's original size - only the host
  // reservation is rounded.
  return mmio_handler_->RegisterRange(virtual_address, mask, size, context, read_callback,
                                      write_callback);
}

runtime::MMIORange* Memory::LookupVirtualMappedRange(uint32_t virtual_address) {
  return mmio_handler_->LookupRange(virtual_address);
}

bool Memory::AccessViolationCallback(std::unique_lock<std::recursive_mutex> global_lock_locked_once,
                                     void* host_address, bool is_write) {
  // Access via physical_membase_ is special, when need to bypass everything
  // (for instance, for a data provider to actually write the data) so only
  // triggering callbacks on virtual memory regions.
  if (reinterpret_cast<size_t>(host_address) < reinterpret_cast<size_t>(virtual_membase_) ||
      reinterpret_cast<size_t>(host_address) >= reinterpret_cast<size_t>(physical_membase_)) {
    return false;
  }
  uint32_t virtual_address = HostToGuestVirtual(host_address);
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap || heap->heap_type() != memory::HeapType::kGuestPhysical) {
    REXSYS_ERROR(
        "Unhandled guest access violation: {} of guest 0x{:08X} (host 0x{:016X}) on thread 0x{:X}",
        is_write ? "write" : "read", virtual_address, reinterpret_cast<uintptr_t>(host_address),
        rex::thread::current_thread_id());
    return false;
  }

  // Access violation callbacks from the guest are triggered when the global
  // critical region mutex is locked once.
  //
  // Will be rounded to physical page boundaries internally, so just pass 1 as
  // the length - guranteed not to cross page boundaries also.
  auto physical_heap = static_cast<PhysicalHeap*>(heap);
  if (physical_heap->TriggerCallbacks(std::move(global_lock_locked_once), virtual_address, 1,
                                      is_write, false)) {
    return true;
  }

  {
    auto lock = global_critical_region_.Acquire();
    const uint32_t host_page = uint32_t(
        (virtual_address - physical_heap->heap_base() + physical_heap->host_address_offset()) /
        system_page_size_);
    if (physical_heap->IsHostPageDataWatched(host_page)) {
      return false;
    }
  }

  // Recovery path for stale host protection state in physical memory:
  // if guest metadata says the page is writable but host protection is still
  // read-only / no-access, restore write access and resume execution.
  if (is_write) {
    constexpr uint32_t kWriteProtectMask =
        memory::kMemoryProtectWrite | memory::kMemoryProtectWriteCombine;
    uint32_t guest_protect = 0;
    bool allow_write = false;
    if (heap->QueryProtect(virtual_address, &guest_protect) &&
        (guest_protect & kWriteProtectMask)) {
      allow_write = true;
    } else {
      // If write-watch left current protect stale, trust committed allocation
      // metadata first for this alias.
      HeapAllocationInfo guest_info{};
      if (heap->QueryRegionInfo(virtual_address, &guest_info) &&
          (guest_info.state & memory::kMemoryAllocationCommit) &&
          (guest_info.allocation_protect & kWriteProtectMask)) {
        guest_protect = guest_info.protect;
        allow_write = true;
      } else {
        // Alias-aware fallback: consult canonical 0x00000000 physical heap
        // metadata when this alias has stale tracking.
        uint32_t physical_address = GetPhysicalAddress(virtual_address);
        if (physical_address != UINT32_MAX) {
          uint32_t physical_protect = 0;
          if (heaps_.physical.QueryProtect(physical_address, &physical_protect) &&
              (physical_protect & kWriteProtectMask)) {
            guest_protect = physical_protect;
            allow_write = true;
          } else {
            HeapAllocationInfo physical_info{};
            if (heaps_.physical.QueryRegionInfo(physical_address, &physical_info) &&
                (physical_info.state & memory::kMemoryAllocationCommit) &&
                (physical_info.allocation_protect & kWriteProtectMask)) {
              guest_protect = physical_info.protect;
              allow_write = true;
            }
          }
        }
      }
    }
    if (allow_write) {
      size_t host_page_size = rex::memory::page_size();
      uintptr_t page_base =
          reinterpret_cast<uintptr_t>(host_address) & ~(uintptr_t(host_page_size - 1));
      if (rex::memory::Protect(reinterpret_cast<void*>(page_base), host_page_size,
                               rex::memory::PageAccess::kReadWrite, nullptr)) {
        REXSYS_WARN(
            "Recovered stale physical page protection for guest {:08X} (host {:016X}, "
            "guest_protect {:08X})",
            virtual_address, static_cast<uint64_t>(page_base), guest_protect);
        return true;
      }
    }

    uint32_t current_guest_protect = 0;
    heap->QueryProtect(virtual_address, &current_guest_protect);
    uint32_t physical_address = GetPhysicalAddress(virtual_address);
    uint32_t physical_protect = 0;
    if (physical_address != UINT32_MAX) {
      heaps_.physical.QueryProtect(physical_address, &physical_protect);
    }
    REXSYS_ERROR(
        "Unhandled guest physical write fault: guest={:08X} host={:016X} phys={:08X} "
        "guest_protect={:08X} physical_protect={:08X}",
        virtual_address, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(host_address)),
        physical_address, current_guest_protect, physical_protect);
  }

  return false;
}

bool Memory::AccessViolationCallbackThunk(
    std::unique_lock<std::recursive_mutex> global_lock_locked_once, void* context,
    void* host_address, bool is_write) {
  return reinterpret_cast<memory::Memory*>(context)->AccessViolationCallback(
      std::move(global_lock_locked_once), host_address, is_write);
}

bool Memory::TriggerPhysicalMemoryCallbacks(
    std::unique_lock<std::recursive_mutex> global_lock_locked_once, uint32_t virtual_address,
    uint32_t length, bool is_write, bool unwatch_exact_range, bool unprotect, bool discard) {
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap) {
    return false;
  }
  if (heap->heap_type() == memory::HeapType::kGuestPhysical) {
    auto physical_heap = static_cast<PhysicalHeap*>(heap);
    return physical_heap->TriggerCallbacks(std::move(global_lock_locked_once), virtual_address,
                                           length, is_write, unwatch_exact_range, unprotect, discard);
  }
  return false;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback, callback_context);
  auto lock = global_critical_region_.Acquire();
  physical_memory_invalidation_callbacks_.push_back(entry);
  return entry;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
  auto entry =
      reinterpret_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(callback_handle);
  {
    auto lock = global_critical_region_.Acquire();
    auto it = std::find(physical_memory_invalidation_callbacks_.begin(),
                        physical_memory_invalidation_callbacks_.end(), entry);
    assert_true(it != physical_memory_invalidation_callbacks_.end());
    if (it != physical_memory_invalidation_callbacks_.end()) {
      physical_memory_invalidation_callbacks_.erase(it);
    }
  }
  delete entry;
}

void* Memory::RegisterPhysicalMemoryDataProvider(PhysicalMemoryDataProvider callback,
                                                  void* context) {
  auto entry = std::make_shared<PhysicalDataProvider>();
  entry->callback = callback;
  entry->context = context;
  auto lock = global_critical_region_.Acquire();
  physical_memory_data_providers_.push_back(entry);
  return entry.get();
}

void Memory::UnregisterPhysicalMemoryDataProvider(void* handle) {
  auto lock = global_critical_region_.Acquire();
  auto it = std::find_if(physical_memory_data_providers_.begin(),
                         physical_memory_data_providers_.end(),
                         [handle](const auto& entry) { return entry.get() == handle; });
  assert_true(it != physical_memory_data_providers_.end());
  if (it == physical_memory_data_providers_.end()) {
    return;
  }
  auto entry = *it;
  entry->registered = false;
  physical_memory_data_providers_.erase(it);
  // GPU shutdown closes/drains its callback queue before destroying providers.
  // Waiting releases the global region so already admitted calls can finish.
  entry->idle.wait(lock, [&]() { return entry->active_calls == 0; });
  if (physical_memory_data_providers_.empty()) {
    DisablePhysicalMemoryDataProviders();
  }
}

void Memory::DisablePhysicalMemoryDataProviders() {
  heaps_.vA0000000.DisableDataProviders();
  heaps_.vC0000000.DisableDataProviders();
  heaps_.vE0000000.DisableDataProviders();
}

bool Memory::ProvidePhysicalMemoryData(std::unique_lock<std::recursive_mutex>& global_lock,
                                       uint32_t address, uint32_t length) {
  const auto providers = physical_memory_data_providers_;
  bool provided = false;
  for (const auto& entry : providers) {
    if (!entry->registered) {
      continue;
    }
    provided = true;
    ++entry->active_calls;
    bool result = entry->callback(entry->context, global_lock, address, length);
    assert_true(global_lock.owns_lock());
    if (!--entry->active_calls) {
      entry->idle.notify_all();
    }
    if (!result) {
      return false;
    }
  }
  return provided;
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications,
                                                 bool enable_data_providers) {
  heaps_.vA0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vC0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vE0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
}

uint32_t Memory::SystemHeapAlloc(uint32_t size, uint32_t alignment, uint32_t system_heap_flags) {
  // TODO(benvanik): lightweight pool.
  bool is_physical = !!(system_heap_flags & memory::kSystemHeapPhysical);
  auto heap = LookupHeapByType(is_physical, 4096);
  uint32_t address;
  if (!heap->AllocSystemHeap(
          size, alignment, memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
          memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  Zero(address, size);
  return address;
}

void Memory::SystemHeapFree(uint32_t address, uint32_t* out_region_size) {
  if (!address) {
    return;
  }
  // TODO(benvanik): lightweight pool.
  auto heap = LookupHeap(address);
  heap->Release(address, out_region_size);
}

void Memory::GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                      uint32_t& unreserved_pages, uint32_t& reserved_pages,
                                      uint32_t& used_pages, uint32_t& reserved_bytes) {
  // Heap array is fixed after init; page counts are approximate statistics.
  for (size_t i = 0; i < heaps_count; i++) {
    const BaseHeap* heap = provided_heaps[i];
    uint32_t heap_unreserved = heap->unreserved_page_count();
    uint32_t heap_reserved = heap->reserved_page_count();

    unreserved_pages += heap_unreserved;
    reserved_pages += heap_reserved;
    used_pages += ((heap->total_page_count() - heap_unreserved) * heap->page_size()) / 4096;
    reserved_bytes += heap_reserved * heap->page_size();
  }
}

void Memory::DumpMap() {
  REXSYS_ERROR("==================================================================");
  REXSYS_ERROR("Memory Dump");
  REXSYS_ERROR("==================================================================");
  REXSYS_ERROR("               System Page Size: {0} ({0:08X})", system_page_size_);
  REXSYS_ERROR("  System Allocation Granularity: {0} ({0:08X})", system_allocation_granularity_);
  REXSYS_ERROR("                Virtual Membase: {}", fmt::ptr(virtual_membase_));
  REXSYS_ERROR("               Physical Membase: {}", fmt::ptr(physical_membase_));
  REXSYS_ERROR("");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Virtual Heaps");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("");
  heaps_.v00000000.DumpMap();
  heaps_.v40000000.DumpMap();
  heaps_.v80000000.DumpMap();
  heaps_.v90000000.DumpMap();
  REXSYS_ERROR("");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Physical Heaps");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("");
  heaps_.physical.DumpMap();
  heaps_.vA0000000.DumpMap();
  heaps_.vC0000000.DumpMap();
  heaps_.vE0000000.DumpMap();
  REXSYS_ERROR("");
}

bool Memory::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("Serializing memory...");
  heaps_.v00000000.Save(stream);
  heaps_.v40000000.Save(stream);
  heaps_.v80000000.Save(stream);
  heaps_.v90000000.Save(stream);
  heaps_.physical.Save(stream);

  return true;
}

bool Memory::Restore(stream::ByteStream* stream) {
  REXSYS_DEBUG("Restoring memory...");
  heaps_.v00000000.Restore(stream);
  heaps_.v40000000.Restore(stream);
  heaps_.v80000000.Restore(stream);
  heaps_.v90000000.Restore(stream);
  heaps_.physical.Restore(stream);

  return true;
}

//=============================================================================
// Recompiled Code Function Table
//=============================================================================

bool Memory::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                                     uint32_t image_size) {
  constexpr uint32_t kThunkReserveSize = runtime::FunctionDispatcher::kThunkReserveSize;
  uint32_t table_base = image_base + image_size;
  uint32_t table_size = (code_size + kThunkReserveSize) * 2;

  REXSYS_DEBUG(
      "Initializing function table at {:08X}, size {:08X} for code {:08X}-{:08X} "
      "(+{:08X} thunk reserve)",
      table_base, table_size, code_base, code_base + code_size, kThunkReserveSize);

  if (!heaps_.v80000000.AllocFixed(
          table_base, table_size, 0x10000,
          memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
          memory::kMemoryProtectRead | memory::kMemoryProtectWrite)) {
    REXSYS_ERROR("Failed to allocate function table at {:08X}", table_base);
    return false;
  }

  Zero(table_base, table_size);

  std::lock_guard lock(function_tables_mutex_);
  function_tables_.push_back({
      .table_base = table_base,
      .code_base = code_base,
      .code_size = code_size,
      .thunk_reserve = kThunkReserveSize,
  });

  return true;
}

bool Memory::DestroyFunctionTable(uint32_t code_base) {
  uint32_t table_base = 0;
  uint32_t table_size = 0;
  uint32_t logged_code_base = 0;
  uint32_t logged_code_size = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    auto it = std::find_if(
        function_tables_.begin(), function_tables_.end(),
        [code_base](const FunctionTableEntry& entry) { return entry.code_base == code_base; });
    if (it == function_tables_.end()) {
      return false;
    }
    table_base = it->table_base;
    table_size = (it->code_size + it->thunk_reserve) * 2;
    logged_code_base = it->code_base;
    logged_code_size = it->code_size;
    function_tables_.erase(it);
  }

  REXSYS_DEBUG("Destroying function table at {:08X}, size {:08X} for code {:08X}-{:08X}",
               table_base, table_size, logged_code_base, logged_code_base + logged_code_size);

  heaps_.v80000000.Release(table_base);
  return true;
}

bool Memory::SetFunction(uint32_t guest_address, PPCFunc* host_function) {
  uint32_t table_address = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    for (const auto& entry : function_tables_) {
      uint32_t range_end = entry.code_base + entry.code_size + entry.thunk_reserve;
      if (guest_address >= entry.code_base && guest_address < range_end) {
        uint64_t offset = (uint64_t(guest_address) - entry.code_base) * 2;
        table_address = entry.table_base + uint32_t(offset);
        break;
      }
    }
  }
  if (!table_address) {
    return false;
  }
  auto* slot = TranslateVirtual<PPCFunc**>(table_address);
  *slot = host_function;
  return true;
}

bool Memory::HasAnyFunctionTable() const {
  std::lock_guard lock(function_tables_mutex_);
  return !function_tables_.empty();
}

rex::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable =
      (protect & memory::kMemoryProtectWrite) || (protect & memory::kMemoryProtectWriteCombine);

  if ((protect & memory::kMemoryProtectRead) && !is_writable) {
    return rex::memory::PageAccess::kReadOnly;
  } else if ((protect & memory::kMemoryProtectRead) && is_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

// Switch: outside physical memory, what the guest requests as read-only is
// given on the host as read-write. On Horizon there are no read-only pages in
// the guest window: they are unmapped, and every read goes through the
// exception handler (2 us per access). The XEX constants live in such pages,
// and the game code reads them constantly. On the first boot of UFC3_UNDISPUTED_NX one of
// those emulated reads was also what exposed the resume bug (see
// resume_ctx.h). The game does not write to those pages (it would fault on the
// 360 and on the PC), so dropping the protection does not change its behavior.
// Physical memory stays as it is: there, read-only is the GPU's write watching,
// and it has to fault.
static rex::memory::PageAccess ToHostPageAccess(uint32_t protect, memory::HeapType heap_type) {
  const rex::memory::PageAccess access = ToPageAccess(protect);
#if REX_PLATFORM_SWITCH
  if (access == rex::memory::PageAccess::kReadOnly &&
      heap_type != memory::HeapType::kGuestPhysical) {
    return rex::memory::PageAccess::kReadWrite;
  }
#else
  (void)heap_type;
#endif
  return access;
}

uint32_t FromPageAccess(rex::memory::PageAccess protect) {
  switch (protect) {
    case memory::PageAccess::kNoAccess:
      return memory::kMemoryProtectNoAccess;
    case memory::PageAccess::kReadOnly:
      return memory::kMemoryProtectRead;
    case memory::PageAccess::kReadWrite:
      return memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
    case memory::PageAccess::kExecuteReadOnly:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return memory::kMemoryProtectRead;
    case memory::PageAccess::kExecuteReadWrite:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
  }

  return memory::kMemoryProtectNoAccess;
}

BaseHeap::BaseHeap() : membase_(nullptr), heap_base_(0), heap_size_(0), page_size_(0) {}

BaseHeap::~BaseHeap() = default;

std::unique_lock<std::recursive_mutex> BaseHeap::AcquireHostPageReconcileLock() const {
  if (page_size_ >= memory_->system_page_size_) {
    return {};
  }
  return rex::thread::global_critical_region::AcquireDirect();
}

bool BaseHeap::SyncHostPageAccess(uint32_t start_page_number, uint32_t end_page_number) {
  const uint32_t host_page_size = memory_->system_page_size_;
  if (page_size_ >= host_page_size &&
      (heap_type_ != memory::HeapType::kGuestPhysical || heap_base_ < 0xA0000000)) {
    return true;
  }

  // The global critical region guards the watch flags read below and must
  // already be held by the caller (AcquireHostPageReconcileLock), acquired
  // ahead of heap_mutex_ so this never inverts against PhysicalHeap.

  // Multiple guest pages may share one host page. Keep the guest page table as
  // the source of truth, then grant the host page the union of the access needed
  // by its committed guest pages. This can't enforce stricter access for one
  // guest page while a neighbor needs broader access, but it keeps neighboring
  // committed pages usable and allows guest protection metadata to stay exact.
  const auto get_host_page_access = [&](uint32_t host_page_number) {
    const uint64_t host_page_start = uint64_t(host_page_number) * host_page_size;
    const uint64_t host_page_end = host_page_start + host_page_size;
    const uint64_t heap_host_start = host_address_offset_;
    const uint64_t heap_host_end = heap_host_start + heap_size_;
    const uint64_t overlap_start = std::max(host_page_start, heap_host_start);
    const uint64_t overlap_end = std::min(host_page_end, heap_host_end);
    if (overlap_start >= overlap_end) {
      return rex::memory::PageAccess::kNoAccess;
    }

    const uint32_t guest_page_start = uint32_t(overlap_start - heap_host_start) >> page_size_shift_;
    const uint32_t guest_page_end = uint32_t(overlap_end - heap_host_start - 1) >> page_size_shift_;

    bool has_read = false;
    bool has_write = false;
    bool has_committed = false;
    for (uint32_t page_number = guest_page_start; page_number <= guest_page_end; ++page_number) {
      const auto& page_entry = page_table_[page_number];
      if (!(page_entry.state & memory::kMemoryAllocationCommit)) {
        continue;
      }
      has_committed = true;
      const rex::memory::PageAccess page_access =
          ToHostPageAccess(page_entry.current_protect, heap_type_);
      if (page_access == rex::memory::PageAccess::kReadWrite) {
        has_read = true;
        has_write = true;
        break;
      }
      if (page_access == rex::memory::PageAccess::kReadOnly) {
        has_read = true;
      }
    }

    rex::memory::PageAccess access;
    // Physical aliases are intentionally left accessible after release unless
    // protect_on_release is enabled because GPU users may still reference them.
    if (!has_committed && heap_type_ == memory::HeapType::kGuestPhysical &&
        !REXCVAR_GET(protect_on_release)) {
      access = rex::memory::PageAccess::kReadWrite;
    } else if (!has_read) {
      access = rex::memory::PageAccess::kNoAccess;
    } else {
      access = has_write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly;
    }

    // The guest page table is not the whole truth: a physical heap page may
    // also be write-watched for GPU invalidation. EnableAccessCallbacks records
    // that in notify_on_invalidation and skips any page whose bit is already
    // set, so handing write access back here would stop the watch firing
    // permanently. One host page covers several guest pages, so an unrelated
    // operation on a neighbour reaches this code constantly - clamp instead.
    // Leaving the page read-only can only cause a spurious invalidation, which
    // is safe; restoring write access loses invalidations, which is not.
    access = GetHostPageAccess(host_page_number, access);
    if (IsHostPageDataWatched(host_page_number)) {
      access = rex::memory::PageAccess::kNoAccess;
    } else if (access == rex::memory::PageAccess::kReadWrite &&
               IsHostPageWriteWatched(host_page_number)) {
      access = rex::memory::PageAccess::kReadOnly;
    }
    return access;
  };

  const uint64_t guest_byte_start = uint64_t(start_page_number) << page_size_shift_;
  const uint64_t guest_byte_end = ((uint64_t(end_page_number) + 1) << page_size_shift_) - 1;
  const uint32_t host_page_first =
      uint32_t((guest_byte_start + host_address_offset_) / host_page_size);
  const uint32_t host_page_last =
      uint32_t((guest_byte_end + host_address_offset_) / host_page_size);

  // Coalesce neighbouring host pages that resolve to the same access into one
  // Protect call. A bulk commit spans thousands of host pages and almost always
  // wants a single uniform access for all of them, so protecting page by page
  // would cost thousands of syscalls where one suffices.
  uint8_t* const heap_host_base = membase_ + heap_base_;
  uint32_t run_first_page = host_page_first;
  rex::memory::PageAccess run_access = get_host_page_access(host_page_first);
  const auto flush_run = [&](uint32_t run_last_page) {
    const size_t run_length = (size_t(run_last_page - run_first_page) + 1) * host_page_size;
    uint8_t* const run_pointer = heap_host_base + size_t(run_first_page) * host_page_size;
    if (!rex::memory::Protect(run_pointer, run_length, run_access, nullptr)) {
      REXSYS_ERROR("BaseHeap::SyncHostPageAccess failed for heap {:08X} host pages {}-{}",
                   heap_base_, run_first_page, run_last_page);
      return false;
    }
    return true;
  };

  for (uint32_t host_page = host_page_first + 1; host_page <= host_page_last; ++host_page) {
    const rex::memory::PageAccess access = get_host_page_access(host_page);
    if (access == run_access) {
      continue;
    }
    if (!flush_run(host_page - 1)) {
      return false;
    }
    run_first_page = host_page;
    run_access = access;
  }
  return flush_run(host_page_last);
}

void BaseHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                          uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                          uint32_t host_address_offset) {
  memory_ = memory;
  membase_ = membase;
  heap_type_ = heap_type;
  heap_base_ = heap_base;
  heap_size_ = heap_size;
  page_size_ = page_size;
  assert_true(rex::is_pow2(page_size_));
  page_size_shift_ = rex::log2_floor(page_size_);
  host_address_offset_ = host_address_offset;
  page_table_.resize(heap_size / page_size);
  unreserved_page_count_ = uint32_t(page_table_.size());
}

void BaseHeap::Dispose() {
  // Walk table and release all regions.
  for (uint32_t page_number = 0; page_number < page_table_.size(); ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (page_entry.state) {
      rex::memory::DeallocFixed(TranslateRelative(page_number << page_size_shift_), 0,
                                rex::memory::DeallocationType::kRelease);
      page_number += page_entry.region_page_count;
    }
  }
}

void BaseHeap::DumpMap() {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Heap: {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("            Heap Base: {:08X}", heap_base_);
  REXSYS_ERROR("            Heap Size: {0} ({0:08X})", heap_size_);
  REXSYS_ERROR("            Page Size: {0} ({0:08X})", page_size_);
  REXSYS_ERROR("           Page Count: {}", page_table_.size());
  REXSYS_ERROR("  Host Address Offset: {0} ({0:08X})", host_address_offset_);
  bool is_empty_span = false;
  uint32_t empty_span_start = 0;
  for (uint32_t i = 0; i < uint32_t(page_table_.size()); ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (!is_empty_span) {
        is_empty_span = true;
        empty_span_start = i;
      }
      continue;
    }
    if (is_empty_span) {
      REXSYS_ERROR("  {:08X}-{:08X} {:6d}p {:10d}b unreserved",
                   heap_base_ + (empty_span_start << page_size_shift_),
                   heap_base_ + (i << page_size_shift_), i - empty_span_start,
                   (i - empty_span_start) << page_size_shift_);
      is_empty_span = false;
    }
    const char* state_name = "   ";
    if (page.state & memory::kMemoryAllocationCommit) {
      state_name = "COM";
    } else if (page.state & memory::kMemoryAllocationReserve) {
      state_name = "RES";
    }
    char access_r = (page.current_protect & memory::kMemoryProtectRead) ? 'R' : ' ';
    char access_w = (page.current_protect & memory::kMemoryProtectWrite) ? 'W' : ' ';
    uint32_t region_pages = page.region_page_count;
    REXSYS_ERROR("  {:08X}-{:08X} {:6d}p {:10d}b {} {}{}", heap_base_ + (i << page_size_shift_),
                 heap_base_ + ((i + region_pages) << page_size_shift_), region_pages,
                 region_pages << page_size_shift_, state_name, access_r, access_w);
    i += region_pages - 1;
  }
  if (is_empty_span) {
    REXSYS_ERROR("  {:08X}-{:08X} - {} unreserved pages)",
                 heap_base_ + (empty_span_start << page_size_shift_), heap_base_ + (heap_size_ - 1),
                 page_table_.size() - empty_span_start);
  }
}

uint32_t BaseHeap::GetTotalPageCount() {
  return uint32_t(page_table_.size());
}

uint32_t BaseHeap::GetUnreservedPageCount() {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  uint32_t count = 0;
  bool is_empty_span = false;
  uint32_t empty_span_start = 0;
  uint32_t size = uint32_t(page_table_.size());
  for (uint32_t i = 0; i < size; ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (!is_empty_span) {
        is_empty_span = true;
        empty_span_start = i;
      }
      continue;
    }
    if (is_empty_span) {
      is_empty_span = false;
      count += i - empty_span_start;
    }
    i += page.region_page_count - 1;
  }
  if (is_empty_span) {
    count += size - empty_span_start;
  }
  return count;
}

uint32_t BaseHeap::GetLargestUnreservedSpanPages() {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  uint32_t largest = 0, span = 0;
  uint32_t size = uint32_t(page_table_.size());
  for (uint32_t i = 0; i < size; ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (++span > largest) {
        largest = span;
      }
      continue;
    }
    span = 0;
    i += page.region_page_count - 1;
  }
  return largest;
}

bool BaseHeap::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    stream->Write(page.qword);
    if (!page.state) {
      // Unallocated.
      continue;
    }

    // TODO(DrChat): write compressed with snappy.
    if (page.state & memory::kMemoryAllocationCommit) {
      void* addr = TranslateRelative(i << page_size_shift_);

      memory::PageAccess old_access;
      memory::Protect(addr, page_size_, memory::PageAccess::kReadWrite, &old_access);

      stream->Write(addr, page_size_);

      memory::Protect(addr, page_size_, old_access, nullptr);
    }
  }

  return true;
}

bool BaseHeap::Restore(stream::ByteStream* stream) {
  REXSYS_DEBUG("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  const uint32_t host_page_size = memory_->system_page_size_;
  const bool reconcile_host_pages = page_size_ < host_page_size;
  uint32_t writable_host_page = UINT32_MAX;

  // Restore doesn't take heap_mutex_, but SyncHostPageAccess below still needs
  // the global critical region held by its caller.
  auto global_lock = AcquireHostPageReconcileLock();

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    page.qword = stream->Read<uint64_t>();
    if (!page.state) {
      // Unallocated.
      continue;
    }

    memory::PageAccess page_access = memory::PageAccess::kNoAccess;
    if ((page.current_protect & memory::kMemoryProtectRead) &&
        (page.current_protect & memory::kMemoryProtectWrite)) {
      page_access = memory::PageAccess::kReadWrite;
    } else if (page.current_protect & memory::kMemoryProtectRead) {
      page_access = memory::PageAccess::kReadOnly;
    }

    if (!(page.state & memory::kMemoryAllocationCommit)) {
      continue;
    }

    void* addr = TranslateRelative(i << page_size_shift_);
    if (!reconcile_host_pages) {
      // Commit the memory if it isn't already. We do not need to reserve any
      // memory, as the mapping has already taken care of that.
      rex::memory::AllocFixed(addr, page_size_, memory::AllocationType::kCommit,
                              memory::PageAccess::kReadWrite);
      // Read into memory with R/W protection, then restore the saved
      // protection.
      // TODO(DrChat): read compressed with snappy.
      rex::memory::Protect(addr, page_size_, memory::PageAccess::kReadWrite, nullptr);
      stream->Read(addr, page_size_);
      rex::memory::Protect(addr, page_size_, page_access, nullptr);
      continue;
    }

    // Guest pages are smaller than host pages, so a per-guest-page commit is
    // rejected and a per-guest-page protect is misaligned - either way the
    // target would never become writable and the read below would fault or be
    // silently lost. Make the containing host page writable once instead;
    // guest pages are visited in ascending order, so one commit covers the
    // whole run that shares a host page. Real protections are reconciled from
    // the restored page table after the loop.
    const uint32_t host_page =
        uint32_t(((uint64_t(i) << page_size_shift_) + host_address_offset_) / host_page_size);
    if (host_page != writable_host_page) {
      uint8_t* host_page_pointer = membase_ + heap_base_ + size_t(host_page) * host_page_size;
      if (!rex::memory::AllocFixed(host_page_pointer, host_page_size,
                                   memory::AllocationType::kCommit,
                                   memory::PageAccess::kReadWrite)) {
        REXSYS_ERROR("BaseHeap::Restore failed to make host page {} writable", host_page);
        return false;
      }
      writable_host_page = host_page;
    }
    stream->Read(addr, page_size_);
  }

  if (reconcile_host_pages && !page_table_.empty()) {
    if (!SyncHostPageAccess(0, uint32_t(page_table_.size()) - 1)) {
      return false;
    }
  }

  return true;
}

void BaseHeap::Reset() {
  // TODO(DrChat): protect pages.
  std::memset(page_table_.data(), 0, sizeof(PageEntry) * page_table_.size());
  // TODO(Triang3l): Remove access callbacks from pages if this is a physical
  // memory heap.
}

bool BaseHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                     bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Reserve address space at the top for thread stacks.
  // 0x3XXXXXXX is for system threads, 0x7XXXXXXX is for title threads.
  uint32_t heap_virtual_guest_offset = 0;
  if (heap_type_ == memory::HeapType::kGuestVirtual) {
    heap_virtual_guest_offset = 0x10000000;
    if (page_size_ == 0x10000) {
      heap_virtual_guest_offset = 0x0F000000;
    }
  }

  uint32_t low_address = heap_base_;
  uint32_t high_address = heap_base_ + (heap_size_ - 1) - heap_virtual_guest_offset;
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                               uint32_t protect, bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  uint32_t low_address = heap_base_;
  if (heap_type_ == memory::HeapType::kGuestVirtual) {
    low_address = heap_base_ + heap_size_ - 0x10000000;
  }
  uint32_t high_address = heap_base_ + (heap_size_ - 1);
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect) {
  alignment = rex::round_up(alignment, page_size_);
  if (base_address % alignment != 0) {
    if (base_address % page_size_ != 0) {
      REXSYS_ERROR(
          "BaseHeap::AllocFixed invalid base alignment: base={:08X} page_size={:08X} "
          "requested_alignment={:08X} heap={:08X}-{:08X}",
          base_address, page_size_, alignment, heap_base_, heap_base_ + (heap_size_ - 1));
      return false;
    }
    // Fixed allocations can only be guaranteed page-aligned. If callers provide
    // a stricter alignment for an already-fixed address, fall back to page size.
    REXSYS_WARN(
        "BaseHeap::AllocFixed clamping alignment from {:08X} to page size {:08X} for "
        "base={:08X}",
        alignment, page_size_, base_address);
    alignment = page_size_;
  }
  size = rex::align(size, alignment);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  if (start_page_number >= page_table_.size() || end_page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::AllocFixed passed out of range address range");
    return false;
  }

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // - If we are reserving the entire range requested must not be already
  //   reserved.
  // - If we are committing it's ok for pages within the range to already be
  //   committed.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    uint32_t state = page_table_[page_number].state;
    if ((allocation_type == memory::kMemoryAllocationReserve) && state) {
      // Already reserved.
      REXSYS_ERROR(
          "BaseHeap::AllocFixed attempting to reserve an already reserved "
          "range");
      return false;
    }
    if ((allocation_type == memory::kMemoryAllocationCommit) &&
        !(state & memory::kMemoryAllocationReserve)) {
      // Attempting a commit-only op on an unreserved page.
      // This may be OK.
      REXSYS_WARN("BaseHeap::AllocFixed attempting commit on unreserved page");
      allocation_type |= memory::kMemoryAllocationReserve;
      break;
    }
  }

  // Allocate from host.
  if (allocation_type == memory::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & memory::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type,
                                ToHostPageAccess(protect, heap_type_));
    if (!result) {
      REXSYS_ERROR("BaseHeap::AllocFixed failed to alloc range from host");
      return false;
    }
  }

  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (allocation_type & memory::kMemoryAllocationReserve) {
      if (!page_entry.state) {
        unreserved_page_count_--;
      }
      // Region is based on reservation.
      page_entry.base_address = start_page_number;
      page_entry.region_page_count = page_count;
    }
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = memory::kMemoryAllocationReserve | allocation_type;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  if (allocation_type != memory::kMemoryAllocationReserve && REXCVAR_GET(scribble_heap) &&
      (protect & memory::kMemoryProtectWrite)) {
    std::memset(TranslateRelative(start_page_number << page_size_shift_), 0xCD,
                page_count << page_size_shift_);
  }

  return true;
}

bool BaseHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address) {
  *out_address = 0;

  alignment = rex::round_up(alignment, page_size_);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  low_address = std::max(heap_base_, rex::align(low_address, alignment));
  high_address = std::min(heap_base_ + (heap_size_ - 1), rex::align(high_address, alignment));
  if (high_address < low_address) {
    REXSYS_ERROR("BaseHeap::Alloc invalid requested range");
    return false;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  low_page_number = std::min(uint32_t(page_table_.size()) - 1, low_page_number);
  high_page_number = std::min(uint32_t(page_table_.size()) - 1, high_page_number);
  if (high_page_number < low_page_number) {
    REXSYS_ERROR("BaseHeap::Alloc invalid requested page range");
    return false;
  }

  uint32_t available_page_count = high_page_number - low_page_number + 1;
  if (!page_count || page_count > available_page_count) {
    REXSYS_ERROR("BaseHeap::Alloc page count too big for requested range");
    return false;
  }

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Find a free page range.
  // The base page must match the requested alignment, so we first scan for
  // a free aligned page and only then check for continuous free pages.
  // TODO(benvanik): optimized searching (free list buckets, bitmap, etc).
  uint32_t start_page_number = UINT_MAX;
  uint32_t end_page_number = UINT_MAX;
  uint32_t page_scan_stride = alignment >> page_size_shift_;
  uint32_t max_base_page_number = high_page_number + 1 - page_count;
  if (top_down) {
    max_base_page_number -= max_base_page_number % page_scan_stride;
    for (int64_t base_page_number = max_base_page_number; base_page_number >= low_page_number;
         base_page_number -= page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = uint32_t(base_page_number);
      end_page_number = uint32_t(base_page_number) + page_count - 1;
      assert_true(end_page_number < page_table_.size());
      bool any_taken = false;
      for (uint32_t page_number = uint32_t(base_page_number);
           !any_taken && page_number <= end_page_number; ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least before this page.
          any_taken = true;
          if (page_count > page_number) {
            // Not enough space left to fit entire page range. Breaks outer
            // loop.
            base_page_number = -1;
          } else {
            base_page_number = page_number - page_count;
            base_page_number -= base_page_number % page_scan_stride;
            base_page_number += page_scan_stride;  // cancel out loop logic
          }
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  } else {
    for (uint32_t base_page_number = low_page_number; base_page_number <= max_base_page_number;
         base_page_number += page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = base_page_number;
      end_page_number = base_page_number + page_count - 1;
      bool any_taken = false;
      for (uint32_t page_number = base_page_number; !any_taken && page_number <= end_page_number;
           ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least after this page.
          any_taken = true;
          base_page_number = rex::round_up(page_number + 1, page_scan_stride);
          base_page_number -= page_scan_stride;  // cancel out loop logic
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  }
  if (start_page_number == UINT_MAX || end_page_number == UINT_MAX) {
    // Out of memory.
    REXSYS_ERROR("BaseHeap::Alloc failed to find contiguous range");
    assert_always("Heap exhausted!");
    return false;
  }

  // Allocate from host.
  if (allocation_type == memory::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & memory::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type,
                                ToHostPageAccess(protect, heap_type_));
    if (!result) {
      REXSYS_ERROR("BaseHeap::Alloc failed to alloc range from host");
      return false;
    }
  }

  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (!page_entry.state) {
      unreserved_page_count_--;
    }
    page_entry.base_address = start_page_number;
    page_entry.region_page_count = page_count;
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = memory::kMemoryAllocationReserve | allocation_type;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  if (allocation_type != memory::kMemoryAllocationReserve && REXCVAR_GET(scribble_heap) &&
      (protect & memory::kMemoryProtectWrite)) {
    std::memset(TranslateRelative(start_page_number << page_size_shift_), 0xCD,
                page_count << page_size_shift_);
  }

  *out_address = heap_base_ + (start_page_number << page_size_shift_);
  return true;
}

bool BaseHeap::Decommit(uint32_t address, uint32_t size) {
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  start_page_number = std::min(uint32_t(page_table_.size()) - 1, start_page_number);
  end_page_number = std::min(uint32_t(page_table_.size()) - 1, end_page_number);

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Release from host.
  // TODO(benvanik): find a way to actually decommit memory;
  //     mapped memory cannot be decommitted.
  /*BOOL result =
      VirtualFree(TranslateRelative(start_page_number << page_size_shift_),
                  page_count << page_size_shift_, MEM_DECOMMIT);
  if (!result) {
    PLOGW("BaseHeap::Decommit failed due to host VirtualFree failure");
    return false;
  }*/

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.state &= ~memory::kMemoryAllocationCommit;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Given address must be a region base address.
  uint32_t base_page_number = (base_address - heap_base_) >> page_size_shift_;
  auto base_page_entry = page_table_[base_page_number];
  if (base_page_entry.base_address != base_page_number) {
    REXSYS_ERROR("BaseHeap::Release failed because address is not a region start");
    return false;
  }

  if (heap_base_ == 0x00000000 && base_page_number == 0) {
    REXSYS_ERROR("BaseHeap::Release: Attempt to free 0!");
    return false;
  }

  if (out_region_size) {
    *out_region_size = (base_page_entry.region_page_count << page_size_shift_);
  }

  // Release from host not needed as mapping reserves the range for us.
  // TODO(benvanik): protect with NOACCESS?
  /*BOOL result = VirtualFree(
      TranslateRelative(base_page_number << page_size_shift_), 0, MEM_RELEASE);
  if (!result) {
    PLOGE("BaseHeap::Release failed due to host VirtualFree failure");
    return false;
  }*/
  // Instead, we just protect it, if we can.
  if (page_size_ == rex::memory::page_size() ||
      ((base_page_entry.region_page_count << page_size_shift_) % rex::memory::page_size() == 0 &&
       ((base_page_number << page_size_shift_) % rex::memory::page_size() == 0))) {
    // TODO(benvanik): figure out why games are using memory after releasing
    // it. It's possible this is some virtual/physical stuff where the GPU
    // still can access it.
    if (REXCVAR_GET(protect_on_release)) {
      if (!rex::memory::Protect(TranslateRelative(base_page_number << page_size_shift_),
                                base_page_entry.region_page_count << page_size_shift_,
                                rex::memory::PageAccess::kNoAccess, nullptr)) {
        REXSYS_WARN("BaseHeap::Release failed due to host VirtualProtect failure");
      }
    }
  }

  // Perform table change.
  uint32_t end_page_number = base_page_number + base_page_entry.region_page_count - 1;
  for (uint32_t page_number = base_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.qword = 0;
    unreserved_page_count_++;
  }

  if (!SyncHostPageAccess(base_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::Protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect) {
  if (!size) {
    REXSYS_ERROR("BaseHeap::Protect failed due to zero size");
    return false;
  }

  // From the VirtualProtect MSDN page:
  //
  // "The region of affected pages includes all pages containing one or more
  //  bytes in the range from the lpAddress parameter to (lpAddress+dwSize).
  //  This means that a 2-byte range straddling a page boundary causes the
  //  protection attributes of both pages to be changed."
  //
  // "The access protection value can be set only on committed pages. If the
  //  state of any page in the specified region is not committed, the function
  //  fails and returns without modifying the access protection of any pages in
  //  the specified region."

  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    REXSYS_ERROR("BaseHeap::Protect failed due to out-of-bounds base address {:08X}", address);
    return false;
  }
  uint32_t end_page_number =
      uint32_t((uint64_t(address) + size - 1 - heap_base_) >> page_size_shift_);
  if (end_page_number >= page_table_.size()) {
    REXSYS_ERROR(
        "BaseHeap::Protect failed due to out-of-bounds range ({:08X} bytes "
        "from {:08x})",
        size, address);
    return false;
  }

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Ensure all pages are in the same reserved region and all are committed.
  uint32_t first_base_address = UINT_MAX;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto page_entry = page_table_[page_number];
    if (first_base_address == UINT_MAX) {
      first_base_address = page_entry.base_address;
    } else if (first_base_address != page_entry.base_address) {
      REXSYS_ERROR("BaseHeap::Protect failed due to request spanning regions");
      return false;
    }
    if (!(page_entry.state & memory::kMemoryAllocationCommit)) {
      REXSYS_ERROR("BaseHeap::Protect failed due to uncommitted page");
      return false;
    }
  }

  // Change host protection directly when guest pages are at least as large as
  // host pages. Smaller guest pages are reconciled after the page table update.
  if (page_size_ >= memory_->system_page_size_) {
    const uint32_t page_count = end_page_number - start_page_number + 1;
    memory::PageAccess old_protect_access;
    if (!rex::memory::Protect(TranslateRelative(start_page_number << page_size_shift_),
                              page_count << page_size_shift_, ToHostPageAccess(protect, heap_type_),
                              old_protect ? &old_protect_access : nullptr)) {
      REXSYS_ERROR("BaseHeap::Protect failed due to host VirtualProtect failure");
      return false;
    }

    if (old_protect) {
#if REX_PLATFORM_SWITCH
      // The host permission is no longer the guest's (ToHostPageAccess).
      *old_protect = page_table_[start_page_number].current_protect;
#else
      *old_protect = FromPageAccess(old_protect_access);
#endif
    }
  } else {
    if (old_protect) {
      *old_protect = page_table_[start_page_number].current_protect;
    }
  }

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.current_protect = protect;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info) {
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  if (start_page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QueryRegionInfo base page out of range");
    return false;
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  auto start_page_entry = page_table_[start_page_number];
  out_info->base_address = base_address;
  out_info->allocation_base = 0;
  out_info->allocation_protect = 0;
  out_info->region_size = 0;
  out_info->state = 0;
  out_info->protect = 0;
  if (start_page_entry.state) {
    // Committed/reserved region.
    out_info->allocation_base = heap_base_ + (start_page_entry.base_address << page_size_shift_);
    out_info->allocation_protect = start_page_entry.allocation_protect;
    out_info->allocation_size = start_page_entry.region_page_count << page_size_shift_;
    out_info->state = start_page_entry.state;
    out_info->protect = start_page_entry.current_protect;

    // Scan forward and report the size of the region matching the initial
    // base address's attributes.
    for (uint32_t page_number = start_page_number;
         page_number < start_page_entry.base_address + start_page_entry.region_page_count;
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.base_address != start_page_entry.base_address ||
          page_entry.state != start_page_entry.state ||
          page_entry.current_protect != start_page_entry.current_protect) {
        // Different region or different properties within the region; done.
        break;
      }
      out_info->region_size += page_size_;
    }
  } else {
    // Free region.
    for (uint32_t page_number = start_page_number; page_number < page_table_.size();
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.state) {
        // First non-free page; done with region.
        break;
      }
      out_info->region_size += page_size_;
    }
  }
  return true;
}

bool BaseHeap::QuerySize(uint32_t address, uint32_t* out_size) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryBaseAndSize(uint32_t* in_out_address, uint32_t* out_size) {
  uint32_t page_number = (*in_out_address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *in_out_address = (page_entry.base_address << page_size_shift_);
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryProtect(uint32_t address, uint32_t* out_protect) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QueryProtect base page out of range");
    *out_protect = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_protect = page_entry.current_protect;
  return true;
}

rex::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
  if (low_address > high_address || low_address < heap_base_ ||
      (high_address - heap_base_) >= heap_size_) {
    return rex::memory::PageAccess::kNoAccess;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  bool all_readable = true;
  bool all_writable = true;
  {
    std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
    for (uint32_t i = low_page_number; i <= high_page_number; ++i) {
      uint32_t page_protect = page_table_[i].current_protect;
      if (!(page_protect & memory::kMemoryProtectRead)) {
        all_readable = false;
      }
      if (!(page_protect & memory::kMemoryProtectWrite) &&
          !(page_protect & memory::kMemoryProtectWriteCombine)) {
        all_writable = false;
      }
    }
  }
  if (all_readable && all_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else if (all_readable) {
    return rex::memory::PageAccess::kReadOnly;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

VirtualHeap::VirtualHeap() = default;

VirtualHeap::~VirtualHeap() = default;

void VirtualHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                             uint32_t heap_base, uint32_t heap_size, uint32_t page_size) {
  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size);
}

PhysicalHeap::PhysicalHeap() : parent_heap_(nullptr) {}

PhysicalHeap::~PhysicalHeap() = default;

void PhysicalHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                              uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                              VirtualHeap* parent_heap) {
  uint32_t host_address_offset;
  if (heap_base >= 0xE0000000 && rex::memory::allocation_granularity() > 0x1000) {
    host_address_offset = 0x1000;
  } else {
    host_address_offset = 0;
  }

  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size,
                       host_address_offset);
  parent_heap_ = parent_heap;
  system_page_size_ = uint32_t(rex::memory::page_size());

  system_page_count_ =
      (size_t(heap_size_) + host_address_offset + (system_page_size_ - 1)) / system_page_size_;
  system_page_flags_.resize((system_page_count_ + 63) / 64);
}

bool PhysicalHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                         uint32_t protect, bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Default top-down. Since parent heap is bottom-up this prevents
  // collisions.
  top_down = true;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  uint32_t parent_heap_start = GetPhysicalAddress(heap_base_);
  uint32_t parent_heap_end = GetPhysicalAddress(heap_base_ + (heap_size_ - 1));
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_heap_start, parent_heap_end, size, alignment,
                                allocation_type, protect, top_down, &parent_address)) {
    REXSYS_ERROR(
        "PhysicalHeap::Alloc unable to alloc physical memory in parent heap: size={:08X} "
        "alignment={:08X} tipo={:08X} | padre libre={} paginas de {} ({:.1f} MB de {} MB)",
        size, alignment, allocation_type, parent_heap_->GetUnreservedPageCount(), page_size_,
        double(parent_heap_->GetUnreservedPageCount()) * page_size_ / 1048576.0,
        double(parent_heap_->GetTotalPageCount()) * page_size_ / 1048576.0);
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - parent_heap_start;
  if (!BaseHeap::AllocFixed(address, size, alignment, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                              uint32_t allocation_type, uint32_t protect) {
  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Fixed commits can touch an existing GPU-owned allocation. Preserve it
  // before taking heap_mutex_ or entering any recursively locked heap code.
  bool provider_failed = false;
  TriggerCallbacks(global_critical_region_.Acquire(), base_address, size, true, true, true, false,
                    &provider_failed);
  if (provider_failed) {
    return false;
  }

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  // NOTE: this can potentially overwrite heap contents if there are already
  // committed pages in the requested physical range.
  // TODO(benvanik): flag for ensure-not-committed?
  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->AllocFixed(parent_base_address, size, alignment, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_base_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixed(address, size, page_size_, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }

  return true;
}

bool PhysicalHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                              uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                              bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Global critical region before heap_mutex_ - see
  // AcquireHostPageReconcileLock for why the order matters.
  auto global_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  low_address = std::max(heap_base_, low_address);
  high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
  uint32_t parent_low_address = GetPhysicalAddress(low_address);
  uint32_t parent_high_address = GetPhysicalAddress(high_address);
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_low_address, parent_high_address, size, alignment,
                                allocation_type, protect, top_down, &parent_address)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixed(address, size, page_size_, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                                   uint32_t protect, bool top_down, uint32_t* out_address) {
  return Alloc(size, alignment, allocation_type, protect, top_down, out_address);
}

bool PhysicalHeap::Decommit(uint32_t address, uint32_t size) {
  auto global_lock = global_critical_region_.Acquire();

  bool provider_failed = false;
  TriggerCallbacks(std::move(global_lock), address, size, true, true, true, true, &provider_failed);
  if (provider_failed) {
    return false;
  }
  global_lock = global_critical_region_.Acquire();
  uint32_t parent_address = GetPhysicalAddress(address);
  if (!parent_heap_->Decommit(parent_address, size)) {
    REXSYS_ERROR("PhysicalHeap::Decommit failed due to parent heap failure");
    return false;
  }

  return BaseHeap::Decommit(address, size);
}

bool PhysicalHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto global_lock = global_critical_region_.Acquire();

  uint32_t region_size;
  if (!QuerySize(base_address, &region_size)) {
    return false;
  }
  bool provider_failed = false;
  TriggerCallbacks(std::move(global_lock), base_address, region_size, true, true, true, true,
                    &provider_failed);
  if (provider_failed) {
    return false;
  }
  global_lock = global_critical_region_.Acquire();
  if (!parent_heap_->Release(GetPhysicalAddress(base_address), out_region_size)) {
    REXSYS_ERROR("PhysicalHeap::Release failed due to parent heap failure");
    return false;
  }

  return BaseHeap::Release(base_address, out_region_size);
}

bool PhysicalHeap::Protect(uint32_t address, uint32_t size, uint32_t protect,
                           uint32_t* old_protect) {
  auto global_lock = global_critical_region_.Acquire();

  // Only invalidate if making writable again, for simplicity - not when simply
  // marking some range as immutable, for instance.
  if (protect & memory::kMemoryProtectWrite) {
    bool provider_failed = false;
    TriggerCallbacks(std::move(global_lock), address, size, true, true, false, false, &provider_failed);
    if (provider_failed) {
      return false;
    }
  }

  if (!parent_heap_->Protect(GetPhysicalAddress(address), size, protect, old_protect)) {
    REXSYS_ERROR("PhysicalHeap::Protect failed due to parent heap failure");
    return false;
  }

  return BaseHeap::Protect(address, size, protect);
}

void PhysicalHeap::EnableAccessCallbacks(uint32_t physical_address, uint32_t length,
                                          bool enable_invalidation_notifications,
                                          bool enable_data_providers) {
  if (!length || (!enable_invalidation_notifications && !enable_data_providers)) {
    return;
  }
  uint32_t physical_offset = GetPhysicalAddress(heap_base_);
  if (physical_address < physical_offset) {
    if (physical_offset - physical_address >= length) {
      return;
    }
    length -= physical_offset - physical_address;
    physical_address = physical_offset;
  }
  uint32_t relative = physical_address - physical_offset;
  if (relative >= heap_size_) {
    return;
  }
  length = std::min(length, heap_size_ - relative);
  uint32_t first = (relative + host_address_offset()) / system_page_size_;
  uint32_t last = (relative + length - 1 + host_address_offset()) / system_page_size_;
  auto lock = global_critical_region_.Acquire();
  uint32_t run_first = UINT32_MAX;
  rex::memory::PageAccess run_access = rex::memory::PageAccess::kNoAccess;
  const auto flush = [&](uint32_t end) {
    if (run_first != UINT32_MAX) {
      rex::memory::Protect(membase_ + heap_base_ + size_t(run_first) * system_page_size_,
                           size_t(end - run_first) * system_page_size_, run_access);
    }
  };
  for (uint32_t page = first; page <= last; ++page) {
    const uint32_t guest_page = rex::sat_sub(page * system_page_size_, host_address_offset()) >>
                                page_size_shift_;
    if (guest_page >= page_table_.size()) {
      flush(page); run_first = UINT32_MAX;
      continue;
    }
    const auto access = GetHostPageAccess(page, rex::memory::PageAccess::kNoAccess);
    if (access == rex::memory::PageAccess::kNoAccess) {
      flush(page); run_first = UINT32_MAX;
      continue;
    }
    auto& flags = system_page_flags_[page >> 6];
    const uint64_t bit = uint64_t(1) << (page & 63);
    if (enable_invalidation_notifications && access == rex::memory::PageAccess::kReadWrite) {
      flags.notify_on_invalidation |= bit;
    }
    if (enable_data_providers) {
      flags.data_provider |= bit;
      ++flags.provider_generation;
    }
    // Reapply even when already flagged: a guest allocation/protection change
    // may have changed host protection since the previous GPU write.
    const auto protected_access = flags.data_provider & bit
        ? rex::memory::PageAccess::kNoAccess
        : (flags.notify_on_invalidation & bit ? rex::memory::PageAccess::kReadOnly : access);
    if (run_first == UINT32_MAX || protected_access != run_access) {
      flush(page);
      run_first = page;
      run_access = protected_access;
    }
  }
  flush(last + 1);
}

void PhysicalHeap::DisableDataProviders() {
  auto lock = global_critical_region_.Acquire();
  for (uint32_t page = 0; page < system_page_count_; ++page) {
    auto& flags = system_page_flags_[page >> 6];
    const uint64_t bit = uint64_t(1) << (page & 63);
    if (!(flags.data_provider & bit)) {
      continue;
    }
    flags.data_provider &= ~bit;
    ++flags.provider_generation;
    auto access = GetHostPageAccess(page, REXCVAR_GET(protect_on_release)
          ? rex::memory::PageAccess::kNoAccess : rex::memory::PageAccess::kReadWrite);
    if (access == rex::memory::PageAccess::kReadWrite && (flags.notify_on_invalidation & bit)) {
      access = rex::memory::PageAccess::kReadOnly;
    }
    rex::memory::Protect(membase_ + heap_base_ + size_t(page) * system_page_size_,
                         system_page_size_, access);
  }
}

bool PhysicalHeap::TriggerCallbacks(std::unique_lock<std::recursive_mutex> global_lock_locked_once,
                                     uint32_t virtual_address, uint32_t length, bool is_write,
                                     bool unwatch_exact_range, bool unprotect, bool discard,
                                     bool* provider_failed) {
  if (provider_failed) {
    *provider_failed = false;
  }
  if (virtual_address < heap_base_) {
    if (heap_base_ - virtual_address >= length) {
      return false;
    }
    length -= heap_base_ - virtual_address;
    virtual_address = heap_base_;
  }
  uint32_t relative = virtual_address - heap_base_;
  if (!length || relative >= heap_size_) {
    return false;
  }
  length = std::min(length, heap_size_ - relative);
  uint32_t first = (relative + host_address_offset()) / system_page_size_;
  uint32_t last = (relative + length - 1 + host_address_offset()) / system_page_size_;
  const uint32_t physical_offset = GetPhysicalAddress(heap_base_);
  const uint32_t physical_first = rex::sat_sub(first * system_page_size_, host_address_offset()) +
                                  physical_offset;
  const uint32_t physical_end = std::min(
      rex::sat_sub((last + 1) * system_page_size_, host_address_offset()) + physical_offset,
      physical_offset + heap_size_);
  const uint32_t original_physical_first = GetPhysicalAddress(virtual_address);
  const uint32_t original_physical_end = original_physical_first + length;
  bool watched = false;

  // Providers may release the global region. Recheck generations before
  // clearing protection; another GPU write may have rearmed any touched page.
  for (;;) {
    std::vector<std::pair<uint32_t, uint64_t>> generations;
    for (uint32_t block = first >> 6; block <= last >> 6; ++block) {
      generations.emplace_back(block, system_page_flags_[block].provider_generation);
    }
    bool supplied = true;
    for (uint32_t page = first; page <= last; ++page) {
      const auto& flags = system_page_flags_[page >> 6];
      const uint64_t bit = uint64_t(1) << (page & 63);
      watched |= (flags.data_provider & bit) != 0 ||
                 (is_write && (flags.notify_on_invalidation & bit) != 0);
      if (!(flags.data_provider & bit)) {
        continue;
      }
      const uint32_t start =
          (rex::sat_sub(page * system_page_size_, host_address_offset()) + physical_offset) &
          ~(system_page_size_ - 1);
      const uint32_t end =
          (std::min(rex::sat_sub((page + 1) * system_page_size_, host_address_offset()) +
                        physical_offset, physical_offset + heap_size_) + system_page_size_ - 1) &
          ~(system_page_size_ - 1);
      // Discard applies to canonical physical pages, including the leading
      // bytes outside the E alias when a 16 KB host page has a 4 KB offset.
      if (!(discard && original_physical_first <= start && original_physical_end >= end) &&
          !memory_->ProvidePhysicalMemoryData(global_lock_locked_once, start, end - start)) {
        if (provider_failed) {
          *provider_failed = true;
        }
        return false;
      }
    }
    for (const auto& generation : generations) {
      if (system_page_flags_[generation.first].provider_generation != generation.second) {
        supplied = false;
        break;
      }
    }
    if (supplied) {
      break;
    }
  }
  if (!watched) {
    return false;
  }

  // Invalidation happens only after data delivery, including for exact partial
  // writes. The callback may widen the write range, but never across providers.
  uint32_t unwatch_first = physical_first;
  uint32_t unwatch_end = physical_end;
  if (is_write) {
    uint32_t allowed_first = 0, allowed_last = UINT32_MAX;
    for (auto callback : memory_->physical_memory_invalidation_callbacks_) {
      auto range = callback->first(callback->second, physical_first,
                                    physical_end - physical_first, unwatch_exact_range || !unprotect);
      allowed_first = std::max(allowed_first, range.first);
      allowed_last = std::min(allowed_last, rex::sat_add(range.first,
                                                       std::max(range.second, uint32_t(1)) - 1));
    }
    if (!unwatch_exact_range && unprotect) {
      constexpr uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
      unwatch_first = std::max(std::min(allowed_first, physical_first),
                               physical_first & ~(kMaxUnwatchExcess - 1));
      unwatch_end = std::min(std::max(allowed_last, physical_end - 1),
                             (physical_end - 1) | (kMaxUnwatchExcess - 1)) + 1;
      unwatch_end = std::min(unwatch_end, physical_offset + heap_size_);
      unwatch_first = std::max(unwatch_first, physical_offset);
      first = (unwatch_first - physical_offset + host_address_offset()) / system_page_size_;
      last = (unwatch_end - 1 - physical_offset + host_address_offset()) / system_page_size_;
    }
  }

  for (uint32_t page = first; page <= last; ++page) {
    auto& flags = system_page_flags_[page >> 6];
    const uint64_t bit = uint64_t(1) << (page & 63);
    const uint32_t start = rex::sat_sub(page * system_page_size_, host_address_offset()) +
                           physical_offset;
    const bool requested = start >= physical_first && start < physical_end;
    if (requested) {
      flags.data_provider &= ~bit;
    }
    // A widened invalidation must leave an unrelated provider protected.
    if (flags.data_provider & bit) {
      continue;
    }
    if (is_write) {
      flags.notify_on_invalidation &= ~bit;
    }
    if (unprotect) {
      const uint32_t guest_page = rex::sat_sub(page * system_page_size_, host_address_offset()) >>
                                  page_size_shift_;
      auto access = GetHostPageAccess(page, REXCVAR_GET(protect_on_release)
          ? rex::memory::PageAccess::kNoAccess : rex::memory::PageAccess::kReadWrite);
      if (access == rex::memory::PageAccess::kReadWrite && (flags.notify_on_invalidation & bit)) {
        access = rex::memory::PageAccess::kReadOnly;
      }
      rex::memory::Protect(membase_ + heap_base_ + size_t(page) * system_page_size_,
                           system_page_size_, access);
    }
  }
  return true;
}

std::unique_lock<std::recursive_mutex> PhysicalHeap::AcquireHostPageReconcileLock() const {
  // Allocation/protection changes must preserve provider protection, even when
  // guest pages are larger than host pages.
  return rex::thread::global_critical_region::AcquireDirect();
}

bool PhysicalHeap::IsHostPageWriteWatched(uint32_t host_page_number) const {
  // Indices match EnableAccessCallbacks: both count host pages from
  // membase_ + heap_base_, including host_address_offset().
  if (host_page_number >= system_page_count_) {
    return false;
  }
  return (system_page_flags_[host_page_number >> 6].notify_on_invalidation &
          (uint64_t(1) << (host_page_number & 63))) != 0;
}

bool PhysicalHeap::IsHostPageDataWatched(uint32_t host_page_number) const {
  return host_page_number < system_page_count_ &&
         (system_page_flags_[host_page_number >> 6].data_provider &
          (uint64_t(1) << (host_page_number & 63))) != 0;
}

rex::memory::PageAccess PhysicalHeap::GetHostPageAccess(
    uint32_t host_page_number, rex::memory::PageAccess fallback) const {
  const uint64_t start = std::max(uint64_t(host_page_number) * system_page_size_,
                                   uint64_t(host_address_offset()));
  const uint64_t end = std::min(uint64_t(host_page_number + 1) * system_page_size_,
                                 uint64_t(host_address_offset()) + heap_size_);
  if (start >= end) {
    return rex::memory::PageAccess::kNoAccess;
  }
  bool readable = false, writable = false, alias_committed = false;
  uint32_t relative = uint32_t(start - host_address_offset());
  const uint32_t relative_end = uint32_t(end - host_address_offset());
  while (relative < relative_end) {
    const uint32_t guest_page = relative >> page_size_shift_;
    const auto& entry = page_table_[guest_page];
    alias_committed |= (entry.state & memory::kMemoryAllocationCommit) != 0;
    uint32_t protect = entry.current_protect;
    uint32_t step = std::min(relative_end - relative,
                             page_size_ - (relative & (page_size_ - 1)));
    if (!(entry.state & memory::kMemoryAllocationCommit)) {
      // Mapped A/C/E mirrors are accessible without separate alias allocations.
      // Use canonical protection for those mirrors, but preserve an explicitly
      // committed alias's guest protection (including genuine no-access).
      const uint32_t physical = GetPhysicalAddress(heap_base_ + relative);
      protect = 0;
      parent_heap_->QueryProtect(physical, &protect);
      step = std::min(step, parent_heap_->page_size() - (physical & (parent_heap_->page_size() - 1)));
    }
    const auto access = ToPageAccess(protect);
    readable |= access != rex::memory::PageAccess::kNoAccess;
    writable |= access == rex::memory::PageAccess::kReadWrite;
    relative += step;
  }
  return writable ? rex::memory::PageAccess::kReadWrite
                  : (readable ? rex::memory::PageAccess::kReadOnly
                              : (alias_committed ? rex::memory::PageAccess::kNoAccess : fallback));
}

uint32_t PhysicalHeap::GetPhysicalAddress(uint32_t address) const {
  assert_true(address >= heap_base_);
  address -= heap_base_;
  assert_true(address < heap_size_);
  if (heap_base_ >= 0xE0000000) {
    address += 0x1000;
  }
  return address;
}

}  // namespace rex::memory
