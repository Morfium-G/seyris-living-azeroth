// "Is this point roofed over?" -- per position, independent of where the player is. One ray straight
// up: anything above within kRoofSearch (a WMO roof or ceiling, or terrain when the point is
// underground) means sheltered. That is what wind -- and later rain and puddles -- actually care
// about, and unlike the client's indoor verdict it works for any point, not just objects.
//
// Rays are cached per 4-yard cell and 4-yard height band around the player, traced within a per-frame
// budget, and re-checked periodically (a WMO that had not streamed in yet reads as open sky).
#pragma once

namespace wxl_livingazeroth::shelter
{
    struct Stats
    {
        unsigned entries = 0;        // cached cells
        unsigned raysThisFrame = 0;  // traces spent this frame
        unsigned deferredThisFrame = 0; // queries answered "open" because the budget was spent
    };

    /// Once per frame, before any Openness() query: resets the ray budget and evicts entries that
    /// are stale or far from `center`.
    void BeginFrame(const float center[3], float dt);

    /// 1 = open sky, 0 = roofed over. Cached; may trace a ray within this frame's budget.
    float Openness(const float pos[3]);

    void  SetEnabled(bool enabled);
    bool  Enabled();
    void  Clear();
    Stats GetStats();
}
