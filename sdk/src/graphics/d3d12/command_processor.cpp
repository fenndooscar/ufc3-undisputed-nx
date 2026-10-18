/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_readback_memexport, false, "GPU/D3D12",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve, false, "GPU/D3D12",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
                    "Submit command list when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// SONDA TEMPORAL: cuenta por donde se van los dibujos. Quitar cuando la escena
// negra este localizada.
REXCVAR_DEFINE_INT32(ufc3_undisputed_nx_sonda_dibujos, 0, "UFC3_UNDISPUTED_NX",
                     "Segundos entre informes del contador de dibujos (0 = apagado; solo pruebas)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {
REXCVAR_DECLARE(int32_t, ufc3_undisputed_nx_sonda_tinte);
}

namespace rex::graphics::d3d12 {

namespace {
// Cuantas texturas distintas se recuerdan por par de shaders. Con la clave por
// direccion base entran la piel, los pantalones, los guantes y las variantes del
// rival, asi que hacen falta mas que con la clave por indice de fetch constant.
static constexpr uint32_t kTexMax = 32;
// Cuantos bytes de cada textura se recorren para su huella de contenido, y cada cuantos
// dibujos del par se hace la parte cara de la sonda (interprete + huellas). El cuerpo
// negro que hay que cazar aparece y desaparece entre fotogramas con todos los agregados
// iguales, asi que la sonda mira el dibujo suelto; pero leer 4 KB por textura en cada
// dibujo serian cientos de MB/s, de ahi el muestreo.
static constexpr uint32_t kBytesHuella = 2048;
static constexpr uint64_t kCadaCaro = 64;
// Hashes from this UFC build's CAF census and material-tag capture: skin and hair.
// Reuse the bounded, opt-in captures to compare both materials' actual inputs.
static bool IsDiagnosticMaterial(uint64_t hash) {
  return hash == 0xC8F8C8858B1E1E5Eull || hash == 0x2C56B11C103C5673ull;
}
// SONDA TEMPORAL: ejecuta el shader de vertices del invitado en el interprete y
// recoge lo que escribe en los interpoladores. Es la unica forma de ver lo que
// el shader de pixeles recibe de verdad: la traduccion a DXBC es fiel (esta
// leida linea a linea), las texturas estan cargadas y las constantes del banco
// de vertices son las que son, asi que lo que falta por medir es el valor que
// sale del shader de vertices hacia el de pixeles. El interprete usa el mismo
// fichero de registros y la misma memoria que el shader real, no una copia.
class SondaSalidaVertices : public ShaderInterpreter::ExportSink {
 public:
  void Reset() {
    interpolador[0] = interpolador[1] = interpolador[2] = false;
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        valor[i][j] = 0.0f;
      }
      mascara[i] = 0;
    }
  }
  void Export(ucode::ExportRegister registro, const float* valor_exportado,
              uint32_t mascara_exportada) override {
    uint32_t indice;
    switch (registro) {
      case ucode::ExportRegister::kVSInterpolator0:
        indice = 0;
        break;
      case ucode::ExportRegister::kVSInterpolator1:
        indice = 1;
        break;
      case ucode::ExportRegister::kVSInterpolator2:
        indice = 2;
        break;
      default:
        return;
    }
    std::memcpy(valor[indice], valor_exportado, sizeof(valor[indice]));
    mascara[indice] = mascara_exportada;
    interpolador[indice] = true;
  }
  float valor[3][4] = {};
  uint32_t mascara[3] = {};
  bool interpolador[3] = {};
};

// Un ESTADO DE MATERIAL, no un dibujo. El sintoma que hay que explicar
// (BRIEFING_CAF.md, UPDATE 22) es que el cuerpo del luchador creado se pone negro entre
// dos fotogramas sin que cambie ningun fetch constant; si el material del dibujo no
// cambia, lo que cambia esta mas abajo y hay que mirar el dibujo suelto. Lo que se
// guarda por par de shaders es la LISTA DE ESTADOS DISTINTOS que ese par usa, con
// cuantos dibujos lleva cada uno: comparar la pelea de luchadores normales con la del
// creado es comparar dos listas, y eso no llena el disco.
struct SondaDibujo {
  uint64_t vs = 0, ps = 0;
  uint32_t prim_visitante = 0, prim_anfitrion = 0, indices = 0, vertices = 0;
  uint32_t mascara_vs = 0, mascara_ps = 0;
  uint32_t c1[4] = {}, c2[4] = {}, c162[4] = {}, c37[4] = {};
  uint32_t color_cntl = 0, blend0 = 0, color_mask = 0, depth_cntl = 0, sc_mode = 0;
  uint32_t color_info = 0, depth_info = 0, surf_info = 0;
  uint32_t has_ps = 0, mata = 0, escribe_color = 0, escribe_prof = 0;
  uint32_t interp_v1[4] = {}, interp_v2[4] = {}, interp_uv[4] = {};
  uint32_t interp_v1_bits[4] = {}, interp_uv_bits[4] = {};
  uint32_t vfetch0_dir = 0, vfetch0_largo = 0, vfetch0_tipo = 0, vfetch0_stride = 0;
  uint32_t vfetch0_bytes[8] = {};
  // Las constantes BOOLEANAS del invitado, que es lo que el shader de vertices usa para
  // elegir rama: el microcodigo tiene `cjmp b0, L37`, `cjmp !b1, L9`, `cjmp !b2, L17`,
  // `cjmp !b3, L27` y `cexec b4`, y cada rama calcula o0 de una manera distinta. Si la
  // rama elegida no es la que la consola elige, o0 lleva otra cosa (una posicion en vez
  // de una UV) y el shader de pixeles muestrea la textura con coordenadas que no son
  // UV: eso da justo un mosaico que se mueve con el luchador.
  uint32_t bools[8] = {};
  uint32_t bools_extra[4] = {};
  uint32_t tex_n = 0;
  struct Tex {
    uint32_t fc = 0, base = 0, mip = 0, fmt = 0, ancho = 0, alto = 0;
    uint32_t swizzle_host = 0, signos = 0, niveles = 0, semilla = 0;
    int32_t delta_media = 0;
    uint32_t enrt = 0;
    uint32_t indice_sin_signo = 0, indice_con_signo = 0;
  };
  Tex tex[8];
  // Cuantos dibujos usaron este estado, y cuando se vio por primera y ultima vez.
  uint64_t veces = 0;
  char primera[24] = {}, ultima[24] = {};
  uint32_t huella_barata = 0;
  // Los 8 bytes crudos del bloque DXT1 que el shader muestrea en ESTE estado, sacados
  // de la memoria del invitado con las UV del estado. Es el numero que decide el color:
  // todo lo demas (constantes, sampler, fetch constant) puede estar bien y el bloque
  // ser negro. Se guardan crudos y se descodifican fuera, con el lector ya probado.
  uint32_t bloque_fc0[2] = {}, bloque_fc3[2] = {};
  uint32_t bloque_uv0[2] = {}, bloque_uv3[2] = {};   // texel al que apuntan
  uint32_t bloque_ok = 0;                            // 1 si se pudieron leer
};

struct SondaDibujos {
  uint64_t total = 0, pintados = 0, vertices = 0;
  uint64_t sin_vs = 0, pitch0 = 0, sin_raster = 0, sin_vertices = 0;
  uint64_t pipeline_fallido = 0, async_pendiente = 0, bindings_fallido = 0;
  // Fetch de vertices que el shader pide y que el juego no tenia puestos: la
  // malla desaparece sin que se note en los contadores de dibujos.
  uint64_t vfetch_invalido = 0, vfetch_sin_rango = 0;
  uint64_t memexport = 0, sin_ps = 0, swaps = 0, copias = 0, sin_rt = 0;
  uint64_t fmt_color[96] = {}, fmt_prof[96] = {};
  uint64_t ultimo_vs = 0, ultimo_ps = 0;
  std::chrono::steady_clock::time_point ultimo = std::chrono::steady_clock::now();

  // Que pares (shader de vertices, shader de pixeles) pintan cada intervalo.
  // Comparar la fase buena (modo demostracion) con la mala (entrenamiento de
  // carrera) dice que shaders son los de la escena negra.
  struct ParShaders {
    uint64_t vs = 0, ps = 0, dibujos = 0, vertices = 0;
    uint32_t recorte[4] = {};  // x0, y0, x1, y1 del scissor del primer dibujo
    uint32_t vista[2] = {};    // ancho, alto del viewport del primer dibujo
    uint32_t vs_const_no_cero = 0, ps_const_no_cero = 0;
    uint32_t vs_c0[4] = {};
    // Estado del destino: si la escena 3D cae en otra superficie que la que se
    // resuelve a la ventana, se vera negra aunque los dibujos sean correctos.
    uint32_t color_info = 0, depth_info = 0, surface_info = 0, modecontrol = 0;
    uint32_t color_mask = 0, blend0 = 0, sc_mode_cntl = 0, clip_cntl = 0;
    // Si el test de profundidad descarta todo, la malla se dibuja y no se ve.
    // depth_info solo dice donde esta el buzon, no como se prueba.
    uint32_t depth_control = 0, stencil_ref_mask = 0;
    // PA_CL_VTE_CNTL decide si la posicion sale dividida por W y en que rango
    // esta Z. Con los bits mal puestos todos los vertices caen al mismo sitio.
    uint32_t vte_cntl = 0;
    // Lo que el analisis del microcodigo dice del shader de pixeles de este par.
    uint32_t ps_escribe_color = 0, ps_escribe_prof = 0, ps_mata_pixeles = 0, ps_exporta = 0;
    uint32_t vs_exporta = 0;
    // RB_COLORCONTROL: si la prueba de alfa esta activa con una funcion que
    // descarta todo, el color nunca llega aunque colorMask y blend esten bien.
    uint32_t color_control = 0;
    // Las texturas que el shader de pixeles muestrea, con sus seis dwords. Si el
    // tipo no es "textura", o no hay pagina base ni de mip, la cache descarta la
    // textura en silencio y el muestreo devuelve (0,0,0,0): el luchador sale sin
    // texturas, negro si el color se multiplica por el muestreo y transparente
    // si el alfa sale de ahi.
    //
    // La tabla se llena en TODOS los dibujos del par, no solo en el primero: el
    // cuerpo, los pantalones y los guantes son dibujos distintos, asi que mirar
    // solo el primero hace creer que la textura del cuerpo es la de los guantes.
    // La clave es el indice del fetch constant, que es lo que el shader nombra.
    uint32_t tex_usadas = 0, tex_malas = 0, tex_sin_pagina = 0;
    uint32_t tex_huecos = 0;
    uint32_t tex_fc[kTexMax] = {}, tex_tipo[kTexMax] = {}, tex_base[kTexMax] = {},
             tex_mip[kTexMax] = {}, tex_fmt[kTexMax] = {}, tex_wh[kTexMax][2] = {},
             tex_dw[kTexMax][6] = {};
    // Cuantos dibujos de este par usaron cada textura, y si su pagina base cae
    // dentro de una resolucion de render target reciente. Una textura hecha en
    // tiempo de ejecucion (la piel y los tatuajes de un luchador creado por el
    // jugador) sale de ahi, y su contenido depende de que la escritura al buzon
    // y la lectura de la textura se compongan bien.
    uint32_t tex_dibujos[kTexMax] = {}, tex_en_rt[kTexMax] = {};
    uint32_t tex_endian[kTexMax] = {}, tex_tiled[kTexMax] = {}, tex_exp_adjust[kTexMax] = {},
             tex_num_format[kTexMax] = {};
    // Los primeros dwords en la pagina base de cada textura: si son todos cero,
    // los datos de la textura no estan en la memoria del invitado y por eso el
    // muestreo da negro, aunque la clave de la textura sea valida.
    uint32_t tex_bytes[kTexMax][8] = {}, tex_bytes_leidos[kTexMax] = {};
    // Y de toda la textura: que parte son ceros y cuantos valores distintos hay.
    // Una textura que nunca se subio es casi toda ceros.
    uint32_t tex_ceros[kTexMax] = {}, tex_varios[kTexMax] = {}, tex_bytes_vistos[kTexMax] = {};
    // Lo que la cache de texturas dice de cada una DESPUES de pedirlas: si el
    // enlace existe y si tiene textura de host. Un enlace nulo o sin textura se
    // convierte en un muestreo de (0,0,0,0), que es negro si el color se
    // multiplica por el y transparente si el alfa sale de ahi.
    uint32_t tex_enlace[kTexMax] = {}, tex_objeto[kTexMax] = {}, tex_swizzle[kTexMax] = {};
    // Los signos que el shader de pixeles va a leer de "texture_swizzled_signs"
    // para cada una. El muestreo elige entre la version con signo y sin signo
    // de la textura segun ellos, y el codigo de conversion de formato del
    // shader traduce el valor. Con signo "gamma" (3) el muestreo aplica ademas
    // la conversion de gamma a lineal: si la textura ya viniera en lineal, ese
    // paso la oscurece.
    uint32_t tex_signos[kTexMax] = {};
    float vport_zscale = 0.0f, vport_zoffset = 0.0f;
    // Sondas de vertices: los fetch de tipo vertice que el juego tenia puestos,
    // con su direccion y tamano (en dwords). Si la malla sale negra y estos
    // apuntan a memoria sin asignar, el problema esta aqui, no en el shader.
    uint32_t vfetch[6][3] = {};  // indice, direccion, tamano
    uint32_t vfetch_tipo[6] = {};
    uint32_t vfetch_leidos = 0;
    uint32_t prim = 0, indices = 0;
    // Y los primeros dwords del primer buffer de vertices, tal cual.
    uint32_t vfetch_bytes[32] = {};
    uint32_t vfetch_bytes_leidos = 0;
    // Los registros de constante que el desensamblado del shader nombra uno por
    // uno. El analisis de microcodigo decide cuantas constantes se suben al
    // cbuffer y en que orden; si esa cuenta no cuadra con lo que el shader lee,
    // el shader recibe el valor de otra constante y la escena sale negra o
    // transparente sin que nada avise.
    uint32_t vs_mapa[2] = {};              // float_count, direccionamiento dinamico
    uint32_t ps_mapa[2] = {};
    uint32_t ps_c[5][4] = {};              // c195, c242, c243, c254, c255
    uint32_t ps_c_usadas = 0;              // cuantas de las cinco se pudieron leer
    uint32_t vs_c74[4][4] = {};            // c74..c77, la primera matriz de hueso
    // Mapa de que constantes flotantes del banco de vertices son distintas de
    // cero, 256 bits en 64 caracteres hexadecimales. El shader de vertices del
    // luchador calcula su color de vertice a partir de c1, c2, c9, c10, c37,
    // c162, c196, c205 y c213; si esas son cero, el color de vertice sale cero,
    // el shader de pixeles multiplica la textura por el y el luchador sale
    // negro sin que falle ni una textura ni un enlace. Es la unica forma de
    // distinguir "la constante esta a cero" de "la constante no se subio".
    char vs_mapa_ceros[65] = {};
    // Las constantes del banco de vertices que el desensamblado del shader del
    // luchador nombra para calcular el color de vertice y el factor de mezcla
    // del borde. Si son cero, el color de vertice sale cero.
    static constexpr uint32_t kIndicesVs[16] = {1,  2,   9,   10,  31, 37, 54, 55,
                                                56, 57,  162, 196, 205, 213, 255, 0};
    uint32_t vs_c[16][4] = {};
    // El muestreador que el emulador construye de verdad para cada textura que el
    // shader de pixeles lee, y no solo los campos del fetch constant: la cache
    // reescribe el filtro (anisotropic_override = 3 fuerza 4x en cuanto la textura
    // tiene mips, y todas las de este juego tienen 4 niveles) y fija MinLOD y
    // MaxLOD. Si el cuerpo del luchador creado sale embadurnado y el nivel base de
    // su textura esta bien, la pregunta es que nivel esta leyendo, y eso lo decide
    // el muestreador, no el fetch constant.
    struct Muestreador {
      uint32_t fc = 0;
      uint32_t fetch_mag = 0, fetch_min = 0, fetch_mip = 0, fetch_aniso = 0;
      uint32_t fetch_lod_bias = 0;      // crudo, 5 bits fraccionarios
      uint32_t fetch_mip_min = 0, fetch_mip_max = 0;
      uint32_t muestreador_valor = 0;   // SamplerParameters::value
      uint32_t muestreador_mag = 0, muestreador_min = 0, muestreador_mip = 0;
      uint32_t muestreador_aniso = 0, muestreador_mip_min = 0, muestreador_base_map = 0;
      uint32_t clamp = 0;
      uint32_t usos = 0;
    };
    Muestreador muestreadores[8];
    uint32_t muestreadores_usados = 0;
    // Lo que el shader muestrea DE VERDAD, no lo que pide el fetch constant: el
    // indice dentro del monton de descriptores sin enlazar y la clave de la textura
    // que ese indice resuelve AHORA MISMO. Todo lo demas que mira la sonda es la
    // peticion del invitado; esto es el resultado. Sirve para el sintoma mas duro
    // que hay: el cuerpo del luchador creado se pone negro entre dos fotogramas sin
    // que cambie ni una constante ni un fetch constant, asi que lo que cambia tiene
    // que estar aqui o mas abajo.
    struct EnlaceReal {
      uint32_t fc = 0;
      uint32_t indice_sin_signo = 0xFFFFFFFFu;   // lo que lee el shader si signos = 00
      uint32_t indice_con_signo = 0xFFFFFFFFu;   // lo que lee si signos != 00
      uint32_t firma_desc_sin = 0, firma_desc_con = 0;
      uint32_t base = 0, mip = 0, fmt = 0, ancho = 0, alto = 0;
      uint32_t niveles = 0, swizzle_host = 0, signos = 0;
      uint32_t es_firmado = 0, banderas_clave = 0;
      uint32_t signos_sistema = 0;
      uint32_t usos = 0;
    };
    EnlaceReal enlaces_reales[8];
    uint32_t enlaces_reales_usados = 0;
    // La identidad del monton de descriptores del fotograma: sin esto, un indice no
    // dice a que recurso apunta. Se rellena una vez por informe.
    uint64_t descriptor_heap_pixel = 0;
    uint32_t descriptor_buffer_pixel = 0;
    // Los estados de material DISTINTOS que ha usado este par, con cuantas veces cada
    // uno. Es la tabla que hay que comparar entre la pelea normal y la del luchador
    // creado: si en la normal el cuerpo aparece con el estado A y en la del creado con
    // el estado B, la diferencia entre A y B es el fallo.
    static constexpr uint32_t kEstadosMax = 32;
    SondaDibujo estados[kEstadosMax];
    uint32_t estados_usados = 0;
    uint64_t dibujos_del_par = 0;
    uint64_t estados_saturados = 0;
    uint32_t huella_impresa = 0;
    // El ultimo juego de texturas e interpoladores medidos, para no perderlos en los
    // dibujos en que la parte cara de la sonda no corre.
    SondaDibujo::Tex tex_ultimas[8];
    uint32_t tex_ultimas_n = 0;
    uint32_t interp_uv_ultimo[4] = {}, interp_v1_ultimo[4] = {}, interp_v2_ultimo[4] = {};
    // Seguimiento del CONTENIDO de cada textura del par a lo largo de la sesion: el
    // minimo y el maximo de la media de sus primeros bytes, cuantas huellas distintas
    // se han visto y cuando estuvo mas oscura. El cuerpo del luchador creado se pone
    // negro entre fotogramas con todos los agregados iguales; si su textura pasa de
    // gris a negro y vuelve, aqui se ve, con la hora.
    struct SeguimientoTex {
      uint32_t base = 0, fc = 0, usada = 0;
      uint32_t media_min = 256, media_max = 0, media_ultima = 0;
      uint32_t huellas = 0, huella_ultima = 0;
      char hora_min[24] = {};
    };
    SeguimientoTex seguimiento[32];
    uint32_t seguimiento_usado = 0;
    // Los seis dwords crudos del fetch de vertice que el shader usa, con el
    // endian y el exp_adjust, mas el formato del render target de color 0. La
    // posicion del vertice sale como basura si el endian no es el que toca, y
    // el color de salida se escala por 2^color_exp_bias: con 8_8_8_8 es 1, y
    // un exponente distinto apaga la escena entera sin que falle nada mas.
    uint32_t vs_vfetch_dw[2] = {};
    uint32_t color_info0 = 0, color_mask0 = 0;
    // Lo que el shader de vertices escribe en los interpoladores, tal cual sale
    // del interprete. Es lo que el shader de pixeles recibe como v0, v1 y v2.
    uint32_t vs_salida[3][4] = {};
    uint32_t vs_salida_mascara[3] = {};
    uint32_t vs_salida_leida = 0;
    // El color de vertice (o1 = TEXCOORD1 = lo que el shader de pixeles lee como
    // v1 y multiplica por la textura) y las UV (o0), recorriendo VARIOS vertices
    // del dibujo. Si el minimo del color es cero, hay vertices con color negro y
    // la malla interpolada sale apagada aunque la textura este bien.
    uint32_t v1_leidos = 0;
    float interp_min[3][4] = {}, interp_max[3][4] = {}, interp_media[3][4] = {};
    // Escala de resolucion con la que se dibuja este par y muestras del destino.
    // Con escala mayor que 1 el shader de pixeles multiplica las UV por ella
    // para que el muestreo caiga bien sobre la textura, y un error ahi manda
    // todas las muestras al mismo texel: la malla sale plana y apagada sin que
    // falle ni una textura ni un enlace.
    uint32_t ancho_vista = 0, alto_vista = 0, muestras = 0;
  };
  // Las constantes del banco de vertices que el desensamblado del shader del
  // luchador nombra para calcular el color de vertice.
  static constexpr uint32_t kIndicesVs[16] = {1,  2,   9,   10,  31, 37, 54, 55,
                                              56, 57,  162, 196, 205, 213, 255, 0};
  static constexpr uint32_t kParesMax = 256;
  ParShaders pares[kParesMax];
  uint32_t pares_usados = 0;

  // Destino de la ultima resolucion de la ventana: si la escena 3D se resuelve
  // a una direccion distinta de la que luego se presenta, se vera negra aunque
  // se dibuje bien.
  uint32_t copia_destino = 0, copia_longitud = 0, copia_repeticiones = 0;
  // Cuantas resoluciones escriben ENCIMA del front buffer. Si hay mas de una por
  // fotograma, la segunda puede estar tapando lo que pinto la primera.
  uint64_t copias_frente = 0;
  // Las direcciones a las que se ha resuelto un render target, para poder
  // reconocer una textura hecha en tiempo de ejecucion.
  std::set<uint32_t> copias_recientes;
  uint32_t swap_ptr = 0, swap_ancho = 0, swap_alto = 0;

  // Flujos memexport vistos.
  uint64_t memexport_rangos = 0, memexport_bytes = 0;
  // Los ultimos flujos exportados: la malla de un luchador se dibuja en un
  // fotograma posterior al que la exporta, asi que hay que comparar contra estos
  // y no contra los del propio dibujo.
  std::vector<draw_util::MemExportRange> me_recientes;
  // Por que un dibujo que exporta memoria acaba sin ningun rango. Si el juego
  // escribe eM# pero "AddMemExportRanges" (draw.cpp:641) descarta todas las
  // constantes de flujo por sus comprobaciones de cordura, no hay nada que
  // acotar, el destino real se queda sin invalidar y la malla del luchador sale
  // negra o colapsada (xenia-canary #1093). Los dos casos son silenciosos.
  uint64_t me_shaders = 0, me_sin_constantes = 0, me_rechazadas = 0, me_aceptadas = 0;
  uint32_t me_constante = 0, me_eM = 0, me_es_vertice = 0;
  uint32_t me_dwords[4] = {};
  uint32_t me_ok_constante = 0, me_ok_dwords[4] = {};
  // Del primer flujo aceptado: formato, numero de indices y lo que ocupa. Si el
  // tamano calculado (indices * bytes por pixel) es mayor que lo reservado, el
  // acotado de #1093 importa; si no, el problema es otro.
  uint32_t me_ok_formato = 0, me_ok_indices = 0, me_primer_base = 0, me_primer_largo = 0;
  uint32_t me_rango_max = 0, me_rango_max_base = 0;
  // Vertices que el juego lee de memoria que un memexport acaba de escribir: si
  // la malla del luchador sale de aqui, el problema es el flujo exportado y no
  // los buffers de vertices ni las texturas.
  uint64_t vfetch_en_export = 0, vfetch_total = 0;
  uint32_t vfetch_export_dir = 0, vfetch_export_largo = 0;
  uint32_t vfetch_export_rango = 0, vfetch_export_rango_largo = 0;


  // Cuenta los flujos memexport de cada dibujo para el informe. El recorte a la
  // reserva del invitado ya no vive aqui: lo hace "ClampMemExportRangeToAllocation"
  // en draw.cpp, que es comun a los dos backends, asi que a este punto los
  // rangos ya llegan acotados.
  void AnotaMemexport(const draw_util::MemExportRange& rango, memory::Memory* memory) {
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    ++memexport_rangos;
    memexport_bytes += rango.size_bytes;
    if (memory == nullptr) {
      return;
    }
    me_recientes.push_back(rango);
    if (me_recientes.size() > 64) {
      me_recientes.erase(me_recientes.begin());
    }
    if (me_primer_largo == 0) {
      me_primer_base = rango.base_address_dwords << 2;
      me_primer_largo = rango.size_bytes;
    }
    if (rango.size_bytes > me_rango_max) {
      me_rango_max = rango.size_bytes;
      me_rango_max_base = rango.base_address_dwords << 2;
    }
  }

  // Reproduce, solo para contarlo, el filtro de "AddMemExportRanges": dice si el
  // shader no declara ninguna constante de flujo o si las que declara no pasan
  // las comprobaciones de cordura. Guarda los cuatro dwords de la primera
  // rechazada, que es lo que hace falta para entender por que.
  void AnotaStreams(const RegisterFile& regs, const Shader& shader, bool es_vertice) {
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    ++me_shaders;
    me_eM = shader.memexport_eM_written();
    me_es_vertice = es_vertice ? 1 : 0;
    const std::set<uint32_t>& constantes = shader.memexport_stream_constants();
    if (constantes.empty()) {
      ++me_sin_constantes;
      return;
    }
    const uint32_t base = es_vertice ? regs.Get<reg::SQ_VS_CONST>().base
                                     : regs.Get<reg::SQ_PS_CONST>().base;
    for (uint32_t indice : constantes) {
      const xenos::xe_gpu_memexport_stream_t flujo = regs.GetMemExportStream(base + indice);
      if (flujo.const_0x1 != 0x1 || flujo.const_0x4b0 != 0x4B0 || flujo.const_0x96 != 0x96 ||
          !flujo.index_count) {
        ++me_rechazadas;
        if (me_rechazadas == 1) {
          me_constante = base + indice;
          me_dwords[0] = flujo.dword_0;
          me_dwords[1] = flujo.dword_1;
          me_dwords[2] = flujo.dword_2;
          me_dwords[3] = flujo.dword_3;
        }
      } else {
        ++me_aceptadas;
        if (me_aceptadas == 1) {
          me_ok_constante = base + indice;
          me_ok_dwords[0] = flujo.dword_0;
          me_ok_dwords[1] = flujo.dword_1;
          me_ok_dwords[2] = flujo.dword_2;
          me_ok_dwords[3] = flujo.dword_3;
          me_ok_formato = uint32_t(flujo.format);
          me_ok_indices = flujo.index_count;
        }
      }
    }
  }

  // Una textura entera del invitado a disco, con cabecera DDS para poder abrirla
  // directamente. Se salta las que ya se volcaron y las que pasan de 8 MB.
  //
  // Se guarda CON el canje de bytes que aplica el cargador segun el campo
  // "endianness" del fetch constant, porque esos son los bytes que ve la GPU.
  // Guardar los crudos de memoria fue un error que costo tiempo: la imagen sale
  // con los colores cambiados (violeta y verde en una textura de piel) y parece
  // que la textura esta rota cuando en realidad esta bien. Los crudos se guardan
  // aparte, con el sufijo "_crudo", solo para poder comparar.
  //
  // El tamano sale de la MISMA cuenta que usa la cache (TextureGuestLayout), no de
  // "ancho*alto*bpp/8": el invitado guarda las texturas lineales con la fila de
  // bloques alineada a 256 bytes, asi que la cuenta empaquetada se queda corta en
  // cuanto la textura es mas estrecha que eso y la parte de abajo nunca se miraba.
  // Costo una conclusion falsa: el "azul" de la ventana de 0x171EC caia fuera de
  // los bytes escritos. `row_pitch` va en el nombre para que se vea a simple vista.
  // Tamano real en memoria del invitado del nivel base de una textura, con la
  // MISMA cuenta que usa la cache para pedir los rangos residentes. La cuenta
  // empaquetada "ancho*alto*bpp/8" se queda corta en cuanto la textura es mas
  // estrecha que la alineacion de fila de 256 bytes que el invitado usa en las
  // lineales, y ese trozo de abajo nunca se volcaba.
  static uint32_t BytesDelNivelBase(const xenos::xe_gpu_texture_fetch_t& t, uint32_t ancho,
                                    uint32_t alto, uint32_t* fila_out) {
    texture_util::TextureGuestLayout dispo = texture_util::GetGuestTextureLayout(
        t.dimension, t.pitch, ancho, alto, 1, bool(t.tiled),
        xenos::TextureFormat(t.format), bool(t.packed_mips), true,
        std::max(t.mip_min_level, t.mip_max_level));
    if (fila_out != nullptr) {
      *fila_out = dispo.base.row_pitch_bytes;
    }
    return dispo.base.level_data_extent_bytes;
  }

  // El muestreador real por textura del shader de pixeles: los campos del fetch
  // constant, el SamplerParameters que la cache construye de ellos y el numero de
  // niveles que tiene la textura en el host. La diferencia entre lo que pide el
  // invitado y lo que acaba en el heap de muestreadores es lo que hay que ver.
  void AnotaMuestreadores(ParShaders& par, const Shader* ps_shader, const RegisterFile& regs,
                          const D3D12TextureCache* cache) {
    if (ps_shader == nullptr || cache == nullptr) {
      return;
    }
    const D3D12Shader& shader = static_cast<const D3D12Shader&>(*ps_shader);
    for (const Shader::TextureBinding& enlace : shader.texture_bindings()) {
      if (enlace.fetch_constant >= 32) {
        continue;
      }
      uint32_t hueco = par.muestreadores_usados;
      for (uint32_t i = 0; i < par.muestreadores_usados; ++i) {
        if (par.muestreadores[i].fc == enlace.fetch_constant) {
          hueco = i;
          break;
        }
      }
      if (hueco >= rex::countof(par.muestreadores)) {
        continue;
      }
      ParShaders::Muestreador& m = par.muestreadores[hueco];
      if (hueco == par.muestreadores_usados) {
        m = ParShaders::Muestreador{};
        m.fc = enlace.fetch_constant;
        ++par.muestreadores_usados;
      }
      ++m.usos;
      const xenos::xe_gpu_texture_fetch_t t = regs.GetTextureFetch(enlace.fetch_constant);
      const ParsedTextureFetchInstruction& instr = enlace.fetch_instr;
      m.fetch_mag = uint32_t(t.mag_filter);
      m.fetch_min = uint32_t(t.min_filter);
      m.fetch_mip = uint32_t(t.mip_filter);
      m.fetch_aniso = uint32_t(t.aniso_filter);
      m.fetch_lod_bias = uint32_t(int32_t(t.lod_bias) & 0x3FF);
      uint32_t min_nivel = 0, max_nivel = 0;
      texture_util::GetSubresourcesFromFetchConstant(t, nullptr, nullptr, nullptr, nullptr,
                                                     nullptr, &min_nivel, &max_nivel);
      m.fetch_mip_min = min_nivel;
      m.fetch_mip_max = max_nivel;
      // Lo que la instruccion de muestreo impone, por encima del fetch constant.
      if (instr.attributes.mag_filter != xenos::TextureFilter::kUseFetchConst) {
        m.fetch_mag = uint32_t(instr.attributes.mag_filter);
      }
      if (instr.attributes.min_filter != xenos::TextureFilter::kUseFetchConst) {
        m.fetch_min = uint32_t(instr.attributes.min_filter);
      }
      if (instr.attributes.mip_filter != xenos::TextureFilter::kUseFetchConst) {
        m.fetch_mip = uint32_t(instr.attributes.mip_filter);
      }
      if (instr.attributes.lod_bias != 0.0f) {
        m.fetch_lod_bias = uint32_t(int32_t(instr.attributes.lod_bias * 32.0f) & 0x3FF);
      }
      // La firma real que acaba en el heap. Se busca el enlace de muestreador del
      // mismo fetch constant ya traducido, que es el que la cache convierte.
      if (cache != nullptr) {
        for (const D3D12Shader::SamplerBinding& sb :
             shader.GetSamplerBindingsAfterTranslation()) {
          if (sb.fetch_constant != enlace.fetch_constant) {
            continue;
          }
          D3D12TextureCache::SamplerParameters p = cache->GetSamplerParameters(sb);
          m.muestreador_valor = p.value;
          m.muestreador_mag = p.mag_linear;
          m.muestreador_min = p.min_linear;
          m.muestreador_mip = p.mip_linear;
          m.muestreador_aniso = uint32_t(p.aniso_filter);
          m.muestreador_mip_min = p.mip_min_level;
          m.muestreador_base_map = p.mip_base_map;
          m.clamp = uint32_t(p.clamp_x) | (uint32_t(p.clamp_y) << 3) | (uint32_t(p.clamp_z) << 6);
          break;
        }
      }
    }
  }

  // El enlace REAL de las texturas del shader de pixeles: el indice dentro del monton
  // de descriptores y la clave de textura que resuelve. Se lee del BUFFER que el
  // fotograma ya subio (current_texture_srv_keys_pixel_), no volviendo a preguntar a
  // la cache: preguntar otra vez da la respuesta de ahora, no la que el dibujo usa.
  // La cache solo se toca para los casos que el buffer no cubre (dimension forzada,
  // version con signo), que son los mismos que trata UpdateBindings.
  void AnotaEnlacesReales(ParShaders& par, const Shader* ps_shader, const RegisterFile& regs,
                          D3D12TextureCache* cache,
                          const std::vector<D3D12TextureCache::TextureSRVKey>* claves,
                          uint64_t monton, uint32_t tam_descriptor) {
    par.descriptor_heap_pixel = monton;
    par.descriptor_buffer_pixel = tam_descriptor;
    if (ps_shader == nullptr || cache == nullptr) {
      return;
    }
    const D3D12Shader& shader = static_cast<const D3D12Shader&>(*ps_shader);
    const std::vector<D3D12Shader::TextureBinding>& enlaces =
        shader.GetTextureBindingsAfterTranslation();
    // Las claves solo valen si son de ESTE shader: el buffer se reutiliza mientras el
    // uid de composicion no cambie, y con otro shader los indices no corresponden.
    const bool claves_validas =
        claves != nullptr && claves->size() == enlaces.size() && !enlaces.empty();
    for (size_t i = 0; i < enlaces.size(); ++i) {
      const D3D12Shader::TextureBinding& e = enlaces[i];
      if (e.fetch_constant >= 32) {
        continue;
      }
      uint32_t hueco = par.enlaces_reales_usados;
      for (uint32_t j = 0; j < par.enlaces_reales_usados; ++j) {
        if (par.enlaces_reales[j].fc == e.fetch_constant) {
          hueco = j;
          break;
        }
      }
      if (hueco >= rex::countof(par.enlaces_reales)) {
        continue;
      }
      ParShaders::EnlaceReal& r = par.enlaces_reales[hueco];
      if (hueco == par.enlaces_reales_usados) {
        r = ParShaders::EnlaceReal{};
        r.fc = e.fetch_constant;
        r.indice_sin_signo = UINT32_MAX;
        r.indice_con_signo = UINT32_MAX;
        ++par.enlaces_reales_usados;
      }
      ++r.usos;
      // Los DOS indices que el shader puede elegir en caliente: el DXBC del camino
      // sin enlazar consulta la ranura sin signo y la ranura con signo, y elige con
      // el byte de signos del sistema. Informar solo la que lleva la bandera
      // `is_signed` del enlace da el descriptor nulo para todos, que es lo que
      // UpdateBindings deja cuando el formato no tiene version con signo aparte: no
      // es el fallo, es la ranura que el shader no lee. Hay que dar las dos.
      D3D12Shader::TextureBinding e_sin = e;
      e_sin.is_signed = false;
      r.indice_sin_signo = cache->GetActiveTextureBindlessSRVIndex(e_sin);
      D3D12Shader::TextureBinding e_con = e;
      e_con.is_signed = true;
      r.indice_con_signo = cache->GetActiveTextureBindlessSRVIndex(e_con);
      const xenos::xe_gpu_texture_fetch_t t = regs.GetTextureFetch(e.fetch_constant);
      uint32_t ancho = 0, alto = 0, base = 0, mip = 0, min_nivel = 0, max_nivel = 0;
      texture_util::GetSubresourcesFromFetchConstant(t, &ancho, &alto, nullptr, &base, &mip,
                                                     &min_nivel, &max_nivel);
      r.base = base;
      r.mip = mip;
      r.fmt = uint32_t(t.format);
      r.ancho = ancho + 1;
      r.alto = alto + 1;
      r.niveles = max_nivel - min_nivel + 1;
      if (claves_validas) {
        const D3D12TextureCache::TextureSRVKey& clave = (*claves)[i];
        r.firma_desc_sin = clave.key.base_page;
        r.firma_desc_con = clave.key.mip_page;
        r.swizzle_host = clave.host_swizzle;
        r.signos = clave.swizzled_signs;
        r.banderas_clave = (uint32_t(clave.key.tiled) << 0) |
                           (uint32_t(clave.key.format) << 4) |
                           (uint32_t(clave.key.packed_mips) << 12) |
                           (uint32_t(clave.key.mip_max_level & 7) << 13) |
                           (uint32_t(clave.key.endianness) << 16);
      }
      r.es_firmado = e.is_signed ? 1 : 0;
    }
  }

  // La identidad del monton de descriptores, para poder fechar los indices: sin esto
  // un indice no dice a que recurso apunta, porque el monton se reutiliza.
  void AnotaMontonDeDescriptores(ParShaders& par, uint64_t monton, uint32_t tam_descriptor) {
    par.descriptor_heap_pixel = monton;
    par.descriptor_buffer_pixel = tam_descriptor;
  }

  // Una huella barata de lo que define el MATERIAL de un dibujo del par: los shaders,
  // las constantes de luz, el estado de mezcla y color, y las texturas con su huella de
  // contenido. NO incluye los interpoladores ni el buffer de vertices, que cambian con
  // la pose en cada fotograma: si entraran, la huella cambiaria siempre y no se veria
  // nada. Lo que se busca es justo el cambio de material que no va acompanado de
  // ningun cambio en el fetch constant.
  static uint32_t HuellaDelDibujo(const SondaDibujo& d) {
    uint32_t h = 2166136261u;
    auto mezcla = [&h](uint32_t v) {
      h ^= v;
      h *= 16777619u;
    };
    mezcla(uint32_t(d.vs));
    mezcla(uint32_t(d.vs >> 32));
    mezcla(uint32_t(d.ps));
    mezcla(uint32_t(d.ps >> 32));
    mezcla(d.prim_anfitrion);
    mezcla(d.c1[0]);
    mezcla(d.c1[1]);
    mezcla(d.c1[2]);
    mezcla(d.c2[0]);
    mezcla(d.c2[1]);
    mezcla(d.c2[2]);
    mezcla(d.c162[0]);
    mezcla(d.c162[1]);
    mezcla(d.c162[2]);
    mezcla(d.c37[0]);
    mezcla(d.color_cntl);
    mezcla(d.blend0);
    mezcla(d.color_mask);
    mezcla(d.depth_cntl);
    mezcla(d.sc_mode);
    // surfInfo y colorInfo dicen a que destino se dibuja, no como es el material: el
    // mismo material se usa contra muchos destinos (la pasada de piel escribe en
    // varios), asi que si entraran la tabla se llenaria de estados que no son distintos.
    // Se imprimen igual, para poder compararlos entre dos estados.
    mezcla(d.has_ps);
    mezcla(d.mata);
    mezcla(d.escribe_color);
    mezcla(d.escribe_prof);
    // Las booleanas deciden la RAMA del shader de vertices, que es lo que determina que
    // o0 lleve UV y no otra cosa. Tienen que entrar en la huella: si cambian, el dibujo
    // es materialmente otro aunque todas las constantes de luz sean iguales.
    for (uint32_t i = 0; i < 8; ++i) {
      mezcla(d.bools[i]);
    }
    for (uint32_t i = 0; i < 4; ++i) {
      mezcla(d.bools_extra[i]);
    }
    for (uint32_t i = 0; i < d.tex_n && i < rex::countof(d.tex); ++i) {
      mezcla(d.tex[i].fc);
      mezcla(d.tex[i].base);
      mezcla(d.tex[i].mip);
      mezcla(d.tex[i].fmt);
      mezcla(d.tex[i].swizzle_host);
      mezcla(d.tex[i].signos);
      mezcla(d.tex[i].enrt);
    }
    return h;
  }

  // Imprime un dibujo entero, en una linea por cosa, para poder restar dos dibujos a
  // mano. `motivo` dice por que se imprime.
  void ImprimeDibujo(const SondaDibujo& d, const char* motivo) {
    REXLOG_WARN(
        "[SONDA-1] {} VS={:016X} PS={:016X} primVisitante={} primAnfitrion={} indices={} "
        "vertices={} mascaraVS={:X} mascaraPS={:X}",
        motivo, d.vs, d.ps, d.prim_visitante, d.prim_anfitrion, d.indices, d.vertices,
        d.mascara_vs, d.mascara_ps);
    REXLOG_WARN(
        "[SONDA-1] {} c1={:08X},{:08X},{:08X},{:08X} c2={:08X},{:08X},{:08X},{:08X} "
        "c162={:08X},{:08X},{:08X},{:08X} c37={:08X},{:08X},{:08X},{:08X}",
        motivo, d.c1[0], d.c1[1], d.c1[2], d.c1[3], d.c2[0], d.c2[1], d.c2[2], d.c2[3],
        d.c162[0], d.c162[1], d.c162[2], d.c162[3], d.c37[0], d.c37[1], d.c37[2], d.c37[3]);
    REXLOG_WARN(
        "[SONDA-1] {} colorCntl={:08X} blend0={:08X} colorMask={:08X} depthCntl={:08X} "
        "scMode={:08X} colorInfo={:08X} depthInfo={:08X} surfInfo={:08X} hayPS={} mata={} "
        "escribeColor={} escribeProf={}",
        motivo, d.color_cntl, d.blend0, d.color_mask, d.depth_cntl, d.sc_mode, d.color_info,
        d.depth_info, d.surf_info, d.has_ps, d.mata, d.escribe_color, d.escribe_prof);
    REXLOG_WARN(
        "[SONDA-1] {} interp v1=({:.4f},{:.4f},{:.4f},{:.4f}) v2=({:.4f},{:.4f},{:.4f},{:.4f}) "
        "uv=({:.4f},{:.4f},{:.4f},{:.4f})",
        motivo, rex::memory::Reinterpret<float>(d.interp_v1[0]),
        rex::memory::Reinterpret<float>(d.interp_v1[1]),
        rex::memory::Reinterpret<float>(d.interp_v1[2]),
        rex::memory::Reinterpret<float>(d.interp_v1[3]),
        rex::memory::Reinterpret<float>(d.interp_v2[0]),
        rex::memory::Reinterpret<float>(d.interp_v2[1]),
        rex::memory::Reinterpret<float>(d.interp_v2[2]),
        rex::memory::Reinterpret<float>(d.interp_v2[3]),
        rex::memory::Reinterpret<float>(d.interp_uv[0]),
        rex::memory::Reinterpret<float>(d.interp_uv[1]),
        rex::memory::Reinterpret<float>(d.interp_uv[2]),
        rex::memory::Reinterpret<float>(d.interp_uv[3]));
    REXLOG_WARN(
        "[SONDA-1] {} vfetch0 dir={:08X} largo={} tipo={} stride={} bytes={:08X},{:08X},"
        "{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
        motivo, d.vfetch0_dir, d.vfetch0_largo, d.vfetch0_tipo, d.vfetch0_stride,
        d.vfetch0_bytes[0], d.vfetch0_bytes[1], d.vfetch0_bytes[2], d.vfetch0_bytes[3],
        d.vfetch0_bytes[4], d.vfetch0_bytes[5], d.vfetch0_bytes[6], d.vfetch0_bytes[7]);
    REXLOG_WARN(
        "[SONDA-1] {} bools={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
        "bools32={:08X},{:08X},{:08X},{:08X}",
        motivo, d.bools[0], d.bools[1], d.bools[2], d.bools[3], d.bools[4], d.bools[5],
        d.bools[6], d.bools[7], d.bools_extra[0], d.bools_extra[1], d.bools_extra[2],
        d.bools_extra[3]);
    // El bloque DXT1 que se muestrea de verdad en este estado: 8 bytes crudos por
    // textura, con el texel al que apuntan las UV. Es lo que decide el color.
    if (d.bloque_ok) {
      REXLOG_WARN(
          "[SONDA-1] {} bloque fc0 texel={},{} bytes={:08X},{:08X} | fc3 texel={},{} "
          "bytes={:08X},{:08X}",
          motivo, d.bloque_uv0[0], d.bloque_uv0[1], d.bloque_fc0[0], d.bloque_fc0[1],
          d.bloque_uv3[0], d.bloque_uv3[1], d.bloque_fc3[0], d.bloque_fc3[1]);
    }
    for (uint32_t i = 0; i < d.tex_n && i < rex::countof(d.tex); ++i) {
      const SondaDibujo::Tex& t = d.tex[i];
      REXLOG_WARN(
          "[SONDA-1] {} tex fc={} base={:X} mip={:X} fmt={} wh={}x{} niveles={} swizzle={:03X} "
          "signos={:02X} enRT={} semilla={:08X} deltaMedia={} sinSigno={} conSigno={}",
          motivo, t.fc, t.base, t.mip, t.fmt, t.ancho, t.alto, t.niveles, t.swizzle_host,
          t.signos, t.enrt, t.semilla, t.delta_media, t.indice_sin_signo, t.indice_con_signo);
    }
  }

  // Rellena la ficha del dibujo y la imprime si su huella cambio respecto al dibujo
  // anterior del mismo par. Se llama en CADA dibujo del par, no una vez por informe:
  // el fallo que hay que cazar aparece y desaparece entre fotogramas con todos los
  // agregados iguales, asi que el unico sitio donde se ve es el dibujo suelto.
  void AnotaDibujo(const Shader* vs_shader, const Shader* ps_shader, const RegisterFile& regs,
                   memory::Memory* memory, D3D12TextureCache* cache, uint32_t prim,
                   uint32_t indices, uint32_t vertices, bool hay_ps, uint64_t vs, uint64_t ps,
                   const PrimitiveProcessor::ProcessingResult* resultado) {
    // Disabling reports must also stop CPU reads of GPU-written resources and
    // vertex interpretation; otherwise the probe still forces GPU readbacks.
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    ParShaders* par = nullptr;
    for (uint32_t i = 0; i < pares_usados; ++i) {
      if (pares[i].vs == vs && pares[i].ps == ps) {
        par = &pares[i];
        break;
      }
    }
    if (par == nullptr) {
      return;
    }
    SondaDibujo d;
    d.vs = vs;
    d.ps = ps;
    d.prim_visitante = prim;
    d.prim_anfitrion = resultado != nullptr ? uint32_t(resultado->host_primitive_type) : 0;
    d.indices = indices;
    d.vertices = vertices;
    d.has_ps = hay_ps ? 1 : 0;
    ++par->dibujos_del_par;
    // La parte CARA (interprete + huellas de textura) solo se hace cada kCadaCaro
    // dibujos: leer 2 KB por textura en cada dibujo serian cientos de MB/s. El estado
    // barato se lee siempre.
    const bool caro = (par->dibujos_del_par % kCadaCaro) == 1;
    if (!caro) {
      // Sin la parte cara, heredar las texturas del ultimo estado completo: si no, la
      // huella alternaria entre "con texturas" y "sin ellas" y la tabla se llenaria de
      // estados que no son distintos.
      d.tex_n = par->tex_ultimas_n;
      for (uint32_t i = 0; i < par->tex_ultimas_n && i < rex::countof(d.tex); ++i) {
        d.tex[i] = par->tex_ultimas[i];
      }
      for (uint32_t i = 0; i < 4; ++i) {
        d.interp_uv[i] = par->interp_uv_ultimo[i];
        d.interp_v1[i] = par->interp_v1_ultimo[i];
        d.interp_v2[i] = par->interp_v2_ultimo[i];
      }
    }
    if (vs_shader != nullptr) {
      d.mascara_vs = uint32_t(vs_shader->writes_interpolators());
    }
    if (ps_shader != nullptr) {
      uint32_t param_gen = 0;
      d.mascara_ps = ps_shader->GetInterpolatorInputMask(
          regs.Get<reg::SQ_PROGRAM_CNTL>(), regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen);
      d.mata = ps_shader->kills_pixels() ? 1 : 0;
      d.escribe_color = ps_shader->writes_color_targets() ? 1 : 0;
      d.escribe_prof = ps_shader->writes_depth() ? 1 : 0;
    }
    // Las constantes que el shader del luchador usa para su luz, en crudo.
    for (uint32_t i = 0; i < 4; ++i) {
      d.c1[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 1 * 4 + i];
      d.c2[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 2 * 4 + i];
      d.c162[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 162 * 4 + i];
      d.c37[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 37 * 4 + i];
    }
    d.color_cntl = regs[XE_GPU_REG_RB_COLORCONTROL];
    d.blend0 = regs[XE_GPU_REG_RB_BLENDCONTROL0];
    d.color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
    d.depth_cntl = regs[XE_GPU_REG_RB_DEPTHCONTROL];
    d.sc_mode = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    d.color_info = regs[XE_GPU_REG_RB_COLOR_INFO];
    d.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
    d.surf_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
    // Las constantes booleanas: el shader de vertices elige rama con ellas y cada rama
    // calcula o0 de otra manera. Es el unico sitio del dibujo donde puede estar la
    // diferencia entre "o0 son las UV" y "o0 es otra cosa".
    for (uint32_t i = 0; i < 8; ++i) {
      d.bools[i] = regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i];
    }
    for (uint32_t i = 0; i < 4; ++i) {
      d.bools_extra[i] = regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_032_063 + i];
    }
    // El buffer de vertices que el shader de vertices declara usar, con sus bytes: si
    // la malla sale negra, aqui se ve si los datos son los mismos que en el dibujo
    // bueno o si el juego entrego otra cosa.
    if (vs_shader != nullptr) {
      const Shader::ConstantRegisterMap& mapa = vs_shader->constant_register_map();
      for (uint32_t palabra = 0; palabra < 3; ++palabra) {
        uint32_t bits = mapa.vertex_fetch_bitmap[palabra];
        uint32_t bit;
        if (rex::bit_scan_forward(bits, &bit)) {
          const uint32_t indice = palabra * 32 + bit;
          const xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(indice);
          d.vfetch0_dir = f.address << 2;
          d.vfetch0_largo = f.size << 2;
          d.vfetch0_tipo = uint32_t(f.type);
          d.vfetch0_stride = uint32_t(f.endian);
          if (memory != nullptr) {
            const uint8_t* p = memory->TranslatePhysical(d.vfetch0_dir);
            if (p != nullptr) {
              std::memcpy(d.vfetch0_bytes, p, sizeof(d.vfetch0_bytes));
            }
          }
          break;
        }
      }
    }
    // La huella BARATA: shaders, constantes de luz, estado de mezcla y color. Se
    // calcula en todos los dibujos porque no lee memoria.
    const uint32_t huella_barata = HuellaDelDibujo(d);
    if (caro) {
      // Los interpoladores del dibujo y, por cada textura, una huella de su contenido
      // AHORA: media de los bytes y un hash barato. Si la textura cambia de un dibujo
      // al siguiente, aqui se ve; si no cambia y el cuerpo se pone negro, el fallo no
      // esta en la textura.
      SondaSalidaVertices salida;
      if (vs_shader != nullptr && memory != nullptr &&
          ShaderInterpreter::CanInterpretShader(*vs_shader)) {
        ShaderInterpreter interprete(regs, *memory);
        interprete.SetShader(*vs_shader);
        interprete.SetExportSink(&salida);
        salida.Reset();
        interprete.temp_registers()[0] = 0.0f;
        interprete.Execute();
        for (uint32_t i = 0; i < 4; ++i) {
          d.interp_uv[i] = rex::memory::Reinterpret<uint32_t>(salida.valor[0][i]);
          d.interp_v1[i] = rex::memory::Reinterpret<uint32_t>(salida.valor[1][i]);
          d.interp_v2[i] = rex::memory::Reinterpret<uint32_t>(salida.valor[2][i]);
        }
        d.interp_uv_bits[0] = salida.mascara[0];
        d.interp_v1_bits[0] = salida.mascara[1];
        d.interp_v1_bits[1] = salida.mascara[2];
        interprete.SetExportSink(nullptr);
      }
      if (ps_shader != nullptr && cache != nullptr && memory != nullptr) {
        const D3D12Shader& shader = static_cast<const D3D12Shader&>(*ps_shader);
        for (const Shader::TextureBinding& enlace : shader.texture_bindings()) {
          if (d.tex_n >= rex::countof(d.tex) || enlace.fetch_constant >= 32) {
            continue;
          }
          const xenos::xe_gpu_texture_fetch_t t = regs.GetTextureFetch(enlace.fetch_constant);
          uint32_t ancho = 0, alto = 0, base = 0, mip = 0, min_nivel = 0, max_nivel = 0;
          texture_util::GetSubresourcesFromFetchConstant(t, &ancho, &alto, nullptr, &base, &mip,
                                                         &min_nivel, &max_nivel);
          SondaDibujo::Tex& tx = d.tex[d.tex_n++];
          tx.fc = enlace.fetch_constant;
          tx.base = base;
          tx.mip = mip;
          tx.fmt = uint32_t(t.format);
          tx.ancho = ancho + 1;
          tx.alto = alto + 1;
          tx.niveles = max_nivel - min_nivel + 1;
          tx.swizzle_host = cache->GetActiveTextureHostSwizzle(enlace.fetch_constant);
          tx.signos = cache->GetActiveTextureSwizzledSigns(enlace.fetch_constant);
          tx.enrt = EnResolucionReciente(base << 12) ? 1 : 0;
          // El indice del descriptor que el dibujo consume de verdad, en las dos
          // ramas que el DXBC puede elegir con el byte de signos del sistema.
          for (const D3D12Shader::TextureBinding& sb : shader.GetTextureBindingsAfterTranslation()) {
            if (sb.fetch_constant != enlace.fetch_constant) {
              continue;
            }
            D3D12Shader::TextureBinding e_sin = sb;
            e_sin.is_signed = false;
            tx.indice_sin_signo = cache->GetActiveTextureBindlessSRVIndex(e_sin);
            D3D12Shader::TextureBinding e_con = sb;
            e_con.is_signed = true;
            tx.indice_con_signo = cache->GetActiveTextureBindlessSRVIndex(e_con);
            break;
          }
          // El bloque DXT1 que el shader muestrea con las UV de ESTE estado: tf0 usa
          // (v0.x, v0.y) y tf3 usa (v0.z, v0.w). Se envuelven con semantica de
          // repeticion. Son los 8 bytes que deciden el color: todo lo demas puede estar
          // bien y el bloque ser negro.
          if (base != 0 && uint32_t(t.format) == uint32_t(xenos::TextureFormat::k_DXT1) &&
              enlace.fetch_constant <= 3) {
            const bool es_fc3 = enlace.fetch_constant == 3;
            const float u = rex::memory::Reinterpret<float>(d.interp_uv[es_fc3 ? 2 : 0]);
            const float v = rex::memory::Reinterpret<float>(d.interp_uv[es_fc3 ? 3 : 1]);
            const uint32_t w = ancho + 1, h = alto + 1;
            const float uu = u - std::floor(u);
            const float vv = v - std::floor(v);
            const uint32_t texel_x = std::min(uint32_t(uu * float(w)), w - 1);
            const uint32_t texel_y = std::min(uint32_t(vv * float(h)), h - 1);
            const uint32_t bloque = (texel_y / 4) * (w / 4) + (texel_x / 4);
            const uint8_t* pb = memory->TranslatePhysical((base << 12) + bloque * 8);
            if (pb != nullptr) {
              std::memcpy(es_fc3 ? d.bloque_fc3 : d.bloque_fc0, pb, 8);
              uint32_t* destino_uv = es_fc3 ? d.bloque_uv3 : d.bloque_uv0;
              destino_uv[0] = texel_x;
              destino_uv[1] = texel_y;
              d.bloque_ok = 1;
            }
          }
          const uint8_t* p = base != 0 ? memory->TranslatePhysical(base << 12) : nullptr;
          if (p != nullptr) {
            uint32_t suma = 0, semilla = 2166136261u;
            for (uint32_t i = 0; i < kBytesHuella; ++i) {
              suma += p[i];
              semilla ^= p[i];
              semilla *= 16777619u;
            }
            tx.semilla = semilla;
            tx.delta_media = int32_t(suma / kBytesHuella);
          }
        }
      }
    }
    // Un ESTADO DE MATERIAL, no un dibujo: se busca en la tabla el que coincide en
    // todo lo que define el material; si no esta, se anade; si esta, solo se cuenta.
    // Asi el log no crece con los fotogramas y la tabla se puede comparar entera
    // entre dos peleas.
    const uint32_t huella = HuellaDelDibujo(d);
    if (caro) {
      par->tex_ultimas_n = d.tex_n;
      for (uint32_t i = 0; i < d.tex_n && i < rex::countof(par->tex_ultimas); ++i) {
        par->tex_ultimas[i] = d.tex[i];
      }
      // Y el seguimiento del contenido, que es lo que cambia cuando el cuerpo se pone
      // negro: media de los primeros bytes, huellas distintas y la hora del minimo.
      for (uint32_t i = 0; i < d.tex_n && i < rex::countof(d.tex); ++i) {
        const SondaDibujo::Tex& t = d.tex[i];
        if (t.base == 0 || t.semilla == 0) {
          continue;
        }
        uint32_t hueco = par->seguimiento_usado;
        for (uint32_t j = 0; j < par->seguimiento_usado; ++j) {
          if (par->seguimiento[j].base == t.base && par->seguimiento[j].fc == t.fc) {
            hueco = j;
            break;
          }
        }
        if (hueco >= rex::countof(par->seguimiento)) {
          continue;
        }
        ParShaders::SeguimientoTex& s = par->seguimiento[hueco];
        if (hueco == par->seguimiento_usado) {
          s = ParShaders::SeguimientoTex{};
          s.base = t.base;
          s.fc = t.fc;
          ++par->seguimiento_usado;
        }
        ++s.usada;
        if (t.semilla != s.huella_ultima) {
          s.huella_ultima = t.semilla;
          ++s.huellas;
        }
        if (t.delta_media < int32_t(s.media_min)) {
          s.media_min = uint32_t(t.delta_media);
          MarcaHora(s.hora_min, sizeof(s.hora_min));
        }
        if (uint32_t(t.delta_media) > s.media_max) {
          s.media_max = uint32_t(t.delta_media);
        }
        s.media_ultima = uint32_t(t.delta_media);
      }
      for (uint32_t i = 0; i < 4; ++i) {
        par->interp_uv_ultimo[i] = d.interp_uv[i];
        par->interp_v1_ultimo[i] = d.interp_v1[i];
        par->interp_v2_ultimo[i] = d.interp_v2[i];
      }
    }
    for (uint32_t i = 0; i < par->estados_usados; ++i) {
      if (par->estados[i].huella_barata == huella) {
        ++par->estados[i].veces;
        MarcaHora(par->estados[i].ultima, sizeof(par->estados[i].ultima));
        return;
      }
    }
    if (par->estados_usados < rex::countof(par->estados)) {
      d.veces = 1;
      MarcaHora(d.primera, sizeof(d.primera));
      std::memcpy(d.ultima, d.primera, sizeof(d.ultima));
      d.huella_barata = huella;
      par->estados[par->estados_usados++] = d;
    } else {
      ++par->estados_saturados;
    }
  }

  static void MarcaHora(char* destino, size_t tamano) {
    const auto ahora = std::chrono::system_clock::now();
    const std::time_t segundos = std::chrono::system_clock::to_time_t(ahora);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &segundos);
#else
    localtime_r(&segundos, &tm);
#endif
    std::snprintf(destino, tamano, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  }

  void VolcarTextura(uint32_t base_bytes, const uint8_t* origen, uint32_t bytes,
                     const xenos::xe_gpu_texture_fetch_t& t, uint32_t ancho, uint32_t alto,
                     uint32_t row_pitch_bytes = 0) {
    static std::vector<uint32_t> volcadas;
    if (origen == nullptr || bytes == 0) {
      return;
    }
    for (uint32_t b : volcadas) {
      if (b == base_bytes) {
        return;
      }
    }
    if (bytes > (32u << 20)) {
      // Un volcado gigante no se salta para siempre: se recuerda solo el tamano, no
      // la direccion, para que el proximo intento con una pagina mas pequena si se
      // guarde.
      return;
    }
    volcadas.push_back(base_bytes);

    std::vector<uint8_t> gpu(origen, origen + bytes);
    switch (t.endianness) {
      case xenos::Endian::k8in16:
        for (uint32_t i = 0; i + 1 < bytes; i += 2) {
          std::swap(gpu[i], gpu[i + 1]);
        }
        break;
      case xenos::Endian::k8in32:
        for (uint32_t i = 0; i + 3 < bytes; i += 4) {
          std::swap(gpu[i], gpu[i + 3]);
          std::swap(gpu[i + 1], gpu[i + 2]);
        }
        break;
      case xenos::Endian::k16in32:
        for (uint32_t i = 0; i + 3 < bytes; i += 4) {
          std::swap(gpu[i], gpu[i + 2]);
          std::swap(gpu[i + 1], gpu[i + 3]);
        }
        break;
      default:
        break;
    }

    const uint32_t cuatro_cc = t.format == xenos::TextureFormat::k_DXT1    ? 0x31545844u  // "DXT1"
                               : t.format == xenos::TextureFormat::k_DXT2_3 ? 0x33545844u  // "DXT3"
                               : t.format == xenos::TextureFormat::k_DXT4_5 ? 0x35545844u  // "DXT5"
                                                                            : 0u;
    uint32_t cabecera[32] = {};
    cabecera[0] = 0x20534444u;  // "DDS "
    cabecera[1] = 124;          // tamano de la cabecera
    cabecera[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;  // alto, ancho, pitch, pixelformat, linearsize
    cabecera[3] = alto;
    cabecera[4] = ancho;
    cabecera[5] = bytes;
    cabecera[8] = 1;  // mipmapCount
    cabecera[19] = 32; // tamano de DDS_PIXELFORMAT
    cabecera[20] = cuatro_cc ? 0x4u : 0x40u;  // DDPF_FOURCC / DDPF_RGB
    cabecera[21] = cuatro_cc;
    if (!cuatro_cc) {
      cabecera[22] = 32;
      cabecera[23] = 0x00FF0000u;
      cabecera[24] = 0x0000FF00u;
      cabecera[25] = 0x000000FFu;
      cabecera[26] = 0xFF000000u;
    }
    cabecera[27] = 0x1000;  // DDSCAPS_TEXTURE

    char nombre[160];
    std::snprintf(nombre, sizeof(nombre), "capturas/tex_%08X_%ux%u_f%u_t%u_fila%u.dds",
                  base_bytes, ancho, alto, uint32_t(t.format), uint32_t(t.tiled),
                  row_pitch_bytes);
    std::ofstream salida(nombre, std::ios::binary);
    if (salida) {
      salida.write(reinterpret_cast<const char*>(cabecera), sizeof(cabecera));
      salida.write(reinterpret_cast<const char*>(gpu.data()), bytes);
      salida.close();
    }

    if (t.endianness != xenos::Endian::kNone) {
      char nombre_crudo[176];
      std::snprintf(nombre_crudo, sizeof(nombre_crudo),
                    "capturas/tex_%08X_%ux%u_f%u_t%u_fila%u_crudo.dds", base_bytes, ancho, alto,
                    uint32_t(t.format), uint32_t(t.tiled), row_pitch_bytes);
      std::ofstream salida_crudo(nombre_crudo, std::ios::binary);
      if (salida_crudo) {
        salida_crudo.write(reinterpret_cast<const char*>(cabecera), sizeof(cabecera));
        salida_crudo.write(reinterpret_cast<const char*>(origen), bytes);
      }
    }
  }

  // Un buffer de vertices que el shader usa y que cae dentro de un flujo
  // memexport reciente: la malla del luchador sale de memoria que la GPU acaba
  // de escribir, asi que un fallo ahi no se ve ni en los fetch ni en las
  // texturas.
  void AnotaVfetchEnExport(const xenos::xe_gpu_vertex_fetch_t& f) {
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    ++vfetch_total;
    const uint32_t inicio = uint32_t(f.address) << 2;
    const uint32_t fin = inicio + (f.size << 2);
    for (const draw_util::MemExportRange& rango : me_recientes) {
      const uint32_t rango_inicio = rango.base_address_dwords << 2;
      if (inicio < rango_inicio + rango.size_bytes && fin > rango_inicio) {
        ++vfetch_en_export;
        vfetch_export_dir = inicio;
        vfetch_export_largo = f.size << 2;
        vfetch_export_rango = rango_inicio;
        vfetch_export_rango_largo = rango.size_bytes;
        return;
      }
    }
  }

  void AnotaCopia(uint32_t destino, uint32_t longitud) {
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    copia_destino = destino;
    copia_longitud = longitud;
    ++copia_repeticiones;
    if (swap_ptr != 0 && destino == swap_ptr) {
      ++copias_frente;
    }
    // Todas las resoluciones de render target vistas en la sesion, para poder
    // decir si una textura que el shader muestrea salio de un render target (la
    // piel y los tatuajes de un luchador creado por el jugador se componen en
    // tiempo de ejecucion) o lleva en memoria desde que se cargo el nivel. No se
    // limita a las ultimas: una textura de luchador se cuece una vez al crearlo
    // y puede muestrearse minutos despues.
    copias_recientes.insert(destino);
  }

  // Si la direccion fisica es el principio de una resolucion de render target
  // vista en esta sesion. Se compara contra el principio y no contra el rango
  // entero porque el tamano que la sonda anota no siempre es el de la textura.
  bool EnResolucionReciente(uint32_t direccion_bytes) const {
    return copias_recientes.find(direccion_bytes) != copias_recientes.end();
  }

  // Ejecuta el shader de vertices en el interprete para varios indices de vertice
  // y guarda lo que escribe en los interpoladores. El interprete lee el mismo
  // fichero de registros y la misma memoria que el shader real, asi que lo que
  // salga de aqui es lo que el shader de pixeles recibe como v0, v1 y v2.
  //
  // Se recorren VARIOS indices y no solo el cero porque el color de vertice que
  // el shader de pixeles multiplica por la textura sale de un atributo del
  // buffer de vertices: con un solo vertice no se puede saber si todos traen el
  // mismo color o si unos traen color y otros cero, y esa diferencia es
  // exactamente la que separa al luchador que se ve bien del que sale negro.
  void SondaSalidaDelShaderDeVertices(const Shader& vs, const RegisterFile& regs,
                                      memory::Memory* memory, ParShaders& par) {
    if (memory == nullptr || !ShaderInterpreter::CanInterpretShader(vs)) {
      return;
    }
    ShaderInterpreter interprete(regs, *memory);
    interprete.SetShader(vs);
    SondaSalidaVertices salida;
    interprete.SetExportSink(&salida);
    static constexpr uint32_t kVertices = 16;
    float minimo[3][4], maximo[3][4];
    double suma[3][4] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        minimo[i][j] = 1e30f;
        maximo[i][j] = -1e30f;
      }
    }
    uint32_t leidos = 0;
    for (uint32_t indice = 0; indice < kVertices; ++indice) {
      salida.Reset();
      interprete.temp_registers()[0] = float(indice);
      interprete.Execute();
      if (!salida.interpolador[0]) {
        continue;
      }
      ++leidos;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!salida.interpolador[i]) {
          continue;
        }
        for (uint32_t j = 0; j < 4; ++j) {
          const float v = salida.valor[i][j];
          minimo[i][j] = std::min(minimo[i][j], v);
          maximo[i][j] = std::max(maximo[i][j], v);
          suma[i][j] += v;
        }
      }
      if (indice == 0) {
        for (uint32_t i = 0; i < 3; ++i) {
          par.vs_salida_mascara[i] = salida.mascara[i];
          for (uint32_t j = 0; j < 4; ++j) {
            par.vs_salida[i][j] = rex::memory::Reinterpret<uint32_t>(salida.valor[i][j]);
          }
          if (salida.interpolador[i]) {
            par.vs_salida_leida |= 1u << i;
          }
        }
      }
    }
    if (leidos != 0) {
      par.v1_leidos = leidos;
      for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
          par.interp_min[i][j] = minimo[i][j];
          par.interp_max[i][j] = maximo[i][j];
          par.interp_media[i][j] = float(suma[i][j] / leidos);
        }
      }
    }
    interprete.SetExportSink(nullptr);
  }

  // Censo de las texturas que el shader de pixeles muestrea, con la misma
  // informacion que usa la cache para decidir si la textura existe. Se llama en
  // cada dibujo del par y acumula por DIRECCION BASE, no por indice de fetch
  // constant: el juego reengancha el mismo fetch constant a una textura distinta
  // para cada luchador, asi que indexar por el indice funde en una sola entrada
  // la piel que sale bien y la que sale mal y esconde justo la diferencia que
  // hay que ver.
  void AnotaTexturas(ParShaders& par, const Shader* ps_shader, const RegisterFile& regs,
                     memory::Memory* memory, const TextureCache* cache) {
    if (ps_shader == nullptr) {
      return;
    }
    for (const Shader::TextureBinding& enlace : ps_shader->texture_bindings()) {
      if (enlace.fetch_constant >= 32) {
        continue;
      }
      const xenos::xe_gpu_texture_fetch_t t = regs.GetTextureFetch(enlace.fetch_constant);
      uint32_t ancho = 0, alto = 0, prof = 0, pagina_base = 0, pagina_mip = 0;
      uint32_t mip_min = 0, mip_max = 0;
      texture_util::GetSubresourcesFromFetchConstant(t, &ancho, &alto, &prof, &pagina_base,
                                                     &pagina_mip, &mip_min, &mip_max);
      uint32_t hueco = kTexMax;
      for (uint32_t i = 0; i < par.tex_huecos; ++i) {
        if (par.tex_base[i] == pagina_base && par.tex_mip[i] == pagina_mip &&
            par.tex_wh[i][0] == ancho + 1 && par.tex_wh[i][1] == alto + 1 &&
            par.tex_fmt[i] == uint32_t(t.format) && par.tex_tiled[i] == uint32_t(t.tiled) &&
            par.tex_endian[i] == uint32_t(t.endianness)) {
          hueco = i;
          break;
        }
      }
      if (hueco == kTexMax) {
        ++par.tex_usadas;
        if (t.type != xenos::FetchConstantType::kTexture) {
          ++par.tex_malas;
        }
        if (pagina_base == 0 && pagina_mip == 0) {
          ++par.tex_sin_pagina;
        }
        if (par.tex_huecos >= kTexMax) {
          continue;
        }
        hueco = par.tex_huecos++;
        par.tex_fc[hueco] = enlace.fetch_constant;
        par.tex_tipo[hueco] = uint32_t(t.type);
        par.tex_base[hueco] = pagina_base;
        par.tex_mip[hueco] = pagina_mip;
        par.tex_fmt[hueco] = uint32_t(t.format);
        par.tex_wh[hueco][0] = ancho + 1;
        par.tex_wh[hueco][1] = alto + 1;
        par.tex_dw[hueco][0] = t.dword_0;
        par.tex_dw[hueco][1] = t.dword_1;
        par.tex_dw[hueco][2] = t.dword_2;
        par.tex_dw[hueco][3] = t.dword_3;
        par.tex_dw[hueco][4] = t.dword_4;
        par.tex_dw[hueco][5] = t.dword_5;
        // El endian con el que se carga la textura. Si la clave dice "hay que
        // canjear los bytes" y los datos ya vienen canjeados (o al reves), el
        // muestreo devuelve ruido o un valor apagado sin que falle nada.
        par.tex_endian[hueco] = uint32_t(t.endianness);
        par.tex_tiled[hueco] = uint32_t(t.tiled);
        par.tex_exp_adjust[hueco] = uint32_t(int32_t(t.exp_adjust) & 0x3F);
        par.tex_num_format[hueco] = uint32_t(t.num_format);
        par.tex_en_rt[hueco] = EnResolucionReciente(pagina_base << 12) ? 1 : 0;
        if (pagina_base != 0 && memory != nullptr) {
          const uint8_t* origen = memory->TranslatePhysical(pagina_base << 12);
          if (origen != nullptr) {
            std::memcpy(par.tex_bytes[hueco], origen, 32);
            par.tex_bytes_leidos[hueco] = 8;
            // Cuanta parte de la textura son ceros y cuantos valores distintos
            // tiene. Una textura cargada trae bytes variados; una que nunca se
            // subio es casi toda ceros, y muestrearla da negro sin que ningun
            // contador de enlaces se queje. Se mira solo el primer nivel y como
            // mucho 1 MB, y solo si cae dentro de la memoria fisica mapeada.
            const FormatInfo& fi = *FormatInfo::Get(xenos::TextureFormat(t.format));
            const uint32_t base_bytes = pagina_base << 12;
            // El tamano REAL en memoria del invitado: la misma cuenta que hace la
            // cache para pedir los rangos residentes. La fila va alineada a 256
            // bytes en las lineales, asi que "ancho*alto*bpp/8" se queda corto y el
            // volcado perdia la parte de abajo. La fila va en el nombre del fichero
            // para que un volcado corto se vea a simple vista.
            uint32_t fila = 0;
            uint32_t bytes_totales = BytesDelNivelBase(t, ancho + 1, alto + 1, &fila);
            uint32_t bytes = bytes_totales;
            if (bytes > (1u << 20)) {
              bytes = 1u << 20;
            }
            if (bytes != 0 && base_bytes + bytes <= 0x20000000u) {
              uint32_t ceros = 0;
              bool cubo[4] = {};
              for (uint32_t i = 0; i < bytes; ++i) {
                const uint8_t v = origen[i];
                if (v == 0) {
                  ++ceros;
                }
                cubo[v >> 6] = true;
              }
              par.tex_ceros[hueco] = ceros * 100 / bytes;
              par.tex_varios[hueco] =
                  (cubo[0] ? 1 : 0) + (cubo[1] ? 1 : 0) + (cubo[2] ? 1 : 0) + (cubo[3] ? 1 : 0);
              par.tex_bytes_vistos[hueco] = bytes;
              // Y la textura entera a disco, para poder mirarla: si el invitado
              // tiene una textura de verdad, el fallo esta en la carga al host;
              // si son datos sin sentido, el juego nunca la subio. Una vez por
              // direccion base, para no llenar el disco.
              if (base_bytes + bytes_totales <= 0x20000000u) {
                VolcarTextura(base_bytes, origen, bytes_totales, t, ancho + 1, alto + 1, fila);
              }
            }
          }
        }
      }
      if (hueco < kTexMax) {
        ++par.tex_dibujos[hueco];
        // Lo que la cache de texturas decidio DESPUES de pedirla. Si el enlace
        // no es valido, "GetActiveTextureHostSwizzle" devuelve
        // XE_GPU_TEXTURE_SWIZZLE_0000 (0), y el descriptor acaba siendo el "SRV
        // nulo" con FORCE_VALUE_0: el muestreo da (0,0,0,0), que es negro si el
        // color se multiplica por el y transparente si el alfa sale de ahi. Una
        // textura normal devuelve XYZW = 0x688.
        if (cache != nullptr) {
          par.tex_swizzle[hueco] = cache->GetActiveTextureHostSwizzle(enlace.fetch_constant);
          par.tex_enlace[hueco] = par.tex_swizzle[hueco] != 0 ? 1 : 0;
          par.tex_signos[hueco] = cache->GetActiveTextureSwizzledSigns(enlace.fetch_constant);
        }
      }
    }
  }

  void AnotaPar(uint64_t vs, uint64_t ps, uint64_t verts, const RegisterFile& regs,
                memory::Memory* memory, uint32_t prim, uint32_t indices,
                const Shader* vs_shader = nullptr, const Shader* ps_shader = nullptr,
                const TextureCache* cache = nullptr, uint32_t ancho_vista = 0,
                uint32_t alto_vista = 0, uint32_t muestras = 0,
                const D3D12TextureCache* cache_d3d12 = nullptr,
                const std::vector<D3D12TextureCache::TextureSRVKey>* claves_pixel = nullptr,
                uint64_t monton = 0, uint32_t tam_descriptor = 0) {
    if (REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) <= 0) {
      return;
    }
    // Temporary per-draw-class capture from this UFC build's material census.
    if (IsDiagnosticMaterial(ps) &&
        REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) > 0) {
      static std::set<std::array<uint32_t, 3>> captured_draws;
      const uint32_t base = uint32_t(regs.GetTextureFetch(0).base_address) << 12;
      if (base && captured_draws.size() < 64 &&
          captured_draws.insert({base, uint32_t(verts), indices}).second) {
        for (uint32_t index : {139u, 143u, 155u, 156u, 158u, 162u, 170u, 183u, 190u, 195u,
                               213u, 220u, 228u, 254u}) {
          const uint32_t offset = XE_GPU_REG_SHADER_CONSTANT_000_X + 1024 + index * 4;
          REXLOG_WARN("[GPU-material] VS={:016X} PS={:016X} base={:08X} vertices={} indices={} c{}={:08X},{:08X},{:08X},{:08X}",
                      vs, ps, base, verts, indices, index, regs[offset], regs[offset + 1],
                      regs[offset + 2], regs[offset + 3]);
        }
        const std::string path = fmt::format("capturas/gpu_material_{:08X}_{:016X}_v{}_i{}.bin",
                                              base, ps, verts, indices);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fwrite(regs.values, sizeof(uint32_t), RegisterFile::kRegisterCount, file);
          std::fclose(file);
        }
      }
    }
    for (uint32_t i = 0; i < pares_usados; ++i) {
      if (pares[i].vs == vs && pares[i].ps == ps) {
        ++pares[i].dibujos;
        pares[i].vertices += verts;
        AnotaTexturas(pares[i], ps_shader, regs, memory, cache);
        AnotaMuestreadores(pares[i], ps_shader, regs, cache_d3d12);
        AnotaEnlacesReales(pares[i], ps_shader, regs, const_cast<D3D12TextureCache*>(cache_d3d12),
                           claves_pixel, monton, tam_descriptor);
        return;
      }
    }
    if (pares_usados < kParesMax) {
      ParShaders& par = pares[pares_usados];
      par = ParShaders{};  // sin restos del informe anterior
      par.vs = vs;
      par.ps = ps;
      par.dibujos = 1;
      par.vertices = verts;
      par.ancho_vista = ancho_vista;
      par.alto_vista = alto_vista;
      par.muestras = muestras;
      // Lo que el analisis del microcodigo dice de estos dos shaders. Si el de
      // pixeles no escribe ningun canal de color, el dibujo se cuenta igual pero
      // no pinta nada: es la diferencia entre "se dibujo mal" y "no se pinto".
      if (ps_shader != nullptr) {
        par.ps_escribe_color = ps_shader->writes_color_targets();
        par.ps_escribe_prof = ps_shader->writes_depth() ? 1 : 0;
        par.ps_mata_pixeles = ps_shader->kills_pixels() ? 1 : 0;
        par.ps_exporta = ps_shader->memexport_eM_written();
      }
      if (vs_shader != nullptr) {
        par.vs_exporta = vs_shader->memexport_eM_written();
      }
      par.color_control = regs[XE_GPU_REG_RB_COLORCONTROL];
      draw_util::Scissor scissor;
      draw_util::GetScissor(regs, scissor, false);
      par.recorte[0] = scissor.offset[0];
      par.recorte[1] = scissor.offset[1];
      par.recorte[2] = scissor.offset[0] + scissor.extent[0];
      par.recorte[3] = scissor.offset[1] + scissor.extent[1];
      draw_util::ViewportInfo viewport;
      draw_util::GetHostViewportInfo(regs, 1, 1, false, 8192, 8192, true,
                                     draw_util::GetNormalizedDepthControl(regs), false, false,
                                     false, viewport);
      par.vista[0] = viewport.xy_extent[0];
      par.vista[1] = viewport.xy_extent[1];
      par.color_info = regs[XE_GPU_REG_RB_COLOR_INFO];
      par.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
      par.surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
      par.modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
      par.color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
      par.blend0 = regs[XE_GPU_REG_RB_BLENDCONTROL0];
      par.sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      par.clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
      par.depth_control = regs[XE_GPU_REG_RB_DEPTHCONTROL];
      par.stencil_ref_mask = regs[XE_GPU_REG_RB_STENCILREFMASK];
      par.vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
      par.vport_zscale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
      par.vport_zoffset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
      // Las constantes flotantes viven directamente en el fichero de registros.
      // Si la matriz de transformacion esta a cero, todos los vertices caen al
      // origen y la escena sale negra (es la hipotesis de xenia-canary #1093).
      for (uint32_t i = 0; i < 4; ++i) {
        par.vs_c0[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + i];
      }
      for (uint32_t i = 0; i < 256; ++i) {
        par.vs_const_no_cero += regs[XE_GPU_REG_SHADER_CONSTANT_000_X + i] != 0 ? 1 : 0;
        par.ps_const_no_cero += regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 1024 + i] != 0 ? 1 : 0;
      }
      // Cuantas constantes cree el analisis del microcodigo que usa el shader, y
      // si hay direccionamiento dinamico (c[74+a0], el esqueleto del luchador).
      // Con direccionamiento dinamico se suben las 256 y no se empaqueta; sin el,
      // solo se suben las marcadas y el orden pasa a importar.
      if (vs_shader != nullptr) {
        const Shader::ConstantRegisterMap& mapa_vs = vs_shader->constant_register_map();
        par.vs_mapa[0] = mapa_vs.float_count;
        par.vs_mapa[1] = mapa_vs.float_dynamic_addressing ? 1 : 0;
      }
      if (ps_shader != nullptr) {
        const Shader::ConstantRegisterMap& mapa_ps = ps_shader->constant_register_map();
        par.ps_mapa[0] = mapa_ps.float_count;
        par.ps_mapa[1] = mapa_ps.float_dynamic_addressing ? 1 : 0;
      }
      // Las constantes que el desensamblado del shader de pixeles nombra: c195,
      // c242, c243, c254 y c255. c254 vale 1.0 en la convencion de la consola y
      // el sombreado depende de ello; si llega a cero, el alfa sale cero y el
      // luchador se vuelve transparente, y el color se apaga.
      {
        static constexpr uint32_t kIndicesPs[5] = {195, 242, 243, 254, 255};
        for (uint32_t n = 0; n < 5; ++n) {
          const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + 1024 + kIndicesPs[n] * 4;
          for (uint32_t i = 0; i < 4; ++i) {
            par.ps_c[n][i] = regs[base + i];
          }
        }
        par.ps_c_usadas = 5;
      }
      // Y la primera matriz de hueso del shader de vertices.
      for (uint32_t n = 0; n < 4; ++n) {
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + (74 + n) * 4;
        for (uint32_t i = 0; i < 4; ++i) {
          par.vs_c74[n][i] = regs[base + i];
        }
      }
      // Que constantes del banco de vertices son cero. Un nibble hexadecimal
      // por cada cuatro constantes: bit puesto = la constante NO es cero.
      {
        static const char* kHex = "0123456789ABCDEF";
        for (uint32_t n = 0; n < 256; n += 4) {
          uint32_t nibble = 0;
          for (uint32_t j = 0; j < 4; ++j) {
            const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + (n + j) * 4;
            if (regs[base] != 0 || regs[base + 1] != 0 || regs[base + 2] != 0 ||
                regs[base + 3] != 0) {
              nibble |= 1u << j;
            }
          }
          par.vs_mapa_ceros[n >> 2] = kHex[nibble];
        }
      }
      for (uint32_t n = 0; n < 15; ++n) {
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + kIndicesVs[n] * 4;
        for (uint32_t i = 0; i < 4; ++i) {
          par.vs_c[n][i] = regs[base + i];
        }
      }
      // El fetch de vertice crudo y el formato del destino de color.
      if (par.vfetch_leidos != 0) {
        const xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(par.vfetch[0][0]);
        par.vs_vfetch_dw[0] = f.dword_0;
        par.vs_vfetch_dw[1] = f.dword_1;
      }
      par.color_info0 = regs[XE_GPU_REG_RB_COLOR_INFO];
      par.color_mask0 = regs[XE_GPU_REG_RB_COLOR_MASK];
      if (vs_shader != nullptr) {
        SondaSalidaDelShaderDeVertices(*vs_shader, regs, memory, par);
      }
      par.prim = prim;
      par.indices = indices;
      // Los fetch de vertice que el shader DECLARA usar, no los primeros que
      // esten puestos: el juego deja muchos sin usar y mirar esos es un espejismo
      // (ya costo una conclusion falsa). Se leen del mapa de constantes del
      // propio shader.
      if (vs_shader != nullptr) {
        const Shader::ConstantRegisterMap& mapa = vs_shader->constant_register_map();
        uint32_t n = 0;
        for (uint32_t palabra = 0; palabra < 3 && n < 6; ++palabra) {
          uint32_t bits = mapa.vertex_fetch_bitmap[palabra];
          uint32_t bit;
          while (rex::bit_scan_forward(bits, &bit)) {
            bits &= ~(uint32_t(1) << bit);
            if (n >= 6) {
              break;
            }
            const uint32_t indice = palabra * 32 + bit;
            xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(indice);
            par.vfetch[n][0] = indice;
            par.vfetch[n][1] = f.address;
            par.vfetch[n][2] = f.size;
            par.vfetch_tipo[n] = uint32_t(f.type);
            ++n;
          }
        }
        par.vfetch_leidos = n;
      }
      // Y los primeros dwords del primero que el shader usa, para ver si son
      // posiciones de verdad o basura (un vertice de 0,0,0,1 hace desaparecer la
      // malla).
      if (par.vfetch[0][2] != 0 && par.vfetch[0][1] != 0 && memory != nullptr) {
        const uint32_t dwords = par.vfetch[0][2] < 32 ? par.vfetch[0][2] : 32;
        const uint8_t* origen = memory->TranslatePhysical(uint32_t(par.vfetch[0][1]) << 2);
        if (origen != nullptr) {
          std::memcpy(par.vfetch_bytes, origen, dwords * 4);
          par.vfetch_bytes_leidos = dwords;
        }
      }
      ++pares_usados;
    }
  }
  // El formato que mas se uso en el intervalo, para saber a que se esta pintando.
  static void Top(const uint64_t* cuenta, char* salida, size_t tam) {
    uint32_t mejor = 0;
    for (uint32_t i = 1; i < 96; ++i) {
      if (cuenta[i] > cuenta[mejor]) {
        mejor = i;
      }
    }
    std::snprintf(salida, tam, "%02X x%llu", mejor, static_cast<unsigned long long>(cuenta[mejor]));
  }

  void Informe(memory::Memory* memory, uint32_t signos_sistema_0, uint32_t signos_sistema_1) {
    const int32_t cada_s = REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos);
    if (cada_s <= 0) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(ahora - ultimo).count() < cada_s) {
      return;
    }
    ultimo = ahora;
    char rt_mas[32], prof_mas[32];
    Top(fmt_color, rt_mas, sizeof(rt_mas));
    Top(fmt_prof, prof_mas, sizeof(prof_mas));
    // Estado del monton fisico del invitado: la sospecha (xenia-canary #1093) es
    // que el juego lo llena y luego no cabe un bloque contiguo lo bastante
    // grande. "heapMayor" es lo que decide eso, no "heapLibre".
    uint32_t heap_libre = 0, heap_mayor = 0, heap_total = 0;
    if (memory) {
      if (memory::BaseHeap* fisico = memory->LookupHeapByType(true, 0x1000)) {
        heap_libre = fisico->GetUnreservedPageCount();
        heap_mayor = fisico->GetLargestUnreservedSpanPages();
        heap_total = fisico->GetTotalPageCount();
      }
    }
    REXLOG_WARN(
        "[SONDA] dibujos={} pintados={} vertices={} swaps={} copias={} sinRT={} | sinVS={} "
        "pitch0={} sinRaster={} sinVertices={} pipelineFallido={} asyncPendiente={} "
        "bindingsFallido={} sinPS={} memexport={} | vfetchInvalido={} vfetchSinRango={} | "
        "RTmas={} profmas={} | heapLibre={}p "
        "heapMayor={}p heapTotal={}p | ultimoVS={:016X} ultimoPS={:016X}",
        total, pintados, vertices, swaps, copias, sin_rt, sin_vs, pitch0, sin_raster, sin_vertices,
        pipeline_fallido, async_pendiente, bindings_fallido, sin_ps, memexport, vfetch_invalido,
        vfetch_sin_rango, rt_mas, prof_mas,
        heap_libre, heap_mayor, heap_total, ultimo_vs, ultimo_ps);
    REXLOG_WARN(
        "[SONDA-copia] resoluciones={} alFrente={} ultimoDestino={:08X} ultimoLargo={} | swaps={} "
        "swapPtr={:08X} swap={}x{}",
        copia_repeticiones, copias_frente, copia_destino, copia_longitud, swaps, swap_ptr,
        swap_ancho, swap_alto);
    REXLOG_WARN(
        "[SONDA-memexport] dibujosConMemexport={} rangos={} bytes={} | "
        "primerFlujoBase={:08X} primerFlujoLargo={} mayor={} en={:08X}",
        memexport, memexport_rangos, memexport_bytes, me_primer_base, me_primer_largo, me_rango_max,
        me_rango_max_base);
    REXLOG_WARN(
        "[SONDA-flujo] shaders={} sinConstantes={} rechazadas={} aceptadas={} | "
        "ultimoVertice={} eM={:02X} rechazada={} dwords={:08X},{:08X},{:08X},{:08X} | "
        "aceptada={} dwords={:08X},{:08X},{:08X},{:08X}",
        me_shaders, me_sin_constantes, me_rechazadas, me_aceptadas, me_es_vertice, me_eM,
        me_constante, me_dwords[0], me_dwords[1], me_dwords[2], me_dwords[3], me_ok_constante,
        me_ok_dwords[0], me_ok_dwords[1], me_ok_dwords[2], me_ok_dwords[3]);
    REXLOG_WARN("[SONDA-flujo2] formato={} indices={} largo={}", me_ok_formato, me_ok_indices,
                me_primer_largo);
    REXLOG_WARN(
        "[SONDA-vfexp] vfetchUsados={} enMemexport={} ultimo={:08X} largo={} rango={:08X} "
        "rangoLargo={}",
        vfetch_total, vfetch_en_export, vfetch_export_dir, vfetch_export_largo,
        vfetch_export_rango, vfetch_export_rango_largo);
    // TODOS los pares, y ANTES del bucle de los cuatro mas dibujados: ese bucle pone
    // `dibujos = 0` en cada pareja que ya ha informado, asi que imprimir despues de el
    // dejaba fuera justo las parejas MAS dibujadas - que son las del escenario y, por
    // tanto, las que mas importan cuando se busca a que pareja pertenece algo.
    // ponytail: se comprobo moviendolo aqui; si algun dia vuelve a hacer falta el
    // listado, que sea antes del bucle de 4, no despues.
    for (uint32_t i = 0; i < pares_usados; ++i) {
      if (pares[i].dibujos == 0) {
        continue;
      }
      REXLOG_WARN("[SONDA-par4] VS={:016X} PS={:016X} dibujos={} vertices={} recorte={},{}-{},{} "
                  "vista={}x{}",
                  pares[i].vs, pares[i].ps, pares[i].dibujos, pares[i].vertices,
                  pares[i].recorte[0], pares[i].recorte[1], pares[i].recorte[2],
                  pares[i].recorte[3], pares[i].vista[0], pares[i].vista[1]);
      // La huella de las texturas de este par, para contar las de todos los pares y no
      // solo las del cuarto mas dibujado.
      for (uint32_t t = 0; t < pares[i].tex_huecos; ++t) {
        REXLOG_WARN("[SONDA-par5] VS={:016X} PS={:016X} fc={} base={:X} mip={:X} fmt={} wh={}x{}",
                    pares[i].vs, pares[i].ps, pares[i].tex_fc[t], pares[i].tex_base[t],
                    pares[i].tex_mip[t], pares[i].tex_fmt[t], pares[i].tex_wh[t][0],
                    pares[i].tex_wh[t][1]);
      }
    }
    for (uint32_t n = 0; n < 4 && pares_usados; ++n) {
      uint32_t mejor = UINT32_MAX;
      for (uint32_t i = 0; i < pares_usados; ++i) {
        if (pares[i].dibujos == 0) {
          continue;
        }
        if (mejor == UINT32_MAX || pares[i].dibujos > pares[mejor].dibujos) {
          mejor = i;
        }
      }
      if (mejor == UINT32_MAX) {
        break;
      }
      REXLOG_WARN(
          "[SONDA-par] VS={:016X} PS={:016X} dibujos={} vertices={} recorte={},{}-{},{} "
          "vista={}x{} constVS={}/256 constPS={}/256 c0={:08X},{:08X},{:08X},{:08X} "
          "escala={}x{} muestras={}",
          pares[mejor].vs, pares[mejor].ps, pares[mejor].dibujos, pares[mejor].vertices,
          pares[mejor].recorte[0], pares[mejor].recorte[1], pares[mejor].recorte[2],
          pares[mejor].recorte[3], pares[mejor].vista[0], pares[mejor].vista[1],
          pares[mejor].vs_const_no_cero, pares[mejor].ps_const_no_cero, pares[mejor].vs_c0[0],
          pares[mejor].vs_c0[1], pares[mejor].vs_c0[2], pares[mejor].vs_c0[3],
          pares[mejor].ancho_vista, pares[mejor].alto_vista, pares[mejor].muestras);
      REXLOG_WARN(
          "[SONDA-par2] VS={:016X} colorInfo={:08X} depthInfo={:08X} surfInfo={:08X} "
          "mode={:08X} colorMask={:08X} blend0={:08X} scMode={:08X} clip={:08X} "
          "zEsc={} zDes={} depthCntl={:08X} stencil={:08X} vte={:08X}",
          pares[mejor].vs, pares[mejor].color_info, pares[mejor].depth_info,
          pares[mejor].surface_info, pares[mejor].modecontrol, pares[mejor].color_mask,
          pares[mejor].blend0, pares[mejor].sc_mode_cntl, pares[mejor].clip_cntl,
          pares[mejor].vport_zscale, pares[mejor].vport_zoffset, pares[mejor].depth_control,
          pares[mejor].stencil_ref_mask, pares[mejor].vte_cntl);
      REXLOG_WARN(
          "[SONDA-par3] VS={:016X} psColor={} psProf={} psMata={} psExporta={} vsExporta={} "
          "colorCntl={:08X}",
          pares[mejor].vs, pares[mejor].ps_escribe_color, pares[mejor].ps_escribe_prof,
          pares[mejor].ps_mata_pixeles, pares[mejor].ps_exporta, pares[mejor].vs_exporta,
          pares[mejor].color_control);
      REXLOG_WARN(
          "[SONDA-const] VS={:016X} mapaVS={}/din={} mapaPS={}/din={} c74={:08X},{:08X},"
          "{:08X},{:08X} c75={:08X},{:08X},{:08X},{:08X}",
          pares[mejor].vs, pares[mejor].vs_mapa[0], pares[mejor].vs_mapa[1],
          pares[mejor].ps_mapa[0], pares[mejor].ps_mapa[1], pares[mejor].vs_c74[0][0],
          pares[mejor].vs_c74[0][1], pares[mejor].vs_c74[0][2], pares[mejor].vs_c74[0][3],
          pares[mejor].vs_c74[1][0], pares[mejor].vs_c74[1][1], pares[mejor].vs_c74[1][2],
          pares[mejor].vs_c74[1][3]);
      REXLOG_WARN(
          "[SONDA-const2] VS={:016X} c195={:08X},{:08X},{:08X},{:08X} c242={:08X},{:08X},"
          "{:08X},{:08X} c243={:08X},{:08X},{:08X},{:08X}",
          pares[mejor].vs, pares[mejor].ps_c[0][0], pares[mejor].ps_c[0][1],
          pares[mejor].ps_c[0][2], pares[mejor].ps_c[0][3], pares[mejor].ps_c[1][0],
          pares[mejor].ps_c[1][1], pares[mejor].ps_c[1][2], pares[mejor].ps_c[1][3],
          pares[mejor].ps_c[2][0], pares[mejor].ps_c[2][1], pares[mejor].ps_c[2][2],
          pares[mejor].ps_c[2][3]);
      REXLOG_WARN(
          "[SONDA-const3] VS={:016X} c254={:08X},{:08X},{:08X},{:08X} c255={:08X},{:08X},"
          "{:08X},{:08X}",
          pares[mejor].vs, pares[mejor].ps_c[3][0], pares[mejor].ps_c[3][1],
          pares[mejor].ps_c[3][2], pares[mejor].ps_c[3][3], pares[mejor].ps_c[4][0],
          pares[mejor].ps_c[4][1], pares[mejor].ps_c[4][2], pares[mejor].ps_c[4][3]);
      REXLOG_WARN("[SONDA-vsceros] VS={:016X} bancoVS={}", pares[mejor].vs,
                  pares[mejor].vs_mapa_ceros);
      REXLOG_WARN(
          "[SONDA-vfetch] VS={:016X} dw0={:08X} dw1={:08X} colorInfo0={:08X} colorMask0={:08X} "
          "blend0={:08X}",
          pares[mejor].vs, pares[mejor].vs_vfetch_dw[0], pares[mejor].vs_vfetch_dw[1],
          pares[mejor].color_info0, pares[mejor].color_mask0, pares[mejor].blend0);
      if (pares[mejor].v1_leidos != 0) {
        for (uint32_t i = 0; i < 3; ++i) {
          REXLOG_WARN(
              "[SONDA-v1] VS={:016X} o{} vertices={} min={:.4f},{:.4f},{:.4f},{:.4f} "
              "max={:.4f},{:.4f},{:.4f},{:.4f} media={:.4f},{:.4f},{:.4f},{:.4f}",
              pares[mejor].vs, i, pares[mejor].v1_leidos, pares[mejor].interp_min[i][0],
              pares[mejor].interp_min[i][1], pares[mejor].interp_min[i][2],
              pares[mejor].interp_min[i][3], pares[mejor].interp_max[i][0],
              pares[mejor].interp_max[i][1], pares[mejor].interp_max[i][2],
              pares[mejor].interp_max[i][3], pares[mejor].interp_media[i][0],
              pares[mejor].interp_media[i][1], pares[mejor].interp_media[i][2],
              pares[mejor].interp_media[i][3]);
        }
      }
      if (pares[mejor].vs_salida_leida != 0) {
        for (uint32_t i = 0; i < 3; ++i) {
          if (!(pares[mejor].vs_salida_leida & (1u << i))) {
            continue;
          }
          REXLOG_WARN(
              "[SONDA-vssalida] VS={:016X} o{} mascara={:X} bits={:08X},{:08X},{:08X},{:08X} "
              "valor={:.5f},{:.5f},{:.5f},{:.5f}",
              pares[mejor].vs, i, pares[mejor].vs_salida_mascara[i], pares[mejor].vs_salida[i][0],
              pares[mejor].vs_salida[i][1], pares[mejor].vs_salida[i][2],
              pares[mejor].vs_salida[i][3],
              rex::memory::Reinterpret<float>(pares[mejor].vs_salida[i][0]),
              rex::memory::Reinterpret<float>(pares[mejor].vs_salida[i][1]),
              rex::memory::Reinterpret<float>(pares[mejor].vs_salida[i][2]),
              rex::memory::Reinterpret<float>(pares[mejor].vs_salida[i][3]));
        }
      }
      for (uint32_t n = 0; n < 15; n += 3) {
        REXLOG_WARN(
            "[SONDA-vsc] VS={:016X} c{}={:08X},{:08X},{:08X},{:08X} c{}={:08X},{:08X},{:08X},"
            "{:08X} c{}={:08X},{:08X},{:08X},{:08X}",
            pares[mejor].vs, kIndicesVs[n], pares[mejor].vs_c[n][0], pares[mejor].vs_c[n][1],
            pares[mejor].vs_c[n][2], pares[mejor].vs_c[n][3], kIndicesVs[n + 1],
            pares[mejor].vs_c[n + 1][0], pares[mejor].vs_c[n + 1][1], pares[mejor].vs_c[n + 1][2],
            pares[mejor].vs_c[n + 1][3], kIndicesVs[n + 2], pares[mejor].vs_c[n + 2][0],
            pares[mejor].vs_c[n + 2][1], pares[mejor].vs_c[n + 2][2], pares[mejor].vs_c[n + 2][3]);
      }
      REXLOG_WARN(
          "[SONDA-tex] VS={:016X} PS={:016X} usadas={} distintas={} tipoMalo={} sinPagina={} "
          "gammaComoUnorm16={} signosSistema={:08X},{:08X}",
          pares[mejor].vs, pares[mejor].ps, pares[mejor].tex_usadas, pares[mejor].tex_huecos,
          pares[mejor].tex_malas, pares[mejor].tex_sin_pagina,
          REXCVAR_GET(gamma_render_target_as_unorm16) ? 1 : 0, signos_sistema_0, signos_sistema_1);
      // El enlace REAL: el indice de descriptor y la clave de textura que resuelve.
      // Es lo unico que puede cambiar sin que cambie ningun fetch constant, que es
      // justo el sintoma del cuerpo que se pone negro entre dos fotogramas.
      if (pares[mejor].enlaces_reales_usados) {
        REXLOG_WARN(
            "[SONDA-srv] VS={:016X} monton={:016X} tamDesc={} enlaces={} signosSistema={:08X}",
            pares[mejor].vs, pares[mejor].descriptor_heap_pixel,
            pares[mejor].descriptor_buffer_pixel, pares[mejor].enlaces_reales_usados,
            signos_sistema_0);
        for (uint32_t e = 0; e < pares[mejor].enlaces_reales_usados; ++e) {
          const ParShaders::EnlaceReal& r = pares[mejor].enlaces_reales[e];
          REXLOG_WARN(
              "[SONDA-srv] VS={:016X} fc={} usos={} sinSigno={} conSigno={} nuloSin={} "
              "nuloCon={} firmadoFlag={} base={:X} mip={:X} fmt={} wh={}x{} niveles={} "
              "swizzleHost={:03X} signos={:02X} vw={:08X} banderas={:08X}",
              pares[mejor].vs, r.fc, r.usos, r.indice_sin_signo, r.indice_con_signo,
              r.indice_sin_signo == UINT32_MAX ? 1 : 0, r.indice_con_signo == UINT32_MAX ? 1 : 0,
              r.es_firmado, r.base, r.mip, r.fmt, r.ancho, r.alto, r.niveles, r.swizzle_host,
              r.signos,
              r.indice_sin_signo == UINT32_MAX
                  ? 0
                  : uint32_t(pares[mejor].descriptor_heap_pixel +
                             uint64_t(r.indice_sin_signo) * pares[mejor].descriptor_buffer_pixel),
              r.banderas_clave);
        }
      }
      for (uint32_t s = 0; s < pares[mejor].muestreadores_usados; ++s) {
        const ParShaders::Muestreador& m = pares[mejor].muestreadores[s];
        REXLOG_WARN(
            "[SONDA-lod] VS={:016X} fc={} usos={} visitaMag={} visitaMin={} visitaMip={} "
            "visitaAniso={} visitaLodBias={} visitaNivelMin={} visitaNivelMax={} | host={:08X} "
            "hostMagLineal={} hostMinLineal={} hostMipLineal={} hostAniso={} hostNivelMin={} "
            "hostBaseMap={} hostClamp={}",
            pares[mejor].vs, m.fc, m.usos, m.fetch_mag, m.fetch_min, m.fetch_mip, m.fetch_aniso,
            m.fetch_lod_bias, m.fetch_mip_min, m.fetch_mip_max, m.muestreador_valor,
            m.muestreador_mag, m.muestreador_min, m.muestreador_mip, m.muestreador_aniso,
            m.muestreador_mip_min, m.muestreador_base_map, m.clamp);
      }
    // Y la tabla de estados de material del par, que es la que se compara entre la
    // pelea de luchadores normales y la del creado.
    if (pares[mejor].estados_usados || pares[mejor].estados_saturados) {
      REXLOG_WARN("[SONDA-est] VS={:016X} PS={:016X} dibujos={} estados={} saturados={}",
                  pares[mejor].vs, pares[mejor].ps, pares[mejor].dibujos_del_par,
                  pares[mejor].estados_usados, pares[mejor].estados_saturados);
      // Lo que de verdad importa: el contenido de cada textura del par a lo largo de la
      // sesion. Si una se pone negra y vuelve, aqui esta, con la hora.
      for (uint32_t s = 0; s < pares[mejor].seguimiento_usado; ++s) {
        const ParShaders::SeguimientoTex& g = pares[mejor].seguimiento[s];
        REXLOG_WARN(
            "[SONDA-est] VS={:016X} textura fc={} base={:X} medidas={} media min={} "
            "max={} ultima={} huellasDistintas={} horaMin={}",
            pares[mejor].vs, g.fc, g.base, g.usada, g.media_min, g.media_max, g.media_ultima,
            g.huellas, g.hora_min);
      }
      for (uint32_t e = 0; e < pares[mejor].estados_usados; ++e) {
        ImprimeDibujo(pares[mejor].estados[e], "ESTADO");
        REXLOG_WARN("[SONDA-est] VS={:016X} estado={} veces={} primera={} ultima={}",
                    pares[mejor].vs, e, pares[mejor].estados[e].veces,
                    pares[mejor].estados[e].primera, pares[mejor].estados[e].ultima);
      }
    }
      for (uint32_t t = 0; t < pares[mejor].tex_huecos; ++t) {
        REXLOG_WARN(
            "[SONDA-tex2] VS={:016X} fc={} dibujos={} tipo={} enRT={} base={:X} mip={:X} fmt={} "
            "wh={}x{} endian={} tiled={} expAjuste={} numFmt={} "
            "dwords={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
            pares[mejor].vs, pares[mejor].tex_fc[t], pares[mejor].tex_dibujos[t],
            pares[mejor].tex_tipo[t], pares[mejor].tex_en_rt[t], pares[mejor].tex_base[t],
            pares[mejor].tex_mip[t], pares[mejor].tex_fmt[t], pares[mejor].tex_wh[t][0],
            pares[mejor].tex_wh[t][1], pares[mejor].tex_endian[t], pares[mejor].tex_tiled[t],
            pares[mejor].tex_exp_adjust[t], pares[mejor].tex_num_format[t],
            pares[mejor].tex_dw[t][0], pares[mejor].tex_dw[t][1], pares[mejor].tex_dw[t][2],
            pares[mejor].tex_dw[t][3], pares[mejor].tex_dw[t][4], pares[mejor].tex_dw[t][5]);
        REXLOG_WARN(
            "[SONDA-tex3] VS={:016X} fc={} leidos={} enlace={} objeto={} swizzle={:08X} "
            "signos={:02X} ceros={}% valores={} vistos={} "
            "bytes={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
            pares[mejor].vs, pares[mejor].tex_fc[t], pares[mejor].tex_bytes_leidos[t],
            pares[mejor].tex_enlace[t], pares[mejor].tex_objeto[t], pares[mejor].tex_swizzle[t],
            pares[mejor].tex_signos[t], pares[mejor].tex_ceros[t], pares[mejor].tex_varios[t],
            pares[mejor].tex_bytes_vistos[t],
            pares[mejor].tex_bytes[t][0], pares[mejor].tex_bytes[t][1],
            pares[mejor].tex_bytes[t][2], pares[mejor].tex_bytes[t][3],
            pares[mejor].tex_bytes[t][4], pares[mejor].tex_bytes[t][5],
            pares[mejor].tex_bytes[t][6], pares[mejor].tex_bytes[t][7]);
      }
      REXLOG_WARN(
          "[SONDA-vert] VS={:016X} prim={} indices={} verts={} declarados={} vfetch0={}/{}/{}t{} "
          "vfetch1={}/{}/{}t{} vfetch2={}/{}/{}t{} vfetch3={}/{}/{}t{}",
          pares[mejor].vs, pares[mejor].prim, pares[mejor].indices, pares[mejor].vertices,
          pares[mejor].vfetch_leidos, pares[mejor].vfetch[0][0], pares[mejor].vfetch[0][1],
          pares[mejor].vfetch[0][2], pares[mejor].vfetch_tipo[0], pares[mejor].vfetch[1][0],
          pares[mejor].vfetch[1][1], pares[mejor].vfetch[1][2], pares[mejor].vfetch_tipo[1],
          pares[mejor].vfetch[2][0], pares[mejor].vfetch[2][1], pares[mejor].vfetch[2][2],
          pares[mejor].vfetch_tipo[2], pares[mejor].vfetch[3][0], pares[mejor].vfetch[3][1],
          pares[mejor].vfetch[3][2], pares[mejor].vfetch_tipo[3]);
      {
        std::string hex;
        for (uint32_t b = 0; b < pares[mejor].vfetch_bytes_leidos; ++b) {
          hex += fmt::format("{:08X} ", pares[mejor].vfetch_bytes[b]);
        }
        REXLOG_WARN("[SONDA-vert2] VS={:016X} bytes={} {}", pares[mejor].vs,
                    pares[mejor].vfetch_bytes_leidos, hex);
      }
      pares[mejor].dibujos = 0;
      // Y la tabla de estados se vacia: asi cada informe es una VENTANA de tiempo y se
      // pueden comparar la ventana en que el luchador sale bien con la que sale negro,
      // en vez de una tabla acumulada que mezcla las dos.
      pares[mejor].estados_usados = 0;
      pares[mejor].estados_saturados = 0;
      pares[mejor].dibujos_del_par = 0;
      pares[mejor].tex_ultimas_n = 0;
    }
    pares_usados = 0;
    total = pintados = vertices = 0;
    sin_vs = pitch0 = sin_raster = sin_vertices = 0;
    pipeline_fallido = async_pendiente = bindings_fallido = 0;
    vfetch_invalido = vfetch_sin_rango = 0;
    memexport = sin_ps = swaps = copias = sin_rt = 0;
    copia_destino = copia_longitud = copia_repeticiones = 0;
    copias_frente = 0;
    swap_ptr = swap_ancho = swap_alto = 0;
    memexport_rangos = memexport_bytes = 0;
    me_shaders = me_sin_constantes = me_rechazadas = me_aceptadas = 0;
    me_constante = me_eM = me_es_vertice = 0;
    me_dwords[0] = me_dwords[1] = me_dwords[2] = me_dwords[3] = 0;
    me_aceptadas = me_ok_constante = 0;
    me_ok_dwords[0] = me_ok_dwords[1] = me_ok_dwords[2] = me_ok_dwords[3] = 0;
    vfetch_en_export = vfetch_total = 0;
    vfetch_export_dir = vfetch_export_largo = 0;
    vfetch_export_rango = vfetch_export_rango_largo = 0;
    me_ok_formato = me_ok_indices = me_primer_base = me_primer_largo = 0;
    me_rango_max = me_rango_max_base = 0;
    me_recientes.clear();
    std::memset(fmt_color, 0, sizeof(fmt_color));
    std::memset(fmt_prof, 0, sizeof(fmt_prof));
  }
};
SondaDibujos g_sonda_dibujos;
}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h"
}  // namespace shaders

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {
  legacy_readback_memexport_cvar_name_ = "d3d12_readback_memexport";
}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslateVirtual<xenos::xe_gpu_depth_sample_counts*>(
          0xA0000000u | (sample_count_addr & 0x1FFFFFFFu));
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    return write_fallback_result();
  }

  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  UpdateDebugMarkersEnabled();

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize(draw_resolution_scale_x == 1 && draw_resolution_scale_y == 1)) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this,
      bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  primitive_processor_ =
      std::make_unique<D3D12PrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // Resolve downscale compute pipeline for scaled readback resolve.
  D3D12_ROOT_PARAMETER
  resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kConstants)];
    constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants_parameter.Constants.ShaderRegister = 0;
    constants_parameter.Constants.RegisterSpace = 0;
    constants_parameter.Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_source_range;
  resolve_downscale_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_downscale_source_range.NumDescriptors = 1;
  resolve_downscale_source_range.BaseShaderRegister = 0;
  resolve_downscale_source_range.RegisterSpace = 0;
  resolve_downscale_source_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& source_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kSource)];
    source_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    source_parameter.DescriptorTable.NumDescriptorRanges = 1;
    source_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_source_range;
    source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_destination_range;
  resolve_downscale_destination_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_downscale_destination_range.NumDescriptors = 1;
  resolve_downscale_destination_range.BaseShaderRegister = 0;
  resolve_downscale_destination_range.RegisterSpace = 0;
  resolve_downscale_destination_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& destination_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kDestination)];
    destination_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    destination_parameter.DescriptorTable.NumDescriptorRanges = 1;
    destination_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_destination_range;
    destination_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
  resolve_downscale_root_signature_desc.NumParameters = UINT(ResolveDownscaleRootParameter::kCount);
  resolve_downscale_root_signature_desc.pParameters = resolve_downscale_root_parameters;
  resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
  resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
  resolve_downscale_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, resolve_downscale_root_signature_desc);
  if (resolve_downscale_root_signature_) {
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::resolve_downscale_cs,
                                               sizeof(shaders::resolve_downscale_cs),
                                               resolve_downscale_root_signature_.Get());
  }
  if (!resolve_downscale_root_signature_ || !resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_.Reset();
    resolve_downscale_root_signature_.Reset();
    REXGPU_WARN("Failed to initialize D3D12 resolve-downscale readback pipeline");
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray: null descriptors return transparent black.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  if (!packet_memory_writes_.empty() && EndSubmission(false)) {
    CheckSubmissionFence(packet_memory_writes_.back().submission);
  }
  // Flush while the pipeline/deferred-command/readback resources still exist.
  // The worker has closed/drained its CPU-provider queue before this point.
  if (shared_memory_ && shared_memory_->HasCpuReadback()) {
    if (!shared_memory_->SynchronizeForCpu(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR("Could not preserve GPU-written memory during device shutdown");
    }
    shared_memory_->ShutdownCpuReadback();
  }
  AwaitAllQueueOperationsCompletion();
  ShutdownOcclusionQueryResources();

  ui::d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;
  for (auto& resolve_readback_pair : readback_buffers_) {
    auto& readback = resolve_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& memexport_readback_pair : memexport_readback_buffers_) {
    auto& readback = memexport_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_buffer_.Reset();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
  }
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ++g_sonda_dibujos.swaps;
  g_sonda_dibujos.swap_ptr = frontbuffer_ptr;
  g_sonda_dibujos.swap_ancho = frontbuffer_width;
  g_sonda_dibujos.swap_alto = frontbuffer_height;
  SCOPE_profile_cpu_f("gpu");

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    return;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource =
      texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                         &frontbuffer_width_unscaled, &frontbuffer_height_unscaled);
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  ++g_sonda_dibujos.total;
  g_sonda_dibujos.Informe(memory_, system_constants_.texture_swizzled_signs[0],
                          system_constants_.texture_swizzled_signs[1]);
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    ++g_sonda_dibujos.sin_vs;
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    ++g_sonda_dibujos.pitch0;
    return true;
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      ++g_sonda_dibujos.sin_raster;
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return false;
  }

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    ++g_sonda_dibujos.sin_vertices;
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Set up the render targets - this may perform dispatches and draws.
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return false;
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  D3D12Shader::D3D12Translation* vertex_shader_translation =
      static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  D3D12Shader::D3D12Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets];
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {
    ++g_sonda_dibujos.pipeline_fallido;
    return false;
  }
  if (REXCVAR_GET(async_shader_compilation) &&
      pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
    // Keep the draw. EndSubmission waits for queued PSOs before replaying the
    // deferred command list, including one-shot texture-generation passes.
    ++g_sonda_dibujos.async_pendiente;
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  if (current_guest_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  bool convert_z_to_float24 =
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  // Build a cache key from all viewport-affecting state to skip redundant
  // recalculation when the viewport registers haven't changed between draws.
  ViewportCacheKey viewport_key;
  viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
  viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  viewport_key.normalized_depth_control = normalized_depth_control.value;
  std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
              sizeof(viewport_key.vport_regs));
  viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                       (uint32_t(host_render_targets_used) << 1) | (uint32_t(ps_writes_depth) << 2);

  draw_util::ViewportInfo viewport_info;
  if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                   D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                   normalized_depth_control, convert_z_to_float24,
                                   host_render_targets_used, ps_writes_depth, viewport_info);
    previous_viewport_key_ = viewport_key;
    previous_viewport_info_ = viewport_info;
    viewport_cache_valid_ = true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);

  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // Update constant buffers, descriptors and root parameters.
  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {
    ++g_sonda_dibujos.bindings_fallido;
    return false;
  }
  // Must not call anything that can change the descriptor heap from now on!

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      // SharedMemory owns page validity. An unchanged fetch can still reference
      // CPU-written or frame-invalidated data, so always revalidate the range.
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          ++g_sonda_dibujos.vfetch_invalido;
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          ++g_sonda_dibujos.vfetch_invalido;
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      g_sonda_dibujos.AnotaVfetchEnExport(vfetch_constant);
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        ++g_sonda_dibujos.vfetch_sin_rango;
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
    }
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
    g_sonda_dibujos.AnotaStreams(regs, *vertex_shader, true);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
    g_sonda_dibujos.AnotaStreams(regs, *pixel_shader, false);
  }
  for (draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    // The stream constant's element count can be larger than what the game
    // allocated - clamping it keeps everything derived from the range inside
    // that allocation.
    draw_util::ClampMemExportRangeToAllocation(memexport_range, memory_);
    g_sonda_dibujos.AnotaMemexport(memexport_range, memory_);
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return false;
    }
  }
  if (memexport_used && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestAllAllocatedRanges()) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count, 1,
                                            0, 0);
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return false;
          }
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0, shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation = primitive_processor_->GetConvertedIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation = primitive_processor_->GetBuiltinIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize, false);
    }
    if (IsReadbackMemexportEnabled(REXCVAR_GET(d3d12_readback_memexport)) &&
        !memexport_ranges_.empty()) {
      uint32_t memexport_total_size = 0;
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        memexport_total_size += memexport_range.size_bytes;
      }
      if (memexport_total_size != 0) {
        if (REXCVAR_GET(readback_memexport_fast)) {
          IssueDraw_MemexportReadbackFastPath(memexport_total_size);
        } else {
          IssueDraw_MemexportReadbackFullPath(memexport_total_size);
        }
      }
    }
  }

  ++g_sonda_dibujos.pintados;
  g_sonda_dibujos.vertices += primitive_processing_result.host_draw_vertex_count;
  g_sonda_dibujos.memexport += memexport_used ? 1 : 0;
  g_sonda_dibujos.sin_ps += pixel_shader ? 0 : 1;
  g_sonda_dibujos.sin_rt += bound_depth_and_color_render_target_bits ? 0 : 1;
  if (bound_depth_and_color_render_target_formats[0] < 96) {
    ++g_sonda_dibujos.fmt_prof[bound_depth_and_color_render_target_formats[0]];
  }
  if (bound_depth_and_color_render_target_formats[1] < 96) {
    ++g_sonda_dibujos.fmt_color[bound_depth_and_color_render_target_formats[1]];
  }
  g_sonda_dibujos.ultimo_vs = vertex_shader->ucode_data_hash();
  // Un dibujo sin shader de pixeles tiene que contarse como tal: si no, el par
  // hereda el hash del dibujo anterior y el informe miente.
  g_sonda_dibujos.ultimo_ps = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  g_sonda_dibujos.AnotaPar(g_sonda_dibujos.ultimo_vs, g_sonda_dibujos.ultimo_ps,
                           primitive_processing_result.host_draw_vertex_count, regs, memory_,
                           uint32_t(primitive_type), index_count, vertex_shader, pixel_shader,
                           texture_cache_.get(),
                           texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1,
                           texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1,
                           uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples),
                           texture_cache_.get(),
                           (pixel_shader != nullptr &&
                            current_texture_layout_uid_pixel_ ==
                                pixel_shader->GetTextureBindingLayoutUserUID())
                               ? &current_texture_srv_keys_pixel_
                               : nullptr,
                           bindless_resources_used_ ? GetViewBindlessHeapCPUStart().ptr : 0,
                           bindless_resources_used_ ? GetD3D12Provider().GetViewDescriptorSize()
                                                    : 0);
  // Y el dibujo suelto: se imprime solo cuando su huella cambia respecto al dibujo
  // anterior del mismo par, que es el unico sitio donde se ve el fallo que aparece y
  // desaparece entre fotogramas con todos los agregados iguales.
  g_sonda_dibujos.AnotaDibujo(vertex_shader, pixel_shader, regs, memory_, texture_cache_.get(),
                              uint32_t(primitive_type), index_count,
                              primitive_processing_result.host_draw_vertex_count,
                              pixel_shader != nullptr, g_sonda_dibujos.ultimo_vs,
                              g_sonda_dibujos.ultimo_ps, &primitive_processing_result);
  // Temporary, bounded capture of the selected fighter atlas material.
  if (IsDiagnosticMaterial(g_sonda_dibujos.ultimo_ps) &&
      REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) > 0) {
    static std::array<uint32_t, 16> captured_bases{};
    const uint32_t base = uint32_t(regs.GetTextureFetch(0).base_address) << 12;
    auto slot = std::find(captured_bases.begin(), captured_bases.end(), 0u);
    if (base && slot != captured_bases.end() &&
        std::find(captured_bases.begin(), captured_bases.end(), base) == captured_bases.end() &&
        BeginSubmission(false)) {
      *slot = base;
      struct Capture {
        uint32_t fetch = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_RESOURCE_DESC description{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
        uint64_t bytes = 0;
      } captures[3];
      bool queued = false;
      const bool hair = g_sonda_dibujos.ultimo_ps == 0x2C56B11C103C5673ull;
      for (uint32_t i = 0; i < (hair ? 2u : 3u); ++i) {
        Capture& capture = captures[i];
        capture.fetch = i == 0 ? 0 : (hair || i == 2 ? 13 : 1);
        queued |= texture_cache_->QueueActiveTextureReadback(
            capture.fetch, capture.buffer, capture.description, capture.layouts, capture.bytes);
      }
      const uint64_t capture_submission = GetCurrentSubmission();
      if (queued && (!EndSubmission(false) || !AwaitAllQueueOperationsCompletion() || device_removed_)) {
        for (Capture& capture : captures) {
          if (capture.buffer) {
            resources_for_deletion_.emplace_back(capture_submission, capture.buffer.Detach());
          }
        }
        REXGPU_WARN("[GPU-host] capture failed before completion for material {:08X}", base);
      } else if (queued) {
        for (Capture& capture : captures) {
          if (!capture.buffer) {
            continue;
          }
          D3D12_RANGE range = {0, SIZE_T(capture.bytes)};
          void* mapping = nullptr;
          if (FAILED(capture.buffer->Map(0, &range, &mapping))) {
            REXGPU_WARN("[GPU-host] capture map failed for material {:08X} fc={}", base, capture.fetch);
            continue;
          }
          const std::string prefix = fmt::format("capturas/gpu_host_{:08X}_fc{}", base, capture.fetch);
          std::ofstream raw(prefix + ".bin", std::ios::binary);
          raw.write(static_cast<const char*>(mapping), std::streamsize(capture.bytes));
          std::ofstream metadata(prefix + ".json");
          metadata << fmt::format("{{\"material_base\":{},\"fetch\":{},\"format\":{},\"width\":{},"
                                  "\"height\":{},\"array_size\":{},\"mips\":{},\"subresources\":[",
                                  base, capture.fetch, uint32_t(capture.description.Format),
                                  capture.description.Width, capture.description.Height,
                                  capture.description.DepthOrArraySize, capture.description.MipLevels);
          for (size_t i = 0; i < capture.layouts.size(); ++i) {
            const auto& layout = capture.layouts[i];
            metadata << fmt::format("{}{{\"offset\":{},\"row_pitch\":{},\"width\":{},\"height\":{},"
                                    "\"depth\":{},\"format\":{}}}",
                                    i ? "," : "", layout.Offset, layout.Footprint.RowPitch,
                                    layout.Footprint.Width, layout.Footprint.Height,
                                    layout.Footprint.Depth, uint32_t(layout.Footprint.Format));
          }
          metadata << "]}";
          REXGPU_WARN("[GPU-host] material={:08X} fc={} path={} bytes={} saved={}",
                      base, capture.fetch, prefix, capture.bytes, bool(raw) && bool(metadata));
          D3D12_RANGE no_writes = {};
          capture.buffer->Unmap(0, &no_writes);
        }
      }
    }
  }
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (shared_memory_->HasCpuReadback()) {
    uint32_t first = SharedMemory::kBufferSize, end = 0;
    for (const auto& range : memexport_ranges_) {
      first = std::min(first, range.base_address_dwords << 2);
      end = std::max(end, (range.base_address_dwords << 2) + range.size_bytes);
    }
    return end <= first || shared_memory_->SynchronizeForCpu(first, end - first);
  }
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  ID3D12Resource* readback_buffer = RequestReadbackBuffer(total_size);
  if (!readback_buffer) {
    return true;
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_buffer_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback_buffer, readback_buffer_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_buffer_offset += memexport_range.size_bytes;
  }

  if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  D3D12_RANGE readback_range = {};
  readback_range.Begin = 0;
  readback_range.End = total_size;
  void* readback_mapping = nullptr;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return true;
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  D3D12_RANGE readback_write_range = {};
  readback_buffer->Unmap(0, &readback_write_range);
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (shared_memory_->HasCpuReadback()) {
    // CPU faults obtain the current generation. Publishing last-frame bytes
    // here could race a CPU store after another provider completed.
    return true;
  }
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] && readback.mapped_data[index] && size <= readback.sizes[index]) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }

    D3D12_RANGE read_range = {0, size};
    void* mapped_data = nullptr;
    if (FAILED(buffer->Map(0, &read_range, &mapped_data))) {
      buffer->Release();
      return false;
    }

    if (readback.buffers[index]) {
      if (!AwaitAllQueueOperationsCompletion()) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return false;
      }
      if (readback.mapped_data[index]) {
        readback.buffers[index]->Unmap(0, nullptr);
      }
      readback.buffers[index]->Release();
    }

    readback.buffers[index] = buffer;
    readback.mapped_data[index] = mapped_data;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback.buffers[write_index], readback_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_offset += memexport_range.size_bytes;
  }
  readback.submission_written[write_index] = submission_current_;
  readback.written_size[write_index] = total_size;

  CheckSubmissionFence(0);
  bool previous_slot_ready = readback.buffers[read_index] && readback.mapped_data[read_index] &&
                             total_size <= readback.sizes[read_index] &&
                             total_size <= readback.written_size[read_index] &&
                             readback.submission_written[read_index] &&
                             readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  const uint8_t* readback_bytes = static_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  readback.current_index = read_index;
  return true;
}

bool D3D12CommandProcessor::IssueCopy() {
  ++g_sonda_dibujos.copias;
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif
  if (!BeginSubmission(true)) {
    return false;
  }
  if (!texture_cache_->IsDrawResolutionScaled()) {
    uint32_t address, length;
    if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, address, length)) {
      return false;
    }
    g_sonda_dibujos.AnotaCopia(address, length);
    // Ordinary resolves remain on the GPU. CPU faults deliver only the dirty
    // runs they consume; an explicit full request remains synchronous.
    return GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve)) != ReadbackResolveMode::kFull ||
           shared_memory_->SynchronizeForCpu(address, length);
  }
  // Scaled output lives outside SharedMemory. Keep its immediate accurate
  // downscale path; never arm a raw SharedMemory provider for these pages.
  return IssueCopy_ReadbackResolvePath();
}

bool D3D12CommandProcessor::WriteGpuMemory(uint32_t address, uint32_t value) {
  address &= SharedMemory::kBufferSize - 1;
  if (!shared_memory_->HasCpuReadback()) {
    if (!EndSubmission(false) || !AwaitAllQueueOperationsCompletion() || device_removed_) {
      return false;
    }
    memory::store(memory_->TranslateVirtual(0xA0000000u | address), value);
    return true;
  }
  // The word is known on the CPU. Preserve any shader-generated neighbours.
  // Keep the current batch open: its fence also orders all queued word writes.
  if (!shared_memory_->SynchronizeForCpu(address, sizeof(value)) || device_removed_) {
    return false;
  }
  if (queue_operations_done_since_submission_signal_ && !submission_open_ &&
      !BeginSubmission(false)) {
    return false;
  }
  // Future GPU consumers must reload this page after the pending word has been
  // published. CPU readers may observe the old completion value until then.
  shared_memory_->MemoryInvalidationCallback(address, sizeof(value), true);
  packet_memory_writes_.push_back(
      {submission_current_ - uint64_t(!submission_open_), address, value});
  PublishPacketMemoryWrites(submission_fence_->GetCompletedValue());
  return !device_removed_;
}

void D3D12CommandProcessor::PublishPacketMemoryWrites(uint64_t completed_submission) {
  if (completed_submission == UINT64_MAX) {
    device_removed_ = true;
  }
  if (device_removed_ || publishing_packet_memory_writes_) {
    return;
  }
  publishing_packet_memory_writes_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{publishing_packet_memory_writes_};
  while (!packet_memory_writes_.empty() &&
         packet_memory_writes_.front().submission <= completed_submission) {
    const auto write = packet_memory_writes_.front();
    memory::store(memory_->TranslateVirtual(0xA0000000u | write.address), write.value);
    packet_memory_writes_.pop_front();
  }
}

bool D3D12CommandProcessor::PreparePacketMemoryReads(
    const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (publishing_packet_memory_writes_) {
    return true;
  }
  uint64_t await_submission = 0;
  const uint32_t page_mask = uint32_t(rex::memory::page_size()) - 1;
  for (const auto& write : packet_memory_writes_) {
    for (size_t i = 0; i < count; ++i) {
      // Uploads preserve whole host pages, including words outside the exact
      // shader fetch. Resolve pending words before any such page is uploaded.
      if (ranges[i].second && (write.address | page_mask) >= ranges[i].first &&
          (write.address & ~page_mask) < uint64_t(ranges[i].first) + ranges[i].second) {
        await_submission = write.submission;
        break;
      }
    }
  }
  if (await_submission) {
    CheckSubmissionFence(await_submission);
    if (submission_completed_ < await_submission || device_removed_) {
      return false;
    }
  }
  return true;
}

void D3D12CommandProcessor::PrepareForWait() {
  // Keep CPU handshakes independent of GPU completion. Wake the existing ring
  // event when a queued word is ready, including while the worker is idle.
  if (!packet_memory_writes_.empty() && EndSubmission(false)) {
    CheckSubmissionFence(0);
    if (packet_memory_writes_.empty() || device_removed_) {
      packet_wakeup_submission_ = 0;
      return;
    }
    uint64_t next_submission = packet_memory_writes_.front().submission;
    if (packet_wakeup_submission_ != next_submission) {
      if (SUCCEEDED(submission_fence_->SetEventOnCompletion(
              next_submission, static_cast<HANDLE>(write_ptr_index_event_->native_handle())))) {
        packet_wakeup_submission_ = next_submission;
      } else {
        // Preserve forward progress if the host cannot register a wakeup.
        CheckSubmissionFence(next_submission);
      }
    }
  }
}

bool D3D12CommandProcessor::ReadbackSharedMemoryForCpu(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges, std::vector<uint8_t>& data) {
  uint32_t total = 0;
  for (const auto& range : ranges) {
    if (range.first >= SharedMemory::kBufferSize ||
        range.second > SharedMemory::kBufferSize - range.first ||
        range.second > SharedMemory::kBufferSize - total) {
      return false;
    }
    total += range.second;
  }
  if (!total) {
    data.clear();
    return true;
  }
  if (!BeginSubmission(false)) {
    return false;
  }
  ID3D12Resource* buffer = RequestReadbackBuffer(total);
  if (!buffer) {
    return false;
  }
  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  uint32_t offset = 0;
  for (const auto& range : ranges) {
    deferred_command_list_.D3DCopyBufferRegion(
        buffer, offset, shared_memory_->GetBuffer(), range.first, range.second);
    offset += range.second;
  }
  if (!EndSubmission(false) || !AwaitAllQueueOperationsCompletion() || device_removed_) {
    return false;
  }
  D3D12_RANGE read_range = {0, total};
  void* mapping = nullptr;
  if (FAILED(buffer->Map(0, &read_range, &mapping))) {
    return false;
  }
  data.resize(total);
  std::memcpy(data.data(), mapping, total);
  D3D12_RANGE no_writes = {};
  buffer->Unmap(0, &no_writes);
  return true;
}

bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return false;
  }

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;

  uint32_t write_index = rb.current_index;
  uint32_t size = AlignReadbackBufferSize(written_length);

  if (size > rb.sizes[write_index]) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return false;
    }
    if (rb.buffers[write_index]) {
      if (rb.mapped_data[write_index]) {
        rb.buffers[write_index]->Unmap(0, nullptr);
        rb.mapped_data[write_index] = nullptr;
      }
      rb.buffers[write_index]->Release();
    }
    rb.buffers[write_index] = buffer;
    rb.sizes[write_index] = size;
    D3D12_RANGE read_range = {0, size};
    if (FAILED(buffer->Map(0, &read_range, &rb.mapped_data[write_index]))) {
      REXGPU_ERROR("Failed to persistently map resolve readback buffer");
      rb.mapped_data[write_index] = nullptr;
      return false;
    }
  }

  if (!rb.buffers[write_index] || !rb.mapped_data[write_index]) {
    return false;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
      return false;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return false;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return false;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return false;
    }

    uint32_t scaled_length = uint32_t(texture_cache_->GetCurrentScaledResolveRangeLengthScaled());
    uint64_t scaled_address = texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    if (!scaled_length) {
      return false;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
      ID3D12Device* device = provider.GetDevice();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, downscale_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource* buffer = nullptr;
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
              IID_PPV_ARGS(&buffer)))) {
        REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return false;
      }
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                             resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    if (!resolve_downscale_buffer_) {
      return false;
    }

    ID3D12Resource* scaled_resolve_buffer = texture_cache_->GetCurrentScaledResolveBufferResource();
    size_t scaled_resolve_buffer_index = texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
    if (!scaled_resolve_buffer) {
      return false;
    }
    uint64_t scaled_buffer_base = uint64_t(scaled_resolve_buffer_index) << 30;
    if (scaled_address < scaled_buffer_base) {
      return false;
    }
    uint64_t source_offset = scaled_address - scaled_buffer_base;

    ui::d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
    if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
      return false;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    uint32_t aligned_scaled_length =
        rex::align(scaled_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                        scaled_resolve_buffer, aligned_scaled_length,
                                        source_offset);
    uint32_t aligned_written_length =
        rex::align(written_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                        resolve_downscale_buffer_.Get(), aligned_written_length, 0);

    PushUAVBarrier(scaled_resolve_buffer);
    texture_cache_->TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();

    SetExternalPipeline(resolve_downscale_pipeline_.Get());
    deferred_command_list_.D3DSetComputeRootSignature(resolve_downscale_root_signature_.Get());
    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(ResolveDownscaleRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kSource), downscale_descriptors[0].second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kDestination), downscale_descriptors[1].second);
    deferred_command_list_.D3DDispatch(tile_count, 1, 1);

    PushUAVBarrier(resolve_downscale_buffer_.Get());
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0,
                                               resolve_downscale_buffer_.Get(), 0, written_length);
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SubmitBarriers();
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0, shared_memory_buffer,
                                               written_address, written_length);
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!EndSubmission(false) || !AwaitAllQueueOperationsCompletion() || device_removed_) {
    return false;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (!rb.buffers[read_index] || written_length > rb.sizes[read_index] ||
                           !rb.mapped_data[read_index])) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return false;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && rb.buffers[read_index] && written_length <= rb.sizes[read_index] &&
      rb.mapped_data[read_index]) {
    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, static_cast<uint8_t*>(rb.mapped_data[read_index]), written_length);
    }
  }

  rb.current_index = 1 - rb.current_index;
  return true;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  if (await_submission >= submission_current_) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_)))) {
        PROFILE_CMD_BUFFER_STALL();
        WaitForSingleObject(fence_completion_event_, INFINITE);
        queue_operations_done_since_submission_signal_ = false;
      } else {
        REXGPU_ERROR(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(
            submission_fence_->SetEventOnCompletion(await_submission, fence_completion_event_))) {
      PROFILE_CMD_BUFFER_STALL();
      WaitForSingleObject(fence_completion_event_, INFINITE);
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    REXGPU_ERROR("Failed to await a submission completion Direct3D 12 fence");
  }
  PublishPacketMemoryWrites(submission_completed_);
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(is_opening_frame ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                        : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_heap_) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    deferred_command_list_.Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    direct_queue->Signal(submission_fence_, submission_current_++);

    submission_open_ = false;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  if (REXCVAR_GET(ufc3_undisputed_nx_sonda_tinte) == 4) {
    static uint64_t checked_frame = UINT64_MAX;
    static bool enabled = false;
    if (checked_frame != frame_current_) {
      checked_frame = frame_current_;
      std::error_code error;
      enabled = std::filesystem::exists("capturas/sonda_tinte_on", error);
    }
    if (enabled) {
      flags |= DxbcShaderTranslator::kSysFlag_DiagnosticTint;
    }
  }
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_constants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
            4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              float_constants,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    if (pixel_shader && IsDiagnosticMaterial(pixel_shader->ucode_data_hash()) &&
        REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) > 0) {
      static std::array<uint32_t, 16> captured_bases{};
      const uint32_t base = uint32_t(regs.GetTextureFetch(0).base_address) << 12;
      auto slot = std::find(captured_bases.begin(), captured_bases.end(), 0u);
      if (base && slot != captured_bases.end() &&
          std::find(captured_bases.begin(), captured_bases.end(), base) == captured_bases.end()) {
        *slot = base;
        const std::string path = fmt::format("capturas/gpu_ps_constants_{:08X}.bin", base);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fwrite(float_constants - float_constant_count_pixel * 16,
                      16, float_constant_count_pixel, file);
          std::fclose(file);
        }
        REXGPU_WARN("[GPU-constants] base={:08X} count={} cb={:016X}",
                    base, float_constant_count_pixel, cbuffer_binding_float_pixel_.address);
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      if (pixel_shader && IsDiagnosticMaterial(pixel_shader->ucode_data_hash()) &&
          REXCVAR_GET(ufc3_undisputed_nx_sonda_dibujos) > 0) {
        static std::array<uint32_t, 16> captured_bases{};
        const uint32_t base = uint32_t(regs.GetTextureFetch(0).base_address) << 12;
        auto slot = std::find(captured_bases.begin(), captured_bases.end(), 0u);
        if (base && slot != captured_bases.end() &&
            std::find(captured_bases.begin(), captured_bases.end(), base) == captured_bases.end()) {
          *slot = base;
          const std::string path = fmt::format("capturas/gpu_ps_descriptors_{:08X}.bin", base);
          if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(descriptor_indices, sizeof(uint32_t), texture_count_pixel + sampler_count_pixel, file);
            std::fclose(file);
          }
          for (const auto& texture : *textures_pixel) {
            const uint32_t value = descriptor_indices[texture.bindless_descriptor_index];
            REXGPU_WARN("[GPU-descriptors] base={:08X} fc={} signed={} slot={} relative={} absolute={} cb={:016X}",
                        base, texture.fetch_constant, texture.is_signed,
                        texture.bindless_descriptor_index, value,
                        value + uint32_t(SystemBindlessView::kUnboundedSRVsStart),
                        cbuffer_binding_descriptor_indices_pixel_.address);
          }
        }
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    assert_true(current_shared_memory_binding_is_uav_.has_value());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
    if (bindless_resources_used_) {
      gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(current_shared_memory_binding_is_uav_.value()
                       ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                       : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
    } else {
      gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                       ? gpu_handle_shared_memory_uav_and_edram_
                                                       : gpu_handle_shared_memory_srv_and_edram_;
    }
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
                                                               sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
                                                               view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

void D3D12CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }
  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
        }
        readback.buffers[i]->Release();
      }
      readback.buffers[i] = nullptr;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = rex::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      resources_for_deletion_.emplace_back(GetCurrentSubmission(), readback_buffer_);
    }
    readback_buffer_ = buffer;
    readback_buffer_size_ = size;
  }
  return readback_buffer_;
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  occlusion_query_resources_available_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    uint32_t host_index = active_occlusion_query_.host_index;
    // Clear before EndSubmission to prevent the EndSubmission safety net from issuing a second
    // EndQuery for the same index.
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         host_index);
      EndSubmission(false);
    }
  } else {
    active_occlusion_query_ = {};
  }
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);

  if (!EndSubmission(false)) {
    return false;
  }

  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;
  CheckSubmissionFence(query_submission);
  if (submission_completed_ < query_submission) {
    return false;
  }
  if (!occlusion_query_readback_mapping_) {
    return false;
  }

  uint64_t samples = occlusion_query_readback_mapping_[host_index];
  samples = NormalizeOcclusionSamples(samples);
  WriteGuestOcclusionResult(sample_counts, samples);
  return true;
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void D3D12CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::d3d12
