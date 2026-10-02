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
        // 0.25 yd cells; the mesh is 4 patches of 160x160 cells (161x161 vertices, 16-bit indices)
        // = 320 cells = 80 yd. The textures are toroidal, one texel per world cell; 384 > 321 so a
        // slot is never shared by two vertices of the grid at once.
        constexpr float kCell = 0.25f;
        constexpr int   kPatchCells = 160, kPatchVerts = kPatchCells + 1, kPatches = 2;
        constexpr int   kGridCells = kPatchCells * kPatches, kGridVerts = kGridCells + 1;
        constexpr int   kTex = 384;
        constexpr int   kRecentreCells = 16;      // recentre after the player moved 4 yd
        constexpr int   kRefreshRowsPerFrame = 8; // re-sample rows round-robin (late-streamed chunks)
        constexpr float kHole = -100000.0f;       // base height of a hole / unloaded spot

        // TerrainType storage (WowClientDB 0xAD4C34): a row's Flags at +0x14, 0x1 = footprints.
        constexpr uintptr_t kTerrainMinId = 0x00AD4C44, kTerrainMaxId = 0x00AD4C40, kTerrainIndex = 0x00AD4C54;
        constexpr size_t    kTerrainFlags = 0x14;

        enum class Coverage : int { Everywhere = 0, FootprintTypes = 1 };

        struct Settings
        {
            int   enabled = 0;
            int   coverage = static_cast<int>(Coverage::FootprintTypes);
            float depth = 0.35f;        // yd of cover at full strength
            float rim = 0.3f;           // rim height as a share of the depth
            float relaxSeconds = 30.0f; // trench back to flat
            int   stamp = 1;
            float stampScale = 1.0f;    // trench radius x the unit's size
            float fadeStart = 32.0f, fadeEnd = 39.5f; // radius from the grid centre, yd
            int   wireframe = 0;
        };

        const WXL_Api* g_api = nullptr;
        Settings       g_settings;

        // CPU state, slot = (world cell mod kTex).
        std::vector<float>   g_base(kTex * kTex, kHole);
        std::vector<uint8_t> g_mask(kTex * kTex, 0);
        std::vector<float>   g_press(kTex * kTex, 0.0f);
        std::vector<float>   g_rimH(kTex * kTex, 0.0f);
        bool g_haveGrid = false;
        int  g_gridI = 0, g_gridJ = 0; // world cell index of the grid's first vertex (x, y)
        int  g_refreshRow = 0;
        bool g_baseDirty = true;
        int  g_lastCoverage = -1;

        // stats
        double   g_fillMs = 0, g_firstFillMs = 0, g_simMs = 0, g_uploadMs = 0, g_drawMs = 0;
        unsigned g_holes = 0, g_stamps = 0;
        const char* g_inactive = "not started";

        // --- drawing in step with the world (verified in-client 2026-10-02) ----------------------
        // The cover draws at OnWorldSceneEnd, after the world pass restored its own state, so it has to
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

        // GPU state (all managed, so a device reset keeps it; a new device drops it).
        IDirect3DDevice9*            g_device = nullptr;
        IDirect3DVertexBuffer9*      g_vb = nullptr;
        IDirect3DIndexBuffer9*       g_ib = nullptr;
        IDirect3DVertexDeclaration9* g_decl = nullptr;
        IDirect3DVertexShader9*      g_vs = nullptr;
        IDirect3DPixelShader9*       g_ps = nullptr;
        IDirect3DTexture9*           g_baseTex = nullptr;
        IDirect3DTexture9*           g_coverTex = nullptr;
        bool                         g_gpuFailed = false;

        // VS: c0..c3 view-projection columns; c4 = (patch first cell i, j, cell size, 1 / texture size);
        // c5 = (grid centre x, y, fade start, fade end); c6 = the camera position (the scene is drawn
        // about the camera, see OnWorldSceneEnd).
        const char* kVsHlsl = R"(
float4 vp0 : register(c0); float4 vp1 : register(c1); float4 vp2 : register(c2); float4 vp3 : register(c3);
float4 grid : register(c4);
float4 centre : register(c5);
float4 eye : register(c6);
sampler2D baseTex : register(s0);
sampler2D coverTex : register(s1);

struct VOut { float4 pos : POSITION; float3 n : TEXCOORD0; float2 d : TEXCOORD1; };

float Fade(float2 xy) { return saturate((centre.w - length(xy - centre.xy)) / (centre.w - centre.z)); }

float Height(float2 idx)
{
    float2 uv = (idx + 0.5) * grid.w;
    float b = tex2Dlod(baseTex, float4(uv, 0, 0)).r;
    float c = tex2Dlod(coverTex, float4(uv, 0, 0)).r;
    return b + c * Fade(idx * grid.z);
}

VOut main(float2 ij : POSITION)
{
    VOut o;
    float2 idx = grid.xy + ij;
    float2 uv = (idx + 0.5) * grid.w;
    float b = tex2Dlod(baseTex, float4(uv, 0, 0)).r;
    float2 xy = idx * grid.z;
    float c = tex2Dlod(coverTex, float4(uv, 0, 0)).r * Fade(xy);

    float hL = Height(idx + float2(-1, 0)), hR = Height(idx + float2(1, 0));
    float hD = Height(idx + float2(0, -1)), hU = Height(idx + float2(0, 1));
    o.n = normalize(float3(hL - hR, hD - hU, 2 * grid.z));

    float4 p = float4(xy - eye.xy, b + c - eye.z, 1);
    o.pos = float4(dot(p, vp0), dot(p, vp1), dot(p, vp2), dot(p, vp3));
    o.d = float2(c, b < -10000 ? 1 : 0);
    return o;
}
)";

        // PS: c0 = (light direction, ambient), c1 = colour, c2.x = smallest drawn depth.
        const char* kPsHlsl = R"(
float4 light : register(c0);
float4 albedo : register(c1);
float4 opts : register(c2);

float4 main(float3 n : TEXCOORD0, float2 d : TEXCOORD1) : COLOR
{
    clip(d.x - opts.x);
    clip(0.5 - d.y);
    float lit = light.w + (1 - light.w) * saturate(dot(normalize(n), light.xyz));
    return float4(albedo.rgb * lit, 1);
}
)";

        int Slot(int i) { const int m = i % kTex; return m < 0 ? m + kTex : m; }
        int SlotOf(int i, int j) { return Slot(j) * kTex + Slot(i); } // row = y (j), column = x (i)

        bool FootprintType(int id)
        {
            const int32_t minId = *reinterpret_cast<const int32_t*>(kTerrainMinId);
            const int32_t maxId = *reinterpret_cast<const int32_t*>(kTerrainMaxId);
            const auto* index = *reinterpret_cast<const uint8_t* const* const*>(kTerrainIndex);
            if (!index || id < minId || id > maxId) return false;
            const uint8_t* row = index[id - minId];
            return row && (*reinterpret_cast<const uint32_t*>(row + kTerrainFlags) & 1);
        }

        // Base height and material mask of one world cell (resets its trench state when `fresh`).
        void Sample(int i, int j, bool fresh)
        {
            const int s = SlotOf(i, j);
            const float x = i * kCell, y = j * kCell;
            float z;
            const bool ok = terrain::HeightAt(x, y, z);
            const float base = ok ? z : kHole;
            if (base != g_base[s]) { g_base[s] = base; g_baseDirty = true; }

            uint8_t mask = 0;
            if (ok)
            {
                if (static_cast<Coverage>(g_settings.coverage) == Coverage::Everywhere) mask = 1;
                else { int type; mask = terrain::TerrainTypeAt(x, y, type) && FootprintType(type) ? 1 : 0; }
            }
            g_mask[s] = mask;
            if (fresh) { g_press[s] = 0.0f; g_rimH[s] = 0.0f; }
        }

        void SampleRect(int i0, int i1, int j0, int j1, bool fresh) // inclusive
        {
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) Sample(i, j, fresh);
        }

        // Moves the grid so the player is near its middle; fills only the cells that came into it.
        void Recentre(const float pos[3])
        {
            const int pi = static_cast<int>(std::floor(pos[0] / kCell)), pj = static_cast<int>(std::floor(pos[1] / kCell));
            const int ni = pi - kGridCells / 2, nj = pj - kGridCells / 2;
            const bool coverageChanged = g_lastCoverage != g_settings.coverage;
            if (g_haveGrid && !coverageChanged && std::abs(ni - g_gridI) < kRecentreCells && std::abs(nj - g_gridJ) < kRecentreCells) return;

            const double t0 = grassperf::Now();
            const int last = kGridVerts - 1;
            if (!g_haveGrid || coverageChanged || std::abs(ni - g_gridI) > last || std::abs(nj - g_gridJ) > last)
            {
                SampleRect(ni, ni + last, nj, nj + last, true);
                g_firstFillMs = grassperf::Now() - t0;
            }
            else
            {
                const int oi0 = g_gridI, oi1 = g_gridI + last, oj0 = g_gridJ, oj1 = g_gridJ + last;
                for (int j = nj; j <= nj + last; ++j)
                {
                    if (j < oj0 || j > oj1) { SampleRect(ni, ni + last, j, j, true); continue; }
                    if (ni < oi0) SampleRect(ni, oi0 - 1, j, j, true);
                    if (ni + last > oi1) SampleRect(oi1 + 1, ni + last, j, j, true);
                }
            }
            g_gridI = ni; g_gridJ = nj;
            g_haveGrid = true;
            g_lastCoverage = g_settings.coverage;
            g_fillMs = grassperf::Now() - t0;
        }

        // A trench under one grounded unit: pressed flat inside r, a rim between r and 1.6 r.
        void Stamp(const float pos[3], float radius)
        {
            float ground;
            if (!terrain::HeightAt(pos[0], pos[1], ground) || std::fabs(pos[2] - ground) > 0.35f) return; // airborne, swimming, on a WMO
            ++g_stamps;
            const float outer = radius * 1.6f, rimPeak = radius * 1.25f, rimHalf = radius * 0.35f;
            const float rimH = g_settings.depth * g_settings.rim;
            const int i0 = static_cast<int>(std::floor((pos[0] - outer) / kCell)), i1 = static_cast<int>(std::ceil((pos[0] + outer) / kCell));
            const int j0 = static_cast<int>(std::floor((pos[1] - outer) / kCell)), j1 = static_cast<int>(std::ceil((pos[1] + outer) / kCell));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    if (i < g_gridI || i >= g_gridI + kGridVerts || j < g_gridJ || j >= g_gridJ + kGridVerts) continue;
                    const float dx = i * kCell - pos[0], dy = j * kCell - pos[1];
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
            rel(g_vb); rel(g_ib); rel(g_decl); rel(g_vs); rel(g_ps); rel(g_baseTex); rel(g_coverTex);
            g_baseDirty = true;
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
            if (g_vb && g_ib && g_decl && g_vs && g_ps && g_baseTex && g_coverTex) return true;
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

            if (FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &g_baseTex, nullptr)) ||
                FAILED(dev->CreateTexture(kTex, kTex, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &g_coverTex, nullptr)))
                return fail("R32F textures failed");

            g_vs = static_cast<IDirect3DVertexShader9*>(Compile(dev, kVsHlsl, "vs_3_0", false));
            g_ps = static_cast<IDirect3DPixelShader9*>(Compile(dev, kPsHlsl, "ps_3_0", true));
            if (!g_vs || !g_ps) return fail("shader compile failed (see the log)");
            g_baseDirty = true;
            return true;
        }

        bool Upload()
        {
            D3DLOCKED_RECT lr{};
            if (g_baseDirty)
            {
                if (FAILED(g_baseTex->LockRect(0, &lr, nullptr, 0))) return false;
                for (int row = 0; row < kTex; ++row)
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &g_base[row * kTex], kTex * 4);
                g_baseTex->UnlockRect(0);
                g_baseDirty = false;
            }
            if (FAILED(g_coverTex->LockRect(0, &lr, nullptr, 0))) return false;
            const float depth = g_settings.depth;
            for (int row = 0; row < kTex; ++row)
            {
                float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                for (int col = 0; col < kTex; ++col)
                {
                    const int s = row * kTex + col;
                    out[col] = g_mask[s] ? (depth + g_rimH[s]) * (1.0f - g_press[s]) : 0.0f;
                }
            }
            g_coverTex->UnlockRect(0);
            return true;
        }

        void __cdecl OnWorldSceneEnd(void* /*user*/, const void* args)
        {
            if (!g_settings.enabled || !g_haveGrid) return;
            const auto* a = static_cast<const ev::WorldSceneEndArgs*>(args);
            auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
            if (!dev || !EnsureGpu(dev)) return;

            // The world's view (no translation: the scene is drawn about the camera, so positions go
            // in camera-relative, c6) and its rendered projection.
            float V[16], P[16];
            const void* graphics = gx::RawGraphicsDevice();
            if (!graphics || !wxl::game::gfx::SceneMatrices(V, P)) { g_inactive = "no scene matrices"; return; }
            std::memcpy(P, static_cast<const uint8_t*>(graphics) + kDeviceRenderProjection, sizeof(P));

            double t0 = grassperf::Now();
            if (!Upload()) { g_inactive = "texture upload failed"; return; }
            g_uploadMs = grassperf::Now() - t0;
            t0 = grassperf::Now();

            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
            // Depth-test against the surface the world was drawn into; whatever is bound now may not be it.
            IDirect3DSurface9* oldDepth = nullptr;
            dev->GetDepthStencilSurface(&oldDepth);
            auto* sceneDepth = a ? static_cast<IDirect3DSurface9*>(a->sceneDepth) : nullptr;
            const bool swapDepth = sceneDepth && sceneDepth != oldDepth;
            if (swapDepth) dev->SetDepthStencilSurface(sceneDepth);

            D3DVIEWPORT9 vp{};
            dev->GetViewport(&vp);
            vp.MinZ = 0.0f;
            vp.MaxZ = *reinterpret_cast<const float*>(kWorldPassMaxZ);
            dev->SetViewport(&vp);

            // View-projection (row-vector convention), uploaded as columns for dot products.
            float vpm[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) sum += V[r * 4 + k] * P[k * 4 + c];
                    vpm[r * 4 + c] = sum;
                }
            float cols[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) cols[c * 4 + r] = vpm[r * 4 + c];
            dev->SetVertexShaderConstantF(0, cols, 4);

            const float centreX = (g_gridI + kGridCells * 0.5f) * kCell, centreY = (g_gridJ + kGridCells * 0.5f) * kCell;
            const float c5[4] = { centreX, centreY, g_settings.fadeStart, g_settings.fadeEnd > g_settings.fadeStart + 0.1f ? g_settings.fadeEnd : g_settings.fadeStart + 0.1f };
            dev->SetVertexShaderConstantF(5, c5, 1);
            float eye[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            wxl::game::camera::GetPosition(eye);
            dev->SetVertexShaderConstantF(6, eye, 1);

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
                dev->SetTexture(sampler, s == 0 ? g_baseTex : g_coverTex);
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

            for (int pj = 0; pj < kPatches; ++pj)
                for (int pi = 0; pi < kPatches; ++pi)
                {
                    const float c4[4] = { static_cast<float>(g_gridI + pi * kPatchCells), static_cast<float>(g_gridJ + pj * kPatchCells),
                                          kCell, 1.0f / kTex };
                    dev->SetVertexShaderConstantF(4, c4, 1);
                    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kPatchVerts * kPatchVerts, 0, kPatchCells * kPatchCells * 2);
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
            g_api->UiSliderFloat("Depth (yd)", &g_settings.depth, 0.0f, 1.5f);
            g_api->UiSliderFloat("Rim (x depth)", &g_settings.rim, 0.0f, 1.0f);
            g_api->UiCheckbox("Units carve trenches", &g_settings.stamp);
            g_api->UiSliderFloat("Trench width (x unit size)", &g_settings.stampScale, 0.3f, 3.0f);
            g_api->UiSliderFloat("Relax time (s)", &g_settings.relaxSeconds, 1.0f, 300.0f);
            g_api->UiSliderFloat("Fade start (yd)", &g_settings.fadeStart, 5.0f, 40.0f);
            g_api->UiSliderFloat("Fade end (yd)", &g_settings.fadeEnd, 5.0f, 40.0f);
            g_api->UiCheckbox("Wireframe", &g_settings.wireframe);

            g_api->UiSeparator();
            std::vector<std::string> lines;
            auto add = [&lines, &line]() { lines.emplace_back(line); };
            if (const char* why = g_settings.enabled ? g_inactive : "switched off") { std::snprintf(line, sizeof(line), "not drawing: %s", why); add(); }
            std::snprintf(line, sizeof(line), "grid %dx%d vertices (%.0f yd, %.2f yd cells), first cell (%d, %d)",
                          kGridVerts, kGridVerts, kGridCells * kCell, kCell, g_gridI, g_gridJ); add();
            std::snprintf(line, sizeof(line), "holes/unloaded cells %u, units stamping %u", g_holes, g_stamps); add();
            std::snprintf(line, sizeof(line), "CPU: full fill %.2f ms, last recentre %.2f ms, sim %.2f ms, upload %.2f ms, draw submit %.2f ms",
                          g_firstFillMs, g_fillMs, g_simMs, g_uploadMs, g_drawMs); add();

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
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        if (!g_settings.enabled || !snap.inWorld) return;
        if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
        const double t0 = grassperf::Now();

        Recentre(snap.playerPos);
        // Chunks stream in after the grid saw them: re-sample a few rows per frame, keeping trenches.
        for (int k = 0; k < kRefreshRowsPerFrame; ++k)
        {
            SampleRect(g_gridI, g_gridI + kGridVerts - 1, g_gridJ + g_refreshRow, g_gridJ + g_refreshRow, false);
            g_refreshRow = (g_refreshRow + 1) % kGridVerts;
        }
        if (g_refreshRow < kRefreshRowsPerFrame) // once per round: count what's still missing
        {
            unsigned holes = 0;
            for (int j = 0; j < kGridVerts; ++j)
                for (int i = 0; i < kGridVerts; ++i) holes += g_base[SlotOf(g_gridI + i, g_gridJ + j)] == kHole;
            g_holes = holes;
        }

        Simulate(dt);
        g_simMs = grassperf::Now() - t0;
    }
}
