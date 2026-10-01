#include "WindView.hpp"

#include "ShaderDump.hpp"

#include "../features/GrassDistanceCap.hpp"
#include "../features/GrassDoodads.hpp"
#include "../features/GrassInstanced.hpp"
#include "../features/GrassMotion.hpp"
#include "../features/GrassPerf.hpp"
#include "../render/ShaderPatch.hpp"

#include "../env/Actors.hpp"
#include "../env/Shelter.hpp"
#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        namespace ev = wxl::events;
        namespace gx = wxl::game::gx;

        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: wind";

        const WXL_Api*           g_api  = nullptr;
        const WXL_SeyrisCdbcApi* g_cdbc = nullptr;

        int   g_showArrows = 0;
        int   g_gridSize = 11;        // arrows per side
        float g_spacing = 6.0f;       // yards between arrows
        float g_heightOffset = 1.0f;  // yards above the player's feet
        int   g_forceWeather = 0;
        float g_forcedWeather = 1.0f;

        const char* Compass(float bearing)
        {
            static const char* const names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            const int i = static_cast<int>(std::floor((bearing + 22.5f) / 45.0f)) & 7;
            return names[i];
        }

        const char* SourceText(wind::Source s)
        {
            switch (s)
            {
                case wind::Source::BuiltIn: return "built-in";
                case wind::Source::Global:  return "Global";
                case wind::Source::Map:     return "Map";
                case wind::Source::Area:    return "Area";
            }
            return "?";
        }

        const char* WeatherName(int type)
        {
            switch (type) { case 1: return "rain"; case 2: return "snow"; case 3: return "sand"; default: return "fine"; }
        }

        void __cdecl WindPanel(void* /*user*/)
        {
            char line[256];
            const world::Snapshot& s = world::Current();

            // --- where we are ---
            if (!s.inWorld)
            {
                g_api->UiText("not in world");
            }
            else
            {
                std::snprintf(line, sizeof(line), "map %d   pos %.1f %.1f %.1f   %s",
                              s.mapId, s.playerPos[0], s.playerPos[1], s.playerPos[2],
                              s.outdoors ? "OUTDOORS" : "INDOORS");
                g_api->UiText(line);

                int n = std::snprintf(line, sizeof(line), "areas:");
                for (int i = 0; i < s.areaCount && n > 0 && n < static_cast<int>(sizeof(line)); ++i)
                    n += std::snprintf(line + n, sizeof(line) - n, "%s %u", i ? " ->" : "", s.areaChain[i]);
                g_api->UiText(line);

                std::snprintf(line, sizeof(line), "client says: zone \"%s\", sub-zone \"%s\"", s.zoneText, s.subZoneText);
                g_api->UiText(line);
            }

            std::snprintf(line, sizeof(line), "weather: %s, raw %.3f -> intensity %.2f (model uses %.2f)",
                          WeatherName(s.weatherType), s.weatherRaw, s.weatherIntensity, wind::EffectiveWeather());
            g_api->UiText(line);

            if (g_api->UiCheckbox("Force weather intensity", &g_forceWeather) || g_forceWeather)
                wind::SetWeatherOverride(g_forceWeather ? g_forcedWeather : -1.0f);
            if (g_forceWeather && g_api->UiSliderFloat("Forced intensity", &g_forcedWeather, 0.0f, 1.0f))
                wind::SetWeatherOverride(g_forcedWeather);

            float mult = wind::StrengthMultiplier();
            if (g_api->UiSliderFloat("Global wind multiplier (testing)", &mult, 0.0f, 5.0f))
                wind::SetStrengthMultiplier(mult);

            g_api->UiSeparator();

            // --- resulting wind ---
            const float here[3] = { s.playerPos[0], s.playerPos[1], s.playerPos[2] };
            const wind::Sample w = wind::At(here);
            std::snprintf(line, sizeof(line), "steady: ground %.2f, aloft %.2f   blowing toward %.0f deg (%s)",
                          wind::SteadyGround(), wind::SteadyAloft(), wind::BearingDegrees(), Compass(wind::BearingDegrees()));
            g_api->UiText(line);
            std::snprintf(line, sizeof(line), "at player: strength %.2f, gust %.2f   %s   lee %.2f",
                          w.strength, w.gust, w.open > 0.5f ? "open sky" : "ROOFED (sheltered)", w.lee);
            g_api->UiText(line);

            int shelterOn = shelter::Enabled() ? 1 : 0;
            if (g_api->UiCheckbox("Per-point shelter (roof ray)", &shelterOn))
                shelter::SetEnabled(shelterOn != 0);
            g_api->UiSameLine();
            int leeOn = shelter::LeeEnabled() ? 1 : 0;
            if (g_api->UiCheckbox("Lee ray", &leeOn))
                shelter::SetLeeEnabled(leeOn != 0);
            g_api->UiSameLine();
            if (g_api->UiButton("Clear shelter cache")) shelter::Clear();
            // Lee shape: runtime only, back to defaults each launch.
            shelter::LeeParams lee = shelter::GetLeeParams();
            bool leeChanged = false;
            leeChanged |= g_api->UiSliderFloat("Lee reach (yards)", &lee.reach, 5.0f, 60.0f) != 0;
            leeChanged |= g_api->UiSliderFloat("Lee angle (deg)", &lee.angleDeg, 0.0f, 45.0f) != 0;
            leeChanged |= g_api->UiSliderFloat("Lee strength", &lee.strength, 0.0f, 1.0f) != 0;
            if (leeChanged) shelter::SetLeeParams(lee);
            if (g_api->UiButton("Reset lee defaults")) shelter::SetLeeParams(shelter::LeeParams{});

            const shelter::Stats st = shelter::GetStats();
            std::snprintf(line, sizeof(line), "shelter cache: %u cells, %u rays this frame, %u deferred",
                          st.entries, st.raysThisFrame, st.deferredThisFrame);
            g_api->UiText(line);

            g_api->UiSeparator();

            // --- profile data ---
            if (wind::ProfilesLoaded())
                std::snprintf(line, sizeof(line), "WindProfile.cdbc: %u row(s)", wind::ProfileRowCount());
            else
                std::snprintf(line, sizeof(line), "WindProfile.cdbc: %s", wind::ProfileError());
            g_api->UiTextWrapped(line);
            if (g_api->UiButton("Reload WindProfile.cdbc"))
                wind::LoadProfiles(g_cdbc);

            if (g_api->UiCollapsingHeader("Resolved profile (value / from)"))
            {
                const wind::Resolved& cur = wind::Current();
                const wind::Resolved& tgt = wind::Target();
                for (int f = 0; f < wind::FieldCount; ++f)
                {
                    if (tgt.source[f] == wind::Source::Map || tgt.source[f] == wind::Source::Area)
                        std::snprintf(line, sizeof(line), "%-16s %7.2f  (target %.2f from %s %u)", wind::FieldName(f),
                                      cur.value[f], tgt.value[f], SourceText(tgt.source[f]), tgt.sourceId[f]);
                    else
                        std::snprintf(line, sizeof(line), "%-16s %7.2f  (target %.2f from %s)", wind::FieldName(f),
                                      cur.value[f], tgt.value[f], SourceText(tgt.source[f]));
                    g_api->UiText(line);
                }
            }

            g_api->UiSeparator();

            // --- grass motion ---
            if (const char* why = grass::DisabledReason())
            {
                std::snprintf(line, sizeof(line), "Grass motion OFF: %s", why);
                g_api->UiTextWrapped(line);
            }
            else
            {
                for (const shaderpatch::RuleStatus& st : shaderpatch::Status())
                {
                    std::snprintf(line, sizeof(line), "shader patch '%s': %u applied, %u failed%s%s",
                                  st.name.c_str(), st.applied, st.failed,
                                  st.lastError.empty() ? "" : " -- ", st.lastError.c_str());
                    g_api->UiTextWrapped(line);
                }
                std::snprintf(line, sizeof(line), "grass chunks fed last frame: %u", grass::ChunkUploadsLastFrame());
                g_api->UiText(line);

                grass::Settings& gs = grass::Tunables();
                int grassOn = gs.enabled ? 1 : 0;
                if (g_api->UiCheckbox("Grass motion", &grassOn)) gs.enabled = grassOn != 0;
                g_api->UiSliderFloat("Sway (yards at full wind)", &gs.amplitude, 0.0f, 1.5f);
                g_api->UiSliderFloat("Flutter (yards)", &gs.flutter, 0.0f, 0.4f);
                g_api->UiSliderFloat("Flutter speed", &gs.flutterSpeed, 0.0f, 12.0f);
                g_api->UiSliderFloat("Stiff base (fraction)", &gs.anchor, 0.0f, 0.8f);
                g_api->UiSliderFloat("Push strength (yards)", &gs.pushStrength, 0.0f, 1.5f);
                g_api->UiSliderFloat("Push radius x bounding radius", &gs.radiusScale, 0.5f, 5.0f);
                g_api->UiSliderFloat("Minimum push radius", &gs.minRadius, 0.1f, 3.0f);
                g_api->UiSliderFloat("Mounted radius factor", &gs.mountedScale, 1.0f, 4.0f);

                if (g_api->UiCollapsingHeader("Actors pushing grass"))
                {
                    std::snprintf(line, sizeof(line), "fed to the shader: %u (max %u, within %.0f yd)",
                                  grass::ActorsFed(), grass::kMaxActors, grass::kActorRange);
                    g_api->UiText(line);
                    unsigned shown = 0;
                    for (const actors::Actor& act : actors::Nearby())
                    {
                        if (++shown > 10) break;
                        std::snprintf(line, sizeof(line), "%5.1f yd  bounding radius %.3f  combat reach %.2f%s%s",
                                      act.distance, act.boundingRadius, act.combatReach,
                                      act.mounted ? "  MOUNTED" : "", act.isPlayer ? "  (you)" : "");
                        g_api->UiText(line);
                        if (act.displayId != act.nativeDisplayId)
                        {
                            std::snprintf(line, sizeof(line), "        morphed: display %u (width %.2f) vs native %u (width %.2f) -> radius %.3f",
                                          act.displayId, act.widthNow, act.nativeDisplayId, act.widthNative, act.effectiveRadius);
                            g_api->UiText(line);
                        }
                    }
                }
                int ignoreUv = gs.debugIgnoreUv ? 1 : 0;
                if (g_api->UiCheckbox("Ignore UV bend (debug: whole blades move)", &ignoreUv))
                    gs.debugIgnoreUv = ignoreUv != 0;
                if (g_api->UiButton("Reset grass defaults")) gs = grass::Settings{};

                // Instanced renderer: the client's placement, drawn from our static buffers.
                if (g_api->UiCollapsingHeader("Instanced grass renderer (experimental)"))
                {
                    grassinst::Settings& is = grassinst::Tunables();
                    int instOn = is.enabled ? 1 : 0;
                    if (g_api->UiCheckbox("Instanced grass (off = the client's own path)", &instOn)) is.enabled = instOn != 0;
                    if (const char* why = grassinst::Problem())
                    {
                        std::snprintf(line, sizeof(line), "UNAVAILABLE: %s", why);
                        g_api->UiTextWrapped(line);
                    }
                    const grassinst::Stats st = grassinst::GetStats();
                    g_api->UiSliderFloat("Build budget (ms per frame)", &is.buildBudgetMs, 0.5f, 20.0f);
                    std::snprintf(line, sizeof(line), "last frame: %u layers instanced in %u draw calls; %u built in %.2f ms (%.2f ms in D3D); "
                                  "drawn stock: %u (over budget), %u (model loading), %u (failed)",
                                  st.slotsInstanced, st.drawCalls, st.builds, st.buildMs, st.buildDeviceMs,
                                  st.fallbackBudget, st.fallbackNotLoaded, st.fallbackFailed);
                    g_api->UiTextWrapped(line);
                    std::snprintf(line, sizeof(line), "grass distance cap (groundEffectDist): %.0f yd", grassdistance::CurrentCap());
                    g_api->UiText(line);
                    g_api->UiSliderFloat("Memory limit (MB)", &is.memoryLimitMB, 16.0f, 1024.0f);
                    std::snprintf(line, sizeof(line), "cached: %u layers (%.1f MB in %u of max %u x 4 MB pages)%s, %u doodad models (%.0f KB), %u shaders",
                                  st.slotsCached, st.instanceMB, st.poolPages, st.poolLimitPages,
                                  st.memoryTight ? " AT LIMIT" : "", st.geometries, st.geometryKB, st.shaders);
                    g_api->UiText(line);
                    // Density preview: our own extra plants around a picked spot, to judge the look.
                    g_api->UiSeparator();
                    g_api->UiText("Density near the player (instanced grass only; multiplier 1 = off):");
                    g_api->UiSliderFloat("Density multiplier", &is.densityMultiplier, 1.0f, 8.0f);
                    g_api->UiSliderFloat("Density radius (yards)", &is.densityRadius, 10.0f, 1000.0f);
                    g_api->UiSliderFloat("Copy spread (yards)", &is.densitySpread, 0.2f, 3.0f);
                    std::snprintf(line, sizeof(line), "last frame: %u layers densified, %u extra plants drawn", st.densifiedLayers, st.densityCopies);
                    g_api->UiText(line);
                    g_api->UiSeparator();

                    if (g_api->UiButton(grassinst::VerifyPending() ? "verifying... (waits for a layer build)##iv"
                                                                   : "Verify against the client's bake (next layer build)##iv"))
                        grassinst::RequestVerify();
                    std::snprintf(line, sizeof(line), "verification: %s", grassinst::VerifyReport().c_str());
                    g_api->UiTextWrapped(line);
                }

                // Per-doodad: what was worked out from each model, with live wind/push switches.
                if (g_api->UiCollapsingHeader("Grass doodads seen (per GroundEffectDoodad ID)"))
                {
                    std::snprintf(line, sizeof(line), "layer slots tracked: %u, slots with more than %u doodads: %u",
                                  grassdoodads::SlotsTracked(), grassdoodads::kDoodadEntries,
                                  grassdoodads::SlotsWithTooManyDoodads());
                    g_api->UiText(line);
                    // Diagnostics: layer draws whose build we never saw use the fallback entry.
                    {
                        unsigned tracked = 0, untracked = 0;
                        grassdoodads::FrameCounters(tracked, untracked);
                        std::snprintf(line, sizeof(line), "layer draws since last panel frame: %u tracked, %u UNTRACKED",
                                      tracked, untracked);
                        g_api->UiText(line);
                        const bool fb = grassdoodads::Highlighted() == grassdoodads::kHighlightFallbackId;
                        if (g_api->UiButton(fb ? "stop highlighting fallback##hf" : "highlight fallback (untagged / overflow)##hf"))
                            grassdoodads::SetHighlight(fb ? 0 : grassdoodads::kHighlightFallbackId);
                    }

                    // Overrides file: every change below becomes an override row for that doodad.
                    static char saveMessage[160] = "";
                    std::snprintf(line, sizeof(line), "GroundEffectDoodadWind.cdbc: %u override row(s) in memory",
                                  grassdoodads::OverrideCount());
                    g_api->UiText(line);
                    if (g_api->UiButton("Save overrides"))
                        grassdoodads::SaveOverrides(saveMessage, sizeof(saveMessage));
                    g_api->UiSameLine();
                    if (g_api->UiButton("Reload overrides"))
                    {
                        grassdoodads::LoadOverrides(g_cdbc);
                        std::snprintf(saveMessage, sizeof(saveMessage), "reloaded: %u row(s)", grassdoodads::OverrideCount());
                    }
                    if (saveMessage[0]) g_api->UiTextWrapped(saveMessage);

                    for (const grassdoodads::DoodadInfo& d : grassdoodads::Seen())
                    {
                        std::snprintf(line, sizeof(line), "#%u %s%s", d.id, d.modelPath, d.hasOverride ? "   [override]" : "");
                        g_api->UiTextWrapped(line);
                        if (!d.analyzed)
                            std::snprintf(line, sizeof(line), "   model not loaded yet");
                        else
                            std::snprintf(line, sizeof(line), "   h %.2f  root v %.2f  tip v %.2f  uv [%.2f,%.2f]-[%.2f,%.2f]%s%s",
                                          d.height, d.rootV, d.tipV, d.uMin, d.vMin, d.uMax, d.vMax,
                                          d.autoFlat ? "  FLAT" : "", d.valid ? "" : "  (default bend)");
                        g_api->UiText(line);

                        grassdoodads::Override o = grassdoodads::CurrentAsOverride(d.id);
                        bool changed = false;
                        char label[48];

                        int wind = (o.flags & grassdoodads::kNoWind) ? 0 : 1;
                        std::snprintf(label, sizeof(label), "wind##w%u", d.id);
                        if (g_api->UiCheckbox(label, &wind))
                        { o.flags = wind ? (o.flags & ~grassdoodads::kNoWind) : (o.flags | grassdoodads::kNoWind); changed = true; }
                        g_api->UiSameLine();

                        int push = (o.flags & grassdoodads::kNoPush) ? 0 : 1;
                        std::snprintf(label, sizeof(label), "push##p%u", d.id);
                        if (g_api->UiCheckbox(label, &push))
                        { o.flags = push ? (o.flags & ~grassdoodads::kNoPush) : (o.flags | grassdoodads::kNoPush); changed = true; }
                        g_api->UiSameLine();

                        int flip = (o.flags & grassdoodads::kFlip) ? 1 : 0;
                        std::snprintf(label, sizeof(label), "flip root/tip##f%u", d.id);
                        if (g_api->UiCheckbox(label, &flip))
                        { o.flags = flip ? (o.flags | grassdoodads::kFlip) : (o.flags & ~grassdoodads::kFlip); changed = true; }

                        std::snprintf(label, sizeof(label), "stiffness##s%u", d.id);
                        if (g_api->UiSliderFloat(label, &o.stiffness, 0.0f, 1.0f)) changed = true;

                        if (changed) grassdoodads::SetOverride(d.id, o);

                        const bool lit = grassdoodads::Highlighted() == d.id;
                        std::snprintf(label, sizeof(label), "%s##h%u", lit ? "stop highlight" : "highlight (lift in world)", d.id);
                        if (g_api->UiButton(label)) grassdoodads::SetHighlight(lit ? 0 : d.id);
                        if (d.hasOverride) g_api->UiSameLine();
                        if (d.hasOverride)
                        {
                            std::snprintf(label, sizeof(label), "back to automatic##a%u", d.id);
                            if (g_api->UiButton(label)) grassdoodads::ClearOverride(d.id);
                        }
                    }
                }
            }

            g_api->UiSeparator();
            // Research: dump the stock grass vertex shaders for the wind-aware replacement.
            if (g_api->UiButton("Dump grass shaders (Logs\\living-azeroth)"))
                DumpGrassShaders(g_api);

            g_api->UiSeparator();
            g_api->UiCheckbox("Show wind arrows", &g_showArrows);
            g_api->UiSliderInt("Grid size", &g_gridSize, 3, 25);
            g_api->UiSliderFloat("Spacing (yards)", &g_spacing, 2.0f, 20.0f);
            g_api->UiSliderFloat("Height above feet", &g_heightOffset, 0.0f, 10.0f);
        }

        // --- grass performance -------------------------------------------------------------------
        std::vector<std::string> GrassPerfReport()
        {
            std::vector<std::string> out;
            char line[256];
            const grassperf::Summary s = grassperf::Summarize();
            if (s.frames == 0) { out.emplace_back("no frames recorded yet"); return out; }

            const world::Snapshot& w = world::Current();
            std::snprintf(line, sizeof(line), "grass performance -- map %d, pos %.0f %.0f %.0f",
                          w.mapId, w.playerPos[0], w.playerPos[1], w.playerPos[2]);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "last %u frames: median %.2f ms (%.0f fps), average %.2f ms",
                          s.frames, s.medianFrameMs, s.medianFrameMs > 0 ? 1000.0 / s.medianFrameMs : 0.0,
                          s.average.frameMs);
            out.emplace_back(line);
            out.emplace_back("AVERAGE PER FRAME");
            std::snprintf(line, sizeof(line), "  grass pass %.2f ms   chunks %u   layer draws %u (submit %.2f ms)",
                          s.average.passMs, s.average.chunks, s.average.draws, s.average.drawSubmitMs);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "  layer builds %u   %.2f ms   plants %u   vertices %u",
                          s.average.builds, s.average.buildMs, s.average.plantsBuilt, s.average.verticesBuilt);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "  instanced: %u layers, %u draw calls, %u plants   instance builds %u (%.2f ms, %.2f ms in D3D)",
                          s.average.instSlots, s.average.instDrawCalls, s.average.instPlants, s.average.instBuilds,
                          s.average.instBuildMs, s.average.instBuildDeviceMs);
            out.emplace_back(line);
            out.emplace_back("WORST FRAME");
            std::snprintf(line, sizeof(line), "  frame %.2f ms   grass pass %.2f ms   draws %u (submit %.2f ms)",
                          s.worst.frameMs, s.worst.passMs, s.worst.draws, s.worst.drawSubmitMs);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "  layer builds %u   %.2f ms total   %.2f ms slowest   plants %u   vertices %u",
                          s.worst.builds, s.worst.buildMs, s.worst.worstBuildMs, s.worst.plantsBuilt, s.worst.verticesBuilt);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "  instanced: %u layers, %u draw calls   instance builds %u (%.2f ms, %.2f ms in D3D)",
                          s.worst.instSlots, s.worst.instDrawCalls, s.worst.instBuilds, s.worst.instBuildMs, s.worst.instBuildDeviceMs);
            out.emplace_back(line);
            std::snprintf(line, sizeof(line), "spike frames (> 2x median): %u   share of spike time spent building grass: %.0f%%",
                          s.spikeFrames, s.spikeBuildShare * 100.0);
            out.emplace_back(line);
            return out;
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

        void __cdecl GrassPerfPanel(void* /*user*/)
        {
            const std::vector<std::string> report = GrassPerfReport();
            for (const std::string& l : report) g_api->UiText(l.c_str());

            static char status[96] = "";
            if (g_api->UiButton("Reset measurements")) { grassperf::Reset(); status[0] = '\0'; }
            g_api->UiSameLine();
            if (g_api->UiButton("Copy report to clipboard"))
            {
                std::string text;
                for (const std::string& l : report) text += l + "\r\n";
                std::snprintf(status, sizeof(status), CopyToClipboard(text) ? "copied" : "clipboard unavailable");
            }
            g_api->UiSameLine();
            if (g_api->UiButton("Write report to log"))
            {
                for (const std::string& l : report) g_api->Log(WXL_LOG_INFO, "wxl-seyris-living-azeroth", "%s", l.c_str());
                std::snprintf(status, sizeof(status), "written to wxl-core.log");
            }
            if (status[0]) g_api->UiText(status);
        }

        // --- world arrows ---------------------------------------------------------------------------
        struct LineVtx { float x, y, z; D3DCOLOR c; };

        D3DCOLOR StrengthColor(float strength, float gust, float lee)
        {
            const float t = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
            float r = t < 0.5f ? t * 2.0f : 1.0f;
            float g = t < 0.5f ? 1.0f : 2.0f - t * 2.0f;
            float b = 0.0f;
            // A passing gust brightens toward white.
            r += (1.0f - r) * gust * 0.8f; g += (1.0f - g) * gust * 0.8f; b += gust * 0.8f;
            // Wind shadow tints toward purple by how deep in the lee the point is.
            const float shade = 1.0f - (lee < 0.0f ? 0.0f : (lee > 1.0f ? 1.0f : lee));
            r += (0.75f - r) * shade; g += (0.25f - g) * shade; b += (1.0f - b) * shade;
            return D3DCOLOR_ARGB(255, static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255));
        }

        void AddArrow(std::vector<LineVtx>& v, float cx, float cy, float cz, const wind::Sample& w)
        {
            if (w.open < 0.5f)
            {
                // Roofed over: a small cyan cross, so the roof outline shows up in the grid.
                const D3DCOLOR c = D3DCOLOR_ARGB(255, 0, 200, 255);
                const float r = 0.4f;
                v.push_back({ cx - r, cy - r, cz, c }); v.push_back({ cx + r, cy + r, cz, c });
                v.push_back({ cx - r, cy + r, cz, c }); v.push_back({ cx + r, cy - r, cz, c });
                return;
            }

            const float len = 0.6f + w.strength * 5.0f; // yards
            const float hx = w.dirX * len * 0.5f, hy = w.dirY * len * 0.5f;
            const D3DCOLOR c = StrengthColor(w.strength, w.gust, w.lee);

            const float tipX = cx + hx, tipY = cy + hy;
            v.push_back({ cx - hx, cy - hy, cz, c });
            v.push_back({ tipX, tipY, cz, c });

            // Head: two short lines angled back from the tip.
            const float headLen = 0.25f + len * 0.2f;
            const float bx = -w.dirX * headLen, by = -w.dirY * headLen;
            const float sx = -w.dirY * headLen * 0.5f, sy = w.dirX * headLen * 0.5f;
            v.push_back({ tipX, tipY, cz, c }); v.push_back({ tipX + bx + sx, tipY + by + sy, cz, c });
            v.push_back({ tipX, tipY, cz, c }); v.push_back({ tipX + bx - sx, tipY + by - sy, cz, c });
        }

        void __cdecl OnWorldSceneEnd(void* /*user*/, const void* args)
        {
            if (!g_showArrows) return;
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) return;

            const auto* a = static_cast<const ev::WorldSceneEndArgs*>(args);
            auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
            if (!dev) return;

            std::vector<LineVtx> verts;
            verts.reserve(static_cast<size_t>(g_gridSize) * g_gridSize * 6);
            const float half = (g_gridSize - 1) * 0.5f;
            const float z = s.playerPos[2] + g_heightOffset;
            for (int iy = 0; iy < g_gridSize; ++iy)
                for (int ix = 0; ix < g_gridSize; ++ix)
                {
                    const float p[3] = { s.playerPos[0] + (ix - half) * g_spacing,
                                         s.playerPos[1] + (iy - half) * g_spacing, z };
                    AddArrow(verts, p[0], p[1], p[2], wind::At(p));
                }
            if (verts.empty()) return;

            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;

            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(identity));
            dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(wxl::game::camera::GetView()));
            dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(wxl::game::camera::GetProjection()));

            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetTexture(0, nullptr);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE); // debug arrows stay visible through terrain
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

            dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
            dev->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(verts.size() / 2), verts.data(), sizeof(LineVtx));

            saved->Apply();
            saved->Release();
        }
    }

    void RegisterWindPanel(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &WindPanel, nullptr);
        api->UiAddPanel("wxl-seyris-living-azeroth: grass performance", &GrassPerfPanel, nullptr);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
    }

    void SetWindCdbc(const WXL_SeyrisCdbcApi* cdbc) { g_cdbc = cdbc; }

    void SetShowArrows(bool show) { g_showArrows = show ? 1 : 0; }
    bool ShowArrows() { return g_showArrows != 0; }
}
