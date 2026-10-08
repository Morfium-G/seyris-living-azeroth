// Ground fog: the field (orchestration docs/r&d/immersion/ground-fog-design.md). One fog system with
// several fog kinds; per kind, an amount (0 none .. 1 its full density) on a grid around the player,
// on the moisture grid's footprint (128 x 128 cells of 2 yd, +-128 yd).
//
// Each kind's amount moves toward a target per cell: up at its build-up rate, down at its burn-off
// rate (half-lives, so nothing switches on or off). The targets come from the kind's rules (where
// fog forms: moisture, temperature, water, wind, time of day); until those exist every target is 0,
// so fog is only there when deposited (the debug panel) and burns off from there.
//
// The renderer (features/GroundFog) reads Grid(): per kind, the amounts plus the kind's layer
// thickness and density.
#pragma once

#include "WorldQuery.hpp"

#include <cstdint>

namespace wxl_livingazeroth::fog
{
    enum Kind : int
    {
        kMist,      // radiation mist: cool, calm, humid nights; pools in low ground
        kWaterFog,  // over water warmer than the air
        kSteam,     // rain or snow on hot ground, hot springs: thin, short-lived
        kAreaFog,   // a place's own fog (swamps, haunted forests)
        kHaze,      // rain and fog weather: a raised base everywhere
        kKindCount
    };
    const char* KindName(int kind);

    /// Per kind, tunable from the panel (not saved yet; FogSettings.cdbc later).
    struct KindSettings
    {
        int   enabled = 1;                // drawn (and, once they exist, its rules run); its amounts still burn off
        float buildUpSeconds = 120.0f;  // half-life of the gap to a higher target (used once targets exist)
        float burnOffSeconds = 600.0f;  // half-life of the gap to a lower target
        float thickness = 4.0f;         // yd: the layer's height above the ground at full amount
        float density = 0.05f;          // extinction per yd at full amount, at the ground
    };
    KindSettings& Settings(int kind);

    constexpr int   kGridSize = 128;
    constexpr float kCellSize = 2.0f;

    /// For the renderer and the panel: world-aligned toroidal grid, cell (i, j) of the world
    /// (i = floor(x / kCellSize)) lives at [mod(j, size) * size + mod(i, size)], valid for i in
    /// [firstI, firstI + size) (same for j). `version` changes whenever any amount does.
    struct Grid
    {
        int          firstI = 0, firstJ = 0;
        const float* amount[kKindCount] = {};
        float        maxAmount[kKindCount] = {};   // the largest amount anywhere, per kind
        uint32_t     version = 0;
    };
    const Grid& Current();

    /// Amount of a kind at a world position (0 outside the grid).
    float AmountAt(int kind, float x, float y);

    /// Once per frame, after fields::Update(). Steps the field a few times a second.
    void Update(float dt, const world::Snapshot& snap);

    // --- testing (the fog panel; not saved) ---
    /// Adds fog of a kind around the player: `amount` at the centre, fading to 0 at `radius` yd
    /// (smooth), clamped to 1. It then burns off by the kind's own rate.
    void Deposit(int kind, float amount, float radius);
    /// Removes all fog.
    void Clear();
    /// Holds every amount where it is (no build-up, no burn-off).
    void  SetPaused(bool paused);
    bool  Paused();

    struct Stats { double tickMs = 0.0; unsigned ticks = 0; unsigned cellsWithFog[kKindCount] = {}; unsigned kilobytes = 0; };
    Stats GetStats();

    /// Forget everything (world left).
    void Reset();
}
