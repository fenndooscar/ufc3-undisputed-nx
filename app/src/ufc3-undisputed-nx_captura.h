// ufc3-undisputed-nx - screenshots of the emulated frame, taken from inside the process.
//
// THE PROBLEM IT SOLVES
//   The port is tested with its window minimised, and no Windows API can read the
//   pixels of a minimised DXGI flip-model swap chain: PrintWindow with
//   PW_RENDERFULLCONTENT succeeds and returns a black client area (measured:
//   brightness 0.2 of 255, 99.5% pure black), and a screen BitBlt sees whatever is
//   on top of the window instead of the game. The only copy of the frame that
//   always exists is the one the emulated GPU has just finished, and the SDK
//   already exposes it as ui::Presenter::CaptureGuestOutput.
//
// WHAT IT DOES
//   A thread of its own asks the presenter for the latest guest output every
//   ufc3_undisputed_nx_captura_cada_s seconds and writes it as a BMP. Nothing about
//   the window matters - not focus, not size, not visibility - because the image
//   never comes from the screen.
//
// HOW TO TURN IT OFF WITHOUT A REBUILD
//   ufc3_undisputed_nx_captura_cada_s = 0 (the default) never starts the thread.
//   The files land in capturas/ next to the executable, named
//   captura_NNNN_tSSS.bmp: NNNN the index, SSS the seconds since start. BMP because
//   the SDK has no PNG encoder and this is a diagnostic, not a feature;
//   out/capturas.py converts them with Pillow.

#pragma once

namespace rex::system {
class IGraphicsSystem;
}

namespace ufc3::captura {

// Starts the capture thread if ufc3_undisputed_nx_captura_cada_s > 0 and the
// graphics system has a presenter. Safe to call with nullptr, and more than once
// (only the first call does anything).
void Arrancar(rex::system::IGraphicsSystem* graficos);

// Stops the thread and waits for it (idempotent).
void Parar();

}  // namespace ufc3::captura
