#include "Fog.hpp"

#include "Climate.hpp"
#include "Fields.hpp"
#include "FogSettings.hpp"
#include "TerrainHeight.hpp"
#include "Wind.hpp"

#include "../features/GrassPerf.hpp"

#include "game/Pick.hpp"

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
        // Kinds that hang at their source's height (Grid::floor): water fog over the water, steam
        // over the lava or hot ground it rose from.
        constexpr bool kAnchored[kKindCount] = { false, true, true, false, false };
        std::vector<float> g_floor[kKindCount];

        /// A cell's amount of an anchored kind grew from `before` to `after` with fog from height z:
        /// its floor moves toward z by the new fog's share.
        void AddFrom(int kind, int index, float before, float after, float z)
        {
            if (!kAnchored[kind] || after <= before) return;
            float& f = g_floor[kind][index];
            f = before > 0.0f ? (f * before + z * (after - before)) / after : z;
        }
        std::vector<uint8_t> g_target[kKindCount]; // where each kind is heading, 0..255 = 0..1 (the rules)
        std::vector<float>   g_ground;   // the surface the fog lies on: terrain/liquid, or a WMO floor on it
        std::vector<float>   g_terrain;  // the terrain or terrain liquid alone
        std::vector<uint8_t> g_groundKnown;
        // WMOs, from one ray straight down per cell: 0 not traced yet, 1 open, 2 a WMO floor at most
        // kFloorRise above the terrain (the fog lies on it), 3 a structure (roof, stands, wall top):
        // no fog, and fog doesn't flow into it.
        enum Block : uint8_t { kUntraced = 0, kOpen = 1, kFloor = 2, kStructure = 3 };
        std::vector<uint8_t> g_block;
        int      g_traceCursor = 0;
        constexpr int   kTracesPerFrame = 64;    // a full grid in ~4 s at 60 fps, then again (late WMOs)
        constexpr float kFloorRise = 3.0f;       // yd: a WMO surface up to this above the terrain is a floor
        constexpr float kTraceFrom = 40.0f;      // yd above the terrain the ray starts
        constexpr float kMaxPool = 4.0f;         // amounts may pile up to this in hollows (heavy fog)
        std::vector<uint8_t> g_liquid;   // terrain liquid on the cell: 0 none, 1 water/ocean, 2 magma, 3 slime
        std::vector<uint8_t> g_fresh;    // scrolled in, not started yet (starts from its target x the area's progress)
        // How far the fog around the player has come toward its targets. 1 on a fresh grid (login,
        // teleport, new map): a place you arrive at shows its fog as if the conditions had held.
        float    g_progress[kKindCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        RulesInputs g_inputs;
        int      g_rulesRow = 0;         // the next grid row the rules evaluate (a quarter per step)
        int      g_fieldMoisture = -2, g_fieldTemperature = -2;
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
            for (int k = 0; k < kKindCount; ++k) g_target[k].assign(kGridSize * kGridSize, 0);
            for (int k = 0; k < kKindCount; ++k)
                if (kAnchored[k])
                {
                    g_floor[k].assign(kGridSize * kGridSize, 0.0f);
                    g_grid.floor[k] = g_floor[k].data();
                }
            g_ground.assign(kGridSize * kGridSize, 0.0f);
            g_terrain.assign(kGridSize * kGridSize, 0.0f);
            g_groundKnown.assign(kGridSize * kGridSize, 0);
            g_block.assign(kGridSize * kGridSize, kUntraced);
            g_liquid.assign(kGridSize * kGridSize, 0);
            g_fresh.assign(kGridSize * kGridSize, 1);
            g_grid.ground = g_ground.data();
            g_grid.groundKnown = g_groundKnown.data();
            g_grid.block = g_block.data();
            g_stats.kilobytes = static_cast<unsigned>((kKindCount * (sizeof(float) + 1) + 4 * sizeof(float) + 4) * kGridSize * kGridSize / 1024);
        }

        void ForgetGround(int index)
        {
            g_groundKnown[index] = 0;
            g_ground[index] = g_terrain[index] = g_groundFallback;
            g_block[index] = kUntraced;
            g_liquid[index] = 0;
            g_fresh[index] = 1;
            for (int k = 0; k < kKindCount; ++k) g_target[k][index] = 0;
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
                uint32_t liquidType = 0;
                g_liquid[index] = 0;
                if (terrain::LiquidAt(x, y, liquid, liquidType) && liquid > z)
                {
                    z = liquid;
                    switch (terrain::LiquidCategoryOf(liquidType))
                    {
                        case terrain::kLiquidMagma: g_liquid[index] = 2; break;
                        case terrain::kLiquidSlime: g_liquid[index] = 3; break;
                        default:                    g_liquid[index] = 1; break; // water, ocean, unknown
                    }
                }
                g_ground[index] = g_terrain[index] = z;
                g_groundKnown[index] = 1;
                g_block[index] = kUntraced;
                changed = true;
            }
            if (changed) ++g_grid.groundVersion;
            unsigned unknown = 0;
            for (uint8_t k : g_groundKnown) unknown += k ? 0 : 1;
            g_grid.groundMissing = unknown;
        }

        /// Some cells per frame, round and round (WMOs stream in after the terrain): one ray straight
        /// down from kTraceFrom above the terrain. The first surface it meets: the terrain (open), a
        /// WMO floor close above it (the fog lies on it: an arena floor, a courtyard), or something
        /// higher (a roof, stands, a wall top: no fog under or in it).
        void TraceCells()
        {
            namespace gw = wxl::game::world;
            bool changed = false;
            int rays = 0;
            for (int n = 0; n < kGridSize * kGridSize && rays < kTracesPerFrame; ++n)
            {
                const int index = g_traceCursor;
                g_traceCursor = (g_traceCursor + 1) % (kGridSize * kGridSize);
                if (!g_groundKnown[index]) continue;
                const int ci = index % kGridSize, cj = index / kGridSize;
                const int i = g_grid.firstI + Wrap(ci - g_grid.firstI);
                const int j = g_grid.firstJ + Wrap(cj - g_grid.firstJ);
                const float x = (i + 0.5f) * kCellSize, y = (j + 0.5f) * kCellSize;
                const float terrainZ = g_terrain[index];
                const float from[3] = { x, y, terrainZ + kTraceFrom };
                const float to[3] = { x, y, terrainZ - 0.5f };
                gw::WorldHit hit;
                ++rays;
                uint8_t state = kOpen;
                float surface = terrainZ;
                if (gw::TraceLine(from, to, hit))
                {
                    const float z = from[2] + (to[2] - from[2]) * hit.t;
                    const float rise = z - terrainZ;
                    if (rise > kFloorRise) state = kStructure;
                    else if (rise > 0.25f) { state = kFloor; surface = z; }
                }
                if (state == g_block[index] && surface == g_ground[index]) continue;
                g_block[index] = state;
                g_ground[index] = surface;
                if (state == kStructure)
                    for (int k = 0; k < kKindCount; ++k) { g_amount[k][index] = 0.0f; g_target[k][index] = 0; }
                changed = true;
            }
            if (changed) { ++g_grid.groundVersion; ++g_grid.version; }
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
                for (int k = 0; k < kKindCount; ++k) g_progress[k] = 1.0f; // a fresh grid: as if the conditions had held
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

        float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        float Smooth(float a, float b, float v) { const float t = Clamp01((v - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }

        /// What the rules need about the place and the moment (the player's place; per-cell values
        /// come from the cell). The air an hour ago is computed from the climate, not remembered, so a
        /// place you just arrived at knows whether it's been cooling.
        void GatherInputs(const world::Snapshot& snap)
        {
            RulesInputs in;
            const uint32_t area = snap.areaCount > 0 ? snap.areaChain[0] : 0;
            const climate::Row& row = climate::For(area, snap.mapId);
            in.airNow = climate::Temperature(row).total;
            in.airHourAgo = climate::TemperatureAtTime(row, climate::Now().dayFraction - 1.0f / 24.0f).total;
            in.water = climate::WaterTemperature(row);
            in.humidity = row.humidity;
            in.fogginess = row.fogginess;
            // The area's steady ground wind, not the sample at the player: standing in a lee or under a
            // roof mustn't calm the fog everywhere. Bearing clockwise from north (+X), east is -Y.
            in.wind = wind::SteadyGround();
            const float bearing = wind::BearingDegrees() * 3.14159265f / 180.0f;
            in.windDirX = std::cos(bearing);
            in.windDirY = -std::sin(bearing);
            in.night = 1.0f - climate::SunNow().daylight;
            const climate::Weather& w = climate::CurrentWeather();
            in.weatherType = w.type;
            in.precipitation = (w.type == 1 || w.type == 2) ? w.intensity : 0.0f;
            in.rain = w.type == 1 ? w.intensity : 0.0f;
            in.snow = w.type == 2 ? w.intensity : 0.0f;
            in.cooling = Clamp01((in.airHourAgo - in.airNow) / 2.5f); // 2.5 degC an hour = full
            in.valid = true;
            g_inputs = in;
        }

        /// How calm it is for a kind: 1 still air .. 0 at its WindThin and above.
        float Calm(int kind)
        {
            const float thin = g_settings[kind].windThin;
            return thin <= 0.0f ? 0.0f : Clamp01(1.0f - g_inputs.wind / thin);
        }

        /// Fresh wetness of world cell (i, j): the ground moisture above its material's rest (0..1),
        /// from the moisture grid (same 2 yd cells); 0 outside it.
        float ExcessAt(int i, int j)
        {
            const fields::WetGrid& w = fields::Wet();
            if (!w.excess || w.size <= 0) return 0.0f;
            if (i < w.firstI || i >= w.firstI + w.size || j < w.firstJ || j >= w.firstJ + w.size) return 0.0f;
            const int mi = ((i % w.size) + w.size) % w.size, mj = ((j % w.size) + w.size) % w.size;
            return w.excess[mj * w.size + mi] / 255.0f;
        }

        /// The targets of one cell (world i, j at storage index), every kind.
        void CellTargets(int i, int j, int index, float out[kKindCount])
        {
            for (int k = 0; k < kKindCount; ++k) out[k] = 0.0f;
            if (g_block[index] == kStructure) return; // inside or under a WMO
            const float z = g_ground[index];
            const float pos[3] = { (i + 0.5f) * kCellSize, (j + 0.5f) * kCellSize, z + 0.5f };
            float wet = 0.0f, groundTemp = g_inputs.airNow;
            if (g_fieldMoisture >= 0) fields::Sample(g_fieldMoisture, pos, wet);
            if (g_fieldTemperature >= 0) fields::Sample(g_fieldTemperature, pos, groundTemp);

            // Low ground: how far the cell lies below the ground 8 yd around it (8 directions).
            float around = 0.0f;
            int known = 0;
            constexpr int kRing = 4;
            const int di[8] = { kRing, -kRing, 0, 0, kRing, kRing, -kRing, -kRing };
            const int dj[8] = { 0, 0, kRing, -kRing, kRing, -kRing, kRing, -kRing };
            for (int d = 0; d < 8; ++d)
            {
                const int ni = i + di[d], nj = j + dj[d];
                if (ni < g_grid.firstI || ni >= g_grid.firstI + kGridSize || nj < g_grid.firstJ || nj >= g_grid.firstJ + kGridSize) continue;
                const int n = Index(ni, nj);
                if (!g_groundKnown[n]) continue;
                around += g_ground[n];
                ++known;
            }
            const float hollow = known ? Clamp01((around / known - z) / 3.0f) : 0.0f; // 3 yd below = full

            // Water: the cell itself, else the nearest water within 3 cells (6 yd), fading out.
            const uint8_t liquid = g_liquid[index];
            float shore = liquid == 1 ? 1.0f : 0.0f;
            if (!shore)
                for (int r = 1; r <= 3 && !shore; ++r)
                    for (int dj2 = -r; dj2 <= r && !shore; ++dj2)
                        for (int di2 = -r; di2 <= r; ++di2)
                        {
                            const int ni = i + di2, nj = j + dj2;
                            if (ni < g_grid.firstI || ni >= g_grid.firstI + kGridSize || nj < g_grid.firstJ || nj >= g_grid.firstJ + kGridSize) continue;
                            if (g_liquid[Index(ni, nj)] == 1) { shore = 1.0f - r / 4.0f; break; }
                        }

            const RulesInputs& in = g_inputs;
            // Mist: humid air or wet ground, air cooling (or simply night), calm; pools in low ground.
            const float humid = Clamp01(Smooth(0.4f, 0.95f, in.humidity) + 0.5f * wet);
            const float still = std::fmax(in.cooling, 0.7f * in.night);
            out[kMist] = humid * still * Calm(kMist) * (0.4f + 0.6f * hollow) * (1.0f + 0.5f * in.fogginess);
            // Water fog: over water and its shore, the water warmer than the air.
            out[kWaterFog] = shore * Smooth(1.0f, 8.0f, in.water - in.airNow) * Calm(kWaterFog) * (0.5f + 0.5f * in.humidity);
            // Steam: rain or snow on hot ground or magma; hot water steams anyway; and drying ground:
            // fresh wetness (moisture above the ground's rest: recent rain, melt water) steams where
            // the ground is clearly warmer than the air (warm wet ground under cooler air: day or
            // night, rain only halves it), and gives thin wisps on warm sunlit ground.
            const float hot = std::fmax(Smooth(35.0f, 80.0f, groundTemp), liquid == 2 ? 1.0f : 0.0f);
            const float hotSpring = liquid == 1 ? Smooth(35.0f, 60.0f, groundTemp) : 0.0f;
            const float sunWisps = Smooth(12.0f, 30.0f, groundTemp) * (1.0f - in.night) * (1.0f - in.precipitation);
            const float warmer = Smooth(2.0f, 15.0f, groundTemp - in.airNow) * (1.0f - 0.5f * in.precipitation);
            const float drying = 0.5f * ExcessAt(i, j) * std::fmax(sunWisps, warmer);
            out[kSteam] = std::fmax(std::fmax(in.precipitation * hot, hotSpring), drying) * Calm(kSteam);
            // Area fog: the place's own fogginess, more at night.
            out[kAreaFog] = in.fogginess * (0.6f + 0.4f * in.night) * Calm(kAreaFog);
            // Haze: rain (snow less).
            out[kHaze] = (0.8f * in.rain + 0.4f * in.snow) * Calm(kHaze);

            for (int k = 0; k < kKindCount; ++k)
                out[k] = g_settings[k].enabled ? Clamp01(out[k] * g_settings[k].strength) : 0.0f;
        }

        /// A quarter of the grid's rows per step: their targets, and new cells started.
        void RulesPass()
        {
            if (!g_inputs.valid) return;
            if (g_fieldMoisture == -2) g_fieldMoisture = fields::Find("moisture");
            if (g_fieldTemperature == -2) g_fieldTemperature = fields::Find("temperature");
            const double t0 = grassperf::Now();
            for (int r = 0; r < kGridSize / 4; ++r)
            {
                const int j = g_grid.firstJ + g_rulesRow;
                g_rulesRow = (g_rulesRow + 1) % kGridSize;
                for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
                {
                    const int index = Index(i, j);
                    if (!g_groundKnown[index]) continue;
                    float t[kKindCount];
                    CellTargets(i, j, index, t);
                    for (int k = 0; k < kKindCount; ++k)
                    {
                        g_target[k][index] = static_cast<uint8_t>(t[k] * 255.0f + 0.5f);
                        // A cell that just scrolled in starts where the fog around the player has got.
                        if (g_fresh[index])
                        {
                            const float before = g_amount[k][index];
                            g_amount[k][index] = std::fmax(before, t[k] * g_progress[k]);
                            AddFrom(k, index, before, g_amount[k][index], g_ground[index]);
                        }
                    }
                    g_fresh[index] = 0;
                }
            }
            g_stats.rulesMs = grassperf::Now() - t0;
        }

        bool InGrid(int i, int j) { return i >= g_grid.firstI && i < g_grid.firstI + kGridSize && j >= g_grid.firstJ && j < g_grid.firstJ + kGridSize; }
        bool Open(int index) { return g_groundKnown[index] && g_block[index] != kStructure; }

        /// The wind carries every kind by its WindDrift: each cell takes the fog from upwind
        /// (bilinear, so less than a cell per step works). Fog blown off the grid or into a WMO is gone;
        /// the upwind edge refills from the rules.
        void Advect(float dt)
        {
            static std::vector<float> next(kGridSize * kGridSize), nextFloor(kGridSize * kGridSize);
            for (int k = 0; k < kKindCount; ++k)
            {
                const float drift = g_settings[k].windDrift;
                if (drift <= 0.0f || g_grid.maxAmount[k] <= 0.0f) continue;
                const float speed = g_inputs.wind * drift * 4.0f * dt / kCellSize; // cells this step (4 yd/s at full wind)
                if (speed < 1.0e-3f) continue;
                const float sx = g_inputs.windDirX * speed, sy = g_inputs.windDirY * speed;
                const std::vector<float>& a = g_amount[k];
                const bool anchored = kAnchored[k];
                auto at = [&](int i, int j) { if (!InGrid(i, j)) return 0.0f; const int n = Index(i, j); return Open(n) ? a[n] : 0.0f; };
                for (int j = g_grid.firstJ; j < g_grid.firstJ + kGridSize; ++j)
                    for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
                    {
                        const int index = Index(i, j);
                        if (!Open(index))
                        {
                            next[index] = 0.0f;
                            if (anchored) nextFloor[index] = g_floor[k][index];
                            continue;
                        }
                        const float fx = i - sx, fy = j - sy;
                        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                        const float tx = fx - x0, ty = fy - y0;
                        // Bilinear from the four upwind cells; an anchored kind's floor comes along,
                        // weighted by the fog each one gives.
                        const int ci[4] = { x0, x0 + 1, x0, x0 + 1 }, cj[4] = { y0, y0, y0 + 1, y0 + 1 };
                        const float w[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
                        float sum = 0.0f, floorSum = 0.0f;
                        for (int s = 0; s < 4; ++s)
                        {
                            const float m = w[s] * at(ci[s], cj[s]);
                            sum += m;
                            if (anchored && m > 0.0f) floorSum += m * g_floor[k][Index(ci[s], cj[s])];
                        }
                        next[index] = sum;
                        if (anchored) nextFloor[index] = sum > 0.0f ? floorSum / sum : g_floor[k][index];
                    }
                std::copy(next.begin(), next.end(), g_amount[k].begin());
                if (anchored) std::copy(nextFloor.begin(), nextFloor.end(), g_floor[k].begin());
            }
        }

        /// Heavy fog flows downhill: per cell, the heavy depth (sum over the kinds of Weight x amount x
        /// Thickness) on top of the ground is a surface; fog moves toward neighbours whose surface is
        /// lower, each kind by its share of the depth, so it runs down slopes and levels out in hollows
        /// (where it may pile up past 1, to kMaxPool). Not into WMO structures.
        void Flow(float dt)
        {
            bool any = false;
            for (int k = 0; k < kKindCount; ++k) any = any || (g_settings[k].weight > 0.0f && g_grid.maxAmount[k] > 0.0f);
            if (!any) return;
            static std::vector<float> depth(kGridSize * kGridSize), delta[kKindCount];
            // Anchored kinds: what flows into each cell, and its floors (amount-weighted).
            static std::vector<float> inflow[kKindCount], inflowFloor[kKindCount];
            for (int k = 0; k < kKindCount; ++k)
            {
                delta[k].assign(kGridSize * kGridSize, 0.0f);
                if (kAnchored[k])
                {
                    inflow[k].assign(kGridSize * kGridSize, 0.0f);
                    inflowFloor[k].assign(kGridSize * kGridSize, 0.0f);
                }
            }
            for (int n = 0; n < kGridSize * kGridSize; ++n)
            {
                float d = 0.0f;
                for (int k = 0; k < kKindCount; ++k) d += g_settings[k].weight * g_amount[k][n] * g_settings[k].thickness;
                depth[n] = d;
            }
            const float rate = std::fmin(1.0f, dt * 1.0f); // share of a surface difference that evens out per second
            const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
            for (int j = g_grid.firstJ; j < g_grid.firstJ + kGridSize; ++j)
                for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
                {
                    const int c = Index(i, j);
                    if (depth[c] < 1.0e-3f || !Open(c)) continue;
                    const float surface = g_ground[c] + depth[c];
                    float out[4] = {}, total = 0.0f;
                    for (int d = 0; d < 4; ++d)
                    {
                        const int ni = i + di[d], nj = j + dj[d];
                        if (!InGrid(ni, nj)) continue;
                        const int n = Index(ni, nj);
                        if (!Open(n)) continue;
                        const float drop = surface - (g_ground[n] + depth[n]);
                        if (drop > 0.0f) { out[d] = 0.25f * drop * rate; total += out[d]; }
                    }
                    if (total <= 0.0f) continue;
                    const float scale = total > 0.5f * depth[c] ? 0.5f * depth[c] / total : 1.0f; // never more than half per step
                    for (int d = 0; d < 4; ++d)
                    {
                        if (out[d] <= 0.0f) continue;
                        const int n = Index(i + di[d], j + dj[d]);
                        const float moved = out[d] * scale; // yd of heavy depth
                        for (int k = 0; k < kKindCount; ++k)
                        {
                            const KindSettings& s = g_settings[k];
                            if (s.weight <= 0.0f || g_amount[k][c] <= 0.0f || s.thickness <= 0.0f) continue;
                            const float share = s.weight * g_amount[k][c] * s.thickness / depth[c];
                            const float amount = moved * share / (s.weight * s.thickness);
                            delta[k][c] -= amount;
                            delta[k][n] += amount;
                            if (kAnchored[k])
                            {
                                inflow[k][n] += amount;
                                inflowFloor[k][n] += amount * g_floor[k][c];
                            }
                        }
                    }
                }
            for (int k = 0; k < kKindCount; ++k)
                for (int n = 0; n < kGridSize * kGridSize; ++n)
                {
                    if (delta[k][n] == 0.0f) continue;
                    const float before = g_amount[k][n];
                    g_amount[k][n] = std::fmin(kMaxPool, std::fmax(0.0f, before + delta[k][n]));
                    // An anchored kind's floor: what stayed keeps its own, what came in brings its.
                    if (kAnchored[k] && inflow[k][n] > 0.0f)
                    {
                        const float kept = std::fmax(0.0f, before + delta[k][n] - inflow[k][n]);
                        g_floor[k][n] = (g_floor[k][n] * kept + inflowFloor[k][n]) / (kept + inflow[k][n]);
                    }
                }
        }

        /// One step: every amount toward its target at the kind's rates (build-up toward a higher one,
        /// burn-off toward a lower one), and how far the fog has come toward its targets overall.
        void Step(float dt)
        {
            Advect(dt);
            Flow(dt);
            bool changed = false;
            for (int k = 0; k < kKindCount; ++k)
            {
                const KindSettings& s = g_settings[k];
                const float up = 1.0f - std::exp2(-dt / (s.buildUpSeconds > 0.1f ? s.buildUpSeconds : 0.1f));
                const float down = 1.0f - std::exp2(-dt / (s.burnOffSeconds > 0.1f ? s.burnOffSeconds : 0.1f));
                float most = 0.0f, sumAmount = 0.0f, sumTarget = 0.0f;
                unsigned cells = 0;
                const uint8_t* target = g_target[k].data();
                float* amount = g_amount[k].data();
                for (int n = 0; n < kGridSize * kGridSize; ++n)
                {
                    float a = amount[n];
                    const float t = target[n] * (1.0f / 255.0f);
                    if (a == 0.0f && t == 0.0f) continue;
                    const float before = a;
                    a += (t - a) * (t > a ? up : down);
                    if (a < kGone && t == 0.0f) a = 0.0f;
                    amount[n] = a;
                    AddFrom(k, n, before, a, g_ground[n]); // forming here: from this cell's surface
                    if (t > 0.02f) { sumAmount += a < t ? a : t; sumTarget += t; }
                    if (a > 0.0f) { ++cells; if (a > most) most = a; }
                }
                if (sumTarget > 0.0f) g_progress[k] = sumAmount / sumTarget;
                g_grid.maxAmount[k] = most;
                g_stats.cellsWithFog[k] = cells;
                changed = changed || cells;
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

    bool GroundAt(float x, float y, float& z, int& block)
    {
        if (g_ground.empty()) return false;
        const int i = static_cast<int>(std::floor(x / kCellSize));
        const int j = static_cast<int>(std::floor(y / kCellSize));
        if (i < g_grid.firstI || i >= g_grid.firstI + kGridSize || j < g_grid.firstJ || j >= g_grid.firstJ + kGridSize) return false;
        const int index = Index(i, j);
        if (!g_groundKnown[index]) return false;
        z = g_ground[index];
        block = g_block[index];
        return true;
    }

    const RulesInputs& Inputs() { return g_inputs; }
    float Progress(int kind) { return kind >= 0 && kind < kKindCount ? g_progress[kind] : 0.0f; }

    float TargetAt(int kind, float x, float y)
    {
        if (kind < 0 || kind >= kKindCount || g_target[kind].empty()) return 0.0f;
        const int i = static_cast<int>(std::floor(x / kCellSize));
        const int j = static_cast<int>(std::floor(y / kCellSize));
        if (i < g_grid.firstI || i >= g_grid.firstI + kGridSize || j < g_grid.firstJ || j >= g_grid.firstJ + kGridSize) return 0.0f;
        return g_target[kind][Index(i, j)] / 255.0f;
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
        TraceCells();

        g_tickTime += dt;
        if (g_tickTime < kTickSeconds) return;
        const float step = g_tickTime;
        g_tickTime = 0.0f;
        if (g_paused) return;

        const double t0 = grassperf::Now();
        GatherInputs(snap);
        RulesPass();
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
                const float before = a;
                a = std::fmin(1.0f, a + amount * w);
                AddFrom(kind, Index(i, j), before, a, g_ground[Index(i, j)]);
                if (a > most) most = a;
            }
        g_grid.maxAmount[kind] = most;
        ++g_grid.version;
    }

    void Clear() { if (!g_amount[0].empty()) ClearAll(); }

    void FastForward()
    {
        if (g_amount[0].empty()) return;
        for (int k = 0; k < kKindCount; ++k)
            for (int n = 0; n < kGridSize * kGridSize; ++n)
            {
                g_amount[k][n] = g_target[k][n] / 255.0f;
                if (kAnchored[k]) g_floor[k][n] = g_ground[n]; // as if it had formed in place
            }
        ++g_grid.version;
    }
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
