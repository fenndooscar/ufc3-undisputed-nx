# Profiles for profile guided optimization

**This folder is empty on purpose.** It holds nothing for UFC 3 Undisputed yet.

The fork this port comes from (nfsmw-nx) shipped 1,328 GCC profile files (`.gcda`) here, one folder per
[edition](../docs/glossary.md#edition) of *Need for Speed: Most Wanted*. They were deleted, and they must not be
restored from git history. The reason is in [AGENTS.md](../AGENTS.md) trap 3: a GCC profile matches a function by a
fingerprint of its name, and every recompiled function is named after its
[guest](../docs/glossary.md#guest) address. A profile recorded for another game therefore does not fail — it quietly
optimises for addresses this game does not have. `tools/build.ps1` defaults `-Pgo` to empty for the same reason.

## Recording a profile for this game

Only worth doing once the game runs and there is something to measure:

1. Build with `-Pgo generar` ([app/CMakeLists.txt](../app/CMakeLists.txt)), an instrumented build that counts how
   often each piece of code runs and dumps the counters to `sdmc:/switch/ufc3-undisputed-nx/pgo/` every three minutes
   ([app/src/ufc3-undisputed-nx_pgo.cpp](../app/src/ufc3-undisputed-nx_pgo.cpp)).
2. Copy those `.gcda` files to a folder here, one per edition.
3. Rebuild with `-Pgo usar`.

The folder name is arbitrary, but `app/CMakeLists.txt` defaults to `pgo/pal_es`, so keep that one unless you change the
default too.

## What the files contain

Counters and hashes of function names, nothing from the game. Each file name is the path of an object file relative to
the build folder, with `#` instead of `/`, so GCC finds them as long as the build folder has the same layout.

How to use a profile, and the traps of PGO with GCC on the Switch, are in [docs/building.md](../docs/building.md) and
[docs/toolchain.md](../docs/toolchain.md).
