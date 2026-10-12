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

#include <cstdint>

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
        kOursFogField,    // this module: ground fog field update (env/Fog)
        kOursFogDraw,     // this module: ground fog pass
        kOursFogUpload,   // this module: ground fog textures (field, colour, top map, wake), inside the fog pass
        kOursFogLights,   // this module: our lights in the fog (per-frame light pass), inside the fog pass
        kOursCoverUpload, // this module: surface cover textures (fold + upload), inside the cover draw
        // Inside the grass pass, CPU only (see CpuOnly): their GPU time stays with the grass row.
        kDetailBake,      // the client's re-bake of a layer (stock path only)
        kOursGrassWind,   // this module: wind grid + actors, once per frame
        kOursGrassChunk,  // this module: per-chunk constants (wind grid, actors)
        kOursGrassPrep,   // this module: per layer, find/build our instances, doodad table constants
        kOursGrassBind,   // this module: per layer, the client's state flush before our draws
        kOursGrassDraw,   // this module: per layer, swap in the instanced state, draw, restore
        kCount
    };

    /// Categories entered per chunk or per layer (thousands a frame): they take CPU time but issue no
    /// GPU timestamps (the ring would overflow); GPU time between them goes to the category below.
    constexpr bool CpuOnly(int c) { return c >= kDetailBake && c <= kOursGrassDraw; }

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

    /// Draws issued straight to D3D (not through the client's Gx draw), charged to the current category.
    void CountDraws(unsigned draws, uint64_t triangles);

    void OnDeviceLost();
}
