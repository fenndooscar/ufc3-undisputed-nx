"""Check file I/O with GPU memory (lectura_memoria_gpu = GPU-memory reads).

Compile XFile's actual physical-read and prepared-write branches with a provider
fake. Check bounded staging, partial/error reads, GPU-neighbour preservation and
no disk/guest writes after failed delivery. No GPU or test framework is needed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from comprobar_sincronizacion_gpu import function

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>
#include <cstdio>
using X_STATUS=int32_t;
constexpr X_STATUS X_STATUS_SUCCESS=0,X_STATUS_NO_MEMORY=-1,X_STATUS_UNSUCCESSFUL=-2,X_STATUS_ACCESS_VIOLATION=-4;
bool XSUCCEEDED(X_STATUS status) { return status>=0; }
namespace rex::thread { struct global_critical_region { static int AcquireDirect() { return 0; } }; }
namespace rex::memory {
enum class HeapType { kGuestPhysical };
struct BaseHeap { HeapType heap_type() const { return HeapType::kGuestPhysical; } };
}
namespace memory=rex::memory;
struct Memory {
  std::vector<uint8_t> cpu=std::vector<uint8_t>(1<<20,0);
  bool stale=true,provider_fail=false;
  rex::memory::BaseHeap* heap=nullptr;
  uint8_t* TranslateVirtual(uint32_t address) { return cpu.data()+address; }
  rex::memory::BaseHeap* LookupHeap(uint32_t) { return heap; }
};
namespace rex::memory {
struct PhysicalHeap : BaseHeap {
  Memory* ram;
  unsigned reads=0,writes=0;
  bool TriggerCallbacks(int,uint32_t,uint32_t,bool write,bool,bool,bool,bool* failed) {
    write ? ++writes : ++reads;
    *failed=ram->provider_fail;
    if (*failed) return false;
    if (ram->stale) { std::fill(ram->cpu.begin(),ram->cpu.end(),42); ram->stale=false; }
    return true;
  }
};
}
struct File {
  Memory* ram;
  std::vector<uint8_t> data,written;
  unsigned calls=0,fail_on=0;
  size_t maximum_chunk=0;
  bool bad_length=false;
  X_STATUS ReadSync(std::span<uint8_t> output,size_t offset,size_t* bytes) {
    ++calls; maximum_chunk=std::max(maximum_chunk,output.size()); *bytes=0;
    if (calls==fail_on) return -3;
    if (bad_length) { *bytes=output.size()+1; return 0; }
    if (offset>=data.size()) return 0;
    *bytes=std::min(output.size(),data.size()-offset);
    std::memcpy(output.data(),data.data()+offset,*bytes); return 0;
  }
  X_STATUS WriteSync(std::span<const uint8_t> input,size_t,size_t* bytes) {
    assert(!ram->stale); ++calls;
    written.assign(input.begin(),input.end()); *bytes=input.size(); return 0;
  }
};
struct Context {
  Memory ram;
  rex::memory::PhysicalHeap physical{{},&ram};
  File file{&ram};
  File* file_=&file;
  Context() { ram.heap=&physical; }
  Memory* memory() { return &ram; }
  struct Result { X_STATUS status; size_t bytes; };
  Result Read(uint32_t buffer_guest_address,uint32_t buffer_length,uint64_t byte_offset=0) {
    auto* buffer_physical_heap=&physical;
    size_t bytes_read=0;
    X_STATUS result=X_STATUS_SUCCESS;
'''
BETWEEN = r'''
    return {result,bytes_read};
  }
  Result Write(uint32_t buffer_guest_address,uint32_t buffer_length,uint64_t byte_offset=0) {
    size_t bytes_written=0;
    X_STATUS result=X_STATUS_ACCESS_VIOLATION;
'''
CHECK = r'''
    return {result,bytes_written};
  }
};
void contents(const Context& c,size_t start,size_t length) {
  assert(std::all_of(c.ram.cpu.begin(),c.ram.cpu.begin()+start,[](auto b){return b==42;}));
  assert(std::all_of(c.ram.cpu.begin()+start,c.ram.cpu.begin()+start+length,[](auto b){return b==165;}));
  assert(std::all_of(c.ram.cpu.begin()+start+length,c.ram.cpu.end(),[](auto b){return b==42;}));
}
int main() {
  Context full;
  full.file.data.assign(131089,165);
  auto a=full.Read(97,131089);
  assert(a.status==0 && a.bytes==131089 && full.file.calls==3 && full.file.maximum_chunk==65536);
  contents(full,97,131089);
  Context short_read;
  short_read.file.data.assign(23,165);
  auto b=short_read.Read(103,400);
  assert(b.status==0 && b.bytes==23 && short_read.file.calls==1);
  contents(short_read,103,23);
  Context error;
  error.file.data.assign(200000,165); error.file.fail_on=2;
  auto c=error.Read(111,140000);
  assert(c.status==-3 && c.bytes==65536 && error.file.calls==2);
  contents(error,111,65536);
  Context malformed;
  malformed.file.bad_length=true;
  auto d=malformed.Read(117,16);
  assert(d.status==X_STATUS_UNSUCCESSFUL && d.bytes==0 && malformed.ram.stale);
  Context failed_read;
  failed_read.file.data.assign(23,165); failed_read.ram.provider_fail=true;
  auto e=failed_read.Read(119,23);
  assert(e.status==X_STATUS_UNSUCCESSFUL && e.bytes==0 && failed_read.ram.cpu[119]==0);
  Context write;
  auto f=write.Write(121,23);
  assert(f.status==0 && f.bytes==23 && write.physical.reads==1 && write.physical.writes==0);
  assert(write.file.written==std::vector<uint8_t>(23,42));
  Context failed_write;
  failed_write.ram.provider_fail=true;
  auto g=failed_write.Write(123,23);
  assert(g.status==X_STATUS_UNSUCCESSFUL && g.bytes==0 && failed_write.file.calls==0);
  Context overflow;
  auto h=overflow.Write(UINT32_MAX,8);
  assert(h.status==X_STATUS_ACCESS_VIOLATION && h.bytes==0 && overflow.file.calls==0);
  puts("Production file I/O: 64 KiB chunks, short/error reads, GPU neighbours, prepared writes, provider failure: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    source = (ROOT / "sdk/src/system/xfile.cpp").read_text(encoding="utf-8")
    read = function(source, "          if (buffer_physical_heap) {")
    write = function(source, "  if (UINT32_MAX - buffer_guest_address >= buffer_length) {")
    output = ROOT / "out/pruebas_lectura_memoria_gpu"
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output / "actual.cpp", output / "actual.exe"
    cpp.write_text(PREAMBLE + read + BETWEEN + write + CHECK, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-fuse-ld=lld", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
