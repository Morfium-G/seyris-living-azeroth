// Debug panel for the ground fog (env/Fog): the amount of each fog kind where the player stands and
// in the grid, a deposit tool (fog of a kind around the player, which then burns off by the kind's
// own rate), pause, clear, and the kinds' settings.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    void RegisterFogPanel(const WXL_Api* api);
}
