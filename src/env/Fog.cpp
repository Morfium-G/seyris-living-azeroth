#include "Fog.hpp"

#include "FogSettings.hpp"
#include "TerrainHeight.hpp"

#include "../features/GrassPerf.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wxl_livingazeroth::fog
{
    namespace
    {
        constexpr float kTickSeconds = 0.25f;
        constexpr float kGone = 1.0e-4f; // below this an amount counts as no fog

        /// Copies a kind's table values (or built-in defaults) into its settings.
        void Apply(KindSettings& s, const fogsettings::Values& v)
        {
            s.strength = v.strength;
            s.buildUpSeconds = v.buildUp;
            s.burnOffSeconds = v.burnOff;
            s.thickness = v.thickness;
            s.density = v.density;
            s.windThin = v.windThin;
            s.windDrift = v.windDrift;
            s.patchiness = v.patchiness;
            s.weight = v.weight;
        }

        KindSettings MakeDefaults(int kind)
        {
            KindSettings s;
            Apply(s, fogsettings::Defaults(kind));
            return s;
        }

        KindSettings g_settings[kKindCount] = { MakeDefaults(0), MakeDefaults(1), MakeDefaults(2), MakeDefaults(3), MakeDefaults(4) };

        // The place and table generation the settings were last resolved for.
        uint32_t g_resolvedArea = ~0u, g_resolvedGeneration = 0;
        int      g_resolvedMap = -2;

        void ResolveForPlace(const world::Snapshot& snap)
        {
            const uint32_t area = snap.areaCount > 0 ? snap.areaChain[0] : 0;
            const uint32_t generation = fogsettings::Generation();
            static int lastOverride[kKindCount] = {};
            bool overrideChanged = false;
            for (int k = 0; k < kKindCount; ++k)
                if (g_settings[k].panelOverride != lastOverride[k]) { lastOverride[k] = g_settings[k].panelOverride; overrideChanged = true; }
            if (!overrideChanged && area == g_resolvedArea && snap.mapId == g_resolvedMap && generation == g_resolvedGeneration) return;
            g_resolvedArea = area;
            g_resolvedMap = snap.mapId;
            g_resolvedGeneration = generation;
            for (int k = 0; k < kKindCount; ++k)
                if (!g_settings[k].panelOverride) Apply(g_settings[k], fogsettings::Resolve(area, snap.mapId, k));
        }

        std::vector<float> g_amount[kKindCount];
        std::vector<float>   g_ground;
        std::vector<uint8_t> g_groundKnown;
        float    g_groundFallback = 0.0f;  // the ground under the player, for cells not known yet
        int      g_groundCursor = 0;
        constexpr int kGroundQueriesPerFrame = 512;
        Grid     g_grid;
        bool     g_placed = false;     // the grid has been centred once
        int      g_map = -1;
        float    g_px = 0.0f, g_py = 0.0f;
        float    g_tickTime = 0.0f;
        bool     g_paused = false;
        Stats    g_stats;

        int Wrap(int v) { const int m = v % kGridSize; return m < 0 ? m + kGridSize : m; }
        int Index(int i, int j) { return Wrap(j) * kGridSize + Wrap(i); }

        void EnsureStorage()
        {
            if (!g_amount[0].empty()) return;
            for (int k = 0; k < kKindCount; ++k)
            {
                g_amount[k].assign(kGridSize * kGridSize, 0.0f);
                g_grid.amount[k] = g_amount[k].data();
            }
            g_ground.assign(kGridSize * kGridSize, 0.0f);
            g_groundKnown.assign(kGridSize * kGridSize, 0);
            g_grid.ground = g_ground.data();
            g_grid.groundKnown = g_groundKnown.data();
            g_stats.kilobytes = static_cast<unsigned>((kKindCount * sizeof(float) + sizeof(float) + 1) * kGridSize * kGridSize / 1024);
        }

        void ForgetGround(int index)
        {
            g_groundKnown[index] = 0;
            g_ground[index] = g_groundFallback;
        }

        /// Zeroes world column i (all rows) / world row j (all columns) in every kind, and forgets
        /// their ground (they're other places now).
        void ClearColumn(int i)
        {
            for (int r = 0; r < kGridSize; ++r)
            {
                const int index = r * kGridSize + Wrap(i);
                for (int k = 0; k < kKindCount; ++k) g_amount[k][index] = 0.0f;
                ForgetGround(index);
            }
        }
        void ClearRow(int j)
        {
            for (int c = 0; c < kGridSize; ++c)
            {
                const int index = Wrap(j) * kGridSize + c;
                for (int k = 0; k < kKindCount; ++k) g_amount[k][index] = 0.0f;
                ForgetGround(index);
            }
        }

        /// Some cells' ground per frame: the higher of the terrain and terrain liquid at the cell's
        /// centre. Cells whose chunk isn't loaded stay unknown and are asked again on the next round.
        void FillGround()
        {
            int queries = 0;
            unsigned missing = 0;
            bool changed = false;
            for (int n = 0; n < kGridSize * kGridSize && queries < kGroundQueriesPerFrame; ++n)
            {
                const int index = g_groundCursor;
                g_groundCursor = (g_groundCursor + 1) % (kGridSize * kGridSize);
                if (g_groundKnown[index]) continue;
                // Storage index -> the world cell it holds now.
                const int ci = index % kGridSize, cj = index / kGridSize;
                const int i = g_grid.firstI + Wrap(ci - g_grid.firstI);
                const int j = g_grid.firstJ + Wrap(cj - g_grid.firstJ);
                const float x = (i + 0.5f) * kCellSize, y = (j + 0.5f) * kCellSize;
                ++queries;
                float z = 0.0f;
                if (!terrain::HeightAt(x, y, z)) { ++missing; continue; }
                float liquid = 0.0f;
                if (terrain::LiquidHeightAt(x, y, liquid) && liquid > z) z = liquid;
                g_ground[index] = z;
                g_groundKnown[index] = 1;
                changed = true;
            }
            if (changed) ++g_grid.groundVersion;
            unsigned unknown = 0;
            for (uint8_t k : g_groundKnown) unknown += k ? 0 : 1;
            g_grid.groundMissing = unknown;
        }

        void ClearAll()
        {
            for (int k = 0; k < kKindCount; ++k)
            {
                std::fill(g_amount[k].begin(), g_amount[k].end(), 0.0f);
                g_grid.maxAmount[k] = 0.0f;
                g_stats.cellsWithFog[k] = 0;
            }
            ++g_grid.version;
        }

        /// Keeps the grid centred on the player; cells that scroll in start without fog (until the
        /// kinds' rules exist, there's nothing to start them from).
        void Recenter(float x, float y)
        {
            const int fi = static_cast<int>(std::floor(x / kCellSize)) - kGridSize / 2;
            const int fj = static_cast<int>(std::floor(y / kCellSize)) - kGridSize / 2;
            if (g_placed && fi == g_grid.firstI && fj == g_grid.firstJ) return;
            if (!g_placed || std::abs(fi - g_grid.firstI) >= kGridSize || std::abs(fj - g_grid.firstJ) >= kGridSize)
            {
                ClearAll();
                for (int index = 0; index < kGridSize * kGridSize; ++index) ForgetGround(index);
                ++g_grid.groundVersion;
            }
            else
            {
                // Columns entering on the side we move toward (they reuse the slots of those leaving).
                for (int i = g_grid.firstI + kGridSize; i < fi + kGridSize; ++i) ClearColumn(i);
                for (int i = fi; i < g_grid.firstI; ++i) ClearColumn(i);
                for (int j = g_grid.firstJ + kGridSize; j < fj + kGridSize; ++j) ClearRow(j);
                for (int j = fj; j < g_grid.firstJ; ++j) ClearRow(j);
                ++g_grid.version;
            }
            g_grid.firstI = fi;
            g_grid.firstJ = fj;
            g_placed = true;
        }

        /// One step: every amount toward its target (0 for now) at the kind's rate.
        void Step(float dt)
        {
            bool changed = false;
            for (int k = 0; k < kKindCount; ++k)
            {
                if (g_grid.maxAmount[k] <= 0.0f) { g_stats.cellsWithFog[k] = 0; continue; }
                const KindSettings& s = g_settings[k];
                const float down = std::exp2(-dt / (s.burnOffSeconds > 0.1f ? s.burnOffSeconds : 0.1f));
                float most = 0.0f;
                unsigned cells = 0;
                for (float& a : g_amount[k])
                {
                    if (a <= 0.0f) continue;
                    a *= down;
                    if (a < kGone) { a = 0.0f; continue; }
                    if (a > most) most = a;
                    ++cells;
                }
                g_grid.maxAmount[k] = most;
                g_stats.cellsWithFog[k] = cells;
                changed = true;
            }
            if (changed) ++g_grid.version;
        }
    }

    const char* KindName(int kind)
    {
        static const char* const kNames[kKindCount] = { "mist", "water fog", "steam", "area fog", "haze" };
        return kind >= 0 && kind < kKindCount ? kNames[kind] : "?";
    }

    KindSettings& Settings(int kind) { return g_settings[kind >= 0 && kind < kKindCount ? kind : 0]; }

    const Grid& Current() { return g_grid; }

    float AmountAt(int kind, float x, float y)
    {
        if (kind < 0 || kind >= kKindCount || g_amount[kind].empty()) return 0.0f;
        const int i = static_cast<int>(std::floor(x / kCellSize));
        const int j = static_cast<int>(std::floor(y / kCellSize));
        if (i < g_grid.firstI || i >= g_grid.firstI + kGridSize || j < g_grid.firstJ || j >= g_grid.firstJ + kGridSize) return 0.0f;
        return g_amount[kind][Index(i, j)];
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        if (!snap.inWorld) { Reset(); return; }
        EnsureStorage();
        ResolveForPlace(snap);
        if (snap.mapId != g_map) { ClearAll(); g_map = snap.mapId; g_placed = false; }
        g_px = snap.playerPos[0];
        g_py = snap.playerPos[1];
        if (!terrain::HeightAt(g_px, g_py, g_groundFallback)) g_groundFallback = snap.playerPos[2];
        Recenter(g_px, g_py);
        FillGround();

        g_tickTime += dt;
        if (g_tickTime < kTickSeconds) return;
        const float step = g_tickTime;
        g_tickTime = 0.0f;
        if (g_paused) return;

        const double t0 = grassperf::Now();
        Step(step);
        g_stats.tickMs = grassperf::Now() - t0;
        ++g_stats.ticks;
    }

    void Deposit(int kind, float amount, float radius)
    {
        if (kind < 0 || kind >= kKindCount || !g_placed) return;
        EnsureStorage();
        float most = g_grid.maxAmount[kind];
        for (int j = g_grid.firstJ; j < g_grid.firstJ + kGridSize; ++j)
            for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
            {
                float w = 1.0f;
                if (radius > 0.0f)
                {
                    const float dx = (i + 0.5f) * kCellSize - g_px, dy = (j + 0.5f) * kCellSize - g_py;
                    const float t = 1.0f - std::sqrt(dx * dx + dy * dy) / radius;
                    if (t <= 0.0f) continue;
                    w = t * t * (3.0f - 2.0f * t);
                }
                float& a = g_amount[kind][Index(i, j)];
                a = std::fmin(1.0f, a + amount * w);
                if (a > most) most = a;
            }
        g_grid.maxAmount[kind] = most;
        ++g_grid.version;
    }

    void Clear() { if (!g_amount[0].empty()) ClearAll(); }
    void SetPaused(bool paused) { g_paused = paused; }
    bool Paused() { return g_paused; }
    Stats GetStats() { return g_stats; }

    void Reset()
    {
        if (!g_amount[0].empty()) ClearAll();
        g_placed = false;
        g_map = -1;
        g_tickTime = 0.0f;
    }
}
