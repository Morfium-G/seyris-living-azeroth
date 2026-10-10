#include "GroundFog.hpp"

#include "../debug/Profiler.hpp"
#include "../env/Fog.hpp"
#include "../env/FogSettings.hpp"
#include "../env/Lights.hpp"
#include "../env/TerrainHeight.hpp"
#include "../env/WorldQuery.hpp"
#include "../render/GxState.hpp"
#include "../render/SceneDepth.hpp"
#include "FogWake.hpp"
#include "GrassPerf.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wxl_livingazeroth::groundfog
{
    namespace
    {
        namespace gx = wxl::game::gx;
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // Client globals (in-client confirmed 2026-10-08, ground-fog-design.md "Phase 1"):
        constexpr uintptr_t kCameraInLiquid = 0x00CD8794; // non-zero while the camera is in a liquid
        constexpr uintptr_t kFogColor       = 0x00D38BF4; // the zone's fog colour this frame (0xAARRGGBB)
        // [believed, coa-vfog] the camera's WMO interior fog blend (0 outdoors .. 1 inside).
        constexpr uintptr_t kInteriorBlend  = 0x00D38B9C;

        // Mods that draw their own fog or water through their own D3D9 wrapper and depth: give way.
        const char* const kConflicts[] = { "CoAVolFog.dll", "wxl-vol-fog.dll", "wxl-modern-water.dll" };

        Settings       g_settings;
        const WXL_Api* g_api = nullptr;

        using WeatherFn    = void(__fastcall*)(void* self, void* edx);
        using LiquidFn     = void(__fastcall*)(void* bank, void* edx, void* transform, int passType);
        using DeviceDrawFn = void(__fastcall*)(void* device, void* edx, uint32_t* batch, int indexed);
        WeatherFn    g_origWeather = nullptr;
        LiquidFn     g_origLiquid = nullptr;
        DeviceDrawFn g_origDraw = nullptr;

        int         g_liquidPass = -1;
        bool        g_drawThisFrame = false;  // decided before the water draws
        const char* g_inactive = "not drawn yet";
        const char* g_conflict = nullptr;
        unsigned    g_conflictCheck = 0;
        double      g_lastMs = 0.0;
        unsigned    g_draws = 0, g_fieldUploads = 0;
        ULONGLONG   g_lastTick = 0;
        float       g_noiseOffset[3] = {};

        // GPU resources (D3DPOOL_MANAGED: they survive a reset; dropped when the device changes).
        IDirect3DDevice9*        g_device = nullptr;
        IDirect3DPixelShader9*   g_ps = nullptr;
        IDirect3DTexture9*       g_field = nullptr;
        IDirect3DTexture9*       g_colorTex = nullptr; // A8R8G8B8: the fog's colour per cell / 2, alpha the pool mask
        IDirect3DTexture9*       g_topTex = nullptr;   // R32F kTopSize^2: the highest fog top per block and its neighbours
        constexpr int   kTopBlock = 4;                                 // cells per top-map block (8 yd)
        constexpr int   kTopSize = fog::kGridSize / kTopBlock;         // 32^2 = 4 KB
        constexpr float kNoTop = -1.0e5f;                              // a block without fog
        float g_topDilated[kTopSize * kTopSize];                       // the top map as uploaded (kNoTop: no fog in or next to the block)
        IDirect3DTexture9* g_lightTex = nullptr;   // A16B16G16R16F, the field's cells: our lights' light reaching the fog there
        double   g_lightMs = 0.0;
        unsigned g_lightsUsed = 0, g_lightCells = 0;
        float                    g_ceiling = -1.0e9f;  // world z of the highest fog top in the field
        IDirect3DVolumeTexture9* g_noise = nullptr;
        bool                     g_shaderFailed = false;
        uint32_t g_fieldVersion = ~0u, g_groundVersion = ~0u;
        float    g_fieldSignature = -1.0f;
        float    g_groundRef = 0.0f;
        float    g_poolDensity = 0.0f; // extinction per yd inside heavy fog pools (their kinds' weighted mean)
        // What the last draw read (for the panel's depth diagnostics).
        D3DVIEWPORT9 g_seenViewport{};
        float        g_seenM22 = 0.0f, g_seenM32 = 0.0f;
        // ------------------------------------------------------------------ shader
        const char* const kPsHlsl = R"(
sampler2D depthTex : register(s0);
sampler2D fieldTex : register(s1);   // r = light density per yd at the ground, g = its thickness (yd), b = ground z - cCam.w, a = heavy pool surface z - cCam.w
sampler3D noiseTex : register(s2);
sampler2D colorTex : register(s3);   // rgb = the fog's colour there / 2 (the kinds' colours, mixed by their share), a = 1 where a pool lies
float4 cDepth  : register(c0);  // x = viewport min z, y = 1 / (max z - min z), z = deepest world depth, w = max distance
float4 cProj   : register(c1);  // projection m00, m11, m20, m21
float4 cProj2  : register(c2);  // projection m22, m32, the march's step budget, depth decode switch
float4 cInv0   : register(c3);  // inverse view, rows (row vectors): world = v.x r0 + v.y r1 + v.z r2 + r3
float4 cInv1   : register(c4);
float4 cInv2   : register(c5);
float4 cCam    : register(c6);  // xyz = r3 = the camera in world space, w = ground reference z
float4 cGrid   : register(c7);  // x = 1 / the field's extent (yd), y = dither (0 none .. 1 the first step in the fog shortened per pixel)
float4 cBox    : register(c8);  // the field's valid box: min x, min y, max x, max y
float4 cColor  : register(c9);  // x = our lights in the fog (lightTex bound), a = density scale
float4 cNoise  : register(c10); // xyz = drift offset (tiles), w = 1 / tile size (yd)
float4 cNoise2 : register(c11); // x = noise amount, y = 1 / edge fade (yd), z = debug colour
float4 cUv     : register(c12); // depth texture uv = xy + uv * zw
float4 cPool   : register(c13); // x = heavy pool density per yd, y = the pools' soft top (yd), z = the fog's ceiling (world z), w = pool gate
float4 cView   : register(c14); // x = debug view: 0 the fog, 1 light layer only, 2 pools only, 3 ground check;
                                //   y = step in the fog (yd), z = the top map's block (yd), w = step growth per yd of distance
sampler2D topTex : register(s4);     // r = the highest fog top in the block and its 8 neighbours - cCam.w (-1e5: none), point sampled
sampler2D lightTex : register(s6);   // our lights' light reaching the fog per field cell (rgb, linear), on when cColor.x
sampler2D wakeTex : register(s5);   // units pushing the fog (features/FogWake): r = push (< 0 at the rim), g = its top z - cCam.w, ba = pattern displacement (yd)
float4 cWake   : register(c15); // xy = the wake map's origin (world), z = 1 / its extent (yd), w = on

// The wake at a step (its middle pm, height zm): how much of the fog stays (the push reaches up to
// the unit's height, softly), and the patch pattern's displacement there.
float WakeKeep(float3 pm, float zm, out float2 displace)
{
    displace = 0;
    if (cWake.w < 0.5) return 1;
    float4 w = tex2Dlod(wakeTex, float4((pm.xy - cWake.xy) * cWake.z, 0, 0));
    float below = saturate((w.g + cCam.w - zm) / 0.5 + 0.5);
    displace = w.ba * below;
    return max(1 - w.r * below, 0);
}

// The light layer's density factor exp(-max(z - B, 0) / T) averaged over z in [za, zb]: full below
// the ground B, falling off above it. Antiderivative: z - B below, T (1 - exp(-(z - B) / T)) above.
float LayerIntegral(float z, float B, float T) { return z < B ? z - B : T * (1 - exp(-(z - B) / T)); }
float LightAverage(float B, float T, float za, float zb)
{
    if (abs(zb - za) < 1e-3) return exp(-max(0.5 * (za + zb) - B, 0) / T);
    return (LayerIntegral(zb, B, T) - LayerIntegral(za, B, T)) / (zb - za);
}

// The pool's factor saturate((S - z) / s) (full below its surface S, a soft top s) averaged over z in
// [za, zb]. With u = (S - z) / s, the antiderivative of saturate(u) is 0, u^2 / 2, u - 1/2.
float RampIntegral(float u) { u = max(u, 0); return u < 1 ? 0.5 * u * u : u - 0.5; }
float PoolAverage(float S, float za, float zb, float s)
{
    float ua = (S - za) / s, ub = (S - zb) / s;
    if (abs(ua - ub) < 1e-3) return saturate(0.5 * (ua + ub));
    return (RampIntegral(ua) - RampIntegral(ub)) / (ua - ub);
}

// The field and the colour texture at a point (xy), our lights' light reaching the fog there (added to
// the colour), and the fade at the field's edge.
float4 SampleField(float2 xy, out float4 col, out float edge)
{
    float4 uvw = float4(xy * cGrid.x, 0, 0);
    col = tex2Dlod(colorTex, uvw);
    col.rgb = col.rgb * 2;
    if (cColor.x > 0.5) col.rgb += tex2Dlod(lightTex, uvw).rgb;
    edge = saturate(min(min(xy.x - cBox.x, cBox.z - xy.x), min(xy.y - cBox.y, cBox.w - xy.y)) * cNoise2.y);
    return tex2Dlod(fieldTex, uvw);
}

// Extinction per yd, averaged over the step's height range [za, zb], with the field read at one
// point. Light layer: falls off above the ground. Heavy fog: everything below the pool's flat
// surface f.a (the terrain hides what's under it), with a soft top, gated by the pool mask (an
// empty cell's level, just under its ground, only kept fog off the banks where the field's ground
// matched the drawn surface; elsewhere, ungated, empty cells filled with fog).
float Sigma(float4 f, float poolMask, float edge, float za, float zb)
{
    float light = f.r * LightAverage(f.b + cCam.w, max(f.g, 0.25), za, zb);
    float pool = cPool.x * PoolAverage(f.a + cCam.w, za, zb, cPool.y);
    if (cPool.w > 0.5) pool *= poolMask;
    if (cView.x > 0.5 && cView.x < 1.5) pool = 0;
    if (cView.x > 1.5 && cView.x < 2.5) light = 0;
    return (light + pool) * edge;
}

// `stepLen`: a step much longer than a patch (~1/8 of the tile) averages over many of them, so the
// patchiness fades toward its mean there (one point read per step gave scattered dots far away).
float Patchiness(float3 p, float stepLen)
{
    float3 q = p * cNoise.w + cNoise.xyz;
    float a = tex3Dlod(noiseTex, float4(q, 0)).r;
    float b = tex3Dlod(noiseTex, float4(q * 2.71 + 0.37, 0)).r;
    float n = a * 0.65 + b * 0.35;
    float amount = cNoise2.x * saturate(1.5 - stepLen * cNoise.w * 8);
    return lerp(1.0, 2.0 * saturate((n - 0.5) * 2.5 + 0.5), amount); // mean ~1
}

float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    float d = tex2Dlod(depthTex, float4(cUv.xy + uv * cUv.zw, 0, 0)).r;
    // The client's projection is OpenGL-style (0x6BF370: m22 = (f + n) / (f - n), m32 = -2fn / (f - n),
    // depth -1..1); the D3D backend maps that to 0..1 within the viewport. So back to -1..1 first:
    // decoding the 0..1 value directly gave twice the real distance.
    float d01 = saturate((d - cDepth.x) * cDepth.y);
    float ndcZ = cProj2.w > 0.5 ? d01 : d01 * 2 - 1; // debug switch: D3D-style decoding
    float viewZ = cProj2.y / min(ndcZ - cProj2.x, -1e-6);
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 v = float3((ndc.x - cProj.z) * viewZ / cProj.x, (ndc.y - cProj.w) * viewZ / cProj.y, viewZ);
    float3 world = v.x * cInv0.xyz + v.y * cInv1.xyz + v.z * cInv2.xyz + cCam.xyz;
    // Debug: the reconstructed world position as contour bands (height every 2 yd in red, x/y every
    // 8 yd in green/blue). Right decoding: they stay on the terrain while the camera turns.
    if (cNoise2.w > 0.5 && d <= cDepth.z)
        return float4(frac(world.z * 0.5), frac(world.x * 0.125) * 0.5, frac(world.y * 0.125) * 0.5, 0);
    // Debug: the field's ground against the drawn surface. White: a pool lies here. Red: the
    // ungated pool term covers this surface without a pool (the drawn surface lies below the
    // field's empty-cell level), brighter the deeper. Green: the drawn surface is within 1 yd above
    // the field's ground. Blue: higher (objects, roofs). Grey: outside the field.
    if (cView.x > 2.5 && d <= cDepth.z)
    {
        float4 uvs = float4(world.xy * cGrid.x, 0, 0);
        float4 g = tex2Dlod(fieldTex, uvs);
        float pooled = tex2Dlod(colorTex, uvs).a;
        float under = g.a + cCam.w + cPool.y - world.z;
        float3 c = float3(0.2, 0.25, 0.45);
        if (pooled > 0.5) c = float3(0.9, 0.9, 0.9);
        else if (under > 0) c = float3(1, 0.1, 0.1) * (0.4 + 0.6 * saturate(under / 4));
        else if (world.z - (g.b + cCam.w) < 1) c = float3(0.1, 0.6, 0.1);
        if (min(min(world.x - cBox.x, cBox.z - world.x), min(world.y - cBox.y, cBox.w - world.y)) < 0) c = 0.15;
        return float4(c, 0);
    }

    float3 ray = world - cCam.xyz;
    float dist = length(ray);
    float3 dir = ray / max(dist, 1e-4);
    if (d > cDepth.z) dist = cDepth.w; // sky and the distant terrain beyond the world viewport
    float len = min(dist, cDepth.w);

    // Only the part of the ray below the fog's ceiling (the highest fog top in the field): the
    // steps fall inside the thin layer instead of spreading over the whole view, so a pool a yard
    // or two deep is sampled every frame alike (spread over 250 yd it was hit or missed depending on
    // where the camera stood).
    float t0 = 0, t1 = len;
    if (cCam.z > cPool.z)
    {
        if (dir.z >= -1e-4) return float4(0, 0, 0, 1);
        t0 = (cCam.z - cPool.z) / -dir.z;
    }
    else if (dir.z > 1e-4) t1 = min(t1, (cPool.z - cCam.z) / dir.z);
    if (t0 >= t1) return float4(0, 0, 0, 1);

    // The march: through air above every fog top it jumps (the top map: the highest fog top per
    // block and its neighbours), inside the fog it takes short steps (cView.y yd, longer far away).
    // Each step's density is its average over the step's height range, computed exactly (the
    // layer's fall-off and the pool's flat top are known in z), sampled in xy at one jittered
    // point. Before, every sample stood for an even 1/16 of the ray: a sample under a pool top
    // counted its whole stretch (up to 15 yd) as pool, so the fog grew with the distance and the
    // camera angle.
    int budget = (int)cProj2.z;
    float jitter = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    float horizontal = max(length(dir.xy), 1e-3);
    float t = t0;
    float T = 1;
    float3 scatter = 0;
    // The field at the step's start, carried over from the previous step's end (each step reads the
    // field at both ends and averages them: one reading per step, edges follow smoothly instead of
    // the bands one reading in the middle gave).
    bool haveStart = false;
    float4 fA = 0, cA = 0;
    float edgeA = 0;
    [loop] for (int i = 0; i < 128; ++i)
    {
        if (i >= budget || t >= t1 || T < 0.005) break;
        float3 p = cCam.xyz + dir * t;
        float top = tex2Dlod(topTex, float4(p.xy * cGrid.x, 0, 0)).r + cCam.w;
        float gap = p.z - top;
        [branch] if (gap > 0)
        {
            // Above the fog here: on to where the ray reaches the top, at most one block on (the
            // top map's neighbours cover that far).
            float jump = cView.z / horizontal;
            if (dir.z < -1e-4) jump = min(jump, gap / -dir.z);
            t += max(jump, 0.05);
            haveStart = false;
            continue;
        }
        // At least the step the rest of the budget needs to reach the end evenly (a far ray that ran
        // out used to put everything left into one last step, sampled at one point: scattered dots).
        // The dither (cGrid.y) shortens the first step in the fog per pixel, shifting the steps:
        // grain instead of banding (there's no frame-to-frame smoothing to hide grain).
        float ds = max(max(cView.y, t * cView.w), (t1 - t) / max(budget - i, 1));
        if (!haveStart) ds *= lerp(1, max(jitter, 0.05), cGrid.y);
        float tb = min(t + ds, t1);
        float za = p.z, zb = cCam.z + dir.z * tb;
        if (!haveStart)
        {
            fA = SampleField(p.xy, cA, edgeA);
            haveStart = true;
        }
        float3 pb = cCam.xyz + dir * tb;
        float4 cB;
        float edgeB;
        float4 fB = SampleField(pb.xy, cB, edgeB);
        float sA = Sigma(fA, cA.a, edgeA, za, zb), sB = Sigma(fB, cB.a, edgeB, za, zb);
        float sigma = 0.5 * (sA + sB);
        [branch] if (sigma > 1e-5)
        {
            // Each step's fog in its own colour (the kinds' mix at both ends, by their fog), seen
            // through what's in front.
            float3 pm = cCam.xyz + dir * (0.5 * (t + tb));
            // Units: the fog pushed out around them and their trail, its pattern dragged along
            // (sampled where the pattern would have come from).
            float2 displace;
            sigma *= WakeKeep(pm, 0.5 * (za + zb), displace);
            float a = 1 - exp(-sigma * Patchiness(pm - float3(displace, 0), tb - t) * (tb - t) * cColor.a);
            float3 c = cNoise2.z > 0.5 ? float3(1, 0, 1) : (sA * cA.rgb + sB * cB.rgb) / (sA + sB);
            scatter += T * a * c;
            T *= 1 - a;
        }
        fA = fB; cA = cB; edgeA = edgeB;
        t = tb;
    }
    return float4(scatter, T);
}
)";

        // ------------------------------------------------------------------ helpers
        uint16_t ToHalf(float f)
        {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
            uint32_t mant = x & 0x7FFFFFu;
            if (exp <= 0) return static_cast<uint16_t>(sign);                 // too small: 0
            if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);     // too large: max
            return static_cast<uint16_t>(sign | (exp << 10) | ((mant + 0x1000u) >> 13 > 0x3FFu ? 0x3FFu : (mant + 0x1000u) >> 13));
        }

        void ReleaseGpu()
        {
            if (g_ps) { g_ps->Release(); g_ps = nullptr; }
            if (g_field) { g_field->Release(); g_field = nullptr; }
            if (g_colorTex) { g_colorTex->Release(); g_colorTex = nullptr; }
            if (g_topTex) { g_topTex->Release(); g_topTex = nullptr; }
            if (g_lightTex) { g_lightTex->Release(); g_lightTex = nullptr; }
            fogwake::Release();
            if (g_noise) { g_noise->Release(); g_noise = nullptr; }
            g_fieldVersion = g_groundVersion = ~0u;
            g_fieldSignature = -1.0f;
        }

        IDirect3DPixelShader9* Compile(IDirect3DDevice9* dev)
        {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            if (!m) m = LoadLibraryA("d3dcompiler_47.dll");
            const auto compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
            if (!compile) return nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = compile(kPsHlsl, std::strlen(kPsHlsl), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err);
            if (err)
            {
                g_api->Log(WXL_LOG_WARN, kTag, "ground fog: shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
                err->Release();
            }
            if (FAILED(hr) || !code) return nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            if (FAILED(dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps))) ps = nullptr;
            code->Release();
            return ps;
        }

        /// A tileable smooth noise volume (32^3, L8): two lattices (period 4 and 8 cells across the
        /// tile), smoothly interpolated, so the hardware's trilinear filter adds no blockiness.
        IDirect3DVolumeTexture9* MakeNoise(IDirect3DDevice9* dev)
        {
            constexpr int N = 32;
            IDirect3DVolumeTexture9* tex = nullptr;
            if (FAILED(dev->CreateVolumeTexture(N, N, N, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) return nullptr;
            auto hash = [](int x, int y, int z, int seed) {
                uint32_t h = static_cast<uint32_t>(x * 374761393 + y * 668265263 + z * 2147483647 + seed * 144665);
                h = (h ^ (h >> 13)) * 1274126177u;
                return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
            };
            auto lattice = [&](float x, float y, float z, int period, int seed) {
                const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y)), z0 = static_cast<int>(std::floor(z));
                const float fx = x - x0, fy = y - y0, fz = z - z0;
                auto s = [](float f) { return f * f * (3.0f - 2.0f * f); };
                const float sx = s(fx), sy = s(fy), sz = s(fz);
                auto at = [&](int a, int b, int c) { return hash(((a % period) + period) % period, ((b % period) + period) % period, ((c % period) + period) % period, seed); };
                const float c00 = at(x0, y0, z0) + (at(x0 + 1, y0, z0) - at(x0, y0, z0)) * sx;
                const float c10 = at(x0, y0 + 1, z0) + (at(x0 + 1, y0 + 1, z0) - at(x0, y0 + 1, z0)) * sx;
                const float c01 = at(x0, y0, z0 + 1) + (at(x0 + 1, y0, z0 + 1) - at(x0, y0, z0 + 1)) * sx;
                const float c11 = at(x0, y0 + 1, z0 + 1) + (at(x0 + 1, y0 + 1, z0 + 1) - at(x0, y0 + 1, z0 + 1)) * sx;
                const float c0 = c00 + (c10 - c00) * sy, c1 = c01 + (c11 - c01) * sy;
                return c0 + (c1 - c0) * sz;
            };
            D3DLOCKED_BOX box{};
            if (FAILED(tex->LockBox(0, &box, nullptr, 0))) { tex->Release(); return nullptr; }
            for (int z = 0; z < N; ++z)
                for (int y = 0; y < N; ++y)
                {
                    auto* row = static_cast<uint8_t*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch;
                    for (int x = 0; x < N; ++x)
                    {
                        const float a = lattice(x * 4.0f / N, y * 4.0f / N, z * 4.0f / N, 4, 1);
                        const float b = lattice(x * 8.0f / N, y * 8.0f / N, z * 8.0f / N, 8, 2);
                        const float v = a * 0.7f + b * 0.3f;
                        row[x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
                    }
                }
            tex->UnlockBox(0);
            return tex;
        }

        float SettingsSignature()
        {
            float s = g_settings.densityScale * 7.0f + g_settings.floorReach * 0.731f;
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                const fog::KindSettings& ks = fog::Settings(k);
                s += (ks.enabled ? 1.0f : 0.0f) * (k + 1) * 13.0f + ks.density * (k + 3) * 101.0f + ks.thickness * (k + 5) * 0.37f
                   + ks.weight * (k + 7) * 3.1f;
            }
            return s;
        }

        void FogColor(int kind, float out[3]);

        /// The field texture: per cell, density (the enabled kinds' amount x density, summed), their
        /// density-weighted layer thickness, and the ground relative to a reference height.
        bool UploadField(IDirect3DDevice9* dev, float groundRef)
        {
            const fog::Grid& grid = fog::Current();
            if (!grid.amount[0] || !grid.ground) return false;
            if (!g_field && FAILED(dev->CreateTexture(fog::kGridSize, fog::kGridSize, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &g_field, nullptr)))
            {
                g_field = nullptr;
                return false;
            }
            if (!g_colorTex && FAILED(dev->CreateTexture(fog::kGridSize, fog::kGridSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_colorTex, nullptr)))
            {
                g_colorTex = nullptr;
                return false;
            }
            if (!g_topTex && FAILED(dev->CreateTexture(kTopSize, kTopSize, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &g_topTex, nullptr)))
            {
                g_topTex = nullptr;
                return false;
            }
            // Each kind's colour here (they follow the stock fog colour, so they change with the time of
            // day), and the "all kinds" colour for cells without fog.
            float kindColor[fog::kKindCount][3], placeColor[3];
            FogColor(-1, placeColor);
            float signature = SettingsSignature() + placeColor[0] * 17.0f + placeColor[1] * 19.0f + placeColor[2] * 23.0f;
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                FogColor(k, kindColor[k]);
                signature += (kindColor[k][0] * 29.0f + kindColor[k][1] * 31.0f + kindColor[k][2] * 37.0f) * (k + 1);
            }
            if (grid.version == g_fieldVersion && grid.groundVersion == g_groundVersion && signature == g_fieldSignature &&
                std::fabs(groundRef - g_groundRef) < 64.0f)
                return true;

            // The fog floor: the light layer starts here, not on each cell's own ground (a layer
            // following every cell drapes up cliffs and walls). The lowest ground within floorReach
            // cells (default 2 = 4 yd), smoothed over the 3x3 around, never above the cell's own ground: on a cliff the
            // wall's cells get the floor at its foot, so the layer has thinned out before it climbs;
            // gentle slopes sink it by a fraction of a yard.
            constexpr int N = fog::kGridSize;
            static std::vector<float> base(N * N), low(N * N);
            auto wrap = [](int v) { const int m = v % N; return m < 0 ? m + N : m; };
            auto inGrid = [&](int i, int j) { return i >= grid.firstI && i < grid.firstI + N && j >= grid.firstJ && j < grid.firstJ + N; };
            for (int pass = 0; pass < 2; ++pass)
                for (int r = 0; r < N; ++r)
                    for (int c = 0; c < N; ++c)
                    {
                        const int i = grid.firstI + wrap(c - grid.firstI), j = grid.firstJ + wrap(r - grid.firstJ); // world cell
                        const int index = r * N + c;
                        if (pass == 0)
                        {
                            float most = grid.ground[index];
                            const int R = g_settings.floorReach < 0 ? 0 : (g_settings.floorReach > 4 ? 4 : g_settings.floorReach);
                            for (int dj = -R; dj <= R; ++dj)
                                for (int di = -R; di <= R; ++di)
                                {
                                    if (!inGrid(i + di, j + dj)) continue;
                                    const int n = wrap(j + dj) * N + wrap(i + di);
                                    if (grid.groundKnown[n] && grid.ground[n] < most) most = grid.ground[n];
                                }
                            low[index] = most;
                            continue;
                        }
                        float sum = 0.0f;
                        int count = 0;
                        for (int dj = -1; dj <= 1; ++dj)
                            for (int di = -1; di <= 1; ++di)
                            {
                                if (!inGrid(i + di, j + dj)) continue;
                                sum += low[wrap(j + dj) * N + wrap(i + di)];
                                ++count;
                            }
                        base[index] = std::fmin(grid.ground[index], count ? sum / count : low[index]);
                    }

            static std::vector<float>   cellRgb(N * N * 3);
            static std::vector<uint8_t> cellHas(N * N), cellPooled(N * N);
            static std::vector<float>   cellTop(N * N);
            float heavyWeight = 0.0f, heavyDensity = 0.0f; // for the pools' density (their weighted mean)
            float ceiling = -1.0e9f;
            D3DLOCKED_RECT lr{}, lc{};
            if (FAILED(g_field->LockRect(0, &lr, nullptr, 0))) return false;
            if (FAILED(g_colorTex->LockRect(0, &lc, nullptr, 0))) { g_field->UnlockRect(0); return false; }
            g_groundRef = groundRef;
            for (int r = 0; r < fog::kGridSize; ++r)
            {
                auto* texel = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch);
                auto* colour = static_cast<uint8_t*>(lc.pBits) + r * lc.Pitch;
                for (int c = 0; c < fog::kGridSize; ++c, texel += 4, colour += 4)
                {
                    const int index = r * fog::kGridSize + c;
                    // Each kind splits by its Weight: the light share is a layer thinning upward
                    // (density, thickness); the heavy share is a pool with a flat top at the ground +
                    // its depth (amount x thickness), drawn with the heavy density (c13).
                    float density = 0.0f, weighted = 0.0f, pool = 0.0f, floorSum = 0.0f;
                    float mix[3] = {}, mixWeight = 0.0f; // colour: the kinds' colours by their share of the fog here
                    for (int k = 0; k < fog::kKindCount; ++k)
                    {
                        const fog::KindSettings& ks = fog::Settings(k);
                        if (!ks.enabled) continue;
                        const float a = grid.amount[k][index];
                        const float d = a * ks.density * (1.0f - ks.weight);
                        density += d;
                        weighted += d * ks.thickness;
                        // Where this kind's layer starts: the fog floor, or (water fog, steam) the
                        // height it came from, never above the ground here (blown down a slope, it
                        // settles on the surface).
                        const float kindFloor = grid.floor[k] ? std::fmin(grid.floor[k][index], grid.ground[index]) : base[index];
                        floorSum += d * kindFloor;
                        pool += a * ks.weight * ks.thickness;
                        heavyWeight += a * ks.weight;
                        heavyDensity += a * ks.weight * ks.density;
                        const float share = a * ks.density;
                        for (int ch = 0; ch < 3; ++ch) mix[ch] += share * kindColor[k][ch];
                        mixWeight += share;
                    }
                    const bool blocked = grid.block && grid.block[index] == 3;
                    const float thickness = density > 0.0f ? weighted / density : 1.0f;
                    const float layerFloor = density > 0.0f ? floorSum / density : base[index]; // the kinds' floors by their density
                    texel[0] = ToHalf(blocked ? 0.0f : density);
                    texel[1] = ToHalf(thickness);
                    texel[2] = ToHalf(layerFloor - g_groundRef);
                    // The pool as a surface: a flat level (the real ground + its depth) with fog below it.
                    // Below the terrain it's hidden anyway, since the march ends at the scene depth. Cells
                    // without a pool sit just under the lowest ground around them, so filtering toward a
                    // pooled neighbour keeps that neighbour's level and fades out at the shore: the fog
                    // reaches onto a bank only where the bank is lower than the pool's surface. (Built
                    // on a lower base instead, the pool's slab reached over the bank, above the slope.)
                    const bool pooled = !blocked && pool >= 0.02f;
                    texel[3] = ToHalf((pooled ? grid.ground[index] + pool : base[index] - 1.0f) - g_groundRef);
                    cellPooled[index] = pooled ? 255 : 0;

                    // The highest fog top in the field: the march only covers the height below it.
                    // Per cell too, for the top map (the march jumps through air above it).
                    cellTop[index] = kNoTop;
                    if (!blocked && (density > 1.0e-4f || pooled))
                    {
                        const float top = std::fmax(pooled ? grid.ground[index] + pool + 0.5f : -1.0e9f,
                                                    density > 1.0e-4f ? layerFloor + thickness * 4.0f : -1.0e9f);
                        if (top > ceiling) ceiling = top;
                        cellTop[index] = top - g_groundRef;
                    }

                    // The cell's own colour; cells without fog are filled from their neighbours below.
                    (void)colour;
                    cellHas[index] = mixWeight > 0.0f ? 1 : 0;
                    for (int ch = 0; ch < 3; ++ch) cellRgb[index * 3 + ch] = mixWeight > 0.0f ? mix[ch] / mixWeight : placeColor[ch];
                }
            }
            // Cells without fog take a fog-bearing neighbour's colour (else the place colour): the
            // density fades into them by the texture filter, and with their own (the zone's) colour
            // every fog patch got a rim in the zone's colour (red in Burning Steppes around white steam).
            auto byteOf = [](float v) { v *= 0.5f; return static_cast<uint8_t>((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f); };
            for (int r = 0; r < N; ++r)
            {
                auto* colour = static_cast<uint8_t*>(lc.pBits) + r * lc.Pitch;
                for (int c = 0; c < N; ++c, colour += 4)
                {
                    const int index = r * N + c;
                    const float* rgb = &cellRgb[index * 3];
                    if (!cellHas[index])
                        for (int dr = -1; dr <= 1; ++dr)
                            for (int dc = -1; dc <= 1; ++dc)
                            {
                                const int n = wrap(r + dr) * N + wrap(c + dc);
                                if (cellHas[n]) { rgb = &cellRgb[n * 3]; dr = 2; break; }
                            }
                    // A8R8G8B8 is B, G, R, A in memory; stored halved so colours up to 2 fit.
                    colour[0] = byteOf(rgb[2]);
                    colour[1] = byteOf(rgb[1]);
                    colour[2] = byteOf(rgb[0]);
                    colour[3] = cellPooled[index]; // the pool mask (the shader's pool gate), the cell's own
                }
            }
            g_field->UnlockRect(0);
            g_colorTex->UnlockRect(0);

            // The top map: per block of kTopBlock x kTopBlock cells, the highest top among it and its 8
            // neighbours (the march jumps at most one block, and the field's filtering reaches one
            // cell past a cell). Blocks line up with the field's wrap (kGridSize is a multiple), so it
            // is sampled with the field's own coordinates.
            {
                constexpr int B = kTopSize;
                static std::vector<float> blockTop(B * B);
                std::fill(blockTop.begin(), blockTop.end(), kNoTop);
                for (int r = 0; r < N; ++r)
                    for (int c = 0; c < N; ++c)
                    {
                        float& b = blockTop[(r / kTopBlock) * B + c / kTopBlock];
                        b = std::fmax(b, cellTop[r * N + c]);
                    }
                D3DLOCKED_RECT lt{};
                if (SUCCEEDED(g_topTex->LockRect(0, &lt, nullptr, 0)))
                {
                    for (int r = 0; r < B; ++r)
                    {
                        auto* row = reinterpret_cast<float*>(static_cast<uint8_t*>(lt.pBits) + r * lt.Pitch);
                        for (int c = 0; c < B; ++c)
                        {
                            float most = kNoTop;
                            for (int dr = -1; dr <= 1; ++dr)
                                for (int dc = -1; dc <= 1; ++dc)
                                    most = std::fmax(most, blockTop[((r + dr + B) % B) * B + (c + dc + B) % B]);
                            row[c] = most;
                            g_topDilated[r * B + c] = most;
                        }
                    }
                    g_topTex->UnlockRect(0);
                }
            }
            g_ceiling = ceiling;
            g_poolDensity = heavyWeight > 0.0f ? heavyDensity / heavyWeight : 0.0f;
            g_fieldVersion = grid.version;
            g_groundVersion = grid.groundVersion;
            g_fieldSignature = signature;
            ++g_fieldUploads;
            return true;
        }

        /// Our lights (env/Lights) in the fog: per field cell with fog in or next to its block, the
        /// light reaching the fog there (at the cell's ground + 1 yd), summed over the lights whose
        /// radius covers it. The same light as on the terrain and the cover (TerrainLights.cpp): a
        /// ~1 yd source on the client's 1 / (0.7 d + 0.03 d^2), windowed to 0 at the radius (its
        /// falloff shape), spot cone, baked occlusion; no facing term (fog scatters every way). Every
        /// frame (the flames flicker). nullptr: off or nothing lit.
        IDirect3DTexture9* UploadLights(IDirect3DDevice9* dev, const float cam[3])
        {
            const double t0 = grassperf::Now();
            g_lightsUsed = g_lightCells = 0;
            if (!g_settings.fogLights || g_settings.lightStrength <= 0.0f) return nullptr;
            const fog::Grid& grid = fog::Current();
            if (!grid.ground) return nullptr;
            constexpr int N = fog::kGridSize;
            static std::vector<float> sum(N * N * 3);
            std::fill(sum.begin(), sum.end(), 0.0f);
            auto wrap = [](int v) { const int m = v % N; return m < 0 ? m + N : m; };
            const bool skipIndoor = lights::Config().indoorSkipsTerrain != 0;
            const float range2 = g_settings.lightRange * g_settings.lightRange;

            for (const lights::ActiveLight& l : lights::Active())
            {
                if (l.indoor && skipIndoor) continue;
                const float cx = l.pos[0] - cam[0], cy = l.pos[1] - cam[1];
                if (cx * cx + cy * cy > range2 || l.radius <= 0.0f) continue;
                const float r = l.radius;
                const int i0 = std::max(grid.firstI, static_cast<int>(std::floor((l.pos[0] - r) / fog::kCellSize)));
                const int i1 = std::min(grid.firstI + N - 1, static_cast<int>(std::floor((l.pos[0] + r) / fog::kCellSize)));
                const int j0 = std::max(grid.firstJ, static_cast<int>(std::floor((l.pos[1] - r) / fog::kCellSize)));
                const int j1 = std::min(grid.firstJ + N - 1, static_cast<int>(std::floor((l.pos[1] + r) / fog::kCellSize)));
                if (i0 > i1 || j0 > j1) continue;
                bool used = false;
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i)
                    {
                        const int wi = wrap(i), wj = wrap(j);
                        if (g_topDilated[(wj / kTopBlock) * kTopSize + wi / kTopBlock] <= kNoTop) continue; // no fog near
                        const int index = wj * N + wi;
                        const float p[3] = { (i + 0.5f) * fog::kCellSize, (j + 0.5f) * fog::kCellSize, grid.ground[index] + 1.0f };
                        const float dx = p[0] - l.pos[0], dy = p[1] - l.pos[1], dz = p[2] - l.pos[2]; // light -> fog
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        const float d = std::sqrt(d2);
                        if (d >= r) continue;
                        float window = 1.0f - (d / r) * (d / r);
                        window *= window;
                        if (l.falloff != 1.0f) window = std::pow(window, l.falloff);
                        float spot = 1.0f;
                        if (l.cosOuter > -1.5f && d > 1.0e-4f)
                        {
                            const float c = (dx * l.spotDir[0] + dy * l.spotDir[1] + dz * l.spotDir[2]) / d;
                            spot = (c - l.cosOuter) * l.spotScale;
                            spot = spot < 0.0f ? 0.0f : (spot > 1.0f ? 1.0f : spot);
                            if (spot <= 0.0f) continue;
                        }
                        // Baked occlusion: the octahedral direction light -> fog, no light past the
                        // stored reach (+ 0.5 yd, soft over 0.5 yd), as the terrain's shader does.
                        float occluded = 1.0f;
                        if (l.occlusion && d > 1.0e-4f)
                        {
                            const float inv = 1.0f / (std::fabs(dx) + std::fabs(dy) + std::fabs(dz));
                            float ox = dx * inv, oy = dy * inv;
                            if (dz * inv < 0.0f)
                            {
                                const float tx = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
                                const float ty = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
                                ox = tx; oy = ty;
                            }
                            const int ix = std::min(lights::kOccSide - 1, static_cast<int>((ox + 1.0f) * 0.5f * lights::kOccSide));
                            const int iy = std::min(lights::kOccSide - 1, static_cast<int>((oy + 1.0f) * 0.5f * lights::kOccSide));
                            const float reach = l.occlusion[iy * lights::kOccSide + std::max(ix, 0)];
                            occluded = (reach + 1.0f - d) / 0.5f;
                            occluded = occluded < 0.0f ? 0.0f : (occluded > 1.0f ? 1.0f : occluded);
                            if (occluded <= 0.0f) continue;
                        }
                        const float dd = std::sqrt(d2 + 1.0f);
                        const float k = window * spot * occluded / (dd * (0.7f + 0.03f * dd));
                        float* s = &sum[index * 3];
                        s[0] += l.color[0] * k; s[1] += l.color[1] * k; s[2] += l.color[2] * k;
                        ++g_lightCells;
                        used = true;
                    }
                if (used) ++g_lightsUsed;
            }
            if (!g_lightsUsed) { g_lightMs = grassperf::Now() - t0; return nullptr; }

            if (!g_lightTex && FAILED(dev->CreateTexture(N, N, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &g_lightTex, nullptr)))
            {
                g_lightTex = nullptr;
                return nullptr;
            }
            D3DLOCKED_RECT lr{};
            if (FAILED(g_lightTex->LockRect(0, &lr, nullptr, 0))) return nullptr;
            const float strength = g_settings.lightStrength;
            for (int r = 0; r < N; ++r)
            {
                auto* texel = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch);
                for (int c = 0; c < N; ++c, texel += 4)
                {
                    const float* s = &sum[(r * N + c) * 3];
                    texel[0] = ToHalf(s[0] * strength);
                    texel[1] = ToHalf(s[1] * strength);
                    texel[2] = ToHalf(s[2] * strength);
                    texel[3] = 0;
                }
            }
            g_lightTex->UnlockRect(0);
            g_lightMs = grassperf::Now() - t0;
            return g_lightTex;
        }

        bool Invert(const float m[16], float out[16])
        {
            float inv[16];
            inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
            inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
            inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
            inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
            inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
            inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
            inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
            inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
            inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
            inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
            inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
            inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
            inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
            inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
            inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
            inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
            const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
            if (std::fabs(det) < 1e-12f) return false;
            for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
            return true;
        }

        float Luminance(const float c[3]) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

        /// A fog kind's colour where the player is (kind -1: the "all kinds" colour): the zone's stock
        /// fog colour (it follows the time of day), changed by FogSettings -- the kind's own row first,
        /// then the "all kinds" row. Hue mode keeps the stock colour's brightness and takes Color's hue
        /// and saturation, so day and night still show. The panel's colour test overrides every kind.
        void FogColor(int kind, float out[3])
        {
            const uint32_t argb = *reinterpret_cast<const uint32_t*>(kFogColor);
            const float stock[3] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f };
            const world::Snapshot& s = world::Current();
            fogsettings::Values v = fogsettings::Resolve(s.areaCount > 0 ? s.areaChain[0] : 0, s.mapId, kind);
            if (g_settings.colorTest)
            {
                v.colorMode = g_settings.colorMode;
                for (int i = 0; i < 3; ++i) v.color[i] = g_settings.color[i];
                v.saturation = g_settings.saturation;
                v.brightness = g_settings.colorBrightness;
            }

            float c[3] = { stock[0], stock[1], stock[2] };
            if (v.colorMode == fogsettings::kColorReplace)
                for (int i = 0; i < 3; ++i) c[i] = v.color[i];
            else if (v.colorMode == fogsettings::kColorHue)
            {
                const float wanted = Luminance(v.color);
                const float scale = wanted > 1.0e-4f ? Luminance(stock) / wanted : 0.0f;
                for (int i = 0; i < 3; ++i) c[i] = v.color[i] * scale;
            }
            const float grey = Luminance(c);
            const float brightness = v.brightness * g_settings.brightness;
            for (int i = 0; i < 3; ++i) out[i] = (grey + (c[i] - grey) * v.saturation) * brightness;
        }

        /// The panel's ground check: every cell's stored ground against a fresh terrain + liquid query
        /// at its centre (the same query the field fills it from). Too high = the case where the
        /// ungated pool term fills an empty cell with fog.
        struct GroundCheck
        {
            bool     done = false;
            unsigned unknown = 0, unknownNoQuery = 0, open = 0, floors = 0, structures = 0;
            unsigned tooHigh = 0, tooLow = 0;     // open/untraced cells off by > 0.7 yd
            float    worst = 0.0f, worstX = 0.0f, worstY = 0.0f, worstStored = 0.0f, worstFresh = 0.0f;
            float    unknownWorst = 0.0f;         // the fallback (player's ground) above the real ground, worst case
        };
        GroundCheck g_check;

        void CheckGround()
        {
            const fog::Grid& grid = fog::Current();
            g_check = GroundCheck{};
            if (!grid.ground || !grid.groundKnown) return;
            constexpr int N = fog::kGridSize;
            auto wrap = [](int v) { const int m = v % N; return m < 0 ? m + N : m; };
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c)
                {
                    const int index = r * N + c;
                    const int i = grid.firstI + wrap(c - grid.firstI), j = grid.firstJ + wrap(r - grid.firstJ);
                    const float x = (i + 0.5f) * fog::kCellSize, y = (j + 0.5f) * fog::kCellSize;
                    float fresh = 0.0f;
                    const bool have = terrain::HeightAt(x, y, fresh);
                    float liquid = 0.0f;
                    uint32_t type = 0;
                    if (have && terrain::LiquidAt(x, y, liquid, type) && liquid > fresh) fresh = liquid;
                    const float stored = grid.ground[index];
                    if (!grid.groundKnown[index])
                    {
                        ++g_check.unknown;
                        if (!have) ++g_check.unknownNoQuery;
                        else if (stored - fresh > g_check.unknownWorst) g_check.unknownWorst = stored - fresh;
                        continue;
                    }
                    const uint8_t block = grid.block ? grid.block[index] : 0;
                    if (block == 3) { ++g_check.structures; continue; }
                    if (block == 2) { ++g_check.floors; continue; } // a WMO floor: not comparable with the terrain
                    ++g_check.open;
                    if (!have) continue;
                    const float off = stored - fresh;
                    if (off > 0.7f) ++g_check.tooHigh;
                    else if (off < -0.7f) ++g_check.tooLow;
                    if (std::fabs(off) > std::fabs(g_check.worst))
                    {
                        g_check.worst = off; g_check.worstX = x; g_check.worstY = y;
                        g_check.worstStored = stored; g_check.worstFresh = fresh;
                    }
                }
            g_check.done = true;
        }

        bool AnyFog()
        {
            const fog::Grid& grid = fog::Current();
            for (int k = 0; k < fog::kKindCount; ++k)
                if (fog::Settings(k).enabled && grid.maxAmount[k] > 0.0f && fog::Settings(k).density > 0.0f) return true;
            return false;
        }

        const char* FindConflict()
        {
            for (const char* dll : kConflicts)
                if (GetModuleHandleA(dll)) return dll;
            return nullptr;
        }

        /// Whether the fog draws this frame. Decided before the water, so the water's depth writes
        /// follow it.
        bool Decide()
        {
            if ((g_conflictCheck++ % 300) == 0)
            {
                const char* found = FindConflict();
                if (found && found != g_conflict)
                    g_api->Log(WXL_LOG_WARN, kTag, "ground fog: '%s' is loaded (it draws its own fog/water); the ground fog gives way and stays off.", found);
                g_conflict = found;
            }
            if (!g_settings.enabled)             { g_inactive = "switched off"; return false; }
            if (g_conflict)                      { g_inactive = "another fog/water mod is loaded (see the log); giving way"; return false; }
            if (!depth::Texture())               { g_inactive = "no readable scene depth (see the scene depth panel)"; return false; }
            if (*reinterpret_cast<const uint32_t*>(kCameraInLiquid)) { g_inactive = "camera under water"; return false; }
            if (!AnyFog() && !g_settings.heightBands && g_settings.debugView != 3) { g_inactive = "no fog in the field (deposit some)"; return false; }
            if (*reinterpret_cast<const float*>(kInteriorBlend) >= 0.999f) { g_inactive = "indoors"; return false; }
            return true;
        }

        void Draw(IDirect3DDevice9* dev)
        {
            prof::Scope scope(prof::kOursFogDraw);
            const double t0 = grassperf::Now();

            if (dev != g_device) { ReleaseGpu(); g_shaderFailed = false; g_device = dev; }
            if (!g_ps && !g_shaderFailed) { g_ps = Compile(dev); g_shaderFailed = !g_ps; }
            if (!g_ps) { g_inactive = "the fog shader failed to compile (see the log)"; return; }
            if (!g_noise) g_noise = MakeNoise(dev);
            if (!g_noise) { g_inactive = "couldn't create the noise volume"; return; }

            float view[16], proj[16], invView[16];
            if (!wxl::game::gfx::SceneMatrices(view, proj) || !Invert(view, invView)) { g_inactive = "no scene matrices"; return; }
            // The scene view is camera-relative (its inverse puts the eye at the origin): add the camera.
            // The eye is right (in-client 2026-10-09: the camera builder 0x795400(eye, target) stores
            // it at 0xCD8F5C; with the player instead, the fog slid while orbiting).
            float cam[3];
            wxl::game::camera::GetPosition(cam);
            if (std::fabs(invView[12]) + std::fabs(invView[13]) + std::fabs(invView[14]) < 1.0f)
            {
                invView[12] += cam[0];
                invView[13] += cam[1];
                invView[14] += cam[2];
            }

            const float groundRef = std::floor(invView[14] / 32.0f) * 32.0f;
            if (!UploadField(dev, groundRef)) { g_inactive = "couldn't upload the fog field"; return; }

            if (!depth::Refresh(dev)) { g_inactive = "the depth copy failed"; return; }
            auto* depthTex = static_cast<IDirect3DTexture9*>(depth::Texture());

            IDirect3DSurface9* rt = nullptr;
            if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
            D3DSURFACE_DESC rtDesc{};
            rt->GetDesc(&rtDesc);
            rt->Release();
            D3DVIEWPORT9 vp{};
            dev->GetViewport(&vp);
            g_seenViewport = vp;
            g_seenM22 = proj[10];
            g_seenM32 = proj[14];
            if (!vp.Width || !vp.Height || vp.MaxZ <= vp.MinZ) { g_inactive = "unexpected viewport"; return; }

            // The noise drifts with the fog itself: the area's steady ground wind at the speed the
            // field moves (env/Fog Advect: 4 yd/s x wind x WindDrift, the drifting kinds' largest), and a
            // slow rise, in tiles. (It used the gusty wind at the camera at its own speed, so the fog
            // moved through a pattern moving otherwise.)
            const ULONGLONG now = GetTickCount64();
            const float dt = g_lastTick ? static_cast<float>(now - g_lastTick) / 1000.0f : 0.0f;
            g_lastTick = now;
            if (dt > 0.0f && dt < 1.0f && !fog::Paused()) // paused, the field holds still: so does its noise
            {
                const fog::RulesInputs& in = fog::Inputs();
                float drift = 0.0f;
                for (int k = 0; k < fog::kKindCount; ++k)
                    if (fog::Settings(k).enabled && fog::Current().maxAmount[k] > 0.0f) drift = std::fmax(drift, fog::Settings(k).windDrift);
                const float speed = 4.0f * in.wind * drift; // yd/s
                const float tile = g_settings.noiseTileYards > 1.0f ? g_settings.noiseTileYards : 1.0f;
                g_noiseOffset[0] = std::fmod(g_noiseOffset[0] - in.windDirX * speed * dt / tile, 64.0f);
                g_noiseOffset[1] = std::fmod(g_noiseOffset[1] - in.windDirY * speed * dt / tile, 64.0f);
                g_noiseOffset[2] = std::fmod(g_noiseOffset[2] - 0.05f * dt / tile, 64.0f);
            }

            const fog::Grid& grid = fog::Current();
            const float extent = fog::kGridSize * fog::kCellSize;
            const float interior = *reinterpret_cast<const float*>(kInteriorBlend);
            const int steps = g_settings.steps < 4 ? 4 : (g_settings.steps > 128 ? 128 : g_settings.steps);

            // Our lights in the fog (nullptr when off or nothing lit).
            IDirect3DTexture9* lightTex = UploadLights(dev, cam);

            // Units pushing the fog: their wake map (nullptr when off or none near).
            IDirect3DTexture9* wake = fogwake::Texture(dev, g_groundRef);
            float wakePlace[3] = {};
            fogwake::Placement(wakePlace);

            const float c[16 * 4] = {
                vp.MinZ, 1.0f / (vp.MaxZ - vp.MinZ), vp.MaxZ + 1.0e-4f, g_settings.maxDistance,
                proj[0], proj[5], proj[8], proj[9],
                proj[10], proj[14], static_cast<float>(steps), g_settings.depthDecode ? 1.0f : 0.0f,
                invView[0], invView[1], invView[2], 0.0f,
                invView[4], invView[5], invView[6], 0.0f,
                invView[8], invView[9], invView[10], 0.0f,
                invView[12], invView[13], invView[14], g_groundRef,
                1.0f / extent, g_settings.dither, 0.0f, 0.0f,
                grid.firstI * fog::kCellSize, grid.firstJ * fog::kCellSize,
                (grid.firstI + fog::kGridSize) * fog::kCellSize, (grid.firstJ + fog::kGridSize) * fog::kCellSize,
                lightTex ? 1.0f : 0.0f, 0.0f, 0.0f, // (colours come from the colour texture)
                g_settings.densityScale * (1.0f - (interior > 0.0f ? interior : 0.0f)),
                g_noiseOffset[0], g_noiseOffset[1], g_noiseOffset[2], 1.0f / (g_settings.noiseTileYards > 1.0f ? g_settings.noiseTileYards : 1.0f),
                g_settings.noiseAmount, 1.0f / 16.0f, g_settings.debugColour ? 1.0f : 0.0f, g_settings.heightBands ? 1.0f : 0.0f,
                static_cast<float>(vp.X) / rtDesc.Width, static_cast<float>(vp.Y) / rtDesc.Height,
                static_cast<float>(vp.Width) / rtDesc.Width, static_cast<float>(vp.Height) / rtDesc.Height,
                g_poolDensity, 0.3f, g_ceiling, g_settings.poolGate ? 1.0f : 0.0f,
                static_cast<float>(g_settings.debugView), g_settings.fogStep > 0.1f ? g_settings.fogStep : 0.1f,
                kTopBlock * fog::kCellSize, 0.04f,
                wakePlace[0], wakePlace[1], wakePlace[2], wake ? 1.0f : 0.0f,
            };

            // Everything the pass changes goes back through the state block, so the client's own cache
            // stays true. The depth binding isn't part of a state block: an INTZ surface can't be bound
            // while it's sampled, so the client's original surface stands in (Active); multisampled
            // (Copied), the client's own surface is bound already and we read our copy.
            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
            IDirect3DSurface9* boundDepth = nullptr;
            dev->GetDepthStencilSurface(&boundDepth);
            if (depth::CurrentState() == depth::State::Active)
                dev->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(depth::OriginalSurface()));

            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x07);

            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(g_ps);
            dev->SetPixelShaderConstantF(0, c, 16);

            dev->SetTexture(0, depthTex);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(1, g_field);
            dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            dev->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(2, g_noise);
            dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            dev->SetSamplerState(2, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
            dev->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(3, g_colorTex);
            dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            dev->SetSamplerState(3, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(4, g_topTex);
            dev->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            dev->SetSamplerState(4, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(6, lightTex);
            dev->SetSamplerState(6, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(6, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(6, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(6, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(6, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            dev->SetSamplerState(6, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(5, wake);
            dev->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, FALSE);

            // The viewport, in pixels; the half-pixel offset lines pixels up with D3D9 pixel centres.
            const float x0 = vp.X - 0.5f, y0 = vp.Y - 0.5f, x1 = x0 + vp.Width, y1 = y0 + vp.Height;
            struct Vtx { float x, y, z, rhw, u, v; };
            const Vtx quad[4] = {
                { x0, y0, 0.0f, 1.0f, 0.0f, 0.0f },
                { x1, y0, 0.0f, 1.0f, 1.0f, 0.0f },
                { x0, y1, 0.0f, 1.0f, 0.0f, 1.0f },
                { x1, y1, 0.0f, 1.0f, 1.0f, 1.0f },
            };
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));

            saved->Apply();
            saved->Release();
            if (boundDepth)
            {
                dev->SetDepthStencilSurface(boundDepth);
                boundDepth->Release();
            }
            ++g_draws;
            g_inactive = nullptr;
            g_lastMs = grassperf::Now() - t0;
        }

        // ------------------------------------------------------------------ hooks
        void __fastcall hkLiquid(void* bank, void* edx, void* transform, int passType)
        {
            if (passType == 1) g_drawThisFrame = Decide();
            const int saved = g_liquidPass;
            g_liquidPass = passType;
            g_origLiquid(bank, edx, transform, passType);
            g_liquidPass = saved;
        }

        void __fastcall hkDraw(void* device, void* edx, uint32_t* batch, int indexed)
        {
            if (g_liquidPass == 1 && g_drawThisFrame && g_settings.forceWaterDepth) gxstate::Force(gxstate::kDepthWrite, 1);
            g_origDraw(device, edx, batch, indexed);
        }

        void __fastcall hkWeather(void* self, void* edx)
        {
            // Above water the order is: water -> weather -> see-through models; under water the weather
            // comes before the water, and the fog stays off there (Decide).
            if (g_drawThisFrame || Decide())
                if (auto* dev = static_cast<IDirect3DDevice9*>(gx::RawDevice())) Draw(dev);
            g_drawThisFrame = false;
            g_origWeather(self, edx);
        }

        template <typename Fn>
        int AttachFront(const WXL_Api* api, const char* point, Fn detour, Fn* original)
        {
            // In front of the default priority (core's normal-phase features go in behind us); behind
            // only if the chain is already live.
            return api->HookAttachByName(point, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), -1000) ||
                   api->HookAttachByName(point, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), 1000);
        }
    }

    Settings& Config() { return g_settings; }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        const int a = AttachFront(api, "Weather.Render", &hkWeather, &g_origWeather);
        const int b = AttachFront(api, "Gx.LiquidRenderPass", &hkLiquid, &g_origLiquid);
        const int c = AttachFront(api, "Gx.DeviceDraw", &hkDraw, &g_origDraw);
        if (!a) g_settings.enabled = 0;
        api->Log((a && b && c) ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "ground fog: hooks weather %d liquid %d draw %d%s",
                 a, b, c, a ? "" : " (no draw point: the fog stays off)");
    }

    void DrawPanelSection()
    {
        char line[256];
        g_api->UiCheckbox("draw the fog##gfon", &g_settings.enabled);
        g_api->UiSliderInt("march step budget##gfsteps", &g_settings.steps, 4, 128);
        g_api->UiSliderFloat("step length in the fog (yd)##gfstep", &g_settings.fogStep, 0.25f, 4.0f);
        g_api->UiSliderFloat("dither (0 smooth .. 1 grainy)##gfdither", &g_settings.dither, 0.0f, 1.0f);
        g_api->UiSliderInt("fog floor reach (cells of 2 yd; 0 = follows every cell)##gffloor", &g_settings.floorReach, 0, 4);
        if (g_api->UiCollapsingHeader("Units pushing the fog##gfwake")) fogwake::DrawPanelSection(g_api);
        if (g_api->UiCollapsingHeader("Lights in the fog##gflights"))
        {
            g_api->UiCheckbox("our lights light the fog##gflon", &g_settings.fogLights);
            g_api->UiSliderFloat("strength##gflstr", &g_settings.lightStrength, 0.0f, 4.0f);
            g_api->UiSliderFloat("range (yd from the camera)##gflrange", &g_settings.lightRange, 16.0f, 256.0f);
            std::snprintf(line, sizeof(line), "%u lights reach the fog, %u cell hits, %.3f ms CPU (every frame: they flicker)",
                          g_lightsUsed, g_lightCells, g_lightMs);
            g_api->UiTextWrapped(line);
        }
        g_api->UiSliderFloat("max distance (yd)##gfdist", &g_settings.maxDistance, 30.0f, 600.0f);
        g_api->UiSliderFloat("density scale##gfdens", &g_settings.densityScale, 0.0f, 4.0f);
        g_api->UiSliderFloat("patchiness (noise)##gfnoise", &g_settings.noiseAmount, 0.0f, 1.0f);
        g_api->UiSliderFloat("noise tile (yd)##gftile", &g_settings.noiseTileYards, 8.0f, 256.0f);
        g_api->UiSliderFloat("colour brightness##gfbright", &g_settings.brightness, 0.0f, 3.0f);
        g_api->UiCheckbox("water writes depth while fog draws##gfwater", &g_settings.forceWaterDepth);
        g_api->UiCheckbox("debug: magenta fog##gfdebug", &g_settings.debugColour);
        g_api->UiCheckbox("debug: height bands (must stay on the terrain when the camera turns)##gfbands", &g_settings.heightBands);
        static const char* const kDecode[] = { "OpenGL-style (-1..1)", "D3D-style (0..1)" };
        g_api->UiCombo("depth decoding##gfdecode", &g_settings.depthDecode, kDecode, 2);
        g_api->UiCheckbox("pool fog only where a pool lies (off: the old way, for comparison)##gfgate", &g_settings.poolGate);
        static const char* const kViews[] = { "the fog", "light layer only", "heavy pools only",
                                              "ground check (white pool, red = pool fog without a pool, green ok, blue above)" };
        g_api->UiCombo("debug view##gfview", &g_settings.debugView, kViews, 4);
        if (g_api->UiButton("Check the field's ground against the terrain##gfcheck")) CheckGround();
        if (g_check.done)
        {
            std::snprintf(line, sizeof(line), "ground check: %u open cells, %u stored > 0.7 yd too high, %u too low; worst %+.2f yd at %.1f, %.1f "
                                              "(stored %.2f, terrain/liquid now %.2f); %u WMO floors, %u structures (not compared); "
                                              "%u not known yet (%u can't be queried: holes/unloaded; the others' placeholder up to %.2f yd above the real ground)",
                          g_check.open, g_check.tooHigh, g_check.tooLow, g_check.worst, g_check.worstX, g_check.worstY,
                          g_check.worstStored, g_check.worstFresh, g_check.floors, g_check.structures,
                          g_check.unknown, g_check.unknownNoQuery, g_check.unknownWorst);
            g_api->UiTextWrapped(line);
        }
        {
            const float m22 = g_seenM22, m32 = g_seenM32;
            std::snprintf(line, sizeof(line), "last draw: viewport x %lu y %lu w %lu h %lu, z %.4f..%.4f; projection m22 %.6f m32 %.4f -> "
                                              "near/far if OpenGL %.3f / %.1f, if D3D %.3f / %.1f",
                          g_seenViewport.X, g_seenViewport.Y, g_seenViewport.Width, g_seenViewport.Height, g_seenViewport.MinZ, g_seenViewport.MaxZ,
                          m22, m32, m32 / (-1.0f - m22), m32 / (1.0f - m22), m22 != 0.0f ? -m32 / m22 : 0.0f, m32 / (1.0f - m22));
            g_api->UiTextWrapped(line);
        }
        {
            const world::Snapshot& s = world::Current();
            const fogsettings::Values& v = fogsettings::Resolve(s.areaCount > 0 ? s.areaChain[0] : 0, s.mapId, -1);
            static const char* const kModes[] = { "stock", "hue", "replace" };
            std::snprintf(line, sizeof(line), "colour here (FogSettings, all kinds): %s, colour %.2f %.2f %.2f, saturation %.2f, brightness %.2f",
                          kModes[v.colorMode], v.color[0], v.color[1], v.color[2], v.saturation, v.brightness);
            g_api->UiTextWrapped(line);
            g_api->UiCheckbox("colour test (instead of the table; not saved)##gfctest", &g_settings.colorTest);
            if (g_settings.colorTest)
            {
                g_api->UiCombo("mode##gfcmode", &g_settings.colorMode, kModes, 3);
                g_api->UiColorEdit("colour##gfccol", g_settings.color);
                g_api->UiSliderFloat("saturation##gfcsat", &g_settings.saturation, 0.0f, 1.5f);
                g_api->UiSliderFloat("brightness##gfcbright", &g_settings.colorBrightness, 0.0f, 3.0f);
            }
        }
        if (g_inactive)
            std::snprintf(line, sizeof(line), "renderer: not drawing -- %s", g_inactive);
        else
            std::snprintf(line, sizeof(line), "renderer: drawing, %.3f ms CPU (GPU time: frame profiler, 'ours: fog draw'); %u frames, %u field uploads",
                          g_lastMs, g_draws, g_fieldUploads);
        g_api->UiTextWrapped(line);
        {
            // The fog's column where the player stands: is the player inside a pool, and how deep is it?
            const world::Snapshot& s = world::Current();
            float ground = 0.0f, pool = 0.0f, light = 0.0f;
            int block = 0;
            const bool known = fog::GroundAt(s.playerPos[0], s.playerPos[1], ground, block);
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                const fog::KindSettings& ks = fog::Settings(k);
                if (!ks.enabled) continue;
                const float a = fog::AmountAt(k, s.playerPos[0], s.playerPos[1]);
                pool += a * ks.weight * ks.thickness;
                light += a * ks.density * (1.0f - ks.weight);
            }
            float cam[3];
            wxl::game::camera::GetPosition(cam);
            if (known)
                std::snprintf(line, sizeof(line), "your column: ground %.1f (WMO state %d), heavy pool depth %.2f yd -> surface %.1f, pool density %.3f/yd, "
                                                  "light density %.3f/yd; fog ceiling %.1f; you %.1f, camera %.1f",
                              ground, block, pool, ground + pool, g_poolDensity, light, g_ceiling, s.playerPos[2], cam[2]);
            else
                std::snprintf(line, sizeof(line), "your column: not known yet");
            g_api->UiTextWrapped(line);
        }
        const fog::Grid& grid = fog::Current();
        std::snprintf(line, sizeof(line), "ground under the field: %u of %d cells not known yet (unloaded terrain or holes)",
                      grid.groundMissing, fog::kGridSize * fog::kGridSize);
        g_api->UiTextWrapped(line);
    }

    void OnDeviceLost()
    {
        // Managed resources survive a reset; only the shader is tied to the device object, and a
        // reset keeps the same device. Nothing to drop here.
    }
}
