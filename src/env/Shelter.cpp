#include "Shelter.hpp"

#include "TerrainHeight.hpp"

#include "game/Pick.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace wxl_livingazeroth::shelter
{
    namespace
    {
        namespace gw = wxl::game::world;

        constexpr float    kCell         = 4.0f;   // yards, horizontal and vertical
        constexpr float    kRoofSearch   = 40.0f;  // how far up a roof still counts
        constexpr float    kRecheckAfter = 10.0f;  // seconds; WMOs stream in after the terrain
        constexpr float    kEvictRange   = 120.0f; // yards from the player
        constexpr unsigned kRayBudget    = 64;     // traces per frame

        // Lee ray: starts this high above the point and runs `reach` yards upwind, rising at
        // `angleDeg` so ordinary slopes pass while walls, cliffs and steep rock block it.
        constexpr float kLeeHeight = 1.5f;
        constexpr int   kSectors   = 8;      // 45-degree wind sectors cached separately
        LeeParams g_leeParams;

        // `open`: the roof cache stores 0/1; the lee cache stores the hit fraction along the ray
        // (1 = no hit), so strength can be applied at query time without re-tracing.
        struct Entry { float open; float tracedAt; float x, y; };

        float LeeFactor(float hitFraction)
        {
            const float t = hitFraction < 0.0f ? 0.0f : (hitFraction > 1.0f ? 1.0f : hitFraction);
            return 1.0f - g_leeParams.strength * (1.0f - t);
        }

        std::unordered_map<uint64_t, Entry> g_cache;
        std::unordered_map<uint64_t, Entry> g_lee[kSectors];
        bool     g_enabled = true;
        bool     g_leeEnabled = true;
        float    g_time = 0.0f;
        unsigned g_rays = 0, g_deferred = 0;

        int32_t Cell(float v) { return static_cast<int32_t>(std::floor(v / kCell)); }

        uint64_t Key(int32_t cx, int32_t cy, int32_t cz)
        {
            // 21 bits per axis, offset to unsigned: plenty for +-4M yards at 4-yard cells.
            const uint64_t ux = static_cast<uint64_t>(cx + (1 << 20)) & 0x1FFFFF;
            const uint64_t uy = static_cast<uint64_t>(cy + (1 << 20)) & 0x1FFFFF;
            const uint64_t uz = static_cast<uint64_t>(cz + (1 << 20)) & 0x1FFFFF;
            return (ux << 42) | (uy << 21) | uz;
        }

        float Trace(float x, float y, float z)
        {
            // Half a yard up, so a query at floor level doesn't hit the floor it stands on.
            const float from[3] = { x, y, z + 0.5f };
            const float to[3]   = { x, y, z + kRoofSearch };
            gw::WorldHit hit;
            return gw::TraceLine(from, to, hit) ? 0.0f : 1.0f;
        }
    }

    void BeginFrame(const float center[3], float dt)
    {
        g_time += (dt > 0.0f && dt < 1.0f) ? dt : 0.0f;
        g_rays = g_deferred = 0;

        const float r2 = kEvictRange * kEvictRange;
        auto evict = [&](std::unordered_map<uint64_t, Entry>& map)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                const float dx = it->second.x - center[0], dy = it->second.y - center[1];
                if (dx * dx + dy * dy > r2) it = map.erase(it);
                else ++it;
            }
        };
        evict(g_cache);
        for (auto& map : g_lee) evict(map);
    }

    float Lee(const float pos[3], float windDirX, float windDirY)
    {
        if (!g_enabled || !g_leeEnabled) return 1.0f;

        // Sector of the direction the wind comes FROM, so a slowly drifting wind reuses its rays.
        const float upX = -windDirX, upY = -windDirY;
        float angle = std::atan2(upY, upX); // -pi..pi
        if (angle < 0.0f) angle += 6.2831853f;
        const int sector = static_cast<int>(angle / (6.2831853f / kSectors) + 0.5f) % kSectors;

        const int32_t cx = Cell(pos[0]), cy = Cell(pos[1]), cz = Cell(pos[2]);
        const uint64_t key = Key(cx, cy, cz);
        auto& map = g_lee[sector];

        auto it = map.find(key);
        const bool stale = it != map.end() && g_time - it->second.tracedAt > kRecheckAfter;
        if (it != map.end() && !stale) return LeeFactor(it->second.open);

        if (g_rays >= kRayBudget)
        {
            ++g_deferred;
            return it != map.end() ? LeeFactor(it->second.open) : 1.0f;
        }
        ++g_rays;

        // Trace along the sector's centre direction, so every query in the cell and sector agrees.
        const float sectorAngle = sector * (6.2831853f / kSectors);
        const float ux = std::cos(sectorAngle), uy = std::sin(sectorAngle);
        const float reach = g_leeParams.reach;
        const float rise  = reach * std::tan(g_leeParams.angleDeg * 3.14159265f / 180.0f);
        // From the cell's centre column, never from below the terrain there (see Openness).
        const float x = (cx + 0.5f) * kCell, y = (cy + 0.5f) * kCell;
        float ground = pos[2];
        const float base = terrain::HeightAt(x, y, ground) && ground > pos[2] ? ground : pos[2];
        const float from[3] = { x, y, base + kLeeHeight };
        const float to[3]   = { x + ux * reach, y + uy * reach, base + kLeeHeight + rise };

        // Hit at fraction t: right behind the obstacle (t ~ 0) loses most of the wind, an obstacle
        // at the far end of the reach barely matters.
        gw::WorldHit hit;
        const float fraction = gw::TraceLine(from, to, hit) ? hit.t : 1.0f;
        map[key] = Entry{ fraction, g_time, x, y };
        return LeeFactor(fraction);
    }

    LeeParams GetLeeParams() { return g_leeParams; }

    void SetLeeParams(const LeeParams& params)
    {
        const bool shapeChanged = params.reach != g_leeParams.reach || params.angleDeg != g_leeParams.angleDeg;
        g_leeParams = params;
        if (shapeChanged)
            for (auto& map : g_lee) map.clear();
    }

    void SetLeeEnabled(bool enabled) { g_leeEnabled = enabled; }
    bool LeeEnabled() { return g_leeEnabled; }

    float Openness(const float pos[3])
    {
        if (!g_enabled) return 1.0f;

        const int32_t cx = Cell(pos[0]), cy = Cell(pos[1]), cz = Cell(pos[2]);
        const uint64_t key = Key(cx, cy, cz);

        auto it = g_cache.find(key);
        const bool stale = it != g_cache.end() && g_time - it->second.tracedAt > kRecheckAfter;
        if (it != g_cache.end() && !stale) return it->second.open;

        if (g_rays >= kRayBudget)
        {
            ++g_deferred;
            return it != g_cache.end() ? it->second.open : 1.0f; // keep the old answer, or assume open
        }
        ++g_rays;

        // Trace from the cell's centre column at the queried height, so every query in the cell
        // shares one answer -- but never from below the terrain there: on a steep slope the terrain
        // at the centre can be higher than the spot that asked, the ray would start underground and
        // "hit a roof" (the terrain from below), and exposed cliffs read as sheltered. The terrain
        // can't be a roof, so the ray starts above it.
        const float x = (cx + 0.5f) * kCell, y = (cy + 0.5f) * kCell;
        float ground = pos[2];
        const float z = terrain::HeightAt(x, y, ground) && ground > pos[2] ? ground : pos[2];
        const float open = Trace(x, y, z);
        g_cache[key] = Entry{ open, g_time, x, y };
        return open;
    }

    void  SetEnabled(bool enabled) { g_enabled = enabled; }
    bool  Enabled() { return g_enabled; }
    void  Clear()
    {
        g_cache.clear();
        for (auto& map : g_lee) map.clear();
    }

    Stats GetStats()
    {
        Stats s;
        s.entries = static_cast<unsigned>(g_cache.size());
        for (const auto& map : g_lee) s.entries += static_cast<unsigned>(map.size());
        s.raysThisFrame = g_rays;
        s.deferredThisFrame = g_deferred;
        return s;
    }
}
