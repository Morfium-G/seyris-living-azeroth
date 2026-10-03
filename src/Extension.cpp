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

#include "Config.hpp"

#include "debug/ClimateView.hpp"
#include "debug/DepthView.hpp"
#include "debug/SurfaceView.hpp"
#include "debug/WindView.hpp"
#include "env/Actors.hpp"
#include "env/Climate.hpp"
#include "env/Fields.hpp"
#include "env/Wind.hpp"
#include "env/WorldQuery.hpp"
#include "features/GrassDensity.hpp"
#include "features/GrassDistanceCap.hpp"
#include "features/GrassDoodads.hpp"
#include "features/GrassInstanced.hpp"
#include "features/GrassMotion.hpp"
#include "features/GrassPerf.hpp"
#include "features/SurfaceCover.hpp"
#include "features/TerrainWetness.hpp"
#include "render/PostAA.hpp"
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
        wxl_livingazeroth::grassdoodads::LoadOverrides(cdbc);
        wxl_livingazeroth::grassdensity::Load(cdbc);
        wxl_livingazeroth::cover::LoadTable(cdbc);
        wxl_livingazeroth::climate::Load(cdbc);
        wxl_livingazeroth::debug::SetClimateCdbc(cdbc);

        wxl_livingazeroth::config::Apply(g_api);
        wxl_livingazeroth::grassdistance::OnFirstFrame();
    }

    // Between frames: the safe moment to swap the depth surface the next world pass will bind.
    void __cdecl OnFrame(void* /*user*/, const void* /*args*/)
    {
        ResolveInterfacesOnce();
        wxl_livingazeroth::grassperf::OnFrameEnd();
        wxl_livingazeroth::grassinst::OnFrameEnd();
        wxl_livingazeroth::depth::Update();
    }

    // Once per frame, main thread: refresh the world snapshot, then everything that reads it.
    void __cdecl OnUpdate(void* /*user*/, const void* args)
    {
        const auto* a = static_cast<const ev::UpdateArgs*>(args);
        const float dt = a ? a->dt : 0.0f;
        const auto& snap = wxl_livingazeroth::world::Refresh();
        wxl_livingazeroth::climate::Update(snap);
        wxl_livingazeroth::wind::Update(dt, snap);
        if (snap.inWorld)
            wxl_livingazeroth::actors::Refresh(snap.playerPos, wxl_livingazeroth::grass::kActorRange);
        wxl_livingazeroth::fields::Update(dt, snap);
        wxl_livingazeroth::cover::Update(dt, snap);
    }

    void __cdecl OnDeviceLost(void* /*user*/, const void* /*args*/)
    {
        wxl_livingazeroth::depth::OnDeviceLost();
        wxl_livingazeroth::grassinst::OnDeviceLost();
        wxl_livingazeroth::postaa::OnDeviceLost();
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

    // Raises the grass distance cap right before the world CVars (and Config.wtf's value) are
    // validated.
    wxl_livingazeroth::grassdistance::Install(api);

    // Features register their shader patches first; the single create hook goes in after.
    wxl_livingazeroth::grass::Install(api);
    wxl_livingazeroth::terrainwet::Register();
    wxl_livingazeroth::shaderpatch::Install(api);

    // Surface cover spike: draws at the end of the world scene (before anti-aliasing's pass).
    wxl_livingazeroth::cover::Install(api);

    // Anti-aliasing subscribes before the debug overlays, so it runs first and they stay sharp on top.
    wxl_livingazeroth::postaa::Install(api);
    wxl_livingazeroth::postaa::RegisterPanel(api);
    wxl_livingazeroth::debug::RegisterPanel(api);
    wxl_livingazeroth::debug::RegisterWindPanel(api);
    wxl_livingazeroth::debug::RegisterSurfacePanel(api);
    wxl_livingazeroth::debug::RegisterClimatePanel(api);

    api->Log(WXL_LOG_INFO, "wxl-seyris-living-azeroth", "v1.1 loaded (WXL_Load reached).");
    return 1;
}
