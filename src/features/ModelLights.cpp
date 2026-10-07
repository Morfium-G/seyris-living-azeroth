#include "ModelLights.hpp"

#include "../env/Lights.hpp"
#include "../render/M2Effects.hpp"
#include "../render/ShaderPatch.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace wxl_livingazeroth::modellights
{
    namespace
    {
        // Everything the patch needs per frame lives in one texture on vertex sampler 0, read with
        // texldl. Vertex constants can't hold it: every register from c31 up can carry bone matrices
        // (the palette is indexed from c31 and reaches c255 for large batches: in-client probe
        // 2026-10-06 found bone rows in c236..c247 at every model draw), and below c31 only c1 is
        // unused by every M2/WMO vertex shader (dumps 2026-10-05). So the shader defines c1 alone
        // (the header's texture coordinates) and reads the rest from the texture. A def above c31
        // would also replace bone data for the batches that reach it.
        //
        // kWidth A32B32G32R32F texels per row (fixed: the shaders' defs know it; the height follows
        // the grid settings and reaches them through the header). Also bound to the terrain's PS s13
        // (TerrainLights), which takes the grid from pixel constants instead of the header.
        //  row 0, the header:
        //  t0  (stock lights off amount, 0, 0, valid)   all zero = stock lighting only
        //  t1  camera position, .w the occlusion region's first row + 0.5
        //                                     t2..t4  the scene view's columns (camera-relative
        //  world -> view, row-vector convention, gfx::SceneMatrices)  t5  debug-view weights
        //  t6  K1 (1/W, 1/H, lights per cell, 0)   t7  K2 (1, 0, 0.5, 2/3)
        //  t8  K3 (0.7, 0.03, 1/ln2, 0.05)         t9  K4 (0.8, 0.001, 0.25, 1 / lights per cell)
        //  t10 K5 (scale of ours on baked-light WMO surfaces, 0, 0, 0)
        //  t11 the grid (world x, y of cell 0's corner, 1 / cell size, cells per side)
        //  rows 1..: the cells' entries, row-major: cell c's k-th light at linear texel c * perCell + k,
        //        each (u, v of the light's first texel, 1, 0); (0, 0, 0, 0) ends a cell's list.
        //  then the light pool (lights::Active()), 4 texels each, never split across rows (world
        //        position + 1 / radius; colour + falloff shape; spot direction + cos outer;
        //        1 / (cos inner - cos outer), 0, flicker dip, 1 = indoor: not on the terrain).
        // The shaders' positions, normals and sun direction are in VIEW space (their fog reads the
        // position's z as depth; lights placed as world - camera moved with the camera's angle,
        // in-client 2026-10-06), hence the camera and the view in the header; each vertex goes back to
        // world space to find its cell.
        constexpr int   kHeader = 12;
        constexpr int   kTexelsPerLight = 4;
        constexpr int   kWidth = 1024;
        constexpr int   kPoolRows = (lights::kMaxPool * kTexelsPerLight + kWidth - 1) / kWidth;
        static_assert(kWidth % kTexelsPerLight == 0, "a light's texels must share a row");
        // Baked occlusion, after the pool: light p's tile (lights::kOccCells distances, 4 per texel) at
        // linear texel p x kOccTexels of this region; its 4th texel .y = p / kTilesPerRow, so the
        // shaders take the row from the whole part and the tile's u from the fraction (or -1: none).
        constexpr int   kOccTexels = lights::kOccCells / 4;
        constexpr int   kTilesPerRow = kWidth / kOccTexels;
        constexpr int   kOccRows = (lights::kMaxPool + kTilesPerRow - 1) / kTilesPerRow;
        static_assert(kWidth % kOccTexels == 0, "a tile must not split across rows");
        std::vector<const float*> g_occWritten; // per pool light, the occlusion its tile holds
        constexpr DWORD kSampler = D3DVERTEXTEXTURESAMPLER0;

        IDirect3DDevice9*  g_device = nullptr;
        IDirect3DTexture9* g_texture = nullptr;
        int                g_height = 0;      // rows of the texture as created
        int                g_entryRows = 0;   // of those, the cells' entries (after the header row)
        unsigned           g_gridWritten = ~0u; // the grid generation the entry rows hold
        bool               g_failed = false;
        bool               g_active = false;  // header valid this frame
        bool               g_gridOn = false;  // entries and pool filled this frame
        unsigned           g_count = 0;       // lights in the pool
        bool               g_stockOff = false;
        unsigned           g_patched = 0;
        int                g_debug = 0;       // 0 off, 1 ours only, 2 count check, 3 world stripes, 4 mark WMO baked surfaces
        unsigned           g_marked = 0;      // baked-colour WMO variants carrying the marker
        unsigned           g_normalsGiven = 0; // of those, shadow-tier-0 variants given a normal input
        // The vertex-set name of the shader being patched: the rule's membership test runs right
        // before its edit for the same wrapper (shaderpatch::CreateWithTableRules), so the edit can
        // tell WMO families from M2 ones that look alike in text.
        std::string        g_family;
        std::string        g_status;

        // Probe (debug): at each M2 batch draw (OnM2BatchDraw, right after the native draw) check
        // that vertex sampler 0 still holds our texture.
        int         g_probe = 0;
        unsigned    g_probeDraws = 0, g_probeMismatch = 0, g_lastDraws = 0, g_lastMismatch = 0;
        DWORD       g_probeTick = 0;
        std::string g_probeLine = "probe off";

        void __cdecl OnM2BatchDraw(void* /*user*/, const void* args)
        {
            if (!g_probe) return;
            const auto* a = static_cast<const wxl::events::M2BatchDrawArgs*>(args);
            auto* device = static_cast<IDirect3DDevice9*>(a ? a->device : nullptr);
            if (!device) return;
            ++g_probeDraws;
            IDirect3DBaseTexture9* bound = nullptr;
            device->GetTexture(kSampler, &bound);
            if (bound != static_cast<IDirect3DBaseTexture9*>(g_texture)) ++g_probeMismatch;
            if (bound) bound->Release();
        }

        void Release()
        {
            if (g_texture) g_texture->Release();
            g_texture = nullptr;
            g_device = nullptr;
            g_height = 0;
        }

        int EntryRows(const lights::Grid& g)
        {
            const int entries = g.cells * g.cells * g.perCell;
            return (entries + kWidth - 1) / kWidth;
        }

        // Sized for the current grid settings; recreated when they change.
        bool EnsureTexture(IDirect3DDevice9* device)
        {
            if (device != g_device) { Release(); g_failed = false; g_device = device; }
            const int entryRows = EntryRows(lights::CellGrid());
            const int height = 1 + entryRows + kPoolRows + kOccRows;
            if (g_texture && height == g_height) return true;
            if (g_texture) { g_texture->Release(); g_texture = nullptr; g_failed = false; }
            if (g_failed) return false;
            if (FAILED(device->CreateTexture(kWidth, height, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &g_texture, nullptr)))
            {
                g_texture = nullptr;
                g_failed = true;
                return false;
            }
            g_height = height;
            g_entryRows = entryRows;
            g_gridWritten = ~0u;
            g_occWritten.clear();
            return true;
        }

        void Bind(IDirect3DDevice9* device)
        {
            device->SetTexture(kSampler, g_texture);
            if (!g_texture) return;
            device->SetSamplerState(kSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }

        // Anchors (all M2/WMO vertex families, dumps 2026-10-05). Each value is captured where the
        // stock code uses it, which holds whatever order the compiler scheduled things in:
        //  - the position (view space): the register the projection reads, `dp4 oN.x, c2, rP`;
        //  - the normal: the register the sun term reads, `dp3_sat rX.c, -c12, rN` (only lit
        //    variants have it; the rest are unlit materials and stay stock);
        //  - the light sum: what the colour write reads, `mad_sat oK, <material>, rL, c29` (M2: a
        //    copy of c28; WMO and CDiffuse: the vertex colour) or `add_sat oK, rL, c29` (WMO
        //    composite).
        const std::regex kPosition(R"(^\s*dp4 o\d+\.x, c2, (r\d+)\s*$)");
        const std::regex kSun(R"(^\s*dp3_sat r\d+\.[xyzw], -c12, (r\d+)\s*$)");
        const std::regex kColour(R"(^\s*(?:mad_sat o\d+(?:\.xyz)?, [rv]\d+, (r\d+), c29|add_sat o\d+(?:\.xyz)?, (r\d+), c29)\s*$)");

        // c1 = (header u of t0, v of row 0, 1 / W: one texel along the row, 0).
        std::string Prologue()
        {
            char d[160];
            std::snprintf(d, sizeof(d), "    def c1, %.9g, 0, %.9g, 0\n    defi i0, %d, 0, 0, 0\n    dcl_2d s0\n",
                          0.5 / kWidth, 1.0 / kWidth, lights::kMaxPerCellCap);
            return d;
        }

        // Before the loop: the vertex's cell. Position r29 (view) -> camera-relative -> world (the
        // world-stripes debug view's transform, confirmed in-client), minus the grid's corner (t11 in
        // r20), in cells, floored, clamped to the grid (a light past its radius adds 0, so a clamped
        // cell is only wasted work); r28.y = its first entry (cell x lights per cell).
        std::string CellSetup()
        {
            return
                "    mov r17.x, r13.w\n"
                "    mov r17.y, r14.w\n"
                "    mov r17.z, r15.w\n"
                "    add r17.xyz, r29, -r17\n"
                "    mul r18.xyz, r17.x, r13\n"
                "    mad r18.xyz, r17.y, r14, r18\n"
                "    mad r18.xyz, r17.z, r15, r18\n"
                "    add r18.xy, r18, r16\n"
                "    add r18.xy, r18, -r20\n"
                "    mul r18.xy, r18, r20.z\n"
                "    frc r19.xy, r18\n"
                "    add r18.xy, r18, -r19\n"
                "    max r18.xy, r18, r10.y\n"
                "    add r19.x, r20.w, -r10.x\n"
                "    min r18.xy, r18, r19.x\n"
                "    mad r18.x, r18.y, r20.w, r18.x\n"
                "    mul r28.y, r18.x, r9.z\n";
        }

        // Baked occlusion (lights::Bake), inside the loop once the light's strength r20.x is known: the
        // direction from the light to this vertex, back to world space (view columns r13..r15, the
        // world-stripes transform), on the octahedral map (n = d / |d|1, lower half folded), its cell
        // of 8 x 8, the tile's texel (4 cells each) and component; a vertex farther than the stored
        // reach (+ 0.5 yd, soft over 0.5 yd) gets none of this light. 4th texel r23.y = tile / tiles
        // per row (-1 = none), r16.w = the region's first row + 0.5.
        std::string OcclusionVS()
        {
            return
                "      if_ge r23.y, c1.w\n"
                "      mul r17.xyz, r26.x, r13\n"
                "      mad r17.xyz, r26.y, r14, r17\n"
                "      mad r17.xyz, r26.z, r15, r17\n"
                "      add r18.x, r17_abs.x, r17_abs.y\n"
                "      add r18.x, r18.x, r17_abs.z\n"
                "      rcp r18.x, r18.x\n"
                "      mul r18.xyz, -r17, r18.x\n"
                "      sge r19.xy, r18, c1.w\n"
                "      add r19.xy, r19, r19\n"
                "      add r19.xy, r19, -r10.x\n"
                "      add r19.zw, r10.x, -r18_abs.xxyx\n"
                "      mul r19.xy, r19, r19.zw\n"
                "      add r19.xy, r19, -r18\n"
                "      slt r19.z, r18.z, c1.w\n"
                "      mad r18.xy, r19, r19.z, r18\n"
                "      rcp r19.w, r12.z\n"
                "      add r18.xy, r18, r10.x\n"
                "      mul r18.xy, r18, r19.w\n"
                "      add r19.z, r19.w, r19.w\n"
                "      add r19.z, r19.z, -r12.y\n"
                "      min r18.xy, r18, r19.z\n"
                "      frc r19.xy, r18\n"
                "      add r18.xy, r18, -r19\n"
                "      mul r19.x, r18.x, r12.z\n"
                "      frc r19.y, r19.x\n"
                "      add r19.x, r19.x, -r19.y\n"
                "      mul r19.y, r19.y, r19.w\n"
                "      add r18.y, r18.y, r18.y\n"
                "      add r19.x, r19.x, r18.y\n"
                "      frc r18.z, r23.y\n"
                "      add r18.w, r23.y, -r18.z\n"
                "      add r19.x, r19.x, r10.z\n"
                "      mad r27.x, r19.x, r9.x, r18.z\n"
                "      add r18.w, r18.w, r16.w\n"
                "      mul r27.y, r18.w, r9.y\n"
                "      texldl r18, r27, s0\n"
                "      add r17.x, r19.y, -r10.y\n"
                "      add r17.y, r19.y, -r10.x\n"
                "      add r17.z, r17.y, -r10.x\n"
                "      add r17.w, r17.z, -r10.x\n"
                "      slt r17, r17_abs, r10.z\n"
                "      dp4 r17.x, r18, r17\n"
                "      add r17.x, r17.x, -r22.w\n"
                "      add r17.x, r17.x, r17.x\n"
                "      add_sat r17.x, r17.x, r10.x\n"
                "      mul r20.x, r20.x, r17.x\n"
                "      endif\n";
        }

        // Lit WMO surfaces are exteriors (interiors draw with the baked-light variants): a light inside
        // an indoor group (4th texel r23.w = 1, set only while the panel's switch is on) stays off them,
        // like off the terrain, so a chimney or cellar torch doesn't light the outside walls.
        std::string IndoorSkip()
        {
            return
                "      add r23.w, r10.x, -r23.w\n"
                "      mul r20.x, r20.x, r23.w\n";
        }

        // At the top of the loop (counter r28.z): this cell's entry r28.y + r28.z at texel (column,
        // 1 + row) of the W-wide rows; an empty entry ends the list; r27.xy = the light's first texel.
        std::string EntryFetch()
        {
            return
                "      break_ge r28.z, r9.z\n"
                "      add r17.x, r28.y, r28.z\n"
                "      mul r17.x, r17.x, r9.x\n"
                "      frc r17.y, r17.x\n"
                "      add r17.z, r17.x, -r17.y\n"
                "      add r27.x, r17.y, c1.x\n"
                "      add r17.z, r17.z, r10.x\n"
                "      add r17.z, r17.z, r10.z\n"
                "      mul r27.y, r17.z, r9.y\n"
                "      texldl r26, r27, s0\n"
                "      break_lt r26.z, r10.z\n"
                "      mov r27.xy, r26\n";
        }

        // Before the colour write, inside `if valid`: the header, then our lights summed into r31
        // from the position r29 and the normal r30 (the terrain's and cover's light: size ~1 yd,
        // 1 / (0.7 s + 0.03 s^2) on s = sqrt(d^2 + 1), windowed to 0 at the radius, wrapped Lambert,
        // spot cone), each moved world -> camera-relative -> view first. Then folded into the stock
        // light sum `l` like on the terrain: t = l + ours; above a knee (max(0.8, brightest channel
        // of l)) the brightest channel of t is compressed smoothly and all three scaled alike, so
        // the hue stays. Last, the debug views (weights from t5): ours only, this cell's count / its
        // capacity in red, world stripes frac(xy / 4). Temps r9..r31 are free here (stock uses up to
        // r8; r9..r12 held the switched stock colours, read before this point).
        // `extra` goes in the loop right after the light's strength (r20.x) is known: IndoorSkip() on
        // WMO surfaces, else empty.
        std::string LampBlock(const std::string& l, const std::string& extra)
        {
            return std::string() +
                "    mov r27, c1\n"
                "    texldl r28, r27, s0\n"
                "    if_gt r28.w, c1.w\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r16, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r13, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r14, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r15, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r9, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r10, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r11, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r12, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r20, r27, s0\n"
                + CellSetup() +
                "    mov r31.xyz, r10.y\n"
                "    mov r28.z, r10.y\n"
                "    mov r27, c1\n"
                "    rep i0\n"
                + EntryFetch() +
                "      texldl r26, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r25, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r24, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r23, r27, s0\n"
                "      add r19.xyz, r26, -r16\n"
                "      mov r19.w, r10.x\n"
                "      dp4 r18.x, r19, r13\n"
                "      dp4 r18.y, r19, r14\n"
                "      dp4 r18.z, r19, r15\n"
                "      add r26.xyz, r18, -r29\n"
                "      dp3 r17.x, r24, r13\n"
                "      dp3 r17.y, r24, r14\n"
                "      dp3 r17.z, r24, r15\n"
                "      mov r24.xyz, r17\n"
                "      dp3 r22.x, r26, r26\n"
                "      add r22.y, r22.x, r10.x\n"
                "      rsq r22.z, r22.x\n"
                "      rcp r22.w, r22.z\n"
                "      mul_sat r21.x, r22.w, r26.w\n"
                "      mad r21.x, r21.x, -r21.x, r10.x\n"
                "      mul r21.x, r21.x, r21.x\n"
                "      pow r21.x, r21.x, r25.w\n"
                "      mul r26.xyz, r26, r22.z\n"
                "      dp3 r21.y, r30, r26\n"
                "      add r21.y, r21.y, r10.z\n"
                "      mul_sat r21.y, r21.y, r10.w\n"
                "      dp3 r21.w, -r26, r24\n"
                "      add r21.w, r21.w, -r24.w\n"
                "      mul_sat r21.w, r21.w, r23.x\n"
                "      mul r21.y, r21.y, r21.w\n"
                "      rsq r21.z, r22.y\n"
                "      rcp r21.w, r21.z\n"
                "      mad r20.x, r21.w, r11.y, r11.x\n"
                "      mul r20.x, r20.x, r21.w\n"
                "      rcp r20.x, r20.x\n"
                "      mul r20.x, r20.x, r21.x\n"
                "      mul r20.x, r20.x, r21.y\n"
                + OcclusionVS() + extra +
                "      mad r31.xyz, r25, r20.x, r31\n"
                "      add r28.z, r28.z, r10.x\n"
                "    endrep\n"
                "    mov r17.x, r13.w\n"
                "    mov r17.y, r14.w\n"
                "    mov r17.z, r15.w\n"
                "    add r17.xyz, r29, -r17\n"
                "    mul r18.xyz, r17.x, r13\n"
                "    mad r18.xyz, r17.y, r14, r18\n"
                "    mad r18.xyz, r17.z, r15, r18\n"
                "    add r18.xyz, r18, r16\n"
                "    mul r18.xy, r18, r12.z\n"
                "    frc r18.xy, r18\n"
                "    mov r18.z, r10.y\n"
                "    mul r19.x, r28.z, r12.w\n"
                "    mov r19.yz, r10.y\n"
                "    mov r27, c1\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r20, r27, s0\n"
                "    add r21.xyz, " + l + ", r31\n"
                "    max r22.x, r21.x, r21.y\n"
                "    max r22.x, r22.x, r21.z\n"
                "    max r22.y, " + l + ".x, " + l + ".y\n"
                "    max r22.y, r22.y, " + l + ".z\n"
                "    max r22.y, r22.y, r12.x\n"
                "    add r22.z, r22.x, -r22.y\n"
                "    max r22.z, r22.z, r10.y\n"
                "    add r22.w, r10.x, -r22.y\n"
                "    max r22.w, r22.w, r12.y\n"
                "    rcp r23.x, r22.w\n"
                "    mul r23.y, r22.z, r23.x\n"
                "    mul r23.y, r23.y, -r11.z\n"
                "    exp r23.y, r23.y\n"
                "    add r23.y, r10.x, -r23.y\n"
                "    mul r23.y, r23.y, r22.w\n"
                "    add r23.z, r22.x, -r22.z\n"
                "    add r23.z, r23.z, r23.y\n"
                "    max r23.w, r22.x, r12.y\n"
                "    rcp r23.w, r23.w\n"
                "    mul r23.z, r23.z, r23.w\n"
                "    mul " + l + ".xyz, r21, r23.z\n"
                "    add r24.x, r10.x, -r20.w\n"
                "    mul " + l + ".xyz, " + l + ", r24.x\n"
                "    mad " + l + ".xyz, r31, r20.x, " + l + "\n"
                "    mad " + l + ".xyz, r19, r20.y, " + l + "\n"
                "    mad " + l + ".xyz, r18, r20.z, " + l + "\n"
                "    endif\n"
                ;
        }

        // At the first instruction: the stock point lights' colours (c17..c20, renamed r9..r12),
        // scaled by 1 - t0.x. An unbound or empty texture reads 0, i.e. the stock lights stay on.
        std::string StockSwitch()
        {
            std::string s = "    mov r16, c1\n    texldl r16, r16, s0\n";
            for (int k = 0; k < 4; ++k)
            {
                const std::string r = "r" + std::to_string(9 + k);
                s += "    mov " + r + ", c" + std::to_string(17 + k) + "\n    mad " + r + ".xyz, " + r + ", -r16.x, " + r + "\n";
            }
            return s;
        }

        // Where the first instruction starts (after the version line, declarations and definitions).
        std::string::size_type FirstInstruction(const std::string& src)
        {
            std::string::size_type at = src.find('\n');
            while (at != std::string::npos && at + 1 < src.size())
            {
                const std::string::size_type end = src.find('\n', at + 1);
                const std::string line = src.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos && line.compare(first, 3, "dcl") != 0 && line.compare(first, 3, "def") != 0 && line.compare(first, 2, "//") != 0)
                    return at + 1;
                at = end;
            }
            return std::string::npos;
        }

        // Every line matching `re`: its start offset and first non-empty capture (empty when the
        // pattern has no groups).
        struct Hit { std::string::size_type at; std::string reg; };
        std::vector<Hit> Lines(const std::string& src, const std::regex& re)
        {
            std::vector<Hit> out;
            std::string::size_type at = 0;
            while (at < src.size())
            {
                std::string::size_type end = src.find('\n', at);
                if (end == std::string::npos) end = src.size();
                const std::string line = src.substr(at, end - at);
                std::smatch m;
                if (std::regex_match(line, m, re))
                {
                    std::string reg;
                    for (size_t g = 1; g < m.size(); ++g)
                        if (m[g].matched) { reg = m[g].str(); break; }
                    out.push_back({ at, reg });
                }
                at = end + 1;
            }
            return out;
        }

        // WMO surfaces with baked light: the WMO variants without a sun term pass the baked vertex
        // colour straight on (`mov oColour, vColour`, all 315 in the dumps). Interior groups draw with
        // them (owner's bind probe + marker, Deadmines 2026-10-06). The baked colour already holds the
        // WMO's own torches, so ours FILL UP to it instead of stacking (owner + design 2026-10-06):
        //  - the same light loop as on lit surfaces, the normal from c31..c33 (the variants without a
        //    normal get distance-only light);
        //  - the baked light (the vertex colour, half scale: the pixel shader doubles it) dips with each
        //    flame's flicker by that flame's share of the spot's brightness (its light / the baked);
        //  - per channel, ours (x the panel's scale, halved) adds only what exceeds the baked light, so
        //    an orange torch over its own orange glow adds ~nothing, a dark corner or another colour gets
        //    it in full;
        //  - the terrain's knee on the brightest channel, then debug views (ours only, the t0.z marker,
        //    the t0.y normal check).
        // Normals: the shadow-tier-0 variants (indices 0..28 of each family) declare none, while the same
        // families at tiers 1/2 read one from the same WMO geometry, so the vertex data has it: those
        // get it declared (`injected`). A normal that reads as zero (data without one after all) leaves
        // that surface on distance-only light: r9.w = 1 when the normal is usable.
        std::string BakedBlock(const std::string& vc, const std::string& oc, const std::string& vn, bool injected)
        {
            std::string b = std::string() +
                "    mov r27, c1\n"
                "    texldl r28, r27, s0\n"
                "    if_gt r28.w, c1.w\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r16, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r13, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r14, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r15, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r9, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r10, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r11, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r12, r27, s0\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r20, r27, s0\n"
                "";
            if (!vn.empty()) b += std::string() +
                "    dp3 r30.x, c31, " + vn + "\n"
                "    dp3 r30.y, c32, " + vn + "\n"
                "    dp3 r30.z, c33, " + vn + "\n"
                "    dp3 r17.w, r30, r30\n"
                "    slt r9.w, r12.y, r17.w\n"
                "    max r17.w, r17.w, r12.y\n"
                "    rsq r17.w, r17.w\n"
                "    mul r30.xyz, r30, r17.w\n"
                "";
            b += std::string() +
                "    dp3 r30.w, " + vc + ", r10.x\n"
                "    add r30.w, r30.w, r30.w\n"
                "    add r30.w, r30.w, r12.y\n"
                "    rcp r29.w, r30.w\n"
                + CellSetup() +
                "    mov r31.xyz, r10.y\n"
                "    mov r28.x, r10.y\n"
                "    mov r28.z, r10.y\n"
                "    mov r27, c1\n"
                "    rep i0\n"
                + EntryFetch() +
                "      texldl r26, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r25, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r24, r27, s0\n"
                "      add r27.x, r27.x, r9.x\n"
                "      texldl r23, r27, s0\n"
                "      add r19.xyz, r26, -r16\n"
                "      mov r19.w, r10.x\n"
                "      dp4 r18.x, r19, r13\n"
                "      dp4 r18.y, r19, r14\n"
                "      dp4 r18.z, r19, r15\n"
                "      add r26.xyz, r18, -r29\n"
                "      dp3 r17.x, r24, r13\n"
                "      dp3 r17.y, r24, r14\n"
                "      dp3 r17.z, r24, r15\n"
                "      mov r24.xyz, r17\n"
                "      dp3 r22.x, r26, r26\n"
                "      add r22.y, r22.x, r10.x\n"
                "      rsq r22.z, r22.x\n"
                "      rcp r22.w, r22.z\n"
                "      mul_sat r21.x, r22.w, r26.w\n"
                "      mad r21.x, r21.x, -r21.x, r10.x\n"
                "      mul r21.x, r21.x, r21.x\n"
                "      pow r21.x, r21.x, r25.w\n"
                "      mul r26.xyz, r26, r22.z\n"
                "";
            if (!vn.empty()) b += std::string() +
                "      dp3 r21.y, r30, r26\n"
                "      add r21.y, r21.y, r10.z\n"
                "      mul_sat r21.y, r21.y, r10.w\n"
                "      add r21.y, r21.y, -r10.x\n"
                "      mad r21.y, r21.y, r9.w, r10.x\n"
                "";
            else b += std::string() +
                "      mov r21.y, r10.x\n"
                "";
            b += std::string() +
                "      dp3 r21.w, -r26, r24\n"
                "      add r21.w, r21.w, -r24.w\n"
                "      mul_sat r21.w, r21.w, r23.x\n"
                "      mul r21.y, r21.y, r21.w\n"
                "      rsq r21.z, r22.y\n"
                "      rcp r21.w, r21.z\n"
                "      mad r20.x, r21.w, r11.y, r11.x\n"
                "      mul r20.x, r20.x, r21.w\n"
                "      rcp r20.x, r20.x\n"
                "      mul r20.x, r20.x, r21.x\n"
                "      mul r20.x, r20.x, r21.y\n"
                + OcclusionVS() +
                "      mul r19.xyz, r25, r20.x\n"
                "      add r31.xyz, r31, r19\n"
                "      dp3 r19.w, r19, r10.x\n"
                "      mul_sat r19.w, r19.w, r29.w\n"
                "      mad r28.x, r23.z, r19.w, r28.x\n"
                "      add r28.z, r28.z, r10.x\n"
                "    endrep\n"
                "    min r28.x, r28.x, r10.x\n"
                "    add r28.x, r10.x, -r28.x\n"
                "    mul r17.xyz, " + vc + ", r28.x\n"
                "    mov r27, c1\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r22, r27, s0\n"
                "    mul r18.xyz, r31, r22.x\n"
                "    mul r18.xyz, r18, r10.z\n"
                "    add r21.xyz, r18, -r17\n"
                "    max r21.xyz, r21, r10.y\n"
                "    add r21.xyz, r17, r21\n"
                "    max r22.x, r21.x, r21.y\n"
                "    max r22.x, r22.x, r21.z\n"
                "    max r22.y, r17.x, r17.y\n"
                "    max r22.y, r22.y, r17.z\n"
                "    max r22.y, r22.y, r12.x\n"
                "    add r22.z, r22.x, -r22.y\n"
                "    max r22.z, r22.z, r10.y\n"
                "    add r22.w, r10.x, -r22.y\n"
                "    max r22.w, r22.w, r12.y\n"
                "    rcp r23.x, r22.w\n"
                "    mul r23.y, r22.z, r23.x\n"
                "    mul r23.y, r23.y, -r11.z\n"
                "    exp r23.y, r23.y\n"
                "    add r23.y, r10.x, -r23.y\n"
                "    mul r23.y, r23.y, r22.w\n"
                "    add r23.z, r22.x, -r22.z\n"
                "    add r23.z, r23.z, r23.y\n"
                "    max r23.w, r22.x, r12.y\n"
                "    rcp r23.w, r23.w\n"
                "    mul r23.z, r23.z, r23.w\n"
                "    mul r17.xyz, r21, r23.z\n"
                "    mov r27, c1\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    add r27.x, r27.x, r27.z\n"
                "    texldl r20, r27, s0\n"
                "    add r24.x, r10.x, -r20.x\n"
                "    mul r17.xyz, r17, r24.x\n"
                "    mad r17.xyz, r18, r20.x, r17\n"
                "    mov r17.w, " + vc + ".w\n"
                "    mov r27, c1\n"
                "    texldl r28, r27, s0\n"
                "    mad r17.yz, r17, -r28.z, r17\n"
                "    if_gt r28.y, c1.w\n"
                "";
            // Normal check (debug view): blue = its own normal, green = given one (injected), red = none
            // usable (distance-only light). Half scale, like the baked colour.
            if (vn.empty()) b += std::string() +
                "    mov r17.x, r10.x\n"
                "    mov r17.yz, r10.y\n"
                "";
            else if (injected) b += std::string() +
                "    add r17.x, r10.x, -r9.w\n"
                "    mov r17.y, r9.w\n"
                "    mov r17.z, r10.y\n"
                "";
            else b += std::string() +
                "    mov r17.xy, r10.y\n"
                "    mov r17.z, r10.x\n"
                "";
            b += std::string() +
                "    mul r17.xyz, r17, r10.z\n"
                "    endif\n"
                "    else\n"
                "    mov r17, " + vc + "\n"
                "    endif\n"
                "    mov " + oc + ", r17\n"
                "";
            return b;
        }

        bool BakedRule(std::string& src, std::string& why)
        {
            why = "unlit (no sun term)";
            if (g_family.compare(0, 6, "MapObj") != 0) return false;
            static const std::regex colourOut(R"(dcl_color (o\d+))"), colourIn(R"(dcl_color (v\d+))"), normalIn(R"(dcl_normal (v\d+))");
            std::smatch o, v, n;
            if (!std::regex_search(src, o, colourOut) || !std::regex_search(src, v, colourIn)) return false;
            const std::string oc = o[1].str(), vc = v[1].str();
            std::string vn = std::regex_search(src, n, normalIn) ? n[1].str() : std::string();
            const std::vector<Hit> copy = Lines(src, std::regex("^\\s*mov " + oc + ", " + vc + "\\s*$"));
            const std::vector<Hit> pos = Lines(src, kPosition);
            if (copy.size() != 1 || pos.size() != 1) { why = "baked: anchors"; return false; }
            if (pos[0].at > copy[0].at) { why = "baked: position after colour"; return false; }
            // No normal declared (shadow tier 0): declare one on the next free input register.
            bool injected = false;
            std::string declareNormal;
            if (vn.empty())
            {
                static const std::regex input(R"(dcl_\w+ v(\d+))");
                int highest = -1;
                for (std::sregex_iterator it(src.begin(), src.end(), input), stop; it != stop; ++it)
                    highest = std::max(highest, std::stoi((*it)[1].str()));
                if (highest + 1 <= 15)
                {
                    vn = "v" + std::to_string(highest + 1);
                    declareNormal = "    dcl_normal " + vn + "\n";
                    injected = true;
                    ++g_normalsGiven;
                }
            }
            std::string::size_type end = src.find('\n', copy[0].at);
            end = end == std::string::npos ? src.size() : end + 1;
            src.replace(copy[0].at, end - copy[0].at, BakedBlock(vc, oc, vn, injected));
            src.insert(pos[0].at, "    mov r29.xyz, " + pos[0].reg + "\n");
            src.insert(src.find('\n') + 1, Prologue() + declareNormal);
            ++g_marked;
            return true;
        }

        bool EditVertex(std::string& src, std::string& why)
        {
            if (src.find("vs_3_0") == std::string::npos) { why = "not vs_3_0"; return false; }
            static const std::regex clash(R"(\b(i0|aL|c1|r9|r1[0-9]|r2[0-9]|r3[01])\b)");
            if (std::regex_search(src, clash)) { why = "uses a register the patch needs"; return false; }
            const std::vector<Hit> sun = Lines(src, kSun);
            if (sun.empty()) return BakedRule(src, why);
            const std::vector<Hit> pos = Lines(src, kPosition), colour = Lines(src, kColour);
            if (pos.size() != 1 || sun.size() != 1 || colour.size() != 1)
            {
                char b[96];
                std::snprintf(b, sizeof(b), "anchors: position %u, sun %u, colour %u",
                              static_cast<unsigned>(pos.size()), static_cast<unsigned>(sun.size()), static_cast<unsigned>(colour.size()));
                why = b;
                return false;
            }
            // Position and normal must be captured before the colour write reads our sum; their own order
            // varies (4 M2 variants compute the sun term first). Inserted from the bottom up, so the
            // earlier offsets stay valid.
            if (!(pos[0].at < colour[0].at && sun[0].at < colour[0].at)) { why = "anchors out of order"; return false; }
            const bool wmo = g_family.compare(0, 6, "MapObj") == 0;
            src.insert(colour[0].at, LampBlock(colour[0].reg, wmo ? IndoorSkip() : std::string()));
            const std::string savePos = "    mov r29.xyz, " + pos[0].reg + "\n", saveNormal = "    mov r30.xyz, " + sun[0].reg + "\n";
            if (sun[0].at > pos[0].at) { src.insert(sun[0].at, saveNormal); src.insert(pos[0].at, savePos); }
            else                       { src.insert(pos[0].at, savePos); src.insert(sun[0].at, saveNormal); }

            static const std::regex stockColour(R"(\bc(17|18|19|20)\b)");
            if (std::regex_search(src, stockColour))
            {
                for (int k = 0; k < 4; ++k)
                    src = std::regex_replace(src, std::regex("\\bc" + std::to_string(17 + k) + "\\b"), "r" + std::to_string(9 + k));
                const std::string::size_type at = FirstInstruction(src);
                if (at == std::string::npos) { why = "no first instruction"; return false; }
                src.insert(at, StockSwitch());
            }
            src.insert(src.find('\n') + 1, Prologue());
            ++g_patched;
            return true;
        }

        // Rows [first, first + count) locked; nullptr on failure. Managed textures upload only the
        // locked rectangle, so the cell entries go up only when the grid changed.
        float* LockRows(int first, int count, int& pitchFloats)
        {
            D3DLOCKED_RECT lr{};
            const RECT r{ 0, first, kWidth, first + count };
            if (!g_texture || count <= 0 || FAILED(g_texture->LockRect(0, &lr, &r, 0))) return nullptr;
            pitchFloats = lr.Pitch / static_cast<int>(sizeof(float));
            return static_cast<float*>(lr.pBits);
        }

        int OccRow() { return 1 + g_entryRows + kPoolRows; }

        // The cells' entries and the light pool (shared with the terrain), every frame while our lights
        // are on; the entries only when the grid changed.
        void FillGrid(bool on)
        {
            const lights::Grid& grid = lights::CellGrid();
            const std::vector<lights::ActiveLight>& pool = lights::Active();
            if (!on) { g_count = 0; g_gridOn = false; return; }
            const int poolRow = 1 + g_entryRows;
            int pitch = 0;
            if (g_gridWritten != grid.generation || !g_gridOn)
            {
                if (float* t = LockRows(1, g_entryRows, pitch))
                {
                    for (int row = 0; row < g_entryRows; ++row) std::memset(t + row * pitch, 0, kWidth * 4 * sizeof(float));
                    const size_t cellCount = static_cast<size_t>(grid.cells) * grid.cells;
                    for (size_t c = 0; c < cellCount; ++c)
                        for (int k = 0; k < grid.count[c]; ++k)
                        {
                            const size_t entry = c * grid.perCell + k;
                            const unsigned light = grid.index[entry];
                            float* o = t + (entry / kWidth) * pitch + (entry % kWidth) * 4;
                            const unsigned texel = light * kTexelsPerLight;
                            o[0] = (static_cast<float>(texel % kWidth) + 0.5f) / kWidth;
                            o[1] = (static_cast<float>(poolRow + texel / kWidth) + 0.5f) / g_height;
                            o[2] = 1.0f;
                        }
                    g_texture->UnlockRect(0);
                    g_gridWritten = grid.generation;
                }
            }
            g_count = 0;
            if (float* t = LockRows(poolRow, kPoolRows, pitch))
            {
                for (int row = 0; row < kPoolRows; ++row) std::memset(t + row * pitch, 0, kWidth * 4 * sizeof(float));
                for (const lights::ActiveLight& l : pool)
                {
                    if (g_count >= static_cast<unsigned>(lights::kMaxPool)) break;
                    const unsigned texel = g_count * kTexelsPerLight;
                    float* o = t + (texel / kWidth) * pitch + (texel % kWidth) * 4;
                    o[0] = l.pos[0]; o[1] = l.pos[1]; o[2] = l.pos[2];
                    o[3] = l.radius > 0.0f ? 1.0f / l.radius : 0.0f;
                    o[4] = l.color[0]; o[5] = l.color[1]; o[6] = l.color[2]; o[7] = l.falloff;
                    o[8] = l.spotDir[0]; o[9] = l.spotDir[1]; o[10] = l.spotDir[2]; o[11] = l.cosOuter;
                    o[12] = l.spotScale; o[14] = l.dip;
                    o[13] = l.occlusion ? static_cast<float>(g_count) / kTilesPerRow : -1.0f;
                    o[15] = l.indoor ? 1.0f : 0.0f; // read by the terrain and lit WMO surfaces (indoor lights stay off them)
                    ++g_count;
                }
                g_texture->UnlockRect(0);
            }
            // The tiles: only when a light gained, lost or changed its occlusion.
            bool occChanged = g_occWritten.size() != g_count;
            for (unsigned p = 0; p < g_count && !occChanged; ++p) occChanged = g_occWritten[p] != pool[p].occlusion;
            if (occChanged)
                if (float* t = LockRows(OccRow(), kOccRows, pitch))
                {
                    for (int row = 0; row < kOccRows; ++row) std::memset(t + row * pitch, 0, kWidth * 4 * sizeof(float));
                    g_occWritten.assign(g_count, nullptr);
                    for (unsigned p = 0; p < g_count; ++p)
                    {
                        const float* occ = pool[p].occlusion;
                        g_occWritten[p] = occ;
                        if (!occ) continue;
                        const unsigned first = p * kOccTexels;
                        float* o = t + (first / kWidth) * pitch + (first % kWidth) * 4;
                        std::memcpy(o, occ, lights::kOccCells * sizeof(float));
                    }
                    g_texture->UnlockRect(0);
                }
            g_gridOn = true;
        }

        // Writes the header (row 0): valid when the model lights are on, else all zero (stock).
        void Fill(bool active)
        {
            int pitch = 0;
            float* t = LockRows(0, 1, pitch);
            if (!t) return;
            std::memset(t, 0, kWidth * 4 * sizeof(float));
            float V[16], P[16];
            if (active && (!g_gridOn || !wxl::game::gfx::SceneMatrices(V, P))) active = false;
            if (active)
            {
                const lights::Grid& grid = lights::CellGrid();
                const lights::Settings& cfg = lights::Config();
                g_stockOff = cfg.modelStockOff != 0;
                t[0] = g_stockOff ? 1.0f : 0.0f; t[3] = 1.0f;
                wxl::game::camera::GetPosition(&t[4]);
                t[7] = OccRow() + 0.5f; // t1.w: the occlusion region's first row (+ texel centre)
                for (int col = 0; col < 3; ++col)
                    for (int row = 0; row < 4; ++row) t[(2 + col) * 4 + row] = V[row * 4 + col];
                if (g_debug >= 1 && g_debug <= 3) { t[5 * 4 + (g_debug - 1)] = 1.0f; t[5 * 4 + 3] = 1.0f; }
                if (g_debug == 4) t[2] = 1.0f; // mark the WMO baked-colour surfaces
                if (g_debug == 5) t[1] = 1.0f; // WMO interiors: which normal they light with
                const float perCell = static_cast<float>(grid.perCell);
                const float k[16] = {
                    1.0f / kWidth, 1.0f / g_height, perCell, 0.0f,
                    1.0f, 0.0f, 0.5f, 2.0f / 3.0f,
                    0.7f, 0.03f, 1.44269502f, 0.05f,
                    0.8f, 0.001f, 0.25f, grid.perCell > 0 ? 1.0f / perCell : 0.0f };
                std::memcpy(t + 6 * 4, k, sizeof(k));
                t[10 * 4] = cfg.bakedAdd;
                const float g[4] = { grid.originX, grid.originY, 1.0f / grid.cellSize, static_cast<float>(grid.cells) };
                std::memcpy(t + 11 * 4, g, sizeof(g));
            }
            else g_stockOff = false;
            g_active = active;
            g_texture->UnlockRect(0);
        }
    }

    void Register()
    {
        shaderpatch::TableRule rule;
        rule.name = "model lights (M2/WMO vertex)";
        rule.pixel = false;
        rule.contains = [](const void* wrapper)
        {
            const m2effects::Effect* e = m2effects::FindVertexEffect(wrapper);
            g_family = e ? e->vertexName : std::string();
            return e != nullptr;
        };
        rule.edit = &EditVertex;
        shaderpatch::Register(std::move(rule));
    }

    bool Prepare(IDirect3DDevice9* device, bool gridOn)
    {
        if (!device || !EnsureTexture(device)) { g_gridOn = false; g_active = false; return false; }
        const lights::Settings& cfg = lights::Config();
        FillGrid(gridOn && cfg.enabled && lights::CellGrid().cells > 0);
        Fill(cfg.enabled && cfg.models);
        Bind(device);
        return g_gridOn;
    }

    IDirect3DTexture9* Texture() { return g_texture; }

    void GridConstants(float c206[4], float c212[4])
    {
        const lights::Grid& grid = lights::CellGrid();
        const bool on = g_gridOn && grid.cells > 0;
        c206[0] = grid.originX; c206[1] = grid.originY;
        c206[2] = 1.0f / grid.cellSize; c206[3] = on ? static_cast<float>(grid.cells) : 0.0f;
        c212[0] = g_height > 0 ? 1.0f / g_height : 0.0f;
        c212[1] = on ? static_cast<float>(grid.perCell) : 0.0f;
        c212[2] = on ? static_cast<float>(grid.cells - 1) : 0.0f;
        c212[3] = OccRow() + 0.5f;
    }

    unsigned TextureBytes() { return g_texture ? static_cast<unsigned>(kWidth) * g_height * 16u : 0u; }

    void Rebind(IDirect3DDevice9* device)
    {
        if (device && g_texture) Bind(device);
    }

    void EndFrame()
    {
        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device || !EnsureTexture(device)) return;
        if (g_active) Fill(false); // stock lighting until the next Prepare (and on screens without a world)
        Bind(device);
    }

    const char* StatusLine()
    {
        char line[512];
        std::snprintf(line, sizeof(line), "model lights (M2/WMO): %s, %u lights in the shared texture (%u x %d texels, %.0f KB); the client's up to 4 per model %s; %u lit and %u baked-light (WMO interior) vertex shader variant(s) patched so far (%u of those given a normal input)%s",
                      g_active ? "on" : "off", g_count, static_cast<unsigned>(kWidth), g_height, TextureBytes() / 1024.0,
                      g_stockOff ? "switched off" : "on", g_patched, g_marked, g_normalsGiven,
                      g_failed ? " -- the light texture (A32B32G32R32F) couldn't be created" : "");
        g_status = line;
        return g_status.c_str();
    }

    int& DebugView() { return g_debug; }

    int& Probe() { return g_probe; }

    void InstallProbe(const WXL_Api* api)
    {
        api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnM2BatchDraw), &OnM2BatchDraw, nullptr);
    }

    const char* ProbeLine()
    {
        const DWORD now = GetTickCount();
        if (now - g_probeTick >= 1000)
        {
            g_probeTick = now;
            g_lastDraws = g_probeDraws; g_lastMismatch = g_probeMismatch;
            g_probeDraws = g_probeMismatch = 0;
            char line[160];
            if (g_probe)
                std::snprintf(line, sizeof(line), "probe (last second): %u model draws; vertex sampler 0 not our light texture at %u",
                              g_lastDraws, g_lastMismatch);
            else std::snprintf(line, sizeof(line), "probe off");
            g_probeLine = line;
        }
        return g_probeLine.c_str();
    }
}
