#include "GrassMotion.hpp"

#include "GrassDoodads.hpp"

#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"
#include "../render/ShaderPatch.hpp"

#include "game/Camera.hpp"
#include "game/Gx.hpp"
#include "game/Pick.hpp"
#include "offsets/game/GroundEffect.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>

namespace wxl_livingazeroth::grass
{
    namespace
    {
        namespace ge = wxl::offsets::game::groundeffect;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // Wind grid: kGrid x kGrid samples, kSpacing yards apart, centred on the player, uploaded to
        // vertex constants c40.. (one float4 per cell: windX*strength, windY*strength, gust, 0).
        constexpr int   kGrid = 8;
        constexpr float kSpacing = 8.0f;
        constexpr int   kGridFirstReg = 40;

        // Stock grass vertex shaders (vs_3_0), live group only: no shadow, shadow-low, shadow-high.
        // The dormant point-light group (1328/1456/1624) reads c14..c22, which we use, so it's left
        // stock.
        const uint32_t kLiveGrassLengths[] = { 548, 676, 844 };

        // --- the patch ------------------------------------------------------------------------------
        // Stock temps are only r0/r1, so r2..r9 are free. Registers we feed:
        //   engine block (shipped with every grass chunk): c14..c22
        //     c14 = camera world xyz, time (s)
        //     c15 = anchor, 1/(1-anchor), amplitude, flutter
        //     c16 = 1/spacing, 1/spacing, grid offset x, grid offset y
        //     c17 = flutter speed, phase-per-yard x, phase-per-yard y, push strength
        //     c18 = player xyz, push radius
        //     c19 = debug: x = ignore UV bend weight (0/1)
        //   our own upload: c40..c103 = the wind grid
        //   shader defs: c36..c38
        // (vs_3_0 allows one constant register per instruction, so constants that meet in one
        // instruction share a register.)
        const char* kPrologue =
            "    def c36, 0.001, 3.0, 0.3, 8.0\n"                 // eps, push height window, flutter base, grid stride
            "    def c37, 0.15915494, 0.5, 6.2831853, 3.1415927\n" // 1/2pi, 0.5, 2pi, pi
            "    def c38, 6.999, 0.0, 0.0, 0.0\n";                // grid clamp

        // One doodad-table entry (A = rectangle register, B = mapping register): is the vertex's UV
        // inside the rectangle, and how far is it from root to tip within it.
        std::string DoodadEntryAsm(int a, int b)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "    sge r10.xy, v3.xy, c%d.xy\n"
                "    sge r10.zw, c%d, v3.xyxy\n"
                "    mul r10.xy, r10, r10.zwzw\n"
                "    mul r10.x, r10.x, r10.y\n"
                "    add r11.x, v3.y, -c%d.x\n"
                "    mul_sat r11.x, r11.x, c%d.y\n"
                "    mad r12.x, r10.x, r11.x, r12.x\n"
                "    mad r12.yz, r10.x, c%d.xzwx, r12\n"
                "    add r12.w, r12.w, r10.x\n",
                a, a, b, b, b);
            return buf;
        }

        // Inserted right after the stock "add r0, r0, c3" (view-space position complete in r0).
        std::string BuildBody()
        {
            return
            // World position: grass batches are unrotated, so c0..c2 are the pure view rotation.
            "    dp3 r2.x, c0, r0\n"
            "    dp3 r2.y, c1, r0\n"
            "    dp3 r2.z, c2, r0\n"
            "    add r2.xyz, r2, c14\n"
            // Per-doodad table (c104..c111, uploaded before each layer draw): 4 entries of
            //   A = {uMin, vMin, uMax, vMax} (the doodad's atlas rectangle)
            //   B = {rootV, 1/(tipV-rootV), windScale, pushScale}
            // The vertex's UV picks its entry; accumulate into r12 = {raw bend, wind, push, matched}.
            "    mov r12, c13.x\n"
            + DoodadEntryAsm(104, 105) + DoodadEntryAsm(106, 107)
            + DoodadEntryAsm(108, 109) + DoodadEntryAsm(110, 111) +
            // Overlapping atlas rectangles can match twice: average rather than add.
            "    max r13.x, r12.w, c13.y\n"
            "    rcp r13.x, r13.x\n"
            "    mul r12.xyz, r12, r13.x\n"
            // No matching entry: fall back to "uv.y is 1 at the root, 0 at the tip", full wind/push.
            "    min r12.w, r12.w, c13.y\n"
            "    add r13.x, c13.y, -r12.w\n"
            "    add r13.y, c13.y, -v3.y\n"
            "    mad r3.w, r13.x, r13.y, r12.x\n"
            "    add r13.z, r12.y, r13.x\n"
            "    add r13.w, r12.z, r13.x\n"
            // Stiff base, squared falloff.
            "    add r3.w, r3.w, -c15.x\n"
            "    mul_sat r3.w, r3.w, c15.y\n"
            "    mul r3.w, r3.w, r3.w\n"
            // Debug (c19.x = 1): ignore the UV weight so every vertex moves fully.
            "    add r4.w, c13.y, -r3.w\n"
            "    mad r3.w, r4.w, c19.x, r3.w\n"
            // Wind grid lookup, bilinear, clamped to the grid edge.
            "    mul r4.xy, r2, c16\n"
            "    add r4.xy, r4, -c16.zw\n"
            "    max r4.xy, r4, c13.x\n"
            "    min r4.xy, r4, c38.x\n"
            "    frc r5.xy, r4\n"
            "    add r4.xy, r4, -r5\n"
            "    mad r4.x, r4.y, c36.w, r4.x\n"
            "    mova a0.x, r4.x\n"
            "    mov r6, c40[a0.x]\n"
            "    mov r7, c41[a0.x]\n"
            "    lrp r8, r5.x, r7, r6\n"
            "    mov r6, c48[a0.x]\n"
            "    mov r7, c49[a0.x]\n"
            "    lrp r9, r5.x, r7, r6\n"
            "    lrp r6, r5.y, r9, r8\n"
            // Flutter: a per-blade phase (position dependent), stronger in gusts.
            "    mov r7.x, c14.w\n"
            "    mul r7.x, r7.x, c17.x\n"
            "    mad r7.x, r2.x, c17.y, r7.x\n"
            "    mad r7.x, r2.y, c17.z, r7.x\n"
            "    mad r7.x, r7.x, c37.x, c37.y\n"
            "    frc r7.x, r7.x\n"
            "    mad r7.x, r7.x, c37.z, -c37.w\n"
            "    sincos r8.xy, r7.x\n"
            "    add r7.y, r6.z, c36.z\n"
            "    mul r7.y, r7.y, c15.w\n"
            "    mad r7.z, r7.y, r8.y, c15.z\n"
            "    mul r8.xy, r6, r7.z\n"
            "    mul r8.xy, r8, r13.z\n"
            // Player parting: lean away within the radius, only near the player's height.
            "    add r9.xy, r2, -c18\n"
            "    mul r9.z, r9.x, r9.x\n"
            "    mad r9.z, r9.y, r9.y, r9.z\n"
            "    add r9.z, r9.z, c36.x\n"
            "    rsq r9.w, r9.z\n"
            "    mul r9.z, r9.z, r9.w\n"
            "    rcp r7.w, c18.w\n"
            "    mul r9.z, r9.z, r7.w\n"
            "    add r9.z, c13.y, -r9.z\n"
            "    max r9.z, r9.z, c13.x\n"
            "    add r7.w, r2.z, -c18.z\n"
            "    abs r7.w, r7.w\n"
            "    add r7.w, c36.y, -r7.w\n"
            "    max r7.w, r7.w, c13.x\n"
            "    min r7.w, r7.w, c13.y\n"
            "    mul r9.z, r9.z, r7.w\n"
            "    mul r9.z, r9.z, c17.w\n"
            "    mul r9.z, r9.z, r13.w\n"
            "    mul r9.z, r9.z, r9.w\n"
            "    mad r8.xy, r9, r9.z, r8\n"
            // Apply the bend weight and rotate the world-space offset back into view space.
            "    mul r8.xy, r8, r3.w\n"
            "    mul r4.xyz, c0, r8.x\n"
            "    mad r4.xyz, c1, r8.y, r4\n"
            "    add r0.xyz, r0, r4\n";
        }

        // --- state ------------------------------------------------------------------------------
        const WXL_Api* g_api = nullptr;
        Settings       g_settings;
        const char*    g_disabled = "not installed yet";
        float          g_grid[kGrid * kGrid][4] = {};
        unsigned       g_chunkUploads = 0, g_chunkUploadsLast = 0;

        ge::InitShaderConstantsFn  g_origInit = nullptr;
        ge::ChunkConstantUploadFn  g_origChunk = nullptr;

        float SecondsNow() { return static_cast<float>(GetTickCount() % 3600000u) * 0.001f; }

        // World-snapped grid cells: cached ground height (so hills above/below the player sample
        // their own surface) and an eased wind value (so changes blend instead of snapping).
        constexpr float    kEaseSeconds     = 0.35f;
        constexpr float    kGroundRecheck   = 10.0f;  // seconds; terrain/WMOs stream in late
        constexpr float    kGroundSearch    = 55.0f;  // GroundZ searches +-60 yd around the player
        constexpr unsigned kGroundRayBudget = 16;     // ground probes per frame

        struct GridCell
        {
            float groundZ = 0.0f, groundAt = 0.0f, lastSeen = 0.0f;
            float value[3] = {};
            bool  haveGround = false, fresh = true;
        };
        std::unordered_map<uint64_t, GridCell> g_cells;
        float g_lastGridTime = 0.0f;

        uint64_t CellKey(int cx, int cy)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
        }

        // Once per frame at the top of the grass pass: the engine just rebuilt c0..c13 and zeroed the
        // rest of its block; we fill c14..c18 (shipped with every chunk) and rebuild the wind grid.
        void __cdecl hkInitShaderConstants()
        {
            g_origInit();

            g_chunkUploadsLast = g_chunkUploads;
            g_chunkUploads = 0;

            const world::Snapshot& s = world::Current();
            const Settings& p = g_settings;
            const bool on = p.enabled && s.inWorld;

            float cam[3];
            wxl::game::camera::GetPosition(cam);

            // Grid origin snapped to world cells (so each cell is a fixed place with its own cached
            // ground height and smoothed wind), with the player near the middle.
            const float inv = 1.0f / kSpacing;
            const int   baseX = static_cast<int>(std::floor(s.playerPos[0] * inv)) - kGrid / 2;
            const int   baseY = static_cast<int>(std::floor(s.playerPos[1] * inv)) - kGrid / 2;
            const float originX = baseX * kSpacing, originY = baseY * kSpacing;

            const float now = SecondsNow();
            float dt = now - g_lastGridTime;
            if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
            g_lastGridTime = now;

            const float anchor = p.anchor < 0.0f ? 0.0f : (p.anchor > 0.9f ? 0.9f : p.anchor);
            const float c[6][4] = {
                { cam[0], cam[1], cam[2], SecondsNow() },
                { anchor, 1.0f / (1.0f - anchor), on ? p.amplitude : 0.0f, on ? p.flutter : 0.0f },
                { inv, inv, originX * inv + 0.5f, originY * inv + 0.5f },
                { p.flutterSpeed, 0.61f, 0.37f, on ? p.pushStrength : 0.0f },
                { s.playerPos[0], s.playerPos[1], s.playerPos[2], p.pushRadius > 0.1f ? p.pushRadius : 0.1f },
                { p.debugIgnoreUv ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f },
            };
            float* block = reinterpret_cast<float*>(ge::kVsConstantBlock) + ge::kVsFirstFreeReg * 4;
            std::memcpy(block, c, sizeof(c));

            // Wind grid from the full model (weather, gusts, shelter, lee), sampled at each cell's
            // ground height and eased toward its target so changes blend instead of snapping.
            const float ease = 1.0f - std::exp(-dt / kEaseSeconds);
            unsigned groundRays = 0;
            for (int iy = 0; iy < kGrid; ++iy)
                for (int ix = 0; ix < kGrid; ++ix)
                {
                    const int cx = baseX + ix, cy = baseY + iy;
                    GridCell& gc = g_cells[CellKey(cx, cy)];
                    gc.lastSeen = now;

                    const float x = (cx + 0.5f) * kSpacing, y = (cy + 0.5f) * kSpacing;
                    const bool groundStale = !gc.haveGround || now - gc.groundAt > kGroundRecheck
                                          || std::fabs(gc.groundZ - s.playerPos[2]) > kGroundSearch;
                    if (groundStale && groundRays < kGroundRayBudget)
                    {
                        ++groundRays;
                        float z;
                        if (wxl::game::world::GroundZ(x, y, s.playerPos[2], z)) { gc.groundZ = z; gc.haveGround = true; }
                        else if (!gc.haveGround) gc.groundZ = s.playerPos[2];
                        gc.groundAt = now;
                    }
                    const float pos[3] = { x, y, gc.haveGround ? gc.groundZ : s.playerPos[2] };

                    const wind::Sample w = on ? wind::At(pos) : wind::Sample{};
                    const float target[3] = { on ? w.dirX * w.strength : 0.0f,
                                               on ? w.dirY * w.strength : 0.0f,
                                               on ? w.gust : 0.0f };
                    for (int k = 0; k < 3; ++k)
                        gc.value[k] = gc.fresh ? target[k] : gc.value[k] + (target[k] - gc.value[k]) * ease;
                    gc.fresh = false;

                    float* cell = g_grid[iy * kGrid + ix];
                    cell[0] = gc.value[0]; cell[1] = gc.value[1]; cell[2] = gc.value[2]; cell[3] = 0.0f;
                }

            // Forget cells we haven't needed for a while (the grid moves with the player).
            if (g_cells.size() > kGrid * kGrid * 4)
                for (auto it = g_cells.begin(); it != g_cells.end();)
                    it = (now - it->second.lastSeen > 5.0f || now < it->second.lastSeen) ? g_cells.erase(it) : std::next(it);
        }

        // Per grass chunk, after the engine's own c0..c22 upload: add the wind grid. Uploaded per
        // chunk rather than once per frame so nothing drawn between chunks can clobber it.
        void __cdecl hkChunkConstantUpload(const float* mtx, int group)
        {
            g_origChunk(mtx, group);
            if (group != 0) return; // the dormant point-light path isn't patched
            if (auto* dev = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice()))
            {
                dev->SetVertexShaderConstantF(kGridFirstReg, &g_grid[0][0], kGrid * kGrid);
                ++g_chunkUploads;
            }
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;

        if (GetModuleHandleA("wxl-grasswind.dll"))
        {
            g_disabled = "wxl-grasswind is loaded (both patch the same grass shaders); ours stays off";
            api->Log(WXL_LOG_WARN, kTag, "grass motion: %s. Remove wxl-grasswind to use Living Azeroth's grass wind.", g_disabled);
            return;
        }

        shaderpatch::VertexRule rule;
        rule.name = "grass-motion";
        rule.versionToken = 0xFFFE0300u;
        rule.stockLengths.assign(std::begin(kLiveGrassLengths), std::end(kLiveGrassLengths));
        rule.prologue = kPrologue;
        rule.anchor = "add r0, r0, c3";
        rule.body = BuildBody();
        shaderpatch::Register(std::move(rule));

        const int a = api->HookAttach("LivingAzeroth.GrassInitConstants", ge::kInitShaderConstants,
                                      reinterpret_cast<void*>(&hkInitShaderConstants),
                                      reinterpret_cast<void**>(&g_origInit), WXL_HOOK_DEFAULT_PRIORITY);
        const int b = api->HookAttach("LivingAzeroth.GrassChunkUpload", ge::kChunkConstantUpload,
                                      reinterpret_cast<void*>(&hkChunkConstantUpload),
                                      reinterpret_cast<void**>(&g_origChunk), WXL_HOOK_DEFAULT_PRIORITY);
        g_disabled = (a && b) ? nullptr : "constant hooks failed to install";
        api->Log((a && b) ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "grass motion: constant hooks %s.",
                 (a && b) ? "installed" : "FAILED");

        // Per-doodad bend/opt-out table. Without it the shader falls back to the default bend.
        grassdoodads::Install(api);
    }

    Settings&   Tunables()              { return g_settings; }
    const char* DisabledReason()        { return g_disabled; }
    unsigned    ChunkUploadsLastFrame() { return g_chunkUploadsLast; }
}
