#include "FogView.hpp"

#include "FogProbe.hpp"

#include "../env/Fog.hpp"
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

            g_api->UiTextWrapped("Ground fog. No rules yet: fog is only there when deposited, and burns off at each kind's rate.");
            g_api->UiSeparator();
            groundfog::DrawPanelSection();
            g_api->UiSeparator();

            g_api->UiText("Here / in the grid:");
            const fog::Stats st = fog::GetStats();
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                std::snprintf(line, sizeof(line), "  %-10s here %.3f   max %.3f   cells with fog %u",
                              fog::KindName(k), fog::AmountAt(k, s.playerPos[0], s.playerPos[1]),
                              fog::Current().maxAmount[k], st.cellsWithFog[k]);
                g_api->UiText(line);
            }

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
            int paused = fog::Paused() ? 1 : 0;
            if (g_api->UiCheckbox("pause build-up and burn-off", &paused)) fog::SetPaused(paused != 0);

            g_api->UiSeparator();
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                char header[64];
                std::snprintf(header, sizeof(header), "kind: %s##fogkind%d", fog::KindName(k), k);
                if (!g_api->UiCollapsingHeader(header)) continue;
                fog::KindSettings& ks = fog::Settings(k);
                char label[64];
                std::snprintf(label, sizeof(label), "enabled##fogen%d", k);
                g_api->UiCheckbox(label, &ks.enabled);
                std::snprintf(label, sizeof(label), "build-up half-life (s)##fogup%d", k);
                g_api->UiSliderFloat(label, &ks.buildUpSeconds, 1.0f, 3600.0f);
                std::snprintf(label, sizeof(label), "burn-off half-life (s)##fogdown%d", k);
                g_api->UiSliderFloat(label, &ks.burnOffSeconds, 1.0f, 3600.0f);
                std::snprintf(label, sizeof(label), "layer thickness (yd)##fogthick%d", k);
                g_api->UiSliderFloat(label, &ks.thickness, 0.5f, 50.0f);
                std::snprintf(label, sizeof(label), "density at the ground (per yd)##fogdens%d", k);
                g_api->UiSliderFloat(label, &ks.density, 0.0f, 0.3f);
            }

            g_api->UiSeparator();
            std::snprintf(line, sizeof(line), "field: %d x %d cells of %.0f yd, %u KB; last step %.3f ms (%u steps, 4 per second)%s",
                          fog::kGridSize, fog::kGridSize, fog::kCellSize, st.kilobytes, st.tickMs, st.ticks,
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
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }
}
