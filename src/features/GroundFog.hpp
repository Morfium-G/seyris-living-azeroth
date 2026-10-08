// Ground fog: the renderer (orchestration docs/r&d/immersion/ground-fog-design.md). Draws the fog
// field (env/Fog) over the world as one full-screen pass at the entry of the weather render: after
// the water (so lakes are fogged by their surface), before the rain and the see-through models (so
// they stay on top). Per pixel it marches the view ray to the scene depth (or a distance cap for the
// sky) through the field: density per yd = the kinds' amounts x their density, falling off with the
// height above the ground over the kinds' layer thickness, broken up by a drifting noise volume.
// Coloured with the zone's stock fog colour; blended dst x transmittance + fog colour x (1 - T).
//
// While there's fog to draw, the water writes depth (render/GxState, per water draw), so the fog
// reads the water surface instead of the lake bed. Stays off (and says why) without readable depth,
// with the camera under water, or when a mod that draws its own fog or water is loaded.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::groundfog
{
    struct Settings
    {
        int   enabled = 1;
        int   steps = 16;               // march steps per pixel
        float maxDistance = 250.0f;     // yd: the march ends here (and for sky pixels)
        float densityScale = 1.0f;      // multiplies every kind's density
        float noiseAmount = 0.6f;       // 0 = smooth layers .. 1 = patchy
        float noiseTileYards = 64.0f;   // size of one noise tile (the patches are ~1/8 of it)
        float brightness = 1.0f;        // multiplies the stock fog colour
        int   forceWaterDepth = 1;      // the water writes depth while there's fog to draw
        int   debugColour = 0;          // draw the fog magenta, to see where it is
    };
    Settings& Config();

    /// Hooks (from WXL_Load: they must be attached there).
    void Install(const WXL_Api* api);

    /// The renderer's part of the fog panel.
    void DrawPanelSection();

    void OnDeviceLost();
}
