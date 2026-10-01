#include "DepthView.hpp"

#include "../render/SceneDepth.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"

#include <d3d9.h>
#include <cstdio>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        namespace gx  = wxl::game::gx;
        namespace ev  = wxl::events;

        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: scene depth";

        // Raw depth d relates to view distance z by d = m10 + m14 / z (row-vector D3D projection,
        // w = z), so z = m14 / (d - m10). Far-plane pixels (sky) have d = 1.
        const char* kDepthViewHlsl = R"(
            sampler2D depthTex : register(s0);
            float4 p : register(c0); // x = proj m10, y = proj m14, z = display range (yards), w = raw mode

            float4 main(float2 uv : TEXCOORD0) : COLOR
            {
                float d = tex2D(depthTex, uv).r;
                if (p.w > 0.5) return float4(d, d, d, 1);
                if (d >= 0.99999) return float4(0.15, 0.25, 0.5, 1);
                // d - m10 is always negative here (m10 = f/(f-n) > 1 > d), as is m14 = -n*f/(f-n);
                // keep the divisor away from zero on the negative side.
                float z = p.y / min(d - p.x, -1e-6);
                float g = sqrt(saturate(z / p.z));
                return float4(g, g, g, 1);
            }
        )";

        const WXL_Api* g_api = nullptr;

        int   g_showOverlay = 0;
        int   g_rawMode     = 0;
        float g_overlaySize = 0.35f;  // fraction of the screen width
        float g_rangeYards  = 300.0f; // distance shown as white

        float g_projM10 = 0.0f, g_projM14 = 0.0f;
        bool  g_haveProjection = false;
        float g_projSeen[16] = {}; // last world projection as read, for the diagnostic readout

        unsigned g_scenesTotal = 0, g_scenesOnOurDepth = 0;

        void* g_pixelShader = nullptr;
        bool  g_shaderFailed = false;

        const char* FormatName(unsigned f)
        {
            switch (f)
            {
                case D3DFMT_D16:   return "D16";
                case D3DFMT_D24X8: return "D24X8";
                case D3DFMT_D24S8: return "D24S8";
                case D3DFMT_D32:   return "D32";
                default:           return "other";
            }
        }

        const char* StateText(depth::State s)
        {
            switch (s)
            {
                case depth::State::NotStarted:   return "not started (no device yet)";
                case depth::State::Active:       return "ACTIVE";
                case depth::State::Disabled:     return "disabled";
                case depth::State::Unsupported:  return "unsupported (driver has no INTZ)";
                case depth::State::Multisampled: return "off: multisampling is on (turn MSAA off)";
                case depth::State::CreateFailed: return "failed to create the INTZ texture";
            }
            return "?";
        }

        void __cdecl DepthPanel(void* /*user*/)
        {
            char line[192];

            int enabled = depth::Enabled() ? 1 : 0;
            if (g_api->UiCheckbox("Readable depth (INTZ swap)", &enabled))
                depth::SetEnabled(enabled != 0);

            std::snprintf(line, sizeof(line), "state: %s", StateText(depth::CurrentState()));
            g_api->UiText(line);

            if (depth::CurrentState() == depth::State::Active)
            {
                std::snprintf(line, sizeof(line), "size %ux%u, replaced %s, swaps %u",
                              depth::Width(), depth::Height(), FormatName(depth::OriginalFormat()), depth::SwapCount());
                g_api->UiText(line);
            }

            std::snprintf(line, sizeof(line), "world passes drawn into our depth: %u / %u",
                          g_scenesOnOurDepth, g_scenesTotal);
            g_api->UiText(line);

            g_api->UiSeparator();
            g_api->UiCheckbox("Show depth overlay", &g_showOverlay);
            g_api->UiCheckbox("Raw (non-linear) depth", &g_rawMode);
            g_api->UiSliderFloat("Overlay size", &g_overlaySize, 0.15f, 1.0f);
            g_api->UiSliderFloat("White at (yards)", &g_rangeYards, 10.0f, 2000.0f);

            // Diagnostic: the projection the linearization is built from, and what it turns a few
            // raw depth values into. Tells us the matrix convention directly.
            if (g_api->UiCollapsingHeader("Projection (diagnostic)"))
            {
                const float* m = g_projSeen;
                for (int row = 0; row < 4; ++row)
                {
                    std::snprintf(line, sizeof(line), "row %d: %10.5f %10.5f %10.5f %10.5f",
                                  row, m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
                    g_api->UiText(line);
                }
                std::snprintf(line, sizeof(line), "used: m10 = %.6f, m14 = %.6f (perspective %s)",
                              g_projM10, g_projM14, g_haveProjection ? "yes" : "NOT seen");
                g_api->UiText(line);
                const float samples[] = { 0.5f, 0.9f, 0.99f, 0.999f, 0.9999f };
                for (float d : samples)
                {
                    const float denom = d - g_projM10;
                    std::snprintf(line, sizeof(line), "d = %.4f -> z = %.3f yards", d,
                                  denom != 0.0f ? g_projM14 / denom : 0.0f);
                    g_api->UiText(line);
                }
            }

            if (g_shaderFailed)
                g_api->UiTextWrapped("Overlay shader failed to compile (d3dcompiler_47.dll missing?).");
        }

        void __cdecl OnWorldSceneEndThunk(void* /*user*/, const void* args) { OnWorldSceneEnd(args); }
        void __cdecl OnWorldRenderEndThunk(void* /*user*/, const void* args) { OnWorldRenderEnd(args); }
    }

    void RegisterPanel(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &DepthPanel, nullptr);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEndThunk, nullptr);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldRenderEnd), &OnWorldRenderEndThunk, nullptr);
    }

    void OnWorldSceneEnd(const void* args)
    {
        const auto* a = static_cast<const ev::WorldSceneEndArgs*>(args);

        ++g_scenesTotal;
        if (a && a->sceneDepth && a->sceneDepth == depth::Surface()) ++g_scenesOnOurDepth;

        const float* proj = wxl::game::camera::GetProjection();
        if (proj)
            for (int i = 0; i < 16; ++i) g_projSeen[i] = proj[i];
        if (proj && proj[11] == 1.0f) // perspective: w = z
        {
            g_projM10 = proj[10];
            g_projM14 = proj[14];
            g_haveProjection = true;
        }
    }

    void OnWorldRenderEnd(const void* args)
    {
        if (!g_showOverlay) return;

        auto* texture  = static_cast<IDirect3DTexture9*>(depth::Texture());
        auto* original = static_cast<IDirect3DSurface9*>(depth::OriginalSurface());
        if (!texture || !original || !g_haveProjection) return;

        const auto* a = static_cast<const ev::WorldRenderEndArgs*>(args);
        auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
        if (!dev) return;

        // A graphics restart can replace the device; a shader from the old one can't be used.
        static void* shaderDevice = nullptr;
        if (dev != shaderDevice)
        {
            if (g_pixelShader) gx::Release(g_pixelShader);
            g_pixelShader = nullptr;
            g_shaderFailed = false;
            shaderDevice = dev;
        }
        if (!g_pixelShader && !g_shaderFailed)
        {
            g_pixelShader = gx::CompilePixelShader(gx::Device9(dev), kDepthViewHlsl, "ps_2_0");
            g_shaderFailed = g_pixelShader == nullptr;
        }
        if (!g_pixelShader) return;

        IDirect3DSurface9* rt = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
        D3DSURFACE_DESC rtDesc{};
        rt->GetDesc(&rtDesc);
        rt->Release();

        // Everything the draw changes is put back by the state block, so the engine's own state
        // cache stays true. Depth bindings aren't part of a state block and are restored by hand.
        IDirect3DStateBlock9* saved = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;

        IDirect3DSurface9* boundDepth = nullptr;
        dev->GetDepthStencilSurface(&boundDepth);
        dev->SetDepthStencilSurface(original); // our INTZ can't be bound while we sample it

        const D3DVIEWPORT9 vp{ 0, 0, rtDesc.Width, rtDesc.Height, 0.0f, 1.0f };
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
        dev->SetPixelShader(static_cast<IDirect3DPixelShader9*>(g_pixelShader));
        dev->SetTexture(0, texture);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        const float constants[4] = { g_projM10, g_projM14, g_rangeYards, g_rawMode ? 1.0f : 0.0f };
        dev->SetPixelShaderConstantF(0, constants, 1);

        // Bottom-right corner, keeping the screen's aspect ratio. Half-pixel offset maps texels 1:1.
        const float w  = static_cast<float>(rtDesc.Width) * g_overlaySize;
        const float h  = static_cast<float>(rtDesc.Height) * g_overlaySize;
        const float x0 = static_cast<float>(rtDesc.Width) - w - 0.5f;
        const float y0 = static_cast<float>(rtDesc.Height) - h - 0.5f;
        struct Vtx { float x, y, z, rhw, u, v; };
        const Vtx quad[4] = {
            { x0,     y0,     0.0f, 1.0f, 0.0f, 0.0f },
            { x0 + w, y0,     0.0f, 1.0f, 1.0f, 0.0f },
            { x0,     y0 + h, 0.0f, 1.0f, 0.0f, 1.0f },
            { x0 + w, y0 + h, 0.0f, 1.0f, 1.0f, 1.0f },
        };
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));

        saved->Apply();
        saved->Release();

        dev->SetDepthStencilSurface(boundDepth);
        if (boundDepth) boundDepth->Release();
    }
}
