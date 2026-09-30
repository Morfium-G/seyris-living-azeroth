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
    /// First vertex constant of the per-draw doodad table: 4 entries x 2 registers.
    constexpr unsigned kFirstReg = 104;
    constexpr unsigned kEntries = 4;

    struct DoodadInfo
    {
        uint32_t    id = 0;
        const char* modelPath = "";     // from the GroundEffectDoodad row (for the panel)
        bool        analyzed = false;   // model data was read
        bool        valid = false;      // produced a usable bend mapping
        float       uMin = 0, vMin = 0, uMax = 0, vMax = 0; // atlas rectangle
        float       rootV = 1, tipV = 0;
        float       height = 0;         // model height in model units
        bool        autoFlat = false;   // too short to sway (pebbles, shells): wind/push off by default
        bool        windOn = true;      // effective switches (auto, then live overrides)
        bool        pushOn = true;
        bool        flipped = false;    // live override: swap root and tip
        unsigned    seenInSlots = 0;
    };

    /// Attaches the fill/draw hooks. Call from WXL_Load, only when grass motion is installed.
    bool Install(const WXL_Api* api);

    /// Snapshot of every doodad seen so far, for the debug panel.
    std::vector<DoodadInfo> Seen();

    /// Live overrides from the debug panel (not saved yet).
    void SetWind(uint32_t id, bool on);
    void SetPush(uint32_t id, bool on);
    void SetFlip(uint32_t id, bool flipped);

    unsigned SlotsTracked();
    unsigned SlotsWithTooManyDoodads(); // slots mixing more than kEntries doodads (extra ones fall back)
}
