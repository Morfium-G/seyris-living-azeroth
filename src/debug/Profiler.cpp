#include "Profiler.hpp"

#include "engine/events/Event.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::prof
{
    namespace
    {
        namespace ev = wxl::events;
        namespace gx = wxl::game::gx;

        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: frame profiler";
        const char* const kNames[kCount] = {
            "other (game logic, unlisted)", "world: rest (sky, misc.)", "terrain", "grass (detail doodads)",
            "M2 models", "M2 animation (main thread)", "particles", "ribbons", "WMOs", "liquids",
            "shadow maps", "weather", "UI + overlay",
            "ours: climate / wind / fields", "ours: lights (scan, grid, baking)", "ours: light textures",
            "ours: surface cover update", "ours: surface cover draw", "ours: anti-aliasing",
        };

        const WXL_Api* g_api = nullptr;
        int   g_enabled = 0;     // panel
        int   g_gpu = 1;         // panel: timestamp queries
        bool  g_frameOn = false; // this frame is being measured (fixed at the frame's start)
        DWORD g_mainThread = 0;

        // --- category stack and accounting ----------------------------------------------------------
        constexpr int kMaxDepth = 256;
        int     g_stack[kMaxDepth];
        int     g_depth = 0;
        int64_t g_last = 0;          // QPC at the last transition
        int64_t g_qpcFreq = 1;

        struct FrameAccum
        {
            int64_t  cpu[kCount] = {};
            uint32_t draws[kCount] = {};
            uint64_t tris[kCount] = {};
        } g_frame;

        int Current() { return g_depth > 0 ? g_stack[g_depth - 1] : kOther; }

        int64_t Now()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return t.QuadPart;
        }

        bool OnMainThread() { return GetCurrentThreadId() == g_mainThread; }

        // --- GPU timestamps ---------------------------------------------------------------------
        // A ring of frames; each holds its timestamps in issue order and, per timestamp, the category
        // whose interval it closes (-1 = the frame's start).
        constexpr int      kGpuFrames = 4;
        constexpr unsigned kMaxStamps = 4096;
        struct GpuFrame
        {
            std::vector<IDirect3DQuery9*> stamps;
            std::vector<int> cats;
            unsigned used = 0;
            IDirect3DQuery9* disjoint = nullptr;
            IDirect3DQuery9* freq = nullptr;
            bool pending = false;
            bool overflow = false;
        };
        GpuFrame g_gpuFrames[kGpuFrames];
        int  g_gpuCur = 0;
        IDirect3DDevice9* g_gpuDevice = nullptr;
        bool g_gpuSupported = true;
        bool g_gpuFrameOn = false;   // stamps are being issued this frame
        unsigned g_gpuNotReady = 0, g_gpuDisjoint = 0;

        void ReleaseGpu()
        {
            for (GpuFrame& f : g_gpuFrames)
            {
                for (IDirect3DQuery9* q : f.stamps) if (q) q->Release();
                f.stamps.clear(); f.cats.clear(); f.used = 0;
                if (f.disjoint) f.disjoint->Release();
                if (f.freq) f.freq->Release();
                f.disjoint = f.freq = nullptr;
                f.pending = f.overflow = false;
            }
            g_gpuDevice = nullptr;
            g_gpuFrameOn = false;
        }

        void Stamp(int closes)
        {
            if (!g_gpuFrameOn) return;
            GpuFrame& f = g_gpuFrames[g_gpuCur];
            if (f.used >= kMaxStamps) { f.overflow = true; return; }
            if (f.used == f.stamps.size())
            {
                IDirect3DQuery9* q = nullptr;
                if (FAILED(g_gpuDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)) || !q) { f.overflow = true; return; }
                f.stamps.push_back(q);
                f.cats.push_back(0);
            }
            f.stamps[f.used]->Issue(D3DISSUE_END);
            f.cats[f.used] = closes;
            ++f.used;
        }

        // Charges the time since the last transition to the current category, then stamps the GPU.
        void Transition()
        {
            const int64_t now = Now();
            const int cur = Current();
            g_frame.cpu[cur] += now - g_last;
            g_last = now;
            Stamp(cur);
        }

        // --- the averaged results the panel shows ---------------------------------------------------
        struct Window
        {
            int64_t  cpu[kCount] = {};
            uint64_t draws[kCount] = {}, tris[kCount] = {};
            double   gpu[kCount] = {};     // ms, summed over the frames with GPU results
            unsigned frames = 0, gpuFrames = 0;
            int64_t  frameTicks = 0;
            double   gpuFrameMs = 0.0;
        } g_window;
        struct Shown
        {
            double   cpuMs[kCount] = {}, gpuMs[kCount] = {};
            double   draws[kCount] = {}, tris[kCount] = {};
            double   frameMs = 0.0, gpuFrameMs = 0.0;
            unsigned frames = 0, gpuFrames = 0;
            bool     valid = false;
        } g_shown;
        int64_t g_frameStart = 0, g_windowStart = 0;

        void ReadGpuFrame(GpuFrame& f)
        {
            if (!f.pending) return;
            f.pending = false;
            if (f.overflow || !f.disjoint || !f.freq || f.used < 2) return;
            BOOL disjoint = TRUE;
            UINT64 freq = 0;
            if (f.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || f.freq->GetData(&freq, sizeof(freq), 0) != S_OK)
            { ++g_gpuNotReady; return; }
            if (disjoint || !freq) { ++g_gpuDisjoint; return; }
            std::vector<UINT64> t(f.used);
            for (unsigned k = 0; k < f.used; ++k)
                if (f.stamps[k]->GetData(&t[k], sizeof(UINT64), 0) != S_OK) { ++g_gpuNotReady; return; }
            const double toMs = 1000.0 / static_cast<double>(freq);
            for (unsigned k = 1; k < f.used; ++k)
            {
                const int c = f.cats[k];
                if (c >= 0 && c < kCount && t[k] >= t[k - 1]) g_window.gpu[c] += static_cast<double>(t[k] - t[k - 1]) * toMs;
            }
            if (t[f.used - 1] >= t[0]) g_window.gpuFrameMs += static_cast<double>(t[f.used - 1] - t[0]) * toMs;
            ++g_window.gpuFrames;
        }

        // Present: closes the frame that just ended, folds it into the window, and opens the next.
        void __cdecl OnFrame(void* /*user*/, const void* /*args*/)
        {
            if (!g_mainThread) g_mainThread = GetCurrentThreadId();
            const int64_t now = Now();

            if (g_frameOn)
            {
                Transition();
                if (g_gpuFrameOn)
                {
                    GpuFrame& f = g_gpuFrames[g_gpuCur];
                    f.disjoint->Issue(D3DISSUE_END);
                    f.freq->Issue(D3DISSUE_END);
                    f.pending = true;
                }
                for (int c = 0; c < kCount; ++c)
                {
                    g_window.cpu[c] += g_frame.cpu[c];
                    g_window.draws[c] += g_frame.draws[c];
                    g_window.tris[c] += g_frame.tris[c];
                }
                g_window.frameTicks += now - g_frameStart;
                ++g_window.frames;
            }
            g_frame = FrameAccum{};
            g_depth = 0;

            // Publish about once a second.
            if (now - g_windowStart >= g_qpcFreq)
            {
                Shown s;
                if (g_window.frames)
                {
                    const double n = g_window.frames, toMs = 1000.0 / static_cast<double>(g_qpcFreq);
                    for (int c = 0; c < kCount; ++c)
                    {
                        s.cpuMs[c] = static_cast<double>(g_window.cpu[c]) * toMs / n;
                        s.draws[c] = static_cast<double>(g_window.draws[c]) / n;
                        s.tris[c] = static_cast<double>(g_window.tris[c]) / n;
                        if (g_window.gpuFrames) s.gpuMs[c] = g_window.gpu[c] / g_window.gpuFrames;
                    }
                    s.frameMs = static_cast<double>(g_window.frameTicks) * toMs / n;
                    s.gpuFrameMs = g_window.gpuFrames ? g_window.gpuFrameMs / g_window.gpuFrames : 0.0;
                    s.frames = g_window.frames;
                    s.gpuFrames = g_window.gpuFrames;
                    s.valid = true;
                }
                g_shown = s;
                g_window = Window{};
                g_windowStart = now;
            }

            // The next frame.
            g_frameOn = g_enabled != 0;
            g_frameStart = now;
            g_last = now;
            g_gpuFrameOn = false;
            if (!g_frameOn || !g_gpu || !g_gpuSupported) return;
            auto* dev = static_cast<IDirect3DDevice9*>(gx::RawDevice());
            if (!dev) return;
            if (dev != g_gpuDevice)
            {
                ReleaseGpu();
                if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, nullptr)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr)) ||
                    FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, nullptr)))
                { g_gpuSupported = false; return; }
                g_gpuDevice = dev;
            }
            g_gpuCur = (g_gpuCur + 1) % kGpuFrames;
            GpuFrame& f = g_gpuFrames[g_gpuCur];
            ReadGpuFrame(f); // the oldest frame, about to be reused
            if (!f.disjoint && FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint))) f.disjoint = nullptr;
            if (!f.freq && FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq))) f.freq = nullptr;
            if (!f.disjoint || !f.freq) return;
            f.used = 0;
            f.overflow = false;
            f.disjoint->Issue(D3DISSUE_BEGIN);
            g_gpuFrameOn = true;
            Stamp(-1);
        }

        // --- client passes: the generic wrapper ------------------------------------------------------
        // Each hooked function gets a naked stub. On entry (main thread, profiler on) it records the
        // caller's return address, opens the category and replaces the return address with its leave
        // stub; then it jumps to the next link of the chain with every register as it found them. The
        // leave stub closes the category and returns to the saved address, keeping eax/edx and the FPU
        // state (a float result) intact. No signature or calling convention is involved.
        struct HookDef { const char* point; int category; };
        const HookDef kHooks[] = {
            { "World.Render",                  kWorld },
            { "Adt.RenderChunks",              kTerrain },
            { "Adt.RenderChunksSinglePass",    kTerrain },
            { "Adt.RenderDetailDoodads",       kDetail },
            { "M2.SceneDraw",                  kM2 },
            { "M2.SceneRenderDraw",            kM2 },
            { "M2.SceneAnimate",               kM2Anim },
            { "M2.DrawBatchedParticles",       kParticles },
            { "M2.DrawParticleBatch",          kParticles },
            { "M2.DrawRibbonBatch",            kRibbons },
            { "Wmo.SceneRenderInstanceGroups", kWmo },
            { "Gx.LiquidRenderPass",           kLiquids },
            { "M2.RenderBatchListShadowMap",   kShadows },
            { "Wmo.RenderShadowMapGroups",     kShadows },
            { "Weather.Render",                kWeather },
        };
        constexpr int kHookCount = static_cast<int>(sizeof(kHooks) / sizeof(kHooks[0]));
        constexpr int kMaxHooks = 16;
        static_assert(kHookCount <= kMaxHooks, "add stubs");

        struct RetEntry { int slot; uintptr_t ret; };
        RetEntry g_ret[kMaxDepth];
        int      g_retDepth = 0;
        bool     g_hooked[kMaxHooks] = {};
        unsigned g_hookCalls[kMaxHooks] = {};
    }

    void Enter(int category)
    {
        if (!g_frameOn || !OnMainThread()) return;
        if (g_depth >= kMaxDepth) return;
        Transition();
        g_stack[g_depth++] = category;
    }

    void Leave(int category)
    {
        if (!g_frameOn || !OnMainThread()) return;
        // The topmost matching entry (scopes that don't nest perfectly, e.g. an event-bracketed one
        // around a hooked function, still come out right).
        for (int i = g_depth - 1; i >= 0; --i)
            if (g_stack[i] == category)
            {
                Transition();
                for (int k = i; k + 1 < g_depth; ++k) g_stack[k] = g_stack[k + 1];
                --g_depth;
                return;
            }
    }
}

// The C side of the stubs (no FPU or SSE state is used here: integer timing only, the queries are
// device calls -- the leave stub saves the FPU state around this anyway).
extern "C" int __cdecl LaProfHookEnter(int slot, uintptr_t ret)
{
    using namespace wxl_livingazeroth::prof;
    if (!g_frameOn || !OnMainThread() || g_retDepth >= kMaxDepth) return 0;
    g_ret[g_retDepth++] = { slot, ret };
    ++g_hookCalls[slot];
    Enter(kHooks[slot].category);
    return 1;
}

extern "C" uintptr_t __cdecl LaProfHookLeave(int /*slot*/)
{
    using namespace wxl_livingazeroth::prof;
    const RetEntry e = g_ret[--g_retDepth];
    Leave(kHooks[e.slot].category);
    return e.ret;
}

// Plain C globals, so the inline assembly below can name them: per stub, the next link of its chain,
// and its leave stub.
extern "C" void* g_laProfOriginal[16] = {};
extern "C" void* g_laProfLeave[16] = {};

// pushfd + pushad = 36 bytes, so the caller's return address sits at [esp + 36] in the enter stub.
// The leave stub pushes a slot for the return address first, then flags, registers and the 108-byte
// FPU state: the slot is at [esp + 36] again once the FPU state is popped.
#define LA_PROF_STUBS(N)                                                                               \
    extern "C" __declspec(naked) void LaProfLeave##N()                                                \
    {                                                                                                  \
        __asm push eax                                                                                 \
        __asm pushfd                                                                                   \
        __asm pushad                                                                                   \
        __asm sub esp, 108                                                                             \
        __asm fnsave [esp]                                                                             \
        __asm push N                                                                                   \
        __asm call LaProfHookLeave                                                                     \
        __asm add esp, 4                                                                               \
        __asm frstor [esp]                                                                             \
        __asm add esp, 108                                                                             \
        __asm mov dword ptr [esp + 36], eax                                                            \
        __asm popad                                                                                    \
        __asm popfd                                                                                    \
        __asm ret                                                                                      \
    }                                                                                                  \
    extern "C" __declspec(naked) void LaProfEnter##N()                                                \
    {                                                                                                  \
        __asm pushfd                                                                                   \
        __asm pushad                                                                                   \
        __asm push dword ptr [esp + 36]                                                                \
        __asm push N                                                                                   \
        __asm call LaProfHookEnter                                                                     \
        __asm add esp, 8                                                                               \
        __asm mov ecx, dword ptr [esp + 36]                                                            \
        __asm test eax, eax                                                                            \
        __asm cmovnz ecx, dword ptr [g_laProfLeave + N * 4]                                            \
        __asm mov dword ptr [esp + 36], ecx                                                            \
        __asm popad                                                                                    \
        __asm popfd                                                                                    \
        __asm jmp dword ptr [g_laProfOriginal + N * 4]                                                 \
    }

LA_PROF_STUBS(0)  LA_PROF_STUBS(1)  LA_PROF_STUBS(2)  LA_PROF_STUBS(3)
LA_PROF_STUBS(4)  LA_PROF_STUBS(5)  LA_PROF_STUBS(6)  LA_PROF_STUBS(7)
LA_PROF_STUBS(8)  LA_PROF_STUBS(9)  LA_PROF_STUBS(10) LA_PROF_STUBS(11)
LA_PROF_STUBS(12) LA_PROF_STUBS(13) LA_PROF_STUBS(14) LA_PROF_STUBS(15)

namespace wxl_livingazeroth::prof
{
    namespace
    {
        void (*const kEnterStubs[kMaxHooks])() = {
            &LaProfEnter0, &LaProfEnter1, &LaProfEnter2, &LaProfEnter3, &LaProfEnter4, &LaProfEnter5, &LaProfEnter6, &LaProfEnter7,
            &LaProfEnter8, &LaProfEnter9, &LaProfEnter10, &LaProfEnter11, &LaProfEnter12, &LaProfEnter13, &LaProfEnter14, &LaProfEnter15 };
        void (*const kLeaveStubs[kMaxHooks])() = {
            &LaProfLeave0, &LaProfLeave1, &LaProfLeave2, &LaProfLeave3, &LaProfLeave4, &LaProfLeave5, &LaProfLeave6, &LaProfLeave7,
            &LaProfLeave8, &LaProfLeave9, &LaProfLeave10, &LaProfLeave11, &LaProfLeave12, &LaProfLeave13, &LaProfLeave14, &LaProfLeave15 };

        // Draws, at the client's own Gx draw (batch: +0x08 index count; core offsets/engine/Gx.hpp).
        using DeviceDrawFn = void(__fastcall*)(void* device, void* edx, uint32_t* batch, int indexed);
        DeviceDrawFn g_origDraw = nullptr;
        void __fastcall hkDeviceDraw(void* device, void* edx, uint32_t* batch, int indexed)
        {
            if (g_frameOn && OnMainThread())
            {
                const int c = Current();
                ++g_frame.draws[c];
                if (batch) g_frame.tris[c] += batch[2] / 3;
            }
            g_origDraw(device, edx, batch, indexed);
        }

        void __cdecl OnWorldRenderEnd(void*, const void*) { Enter(kUi); }
        void __cdecl OnEndScene(void*, const void*) { Leave(kUi); }

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

        std::string Report()
        {
            const Shown& s = g_shown;
            std::string out;
            char line[256];
            std::snprintf(line, sizeof(line), "frame: %.2f ms CPU (%.0f fps)%s", s.frameMs, s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0,
                          s.gpuFrames ? "" : "; GPU: no results yet (or timing off / unsupported)");
            out += line;
            if (s.gpuFrames)
            {
                std::snprintf(line, sizeof(line), "; GPU frame span %.2f ms (%u of %u frames read back)", s.gpuFrameMs, s.gpuFrames, s.frames);
                out += line;
            }
            out += "\n";
            out += "category                            draws   ktris   CPU ms   GPU ms\n";
            // Biggest first, by whichever of CPU and GPU is larger.
            int order[kCount];
            for (int c = 0; c < kCount; ++c) order[c] = c;
            std::sort(order, order + kCount, [&](int a, int b) { return std::max(s.cpuMs[a], s.gpuMs[a]) > std::max(s.cpuMs[b], s.gpuMs[b]); });
            double draws = 0.0, tris = 0.0;
            for (int k = 0; k < kCount; ++k)
            {
                const int c = order[k];
                draws += s.draws[c]; tris += s.tris[c];
                if (s.cpuMs[c] < 0.005 && s.gpuMs[c] < 0.005 && s.draws[c] < 0.5) continue;
                std::snprintf(line, sizeof(line), "%-34s %6.0f %7.1f %8.2f %8.2f\n", kNames[c], s.draws[c], s.tris[c] / 1000.0, s.cpuMs[c], s.gpuMs[c]);
                out += line;
            }
            std::snprintf(line, sizeof(line), "%-34s %6.0f %7.1f\n", "total", draws, tris / 1000.0);
            out += line;
            return out;
        }

        void __cdecl Panel(void*)
        {
            g_api->UiCheckbox("profile the frame (a little overhead while on)", &g_enabled);
            g_api->UiCheckbox("GPU time per category (timestamp queries; read back 3 frames late)", &g_gpu);
            if (!g_gpuSupported) g_api->UiTextWrapped("GPU timestamps are not supported by this device.");
            unsigned hooked = 0;
            for (int k = 0; k < kHookCount; ++k) hooked += g_hooked[k] ? 1 : 0;
            char line[640];
            std::snprintf(line, sizeof(line), "%u of %d client passes wrapped; draw counter %s. GPU frames not ready in time: %u, disjoint: %u. "
                          "Times are exclusive (nested categories are not counted twice), averaged over about a second. "
                          "GPU times are spans on the GPU's timeline, waiting included: when the CPU is the limit "
                          "(the GPU span about equals the CPU frame), the GPU's idle time lands in 'other', and the CPU column is the one to read.",
                          hooked, kHookCount, g_origDraw ? "on" : "off", g_gpuNotReady, g_gpuDisjoint);
            g_api->UiTextWrapped(line);
            if (!g_enabled) { g_api->UiText("(off)"); return; }
            if (!g_shown.valid) { g_api->UiText("(collecting...)"); return; }
            const std::string report = Report();
            if (g_api->UiButton("Copy to clipboard##profcopy")) CopyToClipboard(report);
            size_t at = 0;
            while (at < report.size())
            {
                const size_t end = report.find('\n', at);
                const std::string row = report.substr(at, end == std::string::npos ? std::string::npos : end - at);
                g_api->UiText(row.c_str());
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
        for (int k = 0; k < kHookCount; ++k)
        {
            g_laProfLeave[k] = reinterpret_cast<void*>(kLeaveStubs[k]);
            // First in every chain: everything else on the same point runs inside the measurement. A chain
            // that's already live (core installs some of its own detours at start-up) refuses a new head,
            // so then behind its current head.
            g_hooked[k] = api->HookAttachByName(kHooks[k].point, reinterpret_cast<void*>(kEnterStubs[k]), &g_laProfOriginal[k], -1000) != 0 ||
                          api->HookAttachByName(kHooks[k].point, reinterpret_cast<void*>(kEnterStubs[k]), &g_laProfOriginal[k], 1000) != 0;
            if (!g_hooked[k])
            {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "profiler: hook point '%s' not found; that category stays empty", kHooks[k].point);
                api->Log(WXL_LOG_WARN, "wxl-seyris-living-azeroth", msg);
            }
        }
        // Behind core's own detour on the draw (live from start-up, so it can't be preceded): counting
        // doesn't need to be first.
        if (!api->HookAttachByName("Gx.DeviceDraw", reinterpret_cast<void*>(&hkDeviceDraw), reinterpret_cast<void**>(&g_origDraw), 1000))
            g_origDraw = nullptr;
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnFrame), &OnFrame, nullptr);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldRenderEnd), &OnWorldRenderEnd, nullptr);
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnEndScene), &OnEndScene, nullptr);
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }

    void OnDeviceLost() { ReleaseGpu(); }
}
