#include "SurfaceCoverInternal.hpp"

#include "../debug/Profiler.hpp"
#include "../env/Lights.hpp"
#include "../render/MoistureTexture.hpp"
#include "GrassPerf.hpp"
#include "ModelLights.hpp"
#include "TerrainLights.hpp"
#include "TerrainWetness.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace wxl_livingazeroth::cover::detail
{
    namespace ev = wxl::events;
    namespace gx = wxl::game::gx;

    bool g_tintSupported = false;

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

    // The terrain's lighting and fog as last read (for the panel): VS c12 fog, c24 sun direction
    // (view space), c25 ambient, c26 diffuse, PS c2 fog colour.
    float g_seenFog[4] = {}, g_seenLight[16] = {}, g_seenFogColor[4] = {}; // light: c24..c27 (c27 = specular colour, exponent)
    // The terrain's dynamic shadows as found at our draw point: which shadow maps are still bound
    // on its samplers s5..s8, and whether they were used (all four bound and switched on).
    bool  g_shadowBound[4] = {};
    bool  g_shadowUsed = false;
    bool  g_seenScene = false;

    double g_uploadMs = 0, g_drawMs = 0;
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

    // --- GPU ---------------------------------------------------------------------------------
    void ReleaseGpu()
    {
        auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
        rel(g_vb); rel(g_ib); rel(g_decl); rel(g_vs); rel(g_ps); moisturetex::Release();
        for (Level& L : g_levels) { rel(L.baseTex); rel(L.coverTex); rel(L.lookTex); rel(L.texWTex[0]); rel(L.texWTex[1]); rel(L.propsTex); L.baseDirty = L.coverDirty = L.lookDirty = true; }
        ReleaseCoverTextures();
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

    // inPass: called inside the world pass, where the world's viewport and depth surface are still
    // bound. Otherwise (end of scene) both have to be put back first.
    void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth, bool inPass)
    {
        prof::Scope scope(prof::kOursCoverDraw);
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

        double t0 = grassperf::Now();
        {
            prof::Scope uploads(prof::kOursCoverUpload);
            EnsureCoverTextures(dev);
            g_uploadStats = UploadStats{};
            for (int k = 0; k < levels; ++k)
                if (g_levels[k].haveGrid && !UploadLevel(g_levels[k], k == 0)) { g_inactive = "texture upload failed"; return; }
        }
        g_uploadMs = grassperf::Now() - t0;
        // No cover anywhere around (a zone without any, e.g. Duskwood): nothing to draw.
        bool anyCover = false;
        for (int k = 0; k < levels; ++k) anyCover = anyCover || (g_levels[k].haveGrid && g_levels[k].anyCover);
        if (!anyCover) { g_inactive = "no cover around (nothing to draw)"; return; }
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
            if (!L.haveGrid || !L.anyCover) continue; // (an empty level still leaves its hole in the coarser one: no cover there either)
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
        {
            prof::Scope scope(prof::kOursLightUpload);
            terrainwet::BeforeTerrainStage(static_cast<IDirect3DDevice9*>(gx::RawDevice()));
            terrainlights::BeforeTerrainStage(static_cast<IDirect3DDevice9*>(gx::RawDevice()));
        }
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

    void InstallDraw(const WXL_Api* api)
    {
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
    if (!api->HookAttach("LivingAzeroth.TerrainStage", kTerrainStage, reinterpret_cast<void*>(&hkTerrainStage),
                         reinterpret_cast<void**>(&g_origTerrainStage), WXL_HOOK_DEFAULT_PRIORITY))
    {
        api->Log(WXL_LOG_WARN, kTag, "surface cover: terrain stage hook failed; drawing at the end of the scene instead.");
        g_settings.drawPoint = 1;
    }
    }
}
