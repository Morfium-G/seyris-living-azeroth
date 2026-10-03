// Debug panel for the climate and the world fields: the client's clock, the AreaClimate row and
// the temperature where the player stands, the ground moisture there and why, and test overrides
// (weather, time of day).
#pragma once

#include "wxl/PluginApi.h"

struct WXL_SeyrisCdbcApi;

namespace wxl_livingazeroth::debug
{
    void SetClimateCdbc(const WXL_SeyrisCdbcApi* cdbc);
    void RegisterClimatePanel(const WXL_Api* api);
}
