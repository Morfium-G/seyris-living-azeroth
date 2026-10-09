#include "Pages.hpp"

#include "../wxl_seyris/ControlsApi.hpp"

#include <cstring>
#include <vector>

namespace wxl_livingazeroth::pages
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";
        constexpr const char* kPrefix = "wxl-seyris-living-azeroth: ";

        struct Item { const char* title; WXL_PanelFn draw; };
        std::vector<Item> g_panels, g_quick;
        bool g_published = false;
    }

    void AddPanel(const char* title, WXL_PanelFn draw) { g_panels.push_back({ title, draw }); }
    void AddQuick(const char* name, WXL_PanelFn draw) { g_quick.push_back({ name, draw }); }

    void Publish(const WXL_Api* api)
    {
        if (g_published) return;
        g_published = true;
        const auto* controls = static_cast<const WXL_SeyrisControlsApi*>(
            api->GetInterface(WXL_SEYRIS_CONTROLS_INTERFACE_NAME, WXL_SEYRIS_CONTROLS_INTERFACE_VERSION));
        const bool pages = controls && controls->HasFeature("pages") && controls->AddPage;
        for (const Item& p : g_panels)
        {
            const size_t prefix = std::strncmp(p.title, kPrefix, std::strlen(kPrefix)) == 0 ? std::strlen(kPrefix) : 0;
            if (!pages || !controls->AddPage(kGroup, p.title + prefix, p.draw, nullptr))
                api->UiAddPanel(p.title, p.draw, nullptr);
        }
        if (controls && controls->HasFeature("quick-features") && controls->AddQuickFeature)
            for (const Item& q : g_quick) controls->AddQuickFeature(kGroup, q.title, q.draw, nullptr);
        api->Log(WXL_LOG_INFO, kTag, "ui: %zu panel(s) %s, %zu quick feature(s)%s.", g_panels.size(),
                 pages ? "as pages of the control panel" : "as overlay panels (wxl-seyris-tools' control panel not found)",
                 g_quick.size(), pages ? "" : " not shown");
    }
}
