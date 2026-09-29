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
        unsigned entries = 0;        // cached cells (roof + lee)
        unsigned raysThisFrame = 0;  // traces spent this frame
        unsigned deferredThisFrame = 0; // queries answered "open" because the budget was spent
    };

    /// Once per frame, before any Openness() query: resets the ray budget and evicts entries that
    /// are stale or far from `center`.
    void BeginFrame(const float center[3], float dt);

    /// 1 = open sky, 0 = roofed over. Cached; may trace a ray within this frame's budget.
    float Openness(const float pos[3]);

    /// Wind shadow behind walls, cliffs and steep rock: 1 = fully exposed, lower = in the lee of
    /// something upwind. (windDirX, windDirY) is the direction the wind blows TOWARD. One ray per
    /// cell and 45-degree wind sector, angled slightly upward so ordinary slopes don't count.
    float Lee(const float pos[3], float windDirX, float windDirY);

    void SetLeeEnabled(bool enabled);
    bool LeeEnabled();

    /// Runtime-tunable lee shape (debug panel; not saved, defaults each launch).
    struct LeeParams
    {
        float reach    = 15.0f;  // yards upwind an obstacle still casts shadow
        float angleDeg = 15.0f;  // ray rise; only obstacles steeper than this block
        float strength = 0.85f;  // wind removed right behind an obstacle (0..1)
    };
    LeeParams GetLeeParams();
    /// Changing reach or angle re-traces the lee cache; strength applies instantly.
    void SetLeeParams(const LeeParams& params);

    void  SetEnabled(bool enabled);
    bool  Enabled();
    void  Clear();
    Stats GetStats();
}
