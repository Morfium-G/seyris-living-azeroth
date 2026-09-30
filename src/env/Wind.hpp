// Wind, first draft: WindProfile.cdbc (Global / Map / Area scopes with inheritance) + live weather +
// a slowly drifting steady wind + gusts that travel across the ground. No terrain shaping, no
// liquids yet. Design: orchestration/docs/r&d/immersion/environment-state.md.
//
// Directions are compass bearings the wind blows TOWARD: 0 = north (+X), 90 = east (-Y).
#pragma once

#include "../wxl_seyris/CdbcApi.hpp"
#include "WorldQuery.hpp"

#include "wxl/PluginApi.h"

#include <cstdint>

namespace wxl_livingazeroth::wind
{
    /// Profile fields in table order (after ID / ScopeType / ScopeID). A negative value in any
    /// Map/Area row means "inherit from the next scope up".
    enum Field
    {
        GroundMin, GroundMax, AloftMin, AloftMax, AloftHeight,
        WeatherInfluence, GustStrength, GustFrequency, TerrainWeight,
        LockedDirection, LockStrength,
        FieldCount
    };
    const char* FieldName(int field);

    /// Where a resolved field came from, for the debug panel.
    enum class Source { BuiltIn, Global, Map, Area };

    struct Resolved
    {
        float    value[FieldCount] = {};
        Source   source[FieldCount] = {};
        uint32_t sourceId[FieldCount] = {}; // map or area id that supplied it
        uint32_t flags = 0;                 // most specific row's Flags (reserved)
    };

    struct Sample
    {
        float dirX = 1.0f, dirY = 0.0f; // unit vector in world XY
        float strength = 0.0f;          // 0..1 scale, gusts can push past 1
        float gust = 0.0f;              // 0..1, how much of a gust is passing right here
        float open = 1.0f;              // 1 open sky, 0 roofed over (already applied to strength)
        float lee = 1.0f;               // 1 exposed, lower in the lee of a wall/cliff (already applied)
    };

    void Init(const WXL_Api* api);

    /// (Re)loads WindProfile.cdbc. Safe to call again (the panel's Reload button).
    void LoadProfiles(const WXL_SeyrisCdbcApi* cdbc);

    /// Once per frame, after world::Refresh().
    void Update(float dt, const world::Snapshot& snap);

    /// Wind at a world position. heightAboveGround blends ground -> aloft (0 until terrain exists).
    Sample At(const float pos[3], float heightAboveGround = 0.0f);

    // --- debug readouts ---
    bool        ProfilesLoaded();
    uint32_t    ProfileRowCount();
    const char* ProfileError();
    const Resolved& Target();      // profile for the current location, before blending
    const Resolved& Current();     // blended profile actually in use
    float SteadyGround();          // current steady strength at ground level
    float SteadyAloft();
    float BearingDegrees();        // current steady direction
    float EffectiveWeather();      // weather intensity the model used

    /// Debug/testing: scales every sample's strength (1 = as authored). Not saved.
    void  SetStrengthMultiplier(float multiplier);
    float StrengthMultiplier();

    /// Debug override: < 0 follows the live weather, 0..1 forces an intensity.
    void SetWeatherOverride(float intensity);
    float WeatherOverride();
}
