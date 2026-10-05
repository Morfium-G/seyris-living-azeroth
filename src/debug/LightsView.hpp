// Debug panel for the point lights (orchestration docs/r&d/immersion/point-lights-design.md, build
// step 1): every light the client holds in its M2 scene near the player, with the raw values, to
// confirm the traced offsets in-client before anything renders with them.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    void RegisterLightsPanel(const WXL_Api* api);
}
