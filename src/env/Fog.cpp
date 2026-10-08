#include "Fog.hpp"

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

        KindSettings MakeDefaults(int kind)
        {
            KindSettings s;
            switch (kind)
            {
                case kMist:     s.buildUpSeconds = 300.0f; s.burnOffSeconds = 900.0f;  s.thickness = 4.0f;  s.density = 0.04f; break;
                case kWaterFog: s.buildUpSeconds = 300.0f; s.burnOffSeconds = 1200.0f; s.thickness = 3.0f;  s.density = 0.05f; break;
                case kSteam:    s.buildUpSeconds = 10.0f;  s.burnOffSeconds = 30.0f;   s.thickness = 2.0f;  s.density = 0.08f; break;
                case kAreaFog:  s.buildUpSeconds = 600.0f; s.burnOffSeconds = 1800.0f; s.thickness = 8.0f;  s.density = 0.03f; break;
                case kHaze:     s.buildUpSeconds = 120.0f; s.burnOffSeconds = 600.0f;  s.thickness = 20.0f; s.density = 0.01f; break;
                default: break;
            }
            return s;
        }

        KindSettings g_settings[kKindCount] = { MakeDefaults(0), MakeDefaults(1), MakeDefaults(2), MakeDefaults(3), MakeDefaults(4) };

        std::vector<float> g_amount[kKindCount];
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
            g_stats.kilobytes = static_cast<unsigned>(kKindCount * kGridSize * kGridSize * sizeof(float) / 1024);
        }

        /// Zeroes world column i (all rows) / world row j (all columns) in every kind.
        void ClearColumn(int i) { for (int k = 0; k < kKindCount; ++k) for (int r = 0; r < kGridSize; ++r) g_amount[k][r * kGridSize + Wrap(i)] = 0.0f; }
        void ClearRow(int j)    { for (int k = 0; k < kKindCount; ++k) for (int c = 0; c < kGridSize; ++c) g_amount[k][Wrap(j) * kGridSize + c] = 0.0f; }

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
                ClearAll();
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
        if (snap.mapId != g_map) { ClearAll(); g_map = snap.mapId; g_placed = false; }
        g_px = snap.playerPos[0];
        g_py = snap.playerPos[1];
        Recenter(g_px, g_py);

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
