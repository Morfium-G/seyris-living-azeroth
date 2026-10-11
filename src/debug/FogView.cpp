#include "FogView.hpp"
#include "Pages.hpp"

#include "FogProbe.hpp"

#include "../env/Climate.hpp"
#include "../env/Fog.hpp"
#include "../env/FogSettings.hpp"
#include "../env/Regional.hpp"
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

        void DrawKind(int k);
        void DrawExplanation(int k);

        void __cdecl Panel(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[256];

            // Status and the main controls first; everything else folds away.
            groundfog::DrawStatusLine();
            groundfog::DrawMainControls();
            g_api->UiSeparator();

            // Where you stand, one short line per kind (amount -> where it's heading).
            for (int k = 0; k < fog::kKindCount; ++k)
            {
                std::snprintf(line, sizeof(line), "%-10s %.2f -> %.2f%s", fog::KindName(k),
                              fog::AmountAt(k, s.playerPos[0], s.playerPos[1]), fog::TargetAt(k, s.playerPos[0], s.playerPos[1]),
                              fog::Settings(k).enabled ? "" : "   (off)");
                g_api->UiText(line);
            }
            g_api->UiSeparator();

            if (g_api->UiCollapsingHeader("Kinds (their settings; FogSettings.cdbc)##fogkinds"))
            {
                std::snprintf(line, sizeof(line), "FogSettings.cdbc: %s", fogsettings::Status());
                g_api->UiTextWrapped(line);
                if (g_api->UiButton("Reload FogSettings##fogreload")) fogsettings::Reload();
                g_api->UiSliderFloat("pool speed (how fast heavy fog runs downhill; 0 = stays put)##fogpoolspeed", &fog::Field().poolSpeed, 0.0f, 2.0f);
                g_api->UiSliderFloat("steep from (degrees: more of the fog runs off)##fogsteep0", &fog::Field().steepStart, 0.0f, 89.0f);
                g_api->UiSliderFloat("steep fully (degrees: all of it runs off)##fogsteep1", &fog::Field().steepFull, 0.0f, 89.0f);
                for (int k = 0; k < fog::kKindCount; ++k) DrawKind(k);
            }
            if (g_api->UiCollapsingHeader("Testing: deposit, clear, fast-forward, pause##fogtesting"))
            {
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
                if (g_api->UiCheckbox("pause (no build-up, burn-off, wind or flow)", &paused)) fog::SetPaused(paused != 0);
            }
            groundfog::DrawPanelSection();
            if (g_api->UiCollapsingHeader("Zones (each zone's typical fog: the memory)##fogzones"))
            {
                fog::ZoneState zones[32];
                const unsigned n = fog::ZoneStates(zones, 32);
                g_api->UiText("zone                      mist         area fog     haze");
                for (unsigned z = 0; z < n; ++z)
                {
                    const fog::ZoneState& s = zones[z];
                    const char* name = regional::AreaName(s.zone);
                    std::snprintf(line, sizeof(line), "%-24.24s  %.2f->%.2f    %.2f->%.2f    %.2f->%.2f%s%s",
                                  name[0] ? name : "?", s.amount[fog::kMist], s.target[fog::kMist], s.amount[fog::kAreaFog], s.target[fog::kAreaFog],
                                  s.amount[fog::kHaze], s.target[fog::kHaze], s.zone == regional::PlayerZone() ? "   (you)" : "",
                                  s.inRange ? "" : "   (out of range)");
                    g_api->UiText(line);
                }
                if (!n) g_api->UiText("(none yet)");
            }
            if (g_api->UiCollapsingHeader("Why (where you stand)##fogwhy"))
            {
                const fog::RulesInputs& in = fog::Inputs();
                std::snprintf(line, sizeof(line), "air %.1f degC (an hour ago %.1f, cooling %.2f)", in.airNow, in.airHourAgo, in.cooling);
                g_api->UiText(line);
                std::snprintf(line, sizeof(line), "water %.1f degC, humidity %.2f, fogginess %.2f (AreaClimate)", in.water, in.humidity, in.fogginess);
                g_api->UiText(line);
                std::snprintf(line, sizeof(line), "wind %.2f, night %.2f, rain %.2f, snow %.2f", in.wind, in.night, in.rain, in.snow);
                g_api->UiText(line);
                for (int k = 0; k < fog::kKindCount; ++k) DrawExplanation(k);
            }
            if (g_api->UiCollapsingHeader("Diagnostics##fogdiag"))
            {
                const fog::Stats st = fog::GetStats();
                for (int k = 0; k < fog::kKindCount; ++k)
                {
                    std::snprintf(line, sizeof(line), "%-10s max %.3f, %5u cells, progress %.2f", fog::KindName(k),
                                  fog::Current().maxAmount[k], st.cellsWithFog[k], fog::Progress(k));
                    g_api->UiText(line);
                }
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
                groundfog::DrawDiagnostics();
                if (g_api->UiCollapsingHeader("Frame probe (research: passes, water depth, fog globals)##fogprobe"))
                    DrawFogProbeSection();
            }
        }

        /// One kind's breakdown where the player stands: the rule's factors (they multiply), the target,
        /// and what moved the amount over the last step.
        void DrawExplanation(int k)
        {
            const fog::Explanation& e = fog::Explain(k);
            char line[256];
            // "###": the section's ID is only what follows, so it stays open while the numbers change.
            std::snprintf(line, sizeof(line), "%s: %.2f -> %.2f###fogwhy%d", fog::KindName(k), e.amount, e.target, k);
            if (!g_api->UiCollapsingHeader(line)) return;
            if (!e.valid) { g_api->UiText("  (not evaluated yet)"); return; }
            bool alternatives = false;
            for (int f = 0; f < e.count; ++f)
            {
                const fog::Factor& x = e.factors[f];
                if (x.alternative && !alternatives)
                {
                    g_api->UiText("  the largest of:");
                    alternatives = true;
                }
                else if (!x.alternative && alternatives && x.name[0] != ' ')
                    alternatives = false;
                const char* indent = x.alternative ? "    " : "  ";
                if (x.factor < 0.0f)
                    std::snprintf(line, sizeof(line), "%s%-40s %7.2f", indent, x.name, x.input);
                else
                    std::snprintf(line, sizeof(line), "%s%-40s %7.2f   x%.2f", indent, x.name, x.input, x.factor);
                g_api->UiText(line);
            }
            std::snprintf(line, sizeof(line), "  = target %.2f (clamped 0..1)", e.target);
            g_api->UiText(line);
            std::snprintf(line, sizeof(line), "  amount %.3f (light share x%.2f stays here; ground %.0f deg); per second: toward the target %+.4f, wind %+.4f",
                          e.amount, 1.0f - e.heavyShare, e.slopeDegrees, e.rules, e.wind);
            g_api->UiTextWrapped(line);
            if (e.heavyShare > 0.0f)
            {
                std::snprintf(line, sizeof(line), "  heavy pool %.3f (forms toward %.3f = heavy share x amount); per second: forming/burning off %+.4f, wind %+.4f, flow downhill %+.4f",
                              e.pool, e.poolTarget, e.poolRules, e.poolWind, e.poolFlow);
                g_api->UiTextWrapped(line);
            }
        }

        /// One kind's settings, as a collapsed section (moving a slider switches the kind to the panel's values).
        void DrawKind(int k)
        {
            {
                char header[128];
                std::snprintf(header, sizeof(header), "%s##fogkind%d", fog::KindName(k), k);
                if (!g_api->UiCollapsingHeader(header)) return;
                fog::KindSettings& ks = fog::Settings(k);
                char label[128];
                std::snprintf(label, sizeof(label), "enabled##fogen%d", k);
                g_api->UiCheckbox(label, &ks.enabled);
                std::snprintf(label, sizeof(label), "use the panel's values (instead of the table; not saved)##fogov%d", k);
                g_api->UiCheckbox(label, &ks.panelOverride);
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
        }
    }

    void RegisterFogPanel(const WXL_Api* api)
    {
        g_api = api;
        pages::AddPanel(kPanelTitle, &Panel);
    }
}
