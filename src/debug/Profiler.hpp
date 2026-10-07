// Frame profiler (orchestration docs/r&d/immersion/rendering-foundation.md, "First step before any of
// this: measure"): per category of the client's frame -- terrain, M2s, WMOs, liquids, particles,
// shadows, UI -- and per feature of this module, the draw calls, triangles, CPU time and GPU time.
// It decides which renderer is worth taking over, and what our additions cost.
//
// Client passes are timed by wrapping their native functions (core hook points, attached first so
// everything else on the same point is inside). The wrapper doesn't need a function's signature: it
// swaps the return address, so it fits any calling convention. Draws are counted at the client's own
// Gx draw (Gx.DeviceDraw). GPU time comes from D3D9 timestamp queries issued whenever the innermost
// category changes, read back a few frames later. Times are exclusive: a nested category's time
// isn't counted again in the one around it.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::prof
{
    enum Category : int
    {
        kOther,           // outside every category (game logic, everything unlisted)
        kWorld,           // inside the world render, not in a more specific category (sky, misc.)
        kTerrain,
        kDetail,          // detail doodads (grass), including our instanced grass renderer
        kM2,
        kM2Anim,          // the M2 scene's animate step (main-thread side)
        kParticles,
        kRibbons,
        kWmo,
        kLiquids,
        kShadows,
        kWeather,
        kUi,              // after the world: interface, overlay
        kOursEnv,         // this module: climate, regional, wind, actors, fields
        kOursLights,      // this module: light scan, grid, baking
        kOursLightUpload, // this module: light / moisture textures before the terrain
        kOursCoverFill,   // this module: surface cover update (sampling, trenches)
        kOursCoverDraw,   // this module: surface cover draw
        kOursPost,        // this module: anti-aliasing
        kCount
    };

    /// From WXL_Load: the hooks (they must be attached there), the events and the panel. Call before
    /// the post-AA subscribes, so the UI scope opens first.
    void Install(const WXL_Api* api);

    /// Manual scopes for our own work (main thread; ignored while the profiler is off).
    void Enter(int category);
    void Leave(int category);
    struct Scope
    {
        int category;
        explicit Scope(int c) : category(c) { Enter(category); }
        ~Scope() { Leave(category); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    void OnDeviceLost();
}
