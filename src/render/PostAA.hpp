// Post-process anti-aliasing (FXAA) at the world -> UI boundary. The client's multisampling can't
// coexist with readable scene depth (INTZ can't be multisampled), so the plan is: multisampling
// off, this on. The world image is copied into a texture and drawn back through an FXAA pixel
// shader before the UI draws, so text and icons stay crisp. Our own shader, written from the
// published FXAA algorithm (luminance edges, edge-end search, subpixel blend); no dependency.
//
// With the client's multisampling on it stays off and says why: the image is already smoothed,
// and readable depth isn't available anyway.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::postaa
{
    enum class Mode : int { Off = 0, PassThrough = 1, Fxaa = 2, ShowEdges = 3 };

    struct Settings
    {
        Mode  mode = Mode::Fxaa;
        float subpixel = 0.75f;      // 0 = edges only, 1 = softest; FXAA's usual default is 0.75
        float edgeThreshold = 0.125f; // relative contrast an edge needs (lower = more edges)
    };

    /// Subscribes the pass to OnWorldRenderEnd. Call from WXL_Load, before anything that draws
    /// debug overlays at the same point (those should stay sharp on top).
    void Install(const WXL_Api* api);
    Settings& Tunables();

    void OnDeviceLost();

    /// For the panel: why it isn't running (null = running), and last frame's cost in ms.
    const char* Inactive();
    double LastMs();

    void RegisterPanel(const WXL_Api* api);
}
