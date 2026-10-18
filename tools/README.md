# Tools

This folder has the scripts that build the port: they get the missing libraries, extract the game, translate it to
C++, fix up the generated code, check the result and build the NRO. Look here when a step of
[docs/building.md](../docs/building.md) names a script and you want to know what it does. The names are in Spanish;
each one is translated below.

## Main steps

In the order of [docs/building.md](../docs/building.md):

| File | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty`. The repository only keeps the files this port changed. |
| `fase1_extraer.py` ("phase 1: extract") | Extracts the Xbox 360 ISO into `assets/game_root` and prints the [XEX](../docs/glossary.md#xex) information. |
| `codegen.sh` | Runs the code generator (`rexglue codegen`, which translates the game to C++) on an edition tree (`app/`, or the `app_<edition>` copy of another [edition](../docs/glossary.md#edition)), then `llamadas_directas.py` and `copia_literal.py`. |
| `build.ps1` | Configures and builds the [NRO](../docs/glossary.md#nro) of the default tree (`app/`). |

## After every code generation

The build depends on these two tools. `codegen.sh` runs them for you, right after the code generator. If you run the
code generator another way, run them afterwards, in this order.

| File | What it does |
|---|---|
| `llamadas_directas.py` ("direct calls") | Turns calls between recompiled functions into direct calls, except for calls to functions with a [hook](../docs/glossary.md#hook). [LTO](../docs/glossary.md#lto) needs this to optimise across them (see [docs/toolchain.md](../docs/toolchain.md#direct-calls)). |
| `copia_literal.py` ("literal copy") | Writes the literal copies of five game functions. The [guards](../docs/glossary.md#guard) of their [native versions](../docs/glossary.md#native-replacement) compare against these copies. |

## Checks and analysis

| File | What it does |
|---|---|
| `huecos.py`, `huecos_excluir.txt` ("gaps", "gaps to exclude") | Finds code the code generator's analysis left without a function and writes the list to `tools/huecos_functions.toml`. `huecos_excluir.txt` lists the gaps to leave out. |
| `huecos_punteros.py` ("gap pointers") | The one that writes `app/huecos.toml`: scans the guest image for stored code addresses that land in a gap, i.e. the gaps the game can reach through a pointer or a vtable. Those are the ones that stop it with `Call to invalid or unregistered function`, which `huecos.py` alone cannot tell apart from padding. Needs `out/imagen_guest.bin` (see below). |
| `lee_antes.py` ("read before") | Lists, per recompiled function, the non-volatile registers (the ones a function must keep unchanged for its caller) it reads before writing them. See [docs/toolchain.md](../docs/toolchain.md#registers-as-c-locals) for why this matters. |
| `calientes.py` ("hot ones") | Resolves the samples of the console profiler (`rex_perfil.log`) to functions, hottest first. Its output is the input for `app/orden_funciones.ld`, the list of functions placed first in the program ([function ordering](../docs/glossary.md#function-ordering)). |
| `comprobar_indices_texturas.py` ("check texture indices") | Compiles the actual DXBC texture/sampler binding allocators. Verifies dense zero-based descriptor slots, deduplication and bindful behavior; a negative control reproduces the former descriptor-cbuffer overflow. Uses host clang++; no GPU or framework. |
| `comprobar_enteros_texturas.py` ("check integer textures") | Executes the production DXBC conversion emitter against an arithmetic fake. Exhausts R16 values, reproduces BC3 lookup-address aliasing, and checks formats, filtering, signs, masks and constant channels. Uses host clang++; no game data, GPU or framework. |
| `comprobar_resolve_recortado.py` ("check clipped resolve") | Compiles the actual scissor, register layouts, float conversion and resolve rectangle stage. Checks partial tiles with negative X/Y origins, positive copies, window offsets and screen/pitch clipping; a negative control reproduces the lost tail with unsigned origins. Uses host clang++; no game data, GPU or framework. |
| `comprobar_lectura_memoria_gpu.py` ("check GPU-memory file I/O") | Compiles the actual physical-file-read and prepared-write branches. Checks bounded 64 KiB staging, short reads, errors, invalid lengths, preserved GPU neighbours, and no guest/disk writes after provider failure. Uses host clang++; no GPU or framework. |
| `comprobar_coherencia_gpu.py` ("check GPU coherency") | Compiles the full production SharedMemory implementation, PhysicalHeap provider logic and GPU callback queue with small device/memory fakes. Checks CPU reads and partial writes, preserved GPU neighbours, A/C/E aliases, generations, free/reuse, failed delivery, provider lifetime and queue shutdown. Uses host clang++; no GPU or framework. |
| `comprobar_lecturas_repetidas_gpu.py` ("check repeated GPU reads") | Compiles the production D3D12 CPU-provider entry point. Checks concurrent faults sharing one completed page snapshot, 4/16 KB host pages, new generations, failed delivery and multi-page/fragmented controls. Uses host clang++; no GPU, game data or framework. |
| `comprobar_sincronizacion_gpu.py` ("check GPU synchronization") | Compiles the actual D3D12 packet writer and common SHD/CP handlers. Checks immediate CP scratch writeback without losing the CPU VBlank acknowledgement, batched event writes, publication after GPU completion, same-page GPU consumers, unrelated-page progress, no upload/readback for clean packet-word pages, preserved GPU neighbours, failure paths, endian/counter semantics, nonblocking CP polling and fence wakeups through the existing ring event. Page ownership and alias cases are covered by `comprobar_coherencia_gpu.py`. Uses host clang++; no GPU or framework. |
| `comprobar_residencia_vertices.py` ("check vertex residency") | Compiles the actual D3D12 and Vulkan vertex-residency loops with a tiny page-validity fake. Checks same-address CPU writes within/across frames, valid-page upload reuse, and invalid fetch rejection. Uses the existing host clang++ (`--cxx` selects it); no GPU or test framework. |
| `comprobar_texturas_caf.py` ("check CAF textures") | Validates the eight embedded DDS images in a saved `CAFTEX.DAT` without modifying it. `--all-mips` checks every mip independently (32 in the observed layout); `--mip-bands` reports full-width black RGB bands with color in the next mip for inspection; exit 1 reports any entirely transparent-black checked image, including potentially unused atlases. Uses the existing Pillow environment; `--self-test` checks BC3 decoding, lower-mip corruption and truncated input. See [CAF texture recovery](../docs/building.md#caf-texture-recovery). |
| `comprobar_interrupciones_gpu.py` ("check GPU interrupts") | Compiles the actual PM4 interrupt handler and guest dispatcher. Checks asynchronous delivery, coalescing, reentrant queueing, wakeups independent of VBlank and release of the global memory lock. Run with `--cxx path/to/clang++`. |
| `comprobar_clave_textura.py` ("check texture key") | Checks in the built ELF (the program before it is packed as an NRO) that the texture key is written before it is hashed. It catches an LTO/PGO aliasing pitfall (see [docs/toolchain.md](../docs/toolchain.md#an-lto-and-pgo-pitfall-strict-aliasing-in-xxhash)). |

## Folders

| Folder | What it has |
|---|---|
| `switch/cmake/` | The CMake toolchain for devkitA64 and [libnx](../docs/glossary.md#libnx), `switch-devkitA64.cmake`: the file that tells CMake how to compile for the Switch. |
| `editions/` | Support for the other editions: address matching, tree creation, checks, profile translation, per-edition build ([docs/editions.md](../docs/editions.md)). |
