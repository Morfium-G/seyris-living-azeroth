#include "Overview.hpp"

#include "DepthView.hpp"
#include "Pages.hpp"
#include "../env/ClientWeather.hpp"
#include "../env/Climate.hpp"
#include "../env/Fields.hpp"
#include "../env/Fog.hpp"
#include "../env/FogSettings.hpp"
#include "../env/Regional.hpp"
#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"
#include "../features/GroundFog.hpp"
#include "../render/PostAA.hpp"
#include "../render/SceneDepth.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        const WXL_Api* g_api = nullptr;

        const char* WeatherName(int type)
        {
            static const char* const kNames[] = { "fine", "rain", "snow", "sandstorm" };
            return type >= 0 && type < 4 ? kNames[type] : "?";
        }

        uint32_t PlayerArea(const world::Snapshot& s) { return s.areaCount > 0 ? s.areaChain[0] : 0; }

        // ------------------------------------------------------------------ the overview page
        void __cdecl DrawOverview(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[320];
            if (!s.inWorld) { g_api->UiText("Not in the world."); return; }

            // Where: names and the IDs table rows need.
            std::string chain;
            for (int i = 0; i < s.areaCount; ++i)
            {
                char part[96];
                std::snprintf(part, sizeof(part), "%s%s (%u)", i ? "  <  " : "", regional::AreaName(s.areaChain[i]), s.areaChain[i]);
                chain += part;
            }
            std::snprintf(line, sizeof(line), "You are in: %s", chain.empty() ? "(no area)" : chain.c_str());
            g_api->UiTextWrapped(line);
            std::snprintf(line, sizeof(line), "Map %d, %s; position %.1f %.1f %.1f; zone record %u",
                          s.mapId, s.outdoors ? "outdoors" : "indoors", s.playerPos[0], s.playerPos[1], s.playerPos[2],
                          regional::PlayerZone());
            g_api->UiText(line);
            g_api->UiSeparator();

            // When and the air.
            const climate::Clock& c = climate::Now();
            const climate::Row row = climate::Here(); // blended across area borders
            const climate::Breakdown t = climate::Temperature(row);
            const float hourAgo = climate::TemperatureAtTime(row, c.dayFraction - 1.0f / 24.0f).total;
            const float hourAhead = climate::TemperatureAtTime(row, c.dayFraction + 1.0f / 24.0f).total;
            const float trend = t.total - hourAgo;
            const int sky = static_cast<int>(c.dayFraction * 1440.0f + 0.5f) % 1440;
            std::snprintf(line, sizeof(line), "%04d-%02d-%02d, sky %02d:%02d%s", c.year, c.month, c.day, sky / 60, sky % 60,
                          climate::TimeOverride() >= 0.0f ? " (time override)" : "");
            g_api->UiText(line);
            std::snprintf(line, sizeof(line), "Air %.1f degC, %s %.1f degC/h (in an hour: %.1f)   humidity %.0f%%   fogginess %.2f   water %.1f degC",
                          t.total, trend < -0.2f ? "cooling" : (trend > 0.2f ? "warming" : "steady"), trend, hourAhead,
                          row.humidity * 100.0f, row.fogginess, climate::WaterTemperature(row));
            g_api->UiTextWrapped(line);
            const climate::Weather& w = climate::CurrentWeather();
            const wind::Sample ws = wind::At(s.playerPos);
            std::snprintf(line, sizeof(line), "Weather %s %.2f%s   wind %.2f from bearing %.0f (gust %.2f)   daylight %.2f",
                          WeatherName(w.type), w.intensity, climate::WeatherOverrideType() >= 0 ? " (override)" : "",
                          ws.strength, wind::BearingDegrees(), ws.gust, climate::SunNow().daylight);
            g_api->UiText(line);
            g_api->UiSeparator();

            // The ground here, and the zone's record.
            const fields::MoistureDetail m = fields::Moisture(s.playerPos);
            const fields::SnowDetail sn = fields::Snow(s.playerPos);
            if (m.known)
                std::snprintf(line, sizeof(line), "Ground: moisture %.2f (rest %.2f, %s), ground %.1f degC, fallen snow %.2f yd%s",
                              m.value, m.rest, m.value - m.rest > 0.05f ? "wet" : "at rest", m.temperature, sn.fallen,
                              m.submerged ? ", under liquid" : "");
            else
                std::snprintf(line, sizeof(line), "Ground: not known here yet");
            g_api->UiText(line);
            if (const regional::Zone* z = regional::Find(s.mapId, regional::PlayerZone()))
            {
                std::snprintf(line, sizeof(line), "Zone: rain soak %.2f, fallen snow %.2f yd, painted snow kept %.2f",
                              z->rainSoak, z->snowDepth, z->paintedKeep);
                g_api->UiText(line);
            }
            g_api->UiSeparator();

            // Fog.
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                std::snprintf(line, sizeof(line), "Fog %-10s here %.2f -> %.2f", fog::KindName(k),
                              fog::AmountAt(k, s.playerPos[0], s.playerPos[1]), fog::TargetAt(k, s.playerPos[0], s.playerPos[1]));
                g_api->UiText(line);
            }
        }

        // ------------------------------------------------------------------ quick features
        void __cdecl QuickFog(void* /*user*/)
        {
            groundfog::Settings& g = groundfog::Config();
            g_api->UiCheckbox("enable fog##qfog", &g.enabled);
            g_api->UiSameLine();
            g_api->UiCheckbox("magenta debug##qfog", &g.debugColour);
            g_api->UiSameLine();
            g_api->UiCheckbox("colour test##qfog", &g.colorTest);
            if (g.colorTest)
            {
                static const char* const kModes[] = { "stock", "hue", "replace" };
                g_api->UiCombo("colour mode##qfog", &g.colorMode, kModes, 3);
                g_api->UiColorEdit("colour##qfog", g.color);
                g_api->UiSliderFloat("saturation##qfog", &g.saturation, 0.0f, 1.5f);
                g_api->UiSliderFloat("brightness##qfog", &g.colorBrightness, 0.0f, 3.0f);
            }
            g_api->UiSliderFloat("density scale##qfog", &g.densityScale, 0.0f, 4.0f);
        }

        void __cdecl QuickFogField(void* /*user*/)
        {
            static int kind = fog::kMist;
            static float amount = 0.8f, radius = 40.0f;
            const char* kinds[fog::kKindCount];
            for (int k = 0; k < fog::kKindCount; ++k) kinds[k] = fog::KindName(k);
            g_api->UiCombo("kind##qfield", &kind, kinds, fog::kKindCount);
            g_api->UiSliderFloat("amount##qfield", &amount, 0.0f, 1.0f);
            g_api->UiSliderFloat("radius (yd)##qfield", &radius, 2.0f, 128.0f);
            if (g_api->UiButton("Deposit##qfield")) fog::Deposit(kind, amount, radius);
            g_api->UiSameLine();
            if (g_api->UiButton("Fast-forward##qfield")) fog::FastForward();
            g_api->UiSameLine();
            if (g_api->UiButton("Clear##qfield")) fog::Clear();
            g_api->UiSameLine();
            int paused = fog::Paused() ? 1 : 0;
            if (g_api->UiCheckbox("pause##qfield", &paused)) fog::SetPaused(paused != 0);
        }

        void __cdecl QuickClimate(void* /*user*/)
        {
            static int timeOn = 0, weather = 0;
            static float hours = 20.0f, intensity = 1.0f;
            bool timeChanged = g_api->UiCheckbox("time override##qclim", &timeOn) != 0;
            g_api->UiSameLine();
            timeChanged |= g_api->UiSliderFloat("hour##qclim", &hours, 0.0f, 24.0f) != 0;
            if (timeChanged) climate::SetTimeOverride(timeOn ? hours / 24.0f : -1.0f);
            float offset = climate::TemperatureOffset();
            if (g_api->UiSliderFloat("temperature offset (degC)##qclim", &offset, -30.0f, 30.0f)) climate::SetTemperatureOffset(offset);
            static const char* const kWeather[] = { "fine", "rain", "snow", "sandstorm" };
            g_api->UiCombo("weather##qclim", &weather, kWeather, 4);
            g_api->UiSliderFloat("intensity##qclim", &intensity, 0.0f, 1.0f);
            if (g_api->UiButton("Show in the client##qclim"))
                clientweather::Request(clientweather::StateFor(weather, intensity), intensity, false);
        }

        void __cdecl QuickRendering(void* /*user*/)
        {
            postaa::Settings& aa = postaa::Tunables();
            static const char* const kModes[] = { "off", "pass-through (test)", "FXAA", "show edges (debug)" };
            int mode = static_cast<int>(aa.mode);
            if (g_api->UiCombo("anti-aliasing##qrend", &mode, kModes, 4)) aa.mode = static_cast<postaa::Mode>(mode);
            g_api->UiCheckbox("depth overlay##qrend", &DepthOverlayShown());
            char line[128];
            std::snprintf(line, sizeof(line), "readable depth: %s",
                          depth::CurrentState() == depth::State::Active ? "active"
                          : depth::CurrentState() == depth::State::Copied ? "active (MSAA copy)" : "off (see the scene depth page)");
            g_api->UiText(line);
        }

        void __cdecl QuickWhere(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[160];
            const uint32_t area = PlayerArea(s);
            const uint32_t zone = s.areaCount > 1 ? s.areaChain[s.areaCount - 1] : area;
            std::snprintf(line, sizeof(line), "%s (%u), zone %s (%u), map %d", regional::AreaName(area), area,
                          regional::AreaName(zone), zone, s.mapId);
            g_api->UiText(line);
        }
    }

    void RegisterOverview(const WXL_Api* api)
    {
        g_api = api;
        pages::AddPanel("wxl-seyris-living-azeroth: overview", &DrawOverview);
        pages::AddQuick("Where am I", &QuickWhere);
        pages::AddQuick("Fog", &QuickFog);
        pages::AddQuick("Fog field (testing)", &QuickFogField);
        pages::AddQuick("Climate (testing)", &QuickClimate);
        pages::AddQuick("Rendering", &QuickRendering);
    }
}
