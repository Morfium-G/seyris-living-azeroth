// Instanced grass renderer. At long grass distances the client re-bakes most visible ground-effect
// layers on the CPU every frame (their stream buffers lose their contents), which is what makes
// 512+ yards slow. This keeps the client's placement -- the layer slot's instance records -- and
// replaces the baking and drawing:
//   - each doodad model's geometry goes into a static vertex/index buffer once,
//   - each layer slot's plants go into a static instance buffer once (rebuilt when they change),
//     each plant as its 3x4 transform, normal and colour computed exactly as the client's bake
//     computes them,
//   - the slot is drawn with hardware instancing, one draw per doodad model in it.
// The vertex shader is derived from whichever grass shader the client bound (stock or with our
// wind patch): only its position input is rewritten to come from the plant transform, so every
// output of every shadow variant stays the client's own.
//
// Plan and the disassembly it rests on: orchestration/docs/r&d/immersion/grass-instanced-renderer-plan.md
#pragma once

#include "GrassDoodads.hpp"

#include "wxl/PluginApi.h"

#include <string>

namespace wxl_livingazeroth::grassinst
{
    struct Settings
    {
        bool  enabled = true;       // runtime switch (debug panel); off = the client's own path
        float buildBudgetMs = 3.0f; // per frame; layers past it draw the client's way that frame
        // The player's density limits (instanced path only). How dense grass is comes from
        // GroundEffectDoodadDensity.cdbc (content); these cap it, like a graphics setting.
        float maxMultiplier = 4.0f;   // plants drawn per placed plant
        float maxRadius = 150.0f;     // yards around the player
        float maxSpread = 3.0f;       // yards a copy may move from its plant
        float memoryLimitMB = 256.0f; // cap on the instance pool (dynamic buffers share the 32-bit address space)
    };

    /// Attaches the engine buffer lock/unlock hooks the bake verification uses. Call from WXL_Load.
    void Install(const WXL_Api* api);
    Settings& Tunables();

    // --- from the layer-slot hooks (grassdoodads) ---

    /// Before a layer draw: when the instanced path is on and can take this slot, makes sure its
    /// instance buffer is current and returns its doodad list (entry k of the per-draw table =
    /// ids[k]). Null = draw this slot the client's way.
    const grassdoodads::SlotDoodads* Prepare(void* slot);
    /// Draws a slot Prepare accepted. False = nothing drawn; use the client's draw.
    bool Draw(void* slot);
    /// Around the client's own slot build (instance colours tagged): the bake verification.
    void BeforeStockFill(void* slot);
    void AfterStockFill(void* slot);

    /// Per grass chunk, after the client uploaded its constants: the chunk's world origin (its
    /// layers draw next). Chunk axes are world axes.
    void SetChunkOrigin(const float origin[3]);

    // --- lifecycle ---
    void OnFrameEnd();
    void OnDeviceLost();

    // --- debug panel ---
    struct Stats
    {
        unsigned slotsCached = 0;
        double   instanceMB = 0;       // in use, of ...
        unsigned poolPages = 0;        // ... this many 4 MB pages
        unsigned poolLimitPages = 0;
        bool     memoryTight = false;  // at the cap: stale layers are dropped sooner
        unsigned geometries = 0;
        double   geometryKB = 0;
        unsigned shaders = 0;          // derived instanced shaders
        unsigned densifiedLayers = 0;  // last frame: layers drawn with density copies
        unsigned densityCopies = 0;    // last frame: extra plants drawn
        // the last layer built: its chunk's area, ground effects, and how many of its doodads
        // were traced back to one of them (for checking the density context)
        bool     lastContextValid = false;
        uint32_t lastArea = 0;
        unsigned lastEffects = 0, lastDoodads = 0, lastDoodadsWithEffect = 0;
        // last frame
        unsigned slotsInstanced = 0;
        unsigned drawCalls = 0;
        unsigned builds = 0;
        double   buildMs = 0;           // all builds, of which ...
        double   buildDeviceMs = 0;     // ... creating + filling their D3D buffers
        unsigned fallbackBudget = 0;    // slot drawn stock: over the frame's build budget
        unsigned fallbackNotLoaded = 0; // slot drawn stock: a doodad model wasn't loaded yet
        unsigned fallbackFailed = 0;    // slot drawn stock: couldn't build or draw it
    };
    Stats GetStats();
    /// Why the instanced path can't run at all, or null.
    const char* Problem();

    /// Draws the next slot the client's way once and compares its baked vertices with what the
    /// instanced path computes for the same plants. The result lands in VerifyReport() and the log.
    void RequestVerify();
    bool VerifyPending();
    const std::string& VerifyReport();
}
