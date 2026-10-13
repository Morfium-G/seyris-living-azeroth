#include "SurfaceCoverInternal.hpp"

#include <atomic>
#include <cstring>

namespace wxl_livingazeroth::cover::detail
{
    namespace
    {
        // How an attached model is posed (traced 2026-10-13): at the end of its parent's bone build
        // (0x82F0F0, or 0x82E140 for single-bone models), 0x82E550 walks the parent's attached
        // children (+0x58 / +0x60); for each one hanging on an attachment (+0x54 = index into the
        // parent's attachment records: +0x2C -> +0x150 -> +0xF4, stride 0x28, id at +0, bone at +4,
        // position at +8) it takes the parent's palette matrix of that bone (+0x98), translated by the
        // attachment's position, and calls the child's own build with it as the context matrix
        // (CM2Model::Animate 0x830DC0 does the same). Roots get the scene's context (scene +0x84 =
        // world -> view: translate(-camera) x view). Everything downstream -- the child's bones, its
        // particles, its own children -- comes from that context, so shifting it moves the whole
        // effect. These builds run on the scene's animate threads.
        //
        // Spell effects at a unit's feet (paladin auras: SpellVisualKit base effects) hang on
        // attachment 19 (Base). The cover lies on top of the terrain, so in deep snow they sit under
        // it; this lifts them by the cover depth under them, only as far as they're below its top
        // (a unit jumping or flying keeps its effect where it is).
        constexpr uintptr_t kBuildPalette = 0x0082F0F0;
        constexpr uintptr_t kBuildPaletteSimple = 0x0082E140;
        using BuildFn = void(__fastcall*)(void* inst, void* edx, const float* ctx, const float* scale3,
                                          const float* translate3, float speed, float alpha);
        BuildFn g_origBuild = nullptr, g_origBuildSimple = nullptr;

        constexpr uint32_t kAttachBase = 19;
        constexpr size_t kOffInstScene = 0x28, kOffInstShared = 0x2C, kOffInstParent = 0x48, kOffInstAttachSlot = 0x54;
        constexpr size_t kOffSharedHeader = 0x150, kOffHdrAttachCount = 0xF0, kOffHdrAttachPtr = 0xF4, kAttachStride = 0x28;
        constexpr size_t kOffSceneCtx = 0x84;

        std::atomic<unsigned> g_lifted{ 0 };
        std::atomic<float>    g_lastLift{ 0.0f };

        template <class T> T Read(const void* base, size_t off) { T v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T)); return v; }

        // The context to build `inst` with: `ctx` itself, or a lifted copy in `out`.
        const float* Lifted(void* inst, const float* ctx, float out[16])
        {
            if (!g_settings.liftBase || !inst || !ctx) return ctx;
            __try
            {
                void* parent = Read<void*>(inst, kOffInstParent);
                const uint32_t slot = Read<uint32_t>(inst, kOffInstAttachSlot);
                if (!parent || slot == 0xFFFF) return ctx;
                const void* shared = Read<void*>(parent, kOffInstShared);
                const void* header = shared ? Read<void*>(shared, kOffSharedHeader) : nullptr;
                if (!header || slot >= Read<uint32_t>(header, kOffHdrAttachCount)) return ctx;
                const uint8_t* records = Read<const uint8_t*>(header, kOffHdrAttachPtr);
                if (!records || Read<uint32_t>(records + slot * kAttachStride, 0) != kAttachBase) return ctx;
                const void* scene = Read<void*>(inst, kOffInstScene);
                if (!scene) return ctx;
                float S[16];
                std::memcpy(S, static_cast<const uint8_t*>(scene) + kOffSceneCtx, sizeof(S));

                // The attachment's world position: ctx's origin through the scene context's inverse
                // (rigid, row vectors: view = world x R + t, so world = (view - t) x R^T).
                const float v[3] = { ctx[12] - S[12], ctx[13] - S[13], ctx[14] - S[14] };
                float w[3];
                for (int i = 0; i < 3; ++i) w[i] = v[0] * S[i * 4 + 0] + v[1] * S[i * 4 + 1] + v[2] * S[i * 4 + 2];

                // Level 0 only (the animate threads; see SampleTop): effects near the player. Without
                // trenches: an aura is yards wide, the unit's own footprint ~0.4 yd -- it lies on the
                // snow around the unit (and doesn't pop up on landing while the trench presses in).
                float top, depth;
                if (!SampleTop(w[0], w[1], 1, top, depth, false)) return ctx;
                float lift = top - w[2];
                if (lift <= 0.0f) return ctx;
                if (lift > depth) lift = depth;

                // World up in the context's space is the scene context's z row.
                std::memcpy(out, ctx, 16 * sizeof(float));
                out[12] += lift * S[8];
                out[13] += lift * S[9];
                out[14] += lift * S[10];
                g_lifted.fetch_add(1, std::memory_order_relaxed);
                g_lastLift.store(lift, std::memory_order_relaxed);
                return out;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return ctx; }
        }

        void __fastcall hkBuild(void* inst, void* edx, const float* ctx, const float* scale3, const float* translate3, float speed, float alpha)
        {
            float lifted[16];
            g_origBuild(inst, edx, Lifted(inst, ctx, lifted), scale3, translate3, speed, alpha);
        }

        void __fastcall hkBuildSimple(void* inst, void* edx, const float* ctx, const float* scale3, const float* translate3, float speed, float alpha)
        {
            float lifted[16];
            g_origBuildSimple(inst, edx, Lifted(inst, ctx, lifted), scale3, translate3, speed, alpha);
        }
    }

    unsigned LiftedCount() { return g_lifted.load(std::memory_order_relaxed); }
    float    LastLift() { return g_lastLift.load(std::memory_order_relaxed); }

    void InstallLift(const WXL_Api* api)
    {
        const bool full = api->HookAttach("LivingAzeroth.CoverLift", kBuildPalette, reinterpret_cast<void*>(&hkBuild),
                                          reinterpret_cast<void**>(&g_origBuild), WXL_HOOK_DEFAULT_PRIORITY);
        const bool simple = api->HookAttach("LivingAzeroth.CoverLiftSimple", kBuildPaletteSimple, reinterpret_cast<void*>(&hkBuildSimple),
                                            reinterpret_cast<void**>(&g_origBuildSimple), WXL_HOOK_DEFAULT_PRIORITY);
        if (!full || !simple)
            api->Log(WXL_LOG_WARN, kTag, "surface cover: base-effect lift hooks failed (full %d, simple %d); effects at the feet stay under the cover.",
                     full ? 1 : 0, simple ? 1 : 0);
    }
}
