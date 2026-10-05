#include "TerrainLights.hpp"

#include "ModelLights.hpp"
#include "TerrainWetness.hpp"
#include "../env/Lights.hpp"

#include <cstdio>
#include <cstring>
#include <regex>

namespace wxl_livingazeroth::terrainlights
{
    namespace
    {
        // The light list: 4 texels per light (world position + 1 / radius; colour + falloff shape; spot
        // direction + cos outer angle; 1 / (cos inner - cos outer)), A32B32G32R32F, point sampled on
        // s13. PS c206.x = how many.
        constexpr int   kTexelsPerLight = 4;
        constexpr int   kTexels = lights::kMaxLights * kTexelsPerLight;
        constexpr DWORD kSampler = 13;
        constexpr int   kConstant = 206;

        // VS c250.x: the stock point lights' share (1 = as the client lights the chunk, 0 = off).
        constexpr int kStockSwitch = 250;

        IDirect3DDevice9*  g_device = nullptr;
        IDirect3DTexture9* g_texture = nullptr;
        bool               g_failed = false;
        unsigned           g_drawn = 0;
        bool               g_stockOff = false;
        unsigned           g_switchedVariants = 0;
        std::string        g_status;

        void Release()
        {
            if (g_texture) g_texture->Release();
            g_texture = nullptr;
            g_device = nullptr;
        }

        bool EnsureTexture(IDirect3DDevice9* device)
        {
            if (device != g_device) { Release(); g_failed = false; g_device = device; }
            if (g_texture) return true;
            if (g_failed) return false;
            if (FAILED(device->CreateTexture(kTexels, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &g_texture, nullptr)))
            {
                g_texture = nullptr;
                g_failed = true;
                return false;
            }
            return true;
        }
    }

    std::string PixelBlock(const std::string& r, std::string& defs)
    {
        // c207 = (1 / texels, texels per light / texels, 0.5 / texels, 0): texel steps; c208 = (2, -1, 1 / 1.5, 0);
        // c209 = (0.7, 1 / ln 2, 0.05, 0.03); c210 = the light-list uv's start (u, 0.5, 0, lod 0);
        // c211 = (the knee's minimum 0.8, 0.001).
        // ps_3_0 reads at most one constant register per instruction: 0.7 and 0.03 share c209.
        // Same light as the surface cover's (SurfaceCover.cpp, Lamps): size ~1 yd (falloff on
        // sqrt(d^2 + 1)), like the client's 1 / (0.7 d + 0.03 d^2), windowed to 0 at the radius,
        // wrapped Lambert. The stock lighting is the vertex colour v0 (x 2 in the stock PS); ours fills
        // the headroom it leaves softly: l' = l + (1 - l)(1 - e^-lamps), applied as colour x l' / l.
        char d[320];
        std::snprintf(d, sizeof(d),
                      "    def c207, %.9g, %.9g, %.9g, 0\n    def c208, 2, -1, 0.666666687, 0\n"
                      "    def c209, 0.7, 1.44269502, 0.05, 0.03\n    def c210, 0, 0.5, 0, 0\n    def c211, 0.8, 0.001, 0, 0\n"
                      "    defi i0, %d, 0, 0, 0\n    dcl_2d s13\n",
                      1.0 / kTexels, static_cast<double>(kTexelsPerLight) / kTexels, 0.5 / kTexels, lights::kMaxLights);
        defs = d;
        return
            // The ground's normal (moisture texture red/green), toward up near and beyond the grid's edge.
            "\n    texld r24, r29, s11"
            "\n    mad r24.xy, r24, c208.x, c208.y"
            "\n    mul r24.xy, r24, r31.x"
            "\n    dp2add r24.w, r24, -r24, c202.x"
            "\n    max r24.w, r24.w, c202.y"
            "\n    rsq r24.w, r24.w"
            "\n    rcp r24.z, r24.w"
            "\n    mov r23.xyz, c202.y"
            "\n    mov r22.x, c202.y"
            "\n    mov r21, c210"
            "\n    rep i0"
            "\n      break_ge r22.x, c206.x"
            "\n      mad r21.x, r22.x, c207.y, c207.z"
            "\n      texldl r20, r21, s13"
            "\n      add r21.x, r21.x, c207.x"
            "\n      texldl r19, r21, s13"
            "\n      add r21.x, r21.x, c207.x"
            "\n      texldl r11, r21, s13"
            "\n      add r21.x, r21.x, c207.x"
            "\n      texldl r10, r21, s13"
            "\n      add r20.xyz, r20, -v9"
            "\n      dp3 r18.x, r20, r20"
            "\n      add r18.y, r18.x, c202.x"
            "\n      rsq r18.z, r18.x"
            "\n      rcp r18.w, r18.z"
            "\n      mul_sat r17.x, r18.w, r20.w"
            "\n      mad r17.x, r17.x, -r17.x, c202.x"
            "\n      mul r17.x, r17.x, r17.x"
            // The falloff shape: the window to the light's power (colour texel .w).
            "\n      pow r17.x, r17.x, r19.w"
            "\n      mul r20.xyz, r20, r18.z"
            "\n      dp3 r17.y, r24, r20"
            "\n      add r17.y, r17.y, c202.z"
            "\n      mul_sat r17.y, r17.y, c208.z"
            // The spot cone: saturate((cos to the axis - cos outer) x scale); point lights: cos outer -2.
            "\n      dp3 r17.w, -r20, r11"
            "\n      add r17.w, r17.w, -r11.w"
            "\n      mul_sat r17.w, r17.w, r10.x"
            "\n      mul r17.y, r17.y, r17.w"
            "\n      rsq r17.z, r18.y"
            "\n      rcp r17.w, r17.z"
            "\n      mad r16.x, r17.w, c209.w, c209.x"
            "\n      mul r16.x, r16.x, r17.w"
            "\n      rcp r16.x, r16.x"
            "\n      mul r16.x, r16.x, r17.x"
            "\n      mul r16.x, r16.x, r17.y"
            "\n      mad r23.xyz, r19, r16.x, r23"
            "\n      add r22.x, r22.x, c202.x"
            "\n    endrep"
            // l = min(2 v0, 1) (the stock lighting); t = l + lamps, added like the client's own lights;
            // above a knee (max(0.8, brightest channel of l)) the brightest channel of t is compressed
            // smoothly (knee + room (1 - e^(-over / room))) and all three scaled alike, so the flame's
            // hue stays; without lamps t = l. Then colour x t / max(l, 0.05) (exp is 2^x).
            "\n    add r15.xyz, v0, v0"
            "\n    min r15.xyz, r15, c202.x"
            "\n    add r14.xyz, r15, r23"
            "\n    max r13.x, r14.x, r14.y"
            "\n    max r13.x, r13.x, r14.z"
            "\n    max r13.y, r15.x, r15.y"
            "\n    max r13.y, r13.y, r15.z"
            "\n    max r13.y, r13.y, c211.x"
            "\n    add r13.z, r13.x, -r13.y"
            "\n    max r13.z, r13.z, c202.y"
            "\n    add r13.w, c202.x, -r13.y"
            "\n    max r13.w, r13.w, c211.y"
            "\n    rcp r12.x, r13.w"
            "\n    mul r12.y, r13.z, r12.x"
            "\n    mul r12.y, r12.y, -c209.y"
            "\n    exp r12.y, r12.y"
            "\n    add r12.y, c202.x, -r12.y"
            "\n    mul r12.y, r12.y, r13.w"
            "\n    add r12.z, r13.x, -r13.z"
            "\n    add r12.z, r12.z, r12.y"
            "\n    max r12.w, r13.x, c211.y"
            "\n    rcp r12.w, r12.w"
            "\n    mul r12.z, r12.z, r12.w"
            "\n    mul r14.xyz, r14, r12.z"
            "\n    max r15.xyz, r15, c209.z"
            "\n    rcp r13.x, r15.x"
            "\n    rcp r13.y, r15.y"
            "\n    rcp r13.z, r15.z"
            "\n    mul r14.xyz, r14, r13"
            "\n    mul " + r + ".xyz, " + r + ", r14";
    }

    void EditVertex(std::string& src)
    {
        // The stock point-light code (terrain VS dumps 2026-10-04): the 3 lights' colours c29, c32, c35,
        // each read once as `mul rN.xyz, rM.c, cK`. Temps r10..r12 and c250 are free in every variant
        // (they use up to r6 and c48).
        static const std::regex colourRead(R"(\bc(29|32|35)\b)");
        if (!std::regex_search(src, colourRead)) return;
        src = std::regex_replace(src, std::regex(R"(\bc29\b)"), "r10");
        src = std::regex_replace(src, std::regex(R"(\bc32\b)"), "r11");
        src = std::regex_replace(src, std::regex(R"(\bc35\b)"), "r12");
        // After the declarations: the colours, scaled by the switch.
        std::string::size_type at = src.find('\n');
        while (at != std::string::npos && at + 1 < src.size())
        {
            const std::string::size_type end = src.find('\n', at + 1);
            std::string line = src.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line.compare(first, 3, "dcl") != 0 && line.compare(first, 3, "def") != 0 && line.compare(first, 2, "//") != 0) break;
            at = end;
        }
        if (at == std::string::npos) return;
        src.insert(at + 1,
                   "    mov r10, c29\n    mul r10.xyz, r10, c250.x\n"
                   "    mov r11, c32\n    mul r11.xyz, r11, c250.x\n"
                   "    mov r12, c35\n    mul r12.xyz, r12, c250.x\n");
        ++g_switchedVariants;
    }

    void BeforeTerrainStage(IDirect3DDevice9* device)
    {
        if (!device) return;
        const bool on = lights::Config().enabled && terrainwet::PatchActive() && EnsureTexture(device);
        float c[4] = {};
        g_drawn = 0;
        if (on)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(g_texture->LockRect(0, &lr, nullptr, 0)))
            {
                float* t = static_cast<float*>(lr.pBits);
                std::memset(t, 0, kTexels * 4 * sizeof(float));
                for (const lights::ActiveLight& l : lights::Active())
                {
                    if (g_drawn >= static_cast<unsigned>(lights::kMaxLights)) break;
                    float* o = t + g_drawn * kTexelsPerLight * 4;
                    o[0] = l.pos[0]; o[1] = l.pos[1]; o[2] = l.pos[2];
                    o[3] = l.radius > 0.0f ? 1.0f / l.radius : 0.0f;
                    o[4] = l.color[0]; o[5] = l.color[1]; o[6] = l.color[2]; o[7] = l.falloff;
                    o[8] = l.spotDir[0]; o[9] = l.spotDir[1]; o[10] = l.spotDir[2]; o[11] = l.cosOuter;
                    o[12] = l.spotScale;
                    ++g_drawn;
                }
                g_texture->UnlockRect(0);
            }
            device->SetTexture(kSampler, g_texture);
            device->SetSamplerState(kSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(kSampler, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        c[0] = static_cast<float>(g_drawn);
        device->SetPixelShaderConstantF(kConstant, c, 1);
        // The same list for the M2s and WMOs drawn after the terrain (their own texture, read in
        // their vertex shaders).
        modellights::Prepare(device);
        // Ours replace the client's 3 per chunk only while they're actually drawn.
        g_stockOff = on;
        const float stock[4] = { on ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
        device->SetVertexShaderConstantF(kStockSwitch, stock, 1);
    }

    const char* StatusLine()
    {
        char line[256];
        std::snprintf(line, sizeof(line), "terrain lights: %s, %u drawn per pixel; the client's 3 per chunk %s (switchable in %u vertex variants with point lights, of those created so far)%s",
                      g_stockOff ? "on" : "off", g_drawn, g_stockOff ? "switched off" : "on", g_switchedVariants,
                      g_failed ? " -- the light texture (A32B32G32R32F) couldn't be created" : "");
        g_status = line;
        return g_status.c_str();
    }
}
