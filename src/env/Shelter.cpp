#include "Shelter.hpp"

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

        struct Entry { float open; float tracedAt; float x, y; };

        std::unordered_map<uint64_t, Entry> g_cache;
        bool     g_enabled = true;
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
        for (auto it = g_cache.begin(); it != g_cache.end();)
        {
            const float dx = it->second.x - center[0], dy = it->second.y - center[1];
            if (dx * dx + dy * dy > r2) it = g_cache.erase(it);
            else ++it;
        }
    }

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
        // shares one answer.
        const float x = (cx + 0.5f) * kCell, y = (cy + 0.5f) * kCell;
        const float open = Trace(x, y, pos[2]);
        g_cache[key] = Entry{ open, g_time, x, y };
        return open;
    }

    void  SetEnabled(bool enabled) { g_enabled = enabled; }
    bool  Enabled() { return g_enabled; }
    void  Clear() { g_cache.clear(); }

    Stats GetStats()
    {
        Stats s;
        s.entries = static_cast<unsigned>(g_cache.size());
        s.raysThisFrame = g_rays;
        s.deferredThisFrame = g_deferred;
        return s;
    }
}
