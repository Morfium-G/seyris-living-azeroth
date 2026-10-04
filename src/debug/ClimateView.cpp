#include "ClimateView.hpp"

#include "../env/Climate.hpp"
#include "../env/Fields.hpp"
#include "../env/Regional.hpp"
#include "../env/TerrainHeight.hpp"
#include "../features/TerrainWetness.hpp"
#include "../env/WorldQuery.hpp"

#include <cstdio>
#include <cstring>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: climate";
        const WXL_Api* g_api = nullptr;
        const WXL_SeyrisCdbcApi* g_cdbc = nullptr;

        int   g_weatherChoice = 0;    // 0 live, 1 fine, 2 rain, 3 snow, 4 sand
        float g_weatherIntensity = 1.0f;
        int   g_timeOn = 0;
        float g_timeHours = 12.0f;

        void __cdecl Panel(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[256];
            if (!s.inWorld) { g_api->UiText("not in world"); return; }

            const climate::Clock& c = climate::Now();
            const int minutes = static_cast<int>(c.dayFraction * 1440.0f + 0.5f) % 1440;
            std::snprintf(line, sizeof(line), "game date %04d-%02d-%02d %02d:%02d (server); sky time %02d:%02d; year %.2f",
                          c.year, c.month, c.day, c.hour, c.minute, minutes / 60, minutes % 60, c.yearFraction);
            g_api->UiText(line);

            // Where: the client's zone names and the area chain (what AreaClimate and the other scoped
            // tables match against, most specific first).
            std::snprintf(line, sizeof(line), "zone \"%s\", sub-zone \"%s\", map %d", s.zoneText, s.subZoneText, s.mapId);
            g_api->UiText(line);
            int n = std::snprintf(line, sizeof(line), "area chain:");
            for (int i = 0; i < s.areaCount && n < static_cast<int>(sizeof(line)) - 16; ++i)
                n += std::snprintf(line + n, sizeof(line) - n, "%s %u", i ? " ->" : "", s.areaChain[i]);
            if (!s.areaCount) std::snprintf(line + n, sizeof(line) - n, " none");
            g_api->UiText(line);

            const uint32_t area = s.areaCount > 0 ? s.areaChain[0] : 0;
            const climate::Row& row = climate::For(area, s.mapId);
            std::snprintf(line, sizeof(line), "AreaClimate.cdbc: %s", climate::Status());
            g_api->UiText(line);
            std::snprintf(line, sizeof(line), "here (area %u, map %d): day %.1f / night %.1f degC, season +-%.1f (offset %.1f months), humidity %.2f",
                          area, s.mapId, row.dayTemp, row.nightTemp, row.seasonAmplitude * 0.5f, row.seasonOffset, row.humidity);
            g_api->UiText(line);
            const climate::Breakdown t = climate::Temperature(row);
            const climate::Weather& w = climate::CurrentWeather();
            static const char* const kWeather[] = { "fine", "rain", "snow", "sand" };
            std::snprintf(line, sizeof(line), "temperature %.1f degC = %.1f time of day %+.1f season %+.1f weather (%s %.2f)",
                          t.total, t.daily, t.season, t.weather, kWeather[w.type >= 0 && w.type < 4 ? w.type : 0], w.intensity);
            g_api->UiText(line);
            if (g_api->UiButton("Reload AreaClimate")) climate::Load(g_cdbc);

            g_api->UiSeparator();
            const fields::MoistureDetail m = fields::Moisture(s.playerPos);
            if (!m.known) g_api->UiText("moisture here: not sampled yet");
            else
            {
                std::snprintf(line, sizeof(line), "moisture here %.2f -> settles at %.2f (material rest %.2f, absorbency %.2f)",
                              m.value, m.equilibrium, m.rest, m.absorbency);
                g_api->UiText(line);
                static const char* const kCategory[] = { "water", "ocean", "magma", "slime" };
                const int cat = terrain::LiquidCategoryOf(m.liquid);
                char liquid[96];
                std::snprintf(liquid, sizeof(liquid), "%u \"%s\" (%s), moisture %.2f%s", m.liquid, terrain::LiquidNameOf(m.liquid),
                              cat >= 0 && cat < 4 ? kCategory[cat] : "?", m.liquidMoisture, m.liquidHot ? "" : ", no heat");
                if (m.submerged) std::snprintf(line, sizeof(line), "  in liquid %s", liquid);
                else if (m.waterDistance >= 0.0f)
                    std::snprintf(line, sizeof(line), "  liquid %s: %.1f yd away, %.1f yd above its surface -> shore %.2f",
                                  liquid, m.waterDistance, m.heightAboveWater, m.shore);
                else std::snprintf(line, sizeof(line), "  no liquid within the grid");
                g_api->UiText(line);
                std::snprintf(line, sizeof(line), "  ground %.1f degC (air %.1f%s); open sky %.2f", m.temperature, m.airTemperature,
                              m.liquidHot && m.heat > 0.0f ? ", warmed by the liquid" : "", m.open);
                if (m.liquidHot && m.heat > 0.0f)
                {
                    const size_t len = std::strlen(line);
                    std::snprintf(line + len, sizeof(line) - len, "; liquid %.0f degC, heat reach %.2f", m.liquidTemperature, m.heat);
                }
                g_api->UiText(line);
                if (m.hotShare > 0.0f)
                    std::snprintf(line, sizeof(line), "  hot ground here: %.0f%% painted, %.0f degC", m.hotShare * 100.0f, m.hotTemperature);
                else if (m.groundHeatDistance >= 0.0f)
                    std::snprintf(line, sizeof(line), "  hot ground %.1f yd away (%.0f degC), heat reach %.2f", m.groundHeatDistance, m.groundHeatTemperature, m.groundHeat);
                else
                    std::snprintf(line, sizeof(line), "  no hot ground within the grid");
                g_api->UiText(line);
            }
            const fields::Stats st = fields::GetStats();
            std::snprintf(line, sizeof(line), "moisture grid: %u / %u cells sampled, %u under water, %u hot ground; rain soak so far %.2f; fill %.2f ms, shore pass %.2f ms, step %.2f ms",
                          st.filled, st.cells, st.water, st.hotGround, st.rainSoak, st.fillMs, st.transformMs, st.tickMs);
            g_api->UiText(line);

            // The regional layer: every zone seen this session.
            if (g_api->UiCollapsingHeader("Zones (regional layer)"))
            {
                const uint32_t zone = regional::PlayerZone();
                std::snprintf(line, sizeof(line), "you are in zone %u \"%s\" (AreaTable name; the client says \"%s\")",
                              zone, regional::AreaName(zone), s.zoneText);
                g_api->UiText(line);
                for (const regional::Zone& z : regional::Zones())
                {
                    const double ago = regional::Now() - z.lastSeen;
                    std::snprintf(line, sizeof(line), "%s map %d zone %u \"%s\": rain soak %.2f, %s%s",
                                  z.map == s.mapId && z.id == zone ? ">" : " ", z.map, z.id, regional::AreaName(z.id), z.rainSoak,
                                  z.source == regional::Source::Observed ? "" : "estimated",
                                  z.catchUp > 0.0f ? ", catching up" : "");
                    const size_t len = std::strlen(line);
                    if (!(z.map == s.mapId && z.id == zone) && z.source == regional::Source::Observed)
                        std::snprintf(line + len, sizeof(line) - len, "last here %.0f s ago", ago);
                    g_api->UiText(line);
                }
            }

            g_api->UiSeparator();
            g_api->UiTextWrapped(terrainwet::StatusLine());
            int terrainOn = terrainwet::Enabled() ? 1 : 0;
            if (g_api->UiCheckbox("wet terrain (darker where the ground is wetter than its rest)", &terrainOn)) terrainwet::SetEnabled(terrainOn != 0);
            static const char* const kDebugViews[] = { "debug: off", "debug: whole moisture grid (+-128 yd)", "debug: whole terrain", "debug: 10 yd stripes" };
            int debugView = terrainwet::Debug();
            if (g_api->UiCombo("wet terrain debug view", &debugView, kDebugViews, 4)) terrainwet::SetDebug(debugView);
            float strength = terrainwet::Strength();
            if (g_api->UiSliderFloat("wet terrain strength", &strength, 0.0f, 1.0f)) terrainwet::SetStrength(strength);

            g_api->UiSeparator();
            g_api->UiText("Testing (not saved):");
            static const char* const kChoices[] = { "live weather", "fine", "rain", "snow", "sand" };
            bool changed = g_api->UiCombo("weather", &g_weatherChoice, kChoices, 5) != 0;
            changed |= g_api->UiSliderFloat("intensity", &g_weatherIntensity, 0.0f, 1.0f) != 0;
            if (changed) climate::SetWeatherOverride(g_weatherChoice - 1, g_weatherIntensity);
            bool timeChanged = g_api->UiCheckbox("override time of day", &g_timeOn) != 0;
            timeChanged |= g_api->UiSliderFloat("hour", &g_timeHours, 0.0f, 24.0f) != 0;
            if (timeChanged) climate::SetTimeOverride(g_timeOn ? g_timeHours / 24.0f : -1.0f);
        }
    }

    void SetClimateCdbc(const WXL_SeyrisCdbcApi* cdbc) { g_cdbc = cdbc; }

    void RegisterClimatePanel(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }
}
