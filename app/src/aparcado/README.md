# src/aparcado - code inherited from nfsmw-nx, parked until UFC 3 Undisputed boots

"aparcado" means "parked". Nothing in this folder is compiled: it is not in `app/CMakeLists.txt`, and
nothing in `app/src/` includes it. It is kept because most of it is the *shape* of the work this game will
need, and because it documents what the previous game's port learned.

**It is not UFC 3 Undisputed code.** Every address in it (`sub_82593A10`, `0x826BE1B0`, …), every hook, every
structure offset and every measured number belongs to Need for Speed: Most Wanted. Not one of them has been
checked against this game's `default.xex`.

Measured: of the addresses that appear in these files, almost none exist in UFC 3 Undisputed's generated
code. The codegen produced 65,886 functions from `0x82000000`–`0x8363F400` (23,330,816 bytes of image), and
the addresses these files hook are not among them. Compiling any of them would either fail to link on
`__imp__sub_XXXXXXXX` or, worse, define a strong symbol for an address the game never calls.

| File | What it did in the fork | What has to happen for UFC 3 |
|---|---|---|
| `ufc3-undisputed-nx_d3d_trace.cpp` | Traced the game's Direct3D calls to `rex_d3d.log`; the base of the native renderer | Re-derive: find UFC 3's D3D device and its draw functions |
| `ufc3-undisputed-nx_nativo_*.cpp` | The whole native renderer (graphics system, draws, textures, pipelines, shaders) | Later; only after the game runs with the emulated GPU |
| `ufc3-undisputed-nx_render_targets.cpp`, `ufc3-undisputed-nx_recortes_carrera.cpp` | Tiling/MSAA and race-scene reductions | UFC 3 has no car cubemap or road reflection; the tiling part still applies |
| `ufc3-undisputed-nx_audio_*.cpp` | Audio server rescue, engine-sound probe, resamplers, filter, sum | The shape applies (XAudio voice ring), the addresses do not |
| `ufc3-undisputed-nx_video_wmv3.cpp`, `ufc3-undisputed-nx_video_nativo.cpp`, `ufc3-undisputed-nx_video_bridge.cpp` | WMV3 cutscenes through the SDK's FFmpeg | **Wrong codec**: UFC 3's cutscenes are Bink (`.bik`, `DAT/bik/*.bik` on the disc) and the FFmpeg config needs the Bink decoder |
| `ufc3-undisputed-nx_video_diag.cpp` | Cutscene frame diagnostics | Same, once the Bink path exists |
| `ufc3-undisputed-nx_escenario_nativo.cpp`, `ufc3-undisputed-nx_material_nativo.cpp`, `ufc3-undisputed-nx_matrices_nativo.cpp`, `ufc3-undisputed-nx_eview_nativo.cpp`, `ufc3-undisputed-nx_pegamento_nativo.cpp`, `ufc3-undisputed-nx_efecto_pasada_nativo.cpp`, `ufc3-undisputed-nx_d3d_registros_nativo.cpp` | Native replacements of hot renderer functions, each checked against a literal copy | All of them are the *other* game's functions |
| `ufc3-undisputed-nx_sombras_lod.cpp`, `ufc3-undisputed-nx_recorte_sombras.cpp`, `ufc3-undisputed-nx_escenario_lod.cpp` | Shadow and LOD reductions | UFC 3-specific; nothing carried over |
| `ufc3-undisputed-nx_streaming_anticipacion.cpp` | Hook on `TrackStreamer::GetPredictedZone` | UFC 3 uses a different streamer |
| `ufc3-undisputed-nx_espera_anillo.cpp`, `ufc3-undisputed-nx_espera_fotograma.cpp` | Blocking waits on the PM4 ring and the frame handoff | The PM4 ring exists on UFC 3 too; addresses and structures are different |
| `ufc3-undisputed-nx_shader_library.cpp`, `ufc3-undisputed-nx_nativo_shaders.cpp`, `ufc3-undisputed-nx_shader_hooks.cpp` | Reads `ufc3-undisputed-nx_shaders.nfsp` and matches shaders to draws | The library format is reusable; UFC 3's shaders are inside `.ff` fastfiles |
| `ufc3-undisputed-nx_entorno_mesa.cpp` | Mesa/NVK environment variables and the libnx nvmap area | Mostly generic; re-enable when there is something to run on the console |
| `ufc3-undisputed-nx_ajustes_graficos.cpp` | The F4 settings menu | Tied to the other game's menu; rewrite from UFC 3's own |
| `ufc3-undisputed-nx_prueba_entrada.cpp` | Virtual gamepad driven by a script, for PC tests | Useful, generic enough to re-enable once there is a build |
| `ufc3-undisputed-nx_crt_nativo.cpp` | Native `memset` of the game's CRT | Re-derive `memset`/`memcpy` addresses from UFC 3 (see `overrides.toml` `[rexcrt]`) |
| `ufc3-undisputed-nx_hilos_switch.cpp` | Thread priority and CPU time on Horizon | Generic; re-enable when something on the console needs it |
| `ufc3-undisputed-nx_microcodigo_versiones.h`, `ufc3-undisputed-nx_esperas_tiron.h`, `ufc3-undisputed-nx_informe_diferido.h`, `ufc3-undisputed-nx_reflejo_demanda.h`, `ufc3-undisputed-nx_nativo_vertices_dedupe.h`, `ufc3-undisputed-nx_audio_nativo.h` | Headers of the above | Same as their .cpp |
| `ufc3-undisputed-nx_video_*`, `ufc3-undisputed-nx_shader_vulkan.*` | The experimental shader/video library, off by default | Later, if ever |

## How to bring one back

1. Find the equivalent function in UFC 3's generated code (`app/generated/default/`), not by address but by
   reading what it does. `tools/lee_antes.py`, `tools/huecos.py` and `tools/calientes.py` are the tools.
2. Declare the address in `app/overrides.toml` with its explanation, one entry at a time.
3. Move the file back into `app/src/`, fix its addresses, add it to `app/CMakeLists.txt`.
4. Run `python tools/llamadas_directas.py` again (it reads `app/src` to decide which functions have hooks).

Do not move a file back "to see if it compiles". A file that compiles but hooks the wrong address is worse
than a file that is missing.
