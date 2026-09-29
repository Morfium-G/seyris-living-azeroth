// Debug view for the readable scene depth: a panel with the swap's status, and an optional overlay
// that draws the depth as a greyscale image in a corner of the screen (the plugin UI has no image
// widget, so it's drawn straight into the frame at the world -> UI boundary).
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    void RegisterPanel(const WXL_Api* api);

    /// OnWorldSceneEnd: the world's projection is still current; remembers what linearizing needs,
    /// and whether the world really drew into our depth surface.
    void OnWorldSceneEnd(const void* args);

    /// OnWorldRenderEnd: draws the overlay when it's switched on.
    void OnWorldRenderEnd(const void* args);
}
