// Doodads fading in at their view distance (orchestration docs/r&d/immersion/ground-fog-walkthrough.md
// section 5). The client fades a doodad in over the last few yards of its view distance; while it
// fades, its batches are drawn as see-through (alpha < 0.99999 sets the same flag as a blended
// material, element build 0x821A20), so they land in the see-through M2 pass after the ground fog and
// show unfogged until fully faded in.
//
// The view distances come from a table per size category (bounding radius < 1, < 4, < 15, < 100,
// larger): base values at 0xADF350, runtime values at 0xADF3A0 that the client rebuilds from them with
// the CVar environmentDetail (0x78F570; category 0 is never scaled: a fixed 30 yd). The fade code
// (0x791CB0) reads the runtime values for every doodad, so writing them is enough; they're checked
// every frame because the client rebuilds them when environmentDetail changes.
//
// Known limit: another mod writing the same table would fight with this one.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::doodadfade
{
    struct Settings
    {
        // Defaults (owner, 2026-10-12): on and 3. Stock's 30 yd for small doodads doesn't even reach
        // across an inn; with these no pop-in was seen, and no clear performance cost.
        int smallUseNext = 1;  // size category 0 (radius < 1: mugs, plates; fixed 30 yd) takes category 1's view distance
        int fadeFrom = 3;      // categories below this don't fade (they pop in, drawn opaque, so fogged); 0 = stock, 5 = none fade
    };
    Settings& Config();

    /// Once per frame: keep the client's runtime table at stock plus the settings.
    void Update();

    void DrawPanelSection(const WXL_Api* api);
}
