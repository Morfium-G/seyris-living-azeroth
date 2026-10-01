#include "PostAA.hpp"

#include "../features/GrassPerf.hpp"

#include "engine/events/Event.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>

namespace wxl_livingazeroth::postaa
{
    namespace
    {
        namespace ev = wxl::events;
        namespace gx = wxl::game::gx;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // c0 = (1/width, 1/height, mode, subpixel), c1.x = edge threshold.
        // Mode: 1 pass-through, 2 FXAA, 3 edges shown in red.
        const char* kFxaaHlsl = R"(
sampler2D scene : register(s0);
float4 c0 : register(c0);
float4 c1 : register(c1);

float Luma(float2 uv) { return dot(tex2Dlod(scene, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 rgbM = tex2Dlod(scene, float4(uv, 0, 0)).rgb;
    if (c0.z < 1.5) return float4(rgbM, 1);

    float2 px = c0.xy;
    float lM = dot(rgbM, float3(0.299, 0.587, 0.114));
    float lN = Luma(uv + float2(0, -px.y));
    float lS = Luma(uv + float2(0,  px.y));
    float lE = Luma(uv + float2( px.x, 0));
    float lW = Luma(uv + float2(-px.x, 0));

    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * c1.x)) return float4(rgbM, 1);
    if (c0.z > 2.5) return float4(1, 0, 0, 1);

    float lNW = Luma(uv + float2(-px.x, -px.y));
    float lNE = Luma(uv + float2( px.x, -px.y));
    float lSW = Luma(uv + float2(-px.x,  px.y));
    float lSE = Luma(uv + float2( px.x,  px.y));

    // Edge orientation: horizontal when the vertical gradient dominates.
    float edgeH = abs(lNW + lNE - 2 * lN) + 2 * abs(lW + lE - 2 * lM) + abs(lSW + lSE - 2 * lS);
    float edgeV = abs(lNW + lSW - 2 * lW) + 2 * abs(lN + lS - 2 * lM) + abs(lNE + lSE - 2 * lE);
    bool horz = edgeH >= edgeV;

    // Which side of the pixel the edge lies on.
    float l1 = horz ? lN : lW;
    float l2 = horz ? lS : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    float stepLen = horz ? px.y : px.x;
    float lLocal, grad;
    if (g1 >= g2) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); grad = g1; }
    else          { lLocal = 0.5 * (l2 + lM); grad = g2; }

    float2 uvEdge = uv;
    if (horz) uvEdge.y += stepLen * 0.5; else uvEdge.x += stepLen * 0.5;
    float2 dir = horz ? float2(px.x, 0) : float2(0, px.y);

    // Walk both ways along the edge until its contrast ends.
    float gradScaled = grad * 0.25;
    float2 uv1 = uvEdge - dir, uv2 = uvEdge + dir;
    float e1 = Luma(uv1) - lLocal, e2 = Luma(uv2) - lLocal;
    bool done1 = abs(e1) >= gradScaled, done2 = abs(e2) >= gradScaled;
    [loop] for (int i = 0; i < 12 && !(done1 && done2); ++i)
    {
        float stride = i < 4 ? 1.0 : (i < 8 ? 2.0 : 4.0);
        if (!done1) { uv1 -= dir * stride; e1 = Luma(uv1) - lLocal; done1 = abs(e1) >= gradScaled; }
        if (!done2) { uv2 += dir * stride; e2 = Luma(uv2) - lLocal; done2 = abs(e2) >= gradScaled; }
    }

    float dist1 = horz ? uv.x - uv1.x : uv.y - uv1.y;
    float dist2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool towards1 = dist1 < dist2;
    float edgeLen = dist1 + dist2;
    float pixelOffset = -min(dist1, dist2) / edgeLen + 0.5;
    bool centerSmaller = lM < lLocal;
    bool correct = ((towards1 ? e1 : e2) < 0) != centerSmaller;
    float offset = correct ? pixelOffset : 0;

    // Subpixel aliasing: blend by how much the centre differs from its 3x3 neighbourhood.
    float lAvg = (2 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12;
    float sub = saturate(abs(lAvg - lM) / range);
    sub = (-2 * sub + 3) * sub * sub;
    offset = max(offset, sub * sub * c0.w);

    float2 uvF = uv;
    if (horz) uvF.y += offset * stepLen; else uvF.x += offset * stepLen;
    return float4(tex2Dlod(scene, float4(uvF, 0, 0)).rgb, 1);
}
)";

        const WXL_Api*     g_api = nullptr;
        Settings           g_settings;
        const char*        g_inactive = "no frame yet";
        double             g_lastMs = 0;

        IDirect3DDevice9*       g_device = nullptr; // everything below belongs to this device
        IDirect3DPixelShader9*  g_shader = nullptr;
        bool                    g_shaderFailed = false;
        IDirect3DTexture9*      g_copy = nullptr;   // the world image, sampled by the pass
        IDirect3DSurface9*      g_copySurface = nullptr;
        UINT                    g_copyW = 0, g_copyH = 0;
        D3DFORMAT               g_copyFormat = D3DFMT_UNKNOWN;

        void ReleaseCopy()
        {
            if (g_copySurface) { g_copySurface->Release(); g_copySurface = nullptr; }
            if (g_copy) { g_copy->Release(); g_copy = nullptr; }
            g_copyW = g_copyH = 0;
        }

        // A graphics restart can replace the device without the reset events; nothing made on the
        // old one may be used on the new one.
        void FollowDevice(IDirect3DDevice9* dev)
        {
            if (dev == g_device) return;
            ReleaseCopy();
            if (g_shader) { g_shader->Release(); g_shader = nullptr; }
            g_shaderFailed = false;
            g_device = dev;
        }

        bool EnsureCopy(IDirect3DDevice9* dev, const D3DSURFACE_DESC& rt)
        {
            if (g_copy && g_copyW == rt.Width && g_copyH == rt.Height && g_copyFormat == rt.Format) return true;
            ReleaseCopy();
            if (FAILED(dev->CreateTexture(rt.Width, rt.Height, 1, D3DUSAGE_RENDERTARGET, rt.Format, D3DPOOL_DEFAULT, &g_copy, nullptr)) || !g_copy)
            {
                g_copy = nullptr;
                return false;
            }
            g_copy->GetSurfaceLevel(0, &g_copySurface);
            g_copyW = rt.Width; g_copyH = rt.Height; g_copyFormat = rt.Format;
            return g_copySurface != nullptr;
        }

        void __cdecl OnWorldRenderEnd(void* /*user*/, const void* args)
        {
            if (g_settings.mode == Mode::Off) { g_inactive = "switched off"; return; }
            const auto* a = static_cast<const ev::WorldRenderEndArgs*>(args);
            auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
            if (!dev) return;
            FollowDevice(dev);

            IDirect3DSurface9* rt = nullptr;
            if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
            D3DSURFACE_DESC desc{};
            rt->GetDesc(&desc);
            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            {
                rt->Release();
                g_inactive = "the client's multisampling is on (turn it off in the video options; this replaces it)";
                return;
            }

            if (!g_shader && !g_shaderFailed)
            {
                g_shader = static_cast<IDirect3DPixelShader9*>(gx::CompilePixelShader(gx::Device9(dev), kFxaaHlsl, "ps_3_0"));
                g_shaderFailed = g_shader == nullptr;
                if (g_shaderFailed) g_api->Log(WXL_LOG_WARN, kTag, "anti-aliasing: the FXAA shader failed to compile; off.");
            }
            if (!g_shader) { rt->Release(); g_inactive = "the FXAA shader failed to compile (see the log)"; return; }
            if (!EnsureCopy(dev, desc)) { rt->Release(); g_inactive = "couldn't create the copy target"; return; }

            const double t0 = grassperf::Now();
            // The world image into our texture, then back through the pass.
            dev->StretchRect(rt, nullptr, g_copySurface, nullptr, D3DTEXF_NONE);

            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) { rt->Release(); return; }

            const D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
            dev->SetViewport(&vp);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);

            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(g_shader);
            dev->SetTexture(0, g_copy);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); // FXAA samples between texels
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

            const float c[8] = {
                1.0f / desc.Width, 1.0f / desc.Height, static_cast<float>(static_cast<int>(g_settings.mode)), g_settings.subpixel,
                g_settings.edgeThreshold, 0.0f, 0.0f, 0.0f,
            };
            dev->SetPixelShaderConstantF(0, c, 2);

            // Full screen; the half-pixel offset lines texels up with pixels (D3D9 pixel centres).
            const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
            struct Vtx { float x, y, z, rhw, u, v; };
            const Vtx quad[4] = {
                { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
                { w - 0.5f,  -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
                { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
                { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
            };
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));

            saved->Apply();
            saved->Release();
            rt->Release();
            g_lastMs = grassperf::Now() - t0; // CPU submit time; the GPU work isn't measured here
            g_inactive = nullptr;
        }

        const char* ModeName(Mode m)
        {
            switch (m)
            {
                case Mode::Off:         return "off";
                case Mode::PassThrough: return "pass-through (test: copy only, no smoothing)";
                case Mode::Fxaa:        return "FXAA";
                case Mode::ShowEdges:   return "show edges (debug)";
            }
            return "?";
        }

        void __cdecl Panel(void* /*user*/)
        {
            char line[256];
            int mode = static_cast<int>(g_settings.mode);
            static const char* const modes[] = { "Off", "Pass-through (test)", "FXAA", "Show edges (debug)" };
            if (g_api->UiCombo("Anti-aliasing", &mode, modes, 4)) g_settings.mode = static_cast<Mode>(mode);
            g_api->UiSliderFloat("Subpixel smoothing", &g_settings.subpixel, 0.0f, 1.0f);
            g_api->UiSliderFloat("Edge threshold", &g_settings.edgeThreshold, 0.063f, 0.333f);
            if (const char* why = g_inactive)
                std::snprintf(line, sizeof(line), "%s: not running -- %s", ModeName(g_settings.mode), why);
            else
                std::snprintf(line, sizeof(line), "%s: running, %.3f ms CPU per frame", ModeName(g_settings.mode), g_lastMs);
            g_api->UiTextWrapped(line);
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldRenderEnd), &OnWorldRenderEnd, nullptr);
    }

    Settings& Tunables() { return g_settings; }

    void OnDeviceLost() { ReleaseCopy(); }

    const char* Inactive() { return g_inactive; }
    double LastMs() { return g_lastMs; }

    void RegisterPanel(const WXL_Api* api)
    {
        api->UiAddPanel("wxl-seyris-living-azeroth: anti-aliasing", &Panel, nullptr);
    }
}
