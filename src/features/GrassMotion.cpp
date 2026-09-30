#include "GrassMotion.hpp"

#include "../env/Wind.hpp"
#include "../env/WorldQuery.hpp"
#include "../render/ShaderPatch.hpp"

#include "game/Camera.hpp"
#include "game/Gx.hpp"
#include "offsets/game/GroundEffect.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <iterator>

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
        //   our own upload: c40..c103 = the wind grid
        //   shader defs: c36..c38
        // (vs_3_0 allows one constant register per instruction, so constants that meet in one
        // instruction share a register.)
        const char* kPrologue =
            "    def c36, 0.001, 3.0, 0.3, 8.0\n"                 // eps, push height window, flutter base, grid stride
            "    def c37, 0.15915494, 0.5, 6.2831853, 3.1415927\n" // 1/2pi, 0.5, 2pi, pi
            "    def c38, 6.999, 0.0, 0.0, 0.0\n";                // grid clamp

        // Inserted right after the stock "add r0, r0, c3" (view-space position complete in r0).
        const char* kBody =
            // World position: grass batches are unrotated, so c0..c2 are the pure view rotation.
            "    dp3 r2.x, c0, r0\n"
            "    dp3 r2.y, c1, r0\n"
            "    dp3 r2.z, c2, r0\n"
            "    add r2.xyz, r2, c14\n"
            // Bend weight: uv.y is 1 at the root, 0 at the tip; stiff base, squared falloff.
            "    add r3.w, c13.y, -v3.y\n"
            "    add r3.w, r3.w, -c15.x\n"
            "    mul_sat r3.w, r3.w, c15.y\n"
            "    mul r3.w, r3.w, r3.w\n"
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
            "    mul r9.z, r9.z, r9.w\n"
            "    mad r8.xy, r9, r9.z, r8\n"
            // Apply the bend weight and rotate the world-space offset back into view space.
            "    mul r8.xy, r8, r3.w\n"
            "    mul r4.xyz, c0, r8.x\n"
            "    mad r4.xyz, c1, r8.y, r4\n"
            "    add r0.xyz, r0, r4\n";

        // --- state ------------------------------------------------------------------------------
        const WXL_Api* g_api = nullptr;
        Settings       g_settings;
        const char*    g_disabled = "not installed yet";
        float          g_grid[kGrid * kGrid][4] = {};
        unsigned       g_chunkUploads = 0, g_chunkUploadsLast = 0;

        ge::InitShaderConstantsFn  g_origInit = nullptr;
        ge::ChunkConstantUploadFn  g_origChunk = nullptr;

        float SecondsNow() { return static_cast<float>(GetTickCount() % 3600000u) * 0.001f; }

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

            // Grid origin: the player sits in the middle of the grid.
            const float half = kGrid * kSpacing * 0.5f;
            const float originX = s.playerPos[0] - half, originY = s.playerPos[1] - half;
            const float inv = 1.0f / kSpacing;

            const float anchor = p.anchor < 0.0f ? 0.0f : (p.anchor > 0.9f ? 0.9f : p.anchor);
            const float c[5][4] = {
                { cam[0], cam[1], cam[2], SecondsNow() },
                { anchor, 1.0f / (1.0f - anchor), on ? p.amplitude : 0.0f, on ? p.flutter : 0.0f },
                { inv, inv, originX * inv + 0.5f, originY * inv + 0.5f },
                { p.flutterSpeed, 0.61f, 0.37f, on ? p.pushStrength : 0.0f },
                { s.playerPos[0], s.playerPos[1], s.playerPos[2], p.pushRadius > 0.1f ? p.pushRadius : 0.1f },
            };
            float* block = reinterpret_cast<float*>(ge::kVsConstantBlock) + ge::kVsFirstFreeReg * 4;
            std::memcpy(block, c, sizeof(c));

            // Wind grid from the full model (weather, gusts, shelter, lee).
            for (int iy = 0; iy < kGrid; ++iy)
                for (int ix = 0; ix < kGrid; ++ix)
                {
                    const float pos[3] = { originX + (ix + 0.5f) * kSpacing,
                                           originY + (iy + 0.5f) * kSpacing, s.playerPos[2] };
                    const wind::Sample w = on ? wind::At(pos) : wind::Sample{};
                    float* cell = g_grid[iy * kGrid + ix];
                    cell[0] = on ? w.dirX * w.strength : 0.0f;
                    cell[1] = on ? w.dirY * w.strength : 0.0f;
                    cell[2] = on ? w.gust : 0.0f;
                    cell[3] = 0.0f;
                }
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
        rule.body = kBody;
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
    }

    Settings&   Tunables()              { return g_settings; }
    const char* DisabledReason()        { return g_disabled; }
    unsigned    ChunkUploadsLastFrame() { return g_chunkUploadsLast; }
}
