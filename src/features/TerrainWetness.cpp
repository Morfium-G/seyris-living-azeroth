#include "TerrainWetness.hpp"

#include "../render/MoistureTexture.hpp"
#include "../render/ShaderPatch.hpp"

#include <cstdio>
#include <regex>
#include <string>

namespace wxl_livingazeroth::terrainwet
{
    namespace
    {
        // The client's terrain shader tables (filled by the terrain shader load 0x79E7C0 through the
        // set loader 0x6AA130, which fills a table before creating any of its shaders; layout from
        // debug/ShaderDump, verified 2026-10-02/03).
        constexpr uintptr_t kTerrainVs = 0x00CE0008;  // 0x80 "Terrain"
        const std::vector<std::pair<uintptr_t, int>> kTerrainPs = {
            { 0x00CE0488, 3 },    // Terrain0
            { 0x00CE0004, 1 },    // Terrain0_env
            { 0x00CE0408, 0x20 }, // Terrain1 / Terrain1w (overlapping tables)
            { 0x00CE0388, 0x20 }, // Terrain2 / Terrain2_pcf
            { 0x00CE0208, 0x60 }, // Terrain3 / Terrain3_pcf
        };

        // Registers the patch uses, all free in every stock variant (from the dumps: PS inputs up to
        // v8, temps up to r7, constants up to c13, samplers up to s8): PS v9, r29..r31, c200..c202, s11.
        // The world XY rides as texcoord7 (.xy), the PS reads v9.xy. Where the VS has an output register
        // left (vs_3_0 allows 11 without point size), it gets its own. The 32 variants using all 11
        // declare texcoord3 as their view-space position (.xyz, `mov oN.xyz, r0`), which no terrain
        // pixel shader reads (from the dumps: the only PS reading texcoord3 read it .xy, a layer UV,
        // paired with the VS that write it that way) -- that output is re-labelled texcoord7 and
        // carries the world XY instead. Packing a second semantic into a register's free .zw does NOT
        // work (in-client 2026-10-04: the PS read zeros, whichever components it declared).
        // c200 = (1 / grid extent, debug: whole grid wet, 0, strength), c201 = grid box (world yd),
        // c203 = debug (ignore the box, stripe scale, stripes on, 0); c202 is defined in the shader.
        constexpr int kPsConstant = 200;

        bool  g_enabled = true;
        unsigned g_packed = 0; // vertex variants whose unread view-position output carries it (all 11 outputs in use)
        int   g_debug = 0; // 0 off, 1 whole grid, 2 whole terrain, 3 10 yd stripes
        float g_strength = 0.45f;
        std::string g_status;

        std::string::size_type FirstInstruction(const std::string& src)
        {
            // After the version line, the first line that isn't a declaration, a definition or empty.
            std::string::size_type at = src.find('\n');
            while (at != std::string::npos && at + 1 < src.size())
            {
                const std::string::size_type end = src.find('\n', at + 1);
                std::string line = src.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos)
                {
                    line = line.substr(first);
                    if (line.rfind("dcl", 0) != 0 && line.rfind("def", 0) != 0 && line.rfind("//", 0) != 0) return at + 1;
                }
                at = end;
            }
            return std::string::npos;
        }

        bool EditVertex(std::string& src, std::string& why)
        {
            if (src.find("vs_3_0") == std::string::npos) { why = "not vs_3_0"; return false; }
            if (src.find("dcl_position v0") == std::string::npos) { why = "no position input"; return false; }
            if (src.find("dcl_texcoord7") != std::string::npos) { why = "texcoord7 in use"; return false; }
            const auto at = FirstInstruction(src);
            if (at == std::string::npos) { why = "no instructions"; return false; }
            // Output registers in use.
            static const std::regex outputDcl(R"(dcl_\w+ o(\d+))");
            int highest = -1, declared = 0;
            for (auto it = std::sregex_iterator(src.begin(), src.end(), outputDcl); it != std::sregex_iterator(); ++it)
            {
                ++declared;
                const int n = std::stoi((*it)[1].str());
                highest = n > highest ? n : highest;
            }
            if (declared < 11)
            {
                const std::string o = "o" + std::to_string(highest + 1);
                src.insert(at, "    mov " + o + ".xy, v0.xy\n");
                src.insert(src.find('\n') + 1, "    dcl_texcoord7 " + o + ".xy\n");
                return true;
            }
            // All outputs in use: take over the unread view-position output (texcoord3 .xyz).
            static const std::regex viewDcl(R"(dcl_texcoord3 (o\d+)\.xyz\b)");
            std::smatch m;
            if (!std::regex_search(src, m, viewDcl)) { why = "all outputs in use and no view-position output to take over"; return false; }
            const std::string o = m[1].str();
            const std::regex viewWrite("\n([ \t]*)mov " + o + R"(\.xyz, r\d+[ \t]*\n)");
            std::smatch w;
            if (!std::regex_search(src, w, viewWrite)) { why = "view-position output written unexpectedly"; return false; }
            src.replace(static_cast<size_t>(w.position(0)), static_cast<size_t>(w.length(0)), "\n" + w[1].str() + "mov " + o + ".xy, v0.xy\n");
            src.replace(static_cast<size_t>(m.position(0)), static_cast<size_t>(m.length(0)), "dcl_texcoord7 " + o + ".xy");
            ++g_packed;
            return true;
        }

        bool EditPixel(std::string& src, std::string& why)
        {
            if (src.find("ps_3_0") == std::string::npos) { why = "not ps_3_0"; return false; }
            if (src.find(" v9") != std::string::npos || src.find(" s11") != std::string::npos || src.find("c200") != std::string::npos)
            { why = "v9/s11/c200 in use"; return false; }
            // The stock ending: the colour minus the fog colour, then lerped toward it by the fog.
            static const std::regex fog(R"((\n[ \t]*)add (r\d+)\.xyz, \2, -c2[ \t]*\n[ \t]*mad oC0\.xyz, v\d+\.x, \2, c2)");
            std::smatch m;
            if (!std::regex_search(src, m, fog)) { why = "no fog ending"; return false; }
            const std::string r = m[2].str();
            const std::string block =
                "\n    add r31.xy, v9.xy, -c201.xy"
                "\n    add r31.zw, -v9.xyxy, c201.zwzw"
                "\n    cmp r31, r31, c202.x, c202.y"
                "\n    mul r31.x, r31.x, r31.y"
                "\n    mul r31.x, r31.x, r31.z"
                "\n    mul r31.x, r31.x, r31.w"
                "\n    max r31.x, r31.x, c203.x"
                "\n    mul r29.xy, v9.xy, c200.x"
                "\n    texld r30, r29, s11"
                "\n    max r30.w, r30.w, c200.y"
                "\n    mul r28.xy, v9.xy, c203.y"
                "\n    frc r28.xy, r28"
                "\n    add r28.xy, r28, -c202.z"
                "\n    cmp r28.x, r28.x, c202.y, c202.x"
                "\n    mul r28.x, r28.x, c203.z"
                "\n    max r30.w, r30.w, r28.x"
                "\n    mul r30.x, r30.w, r31.x"
                "\n    mul r30.x, r30.x, c200.w"
                "\n    mad " + r + ".xyz, " + r + ", -r30.x, " + r;
            src.insert(static_cast<size_t>(m.position(0)), block);
            src.insert(src.find('\n') + 1, "    def c202, 1, 0, 0.5, 0\n    dcl_texcoord7 v9.xy\n    dcl_2d s11\n");
            return true;
        }

        const shaderpatch::RuleStatus* Find(const std::vector<shaderpatch::RuleStatus>& all, const char* name)
        {
            for (const auto& s : all) if (s.name == name) return &s;
            return nullptr;
        }

        // Usable only when no terrain vertex shader failed: a patched pixel shader reads v9, which
        // only a patched vertex shader writes.
        bool PairOk(const std::vector<shaderpatch::RuleStatus>& all)
        {
            const shaderpatch::RuleStatus* vs = Find(all, "terrain wetness (vertex)");
            const shaderpatch::RuleStatus* ps = Find(all, "terrain wetness (pixel)");
            return vs && ps && vs->failed == 0 && vs->skipped == 0 && vs->applied > 0 && ps->applied > 0;
        }
    }

    void Register()
    {
        shaderpatch::Register(shaderpatch::TableRule{ "terrain wetness (vertex)", false, { { kTerrainVs, 0x80 } }, &EditVertex });
        shaderpatch::Register(shaderpatch::TableRule{ "terrain wetness (pixel)", true, kTerrainPs, &EditPixel });
    }

    void BeforeTerrainStage(IDirect3DDevice9* device)
    {
        if (!device) return;
        float c[16] = {};
        float inverseExtent = 0.0f, box[4] = {};
        const bool on = g_enabled && PairOk(shaderpatch::Status()) && moisturetex::Mapping(inverseExtent, box) && moisturetex::Bind(device, 11);
        c[0] = inverseExtent;
        c[1] = g_debug >= 1 ? 1.0f : 0.0f;
        c[3] = on ? (g_debug ? 0.8f : g_strength) : 0.0f;
        for (int k = 0; k < 4; ++k) c[4 + k] = box[k];
        c[8] = 1.0f; c[9] = 1.0f; c[10] = 0.0f; c[11] = 0.0f; // c202: the shader's own def wins
        c[12] = g_debug >= 2 ? 1.0f : 0.0f;  // c203.x ignore the box
        c[13] = 0.1f;                         // c203.y stripes every 10 yd (5 dark, 5 not)
        c[14] = g_debug == 3 ? 1.0f : 0.0f;   // c203.z stripes on
        if (g_debug == 3) c[1] = 0.0f;        // stripes only
        device->SetPixelShaderConstantF(kPsConstant, c, 2);
        device->SetPixelShaderConstantF(kPsConstant + 3, c + 12, 1);
    }

    void  SetEnabled(bool enabled) { g_enabled = enabled; }
    void  SetDebug(int mode)       { g_debug = mode < 0 ? 0 : (mode > 3 ? 3 : mode); }
    int   Debug()                  { return g_debug; }
    bool  Enabled()                { return g_enabled; }
    void  SetStrength(float s)     { g_strength = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s); }
    float Strength()               { return g_strength; }

    const char* StatusLine()
    {
        const auto all = shaderpatch::Status();
        const shaderpatch::RuleStatus* vs = Find(all, "terrain wetness (vertex)");
        const shaderpatch::RuleStatus* ps = Find(all, "terrain wetness (pixel)");
        char line[320];
        std::snprintf(line, sizeof(line), "terrain shaders patched: vertex %u (%u reusing an output, skipped %u, failed %u), pixel %u (skipped %u, failed %u)%s%s%s",
                      vs ? vs->applied : 0, g_packed, vs ? vs->skipped : 0, vs ? vs->failed : 0,
                      ps ? ps->applied : 0, ps ? ps->skipped : 0, ps ? ps->failed : 0,
                      PairOk(all) ? "" : " -- not active",
                      (vs && !vs->lastError.empty()) ? ("; vs: " + vs->lastError).c_str() : "",
                      (ps && !ps->lastError.empty()) ? ("; ps: " + ps->lastError).c_str() : "");
        g_status = line;
        return g_status.c_str();
    }
}
