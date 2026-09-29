// Debug view for the wind draft: a panel showing the whole chain (location -> profile rows ->
// resulting wind) and a grid of arrows drawn in the world around the player. Arrow direction and
// length show the wind; colour goes green -> yellow -> red with strength and flashes bright while a
// gust passes, so gusts are visible as waves travelling through the grid.
#pragma once

#include "../wxl_seyris/CdbcApi.hpp"

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    /// cdbc may be null (tools module missing); the Reload button then stays inert.
    void RegisterWindPanel(const WXL_Api* api);
    void SetWindCdbc(const WXL_SeyrisCdbcApi* cdbc);
}
