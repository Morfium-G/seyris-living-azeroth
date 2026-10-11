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
        int   steps = 48;               // the march's step budget per pixel (jumps through air + steps in the fog)
        float fogStep = 1.0f;           // yd: step length inside the fog near the camera (grows 4% of the distance)
        int   fogLights = 1;            // our lights (torches, braziers...) light the fog around them
        float lightStrength = 1.0f;     // x their light in the fog
        float lightRange = 128.0f;      // yd from the camera: lights farther away don't light the fog
        int   floorReach = 2;          // cells: the light layer starts on the lowest ground this far around (smoothed), so it doesn't drape up walls
        float dither = 0.0f;           // 0 sample each step's middle .. 1 anywhere in it, per pixel (grain instead of banding)
        float maxDistance = 250.0f;     // yd: the march ends here (and for sky pixels)
        float densityScale = 1.0f;      // multiplies every kind's density
        float noiseAmount = 0.6f;       // 0 = smooth layers .. 1 = patchy
        float noiseTileYards = 64.0f;   // size of one noise tile (the patches are ~1/8 of it)
        float brightness = 1.0f;        // multiplies the stock fog colour
        int   forceWaterDepth = 1;      // the water writes depth while there's fog to draw
        int   debugColour = 0;          // draw the fog magenta, to see where it is
        int   heightBands = 0;          // debug: paint the reconstructed world height as bands (they must stay on the terrain)
        int   depthDecode = 0;          // 0 OpenGL-style depth (-1..1), 1 D3D-style (0..1): which one keeps the bands still
        int   poolGate = 1;             // heavy pool fog only in and around cells that hold a pool (off: the old behaviour, for comparison)
        int   debugView = 0;           // 0 the fog, 1 light layer only, 2 pools only, 3 ground check (field ground vs the drawn surface)

        // Colour test (panel, not saved): these instead of FogSettings' colour columns, to find the
        // values for a place by eye before writing them into the table.
        int   colorTest = 0;
        int   colorMode = 1;            // fogsettings::ColorMode
        float color[4] = { 0.6f, 0.65f, 0.7f, 1.0f };
        float saturation = 1.0f;
        float colorBrightness = 1.0f;
    };
    Settings& Config();

    /// Hooks (from WXL_Load: they must be attached there).
    void Install(const WXL_Api* api);

    /// The renderer's parts of the fog panel: a one-line status, the main controls (always shown),
    /// the collapsed sections (look, quality, units, lights, colour test), and the diagnostics.
    void DrawStatusLine();
    void DrawMainControls();
    void DrawPanelSection();
    void DrawDiagnostics();

    void OnDeviceLost();
}
