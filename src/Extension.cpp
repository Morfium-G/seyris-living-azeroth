// wxl-seyris-living-azeroth -- client-side, purely cosmetic features that make the world feel more
// alive: weather and wind, surfaces, water, fog, clouds, lighting, characters reacting to the
// world. Standalone WXL v1.1 extension, sibling to wxl-seyris-tools/wxl-seyris-dehardcoding.
//
// See orchestration/docs/r&d/immersion/ for the planning (README.md is the index).
//
// This file only owns the WXL_Query/WXL_Load entry points and the event wiring. Hooks must be
// attached from WXL_Load (core arms the whole detour batch once, right after every extension's
// WXL_Load returns).

#include "wxl/PluginApi.h"

#include "engine/events/Event.hpp"

#include "debug/DepthView.hpp"
#include "debug/WindView.hpp"
#include "env/Wind.hpp"
#include "env/WorldQuery.hpp"
#include "features/GrassMotion.hpp"
#include "render/SceneDepth.hpp"
#include "render/ShaderPatch.hpp"
#include "wxl_seyris/CdbcApi.hpp"

namespace
{
    namespace ev = wxl::events;

    const WXL_PluginInfo kInfo = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-seyris-living-azeroth",
        1, // pluginVersion: opaque to the core, bump manually as this module grows
        WXL_CLIENT_BUILD,
    };

    const WXL_Api* g_api = nullptr;

    // Resolved on the first frame, never in WXL_Load: extensions load alphabetically on one thread,
    // so GetInterface from WXL_Load can see a false NULL for a module that loads later.
    void ResolveInterfacesOnce()
    {
        static bool done = false;
        if (done) return;
        done = true;

        const auto* cdbc = static_cast<const WXL_SeyrisCdbcApi*>(
            g_api->GetInterface(WXL_SEYRIS_CDBC_INTERFACE_NAME, WXL_SEYRIS_CDBC_INTERFACE_VERSION));
        wxl_livingazeroth::debug::SetWindCdbc(cdbc);
        wxl_livingazeroth::wind::LoadProfiles(cdbc);
    }

    // Between frames: the safe moment to swap the depth surface the next world pass will bind.
    void __cdecl OnFrame(void* /*user*/, const void* /*args*/)
    {
        ResolveInterfacesOnce();
        wxl_livingazeroth::depth::Update();
    }

    // Once per frame, main thread: refresh the world snapshot, then everything that reads it.
    void __cdecl OnUpdate(void* /*user*/, const void* args)
    {
        const auto* a = static_cast<const ev::UpdateArgs*>(args);
        const auto& snap = wxl_livingazeroth::world::Refresh();
        wxl_livingazeroth::wind::Update(a ? a->dt : 0.0f, snap);
    }

    void __cdecl OnDeviceLost(void* /*user*/, const void* /*args*/)
    {
        wxl_livingazeroth::depth::OnDeviceLost();
    }
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    return &kInfo;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api) return 0;
    g_api = api;

    wxl_livingazeroth::depth::Init(api);
    api->Subscribe(static_cast<uint32_t>(ev::Event::OnFrame), &OnFrame, nullptr);
    api->Subscribe(static_cast<uint32_t>(ev::Event::OnDeviceLost), &OnDeviceLost, nullptr);

    wxl_livingazeroth::wind::Init(api);
    api->Subscribe(static_cast<uint32_t>(ev::Event::OnUpdate), &OnUpdate, nullptr);

    // Features register their shader patches first; the single create hook goes in after.
    wxl_livingazeroth::grass::Install(api);
    wxl_livingazeroth::shaderpatch::Install(api);

    wxl_livingazeroth::debug::RegisterPanel(api);
    wxl_livingazeroth::debug::RegisterWindPanel(api);

    api->Log(WXL_LOG_INFO, "wxl-seyris-living-azeroth", "v1.1 loaded (WXL_Load reached).");
    return 1;
}
