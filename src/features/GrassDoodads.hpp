// Per-doodad grass settings. Ground-effect doodads usually share one atlas texture, so one grass
// draw can mix grass, flowers and pebbles, and their texture layouts differ. This works out, for
// every GroundEffectDoodad, where its model sits in the atlas and which end of it is the root --
// straight from the model geometry the client builds grass from -- and hands each layer draw a
// small table of its doodads so the shader bends every vertex by its own doodad's rules.
//
// Client layout (disassembly, orchestration/docs/r&d/immersion/ground-effects.md):
//   layer slot: +0x18 instance count, +0x1C instances (0x2C bytes, +0x04 = GroundEffectDoodad ID)
//   doodad table 0xD1C4FC[id] = { +0 dbc row, +4 CM2Model }
//   model +0x2C -> shared: +0x150 header (+0x40 vertices, 48 B), +0x170 skin (+4 count, +8 u16 idx)
#pragma once

#include "wxl/PluginApi.h"

#include <cstdint>
#include <vector>

namespace wxl_livingazeroth::grassdoodads
{
    /// Per-draw doodad table: 32 entries x 1 register {rootV, 1/(tipV-rootV), windScale, pushScale}
    /// at c104..c135. Entries 0..30 are the slot's doodads; entry 31 is the fallback (old UV rule).
    /// A slot can hold more than one terrain layer's doodads (layers sharing an atlas texture draw
    /// together), so up to 16 is expected. Each vertex carries its entry index in its colour: the
    /// low 3 bits in green's lowest 3 bits, the high 2 bits in BOTH red's and blue's lowest 2 bits
    /// (so a red/blue swap for the GPU format doesn't matter). Written into the instance colours
    /// just before the client builds the slot's vertices, restored right after.
    constexpr unsigned kFirstReg = 104;
    constexpr unsigned kEntries = 32;
    constexpr unsigned kDoodadEntries = kEntries - 1;
    constexpr unsigned kFallbackEntry = kEntries - 1;

    struct DoodadInfo
    {
        uint32_t    id = 0;
        const char* modelPath = "";     // from the GroundEffectDoodad row (for the panel)
        bool        analyzed = false;   // model data was read
        bool        valid = false;      // produced a usable bend mapping
        float       uMin = 0, vMin = 0, uMax = 0, vMax = 0; // atlas rectangle
        float       rootV = 1, tipV = 0; // automatic, from the model geometry
        float       height = 0;         // model height in model units
        bool        autoFlat = false;   // too short to sway (pebbles, shells): wind/push off by default
        // Effective values: automatic, or the override row when there is one.
        bool        windOn = true;
        bool        pushOn = true;
        bool        flipped = false;
        float       stiffness = 0.0f;
        float       effRootV = 1, effTipV = 0;
        bool        hasOverride = false;
        unsigned    seenInSlots = 0;
    };

    /// One GroundEffectDoodadWind.cdbc row. Having a row means "use these values instead of the
    /// automatic ones" for that doodad.
    enum OverrideFlags : uint32_t
    {
        kNoWind      = 0x01,
        kNoPush      = 0x02,
        kDroopWet    = 0x04, // reserved
        kWaterCurrent = 0x08, // reserved
        kFlip        = 0x10,
    };
    struct Override
    {
        uint32_t flags = 0;
        float    stiffness = 0.0f; // 0 flexible .. 1 rigid
        float    rootV = -1.0f;    // -1 = automatic
        float    tipV = -1.0f;
    };

    /// Attaches the fill/draw hooks. Call from WXL_Load, only when grass motion is installed.
    bool Install(const WXL_Api* api);

    /// Snapshot of every doodad seen so far, for the debug panel.
    std::vector<DoodadInfo> Seen();

    /// Override rows: set/clear live (debug panel), load from and save to
    /// DBFilesClient\GroundEffectDoodadWind.cdbc.
    void SetOverride(uint32_t id, const Override& o);
    void ClearOverride(uint32_t id);
    bool GetOverride(uint32_t id, Override& out);
    /// Effective values of a doodad as an override row (a starting point for editing).
    Override CurrentAsOverride(uint32_t id);

    /// Loads the overrides file (missing file = no overrides). cdbc may be null.
    void LoadOverrides(const void* cdbcApi);
    /// Writes every override row to the file (a .bak of the previous file is kept). Returns false
    /// with a reason in `message` on failure.
    bool SaveOverrides(char* message, size_t messageSize);
    unsigned OverrideCount();

    /// Debug: lift every instance of one doodad into the air so it can be found in the world.
    /// 0 = none.
    void     SetHighlight(uint32_t id);
    uint32_t Highlighted();
    /// SetHighlight value that lifts everything drawn with the fallback entry (untagged/overflow).
    constexpr uint32_t kHighlightFallbackId = 0xFFFFFFFFu;

    /// Layer draws since the last call whose slot build we saw (tracked) or not (untracked).
    void FrameCounters(unsigned& tracked, unsigned& untracked);

    unsigned SlotsTracked();
    unsigned SlotsWithTooManyDoodads(); // slots mixing more than kEntries doodads (extra ones fall back)
}
