#include "GroundFog.hpp"

#include "../debug/Profiler.hpp"
#include "../env/Fog.hpp"
#include "../env/FogSettings.hpp"
#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"
#include "../render/GxState.hpp"
#include "../render/SceneDepth.hpp"
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
        IDirect3DVolumeTexture9* g_noise = nullptr;
        bool                     g_shaderFailed = false;
        uint32_t g_fieldVersion = ~0u, g_groundVersion = ~0u;
        float    g_fieldSignature = -1.0f;
        float    g_groundRef = 0.0f;

        // ------------------------------------------------------------------ shader
        const char* const kPsHlsl = R"(
sampler2D depthTex : register(s0);
sampler2D fieldTex : register(s1);   // r = density per yd at the ground, g = layer thickness (yd), b = ground z - cCam.w
sampler3D noiseTex : register(s2);
float4 cDepth  : register(c0);  // x = viewport min z, y = 1 / (max z - min z), z = deepest world depth, w = max distance
float4 cProj   : register(c1);  // projection m00, m11, m20, m21
float4 cProj2  : register(c2);  // projection m22, m32, steps, -
float4 cInv0   : register(c3);  // inverse view, rows (row vectors): world = v.x r0 + v.y r1 + v.z r2 + r3
float4 cInv1   : register(c4);
float4 cInv2   : register(c5);
float4 cCam    : register(c6);  // xyz = r3 = the camera in world space, w = ground reference z
float4 cGrid   : register(c7);  // x = 1 / the field's extent (yd)
float4 cBox    : register(c8);  // the field's valid box: min x, min y, max x, max y
float4 cColor  : register(c9);  // rgb = fog colour, a = density scale
float4 cNoise  : register(c10); // xyz = drift offset (tiles), w = 1 / tile size (yd)
float4 cNoise2 : register(c11); // x = noise amount, y = 1 / edge fade (yd), z = debug colour
float4 cUv     : register(c12); // depth texture uv = xy + uv * zw

float Patchiness(float3 p)
{
    float3 q = p * cNoise.w + cNoise.xyz;
    float a = tex3Dlod(noiseTex, float4(q, 0)).r;
    float b = tex3Dlod(noiseTex, float4(q * 2.71 + 0.37, 0)).r;
    float n = a * 0.65 + b * 0.35;
    return lerp(1.0, 2.0 * saturate((n - 0.5) * 2.5 + 0.5), cNoise2.x); // mean ~1
}

float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    float d = tex2Dlod(depthTex, float4(cUv.xy + uv * cUv.zw, 0, 0)).r;
    float ndcZ = saturate((d - cDepth.x) * cDepth.y);
    float viewZ = cProj2.y / min(ndcZ - cProj2.x, -1e-6);
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 v = float3((ndc.x - cProj.z) * viewZ / cProj.x, (ndc.y - cProj.w) * viewZ / cProj.y, viewZ);
    float3 world = v.x * cInv0.xyz + v.y * cInv1.xyz + v.z * cInv2.xyz + cCam.xyz;

    float3 ray = world - cCam.xyz;
    float dist = length(ray);
    float3 dir = ray / max(dist, 1e-4);
    if (d > cDepth.z) dist = cDepth.w; // sky and the distant terrain beyond the world viewport
    float len = min(dist, cDepth.w);

    int steps = (int)cProj2.z;
    float stepLen = len / cProj2.z;
    float t = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715)))) * stepLen;
    float tau = 0;
    [loop] for (int i = 0; i < 64; ++i)
    {
        if (i >= steps) break;
        float3 p = cCam.xyz + dir * t;
        float4 f = tex2Dlod(fieldTex, float4(p.xy * cGrid.x, 0, 0));
        float edge = saturate(min(min(p.x - cBox.x, cBox.z - p.x), min(p.y - cBox.y, cBox.w - p.y)) * cNoise2.y);
        float h = max(p.z - (f.b + cCam.w), 0);
        float sigma = f.r * exp(-h / max(f.g, 0.25)) * edge;
        [branch] if (sigma > 1e-5) tau += sigma * Patchiness(p) * stepLen;
        t += stepLen;
    }
    float T = exp(-tau * cColor.a);
    float3 col = cNoise2.z > 0.5 ? float3(1, 0, 1) : cColor.rgb;
    return float4(col * (1 - T), T);
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
            float s = g_settings.densityScale * 7.0f;
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                const fog::KindSettings& ks = fog::Settings(k);
                s += (ks.enabled ? 1.0f : 0.0f) * (k + 1) * 13.0f + ks.density * (k + 3) * 101.0f + ks.thickness * (k + 5) * 0.37f;
            }
            return s;
        }

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
            const float signature = SettingsSignature();
            if (grid.version == g_fieldVersion && grid.groundVersion == g_groundVersion && signature == g_fieldSignature &&
                std::fabs(groundRef - g_groundRef) < 64.0f)
                return true;

            // The fog lies on the lowest ground among a cell and its 8 neighbours: at a steep edge (a
            // cliff over lava) the 2 yd cells interpolate a ramp the thin layer would climb as a
            // sheet; on the lower envelope it stays at the bottom. Gentle ground barely changes.
            constexpr int N = fog::kGridSize;
            static std::vector<float> base(N * N);
            auto wrap = [](int v) { const int m = v % N; return m < 0 ? m + N : m; };
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c)
                {
                    const int i = grid.firstI + wrap(c - grid.firstI), j = grid.firstJ + wrap(r - grid.firstJ); // world cell
                    float low = grid.ground[r * N + c];
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di)
                        {
                            const int ni = i + di, nj = j + dj;
                            if (ni < grid.firstI || ni >= grid.firstI + N || nj < grid.firstJ || nj >= grid.firstJ + N) continue;
                            const int n = wrap(nj) * N + wrap(ni);
                            if (grid.groundKnown[n] && grid.ground[n] < low) low = grid.ground[n];
                        }
                    base[r * N + c] = low;
                }

            D3DLOCKED_RECT lr{};
            if (FAILED(g_field->LockRect(0, &lr, nullptr, 0))) return false;
            g_groundRef = groundRef;
            for (int r = 0; r < fog::kGridSize; ++r)
            {
                auto* texel = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch);
                for (int c = 0; c < fog::kGridSize; ++c, texel += 4)
                {
                    const int index = r * fog::kGridSize + c;
                    float density = 0.0f, weighted = 0.0f;
                    for (int k = 0; k < fog::kKindCount; ++k)
                    {
                        const fog::KindSettings& ks = fog::Settings(k);
                        if (!ks.enabled) continue;
                        const float d = grid.amount[k][index] * ks.density;
                        density += d;
                        weighted += d * ks.thickness;
                    }
                    const float thickness = density > 0.0f ? weighted / density : 1.0f;
                    texel[0] = ToHalf(density);
                    texel[1] = ToHalf(thickness);
                    texel[2] = ToHalf(base[index] - g_groundRef);
                    texel[3] = 0;
                }
            }
            g_field->UnlockRect(0);
            g_fieldVersion = grid.version;
            g_groundVersion = grid.groundVersion;
            g_fieldSignature = signature;
            ++g_fieldUploads;
            return true;
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

        /// The fog's colour where the player is: the zone's stock fog colour (it follows the time of
        /// day), changed by FogSettings' "all kinds" rows of the player's place. Hue mode keeps the
        /// stock colour's brightness and takes Color's hue and saturation, so day and night still show.
        void FogColor(float out[3])
        {
            const uint32_t argb = *reinterpret_cast<const uint32_t*>(kFogColor);
            const float stock[3] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f };
            const world::Snapshot& s = world::Current();
            fogsettings::Values v = fogsettings::Resolve(s.areaCount > 0 ? s.areaChain[0] : 0, s.mapId, -1);
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
            if (!AnyFog())                       { g_inactive = "no fog in the field (deposit some)"; return false; }
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
            if (!vp.Width || !vp.Height || vp.MaxZ <= vp.MinZ) { g_inactive = "unexpected viewport"; return; }

            // Noise drift with the wind at the camera (and a slow rise), in tiles.
            const ULONGLONG now = GetTickCount64();
            const float dt = g_lastTick ? static_cast<float>(now - g_lastTick) / 1000.0f : 0.0f;
            g_lastTick = now;
            if (dt > 0.0f && dt < 1.0f)
            {
                const wind::Sample w = wind::At(cam);
                const float speed = 0.5f + 6.0f * w.strength; // yd/s
                const float tile = g_settings.noiseTileYards > 1.0f ? g_settings.noiseTileYards : 1.0f;
                g_noiseOffset[0] = std::fmod(g_noiseOffset[0] - w.dirX * speed * dt / tile, 64.0f);
                g_noiseOffset[1] = std::fmod(g_noiseOffset[1] - w.dirY * speed * dt / tile, 64.0f);
                g_noiseOffset[2] = std::fmod(g_noiseOffset[2] - 0.05f * dt / tile, 64.0f);
            }

            const fog::Grid& grid = fog::Current();
            const float extent = fog::kGridSize * fog::kCellSize;
            float color[3];
            FogColor(color);
            const float interior = *reinterpret_cast<const float*>(kInteriorBlend);
            const int steps = g_settings.steps < 2 ? 2 : (g_settings.steps > 64 ? 64 : g_settings.steps);

            const float c[13 * 4] = {
                vp.MinZ, 1.0f / (vp.MaxZ - vp.MinZ), vp.MaxZ + 1.0e-4f, g_settings.maxDistance,
                proj[0], proj[5], proj[8], proj[9],
                proj[10], proj[14], static_cast<float>(steps), 0.0f,
                invView[0], invView[1], invView[2], 0.0f,
                invView[4], invView[5], invView[6], 0.0f,
                invView[8], invView[9], invView[10], 0.0f,
                invView[12], invView[13], invView[14], g_groundRef,
                1.0f / extent, 0.0f, 0.0f, 0.0f,
                grid.firstI * fog::kCellSize, grid.firstJ * fog::kCellSize,
                (grid.firstI + fog::kGridSize) * fog::kCellSize, (grid.firstJ + fog::kGridSize) * fog::kCellSize,
                color[0], color[1], color[2],
                g_settings.densityScale * (1.0f - (interior > 0.0f ? interior : 0.0f)),
                g_noiseOffset[0], g_noiseOffset[1], g_noiseOffset[2], 1.0f / (g_settings.noiseTileYards > 1.0f ? g_settings.noiseTileYards : 1.0f),
                g_settings.noiseAmount, 1.0f / 16.0f, g_settings.debugColour ? 1.0f : 0.0f, 0.0f,
                static_cast<float>(vp.X) / rtDesc.Width, static_cast<float>(vp.Y) / rtDesc.Height,
                static_cast<float>(vp.Width) / rtDesc.Width, static_cast<float>(vp.Height) / rtDesc.Height,
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
            dev->SetPixelShaderConstantF(0, c, 13);

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
        g_api->UiSliderInt("march steps##gfsteps", &g_settings.steps, 2, 64);
        g_api->UiSliderFloat("max distance (yd)##gfdist", &g_settings.maxDistance, 30.0f, 600.0f);
        g_api->UiSliderFloat("density scale##gfdens", &g_settings.densityScale, 0.0f, 4.0f);
        g_api->UiSliderFloat("patchiness (noise)##gfnoise", &g_settings.noiseAmount, 0.0f, 1.0f);
        g_api->UiSliderFloat("noise tile (yd)##gftile", &g_settings.noiseTileYards, 8.0f, 256.0f);
        g_api->UiSliderFloat("colour brightness##gfbright", &g_settings.brightness, 0.0f, 3.0f);
        g_api->UiCheckbox("water writes depth while fog draws##gfwater", &g_settings.forceWaterDepth);
        g_api->UiCheckbox("debug: magenta fog##gfdebug", &g_settings.debugColour);
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
