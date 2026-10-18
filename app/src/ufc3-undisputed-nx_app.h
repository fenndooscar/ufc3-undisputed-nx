// ufc3-undisputed-nx - the app itself: where the game data is, which settings are mandatory, and the hang
// watchdog.
//
// This is the *skeleton* app: it starts the recompiled game with ReXGlue's own emulated GPU and does nothing
// else. It has no hooks, no native renderer, no audio rescue and no cutscene path, because none of those
// exist for UFC 3 Undisputed yet. The inherited ones were parked in src/aparcado/ (see its README).
//
// The order of the work is in README.md, "Not done": first the game has to run on a PC with the emulated
// GPU, because every translation problem is far easier to find there. Only then does it make sense to
// replace any of it with native code.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "ufc3-undisputed-nx_captura.h"
#include "ufc3-undisputed-nx_perfil_pc.h"

class Ufc3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ufc3App>(new Ufc3App(ctx, "ufc3-undisputed-nx",
        PPCImageConfig));
  }

 protected:
  // ==========================================================================
  //  1. WHERE THE GAME DATA IS
  //
  //  Without this, starting without --game_data_root dies with
  //      "--game_data_root was not provided."
  //  because SetupEnvironment only looks at the cvar and, if it is empty,
  //  ConstructRuntime aborts.
  //
  //  OnConfigurePaths runs right after the PathConfig is built and before
  //  anyone uses it, which is the place to fill the gap. ORDER MATTERS: this
  //  runs *before* ufc3-undisputed-nx.toml is read (SetupEnvironment reads it a few lines
  //  further down), so the priority is: --game_data_root on the command line,
  //  and otherwise whatever is found here. Setting game_data_root in the toml
  //  does not work, and that is not our doing: it is how the SDK is ordered.
  //
  //  It searches, in this order:
  //    1. an .iso whose name matches the executable's
  //    2. any other .iso in the folder, in alphabetical order
  //    3. a game_root\ folder, in case someone prefers to extract it
  //
  //  (1) exists so that a folder with ufc3-undisputed-nx.exe and a matching .iso
  //  works unambiguously even if there are more images.
  // ==========================================================================
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ElegirCarpetaDeGuardados();
    ElegirCarpetaDeEscritura(paths);
    if (!paths.game_data_root.empty()) {
      return;  // given on the command line; it takes precedence.
    }

    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }

    std::filesystem::path preferida;
    std::vector<std::filesystem::path> otras;

    std::string yo;
    {
      const auto exe = rex::filesystem::GetExecutablePath();
      if (!exe.empty()) {
        yo = exe.stem().string();
        std::transform(yo.begin(), yo.end(), yo.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
      }
    }

    for (const auto& e : std::filesystem::directory_iterator(carpeta, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec)) continue;

      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (ext != ".iso") continue;

      std::string base = e.path().stem().string();
      std::transform(base.begin(), base.end(), base.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });

      if (!yo.empty() && base == yo) {
        preferida = e.path();
      } else {
        otras.push_back(e.path());
      }
    }

    if (!preferida.empty()) {
      paths.game_data_root = preferida;
    } else if (!otras.empty()) {
      std::sort(otras.begin(), otras.end());
      paths.game_data_root = otras.front();
    } else {
      // No ISO: an extracted folder next to it also works.
      const auto extraida = carpeta / "game_root";
      if (std::filesystem::is_directory(extraida, ec)) {
        paths.game_data_root = extraida;
      }
    }
    // If nothing is found it is left empty on purpose: the SDK gives its own
    // message, which is clearer than anything we could put here.
  }

  // ==========================================================================
  //  2. WHERE THE PORT WRITES
  //
  //  THE FAILURE, AS OBSERVED
  //  The PC build started, loaded the game, initialized the GPU and then
  //  disappeared with exit code 0xE06D7363 and nothing in the log. The
  //  diagnostic in src/ufc3-undisputed-nx_diagnostico_arranque.cpp caught it:
  //
  //      tipo lanzado: .?AVfilesystem_error@filesystem@std@@
  //      create_directories: Access is denied.:
  //          "C:\Users\igor2\Documents\ufc3-undisputed-nx\cache\shaders\shareable"
  //
  //  and the line that survives in the log is its harmless half:
  //
  //      [error] [gpu] Failed to open the Direct3D 12 pipeline description storage
  //      file for writing, persistent shader storage will be disabled:
  //      ...\Documents\ufc3-undisputed-nx\cache\shaders\shareable\5451087D.rtv.d3d12.xpso
  //
  //  WHAT IDENTIFIES THE CAUSE
  //  The path. Nobody chose it: the SDK's default is Documents/<app name>
  //  (src/ui/rex_app.cpp, "User data: cvar override, or platform user directory"),
  //  and Documents is exactly the folder Windows Defender's controlled folder
  //  access protects. A directory the user creates there from a shell is allowed;
  //  the same directory created by an unknown executable is refused, and
  //  std::filesystem::create_directories turns that refusal into a thrown
  //  filesystem_error that nobody catches (src/graphics/vulkan/pipeline_cache.cpp).
  //
  //  WHAT THIS DOES INSTEAD
  //  Puts the user data and the shader cache next to the executable, which is
  //  where everything else in this port already lives: on the Switch that is
  //  sdmc:/switch/ufc3-undisputed-nx/, so the layout is the same on both.
  //
  //  HOW TO TURN IT OFF
  //  --user_data_root=<path> or --cache_root=<path> on the command line: both are
  //  read before this runs, and an explicit value is left alone. The .toml cannot
  //  do it, because it is read after the paths are already built.
  //
  //  The guard asks the cvar, not the path. By the time this runs the SDK has
  //  already filled cache_root with its own default (Documents/<app>/cache), so
  //  "the path is not empty" is true even when nobody asked for anything - which
  //  is how the first version of this function silently did nothing.
  // ==========================================================================
  void ElegirCarpetaDeEscritura(rex::PathConfig& paths) {
    if (rex::cvar::HasNonDefaultValue("user_data_root") ||
        rex::cvar::HasNonDefaultValue("cache_root")) {
      return;
    }
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty()) {
      return;
    }
    paths.user_data_root = carpeta;
    paths.cache_root = carpeta / "cache";
  }

  // ==========================================================================
  //  3. COPY OF THE SAVES, NEXT TO THE EXECUTABLE
  //
  //  The game's saves live where the runtime puts them, which is an opaque
  //  place: <data>/<xuid>/<title>/<type>/<name>. The player cannot see them and
  //  there is no convenient way to copy them or recover a corrupted one.
  //
  //  With content_backup_root the runtime also leaves a copy sorted by profile
  //  when each save is closed:
  //      saves/<profile>/actual      the latest save
  //      saves/<profile>/anterior    the previous one, in case the latest gets corrupted
  //
  //  On the Switch the NRO is in sdmc:/switch/ufc3-undisputed-nx/, so this gives
  //  sdmc:/switch/ufc3-undisputed-nx/saves. On PC, next to the .exe, like everything else.
  //
  //  ORDER: this runs before ufc3-undisputed-nx.toml is read, so the toml can
  //  change the folder or leave it empty to copy nothing.
  // ==========================================================================
  void ElegirCarpetaDeGuardados() {
    if (rex::cvar::GetFlagInfo("content_backup_root") == nullptr) {
      return;  // SDK without that option: nothing happens
    }
    if (!rex::cvar::GetFlagByName("content_backup_root").empty()) {
      return;  // given on the command line
    }
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty()) {
      return;
    }
    const auto destino = carpeta / "saves";
    if (!rex::cvar::SetFlagByName("content_backup_root", rex::path_to_utf8(destino))) {
      REXLOG_WARN("[guardado] no se pudo fijar la carpeta de copias en {}",
                  rex::path_to_utf8(destino));
    }
  }

  // ==========================================================================
  //  4. MANDATORY SETTINGS
  //
  //  So that a plain "ufc3-undisputed-nx.exe", without a single argument, starts
  //  just as well as with the usual long command line.
  //
  //  Only the settings not set explicitly are touched: HasNonDefaultValue tells
  //  "this is the factory default" from "someone asked for this". That way the
  //  command line and ufc3-undisputed-nx.toml still take precedence.
  // ==========================================================================
  void OnPostInitLogging() override {
    // Without a GPU plugin the screen stays black: the game runs, but the
    // runtime discards its graphics calls with "no GPU emulation loaded".
    PonerSiNadieLoPidio("gpu_plugin", "xenos");
#if !REX_PLATFORM_SWITCH
    // Keyboard and mouse in addition to the gamepad. The Switch has neither:
    // the synthetic device would only shadow the libnx controllers.
    PonerSiNadieLoPidio("mnk_mode", "true");
#endif
  }

  void OnPostSetup() override {
    ArrancarVigilante();
    // Sampling CPU profiler, PC only (ufc3_undisputed_nx_perfil_pc_desde_s; off by default).
    ufc3::perfil_pc::Arrancar();
  }

  // The runtime is alive and the window exists from here on, which is the first
  // moment the graphics system has a presenter to ask for frames.
  void OnPostLoadXexImage() override {
    // Screenshots from inside the process (ufc3_undisputed_nx_captura_cada_s, 0 = never).
    ufc3::captura::Arrancar(runtime()->graphics_system());
  }

  // TEMPORARY. Dumps the guest image so the code pointers inside it can be
  // scanned offline. Not committed, removed as soon as the list of gap
  // functions is settled.
  void OnPreLaunchModule() override {
    const char* destino = std::getenv("UFC3_VOLCAR_IMAGEN");
    if (destino == nullptr || *destino == '\0') {
      return;
    }
    auto* memoria = rex::system::kernel_state()->memory();
    if (memoria == nullptr) {
      return;
    }
    std::FILE* f = std::fopen(destino, "wb");
    if (f == nullptr) {
      REXLOG_ERROR("[volcado] no se pudo abrir {}", destino);
      return;
    }
    // The image the XEX declares: base 0x82000000, 23,330,816 bytes.
    constexpr uint32_t kBaseImagen = 0x82000000;
    constexpr size_t kTamImagen = 23330816;
    std::fwrite(memoria->virtual_membase() + kBaseImagen, 1, kTamImagen, f);
    std::fclose(f);
    REXLOG_INFO("[volcado] imagen del juego escrita en {}", destino);
  }

  void OnShutdown() override {
    ufc3::captura::Parar();
    ufc3::perfil_pc::Parar();
    PararVigilante();
  }

 private:
  static void PonerSiNadieLoPidio(const char* nombre, const char* valor) {
    if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
      REXLOG_DEBUG("Ajuste '{}' no registrado todavia; no lo toco.", nombre);
      return;
    }
    if (rex::cvar::HasNonDefaultValue(nombre)) {
      return;  // set explicitly: do not override it.
    }
    if (rex::cvar::SetFlagByName(nombre, valor)) {
      REXLOG_DEBUG("Ajuste por defecto de la build portable: {} = {}", nombre, valor);
    }
  }

  // ==========================================================================
  //  5. HANG WATCHDOG
  //
  //  THE PROBLEM IT SOLVES
  //  When the game freezes, absolutely nothing shows up in the log: no error,
  //  no kernel call, no graphics command. Total silence. That rules out an
  //  exception or an unregistered function (those are visible) and leaves a
  //  single explanation: all the game's threads are stopped at once, waiting
  //  for something that never arrives.
  //
  //  And a deadlock cannot be diagnosed from the log, because what defines it
  //  is precisely that nothing gets written anymore. The threads have to be
  //  asked directly.
  //
  //  HOW IT WORKS
  //  A separate thread looks at all guest threads once per second and records
  //  two registers of each one:
  //
  //    lr  where the current function would return to. It changes constantly
  //        in code that makes progress.
  //    r1  the stack pointer. Same.
  //
  //  If for several seconds in a row no thread has moved either of the two, the
  //  game is not slow: it is stopped. Then the table is dumped.
  //
  //  WHAT THE DUMP GIVES
  //  For each thread: its entry address (which tells which thread it is), lr,
  //  r1 and r13. With that, a thread that waits (lr stuck in a kernel wait
  //  function) can be told apart from one that spins (lr jumping between two or
  //  three addresses).
  //
  //  It lives in the app and not in the SDK on purpose: that way it can be
  //  changed without rebuilding the whole SDK, and it does not impose a
  //  watchdog thread on anyone else.
  // ==========================================================================

  void ArrancarVigilante() {
    vigilante_activo_ = true;
    vigilante_ = std::thread([this] { VigilanteMain(); });
  }

  void PararVigilante() {
    vigilante_activo_ = false;
    if (vigilante_.joinable()) {
      vigilante_.join();
    }
  }

  template <typename Lista>
  static void VolcarHilos(const Lista& hilos, bool grave) {
    for (auto& h : hilos) {
      const auto* cp = h->creation_params();
      auto* estado = h->thread_state();
      if (estado && estado->context()) {
        const auto& c = *estado->context();
        if (grave) {
          REXLOG_ERROR("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} principal={} corriendo={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32);
        } else {
          REXLOG_DEBUG("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} principal={} corriendo={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32);
        }
      } else {
        REXLOG_DEBUG("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} sin contexto", h->thread_id(),
                     cp->start_address);
      }
    }
  }

  void VigilanteMain() {
    // How many seconds in a row without anything moving before raising the
    // alarm. Five is generous: a game at 10 fps still moves registers a hundred
    // times per second, so five still seconds are not slowness.
    constexpr int kSegundosParaSospechar = 5;
    constexpr int kSegundosEntreVolcados = 15;

    uint64_t firma_anterior = 0;
    int quietos = 0;
    int desde_ultimo_volcado = 0;
    int desde_instantanea = 0;
    bool avisado = false;
    int volcados_pilas = 0;  // logs/pilas_N.txt on PC (ufc3-undisputed-nx_perfil_pc.cpp)

    while (vigilante_activo_) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!vigilante_activo_) break;

      auto* kernel = rex::system::kernel_state();
      if (!kernel) continue;

      auto hilos = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
      if (hilos.empty()) continue;

      // A signature of "where everyone is". It does not need to be a good hash:
      // it only has to change if some register changes.
      //
      // Watch the order. GetObjectsByType does not guarantee it: in the real
      // dumps the threads came out shuffled from one round to the next, and
      // even repeated. So the signature changed on its own even when nothing
      // moved, and the alarm never fired. Putting each thread in a map keyed by
      // its id fixes it: the map sorts itself and a repeated id overwrites
      // instead of being counted twice.
      std::map<uint32_t, uint64_t> por_hilo;
      for (auto& h : hilos) {
        auto* estado = h->thread_state();
        if (!estado || !estado->context()) continue;
        const auto& c = *estado->context();
        por_hilo[h->thread_id()] =
            static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
            (static_cast<uint64_t>(c.r3.u32) << 40);
      }

      uint64_t firma = 1469598103934665603ull;
      for (const auto& [id_hilo, huella] : por_hilo) {
        firma = (firma ^ id_hilo) * 1099511628211ull;
        firma = (firma ^ huella) * 1099511628211ull;
      }

      // PERIODIC SNAPSHOT, WHATEVER HAPPENS.
      //
      // The alarm above only fires if nothing moves, and the hangs chased in
      // the fork's game were not of that kind: the registers kept changing,
      // meaning the game runs code but makes no progress. A tight loop waiting
      // for something that never arrives looks just as stuck from outside, and
      // yet the alarm does not catch it.
      //
      // So every ten seconds it records where each thread is, whether there is
      // a problem or not. When the game freezes, two or three snapshots of the
      // bad stretch remain, and if lr cycles between the same two or three
      // addresses, there is the loop. Debug level: no noise in normal use.
      if (++desde_instantanea >= 10) {
        desde_instantanea = 0;
        REXLOG_DEBUG("[vigilante] instantanea: {} hilos del juego", hilos.size());
        VolcarHilos(hilos, false);
      }

      if (firma != firma_anterior) {
        if (avisado) {
          REXLOG_WARN("[vigilante] el juego ha vuelto a moverse despues de {} s parado.", quietos);
          avisado = false;
        }
        firma_anterior = firma;
        quietos = 0;
        desde_ultimo_volcado = 0;
        continue;
      }

      ++quietos;
      ++desde_ultimo_volcado;
      if (quietos < kSegundosParaSospechar) continue;
      if (avisado && desde_ultimo_volcado < kSegundosEntreVolcados) continue;
      desde_ultimo_volcado = 0;

      REXLOG_ERROR("[vigilante] {} s sin que se mueva ni un registro en ninguno de los {} hilos "
                   "del juego. Esto no es lentitud: esta parado.",
                   quietos, hilos.size());
      VolcarHilos(hilos, true);
      // And the stacks of every thread in the process, host ones included: on
      // PC it writes logs/pilas_N.txt (ufc3-undisputed-nx_perfil_pc.cpp). Only on the first
      // two alarms.
      if (volcados_pilas < 2) {
        ++volcados_pilas;
        ufc3::perfil_pc::VolcarPilas("vigilante");
      }
      avisado = true;
    }
  }

  std::thread vigilante_;
  std::atomic<bool> vigilante_activo_{false};
};
