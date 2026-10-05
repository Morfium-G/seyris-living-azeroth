// Point lights on the terrain (orchestration docs/r&d/immersion/point-lights-design.md, step 3):
// the lights of env/Lights, per pixel, in the client's own terrain pixel shaders.
//
// Pixel shader model 3 can't index constant registers, so the light list goes in a small float
// texture (2 texels per light: world position + 1 / radius, colour) read in a loop. The pixel's world
// position comes from the terrain patch's texcoord7 (features/TerrainWetness writes v0.xyz there), its
// normal from the moisture texture's red/green (env/Fields' per-cell ground normal). The block is part
// of the terrain wetness patch (the shader patcher applies one rule per shader).
//
// The client's own 3 point lights per terrain chunk are switched off while ours are on, so nothing
// counts twice: the patched terrain vertex shaders read the 3 lights' colours (c29, c32, c35) through
// temps scaled by VS c250.x (1 stock, 0 while ours are on). Disabling the device lights after the
// chunk's light setup (0x683080 after CMapChunk__SetupLights) did NOT reach the shader (owner test
// 2026-10-05). M2s, WMOs and characters keep the stock lighting.
#pragma once

#include "wxl/PluginApi.h"

#include <d3d9.h>
#include <string>

namespace wxl_livingazeroth::terrainlights
{
    /// The assembly inserted into a terrain pixel shader before its fog ending. `colour` = the register
    /// holding the colour there; needs the wetness block before it (r29 = the moisture texture uv,
    /// r31.x = the near grid's edge fade). Returns the definitions to put after the version line in `defs`.
    std::string PixelBlock(const std::string& colour, std::string& defs);

    /// Edits a terrain vertex shader so the stock point lights' colours pass through a switch (VS
    /// c250.x). Leaves variants without point lights unchanged.
    void EditVertex(std::string& source);

    /// Right before the terrain stage: the light texture and constants.
    void BeforeTerrainStage(IDirect3DDevice9* device);

    /// For the panel.
    const char* StatusLine();
}
