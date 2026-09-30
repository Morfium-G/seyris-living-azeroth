#include "GrassDistanceCap.hpp"

#include "../Config.hpp"

#include "offsets/game/GroundEffect.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace wxl_livingazeroth::grassdistance
{
    namespace
    {
        namespace ge = wxl::offsets::game::groundeffect;
        namespace wo = wxl::offsets::game::world;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // Grass only exists on loaded terrain, so in practice the far clip limits it long before
        // this; the cap just stops the exe refusing a deliberate setting (options UI edit or
        // console).
        constexpr float kDefaultCap = 10000.0f;

        // Handle of the registered groundEffectDist CVar. The world CVar registration stores each
        // result while pushing the next one's arguments (0x78E7CD); non-null = its saved value was
        // already validated.
        constexpr uintptr_t kGroundEffectDistCVar = 0x00CD85C0;

        using ParamInitializeFn = void(__cdecl*)();

        const WXL_Api*    g_api = nullptr;
        ParamInitializeFn g_origParamInit = nullptr;
        bool              g_applied = false;

        float ReadFloat(uintptr_t address)
        {
            float f;
            std::memcpy(&f, reinterpret_cast<const void*>(address), sizeof(f));
            return f;
        }

        void Raise(bool late)
        {
            g_applied = true;
            const float wanted = config::GetFloat(g_api, "GrassDistanceCap",
                "Highest grass distance (groundEffectDist) the client accepts. The exe's own cap is 140; it's only raised, never lowered.",
                kDefaultCap);
            const float current = ReadFloat(ge::kDistCapFloat);
            if (current >= wanted)
            {
                g_api->Log(WXL_LOG_INFO, kTag, "grass distance cap: already %.0f (exe or another module raised it; configured %.0f), left as is.",
                           current, wanted);
                return;
            }

            void* at = reinterpret_cast<void*>(ge::kDistCapFloat);
            DWORD old = 0;
            if (!VirtualProtect(at, sizeof(float), PAGE_READWRITE, &old))
            {
                g_api->Log(WXL_LOG_WARN, kTag, "grass distance cap: couldn't unprotect the cap (error %lu); it stays %.0f.", GetLastError(), current);
                return;
            }
            std::memcpy(at, &wanted, sizeof(float));
            VirtualProtect(at, sizeof(float), old, &old);

            uint32_t registered = 0;
            std::memcpy(&registered, reinterpret_cast<const void*>(kGroundEffectDistCVar), sizeof(registered));
            if (late && registered)
                g_api->Log(WXL_LOG_WARN, kTag, "grass distance cap: raised %.0f -> %.0f, but only after groundEffectDist was validated this session; "
                           "a saved value above %.0f was reset -- set it again (options or console).", current, wanted, current);
            else
                g_api->Log(WXL_LOG_INFO, kTag, "grass distance cap: raised %.0f -> %.0f.", current, wanted);
        }

        void __cdecl hkParamInitialize()
        {
            if (!g_applied) Raise(false);
            g_origParamInit();
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        const int ok = api->HookAttach("LivingAzeroth.WorldParamInitialize", wo::kParamInitialize,
                                       reinterpret_cast<void*>(&hkParamInitialize),
                                       reinterpret_cast<void**>(&g_origParamInit), WXL_HOOK_DEFAULT_PRIORITY);
        if (!ok)
            api->Log(WXL_LOG_WARN, kTag, "grass distance cap: registration hook failed; the cap is raised on the first frame instead.");
    }

    void OnFirstFrame()
    {
        if (!g_applied) Raise(true);
    }

    float CurrentCap() { return ReadFloat(ge::kDistCapFloat); }
}
