#include "FogView.hpp"
#include "Pages.hpp"

#include "FogProbe.hpp"

#include "../env/Climate.hpp"
#include "../env/Fog.hpp"
#include "../env/FogSettings.hpp"
#include "../features/GroundFog.hpp"
#include "../env/WorldQuery.hpp"

#include <cstdio>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: fog";
        const WXL_Api* g_api = nullptr;

        int   g_kind = fog::kMist;
        float g_amount = 0.8f;
        float g_radius = 40.0f;
        int   g_wholeGrid = 0;

        void __cdecl Panel(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[256];

            g_api->UiTextWrapped("Ground fog. Each kind heads for the target its rules give (x its Strength); deposited fog "
                                 "above the target burns off. The climate panel's time / temperature / weather overrides trigger real conditions.");
            g_api->UiSeparator();
            groundfog::DrawPanelSection();
            g_api->UiSeparator();

            g_api->UiText("Here / in the grid:");
            const fog::Stats st = fog::GetStats();
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                std::snprintf(line, sizeof(line), "  %-10s here %.3f -> target %.3f   max %.3f   cells %5u   progress %.2f",
                              fog::KindName(k), fog::AmountAt(k, s.playerPos[0], s.playerPos[1]),
                              fog::TargetAt(k, s.playerPos[0], s.playerPos[1]),
                              fog::Current().maxAmount[k], st.cellsWithFog[k], fog::Progress(k));
                g_api->UiText(line);
            }
            const fog::RulesInputs& in = fog::Inputs();
            std::snprintf(line, sizeof(line), "why: air %.1f degC (an hour ago %.1f, cooling %.2f), water %.1f degC, humidity %.2f, "
                                              "fogginess %.2f, wind %.2f, night %.2f, rain %.2f, snow %.2f",
                          in.airNow, in.airHourAgo, in.cooling, in.water, in.humidity, in.fogginess, in.wind, in.night, in.rain, in.snow);
            g_api->UiTextWrapped(line);

            g_api->UiSeparator();
            g_api->UiText("Deposit (testing, not saved):");
            const char* kinds[fog::kKindCount];
            for (int k = 0; k < fog::kKindCount; ++k) kinds[k] = fog::KindName(k);
            g_api->UiCombo("kind##fogdeposit", &g_kind, kinds, fog::kKindCount);
            g_api->UiSliderFloat("amount##fogdeposit", &g_amount, 0.0f, 1.0f);
            g_api->UiCheckbox("whole grid (+-128 yd)##fogdeposit", &g_wholeGrid);
            if (!g_wholeGrid) g_api->UiSliderFloat("radius (yd)##fogdeposit", &g_radius, 2.0f, 128.0f);
            if (g_api->UiButton("Deposit##fogdeposit")) fog::Deposit(g_kind, g_amount, g_wholeGrid ? 0.0f : g_radius);
            g_api->UiSameLine();
            if (g_api->UiButton("Clear all fog##fogdeposit")) fog::Clear();
            g_api->UiSameLine();
            if (g_api->UiButton("Fast-forward to the targets##fogff")) fog::FastForward();
            int paused = fog::Paused() ? 1 : 0;
            if (g_api->UiCheckbox("pause build-up and burn-off", &paused)) fog::SetPaused(paused != 0);

            g_api->UiSeparator();
            std::snprintf(line, sizeof(line), "FogSettings.cdbc: %s; AreaClimate fogginess here: %.2f",
                          fogsettings::Status(), climate::For(s.areaCount > 0 ? s.areaChain[0] : 0, s.mapId).fogginess);
            g_api->UiTextWrapped(line);
            if (g_api->UiButton("Reload FogSettings##fogreload")) fogsettings::Reload();
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                char header[128];
                std::snprintf(header, sizeof(header), "kind: %s##fogkind%d", fog::KindName(k), k);
                if (!g_api->UiCollapsingHeader(header)) continue;
                fog::KindSettings& ks = fog::Settings(k);
                char label[128];
                std::snprintf(label, sizeof(label), "enabled##fogen%d", k);
                g_api->UiCheckbox(label, &ks.enabled);
                std::snprintf(label, sizeof(label), "use the panel's values (instead of the table; not saved)##fogov%d", k);
                g_api->UiCheckbox(label, &ks.panelOverride);
                std::snprintf(line, sizeof(line), "strength %.2f, wind thin %.2f, wind drift %.2f, patchiness %.2f, weight %.2f (rules and flow: later steps)",
                              ks.strength, ks.windThin, ks.windDrift, ks.patchiness, ks.weight);
                g_api->UiTextWrapped(line);
                std::snprintf(label, sizeof(label), "strength (x the rules' target)##fogstr%d", k);
                if (g_api->UiSliderFloat(label, &ks.strength, 0.0f, 4.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "build-up half-life (s)##fogup%d", k);
                if (g_api->UiSliderFloat(label, &ks.buildUpSeconds, 1.0f, 3600.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "burn-off half-life (s)##fogdown%d", k);
                if (g_api->UiSliderFloat(label, &ks.burnOffSeconds, 1.0f, 3600.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "layer thickness (yd)##fogthick%d", k);
                if (g_api->UiSliderFloat(label, &ks.thickness, 0.5f, 50.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "density at the ground (per yd)##fogdens%d", k);
                if (g_api->UiSliderFloat(label, &ks.density, 0.0f, 0.3f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "weight (0 rises .. 1 heavy, pools, flows)##fogweight%d", k);
                if (g_api->UiSliderFloat(label, &ks.weight, 0.0f, 1.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "wind drift (how far the wind carries it)##fogdrift%d", k);
                if (g_api->UiSliderFloat(label, &ks.windDrift, 0.0f, 1.0f)) ks.panelOverride = 1;
                std::snprintf(label, sizeof(label), "wind thin (wind above which it can't form)##fogthin%d", k);
                if (g_api->UiSliderFloat(label, &ks.windThin, 0.0f, 1.5f)) ks.panelOverride = 1;
            }

            g_api->UiSeparator();
            {
                const fog::Grid& grid = fog::Current();
                unsigned counts[4] = {};
                if (grid.block) for (int n = 0; n < fog::kGridSize * fog::kGridSize; ++n) ++counts[grid.block[n] < 4 ? grid.block[n] : 0];
                std::snprintf(line, sizeof(line), "WMOs in the field: %u cells under/in structures (no fog), %u on WMO floors, %u open, %u not traced yet",
                              counts[3], counts[2], counts[1], counts[0]);
                g_api->UiTextWrapped(line);
            }
            std::snprintf(line, sizeof(line), "field: %d x %d cells of %.0f yd, %u KB; last step %.3f ms (rules %.3f ms; %u steps, 4 per second)%s",
                          fog::kGridSize, fog::kGridSize, fog::kCellSize, st.kilobytes, st.tickMs, st.rulesMs, st.ticks,
                          fog::Paused() ? ", PAUSED" : "");
            g_api->UiTextWrapped(line);

            g_api->UiSeparator();
            if (g_api->UiCollapsingHeader("Frame probe (research: passes, water depth, fog globals)##fogprobe"))
                DrawFogProbeSection();
        }
    }

    void RegisterFogPanel(const WXL_Api* api)
    {
        g_api = api;
        pages::AddPanel(kPanelTitle, &Panel);
    }
}
