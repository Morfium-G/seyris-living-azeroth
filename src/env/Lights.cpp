#include "Lights.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wxl_livingazeroth::lights
{
    namespace
    {
        // The M2 scene (XWorkbench: CM2Scene__GatherLights 0x81E400 walks exactly this).
        constexpr uintptr_t kScene = 0x00CD754C;     // CM2Scene*
        constexpr size_t    kSceneGeneration = 0x14; // lights whose +0x04 differs are hidden by the gather
        constexpr size_t    kSceneGlobal = 0x20;     // CM2Light* head of the global list
        constexpr size_t    kSceneGrid = 0x24;       // CM2Light** [64 * 64] chain heads
        constexpr int       kGridSize = 64;

        // CM2Light (CM2Light__ctor 0x834A40, CM2Lighting__AddLight 0x834F60, SetupGxLights 0x8353D0).
        constexpr size_t kLightGeneration = 0x04, kLightType = 0x08, kLightPos = 0x0C, kLightDir = 0x24,
                         kLightAmbient = 0x30, kLightDiffuse = 0x3C, kLightExtra = 0x48,
                         kLightAttenuation = 0x54, kLightVisible = 0x60, kLightNext = 0x68;
        constexpr int    kMaxChain = 4096; // per chain: guards against a broken (cyclic) list

        template <class T> T At(const void* base, size_t offset)
        {
            T v;
            std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(T));
            return v;
        }
        void Vec(const void* base, size_t offset, float out[3]) { std::memcpy(out, static_cast<const uint8_t*>(base) + offset, 12); }

        ClientLight Read(const void* p, bool global, int cell, uint32_t generation)
        {
            ClientLight l;
            l.address = p; l.global = global; l.cell = cell;
            l.type = At<uint32_t>(p, kLightType);
            l.visible = At<uint32_t>(p, kLightVisible) != 0;
            l.stale = At<const void*>(p, 0) != nullptr && At<uint32_t>(p, kLightGeneration) != generation;
            Vec(p, kLightPos, l.pos); Vec(p, kLightDir, l.dir);
            Vec(p, kLightAmbient, l.ambient); Vec(p, kLightDiffuse, l.diffuse); Vec(p, kLightExtra, l.extra);
            Vec(p, kLightAttenuation, l.attenuation);
            return l;
        }
    }

    Scan Gather(const float center[3], float range, std::vector<ClientLight>& out)
    {
        Scan scan;
        out.clear();
        const void* scene = *reinterpret_cast<const void* const*>(kScene);
        if (!scene) return scan;
        scan.sceneFound = true;
        const uint32_t generation = At<uint32_t>(scene, kSceneGeneration);

        auto take = [&](const ClientLight& l)
        {
            if (l.type == 1)
            {
                ++scan.pointCount;
                if (l.attenuation[0] == 0.0f && std::fabs(l.attenuation[1] - 0.7f) < 1e-4f && std::fabs(l.attenuation[2] - 0.03f) < 1e-4f)
                    ++scan.defaultAttenuation;
            }
            scan.visibleCount += l.visible;
            scan.staleCount += l.stale;
            const float dx = l.pos[0] - center[0], dy = l.pos[1] - center[1], dz = l.pos[2] - center[2];
            if (range <= 0.0f || dx * dx + dy * dy + dz * dz <= range * range) out.push_back(l);
        };
        auto walk = [&](const void* p, bool global, int cell, uint32_t& count)
        {
            for (int n = 0; p; ++n)
            {
                if (n >= kMaxChain) { scan.truncated = true; break; }
                take(Read(p, global, cell, generation));
                ++count;
                p = At<const void*>(p, kLightNext);
            }
        };

        walk(At<const void*>(scene, kSceneGlobal), true, -1, scan.globalCount);
        if (const auto* const* grid = At<const void* const*>(scene, kSceneGrid))
            for (int c = 0; c < kGridSize * kGridSize; ++c) walk(grid[c], false, c, scan.gridCount);

        std::sort(out.begin(), out.end(), [&](const ClientLight& a, const ClientLight& b)
        {
            auto d2 = [&](const ClientLight& l)
            {
                const float dx = l.pos[0] - center[0], dy = l.pos[1] - center[1], dz = l.pos[2] - center[2];
                return dx * dx + dy * dy + dz * dz;
            };
            return d2(a) < d2(b);
        });
        return scan;
    }

    float Reach(const float att[3], float share)
    {
        // f(d) = 1 / (a + b d + c d^2); find d with f(d) = share * f(1), i.e. c d^2 + b d + a = k.
        const float a = att[0], b = att[1], c = att[2];
        const float atOne = a + b + c;
        if (atOne <= 0.0f || share <= 0.0f) return -1.0f;
        const float k = atOne / share;
        if (c > 1e-6f)
        {
            const float disc = b * b - 4.0f * c * (a - k);
            return disc < 0.0f ? -1.0f : (-b + std::sqrt(disc)) / (2.0f * c);
        }
        return b > 1e-6f ? (k - a) / b : -1.0f;
    }
}
