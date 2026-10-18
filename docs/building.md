# Building the port yourself

## CAF texture recovery

Verified on the PC D3D12 build on 2026-10-17: the black CAF's saved `CAFTEX.DAT` contained eight DDS images
that decoded entirely to transparent black. Reloading that save with different graphics settings kept those
images black. An earlier recovery configuration used `readback_resolve = "full"`, synchronously exposing
resolved data to guest CPU consumers at a performance cost. The current configuration disables eager resolve
and memexport readback; the D3D12 1x path delivers data on actual CPU reads, with the source fixes below.

With the current fixed build, back up the CAF and system save data, open the affected fighter in the CAF editor,
change a small tattoo and save. This was verified on existing CAF 1 on 2026-10-18 and regenerates its base albedo;
existing fighters do not need an unavailable skin-tone edit. The earlier full-readback recovery restored eight
black texture images and the fighter's skin and shorts, but colored hair and triangles remained at that stage.
The source fixes and their separate verification are described below.

With the project's Python environment (Pillow already available), check the saved file without modifying it:

```sh
python tools/comprobar_texturas_caf.py --self-test path/to/CAFTEX.DAT
python tools/comprobar_texturas_caf.py --self-test --all-mips --mip-bands path/to/CAFTEX.DAT
python tools/comprobar_resolve_recortado.py --cxx path/to/clang++
```

Exit 1 means at least one checked image is entirely transparent black. `--all-mips` independently decodes every
saved BC3 mip (32 images in the observed eight-atlas layout); the default checks only base images. An unused
atlas may also be empty: this is a pixel-content check, not a verdict about a material. The checker validates
DDS boundaries and reads saved pixels; it does not inspect the GPU resource. Switching a runtime flag alone
cannot repair an already saved black image. `--mip-bands` additionally reports full-width zero-RGB bands
whose corresponding area contains color in the next mip, including opaque black. These reports require
inspection (unused regions may be legitimate) and do not change the exit status.

The current D3D12 path at 1x resolution keeps resolves and memexport on the GPU and supplies dirty physical
pages on CPU demand. Completion writes are ordered after prior GPU work, and publication rechecks each page's
generation before overwriting CPU RAM. Guest interrupt execution must release the global memory lock before
waiting on this service. The compiled checks below exercise those production functions without game data:

```sh
python tools/comprobar_coherencia_gpu.py --cxx path/to/clang++
python tools/comprobar_sincronizacion_gpu.py --cxx path/to/clang++
python tools/comprobar_interrupciones_gpu.py --cxx path/to/clang++
python tools/comprobar_lectura_memoria_gpu.py --cxx path/to/clang++
python tools/comprobar_indices_texturas.py --cxx path/to/clang++
python tools/comprobar_enteros_texturas.py --cxx path/to/clang++
python tools/comprobar_residencia_vertices.py --cxx path/to/clang++
```

For in-game diagnosis, use a separate executable directory with **copies** of the save folders and
`app/ufc3-undisputed-nx.toml` (including the existing title fetch-acceptance setting). Launch with
`--readback_resolve=none --readback_memexport=false --trace_shared_memory_readback=true` to exercise demand
delivery independently of the old eager readback settings. `[GPU-readback]` records the requesting physical
range, the dirty span, batch size and backend success. Tracing is off by default. This is a diagnostic launch,
not a replacement for repairing code or for checking the resulting saved pixels.

The 2026-10-17 CAF test traced a 4 KiB fault at physical `0x1D663000` to a **58,396,672-byte** readback spanning
neighbouring resources (`out/caf_fix_20261017/run1.log`, 22:18:46.870). The range came from the running UFC
build, not the inherited port. Coalescing now extends only across pages from the same GPU write. The production
coherency check preserves one-batch delivery of a 4 MiB write while asserting that an adjacent, separately
written resource remains on the GPU. This bounded-delivery check alone does not validate hair or triangle
rendering; those causes were established separately below.

The separate 22:28 new-CAF run exercised the corrected grouping with eager resolve and memexport readback
disabled: 59 non-completion transfers, largest 5,767,168 bytes, all backend deliveries successful
(`out/caf_fix_20261017/run2.log`). This is evidence of bounded delivery for that generation sequence, not a
frame-rate comparison. In training the original CAF still had blue hair. A subsequent same-binary RTV/ROV comparison
reproduced colored hair in both paths. The first gray-resource/18-constant capture belonged to PS
`EF0A43A22C138715`; a material-tag control did **not** change the visible CAF hair, so that capture cannot clear
the visible hair's inputs. The tag frame and the actual PM4 texture packet instead identify PS
`2C56B11C103C5673`, VS `0E16AF2E3316A93C`, and a 1024x512 BC3 fetch at physical `0x18366000` in this run.
The actual GPU resource and the shader's sampled albedo both decode green; all ten uploaded pixel constants
match the 160 source bytes. On 2026-10-18, the loader trace and live resource references identify its owner as
`hair_d`. PS `F9AF3672BD057603` writes the same physical destination through memexport while compressing RGBA
to BC3. Its seven uploaded constants match all 112 source bytes. A host capture of its input `tf5` contains
the recognizable hair image (BGRA host swizzle), while its immediate BC3 output is green and scrambled.

This compressor samples a 32x32 `k_16` address table at `tf15` with `num_format=1`. The captured table holds
integers 0 through 1023. The D3D12 path previously ignored the integer flag and returned R16_UNORM values,
so `8388608 + 2 * lookup` collapsed all 1024 entries to one float mantissa/index. The DXBC fetch translator
now restores unsigned fixed-point 16-bit integer ranges before bias/exponent adjustment, preserves constant
0/1 channels, and rounds explicitly point-sampled integer values. Normalized and float formats are excluded.
`comprobar_enteros_texturas.py` exercises the production emitter on all 65536 R16 values and reproduces
the old one-address/new 1024-address result. This scoped conversion does not yet support other integer widths
or signed integer texture fetches; the general Vulkan path is unchanged.

The corrected `run_integer16` build (GPU DLL SHA256
`919a72a970fe8eb1862f361c3fa5a16a3f82ec64a5960696ecf6d71c83d62776`) was checked in the same training scene:
the final hair BC3 now preserves the recognizable hair image and brown color. Its unpadded base hash is
`b4ae3f8588ccb97ad03818b7497757205b6542fd884def64c0b0f4d8a9719c2a`.
Both eager readback flags were disabled. The user still observes intermittent black triangles after this
fix and after changing/saving a tattoo in the current CAF copy. The regenerated `CAFTEX.DAT` hash is
`951855f5a3ae2563293d0c02aed8c5066f6ebde5f8d10d1274deb6545b6804e8`; all 32 mip images decode and contain
nonzero data. Its body albedo changed while its normal atlas remained byte-identical. This does not establish
the cause of the remaining triangle artifacts.

A subsequent controlled torso probe identifies PS `897408EB8A94D3C6` and VS `7CE9995F7CDE736D`;
PS `C8F8C8858B1E1E5E` shares atlas resources but does not cover the main torso. All 24 uploaded constants
of the actual torso PS match their 384 source bytes. On the back-facing pose, direct sampled albedo
still shows the triangles: a smooth front-facing capture had not exercised the damaged area. The actual
GPU atlas base contains a full-width black band at rows 1024 through 1279 (two rows contain tattoo data
after the edit), while mips 1 through 3 have skin data there. The same band is present in saved atlas 0;
changing the tattoo did not rebuild that whole region. `--mip-bands` reproduces this content mismatch
without the game. Native frames and independent GPU mip decoding are saved under ignored
`out/caf_fix_20261017/skin3_mask_results/` and `skin3_albedo/`. The shadow-only hypothesis was not
confirmed; intermediate specular coefficients also depend on albedo alpha. A separate CAF 2 was then created with a changed skin tone and saved using the integer conversion fix
and both eager readback modes disabled. Its `CAFTEX.DAT` hash is
`d651cdf326b5765a4291dec0052bb75dfe6712e26fc4e91e8f7a49adf048e6c0`; atlas 0 still has black bands at
rows 1024-1039 and 1088-1279, and the native training frame still shows triangles. The initial per-resource
GPU captures occurred before the final save and must not be treated as exact copies of this final file.
The generation census identifies compositor VS `4A71D4449B655328` and its actual source parts, including
1152x576 BC3 and 2048x768 RGBA textures. A third newly created CAF reproduces atlas 0's full-width
black rows 1024-1279 (`CAFTEX.DAT` SHA256
`be9562da32c9d94df28ae5c8b1a2e524fbeb671e26d19c2179839fc078a31838`). All 67 save files were copied
and verified before further investigation.

A bounded compositor capture localizes the loss before compression. The 1152x576 torso input to
PS `10CBAC43A9254941` and its separate RGBA resolve are populated. PS `AEF9BCF807300534` copies that
RGBA resource into the large atlas at x=448, y=704. Its first 320 rows match the input byte for byte
(after the fetch's R/B swizzle); the remaining 256 rows are black in the resolve, beginning at atlas
Y=1024. Both arm copies lose the same tail. The draw snapshot's window scissor is 2048x1024, while
the subsequent resolve's scissor covers the complete part through Y=1280. This is evidence of a lost
composition region, not yet proof of why the draw is clipped or whether another tile should continue it.
The ordered packet trace captures the missing continuation: packet 137 draws the same torso shifted up
by 1024 pixels, from Y=-320 to 256, into the next tile. Packet 138 resolves it to the second atlas block.
Its window TL encodes -320 modulo 14 bits as `0x3EC0`. The existing unsigned interpretation turns that
into 16064 and collapses the copy to an empty region. Equivalent arm continuations fail the same way.
All captured packet predicates pass; the corresponding draws are emitted with the expected vertices.

The source fix decodes signed window origins for resolve rectangle clipping, before adding window
offsets and intersecting with the screen. Ordinary draw scissors retain their existing interpretation;
BR=8192 remains a valid exclusive limit. `comprobar_resolve_recortado.py` executes the production
rectangle stage and reproduces the lost 256-row tail without game data, alongside X-axis, positive-copy,
offset and clipping controls. The fixed PC binary builds. Changing a tattoo and saving existing CAF 1 regenerates
its complete base albedo without eager resolve or memexport readback. Its saved `CAFTEX.DAT` SHA256 is
`1144bd98f969a82c221508499315dc57d0cb82b5d15ea7202a7a9d30fcd34dff`; all 32 mip images decode, and all
524,288 pixels in the formerly black 256-row band now contain color. Targeted GPU captures confirm populated
1152x256 torso and 448x256 arm continuation resolves. A verified 67-file save backup preserves this result.
In training, native frames show the repaired CAF from the front, back and on the ground with normal hair color
and continuous skin. The user confirmed that triangles no longer appear during movement, turning, strikes and
dodges. A control regeneration with resource captures and draw reports disabled produced `CAFTEX.DAT` SHA256
`c344ba8d1348e162c9751f13d9e4f0809f121b2a245c9a7a13a55acd0cb6faa9`, all 32 mip images decode, and
base albedo has no full-width black band. The resource-capture count and draw-trace length remained unchanged;
the last probe log predates this save. However, a later performance investigation found that setting
`ufc3_undisputed_nx_sonda_dibujos=0` had stopped reports without stopping resource reads and vertex interpretation.
This earlier control did not prove independence from the heavy collectors. They now return early when disabled.
A final CAF 1 tattoo edit/save with these guards active regenerated `CAFTEX.DAT` SHA256
`f9705fdedb1c84c56eafab90659504f7d39bc7050016cfd4c5a5189e2ec45a7a`: all 32 mip images decode,
and atlas 0 has no full-width black band. Native frames retain normal hair and continuous skin, and the user
confirmed restored speed and no black triangles while moving, turning and striking. The 15:09-15:10 CPU-demand trace records no failed deliveries and a largest batch of 5,767,168 bytes.
A final 109-file backup preserves the save folders; the clean production package includes these saves and no
temporary atlas packet/resource probe code. The original backup's 61 files and 58 original save/config files
were verified unchanged. Previously damaged CAF 2 and 3 remain preserved; each needs its own editor regeneration
if used with the corrected build.
Temporary probes and decoded evidence remain under ignored `out/caf_fix_20261017/`; production source
does not widen guest draw scissors or special-case these hashes.

Training now runs at nominal 60 FPS with the corrected CAF paths. Read-only guest-swap counters measured
59.97 FPS (900 swaps / 15.006 s) before the final guarded regeneration and 60.04 FPS
(901 / 15.006 s) afterwards. Both measurement windows logged zero CPU-demand GPU readbacks, and the user
confirmed that the previous speed returned. The earlier collector-enabled run measured 16.4 FPS. These are
PC D3D12 1x measurements in this training scene, not a Switch or whole-game performance claim.

The slowdown had several causes in command synchronization. Disabled draw reports still read GPU resources
and interpreted vertices; six collector entry points now return immediately when disabled. Scalar CP values
were uploaded to the GPU and read back as whole pages. This UFC run's `PS_DONE_TS` packets write
`0x13D3E000`/`0x13D3E004`, alongside ring read-pointer writeback at `0x13D3E03C`.
Known event values are now queued on the CPU with the current submission, without closing the batch for each
word. They are published only after its fence completes. A GPU fetch of the same host page first completes
pending words before uploading that page; unrelated ranges proceed independently. Actual shader-generated
neighbours still use the memory provider. A bounded single-page snapshot can be reused only while its page
generation is unchanged; writes and free/reuse invalidate it.

Scratch writeback and shader completion have different timing. The actual callback `0x8236EF00` calls
`0x8237B5B0` on VBlank, clearing scratch word `0x13D3D004`. Deferring the CP's scratch write of 1 until a GPU
fence missed that VBlank, then overwrote the CPU acknowledgement, forcing another display interval. Scratch
writeback now reports CP progress immediately through the protected CPU alias. `PS_DONE_TS` remains
completion-ordered. Command IRQs wake the existing VSync worker independently of VBlank and stay serialized
there, avoiding synchronous guest ISR execution on the GPU thread. `WAIT_REG_MEM` services CPU providers and
polls cooperatively without a display-dependent millisecond sleep.

Finally, polling a CPU handshake must not block the command processor on all outstanding GPU notifications.
The D3D12 wait hook submits pending work and polls completed fences; it arms the existing ring event to wake
an idle worker when a word is ready. Idle polling publishes completed words even without new ring data, with
one wake registration per submission and a blocking fallback on registration failure. Shutdown and dependent
GPU fetches still require completion. No additional worker thread or performance workaround cvar is used.

Runnable checks with the existing host compiler:

```sh
python tools/comprobar_enteros_texturas.py --cxx path/to/clang++
python tools/comprobar_resolve_recortado.py --cxx path/to/clang++
python tools/comprobar_coherencia_gpu.py --cxx path/to/clang++
python tools/comprobar_sincronizacion_gpu.py --cxx path/to/clang++
python tools/comprobar_interrupciones_gpu.py --cxx path/to/clang++
python tools/comprobar_lecturas_repetidas_gpu.py --cxx path/to/clang++
```

The synchronization check compiles the production scratch branch, PM4 handlers and D3D12 publication paths.
It verifies first-VBlank acknowledgement preservation, no early shader-completion publication, grouped words,
same-page GPU ordering, unrelated-page progress, idle/short/long waits, fence wakeups and failure paths. The
coherency check covers adjacent resources, aliases, newer writes, free/reuse and provider lifetime. The original
save/config files remain unchanged; CAF 1, the validated binary and separate save/package backups are preserved
under ignored `out/caf_fix_20261017/`. The test process remains running; it was not stopped for packaging.

These addresses/hashes come from this UFC run, not the inherited port. Captures are local and ignored;
no game-derived binary belongs in a commit. The runnable checks reproduce both regressions without captures.

The remaining build instructions below describe inherited machinery; see [README.md](README.md).

## In short

Players do not need this page: the [installer page](https://stevensnd.github.io/ufc3-undisputed-nx-installer/) makes the package
from their own copy of the game, with the released NROs. This page is for building the NRO (the Switch program) and
the shader library yourself, from the source code in this repository.

The steps, in order: get the missing libraries, extract the game, build the graphics driver, build the code
generator, translate the game to C++, build the NRO, build the shader library, and copy everything to the SD card.
Each step says what to do and why. If a word is new to you, it is in the [glossary](glossary.md).

Everything was done on Windows 10/11 (64-bit). The scripts are PowerShell, bash (Git Bash or MSYS2) and Python. Only
the driver build is tied to Windows; other systems should work, but nobody has tried them.

## What you need

- [devkitPro](https://devkitpro.org/wiki/Getting_Started), with devkitA64 (the compiler for the Switch) and libnx (the
  Switch library). It installs to `C:/devkitPro`; if you put it somewhere else, set the `DEVKITPRO` environment
  variable to that folder.
- CMake 3.25 or newer, Ninja, Git, and Python 3.10 or newer.
- A C++ compiler for your PC, to build the code generator. Clang 20 or newer was used.
- Your own copy of Need for Speed: Most Wanted for Xbox 360 (an ISO or the extracted files). Nothing from the game is
  in this repository, and nothing made from it may be added to it.
- About 16 GB of RAM. The translated game code is big (about 144 MB of C++), and the final optimization step
  ([LTO](glossary.md#lto)) alone takes about ten minutes.

Only if you need them:

- MSYS2 and Rust, to build the graphics driver yourself (see [mesa/README.md](../mesa/README.md)).
- The Vulkan SDK (for its DXC and spirv-val tools) and MinGW g++, to build the shader library on your PC instead of in
  the installer page.
- Emscripten, to rebuild the tools that the installer page runs in the browser (`shaders/wasm/`).

## 1. Get the missing libraries

**Why:** to keep the repository small, `sdk/thirdparty` only has the files this port changed. The rest comes from the
[ReXGlue](glossary.md#rexglue) version this port is based on (v0.10.0).

```sh
python tools/fetch_thirdparty.py
```

It downloads the rest, and it never overwrites a file that is already there, so the port's changes stay.

## 2. Extract the game

**Why:** the code generator needs the game's program, `default.xex`, and the port needs the game files.

```sh
python tools/fase1_extraer.py path/to/UFC3_UNDISPUTED_NX.iso -o assets/game_root
```

This extracts the disc into `assets/game_root` ("fase1_extraer" means "phase 1: extract"). Git ignores the `assets/`
folder, so the game never ends up in the repository.

Each [edition](glossary.md#edition) of the game has a different `default.xex` and needs its own build. The main
folder, `app/`, is set up for the PAL Spanish edition; [editions.md](editions.md) explains the others.

## 3. Build the graphics driver

**Why:** the NRO carries its own Vulkan driver, [NVK](glossary.md#nvk), with this port's changes.

Follow [mesa/README.md](../mesa/README.md): build [mesa-switch](https://github.com/danfromtico/mesa-switch) at commit
`1a8c1a66d6f` with `mesa/mesa-switch-ufc3-undisputed-nx.patch` applied. The result is an SDK folder. The app needs its subfolder
`opt/devkitpro/portlibs/switch`, which contains `lib/libvulkan.a`.

## 4. Build the code generator

**Why:** the code generator (`rexglue`) is the program that translates the game to C++. It runs on your PC, so you
build it with your PC's compiler, not with the Switch one.

```sh
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue
```

This version has a change that makes the translated code faster: the Xbox 360 processor's registers live in normal C++
variables, shared by the pieces of a split function (`share_registers`, see [toolchain.md](toolchain.md)).

## 5. Translate the game to C++

```sh
REXGLUE=out/host/rexglue tools/codegen.sh app
```

This does three things:

1. It runs the code generator with the settings in `app/ufc3_undisputed_nx_manifest.toml`. The C++ goes to `app/generated/default`,
   which git ignores because it is made from the game.
2. It runs `tools/llamadas_directas.py` ("direct calls"). It turns calls between translated functions into direct C++
   calls, so that LTO can optimize across files.
3. It runs `tools/copia_literal.py` ("literal copy"). It writes `app/src/copias_literales/`: exact copies of five game
   functions. The [guards](glossary.md#guard) of their native replacements compare against them (see
   [native-renderer.md](native-renderer.md)).

## 6. Build the NRO

From PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -MesaSdk C:\path\to\mesa-sdk\opt\devkitpro\portlibs\switch
```

Or by hand:

```sh
cmake -S app -B app/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk \
  -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch \
  -DUFC3_UNDISPUTED_NX_PGO=usar -DUFC3_UNDISPUTED_NX_BUILD_LAUNCHER=OFF
cmake --build app/out/sw8 -j 4
```

The result is `app/out/sw8/ufc3-undisputed-nx.nro`. Rename it to `ufc3-undisputed-nx.nro` when you copy it to the SD card (step 8).

Build options, in `app/CMakeLists.txt`:

| Option | Normally | What it does |
|---|---|---|
| `UFC3_UNDISPUTED_NX_LTO` | ON | Optimizes the translated code and the app as a whole at the end of the build ([LTO](glossary.md#lto)). It uses 2 jobs (`-flto=2`), because more do not fit in 16 GB of RAM. |
| `UFC3_UNDISPUTED_NX_PGO` | empty | `usar` ("use") builds with the recorded profile in `pgo/pal_es`. `generar` ("generate") builds a special NRO that records a new profile. |
| `UFC3_UNDISPUTED_NX_ORDEN_FUNCIONES` | ON | Puts the functions that run most at the start of the program (list in `app/orden_funciones.ld`), which makes them faster to fetch. |

The icon for the Homebrew Menu is not included. Put a 256x256 JPEG at `tools/switch/icono/ufc3-undisputed-nx_icono.jpg`, or the
NRO gets libnx's default icon.

### The PGO profile

[PGO](glossary.md#pgo) uses a record of which code runs most. `pgo/<edition>/` has the profile the released NROs were
built with. It was recorded on the console while playing (a small test, not a full playthrough), and then translated to
each edition with `tools/editions/pgo/traducir_perfil.py`.

**Why the folder paths matter:** GCC finds each function in the profile by a fingerprint of its name. For functions
that only exist inside one file (`static` functions, or those in anonymous namespaces), the fingerprint also includes
the path of that file. So those functions only match if you build from the same folder paths the profile was recorded
with. Otherwise GCC builds them without the profile: a small loss, not an error.

To record a new profile:

1. Configure with `-DUFC3_UNDISPUTED_NX_PGO=generar` and build.
2. Play. The NRO writes its counters to `sdmc:/switch/ufc3-undisputed-nx/pgo/` every three minutes.
3. Copy the `.gcda` files into `pgo/pal_es/`.
4. Build again with `-DUFC3_UNDISPUTED_NX_PGO=usar`.

[toolchain.md](toolchain.md) has the details and the traps.

## 7. Build the shader library

The game's [shaders](glossary.md#shader) are Xbox 360 [microcode](glossary.md#microcode) inside the disc files.
`ufc3-undisputed-nx_shaders.nfsp` has them translated to [SPIR-V](glossary.md#spir-v) (see [shaders.md](shaders.md)). The
installer page builds it in the browser from the disc; you only need this step to build it on your PC.

```sh
MESA=/path/to/mesa-switch shaders/ufc3-undisputed-nx_regenerar_biblioteca_pcf.sh out/library /path/to/extracted/containers
```

The containers (the files that hold the shaders) are taken out of the disc files by
`shaders/ufc3-undisputed-nx_buscar_contenedores.cpp` ("find containers").

## 8. Copy it to the SD card

The NRO expects this layout, the same one the installer page makes:

```
sdmc:/switch/ufc3-undisputed-nx/
    ufc3-undisputed-nx.nro
    ufc3-undisputed-nx.toml
    ufc3-undisputed-nx_shaders.nfsp
    game_root/          the files of the disc
```

`ufc3-undisputed-nx.toml` has the settings read at startup ([cvars](glossary.md#cvar)). The one the installer page gives players is
`release/ufc3-undisputed-nx.toml` in the [installer's repository](https://github.com/StevensND/ufc3-undisputed-nx-installer).

Start the NRO from the Homebrew Menu in [title takeover](glossary.md#title-takeover) mode (hold R while you start a
game), or with a [forwarder](glossary.md#forwarder). Started from the album, it does not get enough memory.

## Other editions

Every `default.xex` is a different program, so every edition needs its own build:

- `tools/editions/crear_arbol.py` ("create tree") makes a copy of the app, `app_<edition>`, with all of our addresses
  translated to that edition.
- `tools/editions/build_edition.ps1` builds it from the same folder paths as the main one, so the PGO profile matches.

[editions.md](editions.md) explains it step by step.
