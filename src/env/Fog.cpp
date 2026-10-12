#include "Fog.hpp"

#include "Climate.hpp"
#include "Fields.hpp"
#include "FogSettings.hpp"
#include "Regional.hpp"
#include "TerrainHeight.hpp"
#include "Wind.hpp"

#include "../features/GrassPerf.hpp"

#include "game/Pick.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
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
        std::vector<float> g_pool[kKindCount];   // heavy fog (Grid::pool)
        std::vector<uint8_t> g_steep;            // Grid::steep
        std::vector<float>   g_slope;            // degrees, per cell (the breakdown)
        float    g_maxPool[kKindCount] = {};
        FieldSettings g_field;

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
        std::vector<uint32_t> g_area;    // the cell's area (its terrain chunk's), 0 = not known: the player's place
        std::vector<uint8_t> g_fresh;    // scrolled in, not started yet (starts from its target x the area's progress)
        // How far the fog around the player has come toward its targets. 1 on a fresh grid (login,
        // teleport, new map): a place you arrive at shows its fog as if the conditions had held.
        float    g_progress[kKindCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        RulesInputs g_inputs;
        uint32_t    g_playerArea = 0;
        /// A place's own climate inputs (per area: fogginess, humidity, temperatures) and its kinds'
        /// Strength from FogSettings; time of day, wind and weather stay the player's (the weather is
        /// the only one the client knows, and it rains around the player alike). Cached per area,
        /// cleared with each GatherInputs.
        struct AreaInputs
        {
            uint32_t zone = 0;
            uint16_t slot = 0;   // its kind settings (g_slots)
            float airNow = 0.0f, airHourAgo = 0.0f, water = 0.0f, humidity = 0.0f, fogginess = 0.0f, cooling = 0.0f;
            float strength[kKindCount] = {};
        };
        std::unordered_map<uint32_t, AreaInputs> g_areaInputs;

        // Each place's kind settings (its FogSettings rows: density, thickness, weight, rates, wind
        // thinning, strength), so a cell's fog behaves like its own place's, not the player's. A slot
        // per area seen this session; a cell keeps its slot number (g_areaSlot). Slot 0 = the
        // player's place (the panel's g_settings): cells whose area isn't known yet. A kind switched to
        // the panel's values uses them everywhere. Wind drift stays the player's (the field moves as one).
        struct AreaSlot { uint32_t area = 0; KindSettings kind[kKindCount]; };
        std::vector<AreaSlot> g_slots(1);
        std::unordered_map<uint32_t, uint16_t> g_slotOfArea;
        std::vector<uint16_t> g_areaSlot; // per cell
        // Per cell, the blend toward the strongest other place around (climate::AreaWeightsAt, the
        // climate's own ~100 yd blend): its slot and its share (0..255). Settings switched at every
        // area border (area fog 0.15/2 yd vs 0.03/8 yd: a visible seam; probe, 2026-10-12). 3 bytes a cell.
        std::vector<uint16_t> g_blendSlot;
        std::vector<uint8_t>  g_blendShare;
        uint32_t g_slotsGeneration = ~0u;
        int      g_slotsMap = -2;
        uint16_t SlotOf(uint32_t area);
        int      g_rulesRow = 0;         // the next grid row the rules evaluate (a quarter per step)
        // After a table reload (AreaClimate, FogSettings): this many rows still jump straight to their
        // new targets as the rules reach them, like a fresh grid (else a changed Fogginess shows only
        // after the kind's build-up, 10 min for area fog). 0 in play.
        int      g_reseedRows = 0;
        uint32_t g_seenClimate = 0, g_seenFogSettings = 0;
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
                g_pool[k].assign(kGridSize * kGridSize, 0.0f);
                g_grid.pool[k] = g_pool[k].data();
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
            g_area.assign(kGridSize * kGridSize, 0);
            g_areaSlot.assign(kGridSize * kGridSize, 0);
            g_blendSlot.assign(kGridSize * kGridSize, 0);
            g_blendShare.assign(kGridSize * kGridSize, 0);
            g_grid.area = g_area.data();
            g_fresh.assign(kGridSize * kGridSize, 1);
            g_grid.fresh = g_fresh.data();
            g_steep.assign(kGridSize * kGridSize, 0);
            g_slope.assign(kGridSize * kGridSize, 0.0f);
            g_grid.steep = g_steep.data();
            g_grid.ground = g_ground.data();
            g_grid.groundKnown = g_groundKnown.data();
            g_grid.block = g_block.data();
            g_stats.kilobytes = static_cast<unsigned>((kKindCount * (2 * sizeof(float) + 1) + 4 * sizeof(float) + sizeof(uint32_t) + 4 + 3) * kGridSize * kGridSize / 1024);
        }

        void ForgetGround(int index)
        {
            g_groundKnown[index] = 0;
            g_ground[index] = g_terrain[index] = g_groundFallback;
            g_block[index] = kUntraced;
            g_liquid[index] = 0;
            g_area[index] = 0;
            g_areaSlot[index] = 0;
            g_blendShare[index] = 0;
            g_steep[index] = 0;
            g_slope[index] = 0.0f;
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
                for (int k = 0; k < kKindCount; ++k) g_amount[k][index] = g_pool[k][index] = 0.0f;
                ForgetGround(index);
            }
        }
        void ClearRow(int j)
        {
            for (int c = 0; c < kGridSize; ++c)
            {
                const int index = Wrap(j) * kGridSize + c;
                for (int k = 0; k < kKindCount; ++k) g_amount[k][index] = g_pool[k][index] = 0.0f;
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
                uint32_t area = 0;
                g_area[index] = terrain::ChunkAreaAt(x, y, area) ? area : 0;
                g_areaSlot[index] = SlotOf(g_area[index]);
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
                    for (int k = 0; k < kKindCount; ++k) { g_amount[k][index] = g_pool[k][index] = 0.0f; g_target[k][index] = 0; }
                changed = true;
            }
            if (changed) { ++g_grid.groundVersion; ++g_grid.version; }
        }

        void ClearAll()
        {
            for (int k = 0; k < kKindCount; ++k)
            {
                std::fill(g_amount[k].begin(), g_amount[k].end(), 0.0f);
                std::fill(g_pool[k].begin(), g_pool[k].end(), 0.0f);
                g_grid.maxAmount[k] = 0.0f;
                g_maxPool[k] = 0.0f;
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
            g_playerArea = area;
            g_areaInputs.clear(); // each place's inputs, computed again as the rules ask for them
            const climate::Row row = climate::At(snap.playerPos[0], snap.playerPos[1]); // blended across borders
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

        /// One slot's kind settings: the place's FogSettings rows over the built-in values, except
        /// kinds on the panel's values (those everywhere); on/off is always the panel's.
        void ResolveSlot(AreaSlot& s)
        {
            for (int k = 0; k < kKindCount; ++k)
            {
                if (g_settings[k].panelOverride || !s.area) { s.kind[k] = g_settings[k]; continue; }
                s.kind[k] = MakeDefaults(k);
                Apply(s.kind[k], fogsettings::Resolve(s.area, g_map, k));
                s.kind[k].enabled = g_settings[k].enabled;
            }
        }

        /// A place's slot (added on first use; 0 for an unknown area).
        uint16_t SlotOf(uint32_t area)
        {
            if (!area) return 0;
            auto found = g_slotOfArea.find(area);
            if (found != g_slotOfArea.end()) return found->second;
            if (g_slots.size() >= 0xFFFF) return 0;
            AreaSlot s;
            s.area = area;
            ResolveSlot(s);
            g_slots.push_back(s);
            const uint16_t slot = static_cast<uint16_t>(g_slots.size() - 1);
            g_slotOfArea.emplace(area, slot);
            return slot;
        }

        /// Each field tick: slot 0 follows the player's place; every slot re-resolves when the table
        /// is reloaded, a panel value changes or the map changes (cheap: compares a signature).
        void RefreshSlots()
        {
            g_slots[0].area = 0;
            for (int k = 0; k < kKindCount; ++k) g_slots[0].kind[k] = g_settings[k];
            uint32_t signature = fogsettings::Generation() * 2654435761u;
            for (int k = 0; k < kKindCount; ++k)
            {
                const KindSettings& p = g_settings[k];
                if (!p.panelOverride && p.enabled) continue;
                const float v[] = { p.strength, p.buildUpSeconds, p.burnOffSeconds, p.thickness, p.density, p.windThin, p.weight };
                for (float f : v) { uint32_t b; std::memcpy(&b, &f, 4); signature = (signature ^ b) * 16777619u; }
                signature = (signature ^ static_cast<uint32_t>(k * 7 + p.enabled * 3 + p.panelOverride)) * 16777619u;
            }
            if (signature == g_slotsGeneration && g_map == g_slotsMap) return;
            // (Area IDs are global, only map rows depend on the map: a new map re-resolves, too.)
            g_slotsGeneration = signature;
            g_slotsMap = g_map;
            for (size_t s = 1; s < g_slots.size(); ++s) ResolveSlot(g_slots[s]);
        }

        /// Two places' settings mixed (share t of b): the numbers blend, on/off and the panel switch
        /// come from the larger share.
        KindSettings Mix(const KindSettings& a, const KindSettings& b, float t)
        {
            KindSettings r = t < 0.5f ? a : b;
            auto lerp = [t](float x, float y) { return x + (y - x) * t; };
            r.strength = lerp(a.strength, b.strength);
            r.buildUpSeconds = lerp(a.buildUpSeconds, b.buildUpSeconds);
            r.burnOffSeconds = lerp(a.burnOffSeconds, b.burnOffSeconds);
            r.thickness = lerp(a.thickness, b.thickness);
            r.density = lerp(a.density, b.density);
            r.windThin = lerp(a.windThin, b.windThin);
            r.windDrift = lerp(a.windDrift, b.windDrift);
            r.patchiness = lerp(a.patchiness, b.patchiness);
            r.weight = lerp(a.weight, b.weight);
            return r;
        }

        /// A cell's kind settings: its place's, blended toward the strongest other place around.
        KindSettings CellSettings(int kind, int index)
        {
            const KindSettings& own = g_slots[g_areaSlot[index]].kind[kind];
            const uint8_t share = g_blendShare[index];
            if (!share) return own;
            return Mix(own, g_slots[g_blendSlot[index]].kind[kind], share / 255.0f);
        }

        /// The cell's blend (g_blendSlot / g_blendShare) from the climate's area weights at its centre:
        /// toward the strongest other area, by its weight against its own area's.
        void UpdateBlend(int i, int j, int index)
        {
            uint32_t areas[16];
            float weights[16];
            const int n = climate::AreaWeightsAt((i + 0.5f) * kCellSize, (j + 0.5f) * kCellSize, areas, weights, 16);
            const uint32_t own = g_area[index];
            float wOwn = 0.0f, wOther = 0.0f;
            uint32_t other = 0;
            for (int m = 0; m < n; ++m)
            {
                if (areas[m] == own) wOwn = weights[m];
                else if (weights[m] > wOther) { wOther = weights[m]; other = areas[m]; }
            }
            if (!own || !other || wOther <= 0.0f) { g_blendShare[index] = 0; return; }
            g_blendSlot[index] = SlotOf(other);
            g_blendShare[index] = static_cast<uint8_t>(wOther / (wOwn + wOther) * 255.0f + 0.5f);
        }

        /// How calm it is for a kind at a place: 1 still air .. 0 at its WindThin and above.
        float CalmFor(const KindSettings& s)
        {
            return s.windThin <= 0.0f ? 0.0f : Clamp01(1.0f - g_inputs.wind / s.windThin);
        }

        /// The climate parts of a place's inputs, from a climate row.
        void SetClimate(AreaInputs& a, const climate::Row& row)
        {
            a.airNow = climate::Temperature(row).total;
            a.airHourAgo = climate::TemperatureAtTime(row, climate::Now().dayFraction - 1.0f / 24.0f).total;
            a.water = climate::WaterTemperature(row);
            a.humidity = row.humidity;
            a.fogginess = row.fogginess;
            a.cooling = Clamp01((a.airHourAgo - a.airNow) / 2.5f);
        }

        /// A place's inputs (see AreaInputs), cached per area until the next GatherInputs.
        const AreaInputs& InputsFor(uint32_t area)
        {
            auto found = g_areaInputs.find(area);
            if (found != g_areaInputs.end()) return found->second;
            AreaInputs a;
            SetClimate(a, climate::For(area, g_map)); // the area's own row: zones' typical fog (cells use At)
            a.zone = area ? regional::ZoneOf(area) : 0;
            a.slot = SlotOf(area);
            for (int k = 0; k < kKindCount; ++k) a.strength[k] = g_slots[a.slot].kind[k].strength;
            return g_areaInputs.emplace(area, a).first->second;
        }

        // --- each zone's typical fog (the memory; design doc "Phase 3 step 5") ---
        // Kinds that are zone-wide by nature; water fog and steam need water or heat detail.
        constexpr bool kZoneWide[kKindCount] = { true, false, false, true, true };
        constexpr float kAverageHollow = 0.33f; // the average-ground factor for mist in a zone
        struct ZoneFog
        {
            int      map = -1;
            uint32_t zone = 0;
            float    amount[kKindCount] = {};
            float    target[kKindCount] = {};
            bool     started = false;
            double   lastInRange = 0.0;
        };
        std::vector<ZoneFog> g_zoneFog;

        /// A zone's targets for the zone-wide kinds (an ordinary open spot there).
        void ZoneTargets(uint32_t zone, float out[kKindCount])
        {
            for (int k = 0; k < kKindCount; ++k) out[k] = 0.0f;
            const AreaInputs& a = InputsFor(zone);
            const RulesInputs& in = g_inputs;
            auto Calm = [&](int k) { return CalmFor(g_slots[a.slot].kind[k]); }; // the zone's own WindThin
            const float wet = Clamp01(regional::RainSoak(g_map, zone));
            const float humid = Clamp01(Smooth(0.4f, 0.95f, a.humidity) + 0.5f * wet);
            const float still = std::fmax(a.cooling, 0.7f * in.night);
            out[kMist] = humid * still * Calm(kMist) * (0.4f + 0.6f * kAverageHollow) * (1.0f + 0.5f * a.fogginess);
            out[kAreaFog] = a.fogginess * (0.6f + 0.4f * in.night) * Calm(kAreaFog);
            out[kHaze] = zone == regional::PlayerZone() ? (0.8f * in.rain + 0.4f * in.snow) * Calm(kHaze) : 0.0f; // others: weather unknown
            for (int k = 0; k < kKindCount; ++k)
                out[k] = g_settings[k].enabled && kZoneWide[k] ? Clamp01(out[k] * a.strength[k]) : 0.0f;
        }

        /// Every zone on the zone map (±1 km) and the player's: started at its target when it first
        /// comes into range (it's far away then: nothing jumps), then toward its target at the kinds'
        /// rates, also while out of range (session only).
        void StepZones(float dt)
        {
            const regional::ZoneMap& map = regional::Map();
            uint32_t seen[32];
            int count = 0;
            auto note = [&](uint32_t z) {
                if (!z || count >= 32) return;
                for (int n = 0; n < count; ++n) if (seen[n] == z) return;
                seen[count++] = z;
            };
            note(regional::PlayerZone());
            if (map.zone)
                for (int n = 0; n < map.size * map.size; ++n) note(map.zone[n]);
            const double now = regional::Now();
            for (int n = 0; n < count; ++n)
            {
                ZoneFog* z = nullptr;
                for (ZoneFog& f : g_zoneFog) if (f.map == g_map && f.zone == seen[n]) { z = &f; break; }
                if (!z) { g_zoneFog.push_back(ZoneFog{}); z = &g_zoneFog.back(); z->map = g_map; z->zone = seen[n]; }
                z->lastInRange = now;
            }
            for (ZoneFog& z : g_zoneFog)
            {
                if (z.map != g_map) continue;
                ZoneTargets(z.zone, z.target);
                for (int k = 0; k < kKindCount; ++k)
                {
                    if (!z.started) { z.amount[k] = z.target[k]; continue; }
                    const KindSettings& s = g_slots[SlotOf(z.zone)].kind[k]; // the zone's own rates
                    const float up = 1.0f - std::exp2(-dt / (s.buildUpSeconds > 0.1f ? s.buildUpSeconds : 0.1f));
                    const float down = 1.0f - std::exp2(-dt / (s.burnOffSeconds > 0.1f ? s.burnOffSeconds : 0.1f));
                    z.amount[k] += (z.target[k] - z.amount[k]) * (z.target[k] > z.amount[k] ? up : down);
                }
                z.started = true;
            }
        }

        const ZoneFog* FindZoneFog(uint32_t zone)
        {
            for (const ZoneFog& z : g_zoneFog) if (z.map == g_map && z.zone == zone) return &z;
            return nullptr;
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

        Explanation g_explain[kKindCount];

        /// The targets of one cell (world i, j at storage index), every kind. With `explain`, each
        /// kind's factors are recorded too (the player's cell, for the panel).
        void CellTargets(int i, int j, int index, float out[kKindCount], Explanation* explain = nullptr)
        {
            for (int k = 0; k < kKindCount; ++k) out[k] = 0.0f;
            auto note = [&](int kind, const char* name, float input, float factor, bool alternative = false) {
                if (!explain) return;
                Explanation& e = explain[kind];
                if (e.count < 10) e.factors[e.count++] = { name, input, factor, alternative };
            };
            if (explain)
                for (int k = 0; k < kKindCount; ++k) { explain[k].count = 0; explain[k].valid = true; }
            if (g_block[index] == kStructure)
            {
                for (int k = 0; k < kKindCount; ++k) { note(k, "under/in a WMO structure", 1.0f, 0.0f); if (explain) explain[k].target = 0.0f; }
                return;
            }
            // The cell's own place (its chunk's area; the player's while not known yet): its kind
            // settings and Strength. Its climate is the blended one at the cell (climate::At), so a
            // border's change in temperature, humidity and fogginess spreads over ~100 yd.
            const float z = g_ground[index];
            const float pos[3] = { (i + 0.5f) * kCellSize, (j + 0.5f) * kCellSize, z + 0.5f };
            AreaInputs a = InputsFor(g_area[index] ? g_area[index] : g_playerArea);
            SetClimate(a, climate::At(pos[0], pos[1]));
            // The cell's settings (its place's, blended across borders like the climate): WindThin, Strength.
            KindSettings cell[kKindCount];
            for (int k = 0; k < kKindCount; ++k) cell[k] = CellSettings(k, index);
            auto Calm = [&](int k) { return CalmFor(cell[k]); };
            float wet = 0.0f, groundTemp = a.airNow;
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

            const RulesInputs& in = g_inputs; // the player's: time of day, wind, weather
            // Mist: humid air or wet ground, air cooling (or simply night), calm; pools in low ground.
            const float humid = Clamp01(Smooth(0.4f, 0.95f, a.humidity) + 0.5f * wet);
            const float still = std::fmax(a.cooling, 0.7f * in.night);
            out[kMist] = humid * still * Calm(kMist) * (0.4f + 0.6f * hollow) * (1.0f + 0.5f * a.fogginess);
            note(kMist, "humid air / wet ground (humidity)", a.humidity, humid);
            note(kMist, "  ... ground wetness", wet, -1.0f);
            note(kMist, "cooling or night (cooling)", a.cooling, still);
            note(kMist, "  ... night", in.night, -1.0f);
            note(kMist, "calm (wind)", in.wind, Calm(kMist));
            note(kMist, "low ground (hollow, 1 = 3 yd below)", hollow, 0.4f + 0.6f * hollow);
            note(kMist, "the place's fogginess", a.fogginess, 1.0f + 0.5f * a.fogginess);
            // Water fog: over water and its shore, the water warmer than the air.
            const float waterWarmer = Smooth(1.0f, 8.0f, a.water - a.airNow);
            out[kWaterFog] = shore * waterWarmer * Calm(kWaterFog) * (0.5f + 0.5f * a.humidity);
            note(kWaterFog, "water here / shore (1 on water)", shore, shore);
            note(kWaterFog, "water warmer than air (degC)", a.water - a.airNow, waterWarmer);
            note(kWaterFog, "calm (wind)", in.wind, Calm(kWaterFog));
            note(kWaterFog, "humidity", a.humidity, 0.5f + 0.5f * a.humidity);
            // Steam: rain or snow on hot ground or magma; hot water steams anyway; and drying ground:
            // fresh wetness (moisture above the ground's rest: recent rain, melt water) steams where
            // the ground is clearly warmer than the air (warm wet ground under cooler air: day or
            // night, rain only halves it), and gives thin wisps on warm sunlit ground.
            const float hot = std::fmax(Smooth(35.0f, 80.0f, groundTemp), liquid == 2 ? 1.0f : 0.0f);
            const float hotSpring = liquid == 1 ? Smooth(35.0f, 60.0f, groundTemp) : 0.0f;
            const float sunWisps = Smooth(12.0f, 30.0f, groundTemp) * (1.0f - in.night) * (1.0f - in.precipitation);
            const float warmer = Smooth(2.0f, 15.0f, groundTemp - a.airNow) * (1.0f - 0.5f * in.precipitation);
            const float drying = 0.5f * ExcessAt(i, j) * std::fmax(sunWisps, warmer);
            out[kSteam] = std::fmax(std::fmax(in.precipitation * hot, hotSpring), drying) * Calm(kSteam);
            note(kSteam, "rain/snow on hot ground (ground degC)", groundTemp, in.precipitation * hot, true);
            note(kSteam, "hot spring (water, ground degC)", groundTemp, hotSpring, true);
            note(kSteam, "wet ground drying (moisture above rest)", ExcessAt(i, j), drying, true);
            note(kSteam, "  ... warmer than the air (degC)", groundTemp - a.airNow, warmer);
            note(kSteam, "  ... sun wisps (ground degC)", groundTemp, sunWisps);
            note(kSteam, "calm (wind)", in.wind, Calm(kSteam));
            // Area fog: the place's own fogginess, more at night.
            out[kAreaFog] = a.fogginess * (0.6f + 0.4f * in.night) * Calm(kAreaFog);
            note(kAreaFog, "the place's fogginess (AreaClimate)", a.fogginess, a.fogginess);
            note(kAreaFog, "time of day (night)", in.night, 0.6f + 0.4f * in.night);
            note(kAreaFog, "calm (wind)", in.wind, Calm(kAreaFog));
            // Haze: rain (snow less).
            out[kHaze] = (0.8f * in.rain + 0.4f * in.snow) * Calm(kHaze);
            note(kHaze, "rain", in.rain, 0.8f * in.rain + 0.4f * in.snow);
            note(kHaze, "  ... snow", in.snow, -1.0f);
            note(kHaze, "calm (wind)", in.wind, Calm(kHaze));

            // Steep ground (SteepStart .. SteepFull): fog doesn't hang there, the ground-hugging kinds
            // don't form (water fog and steam hang at their source's height instead).
            const float clearOfSteep = 1.0f - g_steep[index] / 255.0f;
            for (int k = 0; k < kKindCount; ++k)
                if (!kAnchored[k])
                {
                    note(k, "steep ground (slope degrees)", g_slope[index], clearOfSteep);
                    out[k] *= clearOfSteep;
                }
            for (int k = 0; k < kKindCount; ++k)
            {
                note(k, g_settings[k].enabled ? "Strength (FogSettings, this place, blended)" : "kind switched off", cell[k].strength,
                     g_settings[k].enabled ? cell[k].strength : 0.0f);
                out[k] = g_settings[k].enabled ? Clamp01(out[k] * cell[k].strength) : 0.0f;
                if (explain) explain[k].target = out[k];
            }
        }

        /// A cell's slope (degrees) from its known neighbours' ground, ±1 cell, and its steep share.
        void UpdateSlope(int i, int j, int index)
        {
            auto ground = [&](int ni, int nj, float fallback) {
                if (ni < g_grid.firstI || ni >= g_grid.firstI + kGridSize || nj < g_grid.firstJ || nj >= g_grid.firstJ + kGridSize) return fallback;
                const int n = Index(ni, nj);
                return g_groundKnown[n] ? g_ground[n] : fallback;
            };
            const float z = g_ground[index];
            const float gx = (ground(i + 1, j, z) - ground(i - 1, j, z)) / (2.0f * kCellSize);
            const float gy = (ground(i, j + 1, z) - ground(i, j - 1, z)) / (2.0f * kCellSize);
            const float degrees = std::atan(std::sqrt(gx * gx + gy * gy)) * 57.2957795f;
            g_slope[index] = degrees;
            g_steep[index] = static_cast<uint8_t>(Smooth(g_field.steepStart, std::fmax(g_field.steepFull, g_field.steepStart + 0.1f), degrees) * 255.0f + 0.5f);
        }

        /// A quarter of the grid's rows per step: their targets, and new cells started.
        void RulesPass()
        {
            if (!g_inputs.valid) return;
            if (g_fieldMoisture == -2) g_fieldMoisture = fields::Find("moisture");
            if (g_fieldTemperature == -2) g_fieldTemperature = fields::Find("temperature");
            const double t0 = grassperf::Now();
            static std::vector<int> startedNow;
            startedNow.clear();
            for (int r = 0; r < kGridSize / 4; ++r)
            {
                const int j = g_grid.firstJ + g_rulesRow;
                g_rulesRow = (g_rulesRow + 1) % kGridSize;
                const bool reseed = g_reseedRows > 0;
                if (reseed) --g_reseedRows;
                for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
                {
                    const int index = Index(i, j);
                    if (!g_groundKnown[index]) continue;
                    UpdateBlend(i, j, index);
                    UpdateSlope(i, j, index);
                    float t[kKindCount];
                    CellTargets(i, j, index, t);
                    for (int k = 0; k < kKindCount; ++k)
                    {
                        g_target[k][index] = static_cast<uint8_t>(t[k] * 255.0f + 0.5f);
                        const float weight = HeavyShare(k, index);
                        if (reseed)
                        {
                            g_amount[k][index] = t[k];
                            g_pool[k][index] = weight * t[k];
                            if (kAnchored[k]) g_floor[k][index] = g_ground[index];
                        }
                        // A cell that just scrolled in continues its running neighbours: their progress
                        // (amount / target, averaged) applied to its own target, so where the targets
                        // match it starts at the neighbours' amount, and a hilltop next to a hollow still
                        // starts at its own lower target. Where their targets have gone but fog lingers,
                        // their amount (minus the steep share). Neighbours started in this same pass
                        // don't count (after a teleport one row would be copied over the grid). Each new
                        // strip at the field's edge used to start from an estimate (the zone's progress)
                        // and then relax for minutes (mist: 300 s half-life): bands of rows at a
                        // distance, differing while their targets were equal (probe cross-sections,
                        // 2026-10-12). No running neighbour: the zone's progress (zone-wide kinds), else
                        // the progress around the player.
                        else if (g_fresh[index] == 1)
                        {
                            const float before = g_amount[k][index];
                            float sumRatio = 0.0f, sumAmount = 0.0f;
                            int ratios = 0, amounts = 0;
                            for (int dj = -1; dj <= 1; ++dj)
                                for (int di = -1; di <= 1; ++di)
                                {
                                    const int ni = i + di, nj = j + dj;
                                    if ((di == 0 && dj == 0) || ni < g_grid.firstI || ni >= g_grid.firstI + kGridSize ||
                                        nj < g_grid.firstJ || nj >= g_grid.firstJ + kGridSize) continue;
                                    const int n = Index(ni, nj);
                                    if (!g_groundKnown[n] || g_fresh[n] != 0) continue;
                                    const float tn = g_target[k][n] / 255.0f;
                                    if (tn > 0.01f) { sumRatio += g_amount[k][n] / tn; ++ratios; }
                                    sumAmount += g_amount[k][n];
                                    ++amounts;
                                }
                            float start;
                            if (t[k] > 0.01f && ratios) start = t[k] * std::fmin(sumRatio / ratios, 2.0f);
                            else if (amounts) start = sumAmount / amounts * (1.0f - g_steep[index] / 255.0f);
                            else
                            {
                                const ZoneFog* zone = kZoneWide[k] ? FindZoneFog(InputsFor(g_area[index] ? g_area[index] : g_playerArea).zone) : nullptr;
                                start = t[k] * g_progress[k];
                                if (zone && zone->started)
                                    start = zone->target[k] > 0.01f ? t[k] * std::fmin(zone->amount[k] / zone->target[k], 2.0f)
                                                                    : zone->amount[k] * (1.0f - g_steep[index] / 255.0f);
                            }
                            g_amount[k][index] = std::fmax(before, Clamp01(start));
                            g_pool[k][index] = std::fmax(g_pool[k][index], weight * g_amount[k][index]);
                            AddFrom(k, index, before, g_amount[k][index], g_ground[index]);
                        }
                    }
                    if (g_fresh[index]) { g_fresh[index] = 2; startedNow.push_back(index); } // 2: started in this pass
                }
            }
            for (int index : startedNow) g_fresh[index] = 0;
            if (!startedNow.empty()) ++g_grid.version; // the renderer's far-map stand-in ends there
            g_stats.rulesMs = grassperf::Now() - t0;
        }

        bool InGrid(int i, int j) { return i >= g_grid.firstI && i < g_grid.firstI + kGridSize && j >= g_grid.firstJ && j < g_grid.firstJ + kGridSize; }
        bool Open(int index) { return g_groundKnown[index] && g_block[index] != kStructure; }

        /// One field (a kind's amounts or its pools) carried by the wind by (sx, sy) cells: each cell
        /// takes from upwind (bilinear, so less than a cell per step works). Fog blown off the grid or
        /// into a WMO is gone. With `floor`, an anchored kind's floor comes along, weighted by the fog
        /// each upwind cell gives.
        void AdvectField(std::vector<float>& field, std::vector<float>* floor, float sx, float sy)
        {
            static std::vector<float> next(kGridSize * kGridSize), nextFloor(kGridSize * kGridSize);
            const std::vector<float>& a = field;
            // Upwind of the grid's edge counts as the edge cell itself: the fog beyond is like the fog
            // at the edge. (It counted as none, so a front of missing fog kept entering upwind and
            // crossed the grid with the wind, faster than slow kinds build up: area fog sat well below
            // its target.) WMO structures still give none: fog thins in their lee.
            auto cell = [&](int i, int j) {
                i = i < g_grid.firstI ? g_grid.firstI : (i >= g_grid.firstI + kGridSize ? g_grid.firstI + kGridSize - 1 : i);
                j = j < g_grid.firstJ ? g_grid.firstJ : (j >= g_grid.firstJ + kGridSize ? g_grid.firstJ + kGridSize - 1 : j);
                return Index(i, j);
            };
            auto at = [&](int i, int j) { const int n = cell(i, j); return Open(n) ? a[n] : 0.0f; };
            for (int j = g_grid.firstJ; j < g_grid.firstJ + kGridSize; ++j)
                for (int i = g_grid.firstI; i < g_grid.firstI + kGridSize; ++i)
                {
                    const int index = Index(i, j);
                    if (!Open(index))
                    {
                        next[index] = 0.0f;
                        if (floor) nextFloor[index] = (*floor)[index];
                        continue;
                    }
                    const float fx = i - sx, fy = j - sy;
                    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                    const float tx = fx - x0, ty = fy - y0;
                    const int ci[4] = { x0, x0 + 1, x0, x0 + 1 }, cj[4] = { y0, y0, y0 + 1, y0 + 1 };
                    const float w[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
                    float sum = 0.0f, floorSum = 0.0f;
                    for (int s = 0; s < 4; ++s)
                    {
                        const float m = w[s] * at(ci[s], cj[s]);
                        sum += m;
                        if (floor && m > 0.0f) floorSum += m * (*floor)[cell(ci[s], cj[s])];
                    }
                    next[index] = sum;
                    if (floor) nextFloor[index] = sum > 0.0f ? floorSum / sum : (*floor)[index];
                }
            std::copy(next.begin(), next.end(), field.begin());
            if (floor) std::copy(nextFloor.begin(), nextFloor.end(), floor->begin());
        }

        /// The wind carries every kind by its WindDrift: its amounts and its heavy pools.
        void Advect(float dt)
        {
            for (int k = 0; k < kKindCount; ++k)
            {
                const float drift = g_settings[k].windDrift;
                if (drift <= 0.0f || (g_grid.maxAmount[k] <= 0.0f && g_maxPool[k] <= 0.0f)) continue;
                const float speed = g_inputs.wind * drift * 4.0f * dt / kCellSize; // cells this step (4 yd/s at full wind)
                if (speed < 1.0e-3f) continue;
                const float sx = g_inputs.windDirX * speed, sy = g_inputs.windDirY * speed;
                AdvectField(g_amount[k], kAnchored[k] ? &g_floor[k] : nullptr, sx, sy);
                if (g_maxPool[k] > 0.0f) AdvectField(g_pool[k], nullptr, sx, sy);
            }
        }

        /// Heavy fog flows downhill: per cell, the pools' depth (sum over the kinds of pool x
        /// Thickness) on top of the ground is a surface; pools move toward neighbours whose surface is
        /// lower, each kind by its share of the depth, so they run down slopes and level out in hollows
        /// (where they may pile up past 1, to kMaxPool). Not into WMO structures. The amounts (the
        /// light share) don't flow: they stay where they formed.
        void Flow(float dt)
        {
            bool any = false;
            for (int k = 0; k < kKindCount; ++k) any = any || g_maxPool[k] > 0.0f;
            if (!any || g_field.poolSpeed <= 0.0f) return;
            static std::vector<float> depth(kGridSize * kGridSize), delta[kKindCount];
            for (int k = 0; k < kKindCount; ++k) delta[k].assign(kGridSize * kGridSize, 0.0f);
            for (int n = 0; n < kGridSize * kGridSize; ++n)
            {
                float d = 0.0f;
                for (int k = 0; k < kKindCount; ++k) d += g_pool[k][n] * CellSettings(k, n).thickness;
                depth[n] = d;
            }
            const float rate = std::fmin(1.0f, dt * g_field.poolSpeed); // share of a surface difference that evens out per second
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
                            // Pools are stored in amount units, depth = pool x the cell's own thickness:
                            // the depth leaving converts with the source's thickness, the depth arriving
                            // with the receiver's. (Both with the source's, depth wasn't kept across an
                            // area border with another thickness: x4 one way, / 4 the other for area fog
                            // 2 yd vs 8 yd; the pool jumped at the border and kept growing on one side,
                            // probe cross-sections 2026-10-12.)
                            const KindSettings& s = CellSettings(k, c);  // the source cell's place
                            const KindSettings& sn = CellSettings(k, n); // the receiving cell's place
                            if (g_pool[k][c] <= 0.0f || s.thickness <= 0.0f || sn.thickness <= 0.0f) continue;
                            const float share = g_pool[k][c] * s.thickness / depth[c];
                            delta[k][c] -= moved * share / s.thickness;
                            delta[k][n] += moved * share / sn.thickness;
                        }
                    }
                }
            for (int k = 0; k < kKindCount; ++k)
                for (int n = 0; n < kGridSize * kGridSize; ++n)
                    if (delta[k][n] != 0.0f) g_pool[k][n] = std::fmin(kMaxPool, std::fmax(0.0f, g_pool[k][n] + delta[k][n]));
        }

        /// One step: every amount toward its target at the kind's rates (build-up toward a higher one,
        /// burn-off toward a lower one), and how far the fog has come toward its targets overall.
        void Step(float dt)
        {
            // The player's cell: what the wind, the flow and the rules each did to it this step (the
            // panel's breakdown).
            const int pi = static_cast<int>(std::floor(g_px / kCellSize)), pj = static_cast<int>(std::floor(g_py / kCellSize));
            const bool player = InGrid(pi, pj);
            const int pIndex = player ? Index(pi, pj) : 0;
            float before[kKindCount], afterWind[kKindCount], poolBefore[kKindCount], poolWind[kKindCount], poolFlow[kKindCount];
            for (int k = 0; k < kKindCount; ++k)
            {
                before[k] = player ? g_amount[k][pIndex] : 0.0f;
                poolBefore[k] = player ? g_pool[k][pIndex] : 0.0f;
            }
            Advect(dt);
            for (int k = 0; k < kKindCount; ++k)
            {
                afterWind[k] = player ? g_amount[k][pIndex] : 0.0f;
                poolWind[k] = player ? g_pool[k][pIndex] : 0.0f;
            }
            Flow(dt);
            for (int k = 0; k < kKindCount; ++k) poolFlow[k] = player ? g_pool[k][pIndex] : 0.0f;
            bool changed = false;
            // Each place's build-up and burn-off rates for this step (per slot, so the cells look theirs up),
            // blended like the cell's other settings.
            static std::vector<float> upOf, downOf;
            auto Rate = [](const std::vector<float>& ofSlot, int n) {
                const float own = ofSlot[g_areaSlot[n]];
                const uint8_t share = g_blendShare[n];
                return share ? own + (ofSlot[g_blendSlot[n]] - own) * (share / 255.0f) : own;
            };
            upOf.resize(g_slots.size());
            downOf.resize(g_slots.size());
            for (int k = 0; k < kKindCount; ++k)
            {
                for (size_t sl = 0; sl < g_slots.size(); ++sl)
                {
                    const KindSettings& s = g_slots[sl].kind[k];
                    upOf[sl] = 1.0f - std::exp2(-dt / (s.buildUpSeconds > 0.1f ? s.buildUpSeconds : 0.1f));
                    downOf[sl] = 1.0f - std::exp2(-dt / (s.burnOffSeconds > 0.1f ? s.burnOffSeconds : 0.1f));
                }
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
                    a += (t - a) * Rate(t > a ? upOf : downOf, n);
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

                // The heavy pool forms where the fog forms, toward Weight x amount at the kind's own
                // rates; where it has drained away (a hill) it slowly comes back, where it has piled up
                // (a hollow) it slowly burns off toward that level.
                float* pool = g_pool[k].data();
                float mostPool = 0.0f;
                for (int n = 0; n < kGridSize * kGridSize; ++n)
                {
                    float p = pool[n];
                    const float t = HeavyShare(k, n) * amount[n];
                    if (p == 0.0f && t == 0.0f) continue;
                    p += (t - p) * Rate(t > p ? upOf : downOf, n);
                    if (p < kGone && t == 0.0f) p = 0.0f;
                    pool[n] = p;
                    if (p > mostPool) mostPool = p;
                }
                g_maxPool[k] = mostPool;
                changed = changed || mostPool > 0.0f;
            }
            if (changed) ++g_grid.version;

            if (player && dt > 0.0f)
            {
                float targets[kKindCount];
                CellTargets(pi, pj, pIndex, targets, g_explain); // the factors (the same as the rules pass's)
                for (int k = 0; k < kKindCount; ++k)
                {
                    Explanation& e = g_explain[k];
                    e.amount = g_amount[k][pIndex];
                    e.wind = (afterWind[k] - before[k]) / dt;
                    e.rules = (e.amount - afterWind[k]) / dt;
                    e.pool = g_pool[k][pIndex];
                    e.slopeDegrees = g_slope[pIndex];
                    e.heavyShare = HeavyShare(k, pIndex);
                    e.poolTarget = e.heavyShare * e.amount;
                    e.poolWind = (poolWind[k] - poolBefore[k]) / dt;
                    e.poolFlow = (poolFlow[k] - poolWind[k]) / dt;
                    e.poolRules = (e.pool - poolFlow[k]) / dt;
                }
            }
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
    const Explanation& Explain(int kind) { return g_explain[kind >= 0 && kind < kKindCount ? kind : 0]; }
    FieldSettings& Field() { return g_field; }

    unsigned ZoneStates(ZoneState* out, unsigned max)
    {
        unsigned n = 0;
        const double now = regional::Now();
        for (const ZoneFog& z : g_zoneFog)
        {
            if (z.map != g_map || n >= max) continue;
            ZoneState& s = out[n++];
            s.zone = z.zone;
            for (int k = 0; k < kKindCount; ++k) { s.amount[k] = z.amount[k]; s.target[k] = z.target[k]; }
            s.inRange = now - z.lastInRange < 1.0;
        }
        return n;
    }

    float ZoneAmount(uint32_t zone, int kind)
    {
        const ZoneFog* z = FindZoneFog(zone);
        return z && z->started && kind >= 0 && kind < kKindCount ? z->amount[kind] : 0.0f;
    }

    bool ZoneWide(int kind) { return kind >= 0 && kind < kKindCount && kZoneWide[kind]; }

    KindSettings SettingsAt(int kind, int index)
    {
        if (kind < 0 || kind >= kKindCount) kind = 0;
        return g_areaSlot.empty() ? g_settings[kind] : CellSettings(kind, index);
    }

    const KindSettings& SettingsOfArea(int kind, uint32_t area)
    {
        if (kind < 0 || kind >= kKindCount) kind = 0;
        return g_slots[SlotOf(area)].kind[kind];
    }

    float HeavyShare(int kind, int index)
    {
        // Just the place's Weight: steep ground now keeps fog from forming (CellTargets). Turning its
        // light share heavy there formed thin pools on cliffs faster than they drained, drawn as flat
        // slices per cell: contour stripes and flickering rings (owner-confirmed 2026-10-11).
        return SettingsAt(kind, index).weight;
    }

    float Progress(int kind) { return kind >= 0 && kind < kKindCount ? g_progress[kind] : 0.0f; }

    float BlendOf(int index, uint32_t& otherArea)
    {
        otherArea = 0;
        if (g_blendShare.empty() || index < 0 || index >= kGridSize * kGridSize || !g_blendShare[index]) return 0.0f;
        const uint16_t slot = g_blendSlot[index];
        if (slot >= g_slots.size()) return 0.0f;
        otherArea = g_slots[slot].area;
        return g_blendShare[index] / 255.0f;
    }

    bool CellAt(float x, float y, CellState& out)
    {
        out = CellState{};
        if (!g_placed || g_groundKnown.empty()) return false;
        const int i = static_cast<int>(std::floor(x / kCellSize)), j = static_cast<int>(std::floor(y / kCellSize));
        if (!InGrid(i, j)) return false;
        const int index = Index(i, j);
        out.i = i; out.j = j; out.index = index;
        out.known = g_groundKnown[index] != 0;
        out.fresh = g_fresh[index] != 0;
        out.block = g_block[index];
        out.ground = g_ground[index];
        out.steep = g_steep[index] / 255.0f;
        out.area = g_area[index];
        out.slot = g_areaSlot[index];
        out.blendSlot = g_blendSlot[index];
        out.blendArea = g_blendSlot[index] < g_slots.size() ? g_slots[g_blendSlot[index]].area : 0;
        out.blendShare = g_blendShare[index] / 255.0f;
        for (int k = 0; k < kKindCount; ++k)
        {
            out.target[k] = g_target[k][index] / 255.0f;
            out.amount[k] = g_amount[k][index];
            out.pool[k] = g_pool[k][index];
            out.heavyShare[k] = HeavyShare(k, index);
        }
        return true;
    }

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
        if (climate::Generation() != g_seenClimate || fogsettings::Generation() != g_seenFogSettings)
        {
            // A reload (not the first read): the next full round of the rules re-seeds the grid.
            if (g_seenClimate || g_seenFogSettings)
            {
                g_reseedRows = kGridSize;
                for (int k = 0; k < kKindCount; ++k) g_progress[k] = 1.0f;
                for (ZoneFog& z : g_zoneFog) z.started = false; // the zones start over at their new targets too
            }
            g_seenClimate = climate::Generation();
            g_seenFogSettings = fogsettings::Generation();
        }
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
        RefreshSlots();
        GatherInputs(snap);
        StepZones(step);
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
                // Its heavy share as a pool right away (else it would form at the build-up rate).
                float& p = g_pool[kind][Index(i, j)];
                p = std::fmin(kMaxPool, p + HeavyShare(kind, Index(i, j)) * (a - before));
                if (p > g_maxPool[kind]) g_maxPool[kind] = p;
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
                g_pool[k][n] = HeavyShare(k, n) * g_amount[k][n];
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
