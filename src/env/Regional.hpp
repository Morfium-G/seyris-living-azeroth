// The regional layer: environment state per zone (orchestration docs/r&d/immersion/
// regional-layer-and-snow.md). The near fields (env/Fields) hold detail around the player; this holds
// each zone's average state, for the ground entering the near grid and (later) the far distance.
//
// Zone = the top of an area's chain (server weather is per zone). Rules:
//  - the player's zone follows the live weather (the only weather the client knows);
//  - zones the player has left keep their state and evolve by their own climate with no rain
//    assumed (wet zones dry slowly), so looking back a rained-on zone stays wet for a while;
//  - entering a zone that's new or stale fast-forwards it (catch-up: the same rules, many times
//    faster for a short while) toward what its current weather implies, then it runs normally.
// Session only: nothing survives a logout (until a server can sync it).
#pragma once

#include "WorldQuery.hpp"

#include <cstdint>
#include <vector>

namespace wxl_livingazeroth::regional
{
    enum class Source : uint8_t { Estimated, Observed };

    struct Zone
    {
        int      map = -1;
        uint32_t id = 0;
        float    rainSoak = 0.0f;     // 0..1: how much rain (and melted snow) has soaked open, absorbent ground
        float    snowDepth = 0.0f;    // yd of fallen snow on open, flat ground (env/Snow.hpp)
        float    paintedKeep = 1.0f;  // share of painted snow left on typical open ground (default melt columns)
        float    temperature = 0.0f;  // degC its snow saw last step: air + sun on flat open ground
        bool     keepStarted = false;
        double   lastSeen = 0.0;      // session seconds the player was last in it
        float    catchUp = 0.0f;      // seconds of fast-forward left
        Source   source = Source::Estimated;
    };

    /// Once per frame, after climate::Update().
    void Update(float dt, const world::Snapshot& snap);

    /// The zone an area belongs to (the top of its chain; 0 for none).
    uint32_t ZoneOf(uint32_t area);

    /// The player's zone, and a zone's record (null if never seen).
    uint32_t    PlayerZone();
    const Zone* Find(int map, uint32_t zone);

    /// A zone's rain soak (0 for zones never seen).
    float RainSoak(int map, uint32_t zone);

    /// A zone's fallen snow (yd, 0 for zones never seen) and its painted snow's kept share (zones never
    /// seen: what their temperature allows now).
    float SnowDepth(int map, uint32_t zone);
    float PaintedKeep(int map, uint32_t zone);

    /// The temperature a zone's snow sees (degC): its climate on flat open ground with the sun. The
    /// weather only counts in the player's zone (the only weather the client knows).
    float ZoneTemperature(int map, uint32_t zone);

    /// Drying of a soaked surface per second, at this air temperature, humidity and steady wind (the
    /// zones' rule; the near grid's melt water dries the same way).
    float DryingRate(float temperature, float humidity, float wind);

    // --- testing (the panels' deposit buttons; not saved) ---
    /// Adds fallen snow to the player's zone; it then melts or stays by the zone's rules.
    void DepositSnow(float yd);
    /// Removes every zone's fallen snow, and lets painted snow start over from its temperature.
    void ClearSnow();
    /// Sets the player's zone's rain soak (0..1); the ground follows and dries from there.
    void SetRainSoak(float soak);

    /// How much faster the player's zone runs right now (> 1 while it catches up), so the ground
    /// near the player can fast-forward with it.
    float PlayerZoneSpeed();

    /// Which zone each terrain chunk around the player belongs to, for the far distance: a
    /// world-aligned toroidal grid of `size` x `size` chunks (chunk i = floor(x / cellSize) lives at
    /// [mod(j, size) * size + mod(i, size)]), valid for i in [firstI, firstI + size) (same for j).
    /// zone 0 = not known yet (chunk not loaded).
    struct ZoneMap
    {
        int             size = 0;
        float           cellSize = 0.0f;
        int             firstI = 0, firstJ = 0;
        const uint32_t* zone = nullptr;
        unsigned        known = 0;
    };
    const ZoneMap& Map();

    /// Every zone seen this session (for the panel).
    const std::vector<Zone>& Zones();

    /// An area's name from AreaTable ("" if unknown).
    const char* AreaName(uint32_t area);

    /// Session seconds (for "last seen").
    double Now();

    /// Forget everything (world left).
    void Reset();
}
