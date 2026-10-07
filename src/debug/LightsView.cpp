#include "LightsView.hpp"

#include "../env/Lights.hpp"
#include "../env/WorldQuery.hpp"
#include "../features/DoodadLightTable.hpp"
#include "../features/ModelLights.hpp"
#include "../features/SurfaceCover.hpp"
#include "../features/TerrainLights.hpp"
#include "../features/TerrainWetness.hpp"
#include "../render/M2Effects.hpp"
#include "ShaderDump.hpp"
#include "WmoBindProbe.hpp"

#include "game/Doodad.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: lights";
        const WXL_Api* g_api = nullptr;
        const WXL_SeyrisCdbcApi* g_cdbc = nullptr;
        float g_range = 60.0f;
        int   g_maxListed = 40;
        int   g_showDoodads = 1;
        float g_doodadRange = 30.0f;

        namespace dd = wxl::game::doodad;

        // --- placed doodads: their model's bones, attachments, emitters and lights -------------------
        // Instance = the doodad's render instance (CM2Model, doodad +0x34; core's doodad SDK). Traced
        // 2026-10-05 (lighting.md): the instance's scene at +0x28 (its +0xC4 is a matrix the light
        // positions go through), bone matrices at +0x98 (64 bytes each), light records at +0x1D0
        // (0xD4 each, the CM2Light at +0x68). WoW_M2_UpdateAttachmentsAndChildren 0x828A00 places a
        // point light at (file position x bone matrix) x scene matrix.
        constexpr size_t kInstScene = 0x28, kInstBones = 0x98, kInstLights = 0x1D0;
        constexpr size_t kSceneMatrix = 0xC4;
        constexpr size_t kLightRecord = 0xD4, kRecordLight = 0x68;
        // M2 header arrays (core M2Format: count, then a pointer once parsed).
        constexpr size_t kHdrBones = 0x2C, kHdrAttachments = 0xF0, kHdrLights = 0x108, kHdrEmitters = 0x128;
        constexpr size_t kBoneSize = 0x58, kAttachSize = 0x28 /* [believed: wowdev] */, kLightSize = 0x9C, kEmitterSize = 0x1DC;
        constexpr size_t kEmitterBone = 0x14; // confirmed: +0x16 is its texture (InitializeLoaded 0x833ED5)

        template <class T> bool Get(const void* base, size_t off, T& out)
        {
            const void* p = static_cast<const uint8_t*>(base) + off;
            if (!dd::detail::Readable(p, sizeof(T))) return false;
            std::memcpy(&out, p, sizeof(T));
            return true;
        }

        // Row vector x 4x4 (translation in row 3), as the client's C3Vector x C44Matrix.
        void Mul(const float v[3], const float m[16], float out[3])
        {
            for (int c = 0; c < 3; ++c) out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c];
        }

        void DescribeDoodads(const world::Snapshot& s, std::vector<std::string>& lines)
        {
            char line[512];
            auto add = [&]() { lines.emplace_back(line); };
            void* found[512];
            float center[3] = { s.playerPos[0], s.playerPos[1], s.playerPos[2] };
            const int listed = dd::EnumerateAround(center, 33.3333f, found, 512);
            // A doodad spanning several chunks is in each chunk's list: once each.
            int n = 0;
            for (int k = 0; k < listed; ++k)
                if (std::find(found, found + n, found[k]) == found + n) found[n++] = found[k];
            // Which CM2Lights are in the scene's grid right now (the client unlinks lights of models
            // the camera doesn't see).
            static std::vector<lights::ClientLight> grid;
            lights::Gather(s.playerPos, 0.0f, grid);
            int shown = 0;
            std::snprintf(line, sizeof(line), "doodads in the 3x3 chunks around you: %d (%d list entries; listing those within %.0f yd)", n, listed, g_doodadRange); add();
            for (int k = 0; k < n; ++k)
            {
                void* d = found[k];
                float pos[3];
                dd::Position(d, pos);
                const float dx = pos[0] - s.playerPos[0], dy = pos[1] - s.playerPos[1], dz = pos[2] - s.playerPos[2];
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist > g_doodadRange) continue;
                void* inst = dd::Instance(d);
                void* model = inst ? dd::detail::P(inst, dd::off::kInstModel) : nullptr;
                const char* path = model ? static_cast<dd::off::M2ModelCache*>(model)->fullPath : "";
                void* hdr = model ? dd::detail::P(model, dd::off::kModelHeader) : nullptr;
                uint32_t nBones = 0, nAtt = 0, nLights = 0, nEmit = 0;
                const uint8_t *bones = nullptr, *atts = nullptr, *mlights = nullptr, *emits = nullptr;
                if (hdr)
                {
                    Get(hdr, kHdrBones, nBones); Get(hdr, kHdrBones + 4, bones);
                    Get(hdr, kHdrAttachments, nAtt); Get(hdr, kHdrAttachments + 4, atts);
                    Get(hdr, kHdrLights, nLights); Get(hdr, kHdrLights + 4, mlights);
                    Get(hdr, kHdrEmitters, nEmit); Get(hdr, kHdrEmitters + 4, emits);
                }
                ++shown;
                std::snprintf(line, sizeof(line), "%5.1f yd  \"%s\"  scale %.2f  instance %08X  bones %u, attachments %u, emitters %u, lights %u",
                              dist, dd::detail::Readable(path, 1) ? path : "?", dd::Scale(d),
                              static_cast<unsigned>(reinterpret_cast<uintptr_t>(inst)), nBones, nAtt, nEmit, nLights); add();
                if (!hdr) continue;

                if (atts && nAtt && nAtt < 64)
                {
                    int len = std::snprintf(line, sizeof(line), "    attachments (id:bone):");
                    for (uint32_t a = 0; a < nAtt && len < 440; ++a)
                    {
                        uint32_t id = 0; uint16_t bone = 0;
                        Get(atts + a * kAttachSize, 0, id); Get(atts + a * kAttachSize, 4, bone);
                        len += std::snprintf(line + len, sizeof(line) - len, " %u:%u", id, bone);
                    }
                    add();
                }
                if (emits && nEmit && nEmit < 64)
                {
                    int len = std::snprintf(line, sizeof(line), "    emitters (index:bone):");
                    for (uint32_t e = 0; e < nEmit && len < 440; ++e)
                    {
                        uint16_t bone = 0;
                        Get(emits + e * kEmitterSize, kEmitterBone, bone);
                        len += std::snprintf(line + len, sizeof(line) - len, " %u:%u", e, bone);
                    }
                    add();
                }
                // Each model light: recompute its world position like the client and compare it with
                // the CM2Light the client put in the scene grid.
                const uint8_t* records = nullptr; const uint8_t* boneMats = nullptr; const uint8_t* scene = nullptr;
                if (inst) { Get(inst, kInstLights, records); Get(inst, kInstBones, boneMats); Get(inst, kInstScene, scene); }
                float sceneM[16] = {};
                const bool haveScene = scene && dd::detail::Readable(scene + kSceneMatrix, 64);
                if (haveScene) std::memcpy(sceneM, scene + kSceneMatrix, 64);
                for (uint32_t l = 0; mlights && l < nLights && l < 16; ++l)
                {
                    const uint8_t* fl = mlights + l * kLightSize;
                    uint16_t type = 0; int16_t bone = -1; float local[3] = {};
                    Get(fl, 0, type); Get(fl, 2, bone); Get(fl, 4, local);
                    float computed[3] = {}, stored[3] = {};
                    bool haveComputed = false, haveStored = false;
                    if (boneMats && haveScene && bone >= 0 && static_cast<uint32_t>(bone) < nBones &&
                        dd::detail::Readable(boneMats + bone * 64, 64))
                    {
                        float boneM[16], mid[3];
                        std::memcpy(boneM, boneMats + bone * 64, 64);
                        Mul(local, boneM, mid);
                        Mul(mid, sceneM, computed);
                        haveComputed = true;
                    }
                    const uint8_t* cm2 = records ? records + l * kLightRecord + kRecordLight : nullptr;
                    if (cm2) haveStored = Get(cm2 + 0x0C, 0, stored);
                    // At rest a bone adds nothing: the file position through the doodad's world matrix
                    // (instance +0xB4, read by the renderer every frame, culled or not).
                    float worldM[16], rest[3] = {};
                    const bool haveRest = dd::WorldMatrix(d, worldM);
                    if (haveRest) Mul(local, worldM, rest);
                    auto verdict = [&](bool have, const float v[3])
                    {
                        if (!have || !haveStored) return "can't compare";
                        const float ex = v[0] - stored[0], ey = v[1] - stored[1], ez = v[2] - stored[2];
                        return std::sqrt(ex * ex + ey * ey + ez * ez) < 0.05f ? "MATCH" : "DIFFERENT";
                    };
                    bool inGrid = false;
                    for (const lights::ClientLight& g : grid) inGrid |= g.address == cm2;
                    std::snprintf(line, sizeof(line), "    light %u: type %u, bone %d, local %.2f %.2f %.2f; client's CM2Light %08X at %.2f %.2f %.2f (%s)",
                                  l, type, bone, local[0], local[1], local[2],
                                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(cm2)), stored[0], stored[1], stored[2],
                                  inGrid ? "in the grid now" : "NOT in the grid: culled"); add();
                    std::snprintf(line, sizeof(line), "      via bone x scene matrix: %.2f %.2f %.2f -> %s   |   via world matrix (rest): %.2f %.2f %.2f -> %s",
                                  computed[0], computed[1], computed[2], verdict(haveComputed, computed),
                                  rest[0], rest[1], rest[2], verdict(haveRest, rest)); add();
                }
            }
            if (!shown) { std::snprintf(line, sizeof(line), "  (none within %.0f yd)", g_doodadRange); add(); }
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
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) { g_api->UiText("not in world"); return; }

            // Our lights (step 2: on the surface cover; the terrain keeps the stock lighting for now).
            lights::Settings& cfg = lights::Config();
            g_api->UiCheckbox("our point lights (terrain + surface cover; off = the client's own)", &cfg.enabled);
            g_api->UiSliderFloat("light radius (yd)", &cfg.radius, 2.0f, 40.0f);
            g_api->UiSliderFloat("light brightness (x the client's colour)", &cfg.brightness, 0.0f, 3.0f);
            g_api->UiSliderFloat("light range from the camera (yd)", &cfg.range, 30.0f, 512.0f);
            g_api->UiSliderFloat("light grid: cell size (yd)", &cfg.cellSize, 8.0f, 64.0f);
            g_api->UiSliderInt("light grid: lights per cell", &cfg.maxPerCell, 4, lights::kMaxPerCellCap);
            g_api->UiCheckbox("interior lights don't light the outside: terrain, cover, WMO exteriors (indoor WMO groups; door-frame torches lose the entrance)", &cfg.indoorSkipsTerrain);
            g_api->UiCheckbox("baked occlusion: walls and floors block lights that have it (light editor: \"bake occlusion\")", &cfg.bakeEnabled);
            g_api->UiCheckbox("debug: bake EVERY light (not only flagged ones; attached/moving lights never)", &cfg.bakeAll);
            g_api->UiSliderInt("baking: rays per frame", &cfg.bakeRaysPerFrame, 0, 20000);
            {
                const lights::Stats bs = lights::GetStats();
                char bakeLine[256];
                std::snprintf(bakeLine, sizeof(bakeLine), "baking: %u drawn light(s) want occlusion, %u ready; %u rays this frame (%.2f ms); %u light(s) cached (%d rays x 5 per light)",
                              bs.bakeWanted, bs.bakeDone, bs.bakeRays, bs.bakeMs, bs.bakeCached, lights::kOccCells);
                g_api->UiTextWrapped(bakeLine);
            }
            g_api->UiSliderFloat("merge lights closer than (yd)", &cfg.mergeDistance, 0.0f, 3.0f);
            g_api->UiSliderFloat("flicker (how much a flame's light dips)", &cfg.flicker, 0.0f, 0.8f);
            g_api->UiSliderFloat("flicker speed", &cfg.flickerSpeed, 0.1f, 4.0f);
            g_api->UiCheckbox("our lights on models too (M2s, WMOs; per vertex)", &cfg.models);
            g_api->UiCheckbox("models: replace the client's up to 4 lights (off = add ours to them)", &cfg.modelStockOff);
            g_api->UiSliderFloat("WMO interiors: our light x (fills up to the baked light, never stacks)", &cfg.bakedAdd, 0.0f, 3.0f);
            {
                static const char* const kViews[] = { "off", "our lights only", "count check (flat red)", "world stripes (must not move)",
                                                      "mark WMO baked-light surfaces (red)",
                                                      "WMO interiors: normal check (blue own, green given, red none: distance-only)" };
                g_api->UiCombo("models: debug view", &modellights::DebugView(), kViews, 6);
                g_api->UiCheckbox("models: probe the light texture at every model draw", &modellights::Probe());
                const std::string probe = modellights::ProbeLine();
                g_api->UiTextWrapped(probe.c_str());
                if (g_api->UiButton("Copy model-light status##mlcopy"))
                    CopyToClipboard(std::string(modellights::StatusLine()) + "\r\n" + probe + "\r\n");

                // Research: which effect permutation WMO batches bind (interior lighting rule).
                g_api->UiCheckbox("WMO bind probe: record the shader permutations WMO batches use", &WmoBindProbeEnabled());
                if (g_api->UiButton("Clear##wmoprobe")) ClearWmoBindProbe();
                g_api->UiSameLine();
                const std::string report = WmoBindProbeReport();
                if (g_api->UiButton("Copy WMO bind probe##wmoprobecopy"))
                    CopyToClipboard(std::string(modellights::StatusLine()) + "\r\n" + report);
                g_api->UiTextWrapped(report.c_str());
            }
            const lights::Stats ls = lights::GetStats();
            char head[320];
            std::snprintf(head, sizeof(head), "DoodadLightProperties / DoodadLightAssignment: %s%s; in range: %u table light(s), %u model(s) with their own lights suppressed, %u attachment(s) not found (at the origin instead)",
                          lighttable::Status(), lighttable::Dirty() ? " (edited, not saved)" : "", ls.tableLights, ls.suppressedModels, ls.attachFallbacks);
            g_api->UiTextWrapped(head);
            if (g_api->UiButton("Reload light tables from disk")) lighttable::Load(g_cdbc);
            // Research for M2 receivers: the M2 shaders the client has loaded so far.
            std::snprintf(head, sizeof(head), "Dump M2 shaders (%u effect(s) loaded; Logs\\living-azeroth)",
                          static_cast<unsigned>(m2effects::Effects().size()));
            if (g_api->UiButton(head)) DumpM2Shaders(g_api);
            std::snprintf(head, sizeof(head), "attached models in range (items in hands, ...): %u, of those %u at the parent's rest pose (not animated now); indoor lights (kept off the terrain): %u (%u locate call(s) this frame)",
                          ls.attachedModels, ls.attachedAtRest, ls.indoor, ls.indoorTests);
            g_api->UiTextWrapped(head);
            std::snprintf(head, sizeof(head), "our lights: %u models in the scene, %u in range (scan %.2f ms), %u model lights (%u placed by the client, %u via the world matrix, %u with the file's colour), %u in range, %u merged, %u drawn (max %d%s)",
                          ls.models, ls.modelsInRange, ls.scanMs, ls.modelLights, ls.fromClient, ls.fromWorldMatrix, ls.fileColor,
                          ls.inRange, ls.merged, ls.active, lights::kMaxPool, ls.poolDropped ? ", the farthest dropped" : "");
            g_api->UiTextWrapped(head);
            {
                const lights::Grid& grid = lights::CellGrid();
                unsigned patchMost = 0, patchFull = 0;
                cover::PatchLightStats(patchMost, patchFull);
                char gridLine[384];
                std::snprintf(gridLine, sizeof(gridLine), "light grid: %d x %d cells of %.0f yd (%.0f yd across), up to %d lights each; busiest cell %u lights; %u cell(s) over the limit (%u light entries left out: raise \"lights per cell\" if this stays above 0 where it matters). Surface cover: busiest patch %u lights, %u patch(es) over %d",
                              grid.cells, grid.cells, grid.cellSize, grid.cells * grid.cellSize, grid.perCell, ls.busiestCell, ls.fullCells, ls.droppedFromCells,
                              patchMost, patchFull, lights::kMaxLights);
                g_api->UiTextWrapped(gridLine);
            }
            g_api->UiTextWrapped(terrainlights::StatusLine());
            g_api->UiTextWrapped(modellights::StatusLine());
            g_api->UiTextWrapped(terrainwet::StatusLine());
            g_api->UiSeparator();

            g_api->UiSliderFloat("range (yd)", &g_range, 5.0f, 300.0f);
            g_api->UiSliderInt("lights listed", &g_maxListed, 5, 200);

            static std::vector<lights::ClientLight> list;
            const lights::Scan scan = lights::Gather(s.playerPos, g_range, list);

            std::vector<std::string> lines;
            char line[384];
            auto add = [&]() { lines.emplace_back(line); };
            {
                const lights::Stats ls2 = lights::GetStats();
                std::snprintf(line, sizeof(line), "our lights: %u model lights (%u placed by the client, %u via the world matrix, %u with the file's colour), %u in range, %u merged, %u drawn",
                              ls2.modelLights, ls2.fromClient, ls2.fromWorldMatrix, ls2.fileColor, ls2.inRange, ls2.merged, ls2.active); add();
                int shownActive = 0;
                for (const lights::ActiveLight& a : lights::Active())
                {
                    if (shownActive++ >= 8) break;
                    std::snprintf(line, sizeof(line), "  drawn: pos %.1f %.1f %.1f, colour %.2f %.2f %.2f, radius %.0f", a.pos[0], a.pos[1], a.pos[2],
                                  a.color[0], a.color[1], a.color[2], a.radius); add();
                }
            }
            if (!scan.sceneFound) { std::snprintf(line, sizeof(line), "no M2 scene ([0xCD754C] is null)"); add(); }
            else
            {
                std::snprintf(line, sizeof(line), "scene: %u global + %u grid lights; %u point, %u visible, %u stale%s; point lights at the default falloff (0, 0.7, 0.03): %u of %u",
                              scan.globalCount, scan.gridCount, scan.pointCount, scan.visibleCount, scan.staleCount,
                              scan.truncated ? ", A CHAIN WAS CUT OFF (cycle guard)" : "", scan.defaultAttenuation, scan.pointCount); add();
                std::snprintf(line, sizeof(line), "within %.0f yd: %u (nearest first)", g_range, static_cast<unsigned>(list.size())); add();
            }
            int listed = 0;
            for (const lights::ClientLight& l : list)
            {
                if (listed++ >= g_maxListed) break;
                const float dx = l.pos[0] - s.playerPos[0], dy = l.pos[1] - s.playerPos[1], dz = l.pos[2] - s.playerPos[2];
                const float reach = lights::Reach(l.attenuation, 0.05f);
                char where[16];
                if (l.global) std::snprintf(where, sizeof(where), "global");
                else std::snprintf(where, sizeof(where), "cell %d,%d", l.cell % 64, l.cell / 64);
                std::snprintf(line, sizeof(line),
                              "%5.1f yd  %08X %s type %u%s%s  pos %.1f %.1f %.1f  diffuse %.2f %.2f %.2f  ambient %.2f %.2f %.2f  +0x48 %.2f %.2f %.2f  atten %.3f %.3f %.4f (5%% at %.1f yd)  dir %.2f %.2f %.2f",
                              std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<unsigned>(reinterpret_cast<uintptr_t>(l.address)), where, l.type,
                              l.visible ? "" : " HIDDEN", l.stale ? " STALE" : "",
                              l.pos[0], l.pos[1], l.pos[2], l.diffuse[0], l.diffuse[1], l.diffuse[2],
                              l.ambient[0], l.ambient[1], l.ambient[2], l.extra[0], l.extra[1], l.extra[2],
                              l.attenuation[0], l.attenuation[1], l.attenuation[2], reach, l.dir[0], l.dir[1], l.dir[2]); add();
            }

            g_api->UiCheckbox("doodads near you (model, bones, attachments, emitters, own lights)", &g_showDoodads);
            g_api->UiSliderFloat("doodad range (yd)", &g_doodadRange, 2.0f, 50.0f);
            if (g_showDoodads) { lines.emplace_back(""); DescribeDoodads(s, lines); }

            static char status[64] = "";
            if (g_api->UiButton("Copy to clipboard"))
            {
                std::string text;
                for (const std::string& t : lines) text += t + "\r\n";
                std::snprintf(status, sizeof(status), CopyToClipboard(text) ? "copied" : "clipboard unavailable");
            }
            if (status[0]) { g_api->UiSameLine(); g_api->UiText(status); }
            for (const std::string& t : lines) g_api->UiTextWrapped(t.c_str());
        }
    }

    void SetLightsCdbc(const WXL_SeyrisCdbcApi* cdbc) { g_cdbc = cdbc; }

    void RegisterLightsPanel(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }
}
