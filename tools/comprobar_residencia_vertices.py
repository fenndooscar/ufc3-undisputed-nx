"""Check vertex residency (comprobar = check) using both actual backend draw loops.

No GPU or test framework is needed. A tiny SharedMemory fake invalidates a page
without changing its fetch constant; the production C++ loop must request it
again within a frame and after a frame boundary. Valid pages must avoid uploads.
The compiler and generated executables live outside the tracked source tree.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

RAIZ = Path(__file__).resolve().parents[1]

PREAMBULO = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#define REXCVAR_GET(...) false
#define REXGPU_WARN(...) ((void)0)
#define REXGPU_ERROR(...) ((void)0)
namespace rex {
template<class T, size_t N> constexpr size_t countof(const T (&)[N]) { return N; }
bool bit_scan_forward(uint32_t v, uint32_t* bit) {
  if (!v) return false;
  *bit = __builtin_ctz(v); return true;
}
}
namespace xenos {
enum class FetchConstantType { kVertex, kInvalidVertex, kBad };
struct xe_gpu_vertex_fetch_t {
  FetchConstantType type = FetchConstantType::kVertex;
  uint32_t address = 1024, size = 16, dword_0 = 0, dword_1 = 0;
};
}
struct Shader {
  struct ConstantRegisterMap { uint32_t vertex_fetch_bitmap[3] = {1, 0, 0}; } map;
  const ConstantRegisterMap& constant_register_map() const { return map; }
};
struct Registers {
  xenos::xe_gpu_vertex_fetch_t fetch;
  auto GetVertexFetch(uint32_t) const { return fetch; }
};
struct SharedMemory {
  bool valid = false;
  uint32_t cpu = 11, gpu = 0, requests = 0, uploads = 0;
  bool RequestRange(uint32_t address, uint32_t size) {
    assert(address == 4096 && size == 64);
    ++requests;
    if (!valid) { gpu = cpu; ++uploads; valid = true; }
    return true;
  }
};
struct Probe {
  uint32_t vfetch_invalido = 0, vfetch_sin_rango = 0;
  void AnotaVfetchEnExport(const xenos::xe_gpu_vertex_fetch_t&) {}
} g_sonda_dibujos;
struct Harness {
  Registers regs;
  SharedMemory memory;
  SharedMemory* shared_memory_ = &memory;
  // Kept only so reintroducing the old shortcut compiles and fails this check.
  struct VertexBufferState { uint32_t address = UINT32_MAX, size = UINT32_MAX; };
  VertexBufferState vertex_buffer_states_[96]{};
  uint64_t vertex_buffers_in_sync_[2]{};
  bool Draw(const Shader* vertex_shader) {
'''
FINAL = r'''
    return true;
  }
};
int main() {
  Shader shader;
  Harness same_frame;
  assert(same_frame.Draw(&shader) && same_frame.memory.gpu == 11);
  same_frame.memory.cpu = 27; same_frame.memory.valid = false;
  assert(same_frame.Draw(&shader) && same_frame.memory.gpu == 27);
  uint32_t uploads = same_frame.memory.uploads;
  assert(same_frame.Draw(&shader) && same_frame.memory.uploads == uploads);

  Harness next_frame;
  assert(next_frame.Draw(&shader) && next_frame.memory.gpu == 11);
  next_frame.vertex_buffers_in_sync_[0] = 0;
  next_frame.memory.cpu = 41; next_frame.memory.valid = false;
  assert(next_frame.Draw(&shader) && next_frame.memory.gpu == 41);
  next_frame.regs.fetch.type = xenos::FetchConstantType::kBad;
  assert(!next_frame.Draw(&shader));
  puts("same-address CPU writes, frame invalidation, valid-page reuse, invalid fetch: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    destino = RAIZ / "out" / "pruebas_residencia_vertices"
    destino.mkdir(parents=True, exist_ok=True)
    for backend, marca_fin in (("d3d12", "// Gather memexport ranges"),
                              ("vulkan", "// Synchronize the memory pages")):
        ruta = RAIZ / "sdk" / "src" / "graphics" / backend / "command_processor.cpp"
        texto = ruta.read_text(encoding="utf-8")
        inicio = texto.index("  // Ensure vertex buffers are resident.")
        fin = texto.index(marca_fin, inicio)
        fuente = destino / f"{backend}.cpp"
        ejecutable = destino / f"{backend}.exe"
        fuente.write_text(PREAMBULO + texto[inicio:fin] + FINAL, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-fuse-ld=lld", str(fuente),
                        "-o", str(ejecutable)], check=True)
        print(f"{backend}: production vertex-residency loop", flush=True)
        subprocess.run([str(ejecutable)], check=True)


if __name__ == "__main__":
    main()
