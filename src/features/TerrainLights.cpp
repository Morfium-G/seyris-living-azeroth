#include "TerrainLights.hpp"

#include "ModelLights.hpp"
#include "TerrainWetness.hpp"
#include "../env/Lights.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"

#include <cstdio>
#include <cstring>
#include <regex>

namespace wxl_livingazeroth::terrainlights
{
    namespace
    {
        // The cells and lights: the model lights' texture (modellights::Texture, layout in
        // ModelLights.cpp: rows of 1024 texels, row 0 its header, then the cells' entries, then the
        // lights at 4 texels each), point sampled on s13. The grid in PS c206 (corner x, y, 1 / cell
        // size, cells) and c212 (1 / height, lights per cell, cells - 1, 0).
        constexpr int   kWidth = 1024;          // = ModelLights.cpp kWidth
        constexpr DWORD kSampler = 13;
        constexpr int   kGridConstant = 206, kGridConstant2 = 212;

        // VS c250.x: the stock point lights' share (1 = as the client lights the chunk, 0 = off).
        constexpr int kStockSwitch = 250;

        bool               g_on = false;
        bool               g_stockOff = false;
        unsigned           g_switchedVariants = 0;
        std::string        g_status;
    }

    std::string PixelBlock(const std::string& r, std::string& defs)
    {
        // c207 = (1 / W, 0, 0.5 / W, 0): texel steps along a row; c208 = (2, -1, 1 / 1.5, 0);
        // c209 = (0.7, 1 / ln 2, 0.05, 0.03); c210 = (1.5: header row + texel centre, 0, 0, 0);
        // c211 = (the knee's minimum 0.8, 0.001).
        // ps_3_0 reads at most one constant register per instruction: 0.7 and 0.03 share c209.
        // Same light as the surface cover's (SurfaceCover.cpp, Lamps): size ~1 yd (falloff on
        // sqrt(d^2 + 1)), like the client's 1 / (0.7 d + 0.03 d^2), windowed to 0 at the radius,
        // wrapped Lambert. The stock lighting is the vertex colour v0 (x 2 in the stock PS); ours fills
        // the headroom it leaves softly: l' = l + (1 - l)(1 - e^-lamps), applied as colour x l' / l.
        // Only the pixel's grid cell's lights: its cell from the world position v9 (clamped to the
        // grid: a light past its radius adds 0), entries from cell x lights per cell, each pointing
        // at its light's first texel; an empty entry or the cell's capacity ends the loop.
        // c213 = (4, 7.999, 2, 0.25), c214 = (0, 1, 2, 3): the occlusion lookup's steps.
        char d[448];
        std::snprintf(d, sizeof(d),
                      "    def c207, %.9g, 0, %.9g, 0\n    def c208, 2, -1, 0.666666687, 0\n"
                      "    def c209, 0.7, 1.44269502, 0.05, 0.03\n    def c210, 1.5, 0, 0, 0\n    def c211, 0.8, 0.001, 0, 0\n"
                      "    def c213, 4, 7.99900007, 2, 0.25\n    def c214, 0, 1, 2, 3\n"
                      "    defi i0, %d, 0, 0, 0\n    dcl_2d s13\n",
                      1.0 / kWidth, 0.5 / kWidth, lights::kMaxPerCellCap);
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
            // The cell: r22.y = its first entry.
            "\n    add r22.xy, v9, -c206"
            "\n    mul r22.xy, r22, c206.z"
            "\n    frc r22.zw, r22.xyxy"
            "\n    add r22.xy, r22, -r22.zwzw"
            "\n    max r22.xy, r22, c202.y"
            "\n    min r22.xy, r22, c212.z"
            "\n    mad r22.x, r22.y, c206.w, r22.x"
            "\n    mul r22.y, r22.x, c212.y"
            "\n    mov r23.xyz, c202.y"
            "\n    mov r22.x, c202.y"
            "\n    mov r21, c202.y"
            "\n    rep i0"
            "\n      break_ge r22.x, c212.y"
            // Entry e = first + i at (column, 1 + row) of the W-wide rows.
            "\n      add r18.x, r22.y, r22.x"
            "\n      mul r18.x, r18.x, c207.x"
            "\n      frc r18.y, r18.x"
            "\n      add r18.z, r18.x, -r18.y"
            "\n      add r21.x, r18.y, c207.z"
            "\n      add r18.z, r18.z, c210.x"
            "\n      mul r21.y, r18.z, c212.x"
            "\n      texldl r20, r21, s13"
            "\n      break_lt r20.z, c202.z"
            "\n      mov r21.xy, r20"
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
            // Baked occlusion (lights::Bake; ModelLights.cpp OcclusionVS has the same steps): direction
            // light -> pixel on the 8 x 8 octahedral map, the tile's texel and component (4th texel
            // .y = tile / tiles per row, -1 = none; c212.w = the region's first row + 0.5), and no light
            // past the stored reach (+ 0.5 yd, soft over 0.5 yd).
            "\n      if_ge r10.y, c202.y"
            "\n      add r11.x, r20_abs.x, r20_abs.y"
            "\n      add r11.x, r11.x, r20_abs.z"
            "\n      rcp r11.x, r11.x"
            "\n      mul r11.xyz, -r20, r11.x"
            "\n      cmp r12.xy, r11, c202.x, -c202.x"
            "\n      add r12.zw, c202.x, -r11_abs.xxyx"
            "\n      mul r12.xy, r12, r12.zw"
            "\n      add r12.xy, r12, -r11"
            "\n      cmp r12.z, r11.z, c202.y, c202.x"
            "\n      mad r11.xy, r12, r12.z, r11"
            "\n      add r11.xy, r11, c202.x"
            "\n      mul r11.xy, r11, c213.x"
            "\n      min r11.xy, r11, c213.y"
            "\n      frc r12.xy, r11"
            "\n      add r11.xy, r11, -r12"
            "\n      mul r12.x, r11.x, c213.w"
            "\n      frc r12.y, r12.x"
            "\n      add r12.x, r12.x, -r12.y"
            "\n      mul r12.y, r12.y, c213.x"
            "\n      mad r12.x, r11.y, c213.z, r12.x"
            "\n      frc r11.z, r10.y"
            "\n      add r11.w, r10.y, -r11.z"
            "\n      add r12.x, r12.x, c202.z"
            "\n      mad r21.x, r12.x, c207.x, r11.z"
            "\n      add r11.w, r11.w, c212.w"
            "\n      mul r21.y, r11.w, c212.x"
            "\n      texldl r13, r21, s13"
            "\n      add r14, r12.y, -c214"
            "\n      add r14, -r14_abs, c202.z"
            "\n      cmp r14, r14, c202.x, c202.y"
            "\n      dp4 r13.x, r13, r14"
            "\n      add r13.x, r13.x, -r18.w"
            "\n      add r13.x, r13.x, r13.x"
            "\n      add_sat r13.x, r13.x, c202.x"
            "\n      mul r16.x, r16.x, r13.x"
            "\n      endif"
            // Indoor lights (4th texel .w = 1, only while the panel's switch is on) stay off the terrain.
            "\n      add r10.w, c202.x, -r10.w"
            "\n      mul r16.x, r16.x, r10.w"
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
        // The view the world draws with, for attached models' positions (lights::AttachedWorld).
        {
            float V[16], P[16], cam[3];
            if (wxl::game::gfx::SceneMatrices(V, P))
            {
                wxl::game::camera::GetPosition(cam);
                lights::NoteSceneView(V, cam);
            }
        }
        // The shared texture: filled (and bound for the M2s and WMOs drawn after the terrain, in their
        // vertex shaders) by the model lights.
        const bool gridOn = modellights::Prepare(device, true);
        const bool on = lights::Config().enabled && terrainwet::PatchActive() && gridOn && modellights::Texture();
        float c206[4] = {}, c212[4] = {};
        if (on)
        {
            modellights::GridConstants(c206, c212);
            device->SetTexture(kSampler, modellights::Texture());
            device->SetSamplerState(kSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(kSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(kSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(kSampler, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        // Zero lights per cell (c212.y) while off: the loop ends at once.
        device->SetPixelShaderConstantF(kGridConstant, c206, 1);
        device->SetPixelShaderConstantF(kGridConstant2, c212, 1);
        g_on = on;
        // Ours replace the client's 3 per chunk only while they're actually drawn.
        g_stockOff = on;
        const float stock[4] = { on ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
        device->SetVertexShaderConstantF(kStockSwitch, stock, 1);
    }

    const char* StatusLine()
    {
        char line[256];
        const lights::Grid& grid = lights::CellGrid();
        std::snprintf(line, sizeof(line), "terrain lights: %s, each pixel loops over its cell's lights (%d x %d cells of %.0f yd, up to %d each); the client's 3 per chunk %s (switchable in %u vertex variants with point lights, of those created so far)",
                      g_on ? "on" : "off", grid.cells, grid.cells, grid.cellSize, grid.perCell,
                      g_stockOff ? "switched off" : "on", g_switchedVariants);
        g_status = line;
        return g_status.c_str();
    }
}
