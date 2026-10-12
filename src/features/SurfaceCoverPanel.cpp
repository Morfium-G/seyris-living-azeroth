#include "SurfaceCoverInternal.hpp"

#include "../env/Actors.hpp"
#include "../env/Climate.hpp"
#include "../env/Fields.hpp"
#include "../env/Regional.hpp"
#include "../env/WorldQuery.hpp"
#include "GroundMaterialTable.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::cover::detail
{
    float g_depositYards = 0.2f; // the panel's snow deposit

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
        char line[384];
        g_api->UiCheckbox("Draw the cover", &g_settings.enabled);
        static const char* const coverage[] = { "Everywhere (test: nothing painted, fallen snow anywhere)", "From SurfaceCover.cdbc" };
        g_api->UiCombo("Where", &g_settings.coverage, coverage, 2);
        g_api->UiSliderInt("Levels (40 / 80 / 160 / 320 / 640 yd)", &g_settings.levels, 1, kMaxLevels);
        g_api->UiText("Multipliers on SurfaceCover.cdbc (1 = exactly the table):");
        g_api->UiSliderFloat("Depth (x table)", &g_settings.depthMul, 0.0f, 3.0f);
        g_api->UiSliderFloat("Drift noise (x table)", &g_settings.driftMul, 0.0f, 3.0f);
        g_api->UiSliderFloat("Edge breakup (x table)", &g_settings.breakupMul, 0.0f, 3.0f);
        g_api->UiSliderFloat("Rim (x table)", &g_settings.rimMul, 0.0f, 3.0f);
        g_api->UiSliderFloat("Relax time (x table)", &g_settings.relaxMul, 0.1f, 10.0f);
        g_api->UiCheckbox("Units carve trenches", &g_settings.stamp);
        g_api->UiSliderFloat("Trench width (x unit size)", &g_settings.stampScale, 0.3f, 3.0f);
        g_api->UiSliderFloat("Far lift (yd, 80 -> 400 yd away)", &g_settings.farLift, 0.0f, 3.0f);
        g_api->UiCheckbox("Light and fog like the terrain", &g_settings.sceneLight);
        g_api->UiCheckbox("Dynamic shadows like the terrain", &g_settings.shadows);
        g_api->UiSliderFloat("Snow brightness", &g_settings.brightness, 0.3f, 1.5f);
        g_api->UiSliderFloat("Fill budget (ms per frame)", &g_settings.fillBudgetMs, 0.5f, 10.0f);
        static const char* const sources[] = { "Painted strength of every layer (alpha maps)", "Dominant layer per terrain cell (old)" };
        g_api->UiCombo("Material from", &g_settings.materialSource, sources, 2);
        g_api->UiCheckbox("Swap alpha map axes (debug)", &g_settings.alphaSwap);
        g_api->UiCheckbox("Wireframe", &g_settings.wireframe);
        static const char* const drawPoints[] = { "Right after the terrain (inside the world pass)", "End of the scene (old)" };
        g_api->UiCombo("Draw point", &g_settings.drawPoint, drawPoints, 2);

        g_api->UiSeparator();
        g_api->UiText("Fallen snow (test, not saved): a deposit on your zone, which then melts or stays by the climate.");
        g_api->UiSliderFloat("Deposit (yd)", &g_depositYards, 0.02f, 0.6f);
        if (g_api->UiButton("Deposit snow")) regional::DepositSnow(g_depositYards);
        g_api->UiSameLine();
        if (g_api->UiButton("Clear fallen snow")) regional::ClearSnow();

        g_api->UiSeparator();
        std::snprintf(line, sizeof(line), "SurfaceCover.cdbc: %s", covertable::Status());
        g_api->UiText(line);
        g_api->UiSameLine();
        if (g_api->UiButton("Reload table")) covertable::Load(g_cdbcApi);
        std::snprintf(line, sizeof(line), "  cover textures in view: %u (slots: %d)%s", g_texturesInView, covertable::kMaxCoverTextures,
                      g_texturesInView > static_cast<unsigned>(covertable::kMaxCoverTextures) ? " -- the least present ones draw without their texture" : "");
        g_api->UiText(line);
        for (int sl = 0; sl < covertable::kMaxCoverTextures; ++sl)
        {
            if (!g_slotId[sl]) continue;
            auto st = g_textureStatus.find(g_slotId[sl]);
            std::snprintf(line, sizeof(line), "  slot %d: \"%s\" -- %s", sl + 1, covertable::CoverTexturePath(g_slotId[sl]),
                          st == g_textureStatus.end() ? "?" : st->second.c_str());
            g_api->UiText(line);
        }
        std::vector<std::string> lines;
        auto add = [&lines, &line]() { lines.emplace_back(line); };
        if (const char* why = g_settings.enabled ? g_inactive : "switched off") { std::snprintf(line, sizeof(line), "not drawing: %s", why); add(); }
        const int levels = ActiveLevels();
        for (int k = 0; k < levels; ++k)
        {
            const Level& L = g_levels[k];
            char state[48] = "";
            if (L.jobActive)
                std::snprintf(state, sizeof(state), ", %s %u%%", L.jobHidden ? "filling" : (L.jobFull ? "refilling" : "recentring"),
                              static_cast<unsigned>(L.job.empty() ? 100 : 100 * L.jobPos / L.job.size()));
            std::snprintf(line, sizeof(line), "level %d: %.2f yd cells, %.0f yd out, first cell (%d, %d), holes/unloaded %u, full fill %.2f ms, last recentre %.2f ms%s",
                          k, L.cell, kHalfCells * L.cell, L.gridI, L.gridJ, L.holes, L.firstFillMs, L.fillMs, state); add();
        }
        std::snprintf(line, sizeof(line), "%d x %d vertices; units stamping %u; terrain cells looked up %zu; tint %s",
                      levels * kPatches * kPatches, kPatchVerts * kPatchVerts, g_stamps, static_cast<size_t>(g_cellMisses),
                      g_tintSupported ? "on" : "not supported by this GPU"); add();
        for (const actors::Actor& a : actors::Nearby())
        {
            if (!a.isPlayer) continue;
            const float foot = a.footprintLength > a.footprintWidth ? a.footprintLength : a.footprintWidth;
            std::snprintf(line, sizeof(line), "player footprint: %s, %.2f x %.2f yd -> trench radius %.2f yd%s",
                          a.hasFootprint ? "yes" : "none (no stamp)", a.footprintLength, a.footprintWidth,
                          foot * kFootprintToTrench * g_settings.stampScale, a.mounted ? " (mount's)" : ""); add();
            break;
        }
        if (g_hereValid)
        {
            std::snprintf(line, sizeof(line), "here: texture \"%s\", ground effect %u, TerrainType %d, area %u",
                          g_hereTexture.c_str(), g_hereSurface.groundEffect, g_hereSurface.terrainType, g_hereSurface.area); add();
            const covertable::Values& v = g_hereValues;
            if (v.material)
            {
                const materials::Values mv = materials::Get(v.material);
                std::snprintf(line, sizeof(line), "here: material %u \"%s\" (parent %u), cover material %u \"%s\", rest moisture %.2f, cover stiffness %.2f",
                              v.material, materials::Name(v.material), materials::Parent(v.material),
                              v.coverMaterial, materials::Name(v.coverMaterial), mv.restMoisture, v.stiffness);
            }
            else std::snprintf(line, sizeof(line), "here: no ground material (no selector matches)");
            add();
            std::snprintf(line, sizeof(line), "here (table): depth %.2f, max slope %.0f, fade %.0f, drift %.2f, breakup %.2f, rim %.2f, relax %.0f s, tint %08X x %.2f, cover texture %d, z offset %.3f, wetness %.2f, flags 0x%X",
                          v.depth, v.maxSlope, v.slopeFade, v.driftNoise, v.edgeBreakup, v.rim, v.relaxSeconds, v.tintColor, v.tintStrength, v.coverTexture,
                          v.zOffset, v.wetness, v.flags); add();
            if (g_hereMccvValid)
                std::snprintf(line, sizeof(line), "here: vertex colour (MCCV) r %.2f g %.2f b %.2f (0.50 = neutral)", g_hereMccv[0], g_hereMccv[1], g_hereMccv[2]);
            else
                std::snprintf(line, sizeof(line), "here: no vertex colour (MCCV) in this chunk");
            add();
            if (g_hereLiquidValid)
                std::snprintf(line, sizeof(line), "here: liquid surface z %.2f, %.2f yd above the terrain (%s)", g_hereLiquid, g_hereLiquidDepth,
                              g_hereLiquidDepth > 0.0f ? "underwater" : "dry");
            else
                std::snprintf(line, sizeof(line), "here: no terrain liquid");
            add();
        }
        else { std::snprintf(line, sizeof(line), "here: no terrain under the player"); add(); }
        if (g_hereLayersValid)
        {
            auto dominant = [](const terrain::LayerWeights& lw)
            {
                int best = -1; float w = -1.0f;
                for (int l = 0; l < lw.layers; ++l) if (lw.weight[l] > w) { w = lw.weight[l]; best = l; }
                return best;
            };
            std::snprintf(line, sizeof(line), "here (alpha): %d layer(s), weights %.2f %.2f %.2f %.2f | swapped %.2f %.2f %.2f %.2f",
                          g_hereLayers.layers, g_hereLayers.weight[0], g_hereLayers.weight[1], g_hereLayers.weight[2], g_hereLayers.weight[3],
                          g_hereLayersSwapped.weight[0], g_hereLayersSwapped.weight[1], g_hereLayersSwapped.weight[2], g_hereLayersSwapped.weight[3]); add();
            const int a = dominant(g_hereLayers), b = dominant(g_hereLayersSwapped), c = g_hereLayers.dominantLowRes;
            std::snprintf(line, sizeof(line), "here (check): strongest layer by alpha %d, swapped %d, the client's dominant layer %d -> %s",
                          a, b, c, a == c && b != c ? "alpha read the right way" : (b == c && a != c ? "alpha axes SWAPPED" : "same either way here (try another spot)")); add();
            for (int l = 0; l < g_hereLayers.layers; ++l)
            {
                const terrain::Surface& sf = g_hereLayers.surface[l];
                std::snprintf(line, sizeof(line), "  layer %d: %.2f  \"%s\", effect %u, TerrainType %d", l, g_hereLayers.weight[l],
                              g_hereLayerTexture[l].c_str(), sf.groundEffect, sf.terrainType); add();
            }
        }
        {
            const world::Snapshot& snap = world::Current();
            const fields::SnowDetail sd = snap.inWorld ? fields::Snow(snap.playerPos) : fields::SnowDetail{};
            if (sd.known)
            {
                std::snprintf(line, sizeof(line), "here (snow): fallen %.3f yd -> %.3f (zones' %.3f x held %.2f), painted %.2f yd, kept %.2f -> cap %.2f (kept share %.2f, gone at %.0f degC)",
                              sd.fallen, sd.target, sd.zoneSnow, sd.hold, sd.painted, sd.keep, sd.cap, sd.keptShare, sd.goneTemperature); add();
                std::snprintf(line, sizeof(line), "here (snow): effective %.1f degC (sun %+.1f), zone's snow sees %.1f degC, heat reach %.2f, melt water %.2f",
                              sd.temperature, sd.sun, sd.zoneTemperature, sd.heatReach, sd.meltWet); add();
            }
            else { std::snprintf(line, sizeof(line), "here (snow): not sampled yet"); add(); }
            const climate::Sun& sun = climate::SunNow();
            std::snprintf(line, sizeof(line), "snow: published %u time(s), %s; sun %.2f %.2f %.2f (%s), daylight %.2f; last depth folds %.2f / %.2f / %.2f / %.2f / %.2f ms",
                          fields::GetStats().snowPublished, fields::SnowActive() ? "some snow around" : "none around",
                          sun.dir[0], sun.dir[1], sun.dir[2], sun.fromClient ? "client" : "from the time", sun.daylight,
                          g_levels[0].foldMs, g_levels[1].foldMs, g_levels[2].foldMs, g_levels[3].foldMs, g_levels[4].foldMs); add();
        }
        std::snprintf(line, sizeof(line), "CPU: sim %.2f ms, upload %.2f ms, draw submit %.2f ms", g_simMs, g_uploadMs, g_drawMs); add();
        if (g_seenScene)
        {
            std::snprintf(line, sizeof(line), "terrain light: sun (view) %.3f %.3f %.3f, ambient %.3f %.3f %.3f, diffuse %.3f %.3f %.3f",
                          g_seenLight[0], g_seenLight[1], g_seenLight[2], g_seenLight[4], g_seenLight[5], g_seenLight[6],
                          g_seenLight[8], g_seenLight[9], g_seenLight[10]); add();
            std::snprintf(line, sizeof(line), "terrain fog: params %.6f %.4f %.4f %.4f, colour %.3f %.3f %.3f",
                          g_seenFog[0], g_seenFog[1], g_seenFog[2], g_seenFog[3], g_seenFogColor[0], g_seenFogColor[1], g_seenFogColor[2]); add();
            std::snprintf(line, sizeof(line), "terrain shadow maps at our draw: s5 %s, s6 %s, s7 %s, s8 %s -> cover shadows %s",
                          g_shadowBound[0] ? "bound" : "-", g_shadowBound[1] ? "bound" : "-", g_shadowBound[2] ? "bound" : "-", g_shadowBound[3] ? "bound" : "-",
                          g_shadowUsed ? "on" : (g_settings.shadows ? "off (not all bound: shadow quality off, or the client unbinds them)" : "off (switched off)")); add();
        }

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
