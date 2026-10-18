#pragma once
/*
 * Periodic log lines, off the working threads.
 *
 * The log is synchronous (log_async = false): every few lines the FILE is flushed to the SD card and the
 * writing thread stalls for several ms. Every 10 s the game thread used to write 16 lines, the audio thread 3
 * and the ring thread 7 (the C2 lines of ufc3_undisputed_nx_nativo_destinos.cpp) on their own. These lines are formatted
 * on the thread that produces them and written by the "UFC3_UNDISPUTED_NX informes" thread (InformeDiferido,
 * ufc3_undisputed_nx_nativo_sistema.cpp). Warnings and errors are still written immediately, from their own thread.
 */
#include <string>

#include <rex/logging.h>

namespace ufc3::nativo {
void InformeDiferido(std::string linea);
}  // namespace ufc3::nativo

#define UFC3_UNDISPUTED_NX_INFORME_DIFERIDO(...) ::ufc3::nativo::InformeDiferido(fmt::format(__VA_ARGS__))
