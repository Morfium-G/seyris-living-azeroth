#include "SurfaceCover.hpp"
#include "SurfaceCoverInternal.hpp"

#include <cmath>

namespace wxl_livingazeroth::cover::detail
{
    CameraStats g_cameraStats;

    namespace
    {
        // The 3rd-person camera's collision test (traced 2026-10-13): CGCamera thiscall
        // 0x6059E0(float* distance, const C3Vector* eye, const C3Vector* pivot, flags), ret 0x10.
        // It sweeps the near-plane frustum from the eye towards the pivot through the world
        // (0x6057B0) and on a hit shortens *distance by the hit fraction and returns 1. Its callers
        // (0x605D60, the follow camera, after its own TraceLine 0x77F310 pivot -> eye; 0x6061D0)
        // then place the eye at the shortened distance. The cover is drawn on top of the terrain,
        // which is all that test knows, so deep snow let the camera sink in and show the cover from
        // below. After the client's test: walk the boom from the pivot out and stop it where it
        // would dip under the cover's top + clearance -- the same pull-in the terrain gets.
        constexpr uintptr_t kCameraCollide = 0x006059E0;
        using CollideFn = int(__fastcall*)(void* camera, void* edx, float* distance, const float* eye, const float* pivot, uint32_t flags);
        CollideFn g_origCollide = nullptr;

        constexpr float kStep = 0.25f;  // yd between samples along the boom (level 0's cell)
        constexpr int   kMaxSteps = 400;
        constexpr int   kRefine = 8;    // halvings between the last free and the first blocked sample

        int __fastcall hkCollide(void* camera, void* edx, float* distance, const float* eye, const float* pivot, uint32_t flags)
        {
            const float entry = distance ? *distance : 0.0f;
            const int hit = g_origCollide(camera, edx, distance, eye, pivot, flags);
            if (!g_settings.camera || !distance || !eye || !pivot || entry <= 0.0f) return hit;
            ++g_cameraStats.calls;
            const float clear = g_settings.cameraClearance;

            // Pivot (the unit's head) already under the cover: there's no free spot to pull to.
            float top;
            if (TopAt(pivot[0], pivot[1], top) && pivot[2] < top + clear) return hit;

            const float dx = eye[0] - pivot[0], dy = eye[1] - pivot[1], dz = eye[2] - pivot[2];
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 0.01f) return hit;
            auto blocked = [&](float t)
            {
                float z;
                return TopAt(pivot[0] + dx * t, pivot[1] + dy * t, z) && pivot[2] + dz * t < z + clear;
            };

            int steps = static_cast<int>(len / kStep);
            steps = steps < 1 ? 1 : (steps > kMaxSteps ? kMaxSteps : steps);
            float free = 0.0f, stop = -1.0f;
            for (int k = 1; k <= steps; ++k)
            {
                const float t = static_cast<float>(k) / steps;
                if (blocked(t)) { stop = t; break; }
                free = t;
            }
            if (stop < 0.0f) return hit;
            for (int k = 0; k < kRefine; ++k)
            {
                const float mid = 0.5f * (free + stop);
                if (blocked(mid)) stop = mid; else free = mid;
            }

            // The eye sits at `entry` along the boom, so the free share of it is the distance to keep.
            const float keep = entry * free;
            if (keep >= *distance) return hit;
            ++g_cameraStats.pulled;
            g_cameraStats.lastPull = *distance - keep;
            *distance = keep;
            return 1;
        }
    }

    void InstallCamera(const WXL_Api* api)
    {
        if (!api->HookAttach("LivingAzeroth.CoverCamera", kCameraCollide, reinterpret_cast<void*>(&hkCollide),
                             reinterpret_cast<void**>(&g_origCollide), WXL_HOOK_DEFAULT_PRIORITY))
            api->Log(WXL_LOG_WARN, kTag, "surface cover: camera collision hook failed; the camera can sink into the cover.");
    }
}
