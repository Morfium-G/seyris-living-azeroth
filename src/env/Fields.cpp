#include "Fields.hpp"

#include "Climate.hpp"
#include "Regional.hpp"
#include "Shelter.hpp"
#include "Snow.hpp"
#include "TerrainHeight.hpp"
#include "Wind.hpp"
#include "../features/GrassPerf.hpp"
#include "../features/GroundMaterialTable.hpp"
#include "../features/SurfaceCoverTable.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace wxl_livingazeroth::fields
{
    namespace
    {
        enum FieldId { kTemperature = 0, kMoisture = 1, kSnow = 2, kFieldCount };
        constexpr const char* kNames[kFieldCount] = { "temperature", "moisture", "snow" };

        // Fallen snow eases toward its value (its zone's changes slowly anyway): quickly down (it melts
        // on arrival on hot ground), a little slower up (a pit left by heat fills back in).
        constexpr float kSnowDownSeconds = 2.0f, kSnowUpSeconds = 5.0f;
        // Published to the cover when a spot's snow moved by this much (yd) since the last publish.
        constexpr float kSnowPublishYards = 0.01f;
        constexpr float kSnowEdgeFade = 32.0f; // yd inside the grid's edge where it fades into the zones' values

        // --- the moisture grid --------------------------------------------------------------------
        constexpr int   kSize = 128;          // cells per side
        constexpr float kCell = 2.0f;         // yd: +-128 yd around the player
        constexpr int   kRecentre = 8;        // cells the player moves before the grid follows
        constexpr double kFillBudgetMs = 0.75; // static sampling per frame (a full grid: ~40 frames)
        constexpr float kTick = 0.25f;        // seconds between moisture steps (it changes over minutes)
        constexpr int   kRing = 8;            // cells of the outer ring averaged for beyond the grid
        // Zone borders blend over this radius (cells, 32 yd): rain falls by the share of the player's
        // zone around a spot, and new ground starts from the mix of the zones around it.
        constexpr int   kZoneBlend = 16;
        constexpr float kFollowSeconds = 2.0f; // a spot eases toward its value over about this long

        // Model numbers (rules are code; world-fields.md).
        constexpr float kShoreNear = 2.0f, kShoreFar = 6.0f;      // yd: full wetting .. none
        constexpr float kShoreLow = 0.5f, kShoreHigh = 2.0f;      // yd above the water: full .. none
        constexpr float kHeatDryStart = 20.0f, kHeatDryFull = 40.0f; // degC: hot dry air dries below rest...
        constexpr float kHeatDryMax = 0.6f;                       // ...by up to this share of the rest
        // Heat from a hot liquid (magma): full within 1 yd, gone by 12 yd and 4 yd above its surface;
        // there the ground is warmed by this share of the difference (magma 1000 degC: ~+50 degC).
        constexpr float kHeatNear = 1.0f, kHeatFar = 12.0f, kHeatLow = 0.5f, kHeatHigh = 4.0f;
        constexpr float kHeatShare = 0.05f;
        // Ground counts as a heat source where materials with a Temperature are painted at least this strongly.
        constexpr float kHotGroundShare = 0.5f;
        // Built-in liquid values by category when no GroundMaterialSelector row names the liquid.
        constexpr float kMagmaTemperature = 1000.0f;

        struct Cell
        {
            int      i = std::numeric_limits<int>::min(), j = 0; // world cell this slot holds
            bool     filled = false;  // static parts sampled (terrain loaded)
            float    z = 0.0f;
            float    rest = 0.3f, absorbency = 0.5f;
            uint32_t area = 0;
            uint32_t zone = 0;        // the top of the area's chain (env/Regional)
            bool     submerged = false;
            float    waterZ = 0.0f;   // the liquid surface when submerged
            uint32_t liquid = 0;      // its LiquidType ID
            float    liquidMoisture = 1.0f, liquidTemp = 0.0f; // what it does to the ground around
            bool     liquidHot = false;                        // it has a temperature of its own
            uint32_t nearLiquid = 0;  // the nearest liquid (from the shore pass) and its values
            float    nearMoisture = 0.0f, nearTemp = 0.0f, heat = 0.0f;
            bool     nearHot = false;
            float    temperature = 0.0f; // the ground's, last step (air + heat from a hot liquid or hot ground)
            float    hotShare = 0.0f, hotTemp = 0.0f; // painted strength of materials with a Temperature, and theirs
            float    groundHeat = 0.0f, groundHeatTemp = 0.0f, groundHeatDist = -1.0f; // the nearest hot ground's reach here
            float    open = 1.0f;
            float    shore = 0.0f, waterDist = -1.0f, aboveWater = 0.0f;
            float    value = 0.3f;    // current moisture
            // Diagnostics (panel): the last step's inputs and change.
            float    lastShare = 0.0f, lastSoak = 0.0f, lastRain = 0.0f, lastChange = 0.0f, lastDt = 0.0f;
            bool     started = false; // value initialised (to its equilibrium)

            // Snow (env/Snow.hpp). The ground's slope (unit normal x, y) for the sun; the painted snow
            // cover here (yd, mixed by painted strength) and its rows' melt columns.
            float    nx = 0.0f, ny = 0.0f;
            float    snowPainted = 0.0f;
            float    keptShare = snow::kDefaultKeptShare, goneTemp = snow::kDefaultGoneTemperature;
            float    fallen = 0.0f;   // yd of fallen snow
            float    keep = 1.0f;     // painted snow's share left
            float    meltWet = 0.0f;  // soak from its own melt water (0..1)
            bool     snowStarted = false;
            float    lastSnowTarget = 0.0f, lastSnowMix = 0.0f, lastHold = 1.0f, lastTeff = 0.0f, lastSun = 0.0f,
                     lastZoneT = 0.0f, lastCap = 1.0f, lastReach = 0.0f;
        };
        std::vector<Cell>    g_cells(kSize * kSize);
        std::vector<uint8_t> g_excess(kSize * kSize, 0);
        std::vector<uint8_t> g_farBytes;   // per chunk of the zone map
        std::vector<float>   g_farValues;

        // Snow as last computed, and as last published to the cover (same toroidal layout as the
        // grid; fallen < 0 = spot not known). Far: per chunk of the zone map.
        struct SnowMap
        {
            std::vector<float> fallen = std::vector<float>(kSize * kSize, -1.0f), keep = std::vector<float>(kSize * kSize, 1.0f);
            std::vector<float> farFallen, farKeep;
            std::vector<uint8_t> farKnown;   // the chunk's zone is known (else it shows the player's zone)
            int   firstI = 0, firstJ = 0, farFirstI = 0, farFirstJ = 0, farSize = 0;
            float farCell = 0.0f;
            float outsideFallen = 0.0f, outsideKeep = 1.0f; // beyond the zone map: the player's zone
            bool  valid = false, active = false;
        };
        SnowMap  g_snowNow, g_snowPub;
        uint32_t g_snowVersion = 1;
        unsigned g_snowPublishes = 0;

        // The excess of typical ground (rest 0.3, absorbency 0.5, open sky) at a zone soak s, by the
        // same rule the near spots follow.
        float TypicalExcess(float s)
        {
            constexpr float kRest = 0.3f, kAbsorbency = 0.5f;
            return (1.0f - kRest) * (1.0f - std::pow(1.0f - (s < 0.999f ? s : 0.999f), kAbsorbency));
        }
        WetGrid  g_wet;
        int      g_firstI = 0, g_firstJ = 0;
        bool     g_haveGrid = false;
        int      g_map = -1;
        int      g_fillCursor = 0, g_openCursor = 0;
        bool     g_shoreDirty = false;
        float    g_tickTime = 0.0f, g_shoreTime = 0.0f;
        uint32_t g_climateGeneration = 0, g_materialGeneration = 0;
        Stats    g_stats;
        // How much rain has soaked each zone lives in env/Regional. Ground entering the grid starts at
        // its own zone's value (x its absorbency and open sky), so walking on in the rain doesn't
        // leave a dry ring where the grid only just arrived, and a neighbouring zone isn't wet just
        // because it rains here.

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

        // The ground's materials at a cell's centre, weighted by painted strength (layers without a
        // material count with the defaults), and the painted snow cover there: rows with Depth > 0
        // whose CoverMaterial is snow, their depth mixed over every layer, their melt columns over the
        // snowy ones.
        void MaterialAt(float x, float y, Cell& c)
        {
            terrain::LayerWeights lw;
            const materials::Values defaults;
            c.rest = defaults.restMoisture; c.absorbency = defaults.absorbency; c.area = 0; c.hotShare = 0.0f; c.hotTemp = 0.0f;
            if (!terrain::LayerWeightsAt(x, y, lw) || lw.layers <= 0) return;
            float r = 0.0f, a = 0.0f, w = 0.0f, hot = 0.0f, hotSum = 0.0f;
            float snowW = 0.0f, snowDepth = 0.0f, kept = 0.0f, gone = 0.0f;
            for (int l = 0; l < lw.layers; ++l)
            {
                const float wk = lw.weight[l];
                if (wk <= 0.0f) continue;
                const terrain::Surface& sf = lw.surface[l];
                const materials::Values v = materials::Get(materials::Select(sf.area, g_map, sf.texture, sf.groundEffect, sf.terrainType));
                r += wk * v.restMoisture; a += wk * v.absorbency; w += wk;
                if (v.hasTemperature) { hot += wk; hotSum += wk * v.temperature; }
                c.area = sf.area;
                const covertable::Values cv = covertable::Resolve(sf.area, g_map, sf.texture, sf.groundEffect, sf.terrainType);
                if (cv.depth > 0.0f && covertable::IsSnow(cv.coverMaterial, sf.area, g_map))
                {
                    snowW += wk; snowDepth += wk * cv.depth;
                    kept += wk * cv.meltKeptShare; gone += wk * cv.meltGoneTemperature;
                }
            }
            if (w > 0.0f) { c.rest = r / w; c.absorbency = a / w; c.hotShare = Clamp01(hot / w); c.snowPainted = snowDepth / w; }
            if (hot > 0.0f) c.hotTemp = hotSum / hot;
            if (snowW > 0.0f) { c.keptShare = kept / snowW; c.goneTemp = gone / snowW; }
        }

        bool FillCell(Cell& c, int i, int j)
        {
            c = Cell{};
            c.i = i; c.j = j;
            const float x = (i + 0.5f) * kCell, y = (j + 0.5f) * kCell;
            float z;
            if (!terrain::HeightAt(x, y, z)) return false; // not loaded (or a hole): asked again later
            c.z = z;
            MaterialAt(x, y, c);
            c.zone = regional::ZoneOf(c.area);
            // The slope over +-1 yd, for the sun (what faces it melts first).
            float ex0, ex1, ey0, ey1;
            if (terrain::HeightAt(x - 1.0f, y, ex0) && terrain::HeightAt(x + 1.0f, y, ex1) &&
                terrain::HeightAt(x, y - 1.0f, ey0) && terrain::HeightAt(x, y + 1.0f, ey1))
            {
                const float gx = (ex1 - ex0) * 0.5f, gy = (ey1 - ey0) * 0.5f;
                const float inv = 1.0f / std::sqrt(1.0f + gx * gx + gy * gy);
                c.nx = -gx * inv; c.ny = -gy * inv;
            }
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
            // The same pass for hot ground: painted materials with a Temperature (lava streams, ...)
            // heat the ground around them like hot liquids do.
            unsigned hot = 0;
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    const Cell* c = Live(a, b);
                    const bool h = c && !c->submerged && c->hotShare >= kHotGroundShare;
                    dist[at(a, b)] = h ? 0.0f : kInf;
                    src[at(a, b)] = h ? at(a, b) : -1;
                    hot += h;
                }
            auto relaxHot = [&](int a, int b, int na, int nb, float step)
            {
                if (na < 0 || nb < 0 || na >= kSize || nb >= kSize) return;
                const float d = dist[at(na, nb)] + step;
                if (d < dist[at(a, b)]) { dist[at(a, b)] = d; src[at(a, b)] = src[at(na, nb)]; }
            };
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                { relaxHot(a, b, a - 1, b, 1.0f); relaxHot(a, b, a, b - 1, 1.0f); relaxHot(a, b, a - 1, b - 1, kDiag); relaxHot(a, b, a + 1, b - 1, kDiag); }
            for (int b = kSize - 1; b >= 0; --b)
                for (int a = kSize - 1; a >= 0; --a)
                { relaxHot(a, b, a + 1, b, 1.0f); relaxHot(a, b, a, b + 1, 1.0f); relaxHot(a, b, a + 1, b + 1, kDiag); relaxHot(a, b, a - 1, b + 1, kDiag); }
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    Cell* live = Live(a, b);
                    if (!live) continue;
                    Cell& c = *live;
                    const Cell* h = src[at(a, b)] >= 0 ? Live(src[at(a, b)] % kSize, src[at(a, b)] / kSize) : nullptr;
                    if (!h || dist[at(a, b)] >= kInf) { c.groundHeat = 0.0f; c.groundHeatDist = -1.0f; continue; }
                    c.groundHeatDist = dist[at(a, b)] * kCell;
                    c.groundHeatTemp = h->hotTemp;
                    c.groundHeat = dist[at(a, b)] == 0.0f ? c.hotShare
                                 : Smooth(kHeatNear, kHeatFar, c.groundHeatDist) * Smooth(kHeatLow, kHeatHigh, c.z - h->z);
                }
            g_stats.water = water;
            g_stats.hotGround = hot;
            g_stats.transformMs = grassperf::Now() - t0;
        }

        float Equilibrium(const Cell& c, float temperature, float humidity)
        {
            if (c.submerged) return c.liquidMoisture;
            const float wetter = c.shore * c.nearMoisture * c.absorbency * (1.0f - c.rest);
            const float heat = c.rest * kHeatDryMax * (1.0f - Smooth(kHeatDryStart, kHeatDryFull, temperature)) * (1.0f - humidity) * c.absorbency;
            return Clamp01(c.rest + wetter - heat);
        }

        // The snow as computed this tick (near: filled in by Tick; beyond: per chunk of the zone map,
        // its zone's record) is handed to the cover as a new version when a spot or chunk known both
        // times, at the same place, moved by about a centimetre, or "anything at all" became true or
        // false. Spots and chunks coming in and the grids moving wait for the next such change (at
        // most 10 s): the grid fades into the zones' values toward its edge anyway, so they barely
        // differ, and every publish makes the cover re-fold its depth (several ms per level).
        double g_lastSnowPublish = 0.0;
        void PublishSnow()
        {
            SnowMap& now = g_snowNow;
            now.firstI = g_firstI; now.firstJ = g_firstJ;
            const uint32_t playerZone = regional::PlayerZone();
            now.outsideFallen = regional::SnowDepth(g_map, playerZone);
            now.outsideKeep = regional::PaintedKeep(g_map, playerZone);
            const regional::ZoneMap& zm = regional::Map();
            if (zm.zone && zm.size > 0)
            {
                const size_t n = static_cast<size_t>(zm.size) * zm.size;
                now.farFallen.resize(n); now.farKeep.resize(n); now.farKnown.resize(n);
                uint32_t lastZone = 0xFFFFFFFFu;
                float f = now.outsideFallen, k = now.outsideKeep;
                for (size_t q = 0; q < n; ++q)
                {
                    const uint32_t z = zm.zone[q];
                    now.farKnown[q] = z != 0;
                    if (z != lastZone)
                    {
                        f = z ? regional::SnowDepth(g_map, z) : now.outsideFallen;
                        k = z ? regional::PaintedKeep(g_map, z) : now.outsideKeep;
                        lastZone = z;
                    }
                    now.farFallen[q] = f; now.farKeep[q] = k;
                }
                now.farSize = zm.size; now.farCell = zm.cellSize; now.farFirstI = zm.firstI; now.farFirstJ = zm.firstJ;
            }
            else now.farSize = 0;
            now.valid = true;

            auto some = [](float f, float k) { return f > 0.001f || k < 0.999f; };
            bool active = some(now.outsideFallen, now.outsideKeep);
            for (size_t q = 0; q < now.fallen.size() && !active; ++q) active = some(now.fallen[q], now.keep[q]);
            for (size_t q = 0; q < now.farFallen.size() && now.farSize > 0 && !active; ++q) active = some(now.farFallen[q], now.farKeep[q]);
            now.active = active;

            // Only values that changed at the same place count: a slot compared across a moved grid,
            // or a chunk whose zone only just became known, holds another place (flying, every tick).
            const SnowMap& pub = g_snowPub;
            auto moved = [](float f0, float k0, float f1, float k1)
            {
                return std::fabs(f0 - f1) > kSnowPublishYards || std::fabs(k0 - k1) * snow::kTypicalPaintedDepth > kSnowPublishYards;
            };
            bool publish = !pub.valid || pub.active != now.active || moved(now.outsideFallen, now.outsideKeep, pub.outsideFallen, pub.outsideKeep);
            const bool sameNear = pub.firstI == now.firstI && pub.firstJ == now.firstJ;
            for (size_t q = 0; sameNear && q < now.fallen.size() && !publish; ++q)
                if (now.fallen[q] >= 0.0f && pub.fallen[q] >= 0.0f) publish = moved(now.fallen[q], now.keep[q], pub.fallen[q], pub.keep[q]);
            const bool sameFar = pub.farSize == now.farSize && pub.farFirstI == now.farFirstI && pub.farFirstJ == now.farFirstJ;
            for (size_t q = 0; sameFar && q < now.farFallen.size() && now.farSize > 0 && !publish; ++q)
                if (now.farKnown[q] && pub.farKnown[q]) publish = moved(now.farFallen[q], now.farKeep[q], pub.farFallen[q], pub.farKeep[q]);
            // Spots and chunks that came in, a moved grid: at the latest after 10 s (only where there's snow).
            if (!publish && now.active && regional::Now() - g_lastSnowPublish > 10.0)
            {
                publish = !sameNear || !sameFar || pub.farKnown != now.farKnown;
                for (size_t q = 0; q < now.fallen.size() && !publish; ++q) publish = (now.fallen[q] >= 0.0f) != (pub.fallen[q] >= 0.0f);
            }
            if (!publish) return;
            g_snowPub = now;
            ++g_snowVersion;
            ++g_snowPublishes;
            g_lastSnowPublish = regional::Now();
        }

        // One moisture step for every cell.
        void Tick(float dt)
        {
            const double t0 = grassperf::Now();
            const climate::Weather& weather = climate::CurrentWeather();
            const bool raining = weather.type == 1 && weather.intensity > 0.0f;
            // The weather the client knows is the player's zone's: rain only falls there.
            const uint32_t playerZone = regional::PlayerZone();
            // Zone borders as gradients: per cell, the share of the player's zone around it (rain falls
            // by it) and the mix of the zones' rain soak around it (new ground starts from it).
            // The zones' fallen snow blends across borders the same way.
            static std::vector<float> share(kSize * kSize), soak(kSize * kSize), snowMix(kSize * kSize), weight(kSize * kSize),
                                      tmpA(kSize * kSize), tmpB(kSize * kSize), tmpC(kSize * kSize), tmpW(kSize * kSize);
            {
                uint32_t lastZone = 0xFFFFFFFFu;
                float zoneSoak = 0.0f, zoneSnow = 0.0f;
                for (int b = 0; b < kSize; ++b)
                    for (int a = 0; a < kSize; ++a)
                    {
                        const int k = b * kSize + a;
                        const Cell* c = Live(a, b);
                        if (!c) { share[k] = soak[k] = snowMix[k] = weight[k] = 0.0f; continue; }
                        if (c->zone != lastZone)
                        {
                            zoneSoak = regional::RainSoak(g_map, c->zone);
                            zoneSnow = regional::SnowDepth(g_map, c->zone);
                            lastZone = c->zone;
                        }
                        share[k] = (c->zone == playerZone || !c->zone) ? 1.0f : 0.0f;
                        soak[k] = zoneSoak;
                        snowMix[k] = zoneSnow;
                        weight[k] = 1.0f;
                    }
                // Separable box blur (running sums), weighted by which cells are known.
                auto blur = [&](bool rows)
                {
                    for (int line = 0; line < kSize; ++line)
                    {
                        float sa = 0.0f, sb = 0.0f, sc = 0.0f, sw = 0.0f;
                        auto idx = [&](int n) { return rows ? line * kSize + n : n * kSize + line; };
                        for (int n = -kZoneBlend; n < kSize + kZoneBlend; ++n)
                        {
                            const int in = n + kZoneBlend, out = n - kZoneBlend;
                            if (in >= 0 && in < kSize) { const int k = idx(in); sa += share[k] * weight[k]; sb += soak[k] * weight[k]; sc += snowMix[k] * weight[k]; sw += weight[k]; }
                            if (out >= 0 && out < kSize) { const int k = idx(out); sa -= share[k] * weight[k]; sb -= soak[k] * weight[k]; sc -= snowMix[k] * weight[k]; sw -= weight[k]; }
                            if (n >= 0 && n < kSize)
                            {
                                const int k = idx(n);
                                tmpA[k] = sw > 0.0f ? sa / sw : 0.0f;
                                tmpB[k] = sw > 0.0f ? sb / sw : 0.0f;
                                tmpC[k] = sw > 0.0f ? sc / sw : 0.0f;
                                tmpW[k] = sw > 0.0f ? 1.0f : 0.0f;
                            }
                        }
                    }
                    share.swap(tmpA); soak.swap(tmpB); snowMix.swap(tmpC); weight.swap(tmpW);
                };
                blur(true);
                blur(false);
            }
            const float snowing = weather.type == 2 ? weather.intensity : 0.0f;
            const float wind = wind::SteadyGround();

            // Temperature and humidity per area, once per tick.
            double ringSum = 0.0, ringTypical = 0.0;
            unsigned ringCount = 0;
            uint32_t lastArea = 0xFFFFFFFFu, lastZone = 0xFFFFFFFFu;
            float temperature = 0.0f, humidity = 0.5f, zoneT = 0.0f, zoneKeep = 1.0f;
            for (int b = 0; b < kSize; ++b)
                for (int a = 0; a < kSize; ++a)
                {
                    const int s = SlotOf(g_firstI + a, g_firstJ + b);
                    Cell* live = Live(a, b);
                    if (!live) { g_excess[s] = 0; g_snowNow.fallen[s] = -1.0f; g_snowNow.keep[s] = 1.0f; continue; }
                    Cell& c = *live;
                    if (c.area != lastArea)
                    {
                        const climate::Row& row = climate::For(c.area, g_map);
                        temperature = climate::Temperature(row).total;
                        humidity = row.humidity;
                        lastArea = c.area;
                    }
                    if (c.zone != lastZone)
                    {
                        zoneT = regional::ZoneTemperature(g_map, c.zone);
                        zoneKeep = regional::PaintedKeep(g_map, c.zone);
                        lastZone = c.zone;
                    }
                    // The ground's temperature: the air, warmed (or cooled) near a liquid with a temperature.
                    // Heat from a hot liquid or hot ground nearby: whichever moves it more (they don't add up).
                    const float fromLiquid = c.nearHot ? c.heat * kHeatShare * (c.nearTemp - temperature) : 0.0f;
                    const float fromGround = c.groundHeat > 0.0f ? c.groundHeat * kHeatShare * (c.groundHeatTemp - temperature) : 0.0f;
                    c.temperature = temperature + (std::fabs(fromGround) > std::fabs(fromLiquid) ? fromGround : fromLiquid);

                    // --- snow (env/Snow.hpp) ---
                    // Effective temperature: the surface (the ground's, toward a hot material's own by
                    // its painted share) plus the sun on this slope.
                    const float surface = c.hotShare > 0.0f ? c.temperature + (c.hotTemp - c.temperature) * c.hotShare : c.temperature;
                    const float normal[3] = { c.nx, c.ny, std::sqrt(std::fmax(0.0f, 1.0f - c.nx * c.nx - c.ny * c.ny)) };
                    const float sun = climate::SunWarming(normal, c.open);
                    const float teff = surface + sun;
                    // Local heat's reach (0..1): on a hot source itself everything melts.
                    float reach = c.nearHot && c.nearTemp >= snow::kHotSource ? c.heat : 0.0f;
                    if (c.groundHeatTemp >= snow::kHotSource && c.groundHeat > reach) reach = c.groundHeat;
                    if (c.hotShare >= kHotGroundShare && c.hotTemp >= snow::kHotSource) reach = 1.0f;
                    // Fallen snow follows its zones' (blended), less where this spot is warmer than they are.
                    const float warmer = std::fmax(0.0f, teff) - std::fmax(0.0f, zoneT);
                    const float hold = warmer <= 0.0f ? 1.0f : Smooth(0.0f, snow::kLocalMeltRange, warmer);
                    const float mixSnow = snowMix[b * kSize + a];
                    const float snowTarget = c.submerged ? 0.0f : mixSnow * c.open * hold * (1.0f - reach);
                    if (!c.snowStarted) c.fallen = snowTarget;
                    else
                    {
                        const float ease = dt / (snowTarget < c.fallen ? kSnowDownSeconds : kSnowUpSeconds);
                        c.fallen += (snowTarget - c.fallen) * (ease >= 1.0f ? 1.0f : ease);
                    }
                    // Painted snow: its kept share toward the cap, melt water into the ground.
                    float cap = 1.0f;
                    if (c.snowPainted > 0.0f)
                    {
                        cap = snow::MeltCap(teff, c.keptShare, c.goneTemp) * (1.0f - reach);
                        if (!c.snowStarted) c.keep = zoneKeep * (1.0f - reach); // pits by heat sources are already there
                        else
                        {
                            const float melted = snow::StepKeep(c.keep, cap, teff, reach, snowing * share[b * kSize + a] * c.open, c.snowPainted, dt);
                            if (melted > 0.0f && teff < snow::kEvaporateTemperature)
                                c.meltWet = 1.0f - (1.0f - c.meltWet) * std::exp(-melted * snow::kMeltToSoak);
                        }
                    }
                    else c.keep = 1.0f;
                    if (c.meltWet > 0.0f)
                    {
                        const float dry = regional::DryingRate(c.temperature, humidity, wind) * dt;
                        c.meltWet -= c.meltWet * (dry > 1.0f ? 1.0f : dry);
                    }
                    c.snowStarted = true;
                    c.lastSnowTarget = snowTarget; c.lastSnowMix = mixSnow; c.lastHold = hold; c.lastTeff = teff; c.lastSun = sun;
                    c.lastZoneT = zoneT; c.lastCap = cap; c.lastReach = reach;
                    g_snowNow.fallen[s] = c.fallen;
                    g_snowNow.keep[s] = c.keep;

                    const float eq = Equilibrium(c, c.temperature, humidity);
                    // The rain lives in the zone records (env/Regional: rain only in the player's zone,
                    // catch-up, drying by each zone's climate); a spot follows the zones around it.
                    // The zones' soak mix s is what open, fully absorbent ground would have gained
                    // (1 - s = exp(-rain)); ground taking it at absorbency x open sky is at
                    // 1 - (1 - eq) * (1 - s)^(absorbency * open), on top of its own equilibrium (rest,
                    // water nearby, heat). One simulation, so the ground can't drift from its zone, and
                    // the soak mix blends across zone borders.
                    const float mix = soak[b * kSize + a];
                    const float rainShare = raining && c.open > 0.0f ? share[b * kSize + a] * c.open : 0.0f;
                    // The spot's own melt water (painted snow melting here) soaks it from below like
                    // water nearby: at its absorbency, roof or not.
                    const float target = c.submerged ? c.liquidMoisture
                                       : 1.0f - (1.0f - eq) * std::pow(1.0f - (mix < 0.999f ? mix : 0.999f), c.absorbency * c.open)
                                                            * std::pow(1.0f - (c.meltWet < 0.999f ? c.meltWet : 0.999f), c.absorbency);
                    const float before = c.value;
                    if (!c.started) { c.value = target; c.started = true; }
                    else c.value += (target - c.value) * (dt >= kFollowSeconds ? 1.0f : dt / kFollowSeconds); // eased, so nothing pops
                    c.value = Clamp01(c.value);
                    c.lastShare = share[b * kSize + a]; c.lastSoak = mix; c.lastRain = rainShare;
                    c.lastChange = c.value - before; c.lastDt = dt;
                    const float excess = c.value - c.rest;
                    g_excess[s] = static_cast<uint8_t>(Clamp01(excess) * 255.0f + 0.5f);
                    // The outer ring (8 cells) on dry land feeds the value used beyond the grid.
                    if (!c.submerged && (a < kRing || b < kRing || a >= kSize - kRing || b >= kSize - kRing))
                    { ringSum += Clamp01(excess); ringTypical += TypicalExcess(mix); ++ringCount; }
                }
            // Beyond the grid: each chunk's zone, as typical ground, scaled so the grid's outer ring
            // and the far values agree where they meet (its real materials differ from "typical").
            float scale = 1.0f;
            if (ringCount && ringTypical / ringCount > 0.02)
            {
                scale = static_cast<float>((ringSum / ringCount) / (ringTypical / ringCount));
                scale = scale < 0.25f ? 0.25f : (scale > 4.0f ? 4.0f : scale);
            }
            const float playerFar = Clamp01(scale * TypicalExcess(regional::RainSoak(g_map, playerZone)));
            g_wet.farExcess = playerFar;
            const regional::ZoneMap& zm = regional::Map();
            if (zm.zone && zm.size > 0)
            {
                const size_t n = static_cast<size_t>(zm.size) * zm.size;
                g_farBytes.resize(n); g_farValues.resize(n);
                uint32_t lastZone = 0xFFFFFFFFu;
                float lastValue = playerFar;
                for (size_t k = 0; k < n; ++k)
                {
                    const uint32_t z = zm.zone[k];
                    if (z != lastZone) { lastValue = z ? Clamp01(scale * TypicalExcess(regional::RainSoak(g_map, z))) : playerFar; lastZone = z; }
                    g_farValues[k] = lastValue;
                    g_farBytes[k] = static_cast<uint8_t>(lastValue * 255.0f + 0.5f);
                }
                g_wet.farSize = zm.size; g_wet.farCellSize = zm.cellSize;
                g_wet.farFirstI = zm.firstI; g_wet.farFirstJ = zm.firstJ;
                g_wet.farBytes = g_farBytes.data(); g_wet.farValues = g_farValues.data();
            }
            ++g_wet.version;
            PublishSnow();
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
        if (field == kSnow)
        {
            const Cell* c = CellAt(pos);
            if (!c || !c->snowStarted) return false;
            out = c->fallen + c->snowPainted * c->keep;
            return true;
        }
        return false;
    }

    SnowDetail Snow(const float pos[3])
    {
        SnowDetail d;
        const Cell* c = CellAt(pos);
        if (!c || !c->snowStarted) return d;
        d.known = true;
        d.fallen = c->fallen; d.target = c->lastSnowTarget; d.zoneSnow = c->lastSnowMix; d.hold = c->lastHold;
        d.painted = c->snowPainted; d.keep = c->keep; d.cap = c->lastCap;
        d.keptShare = c->keptShare; d.goneTemperature = c->goneTemp;
        d.temperature = c->lastTeff; d.sun = c->lastSun; d.zoneTemperature = c->lastZoneT;
        d.heatReach = c->lastReach; d.meltWet = c->meltWet;
        return d;
    }

    void SnowAt(float x, float y, float& fallen, float& keep)
    {
        const SnowMap& p = g_snowPub;
        fallen = 0.0f; keep = 1.0f;
        if (!p.valid) return;

        // The zones' values (per terrain chunk, bilinear between chunk centres).
        float farF = p.outsideFallen, farK = p.outsideKeep;
        if (p.farSize > 0)
        {
            const float u = x / p.farCell - 0.5f, v = y / p.farCell - 0.5f;
            const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v));
            const float fx = u - i0, fy = v - j0;
            float sf = 0.0f, sk = 0.0f, sw = 0.0f;
            for (int q = 0; q < 4; ++q)
            {
                const int i = i0 + (q & 1), j = j0 + (q >> 1);
                if (i < p.farFirstI || i >= p.farFirstI + p.farSize || j < p.farFirstJ || j >= p.farFirstJ + p.farSize) continue;
                const float w = ((q & 1) ? fx : 1.0f - fx) * ((q >> 1) ? fy : 1.0f - fy);
                const int mi = i % p.farSize, mj = j % p.farSize;
                const size_t k = static_cast<size_t>((mj < 0 ? mj + p.farSize : mj) * p.farSize + (mi < 0 ? mi + p.farSize : mi));
                sf += w * p.farFallen[k]; sk += w * p.farKeep[k]; sw += w;
            }
            if (sw > 0.0f) { farF = (sf + (1.0f - sw) * farF); farK = (sk + (1.0f - sw) * farK); }
        }
        fallen = farF; keep = farK;

        // The near grid (bilinear between cell centres, over the spots known), fading into the zones'
        // values within its last yards.
        const float u = x / kCell - 0.5f, v = y / kCell - 0.5f;
        const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v));
        if (i0 + 1 < p.firstI || i0 >= p.firstI + kSize || j0 + 1 < p.firstJ || j0 >= p.firstJ + kSize) return;
        const float fx = u - i0, fy = v - j0;
        float sf = 0.0f, sk = 0.0f, sw = 0.0f;
        for (int q = 0; q < 4; ++q)
        {
            const int i = i0 + (q & 1), j = j0 + (q >> 1);
            if (i < p.firstI || i >= p.firstI + kSize || j < p.firstJ || j >= p.firstJ + kSize) continue;
            const int s = SlotOf(i, j);
            if (p.fallen[s] < 0.0f) continue;
            const float w = ((q & 1) ? fx : 1.0f - fx) * ((q >> 1) ? fy : 1.0f - fy);
            sf += w * p.fallen[s]; sk += w * p.keep[s]; sw += w;
        }
        if (sw <= 0.0f) return;
        const float edge = std::fmin(std::fmin(x - p.firstI * kCell, (p.firstI + kSize) * kCell - x),
                                     std::fmin(y - p.firstJ * kCell, (p.firstJ + kSize) * kCell - y));
        const float inner = Clamp01(edge / kSnowEdgeFade) * sw;
        fallen = farF + (sf / sw - farF) * inner;
        keep = farK + (sk / sw - farK) * inner;
    }

    uint32_t SnowVersion() { return g_snowVersion; }
    bool     SnowActive()  { return g_snowPub.valid && g_snowPub.active; }

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
        d.zone = c->zone; d.zoneShare = c->lastShare; d.zoneSoakMix = c->lastSoak; d.rainShare = c->lastRain;
        d.changePerSecond = c->lastDt > 0.0f ? c->lastChange / c->lastDt : 0.0f;
        d.hotShare = c->hotShare; d.hotTemperature = c->hotTemp;
        d.groundHeat = c->groundHeat; d.groundHeatTemperature = c->groundHeatTemp; d.groundHeatDistance = c->groundHeatDist;
        d.value = c->value;
        return d;
    }

    const WetGrid& Wet() { return g_wet; }
    Stats GetStats()
    {
        Stats s = g_stats;
        s.rainSoak = regional::RainSoak(g_map, regional::PlayerZone());
        s.snowPublished = g_snowPublishes;
        return s;
    }

    void Refill()
    {
        for (Cell& c : g_cells) c.filled = false;
        g_fillCursor = 0;
    }

    void Reset()
    {
        for (Cell& c : g_cells) c = Cell{};
        std::fill(g_excess.begin(), g_excess.end(), 0);
        g_haveGrid = false;
        g_fillCursor = 0;
        ++g_wet.version;
        std::fill(g_snowNow.fallen.begin(), g_snowNow.fallen.end(), -1.0f);
        g_snowPub = SnowMap{};
        ++g_snowVersion;
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
