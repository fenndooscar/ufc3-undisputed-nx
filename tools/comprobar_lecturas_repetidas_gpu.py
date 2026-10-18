"""Check repeated GPU reads (comprobar lecturas repetidas) using the SDK function.

Compile the production D3D12 CPU-provider entry point against a serialized worker
fake. No GPU, game data, dependency or test framework is needed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from comprobar_coherencia_gpu import funcion

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#define REXCVAR_GET(...) false
#define REXLOG_INFO(...) ((void)0)
struct CommandProcessor {
  std::mutex worker;
  unsigned copies = 0;
  uint8_t value = 19;
  bool fail = false;
  std::function<void()> during_copy;
  bool CallInThreadAndWait(std::function<bool()> fn) {
    std::lock_guard<std::mutex> lock(worker);
    return fn();
  }
  bool ReadbackSharedMemoryForCpu(
      const std::vector<std::pair<uint32_t,uint32_t>>& ranges,
      std::vector<uint8_t>& data) {
    ++copies;
    if (fail) return false;
    size_t size = 0;
    for (auto range : ranges) size += range.second;
    data.assign(size,value);
    if (during_copy) during_copy();
    return true;
  }
};
struct D3D12SharedMemory {
  static constexpr uint32_t kBufferSize = 1u<<29;
  CommandProcessor command_processor_;
  uint64_t generation = 1;
  uint64_t cpu_readback_cached_generation_ = UINT64_MAX;
  uint32_t cpu_readback_cached_start_ = 0;
  std::vector<uint8_t> cpu_readback_cached_data_;
  unsigned host_page_log2 = 12;
  uint64_t GetMemoryGeneration(uint32_t) { return generation; }
  unsigned page_size_log2() { return host_page_log2; }
  bool ReadbackForCpu(const std::vector<std::pair<uint32_t,uint32_t>>&,
                      std::vector<uint8_t>&);
};
'''
CHECK = r'''
int main() {
  for (unsigned log2 : {12u,14u}) {
    D3D12SharedMemory memory;
    memory.host_page_log2 = log2;
    auto& worker = memory.command_processor_;
    std::vector<std::pair<uint32_t,uint32_t>> page{{0x20000,1u<<log2}};
    std::vector<uint8_t> a,b,c;
    std::thread first([&]{assert(memory.ReadbackForCpu(page,a));});
    std::thread second([&]{assert(memory.ReadbackForCpu(page,b));});
    std::thread third([&]{assert(memory.ReadbackForCpu(page,c));});
    first.join();second.join();third.join();
    assert(worker.copies == 1 && a == b && b == c && a.front() == 19);
    ++memory.generation; worker.value = 23;
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 2 && a.front() == 23);
    // A write or free/reuse advances the requested page's generation.
    ++memory.generation;
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 3);
    page.front().first += 1u<<log2;
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 4);
    // Mutation during readback must not be cached under the newer generation.
    ++memory.generation;
    worker.during_copy = [&]{++memory.generation;};
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 5);
    worker.during_copy = {};
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 6);
    ++memory.generation; worker.fail = true;
    assert(!memory.ReadbackForCpu(page,a) && worker.copies == 7);
    worker.fail = false;
    assert(memory.ReadbackForCpu(page,a) && worker.copies == 8);
    // Do not retain multi-page or fragmented atlas transfers.
    page.front().second *= 2;
    assert(memory.ReadbackForCpu(page,a));assert(memory.ReadbackForCpu(page,b));
    assert(worker.copies == 10);
    assert(memory.ReadbackForCpu({{0x1000,128},{0x3000,128}},a));
    assert(memory.ReadbackForCpu({{0x1000,128},{0x3000,128}},b));
    assert(worker.copies == 12);
    page.front().first += 4; page.front().second = 1u<<log2;
    assert(memory.ReadbackForCpu(page,a));assert(memory.ReadbackForCpu(page,b));
    assert(worker.copies == 14);
  }
  puts("production repeated CPU reads: 3 concurrent faults / 1 GPU copy, 4/16 KB pages;");
  puts("new write, other page, mutation during copy, failure, multi-page/fragmented controls: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    source = (ROOT / "sdk/src/graphics/d3d12/shared_memory.cpp").read_text(encoding="utf-8")
    common = (ROOT / "sdk/src/graphics/shared_memory.cpp").read_text(encoding="utf-8")
    for signature in ("void SharedMemory::MakeRangeValid(",
                      "void SharedMemory::InvalidateAllPages(",
                      "std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback("):
        assert "++next_gpu_generation_" in funcion(common, signature), signature
    output = ROOT / "out/pruebas_lecturas_repetidas_gpu"
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output / "lecturas.cpp", output / "lecturas.exe"
    cpp.write_text(PREAMBLE + funcion(source, "bool D3D12SharedMemory::ReadbackForCpu(") + CHECK,
                   encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++17", "-fuse-ld=lld", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == "__main__":
    main()
