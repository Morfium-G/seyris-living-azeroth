// Debug panel for the snow/sand/mud research: what the client says the player stands on (unit
// TerrainType) next to what the terrain chunk under the player says (its layers' ground effects
// -> GroundEffectTexture's last column), to confirm where the client's value comes from.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    void RegisterSurfacePanel(const WXL_Api* api);
}
