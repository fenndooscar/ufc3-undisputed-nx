// ufc3-undisputed-nx - screenshots of the emulated frame, taken from inside the process
// (see ufc3-undisputed-nx_captura.h for why this exists and how to turn it off).

#include "ufc3-undisputed-nx_captura.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(ufc3_undisputed_nx_captura_cada_s, 0, "UFC3_UNDISPUTED_NX",
                     "Guardar un fotograma del juego cada N segundos en capturas/ junto al ejecutable "
                     "(0 = nunca). Funciona con la ventana minimizada: la imagen sale de la GPU emulada, "
                     "no de la pantalla")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace ufc3::captura {
namespace {

std::atomic<bool> g_parar{true};
std::thread g_hilo;

// 24-bit BMP, bottom-up. The guest output is already R8 G8 B8 X8 with no padding
// between rows, so only the row order and the channel order change.
bool EscribirBMP(const std::filesystem::path& ruta, const rex::ui::RawImage& imagen) {
  if (imagen.width == 0 || imagen.height == 0 ||
      imagen.data.size() < imagen.stride * imagen.height) {
    return false;
  }
  const uint32_t ancho = imagen.width;
  const uint32_t alto = imagen.height;
  const uint32_t relleno = ((ancho * 3 + 3) / 4) * 4;
  const uint32_t tam_pixeles = relleno * alto;
  const uint32_t tam_fichero = 54 + tam_pixeles;

  std::vector<uint8_t> cabecera(54, 0);
  cabecera[0] = 'B';
  cabecera[1] = 'M';
  auto poner32 = [&cabecera](size_t pos, uint32_t v) {
    cabecera[pos] = uint8_t(v);
    cabecera[pos + 1] = uint8_t(v >> 8);
    cabecera[pos + 2] = uint8_t(v >> 16);
    cabecera[pos + 3] = uint8_t(v >> 24);
  };
  poner32(2, tam_fichero);
  poner32(10, 54);
  poner32(14, 40);
  poner32(18, ancho);
  poner32(22, alto);
  cabecera[26] = 1;  // planos
  cabecera[28] = 24;
  poner32(34, tam_pixeles);
  poner32(38, 2835);  // 72 ppp
  poner32(42, 2835);

  std::FILE* f = std::fopen(ruta.string().c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool bien = std::fwrite(cabecera.data(), 1, cabecera.size(), f) == cabecera.size();
  std::vector<uint8_t> fila(relleno, 0);
  for (uint32_t y = alto; y-- > 0 && bien;) {
    const uint8_t* origen = imagen.data.data() + size_t(imagen.stride) * y;
    for (uint32_t x = 0; x < ancho; ++x) {
      fila[x * 3 + 0] = origen[x * 4 + 2];
      fila[x * 3 + 1] = origen[x * 4 + 1];
      fila[x * 3 + 2] = origen[x * 4 + 0];
    }
    bien = std::fwrite(fila.data(), 1, fila.size(), f) == fila.size();
  }
  std::fclose(f);
  return bien;
}

void Bucle(rex::ui::Presenter* presenter, int32_t cada_s) {
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / "capturas";
  std::error_code error;
  std::filesystem::create_directories(carpeta, error);
  if (error) {
    REXLOG_WARN("[captura] no se pudo crear {}", rex::path_to_utf8(carpeta));
    return;
  }

  const auto inicio = std::chrono::steady_clock::now();
  uint32_t n = 0;
  while (!g_parar.load(std::memory_order_relaxed)) {
    // In 100 ms steps so Parar() does not have to wait for a whole period.
    for (int i = 0; i < cada_s * 10; ++i) {
      if (g_parar.load(std::memory_order_relaxed)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    rex::ui::RawImage imagen;
    if (!presenter->CaptureGuestOutput(imagen)) {
      continue;
    }
    ++n;
    const auto segundos =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio)
            .count();
    char nombre[64];
    std::snprintf(nombre, sizeof(nombre), "captura_%04u_t%lld.bmp", n,
                  static_cast<long long>(segundos));
    if (EscribirBMP(carpeta / nombre, imagen)) {
      REXLOG_INFO("[captura] {} ({}x{})", nombre, imagen.width, imagen.height);
    }
  }
}

}  // namespace

void Arrancar(rex::system::IGraphicsSystem* graficos) {
  const int32_t cada_s = REXCVAR_GET(ufc3_undisputed_nx_captura_cada_s);
  if (cada_s <= 0 || graficos == nullptr || g_hilo.joinable()) {
    return;
  }
  rex::ui::Presenter* presenter = graficos->presenter();
  if (presenter == nullptr) {
    REXLOG_WARN("[captura] el sistema de graficos no tiene presenter; no capturo nada");
    return;
  }
  g_parar.store(false, std::memory_order_relaxed);
  g_hilo = std::thread(Bucle, presenter, cada_s);
  REXLOG_INFO("[captura] un fotograma cada {} s en capturas/", cada_s);
}

void Parar() {
  if (!g_hilo.joinable()) {
    return;
  }
  g_parar.store(true, std::memory_order_relaxed);
  g_hilo.join();
}

}  // namespace ufc3::captura
