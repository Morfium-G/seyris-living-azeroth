#include "DoodadFade.hpp"

#include <cstdint>
#include <cstdio>

namespace wxl_livingazeroth::doodadfade
{
    namespace
    {
        // The client's view-distance table (traced 2026-10-12, memory wxl-objectfade-table-lead). Five
        // size categories each. Base rows, never written by the client after load:
        constexpr uintptr_t kBaseDistance  = 0x00ADF364; // 30 / 100 / 200 / 750 / 1250 yd
        constexpr uintptr_t kBaseFadeWidth = 0x00ADF38C; // 5 / 10 / 15 / 20 / 50 yd
        // Runtime rows (rebuilt by 0x78F570 from the base rows and environmentDetail; read by the fade):
        constexpr uintptr_t kDistance        = 0x00ADF3A0;
        constexpr uintptr_t kDistanceSq      = 0x00ADF3B4;
        constexpr uintptr_t kFadeStart       = 0x00ADF3C8;
        constexpr uintptr_t kFadeStartSq     = 0x00ADF3DC;
        constexpr int       kCategories = 5;

        Settings g_settings;
        float    g_scale = 1.0f;

        volatile float* Row(uintptr_t at) { return reinterpret_cast<volatile float*>(at); }
    }

    Settings& Config() { return g_settings; }

    void Update()
    {
        volatile float* base = Row(kBaseDistance);
        volatile float* width = Row(kBaseFadeWidth);
        volatile float* dist = Row(kDistance);
        volatile float* distSq = Row(kDistanceSq);
        volatile float* fade = Row(kFadeStart);
        volatile float* fadeSq = Row(kFadeStartSq);

        // environmentDetail's scale, from category 1 (never changed here): runtime / base.
        if (base[1] > 0.0f && dist[1] > 0.0f) g_scale = dist[1] / base[1];

        for (int k = 0; k < kCategories; ++k)
        {
            // Stock, as 0x78F570 builds it: category 0 unscaled, the fade width never scaled.
            float d = k == 0 ? base[0] : base[k] * g_scale;
            float w = width[k];
            if (k == 0 && g_settings.smallUseNext) { d = base[1] * g_scale; w = width[1]; }
            float start = d - w;
            if (k < g_settings.fadeFrom) start = d; // no fade: full until the view distance, then gone
            if (dist[k] != d || distSq[k] != d * d || fade[k] != start || fadeSq[k] != start * start)
            {
                dist[k] = d;
                distSq[k] = d * d;
                fade[k] = start;
                fadeSq[k] = start * start;
            }
        }
    }

    void DrawPanelSection(const WXL_Api* api)
    {
        if (!api->UiCollapsingHeader("Doodads fading in (they show unfogged while they fade)##ddfade")) return;
        api->UiCheckbox("small doodads (mugs, plates: fixed 30 yd) use the next size's view distance##ddsmall", &g_settings.smallUseNext);
        api->UiSliderInt("fade only from size category (0 stock, 5 none: doodads pop in, fogged)##ddfrom", &g_settings.fadeFrom, 0, 5);
        char line[256];
        const volatile float* dist = Row(kDistance);
        const volatile float* fade = Row(kFadeStart);
        std::snprintf(line, sizeof(line), "view distance / fade start (yd), radius <1, <4, <15, <100, larger: %.0f/%.0f  %.0f/%.0f  %.0f/%.0f  %.0f/%.0f  %.0f/%.0f "
                                          "(environmentDetail scale %.2f)",
                      dist[0], fade[0], dist[1], fade[1], dist[2], fade[2], dist[3], fade[3], dist[4], fade[4], g_scale);
        api->UiTextWrapped(line);
    }
}
