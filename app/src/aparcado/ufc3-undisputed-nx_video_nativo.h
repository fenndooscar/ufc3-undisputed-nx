// ufc3-undisputed-nx - cinematicas WMV3 descodificadas con FFmpeg (ver ufc3_undisputed_nx_video_nativo.cpp)

#pragma once

#include <cstdint>

namespace ufc3::video_nativo {

// Movie frames decoded with FFmpeg since startup. The hang watchdog (ufc3_undisputed_nx_app.h) mixes it into its
// signature: during a cutscene the game threads spend almost all their time waiting in the same place
// and their registers do not change even though the video advances.
uint64_t FotogramasNativos();

}  // namespace ufc3::video_nativo
