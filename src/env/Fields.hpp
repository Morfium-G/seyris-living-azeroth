// World fields: named values over the world near the player (orchestration docs/r&d/immersion/
// world-fields.md, "Kind 2"). Consumers ask by name once (Find) and then sample by handle.
//
//  - "temperature" (degC): the place's climate (env/Climate), and on the grid warmed near hot
//    liquids (magma) and hot ground (painted materials with a Temperature, e.g. lava streams);
//    local sources (spells, campfires) come later.
//  - "moisture" (0 bone dry .. 1 soaked): the ground, on a 2 yd grid around the player. It rests at
//    an equilibrium -- the material's RestMoisture, wetter near a wet liquid (Absorbency x how close
//    and how low; magma doesn't wet), drier in hot dry air (and near magma, which heats it) -- and
//    moves toward it over minutes (faster when warm and windy, slower in humid air). Rain wets
//    ground open to the sky, scaled by Absorbency. Submerged ground takes the liquid's moisture.
//    What a liquid does: its GroundMaterialSelector row (LiquidType ID -> material: RestMoisture,
//    Temperature), else built in by category (water, ocean, slime: wet; magma: dry, 1000 degC).
#pragma once

#include "WorldQuery.hpp"

#include <cstdint>

namespace wxl_livingazeroth::fields
{
    /// A field's handle by name ("temperature", "moisture"); -1 if unknown. Names are resolved once;
    /// sampling by handle costs nothing extra.
    int         Find(const char* name);
    int         Count();
    const char* Name(int field);

    /// The field's value at a world position. False where it isn't known (moisture: outside the
    /// grid or on ground that isn't loaded yet).
    bool Sample(int field, const float pos[3], float& out);

    /// Moisture details at a position, for the panel.
    struct MoistureDetail
    {
        bool  known = false;
        float rest = 0.0f, absorbency = 0.0f;     // the ground's materials (blended by painted strength)
        float open = 1.0f;                        // 1 open sky .. 0 roofed (rain reaches it)
        bool  submerged = false;
        float waterDistance = -1.0f;              // yd to the nearest submerged cell (-1 = none in range)
        float heightAboveWater = 0.0f;            // yd above that water's surface
        float shore = 0.0f;                       // 0..1 how much the water nearby counts
        uint32_t liquid = 0;                      // that liquid's LiquidType ID (the one here when submerged)
        float liquidMoisture = 0.0f;              // what it does to the ground's moisture (magma 0, water 1)
        bool  liquidHot = false;                  // it has a temperature of its own...
        float liquidTemperature = 0.0f;           // ...this one
        float heat = 0.0f;                        // 0..1 how much its heat reaches here
        float hotShare = 0.0f, hotTemperature = 0.0f; // painted strength of hot materials here (lava textures), and their degC
        float groundHeat = 0.0f, groundHeatTemperature = 0.0f, groundHeatDistance = -1.0f; // the nearest hot ground: reach, degC, yd
        float airTemperature = 0.0f;              // the place's climate
        float temperature = 0.0f, humidity = 0.0f; // the ground's (air + liquid heat), the air's humidity
        float equilibrium = 0.0f, value = 0.0f;
    };
    MoistureDetail Moisture(const float pos[3]);

    /// For GPU consumers: moisture above the material's rest (0..255 = 0..1), one byte per cell, in a
    /// world-aligned toroidal layout: cell (i, j) of the world (i = floor(x / cellSize)) lives at
    /// [mod(j, size) * size + mod(i, size)]. Only cells i in [firstI, firstI + size) (same for j) are
    /// valid. `version` changes whenever the data does.
    struct WetGrid
    {
        int            size = 0;
        float          cellSize = 0.0f;
        int            firstI = 0, firstJ = 0;
        const uint8_t* excess = nullptr;
        uint32_t       version = 0;
        float          farExcess = 0.0f; // beyond the grid: the mean excess of its outer ring (rain is regional)
    };
    const WetGrid& Wet();

    struct Stats { unsigned filled = 0, cells = 0, water = 0; double fillMs = 0.0, transformMs = 0.0, tickMs = 0.0; float rainSoak = 0.0f; unsigned hotGround = 0; };
    Stats GetStats();

    /// Once per frame, after world::Refresh(), climate::Update() and wind::Update().
    void Update(float dt, const world::Snapshot& snap);

    /// Forget everything (materials or climate reloaded: refill).
    void Reset();
}
