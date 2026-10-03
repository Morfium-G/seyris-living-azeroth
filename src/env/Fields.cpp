#include "Fields.hpp"

#include "Climate.hpp"
#include "Shelter.hpp"
#include "TerrainHeight.hpp"
#include "Wind.hpp"
#include "../features/GrassPerf.hpp"
#include "../features/GroundMaterialTable.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace wxl_livingazeroth::fields
{
    namespace
    {
        enum FieldId { kTemperature = 0, kMoisture = 1, kFieldCount };
        constexpr const char* kNames[kFieldCount] = { "temperature", "moisture" };

        // --- the moisture grid --------------------------------------------------------------------
        constexpr int   kSize = 128;          // cells per side
        constexpr float kCell = 2.0f;         // yd: +-128 yd around the player
        constexpr int   kRecentre = 8;        // cells the player moves before the grid follows
        constexpr double kFillBudgetMs = 0.75; // static sampling per frame (a full grid: ~40 frames)
        constexpr float kTick = 0.25f;        // seconds between moisture steps (it changes over minutes)

        // Model numbers (rules are code; world-fields.md).
        constexpr float kShoreNear = 2.0f, kShoreFar = 6.0f;      // yd: full wetting .. none
        constexpr float kShoreLow = 0.5f, kShoreHigh = 2.0f;      // yd above the water: full .. none
        constexpr float kRainSeconds = 90.0f;                     // full rain soaks absorbent open ground in ~this
        constexpr float kDrySeconds = 600.0f;                     // back to equilibrium at 0..15 degC, calm, average humidity
        constexpr float kHeatDryStart = 20.0f, kHeatDryFull = 40.0f; // degC: hot dry air dries below rest...
        constexpr float kHeatDryMax = 0.6f;                       // ...by up to this share of the rest
        // Heat from a hot liquid (magma): full within 1 yd, gone by 12 yd and 4 yd above its surface;
        // there the ground is warmed by this share of the difference (magma 1000 degC: ~+50 degC).
        constexpr float kHeatNear = 1.0f, kHeatFar = 12.0f, kHeatLow = 0.5f, kHeatHigh = 4.0f;
        constexpr float kHeatShare = 0.05f;
        // Built-in liquid values by category when no GroundMaterialSelector row names the liquid.
        constexpr float kMagmaTemperature = 1000.0f;

        struct Cell
        {
            int      i = std::numeric_limits<int>::min(), j = 0; // world cell this slot holds
            bool     filled = false;  // static parts sampled (terrain loaded)
            float    z = 0.0f;
            float    rest = 0.3f, absorbency = 0.5f;
            uint32_t area = 0;
            bool     submerged = false;
            float    waterZ = 0.0f;   // the liquid surface when submerged
            uint32_t liquid = 0;      // its LiquidType ID
            float    liquidMoisture = 1.0f, liquidTemp = 0.0f; // what it does to the ground around
            bool     liquidHot = false;                        // it has a temperature of its own
            uint32_t nearLiquid = 0;  // the nearest liquid (from the shore pass) and its values
            float    nearMoisture = 0.0f, nearTemp = 0.0f, heat = 0.0f;
            bool     nearHot = false;
            float    temperature = 0.0f; // the ground's, last step (air + heat from a hot liquid)
            float    open = 1.0f;
            float    shore = 0.0f, waterDist = -1.0f, aboveWater = 0.0f;
            float    value = 0.3f;    // current moisture
            bool     started = false; // value initialised (to its equilibrium)
        };
        std::vector<Cell>    g_cells(kSize * kSize);
        std::vector<uint8_t> g_excess(kSize * kSize, 0);
        WetGrid  g_wet;
        int      g_firstI = 0, g_firstJ = 0;
        bool     g_haveGrid = false;
        int      g_map = -1;
        int      g_fillCursor = 0, g_openCursor = 0;
        bool     g_shoreDirty = false;
        float    g_tickTime = 0.0f, g_shoreTime = 0.0f;
        uint32_t g_climateGeneration = 0, g_materialGeneration = 0;
        Stats    g_stats;

        int Mod(int v) { const int m = v % kSize; return m < 0 ? m + kSize : m; }
        int SlotOf(int i, int j) { return Mod(j) * kSize + Mod(i); }
        int FloorCell(float v) { return static_cast<int>(std::floor(v / kCell)); }
        float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        float Smooth(float edge0, float edge1, float v) // 1 at edge0 .. 0 at edge1
        {
            const float t = Clamp01((v - edge0) / (edge1 - edge0));
            return 1.0f - t * t * (3.0f - 2.0f * t);
        }

        // The cell at grid position (a, b), if its slot holds that world cell and it's filled.
        Cell* Live(int a, int b)
        {
            const int i = g_firstI + a, j = g_firstJ + b;
            Cell& c = g_cells[SlotOf(i, j)];
            return c.filled && c.i == i && c.j == j ? &c : nullptr;
        }

        // The ground's materials at a point, weighted by painted strength (layers without a material
        // count with the defaults).
        void MaterialAt(float x, float y, float& rest, float& absorbency, uint32_t& area)
        {
            terrain::LayerWeights lw;
            const materials::Values defaults;
            rest = defaults.restMoisture; absorbency = defaults.absorbency; area = 0;
            if (!terrain::LayerWeightsAt(x, y, lw) || lw.layers <= 0) return;
            float r = 0.0f, a = 0.0f, w = 0.0f;
            for (int l = 0; l < lw.layers; ++l)
            {
                const float wk = lw.weight[l];
                if (wk <= 0.0f) continue;
                const terrain::Surface& sf = lw.surface[l];
                const materials::Values v = materials::Get(materials::Select(sf.area, g_map, sf.texture, sf.groundEffect, sf.terrainType));
                r += wk * v.restMoisture; a += wk * v.absorbency; w += wk;
                area = sf.area;
            }
            if (w > 0.0f) { rest = r / w; absorbency = a / w; }
        }

        bool FillCell(Cell& c, int i, int j)
        {
            c = Cell{};
            c.i = i; c.j = j;
            const float x = (i + 0.5f) * kCell, y = (j + 0.5f) * kCell;
            float z;
            if (!terrain::HeightAt(x, y, z)) return false; // not loaded (or a hole): asked again later
            c.z = z;
            MaterialAt(x, y, c.rest, c.absorbency, c.area);
            float waterZ;
            uint32_t liquid = 0;
            if (terrain::LiquidAt(x, y, waterZ, liquid) && waterZ > z)
            {
                c.submerged = true; c.waterZ = waterZ; c.liquid = liquid;
                // What the liquid is: its GroundMaterialSelector row (LiquidType), else by category.
                const bool magma = terrain::LiquidCategoryOf(liquid) == terrain::kLiquidMagma;
                c.liquidMoisture = magma ? 0.0f : 1.0f;
                c.liquidHot = magma; c.liquidTemp = magma ? kMagmaTemperature : 0.0f;
                if (const uint32_t material = materials::SelectLiquid(liquid, c.area, g_map))
                {
                    const materials::Values v = materials::Get(material);
                    if (v.restMoistureSet) c.liquidMoisture = v.restMoisture;
                    if (v.hasTemperature) { c.liquidHot = true; c.liquidTemp = v.temperature; }
                }
            }
            const float pos[3] = { x, y, z + 0.5f };
            c.open = shelter::Openness(pos);
            c.value = c.rest;
            c.filled = true;
            return true;
        }

        // Every cell's distance to the nearest submerged cell and that water's surface (two-pass
        // chamfer transform over the grid), then how much the water nearby counts there.
        void UpdateShore()
        {
            const double t0 = grassperf::Now();
            constexpr float kInf = 1.0e9f;
            static std::vector<float> dist(kSize * kSize), surf(kSize * kSize);
            static std::vector<int> src(kSize * kSize);
            auto at = [](int a, int b) { return b * kSize + a; }; // logical (grid-relative) index
            unsigned water = 0;
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    const Cell* c = Live(a, b);
                    const bool w = c && c->submerged;
                    dist[at(a, b)] = w ? 0.0f : kInf;
                    surf[at(a, b)] = w ? c->waterZ : 0.0f;
                    src[at(a, b)] = w ? at(a, b) : -1;
                    water += w;
                }
            auto relax = [&](int a, int b, int na, int nb, float step)
            {
                if (na < 0 || nb < 0 || na >= kSize || nb >= kSize) return;
                const float d = dist[at(na, nb)] + step;
                if (d < dist[at(a, b)]) { dist[at(a, b)] = d; surf[at(a, b)] = surf[at(na, nb)]; src[at(a, b)] = src[at(na, nb)]; }
            };
            constexpr float kDiag = 1.4142f;
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                { relax(a, b, a - 1, b, 1.0f); relax(a, b, a, b - 1, 1.0f); relax(a, b, a - 1, b - 1, kDiag); relax(a, b, a + 1, b - 1, kDiag); }
            for (int b = kSize - 1; b >= 0; --b)
                for (int a = kSize - 1; a >= 0; --a)
                { relax(a, b, a + 1, b, 1.0f); relax(a, b, a, b + 1, 1.0f); relax(a, b, a + 1, b + 1, kDiag); relax(a, b, a - 1, b + 1, kDiag); }

            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    Cell* live = Live(a, b);
                    if (!live) continue;
                    Cell& c = *live;
                    const float d = dist[at(a, b)];
                    if (d >= kInf || src[at(a, b)] < 0) { c.waterDist = -1.0f; c.shore = 0.0f; c.heat = 0.0f; c.nearLiquid = 0; c.nearHot = false; continue; }
                    const Cell* w = Live(src[at(a, b)] % kSize, src[at(a, b)] / kSize);
                    c.waterDist = d * kCell;
                    c.aboveWater = c.z - surf[at(a, b)];
                    c.shore = c.submerged ? 1.0f : Smooth(kShoreNear, kShoreFar, c.waterDist) * Smooth(kShoreLow, kShoreHigh, c.aboveWater);
                    c.nearLiquid = w ? w->liquid : 0;
                    c.nearMoisture = w ? w->liquidMoisture : 1.0f;
                    c.nearHot = w && w->liquidHot;
                    c.nearTemp = w ? w->liquidTemp : 0.0f;
                    c.heat = c.nearHot ? (c.submerged ? 1.0f : Smooth(kHeatNear, kHeatFar, c.waterDist) * Smooth(kHeatLow, kHeatHigh, c.aboveWater)) : 0.0f;
                }
            g_stats.water = water;
            g_stats.transformMs = grassperf::Now() - t0;
        }

        float Equilibrium(const Cell& c, float temperature, float humidity)
        {
            if (c.submerged) return c.liquidMoisture;
            const float wetter = c.shore * c.nearMoisture * c.absorbency * (1.0f - c.rest);
            const float heat = c.rest * kHeatDryMax * (1.0f - Smooth(kHeatDryStart, kHeatDryFull, temperature)) * (1.0f - humidity) * c.absorbency;
            return Clamp01(c.rest + wetter - heat);
        }

        // One moisture step for every cell.
        void Tick(float dt)
        {
            const double t0 = grassperf::Now();
            const climate::Weather& weather = climate::CurrentWeather();
            const bool raining = weather.type == 1 && weather.intensity > 0.0f;
            const float wind = wind::SteadyGround();
            // Temperature and humidity per area, once per tick.
            uint32_t lastArea = 0xFFFFFFFFu;
            float temperature = 0.0f, humidity = 0.5f;
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    const int s = SlotOf(g_firstI + a, g_firstJ + b);
                    Cell* live = Live(a, b);
                    if (!live) { g_excess[s] = 0; continue; }
                    Cell& c = *live;
                    if (c.area != lastArea)
                    {
                        const climate::Row& row = climate::For(c.area, g_map);
                        temperature = climate::Temperature(row).total;
                        humidity = row.humidity;
                        lastArea = c.area;
                    }
                    // The ground's temperature: the air, warmed (or cooled) near a liquid with a temperature.
                    c.temperature = temperature + (c.nearHot ? c.heat * kHeatShare * (c.nearTemp - temperature) : 0.0f);
                    const float eq = Equilibrium(c, c.temperature, humidity);
                    if (!c.started) { c.value = eq; c.started = true; }
                    if (c.submerged) c.value = c.liquidMoisture;
                    else if (raining && c.open > 0.0f)
                        c.value += dt / kRainSeconds * weather.intensity * c.absorbency * c.open * (1.0f - c.value);
                    else
                    {
                        const float warmth = c.temperature <= 0.0f ? 0.2f : 1.0f + c.temperature / 15.0f;
                        const float rate = (1.0f + 2.0f * wind) * warmth * (1.0f - 0.7f * humidity) / kDrySeconds;
                        const float k = rate * dt > 1.0f ? 1.0f : rate * dt;
                        c.value += (eq - c.value) * k;
                    }
                    c.value = Clamp01(c.value);
                    const float excess = c.value - c.rest;
                    g_excess[s] = static_cast<uint8_t>(Clamp01(excess) * 255.0f + 0.5f);
                }
            ++g_wet.version;
            g_stats.tickMs = grassperf::Now() - t0;
        }

        const Cell* CellAt(const float pos[3])
        {
            if (!g_haveGrid) return nullptr;
            const int i = FloorCell(pos[0]), j = FloorCell(pos[1]);
            if (i < g_firstI || i >= g_firstI + kSize || j < g_firstJ || j >= g_firstJ + kSize) return nullptr;
            const Cell& c = g_cells[SlotOf(i, j)];
            return c.filled && c.i == i && c.j == j ? &c : nullptr;
        }
    }

    int Find(const char* name)
    {
        if (!name) return -1;
        for (int f = 0; f < kFieldCount; ++f) if (std::strcmp(name, kNames[f]) == 0) return f;
        return -1;
    }
    int         Count()          { return kFieldCount; }
    const char* Name(int field)  { return field >= 0 && field < kFieldCount ? kNames[field] : ""; }

    bool Sample(int field, const float pos[3], float& out)
    {
        if (field == kTemperature)
        {
            if (const Cell* c = CellAt(pos); c && c->started) { out = c->temperature; return true; }
            terrain::Surface sf;
            const uint32_t area = terrain::SurfaceAt(pos[0], pos[1], sf) ? sf.area : 0;
            out = climate::TemperatureAt(area, g_map);
            return true;
        }
        if (field == kMoisture)
        {
            const Cell* c = CellAt(pos);
            if (!c || !c->started) return false;
            out = c->value;
            return true;
        }
        return false;
    }

    MoistureDetail Moisture(const float pos[3])
    {
        MoistureDetail d;
        const Cell* c = CellAt(pos);
        if (!c) return d;
        const climate::Row& row = climate::For(c->area, g_map);
        d.known = c->started;
        d.rest = c->rest; d.absorbency = c->absorbency; d.open = c->open; d.submerged = c->submerged;
        d.waterDistance = c->waterDist; d.heightAboveWater = c->aboveWater; d.shore = c->shore;
        d.airTemperature = climate::Temperature(row).total; d.humidity = row.humidity;
        d.temperature = c->started ? c->temperature : d.airTemperature;
        d.equilibrium = Equilibrium(*c, d.temperature, d.humidity);
        d.liquid = c->submerged ? c->liquid : c->nearLiquid;
        d.liquidMoisture = c->submerged ? c->liquidMoisture : c->nearMoisture;
        d.liquidHot = c->submerged ? c->liquidHot : c->nearHot;
        d.liquidTemperature = c->submerged ? c->liquidTemp : c->nearTemp;
        d.heat = c->heat;
        d.value = c->value;
        return d;
    }

    const WetGrid& Wet() { return g_wet; }
    Stats GetStats() { return g_stats; }

    void Reset()
    {
        for (Cell& c : g_cells) c = Cell{};
        std::fill(g_excess.begin(), g_excess.end(), 0);
        g_haveGrid = false;
        g_fillCursor = 0;
        ++g_wet.version;
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        if (!snap.inWorld) { if (g_haveGrid) Reset(); g_map = -1; return; }
        if (snap.mapId != g_map || climate::Generation() != g_climateGeneration || materials::Generation() != g_materialGeneration)
        {
            Reset();
            g_map = snap.mapId;
            g_climateGeneration = climate::Generation();
            g_materialGeneration = materials::Generation();
        }

        // Follow the player in steps; slots of cells that leave the grid get refilled when their new
        // cell is reached (each slot remembers which world cell it holds).
        const int pi = FloorCell(snap.playerPos[0]) - kSize / 2, pj = FloorCell(snap.playerPos[1]) - kSize / 2;
        if (!g_haveGrid || std::abs(pi - g_firstI) >= kRecentre || std::abs(pj - g_firstJ) >= kRecentre)
        {
            g_firstI = pi; g_firstJ = pj;
            g_haveGrid = true;
            g_shoreDirty = true;
        }
        g_wet.size = kSize; g_wet.cellSize = kCell; g_wet.firstI = g_firstI; g_wet.firstJ = g_firstJ; g_wet.excess = g_excess.data();

        // Static sampling within the budget: cells whose slot holds another world cell, or that
        // weren't loaded yet. One pass over the grid per sweep; unloaded ones are retried next sweep.
        const double t0 = grassperf::Now();
        for (int n = 0; n < kSize * kSize; ++n)
        {
            const int k = g_fillCursor;
            g_fillCursor = (g_fillCursor + 1) % (kSize * kSize);
            const int i = g_firstI + k % kSize, j = g_firstJ + k / kSize;
            Cell& c = g_cells[SlotOf(i, j)];
            if (c.i == i && c.j == j && c.filled) continue;
            if (FillCell(c, i, j)) g_shoreDirty = true;
            if (grassperf::Now() - t0 > kFillBudgetMs) break;
        }
        g_stats.fillMs = grassperf::Now() - t0;

        // Roof checks change as WMOs stream in: a few cells per frame.
        for (int n = 0; n < 64; ++n)
        {
            const int k = g_openCursor;
            g_openCursor = (g_openCursor + 1) % (kSize * kSize);
            Cell* live = Live(k % kSize, k / kSize);
            if (!live) continue;
            Cell& c = *live;
            const float pos[3] = { (c.i + 0.5f) * kCell, (c.j + 0.5f) * kCell, c.z + 0.5f };
            c.open = shelter::Openness(pos);
        }

        g_shoreTime += dt;
        if (g_shoreDirty && g_shoreTime >= 0.5f) { UpdateShore(); g_shoreDirty = false; g_shoreTime = 0.0f; }

        g_tickTime += dt;
        if (g_tickTime >= kTick) { Tick(g_tickTime); g_tickTime = 0.0f; }

        unsigned count = 0;
        for (int b = 0; b < kSize; ++b)
            for (int a = 0; a < kSize; ++a) count += Live(a, b) != nullptr;
        g_stats.filled = count;
        g_stats.cells = kSize * kSize;
    }
}
