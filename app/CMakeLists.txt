# ufc3-undisputed-nx - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ufc3-undisputed-nx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
#
# The skeleton: start the recompiled game with ReXGlue's own emulated GPU and nothing else. Everything the
# fork did beyond that (native renderer, audio rescue, cutscenes, settings menu) hooked addresses of the
# previous game and is parked in src/aparcado/ - see its README. Bring a file back only after finding its
# equivalent in THIS game's generated code.
set(UFC3_UNDISPUTED_NX_SOURCES
    src/main.cpp
    # Screenshots of the emulated frame from inside the process, so they work with
    # the window minimised (cvar ufc3_undisputed_nx_captura_cada_s, 0 = never).
    src/ufc3-undisputed-nx_captura.cpp
    # Profile dump of the instrumented build for PGO (UFC3_UNDISPUTED_NX_PGO=generar; empty otherwise).
    src/ufc3-undisputed-nx_pgo.cpp
)

if(WIN32)
    add_executable(ufc3-undisputed-nx WIN32 ${UFC3_UNDISPUTED_NX_SOURCES})
    # PC build: masks again the floating-point exceptions that
    # rex::ppc::FPSCR can leave unmasked on x86 (see the file).
    target_sources(ufc3-undisputed-nx PRIVATE src/ufc3-undisputed-nx_pc_coma_flotante.cpp)
    # Sampling CPU profiler for the PC tests (cvar ufc3-undisputed-nx_perfil_pc_desde_s, 0 = never).
    target_sources(ufc3-undisputed-nx PRIVATE src/ufc3-undisputed-nx_perfil_pc.cpp)
    # A report when the process dies with 0xC0000409 and no message anywhere
    # (UFC3_SIN_DIAGNOSTICO=1 turns it off). See the file.
    target_sources(ufc3-undisputed-nx PRIVATE src/ufc3-undisputed-nx_diagnostico_arranque.cpp)
else()
    add_executable(ufc3-undisputed-nx ${UFC3_UNDISPUTED_NX_SOURCES})
endif()

# GPU_PLUGINS leaves the graphics backend DLL next to the .exe
# (rexgpu-xenosd.dll in Debug, rexgpu-xenos.dll in Release).
#
# Both things are needed to get an image:
#   1. this line, which puts the DLL where the loader looks for it
#   2. gpu_plugin set to "xenos" at run time
#
# The gpu_plugin CVar is empty by default, and empty means "no GPU
# emulation": the game initializes its ring buffer and its graphics callbacks, the
# runtime discards them with "no GPU emulation loaded", and the screen stays
# black even though the game is running perfectly.
#
# Since src/ufc3-undisputed-nx_app.h sets gpu_plugin=xenos by default, the --gpu_plugin
# argument is no longer needed; it can still be passed to change it.
rexglue_setup_target(ufc3-undisputed-nx GPU_PLUGINS xenos)

# The SDK's FFmpeg is NOT linked yet. The fork linked it for its WMV3 cutscene path, which is parked in
# src/aparcado/ and whose FFmpeg configuration is still a WMV3 one. UFC 3 Undisputed's cutscenes are Bink
# (.bik, DAT/bik/*.bik on the disc), so the config needs the Bink decoder and IMA ADPCM first
# (sdk/thirdparty/FFmpeg/config_ufc3_undisputed_nx_wmv3.h). Until then libavcodec would be dead weight.

# PC build with the SDK as a subdirectory (tools/pc/construir_pc.bat):
# rex_app.cpp includes imgui.h and rexruntime links imgui privately, so its
# include path does not reach the executable. On Switch rexglue_switch.cmake handles it, and with
# the SDK installed the headers are already in its include directory (the target does not exist there).
# The same goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes outside
# Switch (ufc3-undisputed-nx_nativo_sistema.cpp uses the SDK's Vulkan device).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(ufc3-undisputed-nx PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(ufc3-undisputed-nx PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Precompiled SPIR-V of the SDK presenter: ufc3-undisputed-nx_nativo_destinos.cpp uses it to draw
# the game image into the presenter output.
target_include_directories(ufc3-undisputed-nx PRIVATE "${REXSDK_DIR}/src/ui/shaders")

# The SDK's xxHash: fingerprints for the shader library and the native renderer.
find_path(UFC3_UNDISPUTED_NX_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(ufc3-undisputed-nx PRIVATE "${UFC3_UNDISPUTED_NX_XXHASH_INCLUDE}")

# UFC3_UNDISPUTED_NX_NATIVE_SHADER_LIBRARY is gone: the whole experimental shader/video library is in
# src/aparcado/. It was already off by default (its synthetic tests never validated the full game), so
# nothing is lost, and the option would only point at files that are not in the build any more.

# Nintendo Switch: ufc3-undisputed-nx.nro next to the ELF. On the SD card it goes in sdmc:/switch/ufc3-undisputed-nx/
# with the game_root/ folder extracted beside it (src/ufc3-undisputed-nx_app.h looks for it there).
if(REXGLUE_PLATFORM_SWITCH)
    # Hot functions grouped at the start of .text. There are 53.5 MB of code and the A57 has 48 KB of
    # instruction cache and a small TLB: hot code scattered across the whole executable forces a page
    # table walk on every far jump. This only changes where each function is placed (-ffunction-sections
    # already gives one section per function). The list comes from stack sampling on the console.
    # LTO on the game code and ours (not on the SDK libraries). Together with tools/llamadas_directas.py
    # (calls to functions without a hook go to __imp__, which is not weak) the compiler can inline small
    # game functions across files. -flto=2 and partitions: the whole-program phase handles 144 MB of
    # generated source; with more processes it does not fit in 16 GB.
    option(UFC3_UNDISPUTED_NX_LTO "LTO en el codigo del juego y de la app (Switch)" ON)
    if(UFC3_UNDISPUTED_NX_LTO)
        foreach(objetivo ufc3-undisputed-nx_recomp ufc3-undisputed-nx)
            if(TARGET ${objetivo})
                target_compile_options(${objetivo} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(ufc3-undisputed-nx PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    # Profile-guided optimization (PGO) in two steps.
    #   generar: instrumented build; the game dumps the profile to sdmc:/switch/ufc3-undisputed-nx/pgo every 3 minutes
    #            (src/ufc3-undisputed-nx_pgo.cpp). GCC embeds paths "<profile folder>\<object with # instead of />.gcda"
    #            in the executable; a --wrap of fopen redirects them to the SD card.
    #   usar:    rebuilds with those .gcda files copied to pgo/<edition> on the PC.
    # -fprofile-prefix-path with the build folder in native format (backslashes), which is how GCC sees it:
    # that way each .gcda name is relative and the same on the console and on the PC.
    # Profile folder: one per edition under pgo/ at the root of the repository (tools/editions/crear_arbol.py
    # points each edition tree to its own).
    get_filename_component(UFC3_UNDISPUTED_NX_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo/pal_es" ABSOLUTE)
    set(UFC3_UNDISPUTED_NX_PGO "" CACHE STRING "Optimizacion por perfil: vacio, generar o usar")
    if(UFC3_UNDISPUTED_NX_PGO)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" UFC3_UNDISPUTED_NX_PGO_PREFIJO)
        if(UFC3_UNDISPUTED_NX_PGO STREQUAL "generar")
            # -fno-profile-values: libgcov's indirect-call profiling reads the TLS with
            # mrs tpidr_el0, which is 0 on Horizon (libnx uses -mtp=soft), so the instrumented
            # build crashed at startup. Without value profiling the branch and function counts
            # remain, and those are what PGO uses.
            set(UFC3_UNDISPUTED_NX_PGO_OPCIONES -fprofile-generate=${UFC3_UNDISPUTED_NX_PGO_DIR} "-fprofile-prefix-path=${UFC3_UNDISPUTED_NX_PGO_PREFIJO}"
                                   -fprofile-update=single -fno-profile-values)
            target_link_options(ufc3-undisputed-nx PRIVATE -fprofile-generate -Wl,--wrap=fopen)
            target_compile_definitions(ufc3-undisputed-nx PRIVATE UFC3_UNDISPUTED_NX_PGO_GENERAR=1 "UFC3_UNDISPUTED_NX_PGO_DIR=\"${UFC3_UNDISPUTED_NX_PGO_DIR}\"")
        elseif(UFC3_UNDISPUTED_NX_PGO STREQUAL "usar")
            set(UFC3_UNDISPUTED_NX_PGO_OPCIONES -fprofile-use=${UFC3_UNDISPUTED_NX_PGO_DIR} "-fprofile-prefix-path=${UFC3_UNDISPUTED_NX_PGO_PREFIJO}"
                                   -fprofile-partial-training -fno-profile-values -fprofile-correction
                                   -Wno-missing-profile
                                   -Wno-error=coverage-mismatch)
            # -fprofile-correction: the instrumentation counts without synchronization (-fprofile-update=single)
            # and files used by several threads at once (the renderer) ended up with negative counts: GCC
            # treated that as a corrupt profile and stopped. This option corrects them.
            target_link_options(ufc3-undisputed-nx PRIVATE -fprofile-use -fprofile-partial-training -fprofile-correction)
        else()
            message(FATAL_ERROR "UFC3_UNDISPUTED_NX_PGO tiene que ser vacio, generar o usar (es '${UFC3_UNDISPUTED_NX_PGO}')")
        endif()
        foreach(objetivo ufc3-undisputed-nx_recomp ufc3-undisputed-nx)
            if(TARGET ${objetivo})
                target_compile_options(${objetivo} PRIVATE ${UFC3_UNDISPUTED_NX_PGO_OPCIONES})
            endif()
        endforeach()
    endif()
    # UFC3_UNDISPUTED_NX_RECOMP_O2: A/B test of the optimization level of the recompiled code (ufc3-undisputed-nx_recomp). -O3 bloats
    # the code (53 MB) and the A57 has 48 KB of instruction cache: -O2 gives smaller code. It comes after the
    # -O3 of CMAKE_CXX_FLAGS_RELEASE, so it takes precedence. With LTO, GCC records each function's level at
    # compile time.
    option(UFC3_UNDISPUTED_NX_RECOMP_O2 "Compilar el codigo traducido con -O2 en vez de -O3" OFF)
    if(UFC3_UNDISPUTED_NX_RECOMP_O2 AND TARGET ufc3-undisputed-nx_recomp)
        target_compile_options(ufc3-undisputed-nx_recomp PRIVATE -O2)
    endif()
    option(UFC3_UNDISPUTED_NX_ORDEN_FUNCIONES "Juntar las funciones calientes al principio de .text" ON)
    if(UFC3_UNDISPUTED_NX_ORDEN_FUNCIONES)
        target_link_options(ufc3-undisputed-nx PRIVATE
            "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/orden_funciones.ld")
        set_property(TARGET ufc3-undisputed-nx APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/orden_funciones.ld")
    endif()
    # Homebrew menu icon: a 256x256 JPEG, as elf2nro expects. It is not distributed with the sources; without
    # tools/switch/icono/ufc3-undisputed-nx_icono.jpg the NRO gets the default libnx icon.
    set(UFC3_UNDISPUTED_NX_ICONO "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icono/ufc3-undisputed-nx_icono.jpg")
    set(UFC3_UNDISPUTED_NX_ICONO_ARG)
    if(EXISTS "${UFC3_UNDISPUTED_NX_ICONO}")
        set(UFC3_UNDISPUTED_NX_ICONO_ARG ICON "${UFC3_UNDISPUTED_NX_ICONO}")
    endif()
    rexglue_switch_add_nro(ufc3-undisputed-nx
        NAME "UFC 3 Undisputed"
        AUTHOR "StevensND"
        VERSION "1.0.0"
        ${UFC3_UNDISPUTED_NX_ICONO_ARG})
endif()

# Native Qt launcher (Linux). Used by packaging/appimage/.
option(UFC3_UNDISPUTED_NX_BUILD_LAUNCHER "Build the native Qt launcher" ON)
option(UFC3_UNDISPUTED_NX_LAUNCHER_BUILD_TESTS "Build Qt launcher tests" OFF)
if(UFC3_UNDISPUTED_NX_BUILD_LAUNCHER AND UNIX AND NOT APPLE)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets QUIET)
    if(Qt6_FOUND)
        if(UFC3_UNDISPUTED_NX_LAUNCHER_BUILD_TESTS)
            enable_testing()
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../launcher-linux"
                         "${CMAKE_CURRENT_BINARY_DIR}/launcher-linux")
    else()
        message(WARNING
            "Qt 6.4+ was not found; ufc3-undisputed-nx_launcher will not be built")
    endif()
endif()


# =============================================================================
#  TARGET "dist" - the portable folder, which is named  build\
#
#  Gathers into a single folder the executable, the project DLLs and the
#  Visual C++ runtime DLLs, so it can be taken to a machine without
#  development tools.
#
#      cmake --build --preset win-amd64-release --target dist
#
#  The target is named "dist" and the folder "build". This is not an oversight:
#  build/ was already in .gitignore and dist/ was not, so putting the folder
#  there (it contains the executable with the game code) keeps it out of git
#  automatically. To put it somewhere else:  -D UFC3_UNDISPUTED_NX_DIST_DIR=...
#
#  Do not confuse it with app/out/build/win-amd64-release, which is where
#  CMake builds. This one is the assembled folder, ready to be copied.
#
#  What it does not do, and this matters:
#  it does not pack the ISO into anything. The ISO stays a separate file that
#  the game reads on the fly, and src/ufc3-undisputed-nx_app.h looks for it next to the .exe.
#  That way the folder works wherever it is, with no fixed path.
#
#  None of this changes how the project is built: it is an extra, separate step.
# =============================================================================
if(WIN32)
    # Resolves the VC runtime DLLs from the toolchain in use.
    # With _SKIP set to TRUE it installs nothing: it only fills in the list, which
    # is all we want. Without this, a path like
    # "C:\Program Files\Microsoft Visual Studio\...\Microsoft.VC143.CRT" would have
    # to be written by hand, which is exactly what must not be here.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
    include(InstallRequiredSystemLibraries)

    # Deliberately not cached. If these were cache variables, changing the default
    # would have no effect in an already configured tree: it would silently keep
    # the old value, the target would report "done" and the folder would
    # appear where it did before. This way the value here always applies, and a -D
    # on the command line still overrides it.
    if(NOT DEFINED UFC3_UNDISPUTED_NX_DIST_NOMBRE)
        set(UFC3_UNDISPUTED_NX_DIST_NOMBRE "UFC3_Undisputed")
    endif()
    if(NOT DEFINED UFC3_UNDISPUTED_NX_DIST_DIR)
        set(UFC3_UNDISPUTED_NX_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build")
    endif()

    add_custom_target(dist
        DEPENDS ufc3-undisputed-nx
        COMMAND ${CMAKE_COMMAND}
                -D "D_EXE=$<TARGET_FILE:ufc3-undisputed-nx>"
                -D "D_BUILD=$<TARGET_FILE_DIR:ufc3-undisputed-nx>"
                -D "D_DIST=${UFC3_UNDISPUTED_NX_DIST_DIR}"
                -D "D_NOMBRE=${UFC3_UNDISPUTED_NX_DIST_NOMBRE}"
                -D "D_VCRUNTIME=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}"
                -D "D_REDIST=$ENV{VCToolsRedistDir}"
                -D "D_PLANTILLA=${CMAKE_CURRENT_SOURCE_DIR}/dist_leeme.txt"
                -D "D_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/ufc3-undisputed-nx.toml"
                -D "D_EDRAM=${CMAKE_CURRENT_SOURCE_DIR}/dist_comparar_edram.bat"
                -D "D_LANZADOR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/lanzador/lanzador.ps1"
                -D "D_LANZADOR_BAT=${CMAKE_CURRENT_SOURCE_DIR}/dist_lanzador.bat"
                -D "D_MATRIZ=${CMAKE_CURRENT_SOURCE_DIR}/../tools/matriz.ps1"
                -D "D_PROBAR=${CMAKE_CURRENT_SOURCE_DIR}/dist_probar.bat"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hacer_dist.cmake"
        COMMENT "Armando la carpeta portable en ${UFC3_UNDISPUTED_NX_DIST_DIR}"
        VERBATIM
    )
endif()
