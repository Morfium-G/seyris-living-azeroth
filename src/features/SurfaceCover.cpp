#include "SurfaceCover.hpp"

#include "../env/Actors.hpp"
#include "../env/Climate.hpp"
#include "../env/Fields.hpp"
#include "../env/Lights.hpp"
#include "../env/Regional.hpp"
#include "../env/TerrainHeight.hpp"
#include "../render/BlpTexture.hpp"
#include "../render/MoistureTexture.hpp"
#include "ModelLights.hpp"
#include "TerrainLights.hpp"
#include "TerrainWetness.hpp"
#include "GrassPerf.hpp"
#include "GroundMaterialTable.hpp"
#include "SurfaceCoverTable.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <array>
#include <climits>
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

        // "Everywhere" (test): no painted cover, but fallen snow may lie anywhere (with the plain look);
        // the panel's deposit button puts some down.
        enum class Coverage : int { Everywhere = 0, Table = 1 };

        struct Settings
        {
            int   enabled = 1;
            int   coverage = static_cast<int>(Coverage::Table);
            int   levels = 5;               // 1..5: 40, 80, 160, 320, 640 yd
            // Player/testing multipliers on what SurfaceCover.cdbc says; 1 = exactly the table.
            float depthMul = 1.0f, driftMul = 1.0f, breakupMul = 1.0f, rimMul = 1.0f, relaxMul = 1.0f;
            int   stamp = 1;
            float stampScale = 1.0f;        // trench radius x the unit's size
            float farLift = 0.75f;          // yd added far away, against terrain drawn simpler than its heights
            int   sceneLight = 1;           // light and fog the cover like the terrain just drawn (in-pass only)
            int   shadows = 1;              // the terrain's dynamic shadows on the cover (in-pass, lit like the scene)
            float brightness = 0.93f;       // cover albedo
            float fillBudgetMs = 2.0f;      // sampling work per frame for fills and recentres
            int   materialSource = 0;       // 0 the painted strength of every layer (alpha maps), 1 the dominant layer per cell
            int   alphaSwap = 0;            // debug: read the alpha maps with rows and columns swapped
            int   wireframe = 0;
            int   drawPoint = 0;            // 0 right after the terrain (inside the world pass), 1 end of the scene
        };

        const WXL_Api* g_api = nullptr;
        Settings       g_settings;

        struct Level
        {
            float cell = kCell0;
            std::vector<float>   base = std::vector<float>(kTex * kTex, kHole);
            // Coverage parts per slot, from the table rows of the terrain cells around (blended),
            // combined at upload (so the panel's multipliers need no refill): how covered (0..255,
            // soft across terrain cells), the table's depth (yd), the slope factor (0..255, from the
            // terrain's steepness and the rows' MaxSlope/SlopeFade), the rows' drift and breakup
            // amounts (0..255 = 0..1), the drift and edge noise (0..255), and the tint (ARGB, A =
            // strength).
            std::vector<uint8_t>  material = std::vector<uint8_t>(kTex * kTex, 0);
            std::vector<float>    depth = std::vector<float>(kTex * kTex, 0.0f);
            std::vector<uint8_t>  slope = std::vector<uint8_t>(kTex * kTex, 255);
            std::vector<uint8_t>  driftAmp = std::vector<uint8_t>(kTex * kTex, 0);
            std::vector<uint8_t>  breakup = std::vector<uint8_t>(kTex * kTex, 0);
            std::vector<uint8_t>  stiff = std::vector<uint8_t>(kTex * kTex, 0); // cover material's stiffness (level 0's trenches)
            // Fallen snow (regional-layer-and-snow.md): how much of the spot may hold it (0..255, the
            // share of bare and snow-covered layers, none on or beside other covers, none under
            // liquids), and the share of the painted depth that is snow (it melts by the near grid's
            // kept share). Both static; the fallen depth and kept share are folded in at upload.
            std::vector<uint8_t>  snowShare = std::vector<uint8_t>(kTex * kTex, 0);
            std::vector<uint8_t>  paintedSnow = std::vector<uint8_t>(kTex * kTex, 0);
            uint32_t snowVersion = 0;       // the snow values last folded in (fields::SnowVersion)
            bool     snowFolded = false;    // whether that fold had any snow in it
            double   foldMs = 0;            // the last full depth pass (upload)
            std::vector<uint8_t>  drift = std::vector<uint8_t>(kTex * kTex, 128);
            std::vector<uint8_t>  edgeNoise = std::vector<uint8_t>(kTex * kTex, 128);
            std::vector<uint32_t> tint = std::vector<uint32_t>(kTex * kTex, 0);
            // The cell's two strongest cover textures, by table ID (lo/hi 16 bits) and share (lo/hi
            // byte). Turned into per-slot weights at upload, with whatever IDs hold the slots then.
            std::vector<uint32_t> texIds = std::vector<uint32_t>(kTex * kTex, 0);
            std::vector<uint16_t> texShares = std::vector<uint16_t>(kTex * kTex, 0);
            std::vector<uint32_t> texWeights[2] = { std::vector<uint32_t>(kTex * kTex, 0), std::vector<uint32_t>(kTex * kTex, 0) };
            // A = wetness, RGB = the terrain's vertex colour where rows use it (0x80 = neutral).
            std::vector<uint32_t> props = std::vector<uint32_t>(kTex * kTex, 0x00808080u);
            uint32_t slotsUploaded = 0;     // the slot generation the weight textures were built with
            bool lookDirty = true;
            int boostRows = 0;              // rows left to re-sample at full speed after a setting change
            bool  haveGrid = false;         // drawn: its grid at gridI/gridJ is filled
            // A fill in progress, worked through within the per-frame budget. The level keeps drawing
            // its current grid meanwhile (unless `hidden`: a new place, nothing valid to show), and
            // switches to the job's origin when it's done. The texture has 59 spare rows/columns, so
            // the strips being filled never overwrite cells still on screen.
            struct Segment { int i0, i1, j; };
            std::vector<Segment> job;
            size_t jobPos = 0;
            int    jobI = 0, jobJ = 0;
            bool   jobActive = false, jobHidden = false, jobFull = false;
            double jobMs = 0;
            int   gridI = 0, gridJ = 0;     // world cell index (of this level) of the grid's first vertex
            int   refreshRow = 0;
            bool  baseDirty = true, coverDirty = true;
            unsigned holes = 0;
            double   fillMs = 0, firstFillMs = 0;
            IDirect3DTexture9* baseTex = nullptr;
            IDirect3DTexture9* coverTex = nullptr;
            IDirect3DTexture9* lookTex = nullptr;  // A8R8G8B8 tint per slot (when the GPU can fetch it in the VS)
            IDirect3DTexture9* texWTex[2] = {};    // A8R8G8B8 cover texture weights (pixel shader, always available)
            IDirect3DTexture9* propsTex = nullptr; // A8R8G8B8 props (pixel shader)
        };
        Level g_levels[kMaxLevels];

        // Level 0's trench state, slot = (world cell mod kTex): how far pressed down (0..1) and the
        // rim's strength (0..1; its height is a share of the local cover depth). Both relax over time.
        std::vector<float> g_press(kTex * kTex, 0.0f);
        std::vector<float> g_rim(kTex * kTex, 0.0f);

        // Only cells with a trench change from frame to frame. They're kept in a list (relaxed, and
        // re-uploaded with a dirty rectangle) instead of walking all 147k cells twice per frame. The
        // rest of level 0 keeps its static cover and the trench fade-out factor from the last full
        // pass (a full pass runs whenever coverDirty is set: recentre, refill, settings).
        std::vector<int>     g_active;                            // slots with press or rim > 0
        std::vector<uint8_t> g_isActive(kTex * kTex, 0);
        std::vector<int>     g_changed;                           // slots to re-upload this frame
        bool                 g_changedPending = false;            // changes not uploaded yet
        std::vector<float>   g_static0(kTex * kTex, 0.0f);        // level 0 cover without trenches
        std::vector<float>   g_deformK(kTex * kTex, 0.0f);        // level 0 trench strength (fade-out)
        unsigned             g_frame = 0;
        int                  g_snowCursor = 0;                    // the level the next snow re-fold starts looking at
        float                g_depositYards = 0.2f;               // the panel's snow deposit

        void MarkActive(int s)
        {
            if (g_isActive[s]) return;
            g_isActive[s] = 1;
            g_active.push_back(s);
            g_changed.push_back(s);
        }

        // Level 0's rim share and relax time per slot, from the rows of the cell's materials (blended
        // like everything else; the panel's multipliers apply on top). Per cell, not per player: a
        // cover whose prints only show through their rims (mud below the terrain) must keep its rims
        // when the player steps onto other ground.
        std::vector<float> g_rimCell(kTex * kTex, 0.3f);
        std::vector<float> g_relaxCell(kTex * kTex, 30.0f);

        // What the table sees where the player is (for the panel, so rows can be authored on the spot).
        terrain::Surface   g_hereSurface;
        covertable::Values g_hereValues;
        std::string        g_hereTexture;
        bool               g_hereValid = false;
        float              g_hereMccv[3] = {}, g_hereLiquid = 0.0f, g_hereLiquidDepth = 0.0f;
        bool               g_hereMccvValid = false, g_hereLiquidValid = false;
        bool               g_tintSupported = false;

        // Point lights per patch: each light's 4 registers (camera-relative view space, built once a
        // frame), then per patch the ones that reach it.
        std::vector<float> g_lampRegs;
        unsigned           g_patchLightsMost = 0, g_patchLightsFull = 0; // this frame: the busiest patch; patches over kMaxLights

        // The lights whose radius reaches the patch (world box x0, y0, x1, y1), minus those wholly
        // inside the finer level's box (that ground is drawn by the finer level). Over kMaxLights, the
        // nearest to the camera win (the patch's ground closest to the camera matters most).
        void PatchLights(IDirect3DDevice9* dev, const float patch[4], const float inner[4], const float eye[3])
        {
            static std::vector<std::pair<float, unsigned>> hits;
            hits.clear();
            const std::vector<lights::ActiveLight>& all = lights::Active();
            for (unsigned n = 0; n < all.size(); ++n)
            {
                const lights::ActiveLight& l = all[n];
                if (l.indoor) continue; // the cover lies on the terrain: indoor lights stay off it too
                const float r = l.radius, x = l.pos[0], y = l.pos[1];
                const float nx = std::max(patch[0], std::min(x, patch[2])) - x, ny = std::max(patch[1], std::min(y, patch[3])) - y;
                if (nx * nx + ny * ny > r * r) continue;
                if (x - r >= inner[0] && x + r <= inner[2] && y - r >= inner[1] && y + r <= inner[3]) continue;
                const float dx = x - eye[0], dy = y - eye[1], dz = l.pos[2] - eye[2];
                hits.push_back({ dx * dx + dy * dy + dz * dz, n });
            }
            g_patchLightsMost = std::max(g_patchLightsMost, static_cast<unsigned>(hits.size()));
            if (hits.size() > static_cast<size_t>(lights::kMaxLights))
            {
                ++g_patchLightsFull;
                std::nth_element(hits.begin(), hits.begin() + lights::kMaxLights, hits.end());
                hits.resize(lights::kMaxLights);
            }
            float pl[lights::kMaxLights * 16];
            int count = 0;
            for (const auto& h : hits) std::memcpy(pl + 16 * count++, g_lampRegs.data() + 16 * h.second, 16 * sizeof(float));
            const float c17[4] = { static_cast<float>(count), 0.0f, 0.0f, 0.0f };
            dev->SetVertexShaderConstantF(17, c17, 1);
            if (count) dev->SetVertexShaderConstantF(20, pl, count * 4);
        }

        // CoverTexture slots from the table, loaded from the client's archives into textures of
        // our own; reloaded when the table is.
        IDirect3DTexture9* g_coverTextures[covertable::kMaxCoverTextures] = {}; // per slot (owned by the cache)
        bool               g_slotSpecular[covertable::kMaxCoverTextures] = {};    // per slot: its alpha is a specular mask
        int                g_slotId[covertable::kMaxCoverTextures] = {};          // the ID each slot holds (0 = free)
        std::vector<int>   g_slotOfId;                                            // ID -> slot, -1 = none
        uint32_t           g_slotGeneration = 1;
        int                g_slotFrame = 0;
        unsigned           g_texturesInView = 0;
        std::unordered_map<int, IDirect3DTexture9*> g_textureCache;
        std::unordered_map<int, std::string>       g_textureStatus;
        uint32_t           g_coverTexturesGeneration = 0;
        constexpr float    kCoverTextureTile = 33.3333333f / 8.0f; // one repeat per terrain cell, like the terrain's layers
        const void* g_cdbcApi = nullptr;
        uint32_t    g_tableGeneration = 0;

        int   g_lastCoverage = -1, g_lastMaterialSource = -1, g_lastAlphaSwap = -1;
        int   g_lastMap = -1;
        float g_lastDepthMul = -1.0f, g_lastDriftMul = -1.0f, g_lastBreakupMul = -1.0f;
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

        // Under the player, for the panel: the layer weights both ways round and the client's own
        // dominant layer (the check that the alpha maps are read the right way round).
        terrain::LayerWeights g_hereLayers, g_hereLayersSwapped;
        std::string g_hereLayerTexture[4]; // own copies for the panel (the cache can drop the originals)
        bool g_hereLayersValid = false;
        bool g_wasInWorld = false;
        int g_mapNow = -1;

        // The terrain's lighting and fog as last read (for the panel): VS c12 fog, c24 sun direction
        // (view space), c25 ambient, c26 diffuse, PS c2 fog colour.
        float g_seenFog[4] = {}, g_seenLight[16] = {}, g_seenFogColor[4] = {}; // light: c24..c27 (c27 = specular colour, exponent)
        // The terrain's dynamic shadows as found at our draw point: which shadow maps are still bound
        // on its samplers s5..s8, and whether they were used (all four bound and switched on).
        bool  g_shadowBound[4] = {};
        bool  g_shadowUsed = false;
        bool  g_seenScene = false;

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
        // c8 = (far lift start, end, height) in yd from the camera; c9..c11 = the view rotation rows
        // (identity when not lit like the scene); c12 = the terrain's fog parameters; c13.x = tint on.
        const char* kVsHlsl = R"(
float4 vp0 : register(c0); float4 vp1 : register(c1); float4 vp2 : register(c2); float4 vp3 : register(c3);
float4 grid : register(c4);
float4 lod : register(c5);
float4 eye : register(c6);
float4 fade : register(c7);
float4 lift : register(c8);
float4 vr0 : register(c9); float4 vr1 : register(c10); float4 vr2 : register(c11);
float4 fogp : register(c12);
float4 look : register(c13);
float4 farMap : register(c14);   // 1 / chunk size, map size, 1 / map size, on
float4 farBox : register(c15);   // the far map's box, world yd (min x, min y, max x, max y)
float4 farMisc : register(c16);  // x = the value outside that box
float4 pointInfo : register(c17);   // x = point lights in use (env/Lights)
float4 points[96] : register(c20);  // per light: (view-space position, 1 / radius), (colour, falloff shape),
                                    // (view-space spot direction, cos outer angle), (1 / (cos inner - cos outer))
sampler2D baseTex : register(s0);
sampler2D coverTex : register(s1);
sampler2D lookTex : register(s2);
sampler2D farTex : register(s3);  // ground moisture beyond the near grid, per terrain chunk (R32F)

// The far map at a world position, filtered here (vertex textures aren't).
float FarAt(float2 w)
{
    float2 t = w * farMap.x - 0.5;
    float2 f = frac(t);
    float4 uv = float4((t - f + 0.5) * farMap.z, 0, 0);
    float a = tex2Dlod(farTex, uv).r;
    float b = tex2Dlod(farTex, uv + float4(farMap.z, 0, 0, 0)).r;
    float c = tex2Dlod(farTex, uv + float4(0, farMap.z, 0, 0)).r;
    float d = tex2Dlod(farTex, uv + float4(farMap.z, farMap.z, 0, 0)).r;
    float v = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    float inside = (w.x >= farBox.x && w.y >= farBox.y && w.x <= farBox.z && w.y <= farBox.w) ? farMap.w : 0;
    return lerp(farMisc.x, v, inside);
}

struct VOut { float4 pos : POSITION; float3 n : TEXCOORD0; float2 d : TEXCOORD1; float2 rel : TEXCOORD2; float fog : TEXCOORD3; float4 tint : TEXCOORD4; float2 wuv : TEXCOORD5; float3 vpos : TEXCOORD6; float far : TEXCOORD7; float3 lamps : TEXCOORD8; };

// Point lights (torches; env/Lights) at a view-space position with a view-space unit normal:
// colour x a falloff like the client's 1 / (0.7 d + 0.03 d^2), windowed to exactly 0 at the
// light's radius. The light has a size (~1 yd, like a flame): the falloff uses sqrt(d^2 + 1), so
// right under it it tops out at the client's value at 1 yd instead of a hot spot with a hard edge
// (a torch flame ends up just above deep snow). Wrapped Lambert ((N.L + 0.5) / 1.5), so a low light
// still lights the surface around it, not just at grazing angles. Falloff shape: the window to the
// power of the light's falloff (1 = smooth, higher = tighter). Spot lights: x saturate((cos to the
// axis - cos outer) / (cos inner - cos outer)); point lights have cos outer -2 (always inside). Per
// vertex: the cover's mesh is dense (0.25 yd near the player), and pixel shaders can't index constants.
float3 Lamps(float3 vp, float3 nv)
{
    float3 sum = 0;
    [loop] for (int k = 0; k < 24; ++k)
    {
        if (k >= pointInfo.x) break;
        float4 p = points[k * 4], col = points[k * 4 + 1], spot = points[k * 4 + 2];
        float spotScale = points[k * 4 + 3].x;
        float3 dv = p.xyz - vp;
        float d2 = dot(dv, dv);
        float d = sqrt(d2);
        float3 l = dv / max(d, 0.001);
        float t = saturate(d * p.w);
        float win = 1 - t * t;
        win = pow(win * win, col.w);
        float de = sqrt(d2 + 1);
        float wrap = saturate((dot(nv, l) + 0.5) / 1.5);
        float cone = saturate((dot(-l, spot.xyz) - spot.w) * spotScale);
        sum += col.rgb * win * wrap * cone / (de * (0.7 + 0.03 * de));
    }
    return sum;
}

float4 Tint(float2 idx) { return look.x > 0.5 ? tex2Dlod(lookTex, float4((idx + 0.5) * grid.w, 0, 0)) : 0; }


float Cheb(float2 idx) { float2 a = abs(idx - lod.xy); return max(a.x, a.y); }
float Fade(float2 idx) { return fade.z > 0.5 ? saturate((fade.y - Cheb(idx)) / (fade.y - fade.x)) : 1; }

// x = base height, y = cover depth
float2 Sample(float2 idx)
{
    float2 uv = (idx + 0.5) * grid.w;
    return float2(tex2Dlod(baseTex, float4(uv, 0, 0)).r, tex2Dlod(coverTex, float4(uv, 0, 0)).r * Fade(idx));
}

float B(float2 idx) { return tex2Dlod(baseTex, float4((idx + 0.5) * grid.w, 0, 0)).r; }
float C(float2 idx) { return tex2Dlod(coverTex, float4((idx + 0.5) * grid.w, 0, 0)).r * Fade(idx); }
// Inside the level's filled area (its grid plus the 2-cell apron): beyond it the slots hold stale heights.
float2 Inside(float2 idx) { return clamp(idx, lod.xy - lod.z - 2, lod.xy + lod.z + 2); }

// step = neighbour distance for the normal in cells (2 = the coarser level's spacing).
// The slope has two parts. The terrain's: the terrain is flat triangles (4 per 4.17 yd cell) and
// shades itself with smooth vertex normals, so measured over 0.25 yd its slope jumps at every crease
// and the lit cover shows the triangles -- it's measured over ~2.5 yd instead (the short step near
// holes). The cover's own (drifts, trenches, rims): the fine step.
void Vertex(float2 idx, float step, out float3 p, out float3 n, out float2 s)
{
    s = Sample(idx);
    p = float3(idx * grid.z, s.x + s.y);
    float wide = max(step, floor(2.5 / grid.z));
    float2 xa = Inside(idx - float2(wide, 0)), xb = Inside(idx + float2(wide, 0));
    float2 ya = Inside(idx - float2(0, wide)), yb = Inside(idx + float2(0, wide));
    float bxa = B(xa), bxb = B(xb), bya = B(ya), byb = B(yb);
    float2 terrain;
    [branch] if (min(min(bxa, bxb), min(bya, byb)) < -10000)
        terrain = float2(B(idx - float2(step, 0)) - B(idx + float2(step, 0)), B(idx - float2(0, step)) - B(idx + float2(0, step))) / (2 * step * grid.z);
    else
        terrain = float2((bxa - bxb) / max(xb.x - xa.x, 1), (bya - byb) / max(yb.y - ya.y, 1)) / grid.z;
    float2 cover = float2(C(idx - float2(step, 0)) - C(idx + float2(step, 0)), C(idx - float2(0, step)) - C(idx + float2(0, step))) / (2 * step * grid.z);
    n = float3(terrain + cover, 1);
}

VOut main(float2 ij : POSITION)
{
    VOut o;
    float2 idx = grid.xy + ij;
    float3 p, n; float2 s;
    Vertex(idx, 1, p, n, s);
    o.tint = Tint(idx);
    o.wuv = (idx + 0.5) * grid.w;

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
        float4 t1 = Tint(idx - odd);
        float2 w1 = (idx - odd + 0.5) * grid.w;
        if (m >= 1) { p = p1; n = n1; s = s1; o.tint = t1; o.wuv = w1; }
        else        { p = lerp(p, p1, m); n = lerp(normalize(n), normalize(n1), m); s = lerp(s, s1, m); o.tint = lerp(o.tint, t1, m); o.wuv = lerp(o.wuv, w1, m); }
    }

    o.far = FarAt(p.xy);

    // Far away the client draws simpler terrain than its heights, which pokes through a thin
    // cover; a lift that only depends on the distance stays seamless across the levels.
    float dist = length(float3(p.xy - eye.xy, p.z - eye.z));
    p.z += lift.z * saturate((dist - lift.x) / (lift.y - lift.x)) * saturate(s.y * 10);

    // Normal and position into view space, where the terrain shader lights and fogs (its sun
    // direction is view space; its fog runs on the view depth).
    float3 nw = normalize(n);
    o.n = nw.x * vr0.xyz + nw.y * vr1.xyz + nw.z * vr2.xyz;
    o.rel = p.xy - eye.xy;
    float4 q = float4(o.rel, p.z - eye.z, 1);
    o.pos = float4(dot(q, vp0), dot(q, vp1), dot(q, vp2), dot(q, vp3));
    o.vpos = q.x * vr0.xyz + q.y * vr1.xyz + q.z * vr2.xyz;
    float viewZ = o.vpos.z;
    o.fog = min(pow(max(viewZ * fogp.x + fogp.y, 0), fogp.z), 1);
    o.lamps = pointInfo.x > 0 ? Lamps(o.vpos, normalize(o.n)) : 0;
    o.d = float2(s.y, s.x < -10000 ? 1 : 0);
    return o;
}
)";

        // PS: c0 = light direction, c1 = colour, c2.x = smallest drawn depth, c3 = the next finer
        // level's box (camera-relative min x, min y, max x, max y): this level isn't drawn there;
        // c4 = ambient, c5 = diffuse, c6 = fog colour; c8/c9 = per cover texture slot 1 when its
        // alpha is a specular mask; c10 = specular colour (rgb) and exponent (w). The tint (rgb, a =
        // strength) comes from the VS, wetness and vertex colour from the props texture.
        // Lit the way the terrain shader lights: min(ambient + diffuse * saturate(N.L), 1), plus
        // Blinn-Phong specular in view space (terrain VS: pow(N.normalize(L + normalize(-pos)), c27.w)
        // x c27.rgb, times the layer texture's alpha in its PS), then fogged towards the fog colour.
        const char* kPsHlsl = R"(
float4 light : register(c0);
float4 albedo : register(c1);
float4 opts : register(c2);
float4 inner : register(c3);
float4 ambient : register(c4);
float4 diffuse : register(c5);
float4 fogColor : register(c6);
float4 texMap : register(c7);   // 1 / tile size (yd), uv offset (x, y): uv = rel * x + yz
float4 specOn0 : register(c8);
float4 specOn1 : register(c9);
float4 specular : register(c10);
float4 wetMap : register(c11);  // 1 / grid extent (yd), uv offset (x, y), on
float4 wetBox : register(c12);  // the grid's valid box, camera-relative (min x, min y, max x, max y)
float4 misc : register(c13);     // x = 1: the terrain's dynamic shadows apply; y = (unused), z = 1 / the near grid's edge fade (yd),
                                 // w = how much darker soaked ground gets (the wet terrain's strength, so both match)
float4 pcf[8] : register(c20);   // the terrain's PCF offsets (its PS c3..c10)
float4 shadowM[12] : register(c30); // the terrain's shadow matrices (its VS c37..c48), from view space
sampler2D cover0 : register(s0);
sampler2D cover1 : register(s1);
sampler2D cover2 : register(s2);
sampler2D cover3 : register(s3);
sampler2D cover4 : register(s4);
sampler2D cover5 : register(s5);
sampler2D cover6 : register(s6);
sampler2D cover7 : register(s7);
sampler2D weights0 : register(s8); // slots 1..4 (r..a)
sampler2D weights1 : register(s9); // slots 5..8
sampler2D props : register(s10);   // a = wetness, rgb = vertex colour (0.5 = neutral)
sampler2D wetGrid : register(s11); // a = ground moisture above the material's rest (env/Fields)
sampler2D shadow0 : register(s12); // the terrain's shadow maps (its s5..s8): three cascades, then far
sampler2D shadow1 : register(s13);
sampler2D shadow2 : register(s14);
sampler2D shadow3 : register(s15);

float Tap(sampler2D s, float2 uv, float z) { return tex2Dlod(s, float4(uv, z, 0)).x; }

// 1 = lit, 0 = in shadow. The same cascades, tests and PCF taps as the terrain's Terrain3 shader
// (dumped 2026-10-02): 9 taps in the nearest cascade, 5 in the next two, then the far map fading out.
float Shadow(float3 vp)
{
    float4 p = float4(vp, 1);
    float4 a = float4(dot(p, shadowM[0]), dot(p, shadowM[1]), dot(p, shadowM[2]), dot(p, shadowM[9]));
    float4 b = float4(dot(p, shadowM[3]), dot(p, shadowM[4]), dot(p, shadowM[5]), dot(p, shadowM[10]));
    float4 c = float4(dot(p, shadowM[6]), dot(p, shadowM[7]), dot(p, shadowM[8]), dot(p, shadowM[11]));
    [branch] if (saturate(max(abs(a.x), abs(a.y)) * -3.4482758 + 3.41379309) > 0)
    {
        float2 uv = a.xy * 0.5 + 0.5;
        float s = Tap(shadow0, uv, a.z);
        [unroll] for (int k = 0; k < 8; ++k) s += Tap(shadow0, uv + pcf[k].xy, a.z);
        return s / 9;
    }
    [branch] if (max(abs(b.x), abs(b.y)) < 1)
    {
        float2 uv = b.xy * 0.5 + 0.5;
        float s = Tap(shadow1, uv, b.z);
        [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow1, uv + pcf[k].xy, b.z);
        return s * 0.2;
    }
    [branch] if (max(abs(c.x), abs(c.y)) < 1)
    {
        float2 uv = c.xy * 0.5 + 0.5;
        float s = Tap(shadow2, uv, c.z);
        [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow2, uv + pcf[k].xy, c.z);
        return s * 0.2;
    }
    float2 uv = float2(a.w, b.w) * 0.5 + 0.5;
    float s = Tap(shadow3, uv, c.w);
    [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow3, uv + pcf[k].xy, c.w);
    float edge = saturate(max(abs(a.w), abs(b.w)) * -11.1111107 + 11);
    return edge * (s * 0.2 - 1) + 1;
}

float4 main(float3 n : TEXCOORD0, float2 d : TEXCOORD1, float2 rel : TEXCOORD2, float fog : TEXCOORD3, float4 tint : TEXCOORD4, float2 wuv : TEXCOORD5, float3 vpos : TEXCOORD6, float far : TEXCOORD7, float3 lamps : TEXCOORD8) : COLOR
{
    clip(d.x - opts.x);
    clip(0.5 - d.y);
    float2 a = step(inner.xy, rel) * step(rel, inner.zw);
    clip(0.5 - a.x * a.y);
    float3 nn = normalize(n), l = normalize(light.xyz);
    float ndl = saturate(dot(nn, l));
    float3 lit = min(ambient.rgb + diffuse.rgb * ndl, 1);
    // Cover textures, tiled in world space like the terrain's layers, mixed by their share here
    // (two filtered weight textures, slots 1..8); the plain cover colour fills the rest. Their
    // alpha is the specular mask (as on the terrain's layers, or the _s texture's).
    float2 uv = rel * texMap.x + texMap.yz;
    float4 w = tex2D(weights0, wuv), v = tex2D(weights1, wuv);
    float4 t0 = tex2D(cover0, uv), t1 = tex2D(cover1, uv), t2 = tex2D(cover2, uv), t3 = tex2D(cover3, uv);
    float4 t4 = tex2D(cover4, uv), t5 = tex2D(cover5, uv), t6 = tex2D(cover6, uv), t7 = tex2D(cover7, uv);
    float3 textured = t0.rgb * w.r + t1.rgb * w.g + t2.rgb * w.b + t3.rgb * w.a + t4.rgb * v.r + t5.rgb * v.g + t6.rgb * v.b + t7.rgb * v.a;
    float mask = dot(float4(t0.a, t1.a, t2.a, t3.a) * w, specOn0) + dot(float4(t4.a, t5.a, t6.a, t7.a) * v, specOn1);
    float share = saturate(dot(w, 1) + dot(v, 1));
    float3 base = albedo.rgb * (1 - share) + textured * albedo.w;
    float3 colour = lerp(base, tint.rgb, saturate(tint.a));
    // Vertex colour (the terrain multiplies by 2 x it) and wetness: darker, and a stronger, tighter shine.
    float4 pr = tex2D(props, wuv);
    // The row's own wet look, plus how much wetter than its rest the ground is now (rain, water nearby).
    // Inside its last yards the grid fades into the one regional value used beyond it.
    float2 fromMin = rel - wetBox.xy, toMax = wetBox.zw - rel;
    float inner = saturate(min(min(fromMin.x, fromMin.y), min(toMax.x, toMax.y)) * misc.z);
    float wet = saturate(pr.a + wetMap.w * lerp(far, tex2D(wetGrid, rel * wetMap.x + wetMap.yz).a, inner));
    colour *= pr.rgb * 2 * (1 - misc.w * wet);
    float3 h = normalize(l + normalize(-vpos));
    float sp = pow(saturate(dot(nn, h)), max(specular.w, 1) * (1 + 3 * wet)) * saturate(mask * (1 + 2 * wet));
    // Dynamic shadows like the terrain's: down to 70% in full shadow, no shine there.
    float shade = misc.x > 0.5 ? Shadow(vpos) : 1;
    // Point lights (torches, from the VS): not shadowed by the sun's shadow maps. Added to the sun's
    // light like the client's own (the same amount whichever way the ground faces the sun), then
    // compressed smoothly above a knee instead of clamped (a hard clamp left a flat white plateau with
    // a ring), on the brightest channel with all three scaled alike so the flame's hue stays. The knee
    // starts at the sun's own level (at least 0.8): without lamps this is the sun's lighting as before.
    float3 sunLit = lit * (0.7 + 0.3 * shade);
    float3 total = sunLit + lamps;
    float peak = max(total.r, max(total.g, total.b));
    float knee = max(0.8, max(sunLit.r, max(sunLit.g, sunLit.b)));
    float over = max(peak - knee, 0);
    float room = max(1 - knee, 0.001);
    float shaped = peak - over + room * (1 - exp(-over / room));
    total *= shaped / max(peak, 0.001);
    float3 c = colour * total + specular.rgb * sp * shade;
    return float4(lerp(fogColor.rgb, c, fog), 1);
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
        // wherever their vertices coincide.
        void Sample(Level& L, bool isLevel0, int i, int j, bool fresh)
        {
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
            if (base != L.base[s]) { L.base[s] = base; L.baseDirty = true; }
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
                L.material[s] = material; L.depth[s] = b.depth; L.slope[s] = slopeF; L.driftAmp[s] = driftAmp;
                L.breakup[s] = breakup; L.drift[s] = drift; L.edgeNoise[s] = edge; L.stiff[s] = stiff;
                L.snowShare[s] = snowShare; L.paintedSnow[s] = paintedSnow;
                L.coverDirty = true;
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
                L.lookDirty = true;
            }
            if (isLevel0)
            {
                if (b.rim != g_rimCell[s]) { g_rimCell[s] = b.rim; L.coverDirty = true; }
                g_relaxCell[s] = b.relax;
                if (fresh) { g_press[s] = 0.0f; g_rim[s] = 0.0f; }
            }
        }

        void SampleRect(Level& L, bool isLevel0, int i0, int i1, int j0, int j1, bool fresh) // inclusive
        {
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) Sample(L, isLevel0, i, j, fresh);
        }

        // Plans a level's next grid around the player (origin on an even cell). The filled area is
        // the grid plus an apron of kApron cells: the normals of the border vertices read up to 2
        // cells beyond the grid (morph band normals at the coarser spacing), and without the apron
        // those slots hold stale heights or holes -- a bright or dark line along every level border.
        // Modes: a new place (nothing valid on screen: filled while hidden), a refill in place (the
        // table or the "where" changed: refilled while the old cover stays visible), or a recentre
        // (only the strips that came into the grid).
        enum class Plan { Recentre, RefillInPlace, NewPlace };

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
            L.coverDirty = true;
            L.baseDirty = true;
            L.lookDirty = true;
            if (L.jobFull) { L.firstFillMs = L.jobMs; L.boostRows = kFilled; } // chunks still streaming in get picked up quickly
            L.fillMs = L.jobMs;
            L.jobActive = false;
            L.job.clear();
            return true;
        }

        // Trench radius per yard of footprint (its longer side): a human's 0.33 yd print gives 0.4 yd.
        constexpr float kFootprintToTrench = 1.2f;

        // A trench under one grounded unit: pressed flat inside r, a rim between r and 1.6 r.
        void Stamp(const float pos[3], float radius)
        {
            const Level& L = g_levels[0];
            float ground;
            if (!terrain::HeightAt(pos[0], pos[1], ground) || std::fabs(pos[2] - ground) > 0.35f) return; // airborne, swimming, on a WMO
            ++g_stamps;
            const float outer = radius * 1.6f, rimPeak = radius * 1.25f, rimHalf = radius * 0.35f;
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
                        if (t > g_press[s]) { g_press[s] = t; MarkActive(s); }
                    }
                    const float r = 1.0f - std::fabs(d - rimPeak) / rimHalf;
                    if (r > g_rim[s]) { g_rim[s] = r; MarkActive(s); }
                }
        }

        void Simulate(float dt)
        {
            // Changes the last frame didn't upload (the cover wasn't drawn) would be lost: redo level 0
            // in full instead.
            if (g_changedPending) g_levels[0].coverDirty = true;
            g_changed.clear();

            for (size_t k = 0; k < g_active.size();)
            {
                const int s = g_active[k];
                const float seconds = g_relaxCell[s] * g_settings.relaxMul;
                const float relax = seconds > 0.1f ? dt / seconds : 1.0f;
                g_press[s] = g_press[s] > relax ? g_press[s] - relax : 0.0f;
                g_rim[s]   = g_rim[s] > relax ? g_rim[s] - relax : 0.0f;
                g_changed.push_back(s);
                if (g_press[s] > 0.0f || g_rim[s] > 0.0f) { ++k; continue; }
                g_isActive[s] = 0; // flat again: uploaded once more, then left alone
                g_active[k] = g_active.back();
                g_active.pop_back();
            }

            g_stamps = 0;
            if (!g_settings.stamp) return;
            // Sized like the client's own footprints (CreatureModelData, x the unit's scale): a frog
            // leaves a small trench, a tauren a wide one. Models without footprints (bats, birds and
            // other fliers that "walk" in the air) leave none.
            for (const actors::Actor& a : actors::Nearby())
            {
                if (!a.hasFootprint) continue;
                const float foot = a.footprintLength > a.footprintWidth ? a.footprintLength : a.footprintWidth;
                float r = (foot > 0.0f ? foot * kFootprintToTrench : (a.effectiveRadius > 0.0f ? a.effectiveRadius : 0.4f)) * g_settings.stampScale;
                if (r < 0.06f) r = 0.06f;
                if (r > 2.5f) r = 2.5f;
                Stamp(a.pos, r);
            }
            g_changedPending = !g_changed.empty();
        }

        // --- GPU ---------------------------------------------------------------------------------
        void ReleaseGpu()
        {
            auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
            rel(g_vb); rel(g_ib); rel(g_decl); rel(g_vs); rel(g_ps); moisturetex::Release();
            for (Level& L : g_levels) { rel(L.baseTex); rel(L.coverTex); rel(L.lookTex); rel(L.texWTex[0]); rel(L.texWTex[1]); rel(L.propsTex); L.baseDirty = L.coverDirty = L.lookDirty = true; }
            for (auto& e : g_textureCache) if (e.second) e.second->Release();
            g_textureCache.clear();
            for (IDirect3DTexture9*& t : g_coverTextures) t = nullptr;
            for (bool& b : g_slotSpecular) b = false;
            for (int& t : g_slotId) t = 0;
            g_slotOfId.clear();
            g_coverTexturesGeneration = 0;
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

        bool VertexTexturesSupported(IDirect3DDevice9* dev, D3DFORMAT format = D3DFMT_R32F)
        {
            IDirect3D9* d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DDISPLAYMODE mode{};
            if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
            const bool ok = SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode)) &&
                            SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                             D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, format));
            d3d->Release();
            return ok;
        }

        bool EnsureGpu(IDirect3DDevice9* dev)
        {
            if (dev != g_device) { ReleaseGpu(); g_gpuFailed = false; g_device = dev; }
            if (g_gpuFailed) return false;
            bool texturesReady = true;
            for (const Level& L : g_levels) texturesReady &= L.baseTex && L.coverTex && L.texWTex[0] && L.texWTex[1] && L.propsTex && (!g_tintSupported || L.lookTex);
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
            // The tint is optional: only where the GPU can fetch A8R8G8B8 in the vertex shader.
            g_tintSupported = VertexTexturesSupported(dev, D3DFMT_A8R8G8B8);
            if (g_tintSupported)
                for (Level& L : g_levels)
                    if (FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &L.lookTex, nullptr)))
                    { L.lookTex = nullptr; g_tintSupported = false; }
            for (Level& L : g_levels)
                for (IDirect3DTexture9*& t : L.texWTex)
                    if (FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)))
                        return fail("cover texture weight textures failed");
            for (Level& L : g_levels)
                if (FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &L.propsTex, nullptr)))
                    return fail("props textures failed");

            g_vs = static_cast<IDirect3DVertexShader9*>(Compile(dev, kVsHlsl, "vs_3_0", false));
            g_ps = static_cast<IDirect3DPixelShader9*>(Compile(dev, kPsHlsl, "ps_3_0", true));
            if (!g_vs || !g_ps) return fail("shader compile failed (see the log)");
            return true;
        }

        // The slots follow what's in view: about once a second the loaded rings are counted
        // (sparsely) by cover texture ID, and the 8 most present IDs hold the slots. An ID keeps
        // its slot while it stays among them, so nothing jumps. Textures load on demand and stay in
        // a small cache, so walking back and forth doesn't reload them.
        IDirect3DTexture9* CachedTexture(IDirect3DDevice9* dev, int id)
        {
            auto found = g_textureCache.find(id);
            if (found != g_textureCache.end()) return found->second;
            if (g_textureCache.size() >= 16)
                for (auto it = g_textureCache.begin(); it != g_textureCache.end(); ++it)
                {
                    bool resident = false;
                    for (int t : g_slotId) resident |= t == it->first;
                    if (resident) continue;
                    if (it->second) it->second->Release();
                    g_textureCache.erase(it);
                    break;
                }
            // The _s variant first (later expansions keep the specular mask in its alpha; the plain one
            // has none), unless the row says to ignore specular. Stock 3.3.5 textures keep it in the
            // plain texture's alpha, like the terrain's layers.
            std::string error;
            IDirect3DTexture9* tex = nullptr;
            const std::string path = covertable::CoverTexturePath(id);
            if (!covertable::CoverTextureIgnoresSpecular(id) && path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".blp") == 0)
            {
                std::string unused;
                tex = blp::Load(dev, (path.substr(0, path.size() - 4) + "_s.blp").c_str(), unused);
                if (tex) g_textureStatus[id] = "loaded (_s)";
            }
            if (!tex)
            {
                tex = blp::Load(dev, path.c_str(), error);
                g_textureStatus[id] = tex ? "loaded" : error;
            }
            if (!tex) g_api->Log(WXL_LOG_WARN, kTag, "surface cover: CoverTexture \"%s\": %s", covertable::CoverTexturePath(id), error.c_str());
            g_textureCache[id] = tex;
            return tex;
        }

        void EnsureCoverTextures(IDirect3DDevice9* dev)
        {
            if (g_coverTexturesGeneration != covertable::Generation())
            {
                g_coverTexturesGeneration = covertable::Generation();
                for (auto& e : g_textureCache) if (e.second) e.second->Release();
                g_textureCache.clear();
                g_textureStatus.clear();
                for (int& t : g_slotId) t = 0;
                g_slotFrame = 0; // recount now
            }
            if (g_slotFrame > 0 && --g_slotFrame > 0) return;
            g_slotFrame = 60;

            const int ids = covertable::CoverTextureCount();
            std::vector<unsigned> count(static_cast<size_t>(ids) + 1, 0);
            for (const Level& L : g_levels)
            {
                if (!L.haveGrid) continue;
                for (int sl = 0; sl < kTex * kTex; sl += 7) // sparse is plenty for "which are around"
                    for (int k = 0; k < 2; ++k)
                    {
                        const int id = static_cast<int>((L.texIds[sl] >> (16 * k)) & 0xFFFF);
                        if (id > 0 && id <= ids && ((L.texShares[sl] >> (8 * k)) & 0xFF)) ++count[id];
                    }
            }
            std::vector<int> wanted;
            for (int id = 1; id <= ids; ++id) if (count[id]) wanted.push_back(id);
            std::sort(wanted.begin(), wanted.end(), [&](int x, int y) { return count[x] > count[y]; });
            g_texturesInView = static_cast<unsigned>(wanted.size());
            if (wanted.size() > static_cast<size_t>(covertable::kMaxCoverTextures)) wanted.resize(covertable::kMaxCoverTextures);

            bool changed = false;
            for (int& t : g_slotId) // residents not wanted any more free their slot
                if (t && std::find(wanted.begin(), wanted.end(), t) == wanted.end()) { t = 0; changed = true; }
            for (int id : wanted)
            {
                if (std::find(std::begin(g_slotId), std::end(g_slotId), id) != std::end(g_slotId)) continue;
                for (int& t : g_slotId) if (!t) { t = id; changed = true; break; }
            }
            if (!changed && g_slotOfId.size() == static_cast<size_t>(ids) + 1) return;

            g_slotOfId.assign(static_cast<size_t>(ids) + 1, -1);
            for (int sl = 0; sl < covertable::kMaxCoverTextures; ++sl)
            {
                g_coverTextures[sl] = g_slotId[sl] ? CachedTexture(dev, g_slotId[sl]) : nullptr;
                g_slotSpecular[sl] = g_coverTextures[sl] && !covertable::CoverTextureIgnoresSpecular(g_slotId[sl]);
                if (g_slotId[sl] && g_slotId[sl] <= ids) g_slotOfId[g_slotId[sl]] = sl;
            }
            ++g_slotGeneration;
        }

        bool UploadLook(Level& L)
        {
            if (L.slotsUploaded != g_slotGeneration) L.lookDirty = true;
            if (!L.lookDirty) return true;
            // The cells' cover texture IDs -> weights of whichever slots hold those IDs now.
            for (int sl = 0; sl < kTex * kTex; ++sl)
            {
                uint8_t w[8] = {};
                for (int k = 0; k < 2; ++k)
                {
                    const int id = static_cast<int>((L.texIds[sl] >> (16 * k)) & 0xFFFF);
                    const int slot = id > 0 && id < static_cast<int>(g_slotOfId.size()) ? g_slotOfId[id] : -1;
                    if (slot >= 0) w[slot] = static_cast<uint8_t>((L.texShares[sl] >> (8 * k)) & 0xFF);
                }
                for (int q = 0; q < 2; ++q)
                    L.texWeights[q][sl] = (static_cast<uint32_t>(w[q * 4 + 3]) << 24) | (static_cast<uint32_t>(w[q * 4]) << 16) |
                                          (static_cast<uint32_t>(w[q * 4 + 1]) << 8) | w[q * 4 + 2];
            }
            L.slotsUploaded = g_slotGeneration;
            IDirect3DTexture9* const textures[4] = { L.lookTex, L.texWTex[0], L.texWTex[1], L.propsTex };
            const std::vector<uint32_t>* const data[4] = { &L.tint, &L.texWeights[0], &L.texWeights[1], &L.props };
            for (int t = 0; t < 4; ++t)
            {
                if (!textures[t]) continue;
                D3DLOCKED_RECT lr{};
                if (FAILED(textures[t]->LockRect(0, &lr, nullptr, 0))) return false;
                for (int row = 0; row < kTex; ++row)
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &(*data[t])[row * kTex], kTex * 4);
                textures[t]->UnlockRect(0);
            }
            L.lookDirty = false;
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
            // Level 0's trenches: only the changed cells, inside one dirty rectangle.
            if (isLevel0 && !L.coverDirty)
            {
                if (g_changed.empty()) { g_changedPending = false; return UploadLook(L); }
                int c0 = kTex, c1 = -1, r0 = kTex, r1 = -1;
                for (int sl : g_changed)
                {
                    const int c = sl % kTex, r = sl / kTex;
                    c0 = c < c0 ? c : c0; c1 = c > c1 ? c : c1;
                    r0 = r < r0 ? r : r0; r1 = r > r1 ? r : r1;
                }
                RECT rect{ c0, r0, c1 + 1, r1 + 1 };
                if (FAILED(L.coverTex->LockRect(0, &lr, &rect, 0))) return false;
                for (int sl : g_changed)
                {
                    const int c = sl % kTex, r = sl / kTex;
                    float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + (r - r0) * lr.Pitch) + (c - c0);
                    const float cover = g_static0[sl];
                    const float deformed = cover * (1.0f + g_rimCell[sl] * g_settings.rimMul * g_rim[sl]) * (1.0f - g_press[sl] * (1.0f - L.stiff[sl] / 255.0f));
                    *out = cover + (deformed - cover) * g_deformK[sl];
                }
                L.coverTex->UnlockRect(0);
                g_changed.clear();
                g_changedPending = false;
                return UploadLook(L);
            }
            // The others only change when their coverage or a multiplier does.
            if (!L.coverDirty) return UploadLook(L);
            const double foldStart = grassperf::Now();
            if (FAILED(L.coverTex->LockRect(0, &lr, nullptr, 0))) return false;
            const float depthMul = g_settings.depthMul, driftMul = g_settings.driftMul, breakupMul = g_settings.breakupMul;
            const int slotI0 = Slot(L.gridI), slotJ0 = Slot(L.gridJ);
            const bool snowOn = fields::SnowActive();
            // Material shaping: steeper with more breakup, and the fine edge noise moves the
            // threshold, so the transition breaks into patches. Fully covered and bare stay so.
            auto shape = [](uint8_t coverage, float b, uint8_t edgeNoise)
            {
                float m = (coverage / 255.0f - 0.5f) * (1.0f + 2.0f * b) + 0.5f + 1.5f * b * (edgeNoise / 255.0f - 0.5f);
                return m <= 0.0f ? 0.0f : (m >= 1.0f ? 1.0f : m * m * (3.0f - 2.0f * m));
            };
            for (int row = 0; row < kTex; ++row)
            {
                float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                // Where this row sits in the grid (Chebyshev distance from the centre feeds the trench fade).
                const int dj = (row - slotJ0 + kTex) % kTex;
                const int cj = std::abs(dj - kHalfCells);
                // The world cell this row holds (the apron sits just before the grid's first cell).
                const float wy = static_cast<float>(L.gridJ + (dj >= kTex - kApron ? dj - kTex : dj)) * L.cell;
                for (int col = 0; col < kTex; ++col)
                {
                    const int s = row * kTex + col;
                    float b = L.breakup[s] / 255.0f * breakupMul;
                    b = b > 1.0f ? 1.0f : b;
                    float driftF = 1.0f + L.driftAmp[s] / 255.0f * driftMul * (L.drift[s] / 127.5f - 1.0f);
                    driftF = driftF < 0.0f ? 0.0f : driftF;
                    const float slope = L.slope[s] / 255.0f;
                    float cover = L.depth[s] > 0.0f ? depthMul * L.depth[s] * shape(L.material[s], b, L.edgeNoise[s]) * slope * driftF : 0.0f;
                    // Snow: painted snow thinned by its kept share (patchy: where the drift and edge
                    // noise are low it opens first), fallen snow on top.
                    if (snowOn && (L.snowShare[s] || L.paintedSnow[s]))
                    {
                        const int di = (col - slotI0 + kTex) % kTex;
                        const float wx = static_cast<float>(L.gridI + (di >= kTex - kApron ? di - kTex : di)) * L.cell;
                        float fallen, keep;
                        fields::SnowAt(wx, wy, fallen, keep);
                        if (L.paintedSnow[s] && keep < 1.0f && cover > 0.0f)
                        {
                            constexpr float kPatchy = 0.6f;
                            const float noise = (0.6f * L.drift[s] + 0.4f * L.edgeNoise[s]) / 255.0f;
                            float t = keep * (1.0f + kPatchy) - kPatchy * (1.0f - noise);
                            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                            cover *= 1.0f - L.paintedSnow[s] / 255.0f * (1.0f - t);
                        }
                        if (L.snowShare[s] && fallen > 0.0f) cover += fallen * shape(L.snowShare[s], b, L.edgeNoise[s]) * slope * driftF;
                    }
                    if (!isLevel0) { out[col] = cover > 0.0f ? cover : 0.0f; continue; }
                    g_static0[s] = cover > 0.0f ? cover : 0.0f;
                    if (cover <= 0.0f) { g_deformK[s] = 0.0f; out[col] = 0.0f; continue; }
                    const int di = (col - slotI0 + kTex) % kTex;
                    const int ci = std::abs(di - kHalfCells);
                    const int cheb = ci > cj ? ci : cj;
                    // Trenches fade out before the morph band, so the border matches the static coarser level.
                    float k = cheb <= kDeformFullCells ? 1.0f
                            : (cheb >= kDeformZeroCells ? 0.0f : float(kDeformZeroCells - cheb) / float(kDeformZeroCells - kDeformFullCells));
                    if (dj >= kGridVerts || di >= kGridVerts) k = 0.0f;
                    g_deformK[s] = k;
                    const float deformed = cover * (1.0f + g_rimCell[s] * g_settings.rimMul * g_rim[s]) * (1.0f - g_press[s] * (1.0f - L.stiff[s] / 255.0f));
                    out[col] = cover + (deformed - cover) * k;
                }
            }
            L.coverTex->UnlockRect(0);
            L.coverDirty = false;
            L.foldMs = grassperf::Now() - foldStart;
            if (isLevel0) { g_changed.clear(); g_changedPending = false; } // the full pass included them
            return UploadLook(L);
        }

        // inPass: called inside the world pass, where the world's viewport and depth surface are still
        // bound. Otherwise (end of scene) both have to be put back first.
        void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth, bool inPass)
        {
            if (!dev || !EnsureGpu(dev)) return;
            const int levels = ActiveLevels();
            // Levels still filling a new place aren't drawn yet; the others are (near ones first).
            int drawn = 0, lastDrawn = -1;
            for (int k = 0; k < levels; ++k) if (g_levels[k].haveGrid) { ++drawn; lastDrawn = k; }
            if (!drawn) { g_inactive = "filling"; return; }

            // The world's view (no translation: the scene is drawn about the camera, so positions go
            // in camera-relative, c6) and its rendered projection.
            float V[16], P[16];
            const void* graphics = gx::RawGraphicsDevice();
            if (!graphics || !wxl::game::gfx::SceneMatrices(V, P)) { g_inactive = "no scene matrices"; return; }
            std::memcpy(P, static_cast<const uint8_t*>(graphics) + kDeviceRenderProjection, sizeof(P));

            EnsureCoverTextures(dev);
            double t0 = grassperf::Now();
            for (int k = 0; k < levels; ++k)
                if (g_levels[k].haveGrid && !UploadLevel(g_levels[k], k == 0)) { g_inactive = "texture upload failed"; return; }
            g_uploadMs = grassperf::Now() - t0;
            t0 = grassperf::Now();

            // Right after the terrain stage its shader constants are still on the device: read them
            // before setting ours. Terrain VS (dumped 2026-10-02): c12 fog (min(pow(max(viewZ * x + y,
            // 0), z), 1)), c24 sun direction in view space, c25 ambient, c26 diffuse; PS c2 fog colour.
            const bool sceneLit = inPass && g_settings.sceneLight;
            if (sceneLit)
            {
                dev->GetVertexShaderConstantF(12, g_seenFog, 1);
                dev->GetVertexShaderConstantF(24, g_seenLight, 4);
                dev->GetPixelShaderConstantF(2, g_seenFogColor, 1);
                g_seenScene = true;
            }

            // The terrain's dynamic shadows (its Terrain2/3 shaders): shadow matrices VS c37..c48, PCF
            // offsets PS c3..c10, shadow maps s5..s8 with their sampler states. The client applies its
            // render-state pop lazily (at its next draw), so they're still on the device here.
            float shadowMatrices[48] = {}, pcfOffsets[32] = {};
            IDirect3DBaseTexture9* shadowMaps[4] = {};
            DWORD shadowStates[4][7] = {};
            constexpr D3DSAMPLERSTATETYPE kShadowStates[7] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                                               D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_BORDERCOLOR, D3DSAMP_SRGBTEXTURE };
            g_shadowUsed = false;
            if (sceneLit)
            {
                bool all = true;
                for (int k = 0; k < 4; ++k)
                {
                    dev->GetTexture(5 + k, &shadowMaps[k]);
                    g_shadowBound[k] = shadowMaps[k] != nullptr;
                    all &= g_shadowBound[k];
                    for (int st = 0; st < 7; ++st) dev->GetSamplerState(5 + k, kShadowStates[st], &shadowStates[k][st]);
                }
                g_shadowUsed = all && g_settings.shadows;
                if (g_shadowUsed)
                {
                    dev->GetVertexShaderConstantF(37, shadowMatrices, 12);
                    dev->GetPixelShaderConstantF(3, pcfOffsets, 8);
                }
            }
            else for (bool& b : g_shadowBound) b = false;

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

            // Lighting: the terrain's (view space) when lit like the scene, else a fixed sun from
            // above in world space, no fog.
            const float a = g_settings.brightness;
            float vs9[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 1, 1, 0 }; // c9..c11 rows, c12 fog (none)
            float ps0[28] = {
                0.35f, 0.45f, 0.82f, 0.0f,       // c0 light direction
                a, a * 1.02f, a * 1.07f, a,      // c1 snow colour (a touch blue); w = brightness on cover textures
                0.004f, 0.0f, 0.0f, 0.0f,        // c2 smallest depth drawn (no z-fighting with the terrain)
                0.0f, 0.0f, 0.0f, 0.0f,          // c3 inner box (set per level)
                0.45f, 0.45f, 0.45f, 0.0f,       // c4 ambient
                0.55f, 0.55f, 0.55f, 0.0f,       // c5 diffuse
                0.0f, 0.0f, 0.0f, 0.0f,          // c6 fog colour
            };
            if (sceneLit)
            {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) vs9[r * 4 + c] = V[r * 4 + c];
                std::memcpy(vs9 + 12, g_seenFog, sizeof(g_seenFog));
                std::memcpy(ps0 + 0, g_seenLight + 0, 3 * sizeof(float));
                std::memcpy(ps0 + 16, g_seenLight + 4, 3 * sizeof(float));
                std::memcpy(ps0 + 20, g_seenLight + 8, 3 * sizeof(float));
                std::memcpy(ps0 + 24, g_seenFogColor, 3 * sizeof(float));
            }
            dev->SetVertexShaderConstantF(9, vs9, 4);

            // Point lights (env/Lights) into the same space as the cover's positions: camera-relative,
            // rotated by the rows in c9..c11. Each patch gets the lights that reach it (PatchLights,
            // before its draw): VS c17.x = how many, c20.. = 4 registers each.
            {
                const std::vector<lights::ActiveLight>& all = lights::Active();
                g_lampRegs.resize(all.size() * 16);
                for (size_t n = 0; n < all.size(); ++n)
                {
                    const lights::ActiveLight& l = all[n];
                    const float q[3] = { l.pos[0] - eye[0], l.pos[1] - eye[1], l.pos[2] - eye[2] };
                    float* o = g_lampRegs.data() + n * 16;
                    for (int c = 0; c < 3; ++c) o[c] = q[0] * vs9[c] + q[1] * vs9[4 + c] + q[2] * vs9[8 + c];
                    o[3] = l.radius > 0.0f ? 1.0f / l.radius : 0.0f;
                    o[4] = l.color[0]; o[5] = l.color[1]; o[6] = l.color[2]; o[7] = l.falloff;
                    for (int c = 0; c < 3; ++c) o[8 + c] = l.spotDir[0] * vs9[c] + l.spotDir[1] * vs9[4 + c] + l.spotDir[2] * vs9[8 + c];
                    o[11] = l.cosOuter;
                    o[12] = l.spotScale;
                    o[13] = o[14] = o[15] = 0.0f;
                }
                g_patchLightsMost = 0;
                g_patchLightsFull = 0;
            }
            // Specular: the terrain's colour and exponent (none when not lit like the scene, or when
            // the terrain has no exponent set).
            float ps8[12] = {};
            for (int sl = 0; sl < covertable::kMaxCoverTextures; ++sl) ps8[sl] = g_slotSpecular[sl] ? 1.0f : 0.0f;
            if (sceneLit && g_seenLight[15] > 0.0f) std::memcpy(ps8 + 8, g_seenLight + 12, 4 * sizeof(float));
            dev->SetPixelShaderConstantF(8, ps8, 3);
            const float c13[4] = { g_tintSupported ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
            dev->SetVertexShaderConstantF(13, c13, 1);
            dev->SetPixelShaderConstantF(0, ps0, 7);

            // Cover textures: tiled in world space; the offset keeps the uv small (the scene is drawn
            // about the camera, so positions arrive camera-relative).
            const float inv = 1.0f / kCoverTextureTile;
            const float c7[4] = { inv, eye[0] * inv - std::floor(eye[0] * inv), eye[1] * inv - std::floor(eye[1] * inv), 0.0f };
            dev->SetPixelShaderConstantF(7, c7, 1);
            for (int t = 0; t < covertable::kMaxCoverTextures; ++t)
            {
                dev->SetTexture(t, g_coverTextures[t]);
                dev->SetSamplerState(t, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(t, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(t, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                dev->SetSamplerState(t, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                dev->SetSamplerState(t, D3DSAMP_SRGBTEXTURE, FALSE);
            }

            // Ground moisture (env/Fields), shared with the patched terrain (render/MoistureTexture).
            {
                float winv = 0.0f, box[4] = {};
                const bool wetOk = moisturetex::Mapping(winv, box) && moisturetex::Bind(dev, 11);
                const float c11[4] = { winv, eye[0] * winv - std::floor(eye[0] * winv), eye[1] * winv - std::floor(eye[1] * winv), wetOk ? 1.0f : 0.0f };
                const float c12[4] = { box[0] - eye[0], box[1] - eye[1], box[2] - eye[0], box[3] - eye[1] };
                dev->SetPixelShaderConstantF(11, c11, 1);
                dev->SetPixelShaderConstantF(12, c12, 1);
            }

            // The terrain's shadow maps on our free samplers s12..s15, exactly as it sampled them.
            {
                const float c13[4] = { g_shadowUsed ? 1.0f : 0.0f, moisturetex::FarExcess(), 1.0f / moisturetex::kEdgeFadeYards, terrainwet::Strength() };
                dev->SetPixelShaderConstantF(13, c13, 1);
                if (g_shadowUsed)
                {
                    dev->SetPixelShaderConstantF(20, pcfOffsets, 8);
                    dev->SetPixelShaderConstantF(30, shadowMatrices, 12);
                    for (int k = 0; k < 4; ++k)
                    {
                        dev->SetTexture(12 + k, shadowMaps[k]);
                        for (int st = 0; st < 7; ++st) dev->SetSamplerState(12 + k, kShadowStates[st], shadowStates[k][st]);
                    }
                }
            }
            for (IDirect3DBaseTexture9*& t : shadowMaps) if (t) { t->Release(); t = nullptr; }

            // Ground moisture beyond the near grid, per terrain chunk (its zone's), on vertex sampler 3.
            {
                float farInv = 0.0f, box[4] = {}, outside = 0.0f;
                int size = 0;
                const bool farOn = moisturetex::FarMapping(farInv, box, size, outside) && moisturetex::BindFarVertex(dev, D3DVERTEXTEXTURESAMPLER3);
                const float vs14[12] = { farInv * size, static_cast<float>(size), size > 0 ? 1.0f / size : 0.0f, farOn ? 1.0f : 0.0f,
                                         box[0], box[1], box[2], box[3],
                                         outside, 0.0f, 0.0f, 0.0f };
                dev->SetVertexShaderConstantF(14, vs14, 3);
            }

            dev->SetVertexDeclaration(g_decl);
            dev->SetStreamSource(0, g_vb, 0, 8);
            dev->SetIndices(g_ib);
            dev->SetVertexShader(g_vs);
            dev->SetPixelShader(g_ps);
            for (int s = 0; s < 3; ++s)
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
                if (!L.haveGrid) continue;
                const bool outermost = k == lastDrawn;
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, L.baseTex);
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER1, L.coverTex);
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER2, g_tintSupported ? L.lookTex : nullptr);
                for (int q = 0; q < 3; ++q)
                {
                    dev->SetTexture(8 + q, q < 2 ? L.texWTex[q] : L.propsTex);
                    dev->SetSamplerState(8 + q, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    dev->SetSamplerState(8 + q, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    dev->SetSamplerState(8 + q, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                    dev->SetSamplerState(8 + q, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                    dev->SetSamplerState(8 + q, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                    dev->SetSamplerState(8 + q, D3DSAMP_SRGBTEXTURE, FALSE);
                }

                const float c5[4] = { static_cast<float>(L.gridI + kHalfCells), static_cast<float>(L.gridJ + kHalfCells),
                                      static_cast<float>(kHalfCells), outermost ? 0.0f : static_cast<float>(kMorphCells) };
                dev->SetVertexShaderConstantF(5, c5, 1);
                const float c7[4] = { kHalfCells * kOuterFadeStart, kHalfCells * kOuterFadeEnd, outermost ? 1.0f : 0.0f, 0.0f };
                dev->SetVertexShaderConstantF(7, c7, 1);

                // Not drawn inside the next finer level's grid (camera-relative box); level 0 has none.
                // The box is a hair smaller than that grid, so the two levels overlap slightly where
                // they meet (same heights there) instead of both skipping a pixel exactly on the edge.
                float inner[4] = { 1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f };
                int finer = k - 1;
                while (finer >= 0 && !g_levels[finer].haveGrid) --finer;
                if (finer >= 0)
                {
                    const Level& F = g_levels[finer];
                    const double e = F.cell * 0.25;
                    inner[0] = static_cast<float>(static_cast<double>(F.gridI) * F.cell + e - eye[0]);
                    inner[1] = static_cast<float>(static_cast<double>(F.gridJ) * F.cell + e - eye[1]);
                    inner[2] = static_cast<float>(static_cast<double>(F.gridI + kGridCells) * F.cell - e - eye[0]);
                    inner[3] = static_cast<float>(static_cast<double>(F.gridJ + kGridCells) * F.cell - e - eye[1]);
                }
                dev->SetPixelShaderConstantF(3, inner, 1);
                const float innerWorld[4] = { inner[0] + eye[0], inner[1] + eye[1], inner[2] + eye[0], inner[3] + eye[1] };

                for (int pj = 0; pj < kPatches; ++pj)
                    for (int pi = 0; pi < kPatches; ++pi)
                    {
                        const float c4[4] = { static_cast<float>(L.gridI + pi * kPatchCells), static_cast<float>(L.gridJ + pj * kPatchCells),
                                              L.cell, 1.0f / kTex };
                        dev->SetVertexShaderConstantF(4, c4, 1);
                        const float patch[4] = {
                            static_cast<float>(static_cast<double>(L.gridI + pi * kPatchCells) * L.cell),
                            static_cast<float>(static_cast<double>(L.gridJ + pj * kPatchCells) * L.cell),
                            static_cast<float>(static_cast<double>(L.gridI + (pi + 1) * kPatchCells) * L.cell),
                            static_cast<float>(static_cast<double>(L.gridJ + (pj + 1) * kPatchCells) * L.cell) };
                        PatchLights(dev, patch, innerWorld, eye);
                        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kPatchVerts * kPatchVerts, 0, kPatchCells * kPatchCells * 2);
                    }
            }

            dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
            dev->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
            dev->SetTexture(D3DVERTEXTEXTURESAMPLER2, nullptr);
            dev->SetTexture(D3DVERTEXTEXTURESAMPLER3, nullptr);
            for (int t = 0; t < 16; ++t) dev->SetTexture(t, nullptr);
            if (swapDepth) dev->SetDepthStencilSurface(oldDepth);
            if (oldDepth) oldDepth->Release();
            saved->Apply(); // includes the viewport
            saved->Release();
            // Vertex sampler 0 holds the model lights' list for any M2/WMO drawn after us.
            modellights::Rebind(dev);
            g_drawMs = grassperf::Now() - t0;
            g_inactive = nullptr;
        }

        void __cdecl hkTerrainStage()
        {
            // The patched terrain shaders read the ground moisture (features/TerrainWetness).
            terrainwet::BeforeTerrainStage(static_cast<IDirect3DDevice9*>(gx::RawDevice()));
            terrainlights::BeforeTerrainStage(static_cast<IDirect3DDevice9*>(gx::RawDevice()));
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
            char line[384];
            g_api->UiCheckbox("Draw the cover", &g_settings.enabled);
            static const char* const coverage[] = { "Everywhere (test: nothing painted, fallen snow anywhere)", "From SurfaceCover.cdbc" };
            g_api->UiCombo("Where", &g_settings.coverage, coverage, 2);
            g_api->UiSliderInt("Levels (40 / 80 / 160 / 320 / 640 yd)", &g_settings.levels, 1, kMaxLevels);
            g_api->UiText("Multipliers on SurfaceCover.cdbc (1 = exactly the table):");
            g_api->UiSliderFloat("Depth (x table)", &g_settings.depthMul, 0.0f, 3.0f);
            g_api->UiSliderFloat("Drift noise (x table)", &g_settings.driftMul, 0.0f, 3.0f);
            g_api->UiSliderFloat("Edge breakup (x table)", &g_settings.breakupMul, 0.0f, 3.0f);
            g_api->UiSliderFloat("Rim (x table)", &g_settings.rimMul, 0.0f, 3.0f);
            g_api->UiSliderFloat("Relax time (x table)", &g_settings.relaxMul, 0.1f, 10.0f);
            g_api->UiCheckbox("Units carve trenches", &g_settings.stamp);
            g_api->UiSliderFloat("Trench width (x unit size)", &g_settings.stampScale, 0.3f, 3.0f);
            g_api->UiSliderFloat("Far lift (yd, 80 -> 400 yd away)", &g_settings.farLift, 0.0f, 3.0f);
            g_api->UiCheckbox("Light and fog like the terrain", &g_settings.sceneLight);
            g_api->UiCheckbox("Dynamic shadows like the terrain", &g_settings.shadows);
            g_api->UiSliderFloat("Snow brightness", &g_settings.brightness, 0.3f, 1.5f);
            g_api->UiSliderFloat("Fill budget (ms per frame)", &g_settings.fillBudgetMs, 0.5f, 10.0f);
            static const char* const sources[] = { "Painted strength of every layer (alpha maps)", "Dominant layer per terrain cell (old)" };
            g_api->UiCombo("Material from", &g_settings.materialSource, sources, 2);
            g_api->UiCheckbox("Swap alpha map axes (debug)", &g_settings.alphaSwap);
            g_api->UiCheckbox("Wireframe", &g_settings.wireframe);
            static const char* const drawPoints[] = { "Right after the terrain (inside the world pass)", "End of the scene (old)" };
            g_api->UiCombo("Draw point", &g_settings.drawPoint, drawPoints, 2);

            g_api->UiSeparator();
            g_api->UiText("Fallen snow (test, not saved): a deposit on your zone, which then melts or stays by the climate.");
            g_api->UiSliderFloat("Deposit (yd)", &g_depositYards, 0.02f, 0.6f);
            if (g_api->UiButton("Deposit snow")) regional::DepositSnow(g_depositYards);
            g_api->UiSameLine();
            if (g_api->UiButton("Clear fallen snow")) regional::ClearSnow();

            g_api->UiSeparator();
            std::snprintf(line, sizeof(line), "SurfaceCover.cdbc: %s", covertable::Status());
            g_api->UiText(line);
            g_api->UiSameLine();
            if (g_api->UiButton("Reload table")) covertable::Load(g_cdbcApi);
            std::snprintf(line, sizeof(line), "  cover textures in view: %u (slots: %d)%s", g_texturesInView, covertable::kMaxCoverTextures,
                          g_texturesInView > static_cast<unsigned>(covertable::kMaxCoverTextures) ? " -- the least present ones draw without their texture" : "");
            g_api->UiText(line);
            for (int sl = 0; sl < covertable::kMaxCoverTextures; ++sl)
            {
                if (!g_slotId[sl]) continue;
                auto st = g_textureStatus.find(g_slotId[sl]);
                std::snprintf(line, sizeof(line), "  slot %d: \"%s\" -- %s", sl + 1, covertable::CoverTexturePath(g_slotId[sl]),
                              st == g_textureStatus.end() ? "?" : st->second.c_str());
                g_api->UiText(line);
            }
            std::vector<std::string> lines;
            auto add = [&lines, &line]() { lines.emplace_back(line); };
            if (const char* why = g_settings.enabled ? g_inactive : "switched off") { std::snprintf(line, sizeof(line), "not drawing: %s", why); add(); }
            const int levels = ActiveLevels();
            for (int k = 0; k < levels; ++k)
            {
                const Level& L = g_levels[k];
                char state[48] = "";
                if (L.jobActive)
                    std::snprintf(state, sizeof(state), ", %s %u%%", L.jobHidden ? "filling" : (L.jobFull ? "refilling" : "recentring"),
                                  static_cast<unsigned>(L.job.empty() ? 100 : 100 * L.jobPos / L.job.size()));
                std::snprintf(line, sizeof(line), "level %d: %.2f yd cells, %.0f yd out, first cell (%d, %d), holes/unloaded %u, full fill %.2f ms, last recentre %.2f ms%s",
                              k, L.cell, kHalfCells * L.cell, L.gridI, L.gridJ, L.holes, L.firstFillMs, L.fillMs, state); add();
            }
            std::snprintf(line, sizeof(line), "%d x %d vertices; units stamping %u; terrain cells looked up %zu; tint %s",
                          levels * kPatches * kPatches, kPatchVerts * kPatchVerts, g_stamps, static_cast<size_t>(g_cellMisses),
                          g_tintSupported ? "on" : "not supported by this GPU"); add();
            for (const actors::Actor& a : actors::Nearby())
            {
                if (!a.isPlayer) continue;
                const float foot = a.footprintLength > a.footprintWidth ? a.footprintLength : a.footprintWidth;
                std::snprintf(line, sizeof(line), "player footprint: %s, %.2f x %.2f yd -> trench radius %.2f yd%s",
                              a.hasFootprint ? "yes" : "none (no stamp)", a.footprintLength, a.footprintWidth,
                              foot * kFootprintToTrench * g_settings.stampScale, a.mounted ? " (mount's)" : ""); add();
                break;
            }
            if (g_hereValid)
            {
                std::snprintf(line, sizeof(line), "here: texture \"%s\", ground effect %u, TerrainType %d, area %u",
                              g_hereTexture.c_str(), g_hereSurface.groundEffect, g_hereSurface.terrainType, g_hereSurface.area); add();
                const covertable::Values& v = g_hereValues;
                if (v.material)
                {
                    const materials::Values mv = materials::Get(v.material);
                    std::snprintf(line, sizeof(line), "here: material %u \"%s\" (parent %u), cover material %u \"%s\", rest moisture %.2f, cover stiffness %.2f",
                                  v.material, materials::Name(v.material), materials::Parent(v.material),
                                  v.coverMaterial, materials::Name(v.coverMaterial), mv.restMoisture, v.stiffness);
                }
                else std::snprintf(line, sizeof(line), "here: no ground material (no selector matches)");
                add();
                std::snprintf(line, sizeof(line), "here (table): depth %.2f, max slope %.0f, fade %.0f, drift %.2f, breakup %.2f, rim %.2f, relax %.0f s, tint %08X x %.2f, cover texture %d, z offset %.3f, wetness %.2f, flags 0x%X",
                              v.depth, v.maxSlope, v.slopeFade, v.driftNoise, v.edgeBreakup, v.rim, v.relaxSeconds, v.tintColor, v.tintStrength, v.coverTexture,
                              v.zOffset, v.wetness, v.flags); add();
                if (g_hereMccvValid)
                    std::snprintf(line, sizeof(line), "here: vertex colour (MCCV) r %.2f g %.2f b %.2f (0.50 = neutral)", g_hereMccv[0], g_hereMccv[1], g_hereMccv[2]);
                else
                    std::snprintf(line, sizeof(line), "here: no vertex colour (MCCV) in this chunk");
                add();
                if (g_hereLiquidValid)
                    std::snprintf(line, sizeof(line), "here: liquid surface z %.2f, %.2f yd above the terrain (%s)", g_hereLiquid, g_hereLiquidDepth,
                                  g_hereLiquidDepth > 0.0f ? "underwater" : "dry");
                else
                    std::snprintf(line, sizeof(line), "here: no terrain liquid");
                add();
            }
            else { std::snprintf(line, sizeof(line), "here: no terrain under the player"); add(); }
            if (g_hereLayersValid)
            {
                auto dominant = [](const terrain::LayerWeights& lw)
                {
                    int best = -1; float w = -1.0f;
                    for (int l = 0; l < lw.layers; ++l) if (lw.weight[l] > w) { w = lw.weight[l]; best = l; }
                    return best;
                };
                std::snprintf(line, sizeof(line), "here (alpha): %d layer(s), weights %.2f %.2f %.2f %.2f | swapped %.2f %.2f %.2f %.2f",
                              g_hereLayers.layers, g_hereLayers.weight[0], g_hereLayers.weight[1], g_hereLayers.weight[2], g_hereLayers.weight[3],
                              g_hereLayersSwapped.weight[0], g_hereLayersSwapped.weight[1], g_hereLayersSwapped.weight[2], g_hereLayersSwapped.weight[3]); add();
                const int a = dominant(g_hereLayers), b = dominant(g_hereLayersSwapped), c = g_hereLayers.dominantLowRes;
                std::snprintf(line, sizeof(line), "here (check): strongest layer by alpha %d, swapped %d, the client's dominant layer %d -> %s",
                              a, b, c, a == c && b != c ? "alpha read the right way" : (b == c && a != c ? "alpha axes SWAPPED" : "same either way here (try another spot)")); add();
                for (int l = 0; l < g_hereLayers.layers; ++l)
                {
                    const terrain::Surface& sf = g_hereLayers.surface[l];
                    std::snprintf(line, sizeof(line), "  layer %d: %.2f  \"%s\", effect %u, TerrainType %d", l, g_hereLayers.weight[l],
                                  g_hereLayerTexture[l].c_str(), sf.groundEffect, sf.terrainType); add();
                }
            }
            {
                const world::Snapshot& snap = world::Current();
                const fields::SnowDetail sd = snap.inWorld ? fields::Snow(snap.playerPos) : fields::SnowDetail{};
                if (sd.known)
                {
                    std::snprintf(line, sizeof(line), "here (snow): fallen %.3f yd -> %.3f (zones' %.3f x held %.2f), painted %.2f yd, kept %.2f -> cap %.2f (kept share %.2f, gone at %.0f degC)",
                                  sd.fallen, sd.target, sd.zoneSnow, sd.hold, sd.painted, sd.keep, sd.cap, sd.keptShare, sd.goneTemperature); add();
                    std::snprintf(line, sizeof(line), "here (snow): effective %.1f degC (sun %+.1f), zone's snow sees %.1f degC, heat reach %.2f, melt water %.2f",
                                  sd.temperature, sd.sun, sd.zoneTemperature, sd.heatReach, sd.meltWet); add();
                }
                else { std::snprintf(line, sizeof(line), "here (snow): not sampled yet"); add(); }
                const climate::Sun& sun = climate::SunNow();
                std::snprintf(line, sizeof(line), "snow: published %u time(s), %s; sun %.2f %.2f %.2f (%s), daylight %.2f; last depth folds %.2f / %.2f / %.2f / %.2f / %.2f ms",
                              fields::GetStats().snowPublished, fields::SnowActive() ? "some snow around" : "none around",
                              sun.dir[0], sun.dir[1], sun.dir[2], sun.fromClient ? "client" : "from the time", sun.daylight,
                              g_levels[0].foldMs, g_levels[1].foldMs, g_levels[2].foldMs, g_levels[3].foldMs, g_levels[4].foldMs); add();
            }
            std::snprintf(line, sizeof(line), "CPU: sim %.2f ms, upload %.2f ms, draw submit %.2f ms", g_simMs, g_uploadMs, g_drawMs); add();
            if (g_seenScene)
            {
                std::snprintf(line, sizeof(line), "terrain light: sun (view) %.3f %.3f %.3f, ambient %.3f %.3f %.3f, diffuse %.3f %.3f %.3f",
                              g_seenLight[0], g_seenLight[1], g_seenLight[2], g_seenLight[4], g_seenLight[5], g_seenLight[6],
                              g_seenLight[8], g_seenLight[9], g_seenLight[10]); add();
                std::snprintf(line, sizeof(line), "terrain fog: params %.6f %.4f %.4f %.4f, colour %.3f %.3f %.3f",
                              g_seenFog[0], g_seenFog[1], g_seenFog[2], g_seenFog[3], g_seenFogColor[0], g_seenFogColor[1], g_seenFogColor[2]); add();
                std::snprintf(line, sizeof(line), "terrain shadow maps at our draw: s5 %s, s6 %s, s7 %s, s8 %s -> cover shadows %s",
                              g_shadowBound[0] ? "bound" : "-", g_shadowBound[1] ? "bound" : "-", g_shadowBound[2] ? "bound" : "-", g_shadowBound[3] ? "bound" : "-",
                              g_shadowUsed ? "on" : (g_settings.shadows ? "off (not all bound: shadow quality off, or the client unbinds them)" : "off (switched off)")); add();
            }

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

    void Configure(bool enabled, int levels, bool trenches, float depthMultiplier, float fillBudgetMs)
    {
        g_settings.enabled = enabled ? 1 : 0;
        g_settings.levels = levels < 1 ? 1 : (levels > kMaxLevels ? kMaxLevels : levels);
        g_settings.stamp = trenches ? 1 : 0;
        g_settings.depthMul = depthMultiplier < 0.0f ? 0.0f : depthMultiplier;
        g_settings.fillBudgetMs = fillBudgetMs < 0.1f ? 0.1f : fillBudgetMs;
    }

    bool Enabled() { return g_settings.enabled != 0; }
    int  Levels() { return g_settings.levels; }
    bool Trenches() { return g_settings.stamp != 0; }
    float DepthMultiplier() { return g_settings.depthMul; }
    float FillBudgetMs() { return g_settings.fillBudgetMs; }

    void LoadTable(const void* cdbcApi)
    {
        g_cdbcApi = cdbcApi;
        covertable::Load(cdbcApi);
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

    void PatchLightStats(unsigned& most, unsigned& full) { most = g_patchLightsMost; full = g_patchLightsFull; }

    void Update(float dt, const world::Snapshot& snap)
    {
        // Leaving the world (logout, loading screens without a player) unloads every tile: drop
        // everything that refers to them, and start over on the way back.
        if (!snap.inWorld)
        {
            if (g_wasInWorld)
            {
                terrain::ClearLayerCache();
                g_layerValues.clear();
                ClearCellCache();
                g_hereLayersValid = g_hereValid = false;
                for (Level& L : g_levels) { L.haveGrid = false; L.jobActive = false; }
                g_lastMap = -1;
            }
            g_wasInWorld = false;
            return;
        }
        g_wasInWorld = true;
        if (!g_settings.enabled) return;
        if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
        const double t0 = grassperf::Now();

        g_mapNow = snap.mapId;
        const bool newMap = g_lastMap != snap.mapId;
        const bool refill = newMap || g_lastCoverage != g_settings.coverage || g_tableGeneration != covertable::Generation() ||
                            g_lastMaterialSource != g_settings.materialSource || g_lastAlphaSwap != g_settings.alphaSwap;
        if (refill) { ClearCellCache(); g_layerValues.clear(); }
        if (newMap) terrain::ClearLayerCache();
        g_lastMaterialSource = g_settings.materialSource;
        g_lastAlphaSwap = g_settings.alphaSwap;
        g_lastCoverage = g_settings.coverage;
        g_lastMap = snap.mapId;
        g_tableGeneration = covertable::Generation();
        if (g_lastDepthMul != g_settings.depthMul || g_lastDriftMul != g_settings.driftMul)
        {
            g_lastDepthMul = g_settings.depthMul; g_lastDriftMul = g_settings.driftMul;
            for (Level& L : g_levels) L.coverDirty = true;
        }
        if (g_lastBreakupMul != g_settings.breakupMul)
        {
            g_lastBreakupMul = g_settings.breakupMul;
            for (Level& L : g_levels) { L.coverDirty = true; L.boostRows = kFilled; } // the warp is sampled
        }

        ++g_frame;
        const int levels = ActiveLevels();
        for (int k = 0; k < levels; ++k)
        {
            Level& L = g_levels[k];
            // A refill or a new map replaces whatever job was running; a recentre waits for it.
            if (refill) PlanFill(L, snap.playerPos, newMap || !L.haveGrid ? Plan::NewPlace : Plan::RefillInPlace);
            else if (!L.jobActive) PlanFill(L, snap.playerPos, Plan::Recentre);
            if (L.jobActive) continue; // re-sampling and hole counting wait for the fill
            // Chunks stream in after the grid saw them: re-sample a few rows per frame, keeping trenches.
            // Once nothing is missing one row is enough, except for a pass at full speed after a
            // setting that's applied while sampling changed.
            const int fast = k == 0 ? 8 : 4;
            const bool idle = !L.holes && L.boostRows <= 0;
            const int rows = !idle ? fast : ((g_frame % 8) == static_cast<unsigned>(k) ? 1 : 0);
            if (L.boostRows > 0) L.boostRows -= rows;
            for (int r = 0; r < rows; ++r)
            {
                const int row = L.gridJ - kApron + L.refreshRow;
                SampleRect(L, k == 0, L.gridI - kApron, L.gridI - kApron + kFilled - 1, row, row, false);
                L.refreshRow = (L.refreshRow + 1) % kFilled;
            }
            if (rows && L.refreshRow < rows) // once per pass: count what's still missing
            {
                unsigned holes = 0;
                for (int j = 0; j < kGridVerts; ++j)
                    for (int i = 0; i < kGridVerts; ++i) holes += L.base[SlotOf(L.gridI + i, L.gridJ + j)] == kHole;
                L.holes = holes;
            }
        }
        // A level switched off is refilled from scratch when it comes back.
        for (int k = levels; k < kMaxLevels; ++k) { g_levels[k].haveGrid = false; g_levels[k].jobActive = false; }

        // Fills within the budget. Levels with nothing on screen go first, coarsest first (a coarse
        // level covers the most ground for the same work, so there's always cover under the player);
        // the rest of the budget is shared evenly by the visible levels' jobs, so fast travel can't
        // starve any of them.
        const double end = grassperf::Now() + (g_settings.fillBudgetMs > 0.1f ? g_settings.fillBudgetMs : 0.1f);
        int order[kMaxLevels], jobs = 0;
        for (int k = levels - 1; k >= 0; --k) if (g_levels[k].jobActive && g_levels[k].jobHidden) order[jobs++] = k;
        for (int k = 0; k < levels; ++k)      if (g_levels[k].jobActive && !g_levels[k].jobHidden) order[jobs++] = k;
        for (int n = 0; n < jobs; ++n)
        {
            const double now = grassperf::Now();
            if (now >= end) break;
            const int k = order[n];
            const bool hidden = g_levels[k].jobHidden;
            const double deadline = hidden ? end : now + (end - now) / (jobs - n);
            WorkFill(g_levels[k], k == 0, deadline);
        }

        // New snow values (fallen snow, painted snow melting): one level re-folds its depth per frame.
        // While there's no snow anywhere and the last fold had none either, there's nothing to redo.
        {
            const uint32_t version = fields::SnowVersion();
            const bool active = fields::SnowActive();
            for (int n = 0; n < levels; ++n)
            {
                const int k = (g_snowCursor + n) % levels;
                Level& L = g_levels[k];
                if (!L.haveGrid || L.snowVersion == version) continue;
                L.snowVersion = version;
                if (active || L.snowFolded) L.coverDirty = true;
                L.snowFolded = active;
                g_snowCursor = k + 1;
                break;
            }
        }

        // The table's values where the player is, for the panel's "here" lines.
        covertable::Values here;
        g_hereValid = terrain::SurfaceAt(snap.playerPos[0], snap.playerPos[1], g_hereSurface);
        if (g_hereValid)
        {
            g_hereTexture = g_hereSurface.texture ? g_hereSurface.texture : "";
            if (static_cast<Coverage>(g_settings.coverage) == Coverage::Table)
                here = covertable::Resolve(g_hereSurface.area, snap.mapId, g_hereSurface.texture, g_hereSurface.groundEffect, g_hereSurface.terrainType);
        }
        g_hereValues = here;
        float terrainZ;
        g_hereMccvValid = terrain::VertexColorAt(snap.playerPos[0], snap.playerPos[1], g_hereMccv);
        g_hereLiquidValid = terrain::LiquidHeightAt(snap.playerPos[0], snap.playerPos[1], g_hereLiquid) &&
                            terrain::HeightAt(snap.playerPos[0], snap.playerPos[1], terrainZ);
        if (g_hereLiquidValid) g_hereLiquidDepth = g_hereLiquid - terrainZ;
        g_hereLayersValid = terrain::LayerWeightsAt(snap.playerPos[0], snap.playerPos[1], g_hereLayers, false) &&
                            terrain::LayerWeightsAt(snap.playerPos[0], snap.playerPos[1], g_hereLayersSwapped, true);
        for (int l = 0; l < 4; ++l)
        {
            g_hereLayerTexture[l] = g_hereLayersValid && l < g_hereLayers.layers && g_hereLayers.surface[l].texture ? g_hereLayers.surface[l].texture : "";
            g_hereLayers.surface[l].texture = g_hereLayersSwapped.surface[l].texture = nullptr;
        }

        Simulate(dt);
        g_simMs = grassperf::Now() - t0;
    }
}
