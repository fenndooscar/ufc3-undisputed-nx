"""Check GPU coherency (comprobar = check) with actual SDK provider functions.

Compiles the production SharedMemory implementation and PhysicalHeap provider
functions against a tiny physical-memory/device fake. No GPU, game data, new
library or test framework. Generated C++ and executable stay in ignored out/.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

RAIZ = Path(__file__).resolve().parents[1]

PREAMBULO = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#define assert_true(v) assert(v)
#define assert_false(v) assert(!(v))
#define assert_always(...) assert(false)
#define COUNT_profile_set(...) ((void)0)
#define SCOPE_profile_cpu_f(...) ((void)0)
#define REXSYS_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
#define REXCVAR_DEFINE_BOOL(...)
#define REXCVAR_GET(...) false
namespace rex {
uint32_t log2_ceil(uint32_t v) { return 32 - __builtin_clz(v - 1); }
template<class T> T sat_sub(T a, T b) { return a > b ? a - b : 0; }
uint32_t sat_add(uint32_t a, uint32_t b) { return UINT32_MAX - a < b ? UINT32_MAX : a + b; }
bool bit_scan_forward(uint32_t v, uint32_t* b) { if (!v) return false; *b=__builtin_ctz(v); return true; }
bool bit_scan_forward(uint64_t v, uint32_t* b) { if (!v) return false; *b=__builtin_ctzll(v); return true; }
uint8_t lzcnt(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
uint8_t tzcnt(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
namespace bit {
std::pair<size_t,size_t> GetNextRangeUnset(const uint64_t*, size_t, size_t) { return {0,0}; }
void SetRange(uint64_t*, size_t, size_t) {}
}
namespace thread {
std::recursive_mutex region;
struct global_critical_region {
  auto Acquire() const { return std::unique_lock<std::recursive_mutex>(region); }
  static auto AcquireDirect() { return std::unique_lock<std::recursive_mutex>(region); }
};
}
namespace memory {
constexpr uint32_t kMemoryAllocationCommit = 2;
constexpr uint32_t kMemoryProtectRead = 1, kMemoryProtectWrite = 2;
enum class PageAccess { kNoAccess, kReadOnly, kReadWrite };
size_t page_size() { return 4096; }
std::vector<std::pair<uintptr_t, PageAccess>> protections;
bool Protect(void* p, size_t length, PageAccess access) {
  for (size_t offset=0; offset<length; offset+=page_size()) {
    uintptr_t address=reinterpret_cast<uintptr_t>(p)+offset;
    auto it=std::find_if(protections.begin(),protections.end(),[&](const auto& v){return v.first==address;});
    if(it==protections.end()) protections.emplace_back(address,access); else it->second=access;
  }
  return true;
}
PageAccess ToPageAccess(uint32_t flags) {
  return flags & kMemoryProtectWrite ? PageAccess::kReadWrite :
         (flags & kMemoryProtectRead ? PageAccess::kReadOnly : PageAccess::kNoAccess);
}
struct HeapAllocationInfo { uint32_t state=0, protect=0, region_size=0; };
struct VirtualHeap {
  uint32_t committed_end=16384;
  uint32_t page_size() const { return 4096; }
  bool QueryProtect(uint32_t p,uint32_t* value) {
    *value=p<committed_end ? kMemoryProtectRead|kMemoryProtectWrite : 0; return true;
  }
  bool QueryRegionInfo(uint32_t address,HeapAllocationInfo* info) {
    info->state=address<committed_end ? kMemoryAllocationCommit : 0;
    info->protect=info->state ? kMemoryProtectRead|kMemoryProtectWrite : 0;
    info->region_size=(address<committed_end ? committed_end : 0x20000000)-address;
    return true;
  }
};
class Memory;
struct PhysicalHeap {
  Memory* memory_=nullptr;
  VirtualHeap* parent_heap_=nullptr;
  uint8_t* membase_=reinterpret_cast<uint8_t*>(uintptr_t(0x100000000));
  uint32_t heap_base_=0xA0000000, heap_size_=65536, page_size_=4096, page_size_shift_=12;
  uint32_t system_page_size_=4096, system_page_count_=16, host_address_offset_=0;
  thread::global_critical_region global_critical_region_;
  struct Entry { uint32_t current_protect=3,state=kMemoryAllocationCommit; };
  struct SystemPageFlagsBlock { uint64_t notify_on_invalidation=0,data_provider=0,provider_generation=0; };
  std::vector<Entry> page_table_=std::vector<Entry>(16);
  std::vector<SystemPageFlagsBlock> system_page_flags_=std::vector<SystemPageFlagsBlock>(1);
  uint32_t host_address_offset() const { return host_address_offset_; }
  uint32_t GetPhysicalAddress(uint32_t address) const;
  bool IsHostPageWriteWatched(uint32_t page) const;
  bool IsHostPageDataWatched(uint32_t page) const;
  PageAccess GetHostPageAccess(uint32_t page,PageAccess fallback) const;
  void EnableAccessCallbacks(uint32_t,uint32_t,bool,bool);
  bool TriggerCallbacks(std::unique_lock<std::recursive_mutex>,uint32_t,uint32_t,bool,bool,bool=true,bool=false,bool* =nullptr);
  void DisableDataProviders();
  bool Access(uint32_t physical,bool write,bool discard=false,uint32_t length=1) {
    uint32_t guest=heap_base_+physical-(heap_base_>=0xE0000000 ? 0x1000 : 0);
    return TriggerCallbacks(global_critical_region_.Acquire(),guest,length,write,true,true,discard);
  }
  PageAccess HostAccess(uint32_t physical) const {
    uintptr_t p=reinterpret_cast<uintptr_t>(membase_)+heap_base_+
                (physical-(heap_base_>=0xE0000000 ? 0x1000 : 0)+host_address_offset_)/4096*4096;
    auto it=std::find_if(protections.begin(),protections.end(),[&](const auto& v){return v.first==p;});
    return it==protections.end() ? PageAccess::kReadWrite : it->second;
  }
};
class Memory {
 public:
  std::vector<uint8_t> ram=std::vector<uint8_t>(65536,0x31);
  thread::global_critical_region global_critical_region_;
  using PhysicalMemoryInvalidationCallback=std::pair<uint32_t,uint32_t>(*)(void*,uint32_t,uint32_t,bool);
  using PhysicalMemoryDataProvider=bool(*)(void*,std::unique_lock<std::recursive_mutex>&,uint32_t,uint32_t);
  struct PhysicalDataProvider {
    PhysicalMemoryDataProvider callback; void* context;
    bool registered=true; size_t active_calls=0; std::condition_variable_any idle;
  };
  std::vector<std::pair<PhysicalMemoryInvalidationCallback,void*>*> physical_memory_invalidation_callbacks_;
  std::vector<std::shared_ptr<PhysicalDataProvider>> physical_memory_data_providers_;
  VirtualHeap physical;
  PhysicalHeap aliases[3];
  explicit Memory(uint32_t size=65536) {
    ram.resize(size,0x31);
    for(size_t i=0;i<3;++i) {
      aliases[i].heap_size_=size;
      aliases[i].system_page_count_=size/4096;
      aliases[i].page_table_.resize(size/4096);
      aliases[i].system_page_flags_.resize((size/4096+63)/64);
      aliases[i].memory_=this; aliases[i].parent_heap_=&physical;
      aliases[i].heap_base_=i==0 ? 0xA0000000 : (i==1 ? 0xC0000000 : 0xE0000000);
      if(i==2) aliases[i].host_address_offset_=0x1000;
    }
    // C is a mapped mirror with no independent allocation metadata.
    for(auto& e:aliases[1].page_table_) e={0,0};
  }
  uint8_t* TranslatePhysical(uint32_t p) { assert(p<ram.size()); return ram.data()+p; }
  VirtualHeap* GetPhysicalHeap() { return &physical; }
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback cb,void* ctx) {
    auto p=new std::pair<PhysicalMemoryInvalidationCallback,void*>(cb,ctx);
    auto lock=global_critical_region_.Acquire(); physical_memory_invalidation_callbacks_.push_back(p); return p;
  }
  void UnregisterPhysicalMemoryInvalidationCallback(void* p) {
    auto lock=global_critical_region_.Acquire();
    auto it=std::find(physical_memory_invalidation_callbacks_.begin(),physical_memory_invalidation_callbacks_.end(),p);
    if(it!=physical_memory_invalidation_callbacks_.end()) { delete *it; physical_memory_invalidation_callbacks_.erase(it); }
  }
  void EnablePhysicalMemoryAccessCallbacks(uint32_t a,uint32_t n,bool inv,bool data) {
    for(auto& alias:aliases) alias.EnableAccessCallbacks(a,n,inv,data);
  }
  void* RegisterPhysicalMemoryDataProvider(PhysicalMemoryDataProvider,void*);
  void UnregisterPhysicalMemoryDataProvider(void*);
  bool ProvidePhysicalMemoryData(std::unique_lock<std::recursive_mutex>&,uint32_t,uint32_t);
  void DisablePhysicalMemoryDataProviders() {
    for(auto& alias:aliases) alias.DisableDataProviders();
  }
};
}
}
'''

QUEUE = r'''
namespace rex::thread {
struct Fence {
  std::mutex mutex; std::condition_variable condition; bool signaled=false;
  void Signal() { std::lock_guard<std::mutex> lock(mutex); signaled=true; condition.notify_one(); }
  void Wait() { std::unique_lock<std::mutex> lock(mutex); condition.wait(lock,[&]{return signaled;}); }
};
}
namespace rex::system {
struct Worker { std::thread::id id; };
struct XThread { static bool IsInThread(Worker* w) { return w && w->id==std::this_thread::get_id(); } };
}
namespace rex::graphics {
struct CommandProcessor {
  struct Event { void Set() {} };
  std::atomic<bool> worker_running_{true};
  std::unique_ptr<rex::system::Worker> worker_thread_=std::make_unique<rex::system::Worker>();
  std::unique_ptr<Event> write_ptr_index_event_=std::make_unique<Event>();
  std::mutex pending_fns_mutex_;
  std::queue<std::function<void()>> pending_fns_;
  bool QueueInThread(std::function<void()> fn);
  void CallInThread(std::function<void()> fn);
  bool CallInThreadAndWait(std::function<bool()> fn);
  bool HasPendingFunctions();
  void ProcessPendingFunctions();
};
}
'''

FINAL = r'''
struct FakeGpu final : rex::graphics::SharedMemory {
  std::function<bool()> before_gpu_read;
  bool PrepareRangesForGpu(const std::pair<uint32_t,uint32_t>*, size_t) override {
    return !before_gpu_read || before_gpu_read();
  }
  rex::memory::Memory& ram;
  std::vector<uint8_t> gpu;
  std::vector<std::vector<std::pair<uint32_t,uint32_t>>> batches;
  uint32_t services=0, uploads=0;
  bool fail=false;
  std::function<void()> during_readback;
  explicit FakeGpu(rex::memory::Memory& m):SharedMemory(m),ram(m),gpu(m.ram.size(),0xEE) {
    InitializeCommon(true);
  }
  ~FakeGpu() { ShutdownCommon(); }
  bool UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>& ranges) override {
    for(auto range:ranges) {
      uint32_t start=range.first*4096,n=range.second*4096;
      assert(start+n<=gpu.size()); MakeRangeValid(start,n,false);
      std::memcpy(gpu.data()+start,ram.ram.data()+start,n); ++uploads;
    }
    return true;
  }
  bool ReadbackForCpu(const std::vector<std::pair<uint32_t,uint32_t>>& ranges,
                      std::vector<uint8_t>& bytes) override {
    ++services;
    batches.push_back(ranges);
    // Another thread must be able to acquire the global region during service.
    assert(std::async(std::launch::async,[] {
      bool free=rex::thread::region.try_lock();
      if(free) rex::thread::region.unlock(); return free;
    }).get());
    if(fail) return false;
    for(auto range:ranges) {
      assert(range.first+range.second<=gpu.size());
      bytes.insert(bytes.end(),gpu.begin()+range.first,gpu.begin()+range.first+range.second);
    }
    if(during_readback) { auto fn=std::move(during_readback); during_readback={}; fn(); }
    return true;
  }
  void Write(uint32_t a,uint32_t n,uint8_t value,bool exact=true) {
    assert(RequestRange(a,n));
    std::fill(gpu.begin()+a,gpu.begin()+a+n,value); RangeWrittenByGpu(a,n,exact);
  }
};
int main() {
  using namespace rex::memory;
  protections.clear();
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.before_gpu_read=[&] {ram.ram[0x1234]=0x7B;return true;};
    assert(gpu.RequestRange(0x1234,4) && gpu.gpu[0x1234]==0x7B);
    gpu.MemoryInvalidationCallback(0x1234,4,true);
    const auto uploads=gpu.uploads;
    gpu.before_gpu_read=[] {return false;};
    assert(!gpu.RequestRange(0x1234,4) && gpu.uploads==uploads);
  }
  {
    rex::graphics::CommandProcessor processor;
    std::promise<void> started;
    std::thread worker([&] {
      processor.worker_thread_->id=std::this_thread::get_id(); started.set_value();
      while(processor.worker_running_) {
        processor.ProcessPendingFunctions(); std::this_thread::yield();
      }
      processor.ProcessPendingFunctions();
    });
    started.get_future().wait();
    int calls=0;
    assert(processor.CallInThreadAndWait([&] {
      ++calls;
      return processor.CallInThreadAndWait([&] {++calls; return true;});
    }));
    {
      std::lock_guard<std::mutex> lock(processor.pending_fns_mutex_);
      processor.worker_running_=false;
    }
    worker.join();
    assert(calls==2 && !processor.CallInThreadAndWait([] {return true;}));
  }
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.Write(0x1100,16,0xA7);
    for(auto& alias:ram.aliases) assert(alias.HostAccess(0x1100)==PageAccess::kNoAccess);
    assert(ram.ram[0x1100]==0x31);
    assert(ram.aliases[1].Access(0x1100,false)); // metadata-free C mirror
    assert(ram.ram[0x1100]==0xA7 && ram.ram[0x10FF]==0x31 && ram.ram[0x1110]==0x31);
    assert(ram.aliases[1].HostAccess(0x1100)==PageAccess::kReadOnly);
    assert(ram.aliases[2].Access(0x1108,true)); // E alias partial store
    ram.ram[0x1108]=0x5B;
    assert(gpu.RequestRange(0x1100,16));
    assert(gpu.gpu[0x1100]==0xA7 && gpu.gpu[0x1108]==0x5B && gpu.gpu[0x10FF]==0x31);
    assert(gpu.services==1);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.Write(0x2100,16,0x77);
    gpu.during_readback=[&] {
      std::fill(gpu.gpu.begin()+0x2100,gpu.gpu.begin()+0x2110,0x92);
      gpu.RangeWrittenByGpu(0x2100,16);
    };
    assert(ram.aliases[0].Access(0x2100,false));
    assert(gpu.services==2 && ram.ram[0x2100]==0x92);
    assert(ram.aliases[0].HostAccess(0x2100)==PageAccess::kReadOnly);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.Write(0x3100,16,0x61);
    gpu.during_readback=[&] {
      assert(ram.aliases[2].Access(0x3000,true,true,4096));
      std::fill(ram.ram.begin()+0x3000,ram.ram.begin()+0x4000,0xBC);
    };
    assert(ram.aliases[0].Access(0x3100,false));
    assert(ram.ram[0x3100]==0xBC && gpu.services==1);
    assert(gpu.RequestRange(0x3100,16) && gpu.gpu[0x3100]==0xBC);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.Write(0x1100,16,0x60); gpu.fail=true;
    bool provider_failed=false;
    assert(!ram.aliases[0].TriggerCallbacks(ram.global_critical_region_.Acquire(),
        0xA0001100,1,true,true,true,false,&provider_failed));
    assert(provider_failed);
    assert(ram.aliases[0].HostAccess(0x1100)==PageAccess::kNoAccess);
    assert(ram.ram[0x1100]==0x31);
    gpu.fail=false; assert(ram.aliases[0].Access(0x1100,true));
    assert(ram.ram[0x1100]==0x60);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    assert(gpu.RequestAllAllocatedRanges());
    gpu.RangeWrittenByGpu(0,rex::graphics::SharedMemory::kBufferSize,false);
    assert(gpu.SynchronizeForCpu(0,rex::graphics::SharedMemory::kBufferSize));
    assert(gpu.services==1 && ram.ram[16383]==0x31 && ram.ram[16384]==0x31);
    // Unknown destinations never claim unallocated/uninitialized GPU padding.
    assert(ram.aliases[0].HostAccess(0x4100)!=PageAccess::kNoAccess);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    ram.physical.committed_end=0x8000;
    ram.aliases[2].system_page_size_=16384;
    ram.aliases[2].system_page_count_=4;
    gpu.Write(0x0100,16,0x8C);
    gpu.Write(0x3100,16,0xA9);
    gpu.Write(0x6100,16,0xCE);
    // E starts 4 KB into the first 16 KB host page. Neither touched canonical
    // page is fully discarded; GPU bytes on both sides must be delivered first.
    assert(ram.aliases[2].Access(0x1000,true,true,0x4000));
    assert(ram.ram[0x0100]==0x8C && ram.ram[0x6100]==0xCE);
    assert(gpu.RequestRange(0,0x8000));
    assert(gpu.gpu[0x0100]==0x8C && gpu.gpu[0x6100]==0xCE);
  }
  {
    Memory ram;
    auto gpu=std::make_unique<FakeGpu>(ram);
    gpu->Write(0x1100,16,0xC7);
    assert(gpu->SynchronizeForCpu(0,rex::graphics::SharedMemory::kBufferSize));
    gpu.reset();
    for(auto& alias:ram.aliases) assert(alias.HostAccess(0x1100)!=PageAccess::kNoAccess);
    assert(ram.ram[0x1100]==0xC7);
    struct Lifetime { std::promise<void> entered,finish; } life;
    auto finish=life.finish.get_future().share();
    struct Context { Lifetime* life; std::shared_future<void>* finish; } context{&life,&finish};
    auto handle=ram.RegisterPhysicalMemoryDataProvider(
      [](void* p,std::unique_lock<std::recursive_mutex>& lock,uint32_t,uint32_t) {
        auto& c=*static_cast<Context*>(p); lock.unlock(); c.life->entered.set_value();
        c.finish->wait(); lock.lock(); return true;
      },&context);
    auto active=std::async(std::launch::async,[&] {
      auto lock=ram.global_critical_region_.Acquire(); return ram.ProvidePhysicalMemoryData(lock,4096,4096);
    });
    life.entered.get_future().wait();
    auto unregister=std::async(std::launch::async,[&] {ram.UnregisterPhysicalMemoryDataProvider(handle);});
    assert(unregister.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    life.finish.set_value(); assert(active.get()); unregister.get();
  }
  {
    Memory ram(0x600000); FakeGpu gpu(ram);
    ram.physical.committed_end=uint32_t(ram.ram.size());
    gpu.Write(0x10000,0x400000,0xA2);
    gpu.Write(0x1000,4,0x77);
    assert(gpu.services==0 && ram.ram[0x1000]==0x31 && ram.ram[0x10000]==0x31);
    assert(ram.aliases[1].Access(0x1000,false));
    assert(gpu.services==1 && gpu.batches.back().size()==1);
    assert(gpu.batches.back()[0]==std::make_pair(0x1000u,0x1000u));
    assert(ram.ram[0x1000]==0x77 && ram.ram[0x10000]==0x31);
    // A fault in the middle must pull the whole 4 MB GPU write in one batch.
    assert(ram.aliases[0].Access(0x121000,false));
    assert(gpu.services==2 && gpu.batches.back().size()==1);
    assert(gpu.batches.back()[0]==std::make_pair(0x10000u,0x400000u));
    for(uint32_t page=0x10000;page<0x410000;page+=4096) {
      assert(ram.aliases[2].Access(page,false));
      assert(ram.ram[page]==0xA2 && ram.ram[page+4095]==0xA2);
    }
    assert(gpu.services==2); // No 1024 page-by-page GPU waits.
    assert(gpu.SynchronizeForCpu(0x500000,4096) && gpu.services==2);
  }
  {
    Memory ram; FakeGpu gpu(ram);
    gpu.Write(0x1000,0x3000,0x72);
    gpu.during_readback=[&] {
      // This page is an extra part of the coalesced run, not the fault page.
      assert(ram.aliases[1].Access(0x2000,true,true,4096));
      std::fill(ram.ram.begin()+0x2000,ram.ram.begin()+0x3000,0xBC);
    };
    assert(ram.aliases[0].Access(0x1100,false));
    assert(gpu.services==1 && ram.ram[0x1100]==0x72 && ram.ram[0x3100]==0x72);
    assert(ram.ram[0x2100]==0xBC);
    assert(gpu.RequestRange(0x2000,4096) && gpu.gpu[0x2100]==0xBC);
  }
  for (bool exact : {true, false}) {
    Memory ram(0x600000); FakeGpu gpu(ram);
    ram.physical.committed_end=uint32_t(ram.ram.size());
    gpu.Write(0x10000,0x400000,0xA2,exact);
    gpu.Write(0x410000,0x100000,0xB3,exact);
    assert(ram.aliases[0].Access(0x121000,false));
    assert(gpu.services==1 && gpu.batches.back().size()==1);
    assert(gpu.batches.back()[0]==std::make_pair(0x10000u,0x400000u));
    assert(ram.ram[0x121000]==0xA2 && ram.ram[0x410000]==0x31);
    assert(ram.aliases[0].Access(0x420000,false));
    assert(gpu.services==2 && gpu.batches.back().size()==1);
    assert(gpu.batches.back()[0]==std::make_pair(0x410000u,0x100000u));
    assert(ram.ram[0x420000]==0xB3);
  }
  std::puts("production GPU coherency: same-write batching, adjacent resources isolated, targeted completion, clean no-wait;");
  std::puts("read/partial write/reupload, padding, A/C/E aliases,");
  std::puts("unlocked service, newer-generation retry, free/reuse, failure protection, provider lifetime: OK");
}
'''


def funcion(texto, firma):
    inicio = texto.index(firma)
    llave = texto.index("{", inicio)
    profundidad = 1
    fin = llave + 1
    while profundidad:
        profundidad += (texto[fin] == "{") - (texto[fin] == "}")
        fin += 1
    return texto[inicio:fin] + "\n\n"


def sin_includes(texto):
    return "\n".join(line for line in texto.splitlines() if not line.startswith(("#include", "#pragma once")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    heap = (RAIZ / "sdk/src/system/xmemory.cpp").read_text(encoding="utf-8")
    memoria = sin_includes((RAIZ / "sdk/include/rex/graphics/shared_memory.h").read_text(encoding="utf-8"))
    memoria += "\n" + sin_includes((RAIZ / "sdk/src/graphics/shared_memory.cpp").read_text(encoding="utf-8"))
    firmas = [
        "void* Memory::RegisterPhysicalMemoryDataProvider(",
        "void Memory::UnregisterPhysicalMemoryDataProvider(",
        "bool Memory::ProvidePhysicalMemoryData(",
        "void PhysicalHeap::EnableAccessCallbacks(",
        "bool PhysicalHeap::TriggerCallbacks(",
        "void PhysicalHeap::DisableDataProviders(",
        "bool PhysicalHeap::IsHostPageWriteWatched(",
        "bool PhysicalHeap::IsHostPageDataWatched(",
        "rex::memory::PageAccess PhysicalHeap::GetHostPageAccess(",
        "uint32_t PhysicalHeap::GetPhysicalAddress(",
    ]
    codigo = PREAMBULO + "\nnamespace rex::memory {\n" + "".join(funcion(heap, f) for f in firmas)
    procesador = (RAIZ / "sdk/src/graphics/command_processor.cpp").read_text(encoding="utf-8")
    funciones_cola = ["bool CommandProcessor::QueueInThread(", "void CommandProcessor::CallInThread(",
                      "bool CommandProcessor::CallInThreadAndWait(", "bool CommandProcessor::HasPendingFunctions(",
                      "void CommandProcessor::ProcessPendingFunctions("]
    codigo += "}\n" + memoria + QUEUE + "\nnamespace rex::graphics {\n"
    codigo += "".join(funcion(procesador, f) for f in funciones_cola) + "}\n" + FINAL
    destino = RAIZ / "out/pruebas_coherencia_gpu"
    destino.mkdir(parents=True, exist_ok=True)
    fuente, ejecutable = destino / "coherencia.cpp", destino / "coherencia.exe"
    fuente.write_text(codigo, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++17", "-fuse-ld=lld", str(fuente), "-o", str(ejecutable)], check=True)
    subprocess.run([str(ejecutable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
