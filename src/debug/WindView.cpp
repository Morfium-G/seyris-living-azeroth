#include "WindView.hpp"

#include "ShaderDump.hpp"

#include "../features/GrassMotion.hpp"
#include "../render/ShaderPatch.hpp"

#include "../env/Shelter.hpp"
#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"

#include <d3d9.h>
#include <cmath>
#include <cstdio>
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

        int   g_showArrows = 1;
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
                g_api->UiSliderFloat("Player push (yards)", &gs.pushStrength, 0.0f, 1.5f);
                g_api->UiSliderFloat("Player push radius", &gs.pushRadius, 0.3f, 5.0f);
                if (g_api->UiButton("Reset grass defaults")) gs = grass::Settings{};
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
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
    }

    void SetWindCdbc(const WXL_SeyrisCdbcApi* cdbc) { g_cdbc = cdbc; }
}
