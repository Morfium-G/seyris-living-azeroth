#include "Regional.hpp"

#include "Climate.hpp"
#include "TerrainHeight.hpp"
#include "Wind.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wxl_livingazeroth::regional
{
    namespace
    {
        // Same rates as the near grid (env/Fields), so a zone's value and its ground agree.
        constexpr float kRainSeconds = 90.0f;   // full rain soaks absorbent open ground in ~this
        constexpr float kDrySeconds = 600.0f;   // drying at 0..15 degC, calm, average humidity
        // Entering a new or stale zone: run its rules this many times faster for this long (~6 minutes
        // of weather in 30 seconds), so it looks like the weather has been there a while.
        constexpr float kCatchUpSpeed = 12.0f, kCatchUpSeconds = 30.0f;
        constexpr double kStaleSeconds = 600.0; // a zone not visited for this long counts as unknown again
        constexpr float kTick = 0.25f;

        // AreaTable rows keep the on-disk layout with strings resolved (see WorldQuery): the name
        // (AreaName_Lang, column 11, the client's own locale) at +0x2C [believed: layout; the panel
        // shows it beside the client's zone text].
        constexpr uintptr_t kAreaMaxId = 0x00AD3140, kAreaMinId = 0x00AD3144, kAreaIdTable = 0x00AD3154;
        constexpr size_t    kAreaNameField = 0x2C;

        std::vector<Zone> g_zones;

        // The zone map: one entry per terrain chunk (33.3 yd) in a 64 x 64 window (+-1066 yd) around
        // the player, world-aligned and toroidal; refilled a slice per frame, unknown chunks retried.
        constexpr int   kMapSize = 64;
        constexpr float kChunk = 100.0f / 3.0f;
        constexpr int   kMapRecentre = 4;     // chunks the player moves before the window follows
        constexpr int   kMapPerFrame = 256;   // chunk lookups per frame
        struct MapSlot { int i = 0x7FFFFFFF, j = 0; };
        std::vector<MapSlot>  g_mapSlots(kMapSize * kMapSize);
        std::vector<uint32_t> g_mapZone(kMapSize * kMapSize, 0);
        ZoneMap g_map;
        int     g_mapCursor = 0;
        bool    g_haveMap = false;
        int     g_mapMap = -1;

        int MapMod(int v) { const int m = v % kMapSize; return m < 0 ? m + kMapSize : m; }

        void UpdateZoneMap(const world::Snapshot& snap)
        {
            if (snap.mapId != g_mapMap)
            {
                for (MapSlot& m : g_mapSlots) m = MapSlot{};
                std::fill(g_mapZone.begin(), g_mapZone.end(), 0u);
                g_mapMap = snap.mapId;
                g_haveMap = false;
            }
            const int pi = static_cast<int>(std::floor(snap.playerPos[0] / kChunk)) - kMapSize / 2;
            const int pj = static_cast<int>(std::floor(snap.playerPos[1] / kChunk)) - kMapSize / 2;
            if (!g_haveMap || std::abs(pi - g_map.firstI) >= kMapRecentre || std::abs(pj - g_map.firstJ) >= kMapRecentre)
            {
                g_map.firstI = pi; g_map.firstJ = pj;
                g_haveMap = true;
            }
            g_map.size = kMapSize; g_map.cellSize = kChunk; g_map.zone = g_mapZone.data();

            // A slice per frame: chunks whose slot holds another world chunk or isn't known yet.
            for (int n = 0; n < kMapPerFrame; ++n)
            {
                const int k = g_mapCursor;
                g_mapCursor = (g_mapCursor + 1) % (kMapSize * kMapSize);
                const int i = g_map.firstI + k % kMapSize, j = g_map.firstJ + k / kMapSize;
                const int slot = MapMod(j) * kMapSize + MapMod(i);
                MapSlot& m = g_mapSlots[slot];
                if (m.i == i && m.j == j && g_mapZone[slot]) continue;
                m.i = i; m.j = j;
                uint32_t area = 0;
                g_mapZone[slot] = terrain::ChunkAreaAt((i + 0.5f) * kChunk, (j + 0.5f) * kChunk, area) ? ZoneOf(area) : 0;
            }
            unsigned known = 0;
            for (int b = 0; b < kMapSize; ++b)
                for (int a = 0; a < kMapSize; ++a)
                {
                    const int i = g_map.firstI + a, j = g_map.firstJ + b;
                    const int slot = MapMod(j) * kMapSize + MapMod(i);
                    known += g_mapSlots[slot].i == i && g_mapSlots[slot].j == j && g_mapZone[slot] != 0;
                }
            g_map.known = known;
        }
        uint32_t g_playerZone = 0;
        int      g_playerMap = -1;
        double   g_now = 0.0;
        float    g_tickTime = 0.0f;

        Zone& Get(int map, uint32_t id)
        {
            for (Zone& z : g_zones) if (z.map == map && z.id == id) return z;
            Zone z;
            z.map = map; z.id = id;
            g_zones.push_back(z);
            return g_zones.back();
        }

        // One zone's rain soak over dt: wetting while it rains, else drying by its own climate.
        void Step(Zone& z, float dt, bool raining, float intensity, float wind)
        {
            if (raining) { z.rainSoak += dt / kRainSeconds * intensity * (1.0f - z.rainSoak); }
            else
            {
                const climate::Row& row = climate::For(z.id, z.map);
                const float t = climate::Temperature(row).total;
                const float warmth = t <= 0.0f ? 0.2f : 1.0f + t / 15.0f;
                const float rate = (1.0f + 2.0f * wind) * warmth * (1.0f - 0.7f * row.humidity) / kDrySeconds;
                z.rainSoak -= z.rainSoak * (rate * dt > 1.0f ? 1.0f : rate * dt);
            }
            z.rainSoak = z.rainSoak < 0.0f ? 0.0f : (z.rainSoak > 1.0f ? 1.0f : z.rainSoak);
        }
    }

    uint32_t ZoneOf(uint32_t area)
    {
        uint32_t chain[world::kMaxAreaChain];
        const int n = world::AreaChain(area, chain, world::kMaxAreaChain);
        return n > 0 ? chain[n - 1] : 0;
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        g_now += dt;
        if (!snap.inWorld) return;
        UpdateZoneMap(snap);
        g_playerMap = snap.mapId;
        // The client's area query on the player's location gives nothing while flying: then the
        // area of the terrain chunk below.
        uint32_t zone = snap.areaCount > 0 ? snap.areaChain[snap.areaCount - 1] : 0;
        if (!zone)
        {
            terrain::Surface below;
            if (terrain::SurfaceAt(snap.playerPos[0], snap.playerPos[1], below)) zone = ZoneOf(below.area);
        }
        if (!zone) zone = g_playerZone; // nothing below (over a hole, unloaded): stay
        if (zone && zone != g_playerZone)
        {
            // Entering: a zone never seen, or not seen for a while, fast-forwards.
            const Zone* known = Find(snap.mapId, zone);
            Zone& z = Get(snap.mapId, zone);
            if (!known || g_now - z.lastSeen > kStaleSeconds) z.catchUp = kCatchUpSeconds;
        }
        g_playerZone = zone;

        g_tickTime += dt;
        if (g_tickTime < kTick) return;
        const float step = g_tickTime;
        g_tickTime = 0.0f;

        const climate::Weather& w = climate::CurrentWeather();
        const bool raining = w.type == 1 && w.intensity > 0.0f;
        const float wind = wind::SteadyGround();
        for (Zone& z : g_zones)
        {
            if (z.map == snap.mapId && z.id == g_playerZone)
            {
                const float speed = z.catchUp > 0.0f ? kCatchUpSpeed : 1.0f;
                Step(z, step * speed, raining, w.intensity, wind);
                z.catchUp = z.catchUp > step ? z.catchUp - step : 0.0f;
                z.lastSeen = g_now;
                z.source = Source::Observed;
            }
            else Step(z, step, false, 0.0f, wind); // no rain assumed where we can't see the weather
        }
    }

    uint32_t PlayerZone() { return g_playerZone; }

    float PlayerZoneSpeed()
    {
        const Zone* z = Find(g_playerMap, g_playerZone);
        return z && z->catchUp > 0.0f ? kCatchUpSpeed : 1.0f;
    }

    const Zone* Find(int map, uint32_t zone)
    {
        for (const Zone& z : g_zones) if (z.map == map && z.id == zone) return &z;
        return nullptr;
    }

    float RainSoak(int map, uint32_t zone)
    {
        const Zone* z = Find(map, zone);
        return z ? z->rainSoak : 0.0f;
    }

    const std::vector<Zone>& Zones() { return g_zones; }
    const ZoneMap& Map() { return g_map; }

    const char* AreaName(uint32_t area)
    {
        const int32_t minId = *reinterpret_cast<const int32_t*>(kAreaMinId);
        const int32_t maxId = *reinterpret_cast<const int32_t*>(kAreaMaxId);
        const auto* table = *reinterpret_cast<const uint8_t* const* const*>(kAreaIdTable);
        if (!table || static_cast<int32_t>(area) < minId || static_cast<int32_t>(area) > maxId) return "";
        const uint8_t* row = table[static_cast<int32_t>(area) - minId];
        const char* name = row ? *reinterpret_cast<const char* const*>(row + kAreaNameField) : nullptr;
        return name ? name : "";
    }

    double Now() { return g_now; }

    void Reset()
    {
        g_zones.clear();
        g_playerZone = 0;
    }
}
