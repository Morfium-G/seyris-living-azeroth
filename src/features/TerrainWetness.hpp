// Wet terrain: the client's own terrain shaders, patched to darken the ground where it's wetter than
// its material's rest (env/Fields: rain, water nearby), the same way the cover's wet look does.
//
// Every terrain vertex shader passes its world XY on (texcoord7, packed into a free .zw); every terrain
// pixel shader samples the shared moisture texture (render/MoistureTexture, sampler s11) there and
// darkens its colour right before fog. Only the inserted lines are ours -- every layer count,
// shadow variant and output of the stock shaders stays as it was.
#pragma once

#include "wxl/PluginApi.h"

#include <d3d9.h>

namespace wxl_livingazeroth::terrainwet
{
    /// Registers the shader patches. From WXL_Load, before shaderpatch::Install().
    void Register();

    /// Binds the moisture texture and the patch's constants. Called right before the terrain stage
    /// draws (the cover's terrain-stage hook).
    void BeforeTerrainStage(IDirect3DDevice9* device);

    void  SetEnabled(bool enabled);
    bool  Enabled();
    /// Debug views: 0 off, 1 the whole moisture grid counts as soaked (a dark square around the
    /// player), 2 the whole terrain darkens (ignores the grid box: are the patched shaders and our
    /// constants live at all?), 3 dark stripes every 10 units of the shader's position (is it world
    /// space: fixed to the ground, north-south/east-west, not moving with the camera?).
    void  SetDebug(int mode);
    int   Debug();
    /// How much darker fully soaked ground gets (0..1; 0.45 matches the cover).
    void  SetStrength(float strength);
    float Strength();

    /// For the panel: one line per patch (applied / skipped / failed), and whether the pair is usable.
    const char* StatusLine();
}
