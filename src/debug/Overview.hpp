// The overview page (a weather station for where the player is) and this module's quick features
// for wxl-seyris-tools' quick views (Fog, Fog field, Climate, Rendering). See debug/Pages.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    /// Registers the overview page and the quick features. From WXL_Load, before the other panels
    /// (the overview is the first page).
    void RegisterOverview(const WXL_Api* api);
}
