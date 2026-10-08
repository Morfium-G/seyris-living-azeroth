#include "FogProbe.hpp"

#include "../features/SurfaceCover.hpp"

#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        namespace gx = wxl::game::gx;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // Core hook points used (signatures from the disassembly, XWorkbench 2026-10-08):
        //  Gx.WorldOnRender  0x4F8EA0  __thiscall(world frame), no stack args: the whole world frame.
        //  World.Render      0x77EFF0  __cdecl(2 args): the map render (sky, terrain, WMOs, magma/slime).
        //  M2.SceneDraw      0x823CB0  __thiscall(scene, int pass), ret 4.
        //  Gx.LiquidRenderPass 0x8A2240 __thiscall(bank, transform, int passType), ret 8.
        //  Weather.Render    0x78CA50  __thiscall(weather), no stack args (reached through 0x77F030).
        //  Gx.RenderStateSet 0x408BF0  __cdecl(int state, int value): the Gx state cache setter.
        //  Gx.DeviceDraw               __thiscall(device, batch, indexed): every client draw.
        using FrameFn      = void(__fastcall*)(void* self, void* edx);
        using MapRenderFn  = void(__cdecl*)(void* a, void* b);
        using SceneDrawFn  = void(__fastcall*)(void* scene, void* edx, int pass);
        using LiquidFn     = void(__fastcall*)(void* bank, void* edx, void* transform, int passType);
        using RsSetFn      = void(__cdecl*)(int state, int value);
        using DeviceDrawFn = void(__fastcall*)(void* device, void* edx, uint32_t* batch, int indexed);

        FrameFn      g_origFrame = nullptr;
        MapRenderFn  g_origMap = nullptr;
        SceneDrawFn  g_origScene = nullptr;
        LiquidFn     g_origLiquid = nullptr;
        FrameFn      g_origWeather = nullptr;
        RsSetFn      g_origRsSet = nullptr;
        DeviceDrawFn g_origDraw = nullptr;

        const WXL_Api* g_api = nullptr;

        // Gx state 15 = depth writes (the water materials set it from a device cap, 0x8A56A2..0x8A56B6;
        // coa-vfog: dispatched as D3DRS_ZWRITEENABLE at 0x6A8FC7 [their claim]).
        constexpr int kGxStateDepthWrite = 15;

        // Camera in liquid (0x780620 returns it; the world frame picks the under-water order by it).
        constexpr uintptr_t kCameraInLiquid = 0x00CD8794;

        // --- frame capture ---
        struct Entry
        {
            char     label[48];
            int      depth = 0;             // nesting level
            unsigned before = 0;            // draws since the previous entry ended (or began, if nested)
            unsigned inside = 0;            // draws inside (nested entries included)
        };
        std::vector<Entry> g_entries;
        int      g_armed = 0;
        bool     g_recording = false;
        int      g_level = 0;
        unsigned g_draws = 0, g_mark = 0;
        unsigned g_inLiquid = 0;
        std::string g_report = "(nothing captured yet)";

        // --- water depth writes ---
        int      g_forceWaterDepth = 0;
        int      g_liquidPass = -1;           // the liquid pass type being drawn, -1 outside
        unsigned g_asked[2][2] = {};          // [pass type 0/1][value 0/1]: state 15 requests seen
        unsigned g_forced = 0;

        size_t Begin(const char* label)
        {
            Entry e;
            std::snprintf(e.label, sizeof(e.label), "%s", label);
            e.depth = g_level++;
            e.before = g_draws - g_mark;
            e.inside = g_draws; // start count; turned into the count inside at End
            g_entries.push_back(e);
            g_mark = g_draws;
            return g_entries.size() - 1;
        }

        void End(size_t index)
        {
            --g_level;
            g_entries[index].inside = g_draws - g_entries[index].inside;
            g_mark = g_draws;
        }

        void BuildReport()
        {
            std::string out;
            char line[160];
            std::snprintf(line, sizeof(line), "camera in liquid: %u\r\n", g_inLiquid);
            out += line;
            for (const Entry& e : g_entries)
            {
                if (e.before) { std::snprintf(line, sizeof(line), "%*s  (between: %u draws)\r\n", e.depth * 2, "", e.before); out += line; }
                std::snprintf(line, sizeof(line), "%*s%s: %u draws\r\n", e.depth * 2, "", e.label, e.inside);
                out += line;
            }
            g_report = out;
        }

        void __fastcall hkFrame(void* self, void* edx)
        {
            if (!g_armed || g_recording) { g_origFrame(self, edx); return; }
            g_recording = true;
            g_entries.clear();
            g_level = 0; g_draws = 0; g_mark = 0;
            g_inLiquid = *reinterpret_cast<const uint32_t*>(kCameraInLiquid);
            const size_t i = Begin("world frame (0x4F8EA0)");
            g_origFrame(self, edx);
            End(i);
            g_recording = false;
            g_armed = 0;
            BuildReport();
            g_api->Log(WXL_LOG_INFO, kTag, "fog probe: frame captured:\r\n%s", g_report.c_str());
        }

        void __cdecl hkMap(void* a, void* b)
        {
            if (!g_recording) { g_origMap(a, b); return; }
            const size_t i = Begin("map render (sky, WDL, terrain, WMOs)");
            g_origMap(a, b);
            End(i);
        }

        void __fastcall hkScene(void* scene, void* edx, int pass)
        {
            if (!g_recording) { g_origScene(scene, edx, pass); return; }
            char label[48];
            std::snprintf(label, sizeof(label), "M2 scene pass %d", pass);
            const size_t i = Begin(label);
            g_origScene(scene, edx, pass);
            End(i);
        }

        // The Gx state cache: [[0xC5DF88] + 0x28F4] + state * 0x18 holds each state's value (GxRsSet
        // 0x408BF0 compares and writes it there); usable while [[0xC5DF88] + 0xF58] is non-zero.
        constexpr uintptr_t kGxDevice = 0x00C5DF88;
        constexpr size_t    kGxStateCache = 0x28F4, kGxStateStride = 0x18, kGxReady = 0xF58;

        // The pass (0x8A2240) draws bucket `passType` of the bank (16 bytes each: +4 count, +8 array of
        // instances); each instance's +0 is its material, whose vtable slot 2 ([vtable + 8]) is called.
        // Recorded here: every distinct render function seen, per pass type, with its draw count.
        struct SeenRender { uintptr_t fn = 0; int pass = 0; unsigned instances = 0; };
        SeenRender g_seenRenders[16];
        int        g_seenRenderCount = 0;

        void RecordRenders(void* bank, int passType)
        {
            __try
            {
                const auto* bucket = static_cast<const uint8_t*>(bank) + passType * 16;
                const uint32_t count = *reinterpret_cast<const uint32_t*>(bucket + 4);
                const auto* const* items = *reinterpret_cast<const uint8_t* const* const*>(bucket + 8);
                for (uint32_t i = 0; items && i < count && i < 4096; ++i)
                {
                    const auto* material = *reinterpret_cast<const uint8_t* const*>(items[i]);
                    const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(material);
                    const uintptr_t fn = vtable[2];
                    int k = 0;
                    while (k < g_seenRenderCount && !(g_seenRenders[k].fn == fn && g_seenRenders[k].pass == passType)) ++k;
                    if (k == g_seenRenderCount && k < 16) g_seenRenders[g_seenRenderCount++] = { fn, passType, 0 };
                    if (k < 16) ++g_seenRenders[k].instances;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void DrawLiquid(void* bank, void* edx, void* transform, int passType)
        {
            if (passType == 0 || passType == 1) RecordRenders(bank, passType);
            g_origLiquid(bank, edx, transform, passType);
        }

        // Depth writes on for a water draw, the way the client changes a state itself: IRsDirty
        // (0x685970, __thiscall(gxDevice, state), ret 4: queues the state for the next flush and saves
        // its old value for the current push level), then the cache write. The water materials push
        // their states and write state 15 = 0 inline before drawing (the live one, 0x8A48F0, at
        // 0x8A4AD5..0x8A4AEF, unconditionally; the procedural water inherits it), so this has to run
        // right before each draw; their RsPop at the end puts the old value back.
        using IRsDirtyFn = void(__fastcall*)(void* gxDevice, void* edx, int state);
        constexpr uintptr_t kIRsDirty = 0x00685970;

        void ForceDepthWriteForDraw()
        {
            auto* gxDev = *reinterpret_cast<uint8_t* const*>(kGxDevice);
            if (!gxDev || !*reinterpret_cast<const uint32_t*>(gxDev + kGxReady)) return;
            auto* cache = *reinterpret_cast<uint8_t* const*>(gxDev + kGxStateCache);
            if (!cache) return;
            auto* value = reinterpret_cast<uint32_t*>(cache + kGxStateDepthWrite * kGxStateStride);
            if (*value == 1) return;
            reinterpret_cast<IRsDirtyFn>(kIRsDirty)(gxDev, nullptr, kGxStateDepthWrite);
            *value = 1;
            ++g_forced;
        }

        void __fastcall hkLiquid(void* bank, void* edx, void* transform, int passType)
        {
            const int saved = g_liquidPass;
            g_liquidPass = passType;
            if (!g_recording) { DrawLiquid(bank, edx, transform, passType); g_liquidPass = saved; return; }
            char label[48];
            std::snprintf(label, sizeof(label), "liquid pass type %d", passType);
            const size_t i = Begin(label);
            DrawLiquid(bank, edx, transform, passType);
            End(i);
            g_liquidPass = saved;
        }

        void __fastcall hkWeather(void* self, void* edx)
        {
            if (!g_recording) { g_origWeather(self, edx); return; }
            const size_t i = Begin("weather");
            g_origWeather(self, edx);
            End(i);
        }

        // Diagnostics for the depth-write chain: does the state setter hook fire at all, and inside a
        // liquid pass. (Not all water materials go through it: some write the state cache inline, see
        // ForceDepthWriteForDraw; RecordRenders shows which material renders actually run.)
        unsigned g_rsCalls = 0, g_rs15[2] = {}, g_rsInLiquid = 0;

        void __cdecl hkRsSet(int state, int value)
        {
            ++g_rsCalls;
            if (state == kGxStateDepthWrite) ++g_rs15[value ? 1 : 0];
            if (g_liquidPass >= 0) ++g_rsInLiquid;
            if (state == kGxStateDepthWrite && (g_liquidPass == 0 || g_liquidPass == 1))
            {
                ++g_asked[g_liquidPass][value ? 1 : 0];
                if (g_forceWaterDepth && g_liquidPass == 1 && !value) { value = 1; ++g_forced; }
            }
            g_origRsSet(state, value);
        }

        // What the device actually had for each water draw (read right after it, when the client's
        // lazily applied states have been flushed): [ZENABLE on][ZWRITEENABLE on].
        unsigned g_deviceZ[2][2] = {};

        void __fastcall hkDraw(void* device, void* edx, uint32_t* batch, int indexed)
        {
            if (g_recording) ++g_draws;
            if (g_forceWaterDepth && g_liquidPass == 1) ForceDepthWriteForDraw();
            g_origDraw(device, edx, batch, indexed);
            if (g_liquidPass == 1)
                if (auto* dev = static_cast<IDirect3DDevice9*>(gx::RawDevice()))
                {
                    DWORD z = 0, zw = 0;
                    if (SUCCEEDED(dev->GetRenderState(D3DRS_ZENABLE, &z)) && SUCCEEDED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw)))
                        ++g_deviceZ[z ? 1 : 0][zw ? 1 : 0];
                }
        }

        std::string DepthWriteReport()
        {
            char line[1000];
            std::snprintf(line, sizeof(line),
                          "depth-write requests (Gx state 15): magma/slime pass 0: off %u, on %u;  water pass 1: off %u, on %u;  forced %u (force %s)\r\n"
                          "device after each water draw: depth test off/write off %u, off/on %u, on/off %u, on/on %u\r\n"
                          "state setter hook: %u calls, state 15 off %u / on %u anywhere, %u calls inside a liquid pass\r\n",
                          g_asked[0][0], g_asked[0][1], g_asked[1][0], g_asked[1][1], g_forced, g_forceWaterDepth ? "on" : "off",
                          g_deviceZ[0][0], g_deviceZ[0][1], g_deviceZ[1][0], g_deviceZ[1][1],
                          g_rsCalls, g_rs15[0], g_rs15[1], g_rsInLiquid);
            std::string out = line;
            out += "render functions in the queues:";
            if (!g_seenRenderCount) out += " (none yet)";
            for (int k = 0; k < g_seenRenderCount; ++k)
            {
                char item[64];
                std::snprintf(item, sizeof(item), " pass %d 0x%08X x%u;", g_seenRenders[k].pass, (unsigned)g_seenRenders[k].fn, g_seenRenders[k].instances);
                out += item;
            }
            return out + "\r\n";
        }

        // --- globals ---
        uint32_t U32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
        float    F32(uintptr_t a) { return *reinterpret_cast<const float*>(a); }

        bool CopyToClipboard(const std::string& text)
        {
            if (!OpenClipboard(nullptr)) return false;
            EmptyClipboard();
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            bool ok = false;
            if (mem)
            {
                std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_TEXT, mem) != nullptr;
                if (!ok) GlobalFree(mem);
            }
            CloseClipboard();
            return ok;
        }

        // The day-night block coa-vfog reads [their claim, checked here]: 0xD38B80..0xD38C30.
        constexpr uintptr_t kRegionStart = 0x00D38B80, kRegionEnd = 0x00D38C30;
        constexpr uintptr_t kBands = 0x00D38BD4; // 18 packed colours (core: Liquid.hpp kDnBandColors)
        constexpr int       kBandCount = 18;

        std::string GlobalsReport()
        {
            std::string out;
            char line[200];
            float vsFog[4] = {}, color[4] = {};
            const bool seen = cover::SeenSceneFog(vsFog, color);
            float start = 0.0f, end = 0.0f;
            if (seen && vsFog[0] < 0.0f)
            {
                const float range = -1.0f / vsFog[0];
                end = vsFog[1] * range;
                start = end - range;
            }
            if (seen)
                std::snprintf(line, sizeof(line), "terrain got: VS c12 = (%.6f, %.4f, %.3f) -> fog start %.1f, end %.1f yd, exponent %.3f; PS c2 = (%.3f, %.3f, %.3f) = #%02X%02X%02X\r\n",
                              vsFog[0], vsFog[1], vsFog[2], start, end, vsFog[2], color[0], color[1], color[2],
                              (int)std::lround(color[0] * 255.0f), (int)std::lround(color[1] * 255.0f), (int)std::lround(color[2] * 255.0f));
            else
                std::snprintf(line, sizeof(line), "terrain fog: not read yet (needs the surface cover drawing in the terrain pass with scene lighting on)\r\n");
            out += line;

            std::snprintf(line, sizeof(line), "camera in liquid 0xCD8794: %u   day fraction 0xD38B04: %.4f   storm weight 0xD38B88: %.3f   interior blend 0xD38B9C: %.3f   zone fog distance 0xD38C1C: %.1f   glow 0xD38C2C: %.3f\r\n",
                          U32(kCameraInLiquid), F32(0x00D38B04), F32(0x00D38B88), F32(0x00D38B9C), F32(0x00D38C1C), F32(0x00D38C2C));
            out += line;

            out += "bands at 0xD38BD4 (u32 as 0xAARRGGBB; '<= c2' = matches the terrain's fog colour):\r\n";
            for (int i = 0; i < kBandCount; ++i)
            {
                const uintptr_t a = kBands + 4 * i;
                const uint32_t v = U32(a);
                const int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
                const bool match = seen && std::abs(r - (int)std::lround(color[0] * 255.0f)) <= 2 &&
                                   std::abs(g - (int)std::lround(color[1] * 255.0f)) <= 2 &&
                                   std::abs(b - (int)std::lround(color[2] * 255.0f)) <= 2;
                std::snprintf(line, sizeof(line), "  [%2d] 0x%08X: %08X  #%02X%02X%02X%s\r\n", i, (unsigned)a, v, r, g, b, match ? "  <= c2" : "");
                out += line;
            }

            out += "raw 0xD38B80..0xD38C2C (float / hex; '<= start'/'<= end' = equals the terrain's fog distances):\r\n";
            for (uintptr_t a = kRegionStart; a < kRegionEnd; a += 4)
            {
                const float f = F32(a);
                const char* tag = "";
                if (seen && end > 0.0f && std::fabs(f - end) < 0.5f) tag = "  <= end";
                else if (seen && end > 0.0f && std::fabs(f - start) < 0.5f) tag = "  <= start";
                std::snprintf(line, sizeof(line), "  0x%08X: %12.4f  %08X%s\r\n", (unsigned)a, f, U32(a), tag);
                out += line;
            }
            return out;
        }

        void Section()
        {
            g_api->UiTextWrapped("Ground fog phase 1. Capture a frame above water and one under water; "
                                 "check the water's depth with the scene depth panel's overlay.");
            g_api->UiSeparator();

            if (g_api->UiButton("Capture next frame##fogprobe")) g_armed = 1;
            g_api->UiSameLine();
            if (g_api->UiButton("Copy all##fogprobe"))
                CopyToClipboard(g_report + "\r\n" + DepthWriteReport() + "\r\n" + GlobalsReport());
            g_api->UiTextWrapped(g_report.c_str());

            g_api->UiSeparator();
            g_api->UiCheckbox("Force depth writes during the water pass (liquid type 1)", &g_forceWaterDepth);
            g_api->UiTextWrapped(DepthWriteReport().c_str());
            if (g_api->UiButton("Reset counts##fogprobe"))
            {
                std::memset(g_asked, 0, sizeof(g_asked));
                std::memset(g_deviceZ, 0, sizeof(g_deviceZ));
                std::memset(g_rs15, 0, sizeof(g_rs15));
                g_seenRenderCount = 0;
                g_forced = 0; g_rsCalls = 0; g_rsInLiquid = 0;
            }

            g_api->UiSeparator();
            if (g_api->UiCollapsingHeader("Day-night globals (live)##fogprobe"))
                g_api->UiTextWrapped(GlobalsReport().c_str());
        }
    }

    void InstallFogProbe(const WXL_Api* api)
    {
        g_api = api;
        const int a = api->HookAttachByName("Gx.WorldOnRender", reinterpret_cast<void*>(&hkFrame), reinterpret_cast<void**>(&g_origFrame), WXL_HOOK_DEFAULT_PRIORITY);
        const int b = api->HookAttachByName("World.Render", reinterpret_cast<void*>(&hkMap), reinterpret_cast<void**>(&g_origMap), WXL_HOOK_DEFAULT_PRIORITY);
        const int c = api->HookAttachByName("M2.SceneDraw", reinterpret_cast<void*>(&hkScene), reinterpret_cast<void**>(&g_origScene), WXL_HOOK_DEFAULT_PRIORITY);
        const int d = api->HookAttachByName("Gx.LiquidRenderPass", reinterpret_cast<void*>(&hkLiquid), reinterpret_cast<void**>(&g_origLiquid), WXL_HOOK_DEFAULT_PRIORITY);
        const int e = api->HookAttachByName("Weather.Render", reinterpret_cast<void*>(&hkWeather), reinterpret_cast<void**>(&g_origWeather), WXL_HOOK_DEFAULT_PRIORITY);
        const int f = api->HookAttachByName("Gx.RenderStateSet", reinterpret_cast<void*>(&hkRsSet), reinterpret_cast<void**>(&g_origRsSet), WXL_HOOK_DEFAULT_PRIORITY);
        // In front of the default priority, so core's normal-phase features (installed after the
        // extensions' chains are live, m2native-indices among them) can still go in behind us; see
        // the profiler's draw hook.
        const int g = api->HookAttachByName("Gx.DeviceDraw", reinterpret_cast<void*>(&hkDraw), reinterpret_cast<void**>(&g_origDraw), -1000) ||
                      api->HookAttachByName("Gx.DeviceDraw", reinterpret_cast<void*>(&hkDraw), reinterpret_cast<void**>(&g_origDraw), 1000);
        api->Log((a && b && c && d && e && f && g) ? WXL_LOG_INFO : WXL_LOG_WARN, kTag,
                 "fog probe: hooks frame %d map %d scene %d liquid %d weather %d state %d draw %d", a, b, c, d, e, f, g);
    }

    void DrawFogProbeSection() { Section(); }
}
