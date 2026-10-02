#include "SurfaceCover.hpp"

#include "../env/Actors.hpp"
#include "../env/TerrainHeight.hpp"
#include "GrassPerf.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::cover
{
    namespace
    {
        namespace ev = wxl::events;
        namespace gx = wxl::game::gx;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";
        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: surface cover (spike)";

        // --- grid ------------------------------------------------------------------------------
        // A clipmap: every level is the same 320x320-cell grid (4 patches of 160x160 cells, 161x161
        // vertices, 16-bit indices), level k with cells of 0.25 * 2^k yd, so the levels reach 40, 80,
        // 160, 320 and 640 yd from the player. Only level 0 deforms (trenches); the coarser ones carry the
        // static cover. Each level has its own toroidal textures, one texel per world cell of that
        // level; 384 > 321 so a slot is never shared by two vertices of the grid at once.
        //
        // Seams: every level's origin snaps to an even cell, so its border lies on the next coarser
        // level's vertex lines. In a band along the border the odd vertices morph onto their even
        // neighbours, which turns the edge into exactly the coarser level's triangles; the coarser
        // level discards its pixels inside the finer level's box. In the band the normals switch to the
        // coarser level's spacing too, or the two levels shade the same surface differently.
        constexpr float kCell0 = 0.25f;
        constexpr int   kMaxLevels = 5;
        constexpr int   kPatchCells = 160, kPatchVerts = kPatchCells + 1, kPatches = 2;
        constexpr int   kGridCells = kPatchCells * kPatches, kGridVerts = kGridCells + 1, kHalfCells = kGridCells / 2;
        constexpr int   kTex = 384;
        constexpr int   kApron = 2;                // cells filled beyond the grid on every side (border normals)
        constexpr int   kFilled = kGridVerts + 2 * kApron;
        static_assert(kFilled <= kTex, "the filled area must fit the toroidal texture");
        constexpr int   kRecentreCells = 16;       // recentre after the player moved 16 cells of the level
        constexpr int   kMorphCells = 16;          // morph band along a level's border
        constexpr int   kDeformFullCells = 120;    // level 0: trenches at full strength inside this (Chebyshev, cells)...
        constexpr int   kDeformZeroCells = 140;    // ...gone by this, before the morph band starts
        constexpr float kOuterFadeStart = 0.80f, kOuterFadeEnd = 0.97f; // outermost level: cover fades out (share of its half)
        constexpr float kHole = -100000.0f;        // base height of a hole / unloaded spot

        // TerrainType storage (WowClientDB 0xAD4C34): a row's Flags at +0x14, 0x1 = footprints.
        constexpr uintptr_t kTerrainMinId = 0x00AD4C44, kTerrainMaxId = 0x00AD4C40, kTerrainIndex = 0x00AD4C54;
        constexpr size_t    kTerrainFlags = 0x14;

        enum class Coverage : int { Everywhere = 0, FootprintTypes = 1 };

        struct Settings
        {
            int   enabled = 0;
            int   coverage = static_cast<int>(Coverage::FootprintTypes);
            int   levels = 5;               // 1..5: 40, 80, 160, 320, 640 yd
            float depth = 0.35f;            // yd of cover at full strength
            float rim = 0.3f;               // rim height as a share of the depth
            float relaxSeconds = 30.0f;     // trench back to flat
            int   stamp = 1;
            float stampScale = 1.0f;        // trench radius x the unit's size
            float farLift = 0.75f;          // yd added far away, against terrain drawn simpler than its heights
            float maxSlope = 45.0f;         // degrees: steeper ground holds no cover...
            float slopeBand = 15.0f;        // ...and it thins out over this many degrees below that
            float noise = 0.35f;            // depth variation (drifts), share of the depth
            float edgeBreakup = 0.5f;       // 0 = smooth rounded material edges, 1 = ragged, patchy ones
            int   wireframe = 0;
            int   drawPoint = 0;            // 0 right after the terrain (inside the world pass), 1 end of the scene
        };

        const WXL_Api* g_api = nullptr;
        Settings       g_settings;

        struct Level
        {
            float cell = kCell0;
            std::vector<float>   base = std::vector<float>(kTex * kTex, kHole);
            // Coverage parts per slot, combined at upload (so the slope/noise sliders need no refill):
            // material 0..255 (soft across terrain cells), the terrain's up-ness (normal z) 0..255,
            // and the drift noise 0..255.
            std::vector<uint8_t> material = std::vector<uint8_t>(kTex * kTex, 0);
            std::vector<uint8_t> upness = std::vector<uint8_t>(kTex * kTex, 255);
            std::vector<uint8_t> drift = std::vector<uint8_t>(kTex * kTex, 128);
            std::vector<uint8_t> edgeNoise = std::vector<uint8_t>(kTex * kTex, 128);
            int boostRows = 0;              // rows left to re-sample at full speed after a setting change
            bool  haveGrid = false;
            int   gridI = 0, gridJ = 0;     // world cell index (of this level) of the grid's first vertex
            int   refreshRow = 0;
            bool  baseDirty = true, coverDirty = true;
            unsigned holes = 0;
            double   fillMs = 0, firstFillMs = 0;
            IDirect3DTexture9* baseTex = nullptr;
            IDirect3DTexture9* coverTex = nullptr;
        };
        Level g_levels[kMaxLevels];

        // Level 0's trench state, slot = (world cell mod kTex).
        std::vector<float> g_press(kTex * kTex, 0.0f);
        std::vector<float> g_rimH(kTex * kTex, 0.0f);

        int   g_lastCoverage = -1;
        int   g_lastMap = -1;
        float g_lastDepth = -1.0f, g_lastMaxSlope = -1.0f, g_lastSlopeBand = -1.0f, g_lastNoise = -1.0f;
        float g_lastEdgeBreakup = -1.0f;
        constexpr float kEdgeWarpYards = 5.0f; // how far the material edge wanders at full breakup

        // Material per terrain cell (the client's lookup is constant over one: it's the cell's
        // dominant layer): 0 bare, 1 covered. Unloaded cells aren't cached, so they're asked again.
        constexpr float kTerrainCell = 33.3333333f / 8.0f;
        std::unordered_map<int64_t, uint8_t> g_cellMaterial;

        // stats
        double   g_simMs = 0, g_uploadMs = 0, g_drawMs = 0;
        unsigned g_stamps = 0;
        const char* g_inactive = "not started";

        // --- drawing in step with the world (verified in-client 2026-10-02) ----------------------
        // At the end of the scene the world pass has restored its own state, so a draw there has to
        // put back what the world was drawn with or its depth won't match the terrain's:
        //  - Projection: the graphics device keeps the matrix it was GIVEN at +0xF88 (what
        //    gfx::SceneMatrices returns) and the one it RENDERS with at +0xFC8 (its set-projection
        //    routine 0x6A9B40 recomputes the depth terms; the shader system copies +0xFC8, 0x872C10).
        //  - Depth range: the world render (0x4F8EA0) draws with viewport z = 0..[0xADEEE4], a static
        //    0.94 in .data (set at 0x4F905A), and the pass restores 0..1 before the event. Drawing with
        //    0..1 puts our depth ~6% further away, and the cover loses the depth test to the terrain.
        //    The neighbouring 0xADEEE8/EC pair is the sky's range, not the world's.
        constexpr size_t    kDeviceRenderProjection = 0xFC8;
        constexpr uintptr_t kWorldPassMaxZ = 0x00ADEEE4;

        // The scene render (0x79A870) draws its stages in a fixed order; the terrain stage 0x798DA0
        // (state setup, then the terrain draws at 0x799263/68/6D, inside its own render-state
        // push/pop) comes before the later stages that handle M2 models (0x793980) and WMO groups
        // (0x793D20). Drawing the cover right after the terrain stage puts it where the terrain
        // itself is, so what's drawn later -- transparent model parts above all -- depth-tests and
        // blends against it. The world's viewport, projection and depth buffer are still bound there.
        // __cdecl, no arguments; its only caller is 0x79AC30.
        constexpr uintptr_t kTerrainStage = 0x00798DA0;
        using TerrainStageFn = void(__cdecl*)();
        TerrainStageFn g_origTerrainStage = nullptr;

        // GPU state (all managed, so a device reset keeps it; a new device drops it).
        IDirect3DDevice9*            g_device = nullptr;
        IDirect3DVertexBuffer9*      g_vb = nullptr;
        IDirect3DIndexBuffer9*       g_ib = nullptr;
        IDirect3DVertexDeclaration9* g_decl = nullptr;
        IDirect3DVertexShader9*      g_vs = nullptr;
        IDirect3DPixelShader9*       g_ps = nullptr;
        bool                         g_gpuFailed = false;

        // VS: c0..c3 view-projection columns; c4 = (patch first cell i, j, cell size, 1 / texture
                // size); c5 = (level centre cell i, j, half extent, morph band) in cells; c6 = the camera
        // position (the scene is drawn about the camera); c7 = (fade start, fade end, fade on) in cells;
        // c8 = (far lift start, end, height) in yd from the camera.
        const char* kVsHlsl = R"(
float4 vp0 : register(c0); float4 vp1 : register(c1); float4 vp2 : register(c2); float4 vp3 : register(c3);
float4 grid : register(c4);
float4 lod : register(c5);
float4 eye : register(c6);
float4 fade : register(c7);
float4 lift : register(c8);
sampler2D baseTex : register(s0);
sampler2D coverTex : register(s1);

struct VOut { float4 pos : POSITION; float3 n : TEXCOORD0; float2 d : TEXCOORD1; float2 rel : TEXCOORD2; };

float Cheb(float2 idx) { float2 a = abs(idx - lod.xy); return max(a.x, a.y); }
float Fade(float2 idx) { return fade.z > 0.5 ? saturate((fade.y - Cheb(idx)) / (fade.y - fade.x)) : 1; }

// x = base height, y = cover depth
float2 Sample(float2 idx)
{
    float2 uv = (idx + 0.5) * grid.w;
    return float2(tex2Dlod(baseTex, float4(uv, 0, 0)).r, tex2Dlod(coverTex, float4(uv, 0, 0)).r * Fade(idx));
}

float H(float2 idx) { float2 s = Sample(idx); return s.x + s.y; }

// step = neighbour distance for the normal in cells (2 = the coarser level's spacing)
void Vertex(float2 idx, float step, out float3 p, out float3 n, out float2 s)
{
    s = Sample(idx);
    p = float3(idx * grid.z, s.x + s.y);
    n = float3(H(idx - float2(step, 0)) - H(idx + float2(step, 0)), H(idx - float2(0, step)) - H(idx + float2(0, step)), 2 * step * grid.z);
}

VOut main(float2 ij : POSITION)
{
    VOut o;
    float2 idx = grid.xy + ij;
    float3 p, n; float2 s;
    Vertex(idx, 1, p, n, s);

    // Border band: odd vertices slide onto their even neighbours, so the edge becomes the coarser
    // level's triangles.
    float m = lod.w > 1 ? saturate((Cheb(idx) - (lod.z - lod.w)) / (lod.w - 1)) : 0;
    [branch] if (m > 0)
    {
        float2 odd = frac(idx * 0.5) * 2;
        float3 p1, n1; float2 s1;
        Vertex(idx - odd, 2, p1, n1, s1);
        // Fully morphed takes the coarser vertex exactly: lerp(p, p1, 1) = p + (p1 - p) rounds off p1
        // at world-sized coordinates, and the two levels' edges no longer meet (hairline cracks).
        if (m >= 1) { p = p1; n = n1; s = s1; }
        else        { p = lerp(p, p1, m); n = lerp(normalize(n), normalize(n1), m); s = lerp(s, s1, m); }
    }

    // Far away the client draws simpler terrain than its heights, which pokes through a thin
    // cover; a lift that only depends on the distance stays seamless across the levels.
    float dist = length(float3(p.xy - eye.xy, p.z - eye.z));
    p.z += lift.z * saturate((dist - lift.x) / (lift.y - lift.x)) * saturate(s.y * 10);

    o.n = normalize(n);
    o.rel = p.xy - eye.xy;
    float4 q = float4(o.rel, p.z - eye.z, 1);
    o.pos = float4(dot(q, vp0), dot(q, vp1), dot(q, vp2), dot(q, vp3));
    o.d = float2(s.y, s.x < -10000 ? 1 : 0);
    return o;
}
)";

        // PS: c0 = (light direction, ambient), c1 = colour, c2.x = smallest drawn depth, c3 = the next
        // finer level's box (camera-relative min x, min y, max x, max y): this level isn't drawn there.
        const char* kPsHlsl = R"(
float4 light : register(c0);
float4 albedo : register(c1);
float4 opts : register(c2);
float4 inner : register(c3);

float4 main(float3 n : TEXCOORD0, float2 d : TEXCOORD1, float2 rel : TEXCOORD2) : COLOR
{
    clip(d.x - opts.x);
    clip(0.5 - d.y);
    float2 a = step(inner.xy, rel) * step(rel, inner.zw);
    clip(0.5 - a.x * a.y);
    float lit = light.w + (1 - light.w) * saturate(dot(normalize(n), light.xyz));
    return float4(albedo.rgb * lit, 1);
}
)";

        int Slot(int i) { const int m = i % kTex; return m < 0 ? m + kTex : m; }
        int SlotOf(int i, int j) { return Slot(j) * kTex + Slot(i); } // row = y (j), column = x (i)
        int FloorDiv2(int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

        int ActiveLevels()
        {
            const int n = g_settings.levels;
            return n < 1 ? 1 : (n > kMaxLevels ? kMaxLevels : n);
        }

        bool FootprintType(int id)
        {
            const int32_t minId = *reinterpret_cast<const int32_t*>(kTerrainMinId);
            const int32_t maxId = *reinterpret_cast<const int32_t*>(kTerrainMaxId);
            const auto* index = *reinterpret_cast<const uint8_t* const* const*>(kTerrainIndex);
            if (!index || id < minId || id > maxId) return false;
            const uint8_t* row = index[id - minId];
            return row && (*reinterpret_cast<const uint32_t*>(row + kTerrainFlags) & 1);
        }

        // Covered or bare at the centre of terrain cell (cx, cy); -1 while its chunk isn't loaded.
        int CellMaterial(int cx, int cy)
        {
            const int64_t key = (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
            const auto it = g_cellMaterial.find(key);
            if (it != g_cellMaterial.end()) return it->second;
            const float x = (cx + 0.5f) * kTerrainCell, y = (cy + 0.5f) * kTerrainCell;
            float z;
            if (!terrain::HeightAt(x, y, z)) return -1; // not loaded (or a hole): ask again later
            int type;
            const uint8_t covered = terrain::TerrainTypeAt(x, y, type) && FootprintType(type) ? 1 : 0;
            if (g_cellMaterial.size() > 400000) g_cellMaterial.clear(); // far beyond any grid's reach
            g_cellMaterial.emplace(key, covered);
            return covered;
        }

        // Soft material: blended between terrain cell centres, so the cover tapers out over about one
        // terrain cell instead of stopping at the cell's edge (which is also what made it blocky).
        float SoftMaterial(float x, float y)
        {
            const float u = x / kTerrainCell - 0.5f, v = y / kTerrainCell - 0.5f;
            const int cx = static_cast<int>(std::floor(u)), cy = static_cast<int>(std::floor(v));
            const float fx = u - cx, fy = v - cy;
            float m[4];
            const int c[4][2] = { { cx, cy }, { cx + 1, cy }, { cx, cy + 1 }, { cx + 1, cy + 1 } };
            for (int k = 0; k < 4; ++k)
            {
                const int r = CellMaterial(c[k][0], c[k][1]);
                m[k] = r < 0 ? 0.0f : static_cast<float>(r);
            }
            const float a = m[0] + (m[1] - m[0]) * fx, b = m[2] + (m[3] - m[2]) * fx;
            return a + (b - a) * fy;
        }

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
        // wherever their vertices coincide.
        void Sample(Level& L, bool isLevel0, int i, int j, bool fresh)
        {
            const int s = SlotOf(i, j);
            const float x = i * L.cell, y = j * L.cell;
            float z;
            const bool ok = terrain::HeightAt(x, y, z);
            const float base = ok ? z : kHole;
            if (base != L.base[s]) { L.base[s] = base; L.baseDirty = true; }

            uint8_t material = 0, up = 255;
            if (ok)
            {
                // The material edge wanders: the lookup point is pushed around by broad noise, so a
                // snowy terrain cell's border follows an irregular line instead of a rounded square.
                const float warp = kEdgeWarpYards * g_settings.edgeBreakup;
                const float mx = x + warp * (2.0f * ValueNoise(x / 7.0f, y / 7.0f, 1) - 1.0f);
                const float my = y + warp * (2.0f * ValueNoise(x / 7.0f, y / 7.0f, 2) - 1.0f);
                material = static_cast<Coverage>(g_settings.coverage) == Coverage::Everywhere
                         ? 255 : static_cast<uint8_t>(SoftMaterial(mx, my) * 255.0f + 0.5f);
                // Steepness 1 yd around (fixed, so every level gets the same value at the same spot).
                float zx, zy;
                const float gx = terrain::HeightAt(x + 1.0f, y, zx) ? zx - z : 0.0f;
                const float gy = terrain::HeightAt(x, y + 1.0f, zy) ? zy - z : 0.0f;
                up = static_cast<uint8_t>(255.0f / std::sqrt(1.0f + gx * gx + gy * gy) + 0.5f);
            }
            const uint8_t drift = static_cast<uint8_t>(Drift(x, y) * 255.0f + 0.5f);
            const uint8_t edge = static_cast<uint8_t>(EdgeNoise(x, y) * 255.0f + 0.5f);
            if (material != L.material[s] || up != L.upness[s] || drift != L.drift[s] || edge != L.edgeNoise[s])
            {
                L.material[s] = material; L.upness[s] = up; L.drift[s] = drift; L.edgeNoise[s] = edge;
                L.coverDirty = true;
            }
            if (isLevel0 && fresh) { g_press[s] = 0.0f; g_rimH[s] = 0.0f; }
        }

        void SampleRect(Level& L, bool isLevel0, int i0, int i1, int j0, int j1, bool fresh) // inclusive
        {
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) Sample(L, isLevel0, i, j, fresh);
        }

        // Moves a level's grid so the player is near its middle (origin on an even cell); fills only
        // the cells that came into it. The filled area is the grid plus an apron of kApron cells: the
        // normals of the border vertices read up to 2 cells beyond the grid (morph band normals at the
        // coarser spacing), and without the apron those slots hold stale heights or holes -- a bright
        // or dark line along every level border.
        void Recentre(Level& L, bool isLevel0, const float pos[3], bool refill)
        {
            const int pi = static_cast<int>(std::floor(pos[0] / L.cell)), pj = static_cast<int>(std::floor(pos[1] / L.cell));
            const int ni = FloorDiv2(pi - kHalfCells) * 2, nj = FloorDiv2(pj - kHalfCells) * 2;
            if (L.haveGrid && !refill && std::abs(ni - L.gridI) < kRecentreCells && std::abs(nj - L.gridJ) < kRecentreCells) return;

            const double t0 = grassperf::Now();
            const int span = kFilled - 1;
            const int fi = ni - kApron, fj = nj - kApron;  // new filled area, first cell
            if (!L.haveGrid || refill || std::abs(ni - L.gridI) > span || std::abs(nj - L.gridJ) > span)
            {
                SampleRect(L, isLevel0, fi, fi + span, fj, fj + span, true);
                L.firstFillMs = grassperf::Now() - t0;
                L.boostRows = kFilled; // chunks still streaming in get picked up quickly
            }
            else
            {
                const int oi0 = L.gridI - kApron, oi1 = oi0 + span, oj0 = L.gridJ - kApron, oj1 = oj0 + span;
                for (int j = fj; j <= fj + span; ++j)
                {
                    if (j < oj0 || j > oj1) { SampleRect(L, isLevel0, fi, fi + span, j, j, true); continue; }
                    if (fi < oi0) SampleRect(L, isLevel0, fi, oi0 - 1, j, j, true);
                    if (fi + span > oi1) SampleRect(L, isLevel0, oi1 + 1, fi + span, j, j, true);
                }
            }
            L.gridI = ni; L.gridJ = nj;
            L.haveGrid = true;
            L.coverDirty = true;
            L.fillMs = grassperf::Now() - t0;
        }

        // A trench under one grounded unit: pressed flat inside r, a rim between r and 1.6 r.
        void Stamp(const float pos[3], float radius)
        {
            const Level& L = g_levels[0];
            float ground;
            if (!terrain::HeightAt(pos[0], pos[1], ground) || std::fabs(pos[2] - ground) > 0.35f) return; // airborne, swimming, on a WMO
            ++g_stamps;
            const float outer = radius * 1.6f, rimPeak = radius * 1.25f, rimHalf = radius * 0.35f;
            const float rimH = g_settings.depth * g_settings.rim;
            const int i0 = static_cast<int>(std::floor((pos[0] - outer) / L.cell)), i1 = static_cast<int>(std::ceil((pos[0] + outer) / L.cell));
            const int j0 = static_cast<int>(std::floor((pos[1] - outer) / L.cell)), j1 = static_cast<int>(std::ceil((pos[1] + outer) / L.cell));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    if (i < L.gridI || i >= L.gridI + kGridVerts || j < L.gridJ || j >= L.gridJ + kGridVerts) continue;
                    const float dx = i * L.cell - pos[0], dy = j * L.cell - pos[1];
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d >= outer) continue;
                    const int s = SlotOf(i, j);
                    if (d < radius)
                    {
                        const float t = d < radius * 0.7f ? 1.0f : (radius - d) / (radius * 0.3f);
                        if (t > g_press[s]) g_press[s] = t;
                    }
                    const float r = 1.0f - std::fabs(d - rimPeak) / rimHalf;
                    if (r > 0.0f && rimH * r > g_rimH[s]) g_rimH[s] = rimH * r;
                }
        }

        void Simulate(float dt)
        {
            const float relax = g_settings.relaxSeconds > 0.1f ? dt / g_settings.relaxSeconds : 1.0f;
            const float rimRelax = relax * g_settings.depth * g_settings.rim;
            for (int s = 0; s < kTex * kTex; ++s)
            {
                if (g_press[s] > 0.0f) g_press[s] = g_press[s] > relax ? g_press[s] - relax : 0.0f;
                if (g_rimH[s] > 0.0f)  g_rimH[s]  = g_rimH[s] > rimRelax ? g_rimH[s] - rimRelax : 0.0f;
            }

            g_stamps = 0;
            if (!g_settings.stamp) return;
            for (const actors::Actor& a : actors::Nearby())
            {
                float r = (a.effectiveRadius > 0.0f ? a.effectiveRadius : 0.4f) * g_settings.stampScale;
                if (r < 0.3f) r = 0.3f;
                if (r > 2.5f) r = 2.5f;
                Stamp(a.pos, r);
            }
        }

        // --- GPU ---------------------------------------------------------------------------------
        void ReleaseGpu()
        {
            auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
            rel(g_vb); rel(g_ib); rel(g_decl); rel(g_vs); rel(g_ps);
            for (Level& L : g_levels) { rel(L.baseTex); rel(L.coverTex); L.baseDirty = L.coverDirty = true; }
        }

        void* Compile(IDirect3DDevice9* dev, const char* hlsl, const char* target, bool pixel)
        {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            if (!m) m = LoadLibraryA("d3dcompiler_47.dll");
            const auto compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
            if (!compile) return nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = compile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &err);
            if (err)
            {
                g_api->Log(WXL_LOG_WARN, kTag, "surface cover: %s compile: %s", target, static_cast<const char*>(err->GetBufferPointer()));
                err->Release();
            }
            if (FAILED(hr) || !code) return nullptr;
            void* shader = nullptr;
            const auto* bytes = static_cast<const DWORD*>(code->GetBufferPointer());
            if (pixel) { IDirect3DPixelShader9* ps = nullptr; if (SUCCEEDED(dev->CreatePixelShader(bytes, &ps))) shader = ps; }
            else       { IDirect3DVertexShader9* vs = nullptr; if (SUCCEEDED(dev->CreateVertexShader(bytes, &vs))) shader = vs; }
            code->Release();
            return shader;
        }

        bool VertexTexturesSupported(IDirect3DDevice9* dev)
        {
            IDirect3D9* d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DDISPLAYMODE mode{};
            if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
            const bool ok = SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode)) &&
                            SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                             D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_R32F));
            d3d->Release();
            return ok;
        }

        bool EnsureGpu(IDirect3DDevice9* dev)
        {
            if (dev != g_device) { ReleaseGpu(); g_gpuFailed = false; g_device = dev; }
            if (g_gpuFailed) return false;
            bool texturesReady = true;
            for (const Level& L : g_levels) texturesReady &= L.baseTex && L.coverTex;
            if (g_vb && g_ib && g_decl && g_vs && g_ps && texturesReady) return true;
            ReleaseGpu();

            auto fail = [](const char* why) { g_inactive = why; g_gpuFailed = true; g_api->Log(WXL_LOG_WARN, kTag, "surface cover: %s", why); return false; };
            if (!VertexTexturesSupported(dev)) return fail("this GPU/driver has no R32F vertex textures");

            const D3DVERTEXELEMENT9 elems[] = {
                { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                D3DDECL_END(),
            };
            if (FAILED(dev->CreateVertexDeclaration(elems, &g_decl))) return fail("vertex declaration failed");

            if (FAILED(dev->CreateVertexBuffer(kPatchVerts * kPatchVerts * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_vb, nullptr)))
                return fail("vertex buffer failed");
            float* v = nullptr;
            if (FAILED(g_vb->Lock(0, 0, reinterpret_cast<void**>(&v), 0))) return fail("vertex buffer lock failed");
            for (int j = 0; j < kPatchVerts; ++j)
                for (int i = 0; i < kPatchVerts; ++i) { *v++ = static_cast<float>(i); *v++ = static_cast<float>(j); }
            g_vb->Unlock();

            const UINT indices = kPatchCells * kPatchCells * 6;
            if (FAILED(dev->CreateIndexBuffer(indices * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib, nullptr)))
                return fail("index buffer failed");
            uint16_t* ix = nullptr;
            if (FAILED(g_ib->Lock(0, 0, reinterpret_cast<void**>(&ix), 0))) return fail("index buffer lock failed");
            for (int j = 0; j < kPatchCells; ++j)
                for (int i = 0; i < kPatchCells; ++i)
                {
                    const uint16_t a = static_cast<uint16_t>(j * kPatchVerts + i), b = a + 1;
                    const uint16_t c = static_cast<uint16_t>(a + kPatchVerts), d = c + 1;
                    *ix++ = a; *ix++ = b; *ix++ = c;
                    *ix++ = b; *ix++ = d; *ix++ = c;
                }
            g_ib->Unlock();

            for (Level& L : g_levels)
                if (FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &L.baseTex, nullptr)) ||
                    FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &L.coverTex, nullptr)))
                    return fail("R32F textures failed");

            g_vs = static_cast<IDirect3DVertexShader9*>(Compile(dev, kVsHlsl, "vs_3_0", false));
            g_ps = static_cast<IDirect3DPixelShader9*>(Compile(dev, kPsHlsl, "ps_3_0", true));
            if (!g_vs || !g_ps) return fail("shader compile failed (see the log)");
            return true;
        }

        bool UploadLevel(Level& L, bool isLevel0)
        {
            D3DLOCKED_RECT lr{};
            if (L.baseDirty)
            {
                if (FAILED(L.baseTex->LockRect(0, &lr, nullptr, 0))) return false;
                for (int row = 0; row < kTex; ++row)
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &L.base[row * kTex], kTex * 4);
                L.baseTex->UnlockRect(0);
                L.baseDirty = false;
            }
            // Level 0 changes every frame (trenches); the others only when their mask or the depth does.
            if (!isLevel0 && !L.coverDirty) return true;
            if (FAILED(L.coverTex->LockRect(0, &lr, nullptr, 0))) return false;
            const float depth = g_settings.depth;

            // Lookup tables for this upload: steepness -> factor, drift -> factor.
            float slopeF[256], driftF[256];
            const float rad = 3.14159265f / 180.0f;
            const float cosMax = std::cos(g_settings.maxSlope * rad);
            const float cosFull = std::cos((g_settings.maxSlope - g_settings.slopeBand) * rad);
            for (int k = 0; k < 256; ++k)
            {
                const float up = k / 255.0f;
                float t = cosFull > cosMax ? (up - cosMax) / (cosFull - cosMax) : (up >= cosMax ? 1.0f : 0.0f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                slopeF[k] = t * t * (3.0f - 2.0f * t);
                const float d = 1.0f + g_settings.noise * (k / 127.5f - 1.0f);
                driftF[k] = d < 0.0f ? 0.0f : d;
            }
            // Material shaping: steeper with more breakup, and the fine edge noise moves the threshold,
            // so the transition breaks into patches. Fully covered (1) and bare (0) stay as they are.
            const float b = g_settings.edgeBreakup;
            const float contrast = 1.0f + 2.0f * b, jitter = 1.5f * b;
            const int slotI0 = Slot(L.gridI), slotJ0 = Slot(L.gridJ);
            for (int row = 0; row < kTex; ++row)
            {
                float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                // Where this row sits in the grid (Chebyshev distance from the centre feeds the trench fade).
                const int dj = (row - slotJ0 + kTex) % kTex;
                const int cj = std::abs(dj - kHalfCells);
                for (int col = 0; col < kTex; ++col)
                {
                    const int s = row * kTex + col;
                    float m = (L.material[s] / 255.0f - 0.5f) * contrast + 0.5f + jitter * (L.edgeNoise[s] / 255.0f - 0.5f);
                    m = m <= 0.0f ? 0.0f : (m >= 1.0f ? 1.0f : m * m * (3.0f - 2.0f * m));
                    const float cover = depth * m * slopeF[L.upness[s]] * driftF[L.drift[s]];
                    if (cover <= 0.0f) { out[col] = 0.0f; continue; }
                    if (!isLevel0) { out[col] = cover; continue; }
                    const int di = (col - slotI0 + kTex) % kTex;
                    const int ci = std::abs(di - kHalfCells);
                    const int cheb = ci > cj ? ci : cj;
                    // Trenches fade out before the morph band, so the border matches the static coarser level.
                    float k = cheb <= kDeformFullCells ? 1.0f
                            : (cheb >= kDeformZeroCells ? 0.0f : float(kDeformZeroCells - cheb) / float(kDeformZeroCells - kDeformFullCells));
                    if (dj >= kGridVerts || di >= kGridVerts) k = 0.0f;
                    const float deformed = (cover + g_rimH[s]) * (1.0f - g_press[s]);
                    out[col] = cover + (deformed - cover) * k;
                }
            }
            L.coverTex->UnlockRect(0);
            L.coverDirty = false;
            return true;
        }

        // inPass: called inside the world pass, where the world's viewport and depth surface are still
        // bound. Otherwise (end of scene) both have to be put back first.
        void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth, bool inPass)
        {
            if (!dev || !EnsureGpu(dev)) return;
            const int levels = ActiveLevels();
            for (int k = 0; k < levels; ++k) if (!g_levels[k].haveGrid) return;

            // The world's view (no translation: the scene is drawn about the camera, so positions go
            // in camera-relative, c6) and its rendered projection.
            float V[16], P[16];
            const void* graphics = gx::RawGraphicsDevice();
            if (!graphics || !wxl::game::gfx::SceneMatrices(V, P)) { g_inactive = "no scene matrices"; return; }
            std::memcpy(P, static_cast<const uint8_t*>(graphics) + kDeviceRenderProjection, sizeof(P));

            double t0 = grassperf::Now();
            for (int k = 0; k < levels; ++k)
                if (!UploadLevel(g_levels[k], k == 0)) { g_inactive = "texture upload failed"; return; }
            g_uploadMs = grassperf::Now() - t0;
            t0 = grassperf::Now();

            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
            // Depth-test against the surface the world was drawn into; at the end of the scene whatever
            // is bound may not be it.
            IDirect3DSurface9* oldDepth = nullptr;
            dev->GetDepthStencilSurface(&oldDepth);
            const bool swapDepth = !inPass && sceneDepth && sceneDepth != oldDepth;
            if (swapDepth) dev->SetDepthStencilSurface(sceneDepth);

            if (!inPass)
            {
                D3DVIEWPORT9 vp{};
                dev->GetViewport(&vp);
                vp.MinZ = 0.0f;
                vp.MaxZ = *reinterpret_cast<const float*>(kWorldPassMaxZ);
                dev->SetViewport(&vp);
            }

            // View-projection (row-vector convention), uploaded as columns for dot products.
            float vpm[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    for (int j = 0; j < 4; ++j) sum += V[r * 4 + j] * P[j * 4 + c];
                    vpm[r * 4 + c] = sum;
                }
            float cols[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) cols[c * 4 + r] = vpm[r * 4 + c];
            dev->SetVertexShaderConstantF(0, cols, 4);

            float eye[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            wxl::game::camera::GetPosition(eye);
            dev->SetVertexShaderConstantF(6, eye, 1);
            const float c8[4] = { 80.0f, 400.0f, g_settings.farLift, 0.0f };
            dev->SetVertexShaderConstantF(8, c8, 1);

            const float lx = 0.35f, ly = 0.45f, lz = 0.82f, inv = 1.0f / std::sqrt(lx * lx + ly * ly + lz * lz);
            const float ps[12] = {
                lx * inv, ly * inv, lz * inv, 0.45f, // light, ambient
                0.93f, 0.95f, 1.0f, 1.0f,             // snow-ish white
                0.004f, 0.0f, 0.0f, 0.0f,             // smallest depth drawn (no z-fighting with the terrain)
            };
            dev->SetPixelShaderConstantF(0, ps, 3);

            dev->SetVertexDeclaration(g_decl);
            dev->SetStreamSource(0, g_vb, 0, 8);
            dev->SetIndices(g_ib);
            dev->SetVertexShader(g_vs);
            dev->SetPixelShader(g_ps);
            for (int s = 0; s < 2; ++s)
            {
                const DWORD sampler = D3DVERTEXTEXTURESAMPLER0 + s;
                dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }

            dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
            dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
            dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x07);
            dev->SetRenderState(D3DRS_FILLMODE, g_settings.wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

            for (int k = 0; k < levels; ++k)
            {
                const Level& L = g_levels[k];
                const bool outermost = k == levels - 1;
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, L.baseTex);
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER1, L.coverTex);

                const float c5[4] = { static_cast<float>(L.gridI + kHalfCells), static_cast<float>(L.gridJ + kHalfCells),
                                      static_cast<float>(kHalfCells), outermost ? 0.0f : static_cast<float>(kMorphCells) };
                dev->SetVertexShaderConstantF(5, c5, 1);
                const float c7[4] = { kHalfCells * kOuterFadeStart, kHalfCells * kOuterFadeEnd, outermost ? 1.0f : 0.0f, 0.0f };
                dev->SetVertexShaderConstantF(7, c7, 1);

                // Not drawn inside the next finer level's grid (camera-relative box); level 0 has none.
                // The box is a hair smaller than that grid, so the two levels overlap slightly where
                // they meet (same heights there) instead of both skipping a pixel exactly on the edge.
                float inner[4] = { 1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f };
                if (k > 0)
                {
                    const Level& F = g_levels[k - 1];
                    const double e = F.cell * 0.25;
                    inner[0] = static_cast<float>(static_cast<double>(F.gridI) * F.cell + e - eye[0]);
                    inner[1] = static_cast<float>(static_cast<double>(F.gridJ) * F.cell + e - eye[1]);
                    inner[2] = static_cast<float>(static_cast<double>(F.gridI + kGridCells) * F.cell - e - eye[0]);
                    inner[3] = static_cast<float>(static_cast<double>(F.gridJ + kGridCells) * F.cell - e - eye[1]);
                }
                dev->SetPixelShaderConstantF(3, inner, 1);

                for (int pj = 0; pj < kPatches; ++pj)
                    for (int pi = 0; pi < kPatches; ++pi)
                    {
                        const float c4[4] = { static_cast<float>(L.gridI + pi * kPatchCells), static_cast<float>(L.gridJ + pj * kPatchCells),
                                              L.cell, 1.0f / kTex };
                        dev->SetVertexShaderConstantF(4, c4, 1);
                        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kPatchVerts * kPatchVerts, 0, kPatchCells * kPatchCells * 2);
                    }
            }

            dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
            dev->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
            if (swapDepth) dev->SetDepthStencilSurface(oldDepth);
            if (oldDepth) oldDepth->Release();
            saved->Apply(); // includes the viewport
            saved->Release();
            g_drawMs = grassperf::Now() - t0;
            g_inactive = nullptr;
        }

        void __cdecl hkTerrainStage()
        {
            g_origTerrainStage();
            if (g_settings.enabled && g_settings.drawPoint == 0)
                Draw(static_cast<IDirect3DDevice9*>(gx::RawDevice()), nullptr, true);
        }

        void __cdecl OnWorldSceneEnd(void* /*user*/, const void* args)
        {
            if (!g_settings.enabled || g_settings.drawPoint != 1) return;
            const auto* a = static_cast<const ev::WorldSceneEndArgs*>(args);
            auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
            Draw(dev, a ? static_cast<IDirect3DSurface9*>(a->sceneDepth) : nullptr, false);
        }

        bool CopyToClipboard(const std::string& text)
        {
            if (!OpenClipboard(nullptr)) return false;
            EmptyClipboard();
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            bool ok = false;
            if (mem)
            {
                if (void* p = GlobalLock(mem))
                {
                    std::memcpy(p, text.c_str(), text.size() + 1);
                    GlobalUnlock(mem);
                    ok = SetClipboardData(CF_TEXT, mem) != nullptr;
                }
                if (!ok) GlobalFree(mem); // on success the clipboard owns it
            }
            CloseClipboard();
            return ok;
        }

        void __cdecl Panel(void* /*user*/)
        {
            char line[256];
            g_api->UiCheckbox("Draw the cover", &g_settings.enabled);
            static const char* const coverage[] = { "Everywhere (test)", "Only TerrainTypes with footprints (Snow, Sand)" };
            g_api->UiCombo("Where", &g_settings.coverage, coverage, 2);
            g_api->UiSliderInt("Levels (40 / 80 / 160 / 320 / 640 yd)", &g_settings.levels, 1, kMaxLevels);
            g_api->UiSliderFloat("Depth (yd)", &g_settings.depth, 0.0f, 1.5f);
            g_api->UiSliderFloat("Rim (x depth)", &g_settings.rim, 0.0f, 1.0f);
            g_api->UiSliderFloat("Max slope (deg)", &g_settings.maxSlope, 10.0f, 90.0f);
            g_api->UiSliderFloat("Slope fade (deg)", &g_settings.slopeBand, 0.0f, 40.0f);
            g_api->UiSliderFloat("Drift noise (x depth)", &g_settings.noise, 0.0f, 1.0f);
            g_api->UiSliderFloat("Edge breakup", &g_settings.edgeBreakup, 0.0f, 1.0f);
            g_api->UiCheckbox("Units carve trenches", &g_settings.stamp);
            g_api->UiSliderFloat("Trench width (x unit size)", &g_settings.stampScale, 0.3f, 3.0f);
            g_api->UiSliderFloat("Relax time (s)", &g_settings.relaxSeconds, 1.0f, 300.0f);
            g_api->UiSliderFloat("Far lift (yd, 80 -> 400 yd away)", &g_settings.farLift, 0.0f, 3.0f);
            g_api->UiCheckbox("Wireframe", &g_settings.wireframe);
            static const char* const drawPoints[] = { "Right after the terrain (inside the world pass)", "End of the scene (old)" };
            g_api->UiCombo("Draw point", &g_settings.drawPoint, drawPoints, 2);

            g_api->UiSeparator();
            std::vector<std::string> lines;
            auto add = [&lines, &line]() { lines.emplace_back(line); };
            if (const char* why = g_settings.enabled ? g_inactive : "switched off") { std::snprintf(line, sizeof(line), "not drawing: %s", why); add(); }
            const int levels = ActiveLevels();
            for (int k = 0; k < levels; ++k)
            {
                const Level& L = g_levels[k];
                std::snprintf(line, sizeof(line), "level %d: %.2f yd cells, %.0f yd out, first cell (%d, %d), holes/unloaded %u, full fill %.2f ms, last recentre %.2f ms",
                              k, L.cell, kHalfCells * L.cell, L.gridI, L.gridJ, L.holes, L.firstFillMs, L.fillMs); add();
            }
            std::snprintf(line, sizeof(line), "%d x %d vertices; units stamping %u; terrain cells cached %zu",
                          levels * kPatches * kPatches, kPatchVerts * kPatchVerts, g_stamps, g_cellMaterial.size()); add();
            std::snprintf(line, sizeof(line), "CPU: sim %.2f ms, upload %.2f ms, draw submit %.2f ms", g_simMs, g_uploadMs, g_drawMs); add();

            static char status[64] = "";
            if (g_api->UiButton("Copy status to clipboard"))
            {
                std::string text;
                for (const std::string& l : lines) text += l + "\r\n";
                std::snprintf(status, sizeof(status), CopyToClipboard(text) ? "copied" : "clipboard unavailable");
            }
            if (status[0]) { g_api->UiSameLine(); g_api->UiText(status); }
            for (const std::string& l : lines) g_api->UiTextWrapped(l.c_str());
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        for (int k = 0; k < kMaxLevels; ++k) g_levels[k].cell = kCell0 * static_cast<float>(1 << k);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
        if (!api->HookAttach("LivingAzeroth.TerrainStage", kTerrainStage, reinterpret_cast<void*>(&hkTerrainStage),
                             reinterpret_cast<void**>(&g_origTerrainStage), WXL_HOOK_DEFAULT_PRIORITY))
        {
            api->Log(WXL_LOG_WARN, kTag, "surface cover: terrain stage hook failed; drawing at the end of the scene instead.");
            g_settings.drawPoint = 1;
        }
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        if (!g_settings.enabled || !snap.inWorld) return;
        if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
        const double t0 = grassperf::Now();

        const bool refill = g_lastCoverage != g_settings.coverage || g_lastMap != snap.mapId;
        if (refill) g_cellMaterial.clear();
        g_lastCoverage = g_settings.coverage;
        g_lastMap = snap.mapId;
        if (g_lastDepth != g_settings.depth || g_lastMaxSlope != g_settings.maxSlope ||
            g_lastSlopeBand != g_settings.slopeBand || g_lastNoise != g_settings.noise)
        {
            g_lastDepth = g_settings.depth; g_lastMaxSlope = g_settings.maxSlope;
            g_lastSlopeBand = g_settings.slopeBand; g_lastNoise = g_settings.noise;
            for (Level& L : g_levels) L.coverDirty = true;
        }
        if (g_lastEdgeBreakup != g_settings.edgeBreakup)
        {
            g_lastEdgeBreakup = g_settings.edgeBreakup;
            for (Level& L : g_levels) { L.coverDirty = true; L.boostRows = kFilled; } // the warp is sampled
        }

        const int levels = ActiveLevels();
        for (int k = 0; k < levels; ++k)
        {
            Level& L = g_levels[k];
            Recentre(L, k == 0, snap.playerPos, refill);
            // Chunks stream in after the grid saw them: re-sample a few rows per frame, keeping trenches.
            // Once nothing is missing one row is enough, except for a pass at full speed after a
            // setting that's applied while sampling changed.
            const int fast = k == 0 ? 8 : 4;
            const int rows = (L.holes || L.boostRows > 0) ? fast : 1;
            if (L.boostRows > 0) L.boostRows -= rows;
            for (int r = 0; r < rows; ++r)
            {
                const int row = L.gridJ - kApron + L.refreshRow;
                SampleRect(L, k == 0, L.gridI - kApron, L.gridI - kApron + kFilled - 1, row, row, false);
                L.refreshRow = (L.refreshRow + 1) % kFilled;
            }
            if (L.refreshRow < rows) // once per pass: count what's still missing
            {
                unsigned holes = 0;
                for (int j = 0; j < kGridVerts; ++j)
                    for (int i = 0; i < kGridVerts; ++i) holes += L.base[SlotOf(L.gridI + i, L.gridJ + j)] == kHole;
                L.holes = holes;
            }
        }
        // A level switched off is refilled from scratch when it comes back.
        for (int k = levels; k < kMaxLevels; ++k) g_levels[k].haveGrid = false;

        Simulate(dt);
        g_simMs = grassperf::Now() - t0;
    }
}
