// This module's overlay UI, routed through wxl-seyris-tools' control panel ("seyris.controls": one
// page window, quick views) when it's there, else as plain overlay panels. Panels and quick features
// are collected from WXL_Load and handed over on the first frame (the interface can't be resolved in
// WXL_Load: extensions load in name order, and wxl-seyris-tools loads after this module).
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::pages
{
    constexpr const char* kGroup = "Living Azeroth";

    /// A full panel. `title` is the overlay panel's title ("wxl-seyris-living-azeroth: fog"); as a
    /// page it's shown without the module prefix ("fog"). Strings must outlive the module (literals).
    void AddPanel(const char* title, WXL_PanelFn draw);

    /// A quick feature for the quick views ("Fog", "Climate", ...): a short strip of controls. Only
    /// shown when wxl-seyris-tools is there.
    void AddQuick(const char* name, WXL_PanelFn draw);

    /// First frame: hands everything to the control panel, or registers the panels with the overlay.
    void Publish(const WXL_Api* api);
}
