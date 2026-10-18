"""Check CP scratch and ordered GPU events (sincronizacion = synchronization).

Compile actual SHD/packet handlers and D3D12 write/readback functions against a
deferred device fake. Check publication after GPU completion without blocking
unrelated GPU work, same-page GPU consumers, idle/short/long polling progress and
failed delivery. Actual provider ownership/coalescing/queue behavior is checked
by comprobar_coherencia_gpu.py. No GPU or test framework is needed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    level = 1
    end = opening + 1
    while level:
        level += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>
#define REXGPU_ERROR(...) ((void)0)
#define SCOPE_profile_cpu_f(...) ((void)0)
#define REXCVAR_GET(...) true
#define FAILED(v) ((v)<0)
#define SUCCEEDED(v) ((v)>=0)
using HANDLE=void*;
constexpr unsigned XE_GPU_REG_VGT_EVENT_INITIATOR = 0, XE_GPU_REG_COHER_STATUS_HOST=1;
constexpr unsigned XE_GPU_REG_SCRATCH_REG0=8,XE_GPU_REG_SCRATCH_REG7=15,
  XE_GPU_REG_SCRATCH_UMSK=16,XE_GPU_REG_SCRATCH_ADDR=17;
namespace rex {uint32_t byte_swap(uint32_t v){return __builtin_bswap32(v);}}
using UINT64=uint64_t;
constexpr uint32_t D3D12_RESOURCE_STATE_COPY_DEST=1,
  D3D12_RESOURCE_STATE_COPY_SOURCE=2,D3D12_RESOURCE_STATE_UNORDERED_ACCESS=4;
using D3D12_RESOURCE_STATES=uint32_t;
struct D3D12_RANGE { size_t Begin,End; };
std::vector<std::string> events;
struct ID3D12Resource {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(65536);
  bool map_fail=false;
  int Map(uint32_t,const D3D12_RANGE*,void** ptr) {
    if(map_fail) return -1; *ptr=bytes.data(); return 0;
  }
  void Unmap(uint32_t,const D3D12_RANGE*) {}
};
namespace xenos {
enum class Endian { kNone,k8in16,k8in32,k16in32 };
uint32_t GpuSwap(uint32_t value,Endian endian) {
  switch(endian) {
    case Endian::k8in16: return ((value&0x00FF00FF)<<8)|((value&0xFF00FF00)>>8);
    case Endian::k8in32: return __builtin_bswap32(value);
    case Endian::k16in32: return (value<<16)|(value>>16);
    default: return value;
  }
}
}
using xenos::GpuSwap;
namespace rex::thread {
struct Event {bool signaled=false;void* native_handle(){return this;}void Set(){signaled=true;}};
std::function<void()> on_yield;
void MaybeYield() {if(on_yield) on_yield();}
void SyncMemory() {}
template<class T> void Sleep(T) { assert(false); }
}
namespace rex::memory { size_t page_size() {return 4096;} }
namespace memory {
struct RingBuffer {
  std::vector<uint32_t> words; size_t cursor=0;
  template<class T> T ReadAndSwap() { return T(words.at(cursor++)); }
};
template<class T> void store(void* destination,T value) {
  events.push_back("cpu-publish"); std::memcpy(destination,&value,sizeof(value));
}
template<class T> T load(const void* source) { T value; std::memcpy(&value,source,sizeof(value)); return value; }
}
struct Memory {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(65536);
  std::function<bool(uint32_t,uint32_t)> provider;
  template<class T=uint8_t*> T TranslatePhysical(uint32_t address) {
    assert(address<bytes.size()); return reinterpret_cast<T>(bytes.data()+address);
  }
  template<class T=uint8_t*> T TranslateVirtual(uint32_t address) {
    assert((address&0xE0000000u)==0xA0000000u);
    address&=0x1FFFFFFFu;
    if(provider && !provider(address,4)) throw std::runtime_error("provider failed");
    return TranslatePhysical<T>(address);
  }
};
struct SharedMemory { static constexpr uint32_t kBufferSize=512u<<20; };
struct RegisterFile {uint32_t values[32]{};};
struct CommandProcessor {
  Memory memory;
  Memory* memory_=&memory;
  uint32_t counter_=77;
  RegisterFile reg_file;
  RegisterFile* register_file_=&reg_file;
  uint32_t (&registers)[32]=reg_file.values;
  void ScratchForCheck(uint32_t index,uint32_t value);
  bool worker_running_=true;
  std::unique_ptr<rex::thread::Event> write_ptr_index_event_=std::make_unique<rex::thread::Event>();
  virtual bool WriteGpuMemory(uint32_t,uint32_t);
  void WriteRegister(uint32_t reg,uint32_t value) { registers[reg]=value; }
  uint32_t ReadRegisterValue(uint32_t reg) { return registers[reg]; }
  void MakeCoherent() {}
  void ProcessPendingFunctions() {}
  virtual void PrepareForWait() {}
  void ReturnFromWait() {}
  bool ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer*,uint32_t,uint32_t);
  bool ExecutePacketType3_MEM_WRITE(memory::RingBuffer*,uint32_t,uint32_t);
  bool ExecutePacketType3_REG_TO_MEM(memory::RingBuffer*,uint32_t,uint32_t);
  bool ExecutePacketType3_COND_WRITE(memory::RingBuffer*,uint32_t,uint32_t);
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer*,uint32_t,uint32_t);
  bool Shd(uint32_t initiator=3,uint32_t address=512,uint32_t value=19) {
    memory::RingBuffer reader{{initiator,address,value}};
    bool ok=ExecutePacketType3_EVENT_WRITE_SHD(&reader,0,3);
    assert(reader.cursor==3); return ok;
  }
  uint32_t RawWord(uint32_t address=512) { return memory::load<uint32_t>(memory.bytes.data()+address); }
};
struct D3D12CommandProcessor;
struct UploadPool {
  ID3D12Resource resource;
  size_t used=0;
  bool fail=false;
  uint8_t* Request(uint64_t,size_t size,size_t alignment,ID3D12Resource** buffer,
                   size_t* offset,void*) {
    if(fail) return nullptr;
    assert(alignment==4 && used+size<=resource.bytes.size());
    *buffer=&resource; *offset=used; used+=size; return resource.bytes.data()+*offset;
  }
};
struct D3D12SharedMemory : SharedMemory {
  D3D12CommandProcessor& command_processor_;
  Memory& memory;
  ID3D12Resource resource;
  ID3D12Resource* buffer_=&resource;
  D3D12_RESOURCE_STATES buffer_state_=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool buffer_uav_writes_commit_needed_=true;
  std::unique_ptr<UploadPool> upload_buffer_pool_=std::make_unique<UploadPool>();
  bool provider=true,request_ok=true;
  bool dirty[16]{},valid[16]{};
  explicit D3D12SharedMemory(D3D12CommandProcessor& cp,Memory& m):command_processor_(cp),memory(m) {}
  bool HasCpuReadback() const { return provider; }
  bool RequestRange(uint32_t address,uint32_t length) {
    if(!request_ok) return false;
    assert(length==4 && address+length<=memory.bytes.size());
    std::pair<uint32_t,uint32_t> range{address,length};
    if(!PrepareRangesForGpu(&range,1)) return false;
    const uint32_t page=address/4096;
    if(!valid[page]) {
      std::memcpy(resource.bytes.data()+page*4096,memory.bytes.data()+page*4096,4096);
      valid[page]=true;
    }
    return true;
  }
  void RangeWrittenByGpu(uint32_t address,uint32_t length) {
    assert(length==4); dirty[address/4096]=true;
  }
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES);
  void UseAsCopySource() { CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE); }
  ID3D12Resource* GetBuffer() { return buffer_; }
  bool WriteGpuMemory(uint32_t,uint32_t);
  bool SynchronizeForCpu(uint32_t,uint32_t);
  bool PrepareRangesForGpu(const std::pair<uint32_t,uint32_t>*,size_t);
  std::pair<uint32_t,uint32_t> MemoryInvalidationCallback(uint32_t address,uint32_t length,bool) {
    valid[address/4096]=false; dirty[address/4096]=false; return {address,length};
  }
};
struct DeferredList {
  std::vector<std::function<void()>> pending;
  void D3DCopyBufferRegion(ID3D12Resource* dst,uint64_t to,ID3D12Resource* src,uint64_t from,uint64_t size) {
    events.push_back("queue-copy");
    pending.push_back([=] {
      events.push_back("gpu-copy");
      std::memcpy(dst->bytes.data()+to,src->bytes.data()+from,size);
    });
  }
};
struct D3D12CommandProcessor : CommandProcessor {
  D3D12SharedMemory shared{*this,memory};
  D3D12SharedMemory* shared_memory_=&shared;
  DeferredList deferred_command_list_;
  ID3D12Resource readback;
  bool device_removed_=false,begin_ok=true,end_ok=true,complete_ok=true;
  uint32_t waits=0,readbacks=0;
  std::vector<std::pair<uint32_t,uint32_t>> last_ranges;
  D3D12CommandProcessor() {
    memory.provider=[this](uint32_t a,uint32_t n) {return shared.SynchronizeForCpu(a,n);};
  }
  struct PacketMemoryWrite {uint64_t submission;uint32_t address,value;};
  std::deque<PacketMemoryWrite> packet_memory_writes_;
  bool publishing_packet_memory_writes_=false;
  uint64_t packet_wakeup_submission_=0;
  bool submission_open_=false,queue_operations_done_since_submission_signal_=false;
  uint64_t submission_current_=1,submission_completed_=0;
  struct Fence {
    uint64_t completed=0;unsigned registrations=0;bool register_ok=true;
    std::vector<std::pair<uint64_t,rex::thread::Event*>> wakeups;
    uint64_t GetCompletedValue(){return completed;}
    int SetEventOnCompletion(uint64_t number,HANDLE handle) {
      ++registrations;if(!register_ok)return -1;
      auto* event=static_cast<rex::thread::Event*>(handle);
      if(number<=completed) event->Set();else wakeups.push_back({number,event});
      return 0;
    }
    void SignalReady() {
      for(auto it=wakeups.begin();it!=wakeups.end();) {
        if(it->first<=completed){it->second->Set();it=wakeups.erase(it);}else ++it;
      }
    }
  } fence;
  Fence* submission_fence_=&fence;
  struct Submission {uint64_t number;std::vector<std::function<void()>> work;};
  std::deque<Submission> submitted;
  void AdvanceTo(uint64_t number) {
    while(!submitted.empty() && submitted.front().number<=number) {
      auto work=std::move(submitted.front().work);fence.completed=submitted.front().number;
      submitted.pop_front();for(auto& fn:work) fn();fence.SignalReady();
    }
  }
  void PublishPacketMemoryWrites(uint64_t);
  bool PreparePacketMemoryReads(const std::pair<uint32_t,uint32_t>*,size_t);
  void PrepareForWait() override;
  void CheckSubmissionFence(uint64_t number) {
    if(number>=submission_current_ && submission_open_ && !EndSubmission(false)) return;
    number=std::min(number,submission_current_-1);
    if(fence.completed<number) {++waits;if(complete_ok) AdvanceTo(number);}
    submission_completed_=fence.completed;PublishPacketMemoryWrites(submission_completed_);
  }
  uint64_t GetCurrentSubmission() const { return submission_current_; }
  DeferredList& GetDeferredCommandList() { return deferred_command_list_; }
  void SubmitBarriers() { events.push_back("barriers"); }
  void PushUAVBarrier(ID3D12Resource*) { events.push_back("uav-barrier"); }
  void PushTransitionBarrier(ID3D12Resource*,uint32_t,uint32_t) { events.push_back("transition"); }
  bool BeginSubmission(bool) { if(begin_ok) submission_open_=true; return begin_ok; }
  bool EndSubmission(bool) {
    if(!end_ok)return false;
    if(submission_open_) {
      submitted.push_back({submission_current_++,std::move(deferred_command_list_.pending)});
      deferred_command_list_.pending.clear();
      submission_open_=false;queue_operations_done_since_submission_signal_=false;
    }
    return true;
  }
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFence(submission_current_);return submission_completed_+1>=submission_current_;
  }
  ID3D12Resource* RequestReadbackBuffer(uint32_t total) {
    assert(total && total<=readback.bytes.size()); ++readbacks; return &readback;
  }
  bool WriteGpuMemory(uint32_t,uint32_t) override;
  bool ReadbackSharedMemoryForCpu(const std::vector<std::pair<uint32_t,uint32_t>>&,std::vector<uint8_t>&);
  uint32_t CpuWord(uint32_t address=512) {
    return memory::load<uint32_t>(memory.TranslateVirtual(0xA0000000u|address));
  }
};
bool D3D12SharedMemory::SynchronizeForCpu(uint32_t address,uint32_t length) {
  if(!provider) return true;
  uint32_t page=address/4096;
  assert(length==4);
  if(!dirty[page]) return true;
  command_processor_.last_ranges={{page*4096,4096}};
  std::vector<uint8_t> bytes;
  if(!command_processor_.ReadbackSharedMemoryForCpu(command_processor_.last_ranges,bytes)) return false;
  assert(bytes.size()==4096);
  std::memcpy(memory.bytes.data()+page*4096,bytes.data(),bytes.size());
  dirty[page]=false; return true;
}
'''

CHECK = r'''
int main() {
  {
    D3D12CommandProcessor p;
    events.clear(); bool done=false;
    p.shared.dirty[2]=true;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([&]{events.push_back("prior-work");done=true;});
    assert(p.Shd() && !done && p.RawWord()==0 && p.waits==0 && p.readbacks==0);
    assert(p.packet_memory_writes_.size()==1 && !p.shared.dirty[0]);
    assert(p.submitted.empty() && p.submission_open_);
    assert(p.EndSubmission(false));
    p.AdvanceTo(1); assert(done && p.RawWord()==0);
    p.CheckSubmissionFence(0);
    assert(p.RawWord()==19 && p.packet_memory_writes_.empty() && p.readbacks==0);
    assert(p.shared.dirty[2]);
    assert(std::find(events.begin(),events.end(),"prior-work") <
           std::find(events.begin(),events.end(),"cpu-publish"));
    assert(p.Shd(0x80000003) && p.RawWord()==77);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd(3,512,11));
    assert(p.EndSubmission(false));
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd(3,516,22));
    p.AdvanceTo(1);p.CheckSubmissionFence(0);
    assert(p.RawWord(512)==11 && p.RawWord(516)==0);
    assert(p.shared.RequestRange(4096,4) && p.waits==0 && p.RawWord(516)==0);
    // A neighbouring fetch uploads the whole host page, including the word.
    assert(p.shared.RequestRange(128,4) && p.waits==1 && p.RawWord(516)==22);
    assert(memory::load<uint32_t>(p.shared.resource.bytes.data()+516)==22);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);
    bool completed=false;
    p.deferred_command_list_.pending.push_back([&]{completed=true;});
    for(unsigned i=0;i<18;++i) assert(p.Shd(3,512+i*4,i+1));
    assert(p.submitted.empty() && p.submission_current_==1 && !completed);
    for(unsigned i=0;i<18;++i) assert(p.RawWord(512+i*4)==0);
    p.PrepareForWait();
    assert(!completed && p.submission_current_==2 && p.waits==0 && p.fence.registrations==1);
    for(unsigned i=0;i<100;++i) p.PrepareForWait();
    assert(p.fence.registrations==1 && !p.write_ptr_index_event_->signaled);
    p.AdvanceTo(1);assert(p.write_ptr_index_event_->signaled);
    p.PrepareForWait();
    assert(completed && p.waits==0);
    for(unsigned i=0;i<18;++i) assert(p.RawWord(512+i*4)==i+1);
  }
  {
    D3D12CommandProcessor p;
    p.queue_operations_done_since_submission_signal_=true;
    assert(p.Shd() && p.RawWord()==0 && p.submission_open_);
    p.PrepareForWait();assert(p.RawWord()==0);p.AdvanceTo(1);p.PrepareForWait();assert(p.RawWord()==19);
    D3D12CommandProcessor failed;
    failed.queue_operations_done_since_submission_signal_=true;failed.begin_ok=false;
    assert(!failed.Shd() && failed.RawWord()==0);
  }
  {
    D3D12CommandProcessor p;
    p.registers[XE_GPU_REG_SCRATCH_ADDR]=4096;
    p.registers[XE_GPU_REG_SCRATCH_UMSK]=0xFF;
    bool gpu_finished=false;
    p.BeginSubmission(false);
    p.deferred_command_list_.pending.push_back([&]{gpu_finished=true;});
    assert(p.Shd(3,512,77)); // PS_DONE remains completion-ordered.
    for(unsigned i=0;i<8;++i) {
      p.ScratchForCheck(XE_GPU_REG_SCRATCH_REG0+i,i+1);
      assert(p.RawWord(4096+i*4)==rex::byte_swap(i+1));
    }
    assert(!gpu_finished && p.waits==0 && p.RawWord(512)==0);
    // First VBlank acknowledges scratch 1 before prior GPU work completes.
    memory::store(p.memory.bytes.data()+4100,uint32_t(0));
    p.PrepareForWait();assert(!gpu_finished && p.RawWord(4100)==0);
    p.AdvanceTo(1);p.PrepareForWait();
    assert(gpu_finished && p.RawWord(512)==77 && p.RawWord(4100)==0);
    assert(p.readbacks==0);
    p.registers[XE_GPU_REG_SCRATCH_UMSK]=0;
    p.ScratchForCheck(XE_GPU_REG_SCRATCH_REG0+1,1);
    assert(p.RawWord(4100)==0);
  }
  {
    D3D12CommandProcessor p;
    p.registers[XE_GPU_REG_SCRATCH_ADDR]=4096;
    p.registers[XE_GPU_REG_SCRATCH_UMSK]=2;
    p.shared.dirty[1]=p.shared.dirty[2]=true;
    p.BeginSubmission(false);
    p.deferred_command_list_.pending.push_back([&]{p.shared.resource.bytes[4160]=123;});
    p.ScratchForCheck(XE_GPU_REG_SCRATCH_REG0+1,1);
    assert(p.RawWord(4100)==rex::byte_swap(1) && p.memory.bytes[4160]==123);
    assert(p.readbacks==1 && !p.shared.dirty[1] && p.shared.dirty[2]);
  }
  for (uint32_t interval : {0u,8u,0x100u}) {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd());
    memory::RingBuffer wait{{0x13,512,19,UINT32_MAX,interval}};
    rex::thread::on_yield=[&]{p.AdvanceTo(1);};
    assert(p.ExecutePacketType3_WAIT_REG_MEM(&wait,0,5) && p.RawWord()==19 && p.waits==0);
    rex::thread::on_yield={};
    // The fixture enables VSync; Sleep() asserts, so this also checks polling.
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd() && p.RawWord()==0);
    p.PrepareForWait();assert(p.RawWord()==0 && !p.packet_memory_writes_.empty());
    p.AdvanceTo(1);assert(p.write_ptr_index_event_->signaled);
    p.PrepareForWait();assert(p.RawWord()==19 && p.packet_memory_writes_.empty());
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd());p.fence.register_ok=false;
    p.PrepareForWait();assert(p.RawWord()==19 && p.waits==1);
  }
  for(uint32_t endian=0;endian<4;++endian) {
    D3D12CommandProcessor p;
    assert(p.Shd(3,512|endian,0x12345678));
    assert(p.RawWord()==GpuSwap(0x12345678,static_cast<xenos::Endian>(endian)));
    assert(p.readbacks==0);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    memory::RingBuffer writes{{513,0x11223344,0x55667788}};
    assert(p.ExecutePacketType3_MEM_WRITE(&writes,0,3) && p.RawWord()==0);
    assert(p.shared.RequestRange(512,4));
    assert(p.RawWord(512)==0x22114433 && p.RawWord(516)==0x66558877);
    p.registers[2]=0x12345678;
    memory::RingBuffer reg{{2,1024}};
    assert(p.ExecutePacketType3_REG_TO_MEM(&reg,0,2) && p.RawWord(1024)==0x12345678);
    memory::RingBuffer conditional{{0x113,1024,0x12345678,UINT32_MAX,2048,91}};
    assert(p.ExecutePacketType3_COND_WRITE(&conditional,0,6) && p.RawWord(2048)==91);
    assert(p.readbacks==0);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.end_ok=false;assert(p.Shd() && p.RawWord()==0);
    p.PrepareForWait();assert(p.RawWord()==0 && !p.packet_memory_writes_.empty());
    assert(!p.shared.RequestRange(512,4) && p.RawWord()==0);
    p.end_ok=true;p.device_removed_=true;assert(!p.Shd() && p.RawWord()==0);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});
    assert(p.Shd());p.complete_ok=false;
    assert(!p.shared.RequestRange(512,4) && p.RawWord()==0);
    assert(!p.packet_memory_writes_.empty());
    p.complete_ok=true;assert(p.shared.RequestRange(512,4) && p.RawWord()==19);
  }
  {
    D3D12CommandProcessor p;
    p.shared.dirty[0]=p.shared.dirty[2]=true;p.shared.resource.bytes[64]=123;
    assert(p.Shd() && p.RawWord()==19 && p.memory.bytes[64]==123);
    assert(p.readbacks==1 && !p.shared.dirty[0] && p.shared.dirty[2]);
    assert((p.last_ranges==std::vector<std::pair<uint32_t,uint32_t>>({{0,4096}})));
  }
  {
    D3D12CommandProcessor p;
    p.shared.dirty[0]=true;p.readback.map_fail=true;
    assert(!p.Shd() && p.RawWord()==0 && p.shared.dirty[0] && p.packet_memory_writes_.empty());
    p.readback.map_fail=false;assert(p.Shd() && p.RawWord()==19);
  }
  {
    D3D12CommandProcessor p;
    p.BeginSubmission(false);p.deferred_command_list_.pending.push_back([]{});assert(p.Shd());
    p.EndSubmission(false);p.device_removed_=true;p.AdvanceTo(1);p.CheckSubmissionFence(0);assert(p.RawWord()==0);
  }
  {
    CommandProcessor other;assert(other.Shd() && other.RawWord()==19);
    D3D12CommandProcessor scaled;scaled.shared.provider=false;bool done=false;
    scaled.BeginSubmission(false);scaled.deferred_command_list_.pending.push_back([&]{done=true;});
    assert(scaled.Shd() && done && scaled.RawWord()==19 && scaled.waits==1);
    scaled.BeginSubmission(false);scaled.deferred_command_list_.pending.push_back([]{});scaled.complete_ok=false;
    assert(!scaled.Shd(3,512,29) && scaled.RawWord()==19);
  }
  puts("Production async packet writes: publication after completion, batched words, ordered prefix, no word roundtrip;");
  puts("unrelated GPU reads stay asynchronous; same-page GPU consumers wait before upload;");
  puts("scratch CP writeback before completion preserves CPU VBlank ACK; dirty neighbours remain protected;\nnonblocking idle/WAIT_REG_MEM progress, single fence wake registration, wake failure fallback;\nVSync, endian/counter, failures and scaled path: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    common = (ROOT / "sdk/src/graphics/command_processor.cpp").read_text(encoding="utf-8")
    backend = (ROOT / "sdk/src/graphics/d3d12/command_processor.cpp").read_text(encoding="utf-8")
    shared = (ROOT / "sdk/src/graphics/d3d12/shared_memory.cpp").read_text(encoding="utf-8")
    # All CP consumers/metadata stores use provider-protected aliases. Only the
    # default write implementation (for backends without providers) remains raw.
    assert common.count("TranslatePhysical") == 1
    worker = function(common, "void CommandProcessor::WorkerThreadMain(")
    idle = worker[worker.index("do {"):worker.index("ReturnFromWait();")]
    assert "PrepareForWait();" in idle, "Idle fence wakeups must poll completed notifications"

    for signature in ("bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(",
                      "bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(",
                      "bool CommandProcessor::ExecutePacketType3_MEM_WRITE(",
                      "bool CommandProcessor::ExecutePacketType3_COND_WRITE(",
                      "bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT("):
        assert "WriteGpuMemory(" in function(common, signature), signature
    source = PREAMBLE + function(common, "bool CommandProcessor::WriteGpuMemory(")
    scratch = function(function(common, "void CommandProcessor::WriteRegister("),
                       "if (index >= XE_GPU_REG_SCRATCH_REG0")
    assert "WriteGpuMemory(" not in scratch and "TranslateVirtual" in scratch
    source += "void CommandProcessor::ScratchForCheck(uint32_t index,uint32_t value) { auto& regs=*register_file_;" + scratch + "}\n"

    for suffix in ("EVENT_WRITE_SHD", "MEM_WRITE", "REG_TO_MEM", "COND_WRITE", "WAIT_REG_MEM"):
        source += function(common, "bool CommandProcessor::ExecutePacketType3_" + suffix + "(")
    source += function(shared, "void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(")
    source += function(shared, "bool D3D12SharedMemory::WriteGpuMemory(")
    source += function(backend, "bool D3D12CommandProcessor::WriteGpuMemory(")
    source += function(backend, "void D3D12CommandProcessor::PublishPacketMemoryWrites(")
    source += function(backend, "bool D3D12CommandProcessor::PreparePacketMemoryReads(")
    source += function(backend, "void D3D12CommandProcessor::PrepareForWait(")
    source += function(shared, "bool D3D12SharedMemory::PrepareRangesForGpu(")
    source += function(backend, "bool D3D12CommandProcessor::ReadbackSharedMemoryForCpu(")
    output = ROOT / "out/pruebas_sincronizacion_gpu"
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output / "sincronizacion.cpp", output / "sincronizacion.exe"
    cpp.write_text(source + CHECK, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++17", "-fuse-ld=lld", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
