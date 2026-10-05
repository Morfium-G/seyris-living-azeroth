#include "M2Effects.hpp"

namespace wxl_livingazeroth::m2effects
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // The M2 effect's set load: __thiscall(effect, vertexName, pixelName), ret 8. Loads both sets
        // through the device's set loader (vtable +0x110), only when M2 shaders are on ([0xD43020]).
        // Callers: the effect cache's create path (0x836600, 0x836C90) and one more (0x876D52).
        constexpr uintptr_t kEffectLoad = 0x00872D30;
        using EffectLoadFn = void(__fastcall*)(void* effect, void* edx, const char* vertexName, const char* pixelName);

        EffectLoadFn        g_origLoad = nullptr;
        std::vector<Effect> g_effects;
        const WXL_Api*      g_api = nullptr;

        void __fastcall hkEffectLoad(void* effect, void* edx, const char* vertexName, const char* pixelName)
        {
            // Recorded before the load: the shaders are created inside it.
            Effect e;
            e.object = reinterpret_cast<uintptr_t>(effect);
            e.vertexName = vertexName ? vertexName : "";
            e.pixelName = pixelName ? pixelName : "";
            bool known = false;
            for (const Effect& k : g_effects) known |= k.object == e.object;
            if (!known)
            {
                g_effects.push_back(e);
                g_api->Log(WXL_LOG_INFO, kTag, "m2 effect %u: %s + %s (0x%08X)", static_cast<unsigned>(g_effects.size()),
                           e.vertexName.c_str(), e.pixelName.c_str(), static_cast<unsigned>(e.object));
            }
            g_origLoad(effect, edx, vertexName, pixelName);
        }
    }

    bool Install(const WXL_Api* api)
    {
        g_api = api;
        const int ok = api->HookAttach("LivingAzeroth.M2EffectLoad", kEffectLoad, reinterpret_cast<void*>(&hkEffectLoad),
                                       reinterpret_cast<void**>(&g_origLoad), WXL_HOOK_DEFAULT_PRIORITY);
        if (!ok) api->Log(WXL_LOG_WARN, kTag, "m2 effects: the effect load hook failed to install.");
        return ok != 0;
    }

    const std::vector<Effect>& Effects() { return g_effects; }

    bool IsVertexWrapper(const void* wrapper)
    {
        if (!wrapper) return false;
        for (const Effect& e : g_effects)
        {
            const auto* table = reinterpret_cast<void* const*>(e.object + kVertexTable);
            for (int i = 0; i < kVertexEntries; ++i)
                if (table[i] == wrapper) return true;
        }
        return false;
    }
}
