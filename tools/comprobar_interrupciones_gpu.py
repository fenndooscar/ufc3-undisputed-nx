"""Check GPU interrupt dispatch (interrupciones = interrupts) in production C++.

Compile the actual dispatcher, graphics interrupt delivery and PM4 interrupt
handler. Guest ISR code must not hold the global memory lock while a provider
needs the GPU. Command interrupts wake the existing worker independently of VBlank.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from comprobar_sincronizacion_gpu import function

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#include <cstdio>
#define SCOPE_profile_cpu_f(...) ((void)0)
#define PROFILE_INTERRUPT_DISPATCHED() ((void)0)
#define assert_true(v) assert(v)
#define assert_not_null(v) assert(v)
#define REXGPU_INFO(...) ((void)0)
namespace rex {
template<class T,size_t N> constexpr size_t countof(const T(&)[N]) { return N; }
namespace thread {
struct Event {
  std::mutex mutex;
  std::condition_variable cv;
  bool signaled=false;
  static std::unique_ptr<Event> CreateAutoResetEvent(bool) { return std::make_unique<Event>(); }
  void Set() { std::lock_guard lock(mutex); signaled=true; cv.notify_one(); }
  bool Wait() {
    std::unique_lock lock(mutex);
    bool ready=cv.wait_for(lock,std::chrono::seconds(2),[&]{return signaled;});
    signaled=false;
    return ready;
  }
};
std::recursive_mutex global_mutex;
struct global_critical_region {
  static std::unique_lock<std::recursive_mutex> AcquireDirect() { return std::unique_lock(global_mutex); }
  auto Acquire() { return AcquireDirect(); }
};
}
namespace memory {
template<class T> T load_and_swap(const void* p) { T v; std::memcpy(&v,p,sizeof(v)); return v; }
template<class T> void store_and_swap(void* p,T v) { std::memcpy(p,&v,sizeof(v)); }
struct Memory { uint32_t pcr=0x12345678; uint8_t* TranslateVirtual(uint32_t a) { assert(a==0); return reinterpret_cast<uint8_t*>(&pcr); } };
struct RingBuffer { uint32_t mask; template<class T> T ReadAndSwap() { return T(mask); } };
}
namespace runtime {
struct Register { uint64_t u64=0; };
struct Context { Register r3,r4,r5,r6,r7,r13; };
struct ThreadState { Context ctx; Context* context() { return &ctx; } };
struct FunctionDispatcher {
  memory::Memory* memory_;
  thread::global_critical_region global_critical_region_;
  bool succeed=true;
  std::function<void(ThreadState*)> callback;
  bool Execute(ThreadState* state,uint32_t) {
    bool free=false;
    std::thread peer([&] { std::unique_lock lock(thread::global_mutex,std::try_to_lock); free=lock.owns_lock(); });
    peer.join();
    assert(free && "Guest ISR holds the global lock needed by GPU providers");
    assert(memory_->pcr==0);
    if(callback) callback(state);
    state->ctx.r3.u64=99;
    return succeed;
  }
  uint64_t ExecuteInterrupt(ThreadState*,uint32_t,uint64_t[],size_t);
};
}
namespace system {
struct XThread {
  runtime::ThreadState state;
  uint32_t cpu=0;
  static XThread* GetCurrentThread();
  auto thread_state() { return &state; }
  void SetActiveCpu(uint32_t value) { cpu=value; }
};
XThread current_thread;
XThread* XThread::GetCurrentThread() { return &current_thread; }
}
namespace graphics {
struct Counter { unsigned value=0; void increment_counter() { ++value; } };
struct GraphicsSystem {
  Counter counter;
  Counter* command_processor_=&counter;
  runtime::FunctionDispatcher* function_dispatcher_;
  uint32_t interrupt_callback_=0,interrupt_callback_data_=0;
  std::atomic<uint32_t> pending_interrupt_cpus_{0};
  std::unique_ptr<thread::Event> interrupt_event_=thread::Event::CreateAutoResetEvent(false);
  void SetInterruptCallback(uint32_t,uint32_t);
  void QueueInterruptCallback(uint32_t);
  void DispatchInterruptCallback(uint32_t,uint32_t);
  void MarkVblank();
  void DispatchPendingInterrupts();
};
struct CommandProcessor {
  GraphicsSystem* graphics_system_;
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer*,uint32_t,uint32_t);
};
}
}
'''
CHECK = r'''
int main() {
  using namespace rex;
  memory::Memory ram;
  runtime::FunctionDispatcher dispatcher{&ram};
  graphics::GraphicsSystem graphics;
  graphics.function_dispatcher_=&dispatcher;
  graphics.SetInterruptCallback(1,17);
  graphics::CommandProcessor command{&graphics};
  std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> delivered;
  dispatcher.callback=[&](runtime::ThreadState* state) {
    uint32_t source=uint32_t(state->ctx.r3.u64),cpu=system::current_thread.cpu;
    delivered.emplace_back(source,cpu,uint32_t(state->ctx.r4.u64));
    if(source==1 && cpu==1) graphics.QueueInterruptCallback(3);
  };
  memory::RingBuffer packet{(1u<<1)|(1u<<4)};
  assert(command.ExecutePacketType3_INTERRUPT(&packet,0,1));
  assert(command.ExecutePacketType3_INTERRUPT(&packet,0,1));
  graphics.QueueInterruptCallback(32);
  assert(delivered.empty() && "GPU thread executed a guest ISR synchronously");
  graphics.DispatchPendingInterrupts();
  assert((delivered==std::vector<std::tuple<uint32_t,uint32_t,uint32_t>>{{1,1,17},{1,4,17}}));
  assert(graphics.counter.value==0 && graphics.pending_interrupt_cpus_==(1u<<3));
  assert(ram.pcr==0x12345678);
  graphics.DispatchPendingInterrupts();
  assert(delivered.size()==3 && delivered.back()==std::make_tuple(1u,3u,17u));
  assert(graphics.pending_interrupt_cpus_==0 && ram.pcr==0x12345678);
  graphics.SetInterruptCallback(0,0);
  graphics.QueueInterruptCallback(2);
  graphics.DispatchPendingInterrupts();
  assert(delivered.size()==3 && graphics.pending_interrupt_cpus_==0);
  // Neither pre-wait nor in-wait notifications depend on a vblank tick.
  graphics.SetInterruptCallback(1,17);
  graphics.QueueInterruptCallback(2);
  assert(graphics.interrupt_event_->Wait());
  graphics.DispatchPendingInterrupts();
  assert(graphics.counter.value==0 && delivered.back()==std::make_tuple(1u,2u,17u));
  std::atomic<bool> waiting=false;
  std::thread worker([&] {
    waiting=true;
    assert(graphics.interrupt_event_->Wait());
    graphics.DispatchPendingInterrupts();
  });
  while(!waiting) std::this_thread::yield();
  graphics.QueueInterruptCallback(4);
  worker.join();
  assert(graphics.counter.value==0 && delivered.back()==std::make_tuple(1u,4u,17u));
  graphics.MarkVblank();
  assert(graphics.counter.value==1 && delivered.back()==std::make_tuple(0u,2u,17u));
  uint64_t args[]={1,17};
  dispatcher.succeed=false;
  auto result=dispatcher.ExecuteInterrupt(system::current_thread.thread_state(),1,args,2);
  assert(result==0xDEADBABE && ram.pcr==0x12345678);
  puts("Production IRQ: unlocked guest execution, queued/coalesced GPU events, wake without vblank, reentrant pending delivery, TLS restore: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to an existing host clang++ compiler")
    dispatcher = (ROOT / "sdk/src/system/function_dispatcher.cpp").read_text(encoding="utf-8")
    graphics = (ROOT / "sdk/src/graphics/graphics_system.cpp").read_text(encoding="utf-8")
    command = (ROOT / "sdk/src/graphics/command_processor.cpp").read_text(encoding="utf-8")
    setup = function(graphics, "X_STATUS GraphicsSystem::SetupGuestGpu(")
    assert "DispatchPendingInterrupts();" in setup
    assert "rex::thread::Wait(interrupt_event_.get()" in setup
    assert "DispatchPendingInterrupts" not in function(graphics, "void GraphicsSystem::MarkVblank(")
    code = PREAMBLE + "\nnamespace rex::runtime {\n" + function(
        dispatcher, "uint64_t FunctionDispatcher::ExecuteInterrupt(") + "\n}\nnamespace rex::graphics {\n"
    for signature in ("void GraphicsSystem::SetInterruptCallback(",
                      "void GraphicsSystem::QueueInterruptCallback(",
                      "void GraphicsSystem::DispatchInterruptCallback(",
                      "void GraphicsSystem::MarkVblank(",
                      "void GraphicsSystem::DispatchPendingInterrupts("):
        code += function(graphics, signature) + "\n"
    code += function(command, "bool CommandProcessor::ExecutePacketType3_INTERRUPT(") + "\n}\n" + CHECK
    output = ROOT / "out/pruebas_interrupciones_gpu"
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output / "actual.cpp", output / "actual.exe"
    cpp.write_text(code, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-fuse-ld=lld", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
