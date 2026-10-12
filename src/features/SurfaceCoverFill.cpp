#include "SurfaceCoverInternal.hpp"

#include "GrassPerf.hpp"

#include <array>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::cover::detail
{
    constexpr float kEdgeWarpYards = 5.0f; // how far the material edge wanders at full breakup

    // What the table says about one terrain layer (or cell): its own row (Depth > 0 = a painted
    // cover), whether that cover is snow (fallen snow adds to it), and the look fallen snow takes
    // on it where nothing is painted.
    struct LayerRows
    {
        covertable::Values painted;
        covertable::Values look;
        bool paintedSnow = false;
    };
    LayerRows RowsFor(uint32_t area, int map, const char* texture, uint32_t effect, int terrainType)
    {
        LayerRows r;
        r.painted = covertable::Resolve(area, map, texture, effect, terrainType);
        r.paintedSnow = r.painted.depth > 0.0f && covertable::IsSnow(r.painted.coverMaterial, area, map);
        r.look = covertable::SnowLook(area, map, texture, effect, terrainType);
        return r;
    }

    // The table's values per terrain cell (what the cell is made of is constant over one: it's
    // the cell's dominant layer). Unloaded cells aren't cached, so they're asked again.
    constexpr float kTerrainCell = 33.3333333f / 8.0f;
    // A fixed window of terrain cells around the player (384 cells = +-800 yd, beyond the
    // outermost level), indexed by cell mod the window: one array access per lookup.
    constexpr int kCellWindow = 384;
    struct CellEntry { int cx = INT_MIN, cy = INT_MIN; LayerRows rows; };
    std::vector<CellEntry> g_cellCache(kCellWindow * kCellWindow);
    unsigned g_cellMisses = 0;

    void ClearCellCache() { for (CellEntry& e : g_cellCache) e.cx = INT_MIN; }

    // The table's values for every layer of a decoded chunk, by the chunk's decode serial.
    std::unordered_map<unsigned, std::array<LayerRows, 4>> g_layerValues;

    // The table's values at the centre of terrain cell (cx, cy); null while its chunk isn't loaded.
    const LayerRows* CellRows(int cx, int cy)
    {
        const int wx = ((cx % kCellWindow) + kCellWindow) % kCellWindow, wy = ((cy % kCellWindow) + kCellWindow) % kCellWindow;
        CellEntry& e = g_cellCache[wy * kCellWindow + wx];
        if (e.cx == cx && e.cy == cy) return &e.rows;
        terrain::Surface surface;
        if (!terrain::SurfaceAt((cx + 0.5f) * kTerrainCell, (cy + 0.5f) * kTerrainCell, surface))
            return nullptr; // not loaded (or a hole): ask again later
        e.rows = RowsFor(surface.area, g_mapNow, surface.texture, surface.groundEffect, surface.terrainType);
        e.cx = cx; e.cy = cy;
        ++g_cellMisses;
        return &e.rows;
    }

    // The cover between terrain cell centres: how covered (0..1, blended, so the cover tapers out
    // over about one terrain cell instead of stopping in a staircase), and every other property
    // as the covered neighbours' values weighted the same way (so an edge isn't thinned twice,
    // and two materials meeting blend their depth, slope limits, tint, ...).
    //
    // Fallen snow: layers without a painted cover add their snow look to every property but the
    // depth -- only where little is painted (fully below a quarter covered), so a painted cover's
    // own look stays as it was. snowShare says how much of the spot fallen snow may lie on
    // (shaped and filled in at upload), paintedSnow the share of the painted depth that is snow.
    struct Blend
    {
        float covered = 0.0f, depth = 0.0f, maxSlope = 45.0f, slopeFade = 15.0f, drift = 0.0f, breakup = 0.5f;
        float tint[3] = {}, tintStrength = 0.0f;
        float zOffset = 0.0f, wetness = 0.0f, stiffness = 0.0f, rim = 0.3f, relax = 30.0f;
        float vertexColor[3] = { 0.5f, 0.5f, 0.5f }; // MCCV mixed in by the rows' "use MCCV" share; 0.5 = neutral
        int   texId[8] = {}; float texShare[8] = {}; int texCount = 0; // cover textures present, by ID
        float snowShare = 0.0f, paintedSnow = 0.0f;
        bool  hasLook = false; // some row (painted or fallen snow's look) set the properties above
    };

    // Sums per blended property, in this order (cover textures go through AddTexture).
    enum Sum { kSumDepth, kSumMaxSlope, kSumSlopeFade, kSumDrift, kSumBreakup, kSumTintR, kSumTintG, kSumTintB,
               kSumTintStrength, kSumZOffset, kSumWetness, kSumMccv, kSumStiffness, kSumRim, kSumRelax, kSumCount };

    // What the rows' flags need to know about the spot being sampled: how deep it lies under a
    // liquid surface and the terrain's vertex colour there. Both are looked up only when a row
    // there asks (lazily, once per sample), and always at the sample's own position -- not the
    // warped material lookup point -- so the shoreline follows the water exactly.
    constexpr float kShoreFade = 0.3f; // yd under the surface over which "prevent" covers taper
    struct Probe
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool  valid = false;
        int   waterState = -1;        // -1 not asked, 0 no liquid, 1 liquid
        float waterDepth = 0.0f;      // surface - terrain (negative: the terrain is above it)
        int   colorState = -1;
        float color[3] = { 0.5f, 0.5f, 0.5f };

        float UnderWater()
        {
            if (waterState < 0)
            {
                float h;
                waterState = valid && terrain::LiquidHeightAt(x, y, h) ? 1 : 0;
                waterDepth = waterState ? h - z : 0.0f;
            }
            return waterState ? waterDepth : -1.0e6f;
        }
        const float* VertexColor()
        {
            if (colorState < 0) colorState = valid && terrain::VertexColorAt(x, y, color) ? 1 : 0;
            return color;
        }
    };
    Probe g_probe;

    // A row's weight after its water flags: "prevent underwater" thins out below the surface,
    // "prevent on land" only exists there.
    float WaterFactor(uint32_t flags)
    {
        if (!(flags & (covertable::kFlagPreventUnderwater | covertable::kFlagPreventOnLand))) return 1.0f;
        const float d = g_probe.UnderWater() / kShoreFade;
        float f = 1.0f;
        if (flags & covertable::kFlagPreventUnderwater) f *= d <= 0.0f ? 1.0f : (d >= 1.0f ? 0.0f : 1.0f - d);
        if (flags & covertable::kFlagPreventOnLand)     f *= d <= 0.0f ? 0.0f : (d >= 1.0f ? 1.0f : d);
        return f;
    }

    const std::array<LayerRows, 4>& LayerValues(const terrain::LayerWeights& lw)
    {
        auto found = g_layerValues.find(lw.serial);
        if (found != g_layerValues.end()) return found->second;
        if (g_layerValues.size() > 3000) g_layerValues.clear();
        std::array<LayerRows, 4>& v = g_layerValues[lw.serial];
        for (int l = 0; l < lw.layers; ++l)
        {
            const terrain::Surface& sf = lw.surface[l];
            v[l] = RowsFor(sf.area, g_mapNow, sf.texture, sf.groundEffect, sf.terrainType);
        }
        return v;
    }

    // From the painted strength of every texture layer: each layer's row values, weighted by
    // how strongly it's painted here (alpha maps, ~0.5 yd). Edges follow what was painted, and
    // two materials painted over each other mix.
    Blend* g_blendTarget = nullptr; // where AddTexture collects (the Blend being built)
    void AddTexture(int id, float w)
    {
        Blend& b = *g_blendTarget;
        for (int i = 0; i < b.texCount; ++i) if (b.texId[i] == id) { b.texShare[i] += w; return; }
        if (b.texCount < 8) { b.texId[b.texCount] = id; b.texShare[b.texCount++] = w; }
    }

    // One row's look (everything but the depth) at weight wk.
    void AddLook(const covertable::Values& cv, float wk, float& lookW, float sum[kSumCount])
    {
        if (wk <= 0.0f) return;
        lookW += wk;
        sum[kSumMaxSlope] += wk * cv.maxSlope; sum[kSumSlopeFade] += wk * cv.slopeFade;
        sum[kSumDrift] += wk * cv.driftNoise; sum[kSumBreakup] += wk * cv.edgeBreakup;
        sum[kSumTintR] += wk * ((cv.tintColor >> 16) & 0xFF) / 255.0f;
        sum[kSumTintG] += wk * ((cv.tintColor >> 8) & 0xFF) / 255.0f;
        sum[kSumTintB] += wk * (cv.tintColor & 0xFF) / 255.0f;
        sum[kSumTintStrength] += wk * cv.tintStrength;
        sum[kSumZOffset] += wk * cv.zOffset;
        sum[kSumWetness] += wk * cv.wetness;
        sum[kSumStiffness] += wk * cv.stiffness;
        sum[kSumRim] += wk * cv.rim;
        sum[kSumRelax] += wk * cv.relaxSeconds;
        if (cv.flags & covertable::kFlagUseVertexColor) sum[kSumMccv] += wk;
        if (cv.coverTexture > 0) AddTexture(cv.coverTexture, wk);
    }

    void FinishBlend(Blend& b, float lookW, const float sum[kSumCount])
    {
        if (b.covered > 0.0f) b.depth = sum[kSumDepth] / b.covered;
        if (b.covered > 1.0f) b.covered = 1.0f;
        if (lookW <= 0.0f) return;
        b.hasLook = true;
        const float inv = 1.0f / lookW;
        b.maxSlope = sum[kSumMaxSlope] * inv; b.slopeFade = sum[kSumSlopeFade] * inv;
        b.drift = sum[kSumDrift] * inv; b.breakup = sum[kSumBreakup] * inv;
        b.tint[0] = sum[kSumTintR] * inv; b.tint[1] = sum[kSumTintG] * inv; b.tint[2] = sum[kSumTintB] * inv;
        b.tintStrength = sum[kSumTintStrength] * inv;
        b.zOffset = sum[kSumZOffset] * inv;
        b.wetness = sum[kSumWetness] * inv;
        b.stiffness = sum[kSumStiffness] * inv;
        b.rim = sum[kSumRim] * inv;
        b.relax = sum[kSumRelax] * inv;
        const float mccv = sum[kSumMccv] * inv;
        if (mccv > 0.0f)
        {
            const float* c = g_probe.VertexColor();
            for (int k = 0; k < 3; ++k) b.vertexColor[k] = 0.5f + (c[k] - 0.5f) * mccv;
        }
        for (int t = 0; t < b.texCount; ++t) b.texShare[t] *= inv;
    }

    // The layers (or cells) at a point with their weights, combined. Painted rows (Depth > 0,
    // after their water flags) make the cover and its look; the rest of every layer is bare
    // ground, where fallen snow may lie with the layer's snow look.
    struct Part { float weight; const LayerRows* rows; };
    Blend Combine(const Part* parts, int count)
    {
        Blend b;
        g_blendTarget = &b;
        float sum[kSumCount] = {}, lookW = 0.0f, total = 0.0f, snowPainted = 0.0f, otherPainted = 0.0f;
        float paintedDepth = 0.0f, snowDepth = 0.0f;
        float bare[4] = {};
        for (int k = 0; k < count; ++k)
        {
            const float wk = parts[k].weight;
            if (wk <= 0.0f) continue;
            total += wk;
            const covertable::Values& pv = parts[k].rows->painted;
            const float pw = pv.depth > 0.0f ? wk * WaterFactor(pv.flags) : 0.0f;
            bare[k] = wk - pw;
            if (pw <= 0.0f) continue;
            b.covered += pw;
            sum[kSumDepth] += pw * pv.depth;
            AddLook(pv, pw, lookW, sum);
            paintedDepth += pw * pv.depth;
            if (parts[k].rows->paintedSnow) { snowPainted += pw; snowDepth += pw * pv.depth; }
            else otherPainted += pw;
        }
        if (total <= 0.0f) return b;
        // Fallen snow's look on the bare parts, fully only where nothing is painted.
        float bareTotal = 0.0f;
        for (int k = 0; k < count; ++k) bareTotal += bare[k];
        const float lookShare = 1.0f - (b.covered * 4.0f > 1.0f ? 1.0f : b.covered * 4.0f);
        if (lookShare > 0.0f)
            for (int k = 0; k < count; ++k) AddLook(parts[k].rows->look, bare[k] * lookShare, lookW, sum);
        // Where fallen snow may lie: bare ground and snow covers, not on or beside other covers,
        // not under a liquid's surface.
        const float other = otherPainted * 4.0f / total;
        const float dry = WaterFactor(covertable::kFlagPreventUnderwater);
        b.snowShare = (snowPainted + bareTotal) / total * (1.0f - (other > 1.0f ? 1.0f : other)) * dry;
        b.paintedSnow = paintedDepth > 0.0f ? snowDepth / paintedDepth : 0.0f;
        FinishBlend(b, lookW, sum);
        return b;
    }

    Blend BlendAt(float x, float y)
    {
        if (static_cast<Coverage>(g_settings.coverage) == Coverage::Everywhere)
        {
            // No painted cover; fallen snow anywhere above water, with the plain look.
            static const LayerRows kPlain = [] { LayerRows r; r.painted.depth = 0.0f; return r; }();
            const Part part{ 1.0f, &kPlain };
            return Combine(&part, 1);
        }
        Part parts[4];
        int count = 0;
        if (g_settings.materialSource == 0)
        {
            terrain::LayerWeights lw;
            if (!terrain::LayerWeightsAt(x, y, lw, g_settings.alphaSwap != 0)) return Blend{};
            const std::array<LayerRows, 4>& rows = LayerValues(lw);
            for (int l = 0; l < lw.layers && l < 4; ++l) parts[count++] = { lw.weight[l], &rows[l] };
            return Combine(parts, count);
        }
        const float u = x / kTerrainCell - 0.5f, v = y / kTerrainCell - 0.5f;
        const int cx = static_cast<int>(std::floor(u)), cy = static_cast<int>(std::floor(v));
        const float fx = u - cx, fy = v - cy;
        const int c[4][2] = { { cx, cy }, { cx + 1, cy }, { cx, cy + 1 }, { cx + 1, cy + 1 } };
        const float w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
        for (int k = 0; k < 4; ++k)
            if (const LayerRows* rows = CellRows(c[k][0], c[k][1])) parts[count++] = { w[k], rows };
        return Combine(parts, count);
    }

    uint8_t ToByte(float v) { return static_cast<uint8_t>((v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v)) * 255.0f + 0.5f); }

    // Smooth value noise in world space, 0..1. `seed` gives independent fields.
    float Hash(int x, int y, uint32_t seed = 0)
    {
        uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
    }

    float ValueNoise(float x, float y, uint32_t seed = 0)
    {
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        float fx = x - ix, fy = y - iy;
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        const float a = Hash(ix, iy, seed) + (Hash(ix + 1, iy, seed) - Hash(ix, iy, seed)) * fx;
        const float b = Hash(ix, iy + 1, seed) + (Hash(ix + 1, iy + 1, seed) - Hash(ix, iy + 1, seed)) * fx;
        return a + (b - a) * fy;
    }

    // Two scales: broad drifts and small lumps.
    float Drift(float x, float y) { return 0.65f * ValueNoise(x / 9.0f, y / 9.0f) + 0.35f * ValueNoise(x / 2.5f, y / 2.5f); }

    // Fine noise for ragged, patchy material edges.
    float EdgeNoise(float x, float y) { return 0.6f * ValueNoise(x / 1.6f, y / 1.6f, 3) + 0.4f * ValueNoise(x / 0.7f, y / 0.7f, 4); }

    // Base height and coverage parts of one world cell of a level (level 0 also resets its trench
    // state when `fresh`). Everything depends only on the world position, so the levels agree
    // wherever their vertices coincide. Returns what changed (kDirty* bits).
    uint8_t Sample(Level& L, bool isLevel0, int i, int j, bool fresh)
    {
        uint8_t changed = 0;
        const int s = SlotOf(i, j);
        const float x = i * L.cell, y = j * L.cell;
        float z;
        const bool ok = terrain::HeightAt(x, y, z);
        g_probe = Probe{};
        g_probe.x = x; g_probe.y = y; g_probe.z = z; g_probe.valid = ok;

        Blend b;
        uint8_t slopeF = 0;
        if (ok)
        {
            // The material edge wanders: the lookup point is pushed around by broad noise, so a
            // snowy terrain cell's border follows an irregular line instead of a rounded square.
            // How far comes from the rows near the unwarped point.
            const Blend around = BlendAt(x, y);
            const float breakup0 = (around.hasLook ? around.breakup : covertable::Values().edgeBreakup) * g_settings.breakupMul;
            const float warp = kEdgeWarpYards * (breakup0 > 1.0f ? 1.0f : breakup0);
            const float mx = x + warp * (2.0f * ValueNoise(x / 7.0f, y / 7.0f, 1) - 1.0f);
            const float my = y + warp * (2.0f * ValueNoise(x / 7.0f, y / 7.0f, 2) - 1.0f);
            b = BlendAt(mx, my);

            // Steepness 1 yd around (fixed, so every level gets the same value at the same spot),
            // against the rows' slope limit.
            float zx, zy;
            const float gx = terrain::HeightAt(x + 1.0f, y, zx) ? zx - z : 0.0f;
            const float gy = terrain::HeightAt(x, y + 1.0f, zy) ? zy - z : 0.0f;
            const float up = 1.0f / std::sqrt(1.0f + gx * gx + gy * gy);
            const float rad = 3.14159265f / 180.0f;
            const float cosMax = std::cos(b.maxSlope * rad), cosFull = std::cos((b.maxSlope - b.slopeFade) * rad);
            float t = cosFull > cosMax ? (up - cosMax) / (cosFull - cosMax) : (up >= cosMax ? 1.0f : 0.0f);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            slopeF = ToByte(t * t * (3.0f - 2.0f * t));
        }
        // The cover's base: the terrain, moved by the rows' ZOffset (below the terrain only what
        // rises above it shows, e.g. trench rims through grass).
        const float base = ok ? z + (b.hasLook ? b.zOffset : 0.0f) : kHole;
        if (base != L.base[s]) { L.base[s] = base; changed |= kDirtyBase; }
        const uint8_t material = ToByte(b.covered), driftAmp = ToByte(b.drift), breakup = ToByte(b.breakup), stiff = ToByte(b.stiffness);
        const uint8_t drift = static_cast<uint8_t>(Drift(x, y) * 255.0f + 0.5f);
        const uint8_t edge = static_cast<uint8_t>(EdgeNoise(x, y) * 255.0f + 0.5f);
        const uint8_t snowShare = ToByte(b.snowShare), paintedSnow = ToByte(b.paintedSnow);
        const uint32_t tint = (static_cast<uint32_t>(ToByte(b.tintStrength)) << 24) | (static_cast<uint32_t>(ToByte(b.tint[0])) << 16) |
                              (static_cast<uint32_t>(ToByte(b.tint[1])) << 8) | ToByte(b.tint[2]);
        if (material != L.material[s] || b.depth != L.depth[s] || slopeF != L.slope[s] || driftAmp != L.driftAmp[s] ||
            breakup != L.breakup[s] || drift != L.drift[s] || edge != L.edgeNoise[s] || stiff != L.stiff[s] ||
            snowShare != L.snowShare[s] || paintedSnow != L.paintedSnow[s])
        {
            L.coverCells += ((b.depth > 0.0f || paintedSnow) ? 1 : 0) - ((L.depth[s] > 0.0f || L.paintedSnow[s]) ? 1 : 0);
            L.snowCells += (snowShare ? 1 : 0) - (L.snowShare[s] ? 1 : 0);
            L.material[s] = material; L.depth[s] = b.depth; L.slope[s] = slopeF; L.driftAmp[s] = driftAmp;
            L.breakup[s] = breakup; L.drift[s] = drift; L.edgeNoise[s] = edge; L.stiff[s] = stiff;
            L.snowShare[s] = snowShare; L.paintedSnow[s] = paintedSnow;
            changed |= kDirtyCover;
        }
        int a = -1, c = -1; // the two strongest
        for (int i = 0; i < b.texCount; ++i)
        {
            if (a < 0 || b.texShare[i] > b.texShare[a]) { c = a; a = i; }
            else if (c < 0 || b.texShare[i] > b.texShare[c]) c = i;
        }
        const uint32_t ids = (a >= 0 ? static_cast<uint32_t>(b.texId[a]) & 0xFFFF : 0) | (c >= 0 ? (static_cast<uint32_t>(b.texId[c]) & 0xFFFF) << 16 : 0);
        const uint16_t shares = static_cast<uint16_t>((a >= 0 ? ToByte(b.texShare[a]) : 0) | (c >= 0 ? ToByte(b.texShare[c]) << 8 : 0));
        const uint32_t props = (static_cast<uint32_t>(ToByte(b.wetness)) << 24) | (static_cast<uint32_t>(ToByte(b.vertexColor[0])) << 16) |
                               (static_cast<uint32_t>(ToByte(b.vertexColor[1])) << 8) | ToByte(b.vertexColor[2]);
        if (tint != L.tint[s] || ids != L.texIds[s] || shares != L.texShares[s] || props != L.props[s])
        {
            L.tint[s] = tint; L.texIds[s] = ids; L.texShares[s] = shares; L.props[s] = props;
            changed |= kDirtyLook;
        }
        if (isLevel0)
        {
            if (b.rim != g_rimCell[s]) { g_rimCell[s] = b.rim; changed |= kDirtyCover; }
            g_relaxCell[s] = b.relax;
            if (fresh) { g_press[s] = 0.0f; g_rim[s] = 0.0f; }
        }
        return changed;
    }

    // Marks what changed for the uploads: a segment across the whole filled width as its row, a
    // shorter one (a recentre's strip along y) by its columns.
    void SampleRect(Level& L, bool isLevel0, int i0, int i1, int j0, int j1, bool fresh) // inclusive
    {
        const bool wholeRows = i1 - i0 + 1 >= kFilled;
        for (int j = j0; j <= j1; ++j)
        {
            uint8_t row = 0;
            for (int i = i0; i <= i1; ++i)
            {
                const uint8_t changed = Sample(L, isLevel0, i, j, fresh);
                if (wholeRows) row |= changed;
                else if (changed) L.dirtyCol[Slot(i)] |= changed;
            }
            if (row) L.dirtyRow[Slot(j)] |= row;
        }
    }

    void PlanFill(Level& L, const float pos[3], Plan plan)
    {
        const int pi = static_cast<int>(std::floor(pos[0] / L.cell)), pj = static_cast<int>(std::floor(pos[1] / L.cell));
        int ni = FloorDiv2(pi - kHalfCells) * 2, nj = FloorDiv2(pj - kHalfCells) * 2;
        const int span = kFilled - 1;
        if (plan == Plan::Recentre)
        {
            if (!L.haveGrid) plan = Plan::NewPlace;
            else if (std::abs(ni - L.gridI) < kRecentreCells && std::abs(nj - L.gridJ) < kRecentreCells) return;
            // The player left this level's grid entirely (a teleport): nothing on screen is worth
            // keeping, start over at the new place.
            else if (std::abs(ni - L.gridI) > kGridCells || std::abs(nj - L.gridJ) > kGridCells) plan = Plan::NewPlace;
            else
            {
                // Lagging behind (fast travel): follow in steps that fit the spare rows, so the
                // strips being filled never overwrite cells still on screen, and stay visible.
                constexpr int kMaxStep = ((kTex - kFilled) / 2) * 2;
                const int di = ni - L.gridI, dj = nj - L.gridJ;
                ni = L.gridI + (di > kMaxStep ? kMaxStep : (di < -kMaxStep ? -kMaxStep : di));
                nj = L.gridJ + (dj > kMaxStep ? kMaxStep : (dj < -kMaxStep ? -kMaxStep : dj));
            }
        }

        L.job.clear();
        L.jobPos = 0;
        L.jobI = ni; L.jobJ = nj;
        L.jobMs = 0;
        const int fi = ni - kApron, fj = nj - kApron;  // new filled area, first cell
        if (plan != Plan::Recentre)
        {
            for (int j = fj; j <= fj + span; ++j) L.job.push_back({ fi, fi + span, j });
        }
        else
        {
            const int oi0 = L.gridI - kApron, oi1 = oi0 + span, oj0 = L.gridJ - kApron, oj1 = oj0 + span;
            for (int j = fj; j <= fj + span; ++j)
            {
                if (j < oj0 || j > oj1) { L.job.push_back({ fi, fi + span, j }); continue; }
                if (fi < oi0) L.job.push_back({ fi, oi0 - 1, j });
                if (fi + span > oi1) L.job.push_back({ oi1 + 1, fi + span, j });
            }
        }
        L.jobActive = true;
        L.jobFull = plan != Plan::Recentre;
        L.jobHidden = plan == Plan::NewPlace;
        if (L.jobHidden) L.haveGrid = false;
    }

    // Works one level's job until the deadline; true when the job is done.
    bool WorkFill(Level& L, bool isLevel0, double deadline)
    {
        const double t0 = grassperf::Now();
        while (L.jobPos < L.job.size())
        {
            const Level::Segment& seg = L.job[L.jobPos++];
            SampleRect(L, isLevel0, seg.i0, seg.i1, seg.j, seg.j, true);
            if (grassperf::Now() >= deadline) break;
        }
        L.jobMs += grassperf::Now() - t0;
        if (L.jobPos < L.job.size()) return false;

        L.gridI = L.jobI; L.gridJ = L.jobJ;
        L.haveGrid = true;
        // A recentre only changed its strips (marked while sampling); a full fill changed everything.
        if (L.jobFull)
        {
            L.coverDirty = L.baseDirty = L.lookDirty = true;
            L.firstFillMs = L.jobMs;
            L.boostRows = kFilled; // chunks still streaming in get picked up quickly
        }
        L.fillMs = L.jobMs;
        L.jobActive = false;
        L.job.clear();
        return true;
    }

    void ClearLayerValues() { g_layerValues.clear(); }
}
