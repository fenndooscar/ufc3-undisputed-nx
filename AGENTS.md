# AGENTS.md

How an AI agent should work in this repository. Read [README.md](README.md) first; this file is about *how to act*,
not about what the port is.

## 1. What you are looking at

This is a hard fork of [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) at commit
`4e3ffc24fe80285189f2cea601606c529d5f1b97`, being turned into a Nintendo Switch port of the Xbox 360 version of
**UFC 3 Undisputed** by static recompilation with [ReXGlue](https://github.com/rexglue/rexglue-sdk).

**The game is recompiled now, but the port is not finished.** This game's `default.xex` is extracted, translated
(65,886 functions) and builds and runs as a PC executable; what it does not have is a Switch build, and every part of
the port beyond "start the game" is still missing. The reusable machinery — the Horizon layer in `sdk/`, the Mesa/NVK
patch, the shader tooling, the build scripts — is the previous game's work and is sound. Everything that names an
address, a hook, a function or a measured number is still **Need for Speed: Most Wanted's**, and is wrong for this
game, except for what is listed as re-derived below.

> **The one rule that follows from this:** never treat an address, symbol, hook, offset or measured number found in
> this repository as belonging to UFC 3 Undisputed. Re-derive it from this game's own `default.xex`, and say in your
> answer where the number came from. The inherited values are a checklist of *what to look for*, not answers.

**What IS this game's, and may be treated as true:**

| Where | What | How it was derived |
|---|---|---|
| `app/overrides.toml` | 4 addresses | The first codegen run's own `UnresolvedCall` errors |
| `app/huecos.toml` | 2,757 addresses | `tools/huecos_punteros.py`, from this game's own mapped image |
| `app/generated/` | All of it | `rexglue codegen` on this game's `default.xex` |
| `tools/fase1_extraer.py` | Title ID `5451087D` | The retail ISO's XEX header |
| `app/ufc3_undisputed_nx_manifest.toml` | Image base, size, entry point | Same |

The same applies to `docs/`: the documents were written for the other game. `docs/README.md` says so at the top.

## 2. Hard rules

1. **Never commit game data, or anything derived from it.** No `.xex`, `.iso`, `.ff`, `.bik`, `.nfsp`, no `assets/`,
   no `generated/`, no `app/src/copias_literales/` (exact copies of the game's own functions). `.gitignore` covers all
   of it; if `git status` shows one of those paths, fix the ignore rule — do not commit it.
2. **Never `git add -f`** anything under an ignored path. If a file looks like it should be tracked, check
   `.gitignore` and the reason it is there before overriding it.
3. **`sdk/thirdparty` tracks exactly seven files** — the ones this port changed
   (`CMakeLists.txt`, the three FFmpeg configs, two FFmpeg templates, `ffmpeg-overlay/codec_list.c`). Everything else
   arrives through `tools/fetch_thirdparty.py`. Do not commit the ~19,000 vendored files; `.gitignore` exists
   specifically to prevent it.
4. **Do not add a dependency.** The Switch build is a static NRO with a hard memory limit, and the SDK vendors its
   third-party sources on purpose. If something seems to need a new library, say so and stop.
5. **Respect the licences.** The port is GPL-3.0; the SDK changes are BSD-3-Clause; the shader translator and Mesa
   changes are MIT. Anything copied from another project goes into [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
6. **Do not commit or push unless asked.** The working tree currently holds the whole rename/cleanup pass uncommitted
   on top of `a5adf8a`; treat that as intentional.

## 3. Where things live

| Path | What it is | Can you change it |
|---|---|---|
| `app/` | The game tree: CMake project, codegen manifest, runtime config, hooks, native renderer, audio, video | **Yes — this is where the work is** |
| `app/src/ufc3-undisputed-nx_*.cpp/h` | The port's own code: the app, the PC profiling helpers and the PGO dump | Yes |
| `app/src/aparcado/` | The inherited port's code (native renderer, audio, video, menus). **Not compiled.** It hooks the previous game's addresses | Only to take a file back out, after re-deriving its addresses |
| `app/src/shaders/` | The port's native gamma-ramp shader (`.frag` + generated `.h`) | Yes |
| `app/orden_funciones.ld` | Linker script: function ordering by mangled symbol name | Yes, carefully — see trap 1 |
| `app/ufc3-undisputed-nx.toml` | Runtime settings ([cvars](docs/glossary.md#cvar)), shipped to the SD card | Yes |
| `app/ufc3-undisputed-nx_manifest.toml` | The code generator's manifest | Yes |
| `app/overrides.toml`, `app/huecos.toml` | Codegen declarations | **Only after re-deriving the addresses** — see trap 2 |
| `sdk/` | ReXGlue v0.10.0 plus this port's Horizon layer and codegen changes | Yes, but the Switch layer only; upstream SDK code needs a reason |
| `shaders/` | XenosRecomp plus this port's container reader, packer and WASM builds | Yes |
| `mesa/` | One patch file for mesa-switch and its build script | Yes — keep it a patch, never a full Mesa copy |
| `tools/` | Build and analysis scripts; Spanish names, translated in [tools/README.md](tools/README.md) | Yes |
| `pgo/` | Empty on purpose | See trap 3 |
| `docs/` | English documentation of the inherited port | Yes, and you should |
| `extras/` | Banner image and the SaltyNX overlay template | Rarely |

## 4. Conventions

- **Prose is English.** Docs, comments, commit messages, error strings. The fork's author wrote some identifiers and
  file suffixes in Spanish (`_nativo` "native", `_servidor` "server", `_escenario` "scene", `_ganchos` "hooks",
  `huecos` "gaps"); keep that pattern for new port files rather than mixing in English ones, and translate the name in
  the file's first line.
- **File names** are `ufc3-undisputed-nx_*` with a snake_case suffix saying what the file is. Never `nfsmw_*`, never a bare
  `utils.cpp`.
- **Runtime settings** are cvars: lowercase snake_case, prefixed `ufc3-undisputed-nx_` (`ufc3-undisputed-nx_audio_rescate`, `ufc3-undisputed-nx_nativo_zcull`).
- **Every `app/src` file starts with a header comment** `// ufc3-undisputed-nx - <one line>`. When the file exists to work around a
  specific observed failure, the header then explains, in this order: the failure as observed (with the log line or
  crash), the evidence that identifies the cause, what the file does instead, and **how to turn it off without a
  rebuild** (a cvar). [app/src/ufc3-undisputed-nx_audio_servidor.cpp](app/src/ufc3-undisputed-nx_audio_servidor.cpp) is the reference example.
  Write the same thing for anything you add — the next reader has no console in front of them.
- **Use the project's vocabulary.** [docs/glossary.md](docs/glossary.md) defines every technical word the docs use
  (guest, hook, guard, ring thread, native replacement, PM4 ring, …). Link to it rather than inventing a synonym.
- **Line endings** are LF in the repository (`core.autocrlf=true` normalises on checkout). Do not fight it.
- **Keep the docs true.** If you change a requirement, a build step, a file name or a default, update
  [README.md](README.md), [docs/building.md](docs/building.md) and [tools/README.md](tools/README.md) in the same
  change. Documentation that describes the previous game is a known and marked problem; do not add to it.

## 5. Building and verifying

Verified on Windows with clang 23.1.3 and CMake 4.4.4. Step numbers are [docs/building.md](docs/building.md)'s.

```sh
# 1. Third-party sources. Once; a second run reports "0 files copied" and is harmless.
python tools/fetch_thirdparty.py

# 2. Extract the disc. Needs your own ISO; the Title ID check expects UFC 3 Undisputed (5451087D).
python tools/fase1_extraer.py "path/to/UFC Undisputed 3.iso" -o assets/game_root

# 3. Build the code generator. Needs clang on PATH.
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue -j 12
# The binary is NOT in out/host. It lands in sdk/out/win-amd64/rexglue.exe.

# 4. Translate the game to C++. From app/, or through tools/codegen.sh (Git Bash, not PowerShell: bash on PATH is WSL).
cd app && ../sdk/out/win-amd64/rexglue.exe codegen ufc3_undisputed_nx_manifest.toml

# 5. Build and run it on a PC. This is the step that finds translation problems.
cmake -S app -B app/out/build/win-amd64-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_USE_VULKAN=ON \
  -DREXSDK_DIR="$PWD/sdk"
cmake --build app/out/build/win-amd64-release --target ufc3-undisputed-nx -j 12

# 6. Build the NRO. Needs the Mesa SDK (mesa/README.md). NOT RUN YET.
powershell -ExecutionPolicy Bypass -File tools\build.ps1 `
  -MesaSdk C:\path\to\mesa-sdk\opt\devkitpro\portlibs\switch
```

State of each step on this machine: 1, 2, 3, 4 and 5 verified working. 6 has never been run, because there is no
built Mesa SDK here. `tools/fetch_thirdparty.py` also fails in a confined shell (it cannot `git init`); the same files
can be produced with the helper scripts that were used instead.

`out/host` is only CMake's bookkeeping. `sdk/out/<platform>/` is where the SDK writes its binaries. Both are ignored.

## 6. Traps that fail silently

These are the ones that cost real time. None of them produces an obvious error.

| # | Trap | Symptom | What to do |
|---|---|---|---|
| 1 | **Mangled names in the linker script.** `app/orden_funciones.ld` matches Itanium-ABI symbols by name, and the name carries the identifier's length: `_ZN5ufc3-undisputed-nx` for a 5-character namespace, `_ZN4ufc3-undisputed-nx` for 4. Renaming a namespace or class changes that prefix. | **No build error at all.** The entry stops matching and the function silently loses its place in the binary. | If you rename anything that appears in `orden_funciones.ld`, fix the length prefix and then verify programmatically that every symbol in the file actually exists in the generated code. The fork's own rename had 39 wrong entries. |
| 2 | **Inherited address tables.** `app/overrides.toml` and `app/huecos.toml` were the other game's addresses (35 and 774 entries). Both have been regenerated from THIS game: `overrides.toml` from the codegen's own `UnresolvedCall` errors, `huecos.toml` from `tools/huecos_punteros.py` (2,757 entries). | Declaring an address that is not a function start clips the function that contains it, and its branches to its own tail become unresolvable: `Unresolved conditional branch to 0x... from 0x...`, plus `Function 0x... has no blocks - generating stub`. | Regenerate `huecos.toml` with `tools/huecos_punteros.py` after every codegen that changes which functions exist, and **from a baseline generated without `huecos.toml`** (see `out/rehacer_huecos.ps1`), because the gap measurement reads the generated code. Never hand-edit either file. |
| 3 | **PGO profiles.** `pgo/` is empty on purpose; the fork's 1,328 `.gcda` files were deleted. A GCC profile matches a function by a fingerprint of its name, and every recompiled function is named after its guest address. | `-Dufc3-undisputed-nx_PGO=usar` against an empty folder builds without PGO and says nothing. Restoring the old profiles would "work" and optimise for addresses this game does not have. | Never restore them from git history. `tools/build.ps1` defaults `-Pgo` to empty for this reason. |
| 4 | **SSSE3/SSE4.1 on the host build.** The SDK's x86-64 code uses these intrinsics; clang does not enable them for a generic x86-64 target. | `error: always_inline function '_mm_shuffle_epi8' requires target feature 'ssse3'` | Already handled globally in `sdk/CMakeLists.txt`. Do not add the flag per-file or per-target; the SDK's own libraries are OBJECT libraries and never receive target-level flags. |
| 5 | **Symlinks in `sdk/thirdparty`.** Git on Windows writes a symlink as a small text file containing the target path. | A C compiler tries to compile `../../libmspack/mspack/lzxd.c` as source: `expected identifier or '('` | `tools/fetch_thirdparty.py` replaces them with real copies. If you see this, re-run it rather than patching the file. |
| 6 | **Submodule markers.** `sdk/thirdparty/CMakeLists.txt` refuses to configure unless each vendored library has a `.git` entry. | `CMake Error: Git submodule 'cli11' is not initialized` | `tools/fetch_thirdparty.py` writes the markers. Re-run it. |
| 7 | **`DEVKITPRO` from the registry.** devkitPro's installer leaves `DEVKITPRO=/opt/devkitpro` in the machine environment — the path the MSYS2 shell uses, which does not exist for a native Windows program. | `libnx linker specs not found: /opt/devkitpro/libnx/switch.specs` | `tools/switch/cmake/switch-devkitA64.cmake` already ignores the variable when the path does not exist. Do not delete the variable: devkitPro's own msys2 shell needs it. |
| 8 | **Relative `CMAKE_TOOLCHAIN_FILE`.** CMake resolves it against the *build* directory, not the working directory. | `Could not find toolchain file` | Pass an absolute path. |
| 9 | **`bash` on `PATH` is WSL**, not Git Bash. | `tools/codegen.sh` behaves oddly or cannot see Windows paths | Use `C:\Program Files\Git\bin\bash.exe` explicitly. |
| 10 | **CMake writes the code generator outside `out/host`.** | `REXGLUE=out/host/rexglue` fails: no such file | The binary is `sdk/out/win-amd64/rexglue.exe`. |

## 7. How to work on the port

**Follow the order in [README.md](README.md)'s "Where the port is" table.** It is not a suggestion. The current
position: the game is translated and starts on a PC, and it stops at the first indirect call to an address the analysis
did not open as a function. Keep going down that road — one address at a time, each one traced to this game's own
`default.xex` — and do not write native renderer code, hooks or optimisations before the game runs with ReXGlue's own
emulated GPU. Every problem of the translation is far easier to find there, and an address confirmed by guessing is
worse than no address at all.

`app/src/aparcado/README.md` says which inherited files exist and what each one would need to come back.

The two workstreams that are genuinely new for this game, and where they belong:

- **Bink video.** The cutscenes are `.bik`, not the WMV3 the fork's video path handles
  ([docs/audio-and-video.md](docs/audio-and-video.md), `app/src/ufc3-undisputed-nx_video_wmv3.cpp` and the bridge around it). The
  FFmpeg configuration in `sdk/thirdparty/FFmpeg/config_ufc3-undisputed-nx_wmv3.h` is still a WMV3 one and needs the Bink decoder
  and IMA ADPCM audio.
- **Shaders inside fastfiles.** The shaders are not loose files: they are inside `.ff` fastfiles, which are zlib
  streams (the stream starts at `+0x14` after a 20-byte big-endian header). The container format itself is the one
  `shaders/ufc3-undisputed-nx_contenedor.h` already handles — it accepts the `0x102A0E00`/`0x102A0E01` signature pair, the 2005
  format, which is what this game uses — so the missing piece is unpacking the fastfile before scanning, in
  `shaders/ufc3-undisputed-nx_buscar_contenedores.cpp`.

## 8. Before you report a change as done

- `git status --short` shows **no unexpected untracked files**. `out/`, `assets/`, `generated/` and the vendored
  third-party sources are ignored; a `??` entry should be a file you just created on purpose, and nothing else.
- TOML files still parse:
  `python -c "import tomllib; [tomllib.load(open(p,'rb')) for p in ['app/ufc3-undisputed-nx_manifest.toml','app/ufc3-undisputed-nx.toml','app/overrides.toml','app/huecos.toml']]"`
- If you touched a build file, **run the build step you touched**. Do not claim a build works because it looks right;
  this repository has already had two silent build breakages (traps 4 and 6) that only a real run finds.
- If you touched `app/orden_funciones.ld`, the manifest or `overrides.toml`, check the generated output, not just the
  exit code.
- Non-trivial logic (a branch, a loop, a parser, an address calculation) leaves **one runnable check** behind: a small
  `assert`-based self-check, a `tools/comprobar_*.py`-style script, or a `shaders/ufc3-undisputed-nx_probar_*.cpp`. There is no test
  framework here and none should be added. Trivial one-liners need no test.
- Say explicitly **what you verified and what you did not**. A step that needs the ISO, the console or a built Mesa
  SDK cannot be verified here, and saying "done" about it is worse than saying "not run".

## 9. Ask before doing these

- Committing or pushing.
- Adding a dependency, a test framework or a build system.
- Deleting anything under `docs/`, `tools/editions/` or `extras/`.
- Re-enabling `huecos.toml`, restoring `pgo/`, or re-deriving addresses — these need this game's `default.xex` and a
  decision about scope.
- Any change to the layout the NRO expects on the SD card (`sdmc:/switch/ufc3-undisputed-nx/`), because the installer repository
  has to match it.
