#include "WmoBindProbe.hpp"

#include "../render/M2Effects.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // Core hook points (offsets/game/WMO.hpp, offsets/engine/Shader.hpp):
        //  Wmo.IntRender / Wmo.ExtRender: the batch loops, __thiscall(root, group, int flag), ret 8;
        //  Shader.EffectBind: __cdecl(vtxIdx, pixIdx), binds a permutation of the active collection.
        // The active collection is the effect object M2Effects records ([0xD43024]); the group's MOGP
        // flags sit at +0x30 (indoor 0x2000, exterior 0x8).
        using RenderLeafFn = void(__fastcall*)(void* root, void* edx, void* group, int flag);
        using EffectBindFn = void(__cdecl*)(uint32_t vtxIdx, uint32_t pixIdx);
        constexpr uintptr_t kActiveCollection = 0x00D43024;
        constexpr size_t    kOffGroupFlags = 0x30;

        RenderLeafFn g_origInt = nullptr, g_origExt = nullptr;
        EffectBindFn g_origBind = nullptr;
        int          g_enabled = 0;
        int          g_loop = 0;          // 0 none, 1 interior loop, 2 exterior loop
        uint32_t     g_groupFlags = 0;

        using Key = std::tuple<int, int, uintptr_t, uint32_t, uint32_t>; // loop, indoor, effect, vtx, pix
        std::map<Key, unsigned> g_seen;

        uint32_t GroupFlags(void* group)
        {
            uint32_t f = 0;
            __try { if (group) f = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(group) + kOffGroupFlags); }
            __except (EXCEPTION_EXECUTE_HANDLER) { f = 0; }
            return f;
        }

        void __fastcall hkInt(void* root, void* edx, void* group, int flag)
        {
            if (!g_enabled) { g_origInt(root, edx, group, flag); return; }
            const int savedLoop = g_loop; const uint32_t savedFlags = g_groupFlags;
            g_loop = 1; g_groupFlags = GroupFlags(group);
            g_origInt(root, edx, group, flag);
            g_loop = savedLoop; g_groupFlags = savedFlags;
        }

        void __fastcall hkExt(void* root, void* edx, void* group, int flag)
        {
            if (!g_enabled) { g_origExt(root, edx, group, flag); return; }
            const int savedLoop = g_loop; const uint32_t savedFlags = g_groupFlags;
            g_loop = 2; g_groupFlags = GroupFlags(group);
            g_origExt(root, edx, group, flag);
            g_loop = savedLoop; g_groupFlags = savedFlags;
        }

        void __cdecl hkBind(uint32_t vtxIdx, uint32_t pixIdx)
        {
            g_origBind(vtxIdx, pixIdx);
            if (!g_enabled || !g_loop) return;
            const uintptr_t effect = *reinterpret_cast<const uintptr_t*>(kActiveCollection);
            ++g_seen[Key{ g_loop, (g_groupFlags & 0x2000) ? 1 : 0, effect, vtxIdx, pixIdx }];
        }
    }

    void InstallWmoBindProbe(const WXL_Api* api)
    {
        const int a = api->HookAttachByName("Wmo.IntRender", reinterpret_cast<void*>(&hkInt), reinterpret_cast<void**>(&g_origInt), WXL_HOOK_DEFAULT_PRIORITY);
        const int b = api->HookAttachByName("Wmo.ExtRender", reinterpret_cast<void*>(&hkExt), reinterpret_cast<void**>(&g_origExt), WXL_HOOK_DEFAULT_PRIORITY);
        const int c = api->HookAttachByName("Shader.EffectBind", reinterpret_cast<void*>(&hkBind), reinterpret_cast<void**>(&g_origBind), WXL_HOOK_DEFAULT_PRIORITY);
        if (!a || !b || !c) api->Log(WXL_LOG_WARN, kTag, "wmo bind probe: hooks int %d ext %d bind %d", a, b, c);
    }

    int& WmoBindProbeEnabled() { return g_enabled; }
    void ClearWmoBindProbe() { g_seen.clear(); }

    std::string WmoBindProbeReport()
    {
        std::string out;
        char line[200];
        for (const auto& [k, n] : g_seen)
        {
            const auto [loop, indoor, effect, vtx, pix] = k;
            const char* name = "?";
            for (const m2effects::Effect& e : m2effects::Effects())
                if (e.object == effect) { name = e.vertexName.c_str(); break; }
            std::string pixel = "?";
            for (const m2effects::Effect& e : m2effects::Effects())
                if (e.object == effect) { pixel = e.pixelName; break; }
            std::snprintf(line, sizeof(line), "%s loop, %s group: %s + %s  vertex %u  pixel %u  (%u binds)\r\n",
                          loop == 1 ? "interior" : "exterior", indoor ? "indoor" : "outdoor", name, pixel.c_str(), vtx, pix, n);
            out += line;
        }
        return out.empty() ? std::string("(nothing recorded: enable, then look at WMOs)") : out;
    }
}
