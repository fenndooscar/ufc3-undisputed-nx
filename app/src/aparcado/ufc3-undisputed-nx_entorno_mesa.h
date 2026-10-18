// ufc3-undisputed-nx - environment variables for Mesa/NVK before creating Vulkan (ufc3_undisputed_nx_entorno_mesa.cpp)

#pragma once

namespace ufc3::entorno {

// Sets the variables from ufc3_undisputed_nx_mesa_entorno and, on the Switch, logs whether Mesa's on-disk shader cache
// exists and how big it is. Must be called with the toml already read and before SetupPresentation
// (OnPostInitLogging).
void AplicarEntornoMesa();

}  // namespace ufc3::entorno
