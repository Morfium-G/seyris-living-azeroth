// Our point lights on M2s and WMOs (orchestration docs/r&d/immersion/point-lights-design.md, "M2
// receivers"). Both are drawn through the client's shader effects (render/M2Effects), whose lit
// vertex shaders sum the sun, the ambient and up to 4 point lights per vertex. The patch adds our
// light list to that sum, per vertex like the stock lights, and can switch the stock 4 off. Its
// per-frame data (switch, count, camera, view, debug, the lights) is one texture on vertex sampler
// 0: vertex constants from c31 up can hold bone matrices.
#pragma once

#include "wxl/PluginApi.h"

#include <d3d9.h>

namespace wxl_livingazeroth::modellights
{
    /// Registers the vertex-shader patch. From WXL_Load, before shaderpatch::Install().
    void Register();

    /// Once per frame before the world draws: fills the texture from the light list and binds it.
    void Prepare(IDirect3DDevice9* device);

    /// After something else used vertex sampler 0 (the surface cover): binds the texture again.
    void Rebind(IDirect3DDevice9* device);

    /// Between frames: the header back to "stock lighting only", so whatever draws before the next
    /// Prepare (and screens without a world: login, character select) keeps the client's lighting.
    void EndFrame();

    const char* StatusLine();

    /// Debug view on lit models: 0 off, 1 our lights only, 2 count check (flat red = our count /
    /// 24), 3 world stripes (the vertex position taken back to world space; they must stay fixed
    /// while the camera turns).
    int& DebugView();

    /// Probe (debug): checks at every M2 batch draw that vertex sampler 0 holds our texture.
    /// InstallProbe from WXL_Load; ProbeLine once a frame from the panel (counts roll each second).
    int& Probe();
    void InstallProbe(const WXL_Api* api);
    const char* ProbeLine();
}
