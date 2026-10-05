#include "Lights.hpp"

#include "../features/GrassPerf.hpp"

#include "game/Camera.hpp"
#include "game/Doodad.hpp"

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

    // --- our light list ----------------------------------------------------------------------------
    namespace
    {
        namespace dd = wxl::game::doodad;

        // M2 instance and header (confirmed in-client 2026-10-05, lighting.md): light records at
        // instance +0x1D0 (0xD4 each, CM2Light at +0x68); header lights array at +0x108 (0x9C each:
        // type +0, bone +2, position +4, diffuse colour track +0x38, diffuse intensity track +0x4C).
        constexpr size_t kInstLights = 0x1D0, kLightRecord = 0xD4, kRecordLight = 0x68;
        constexpr size_t kHdrLights = 0x108, kFileLight = 0x9C;
        constexpr size_t kFileType = 0x00, kFilePos = 0x04, kFileDiffuse = 0x38, kFileIntensity = 0x4C;
        constexpr float  kChunk = 100.0f / 3.0f;
        constexpr float  kScanSeconds = 0.25f;

        template <class T> bool Get(const void* base, size_t off, T& out)
        {
            const void* p = static_cast<const uint8_t*>(base) + off;
            if (!base || !dd::detail::Readable(p, sizeof(T))) return false;
            std::memcpy(&out, p, sizeof(T));
            return true;
        }

        // The first value of an M2 track's first sequence [believed: once parsed, the nested arrays
        // hold pointers, like the header's]. Track: interpolation u16, global sequence s16,
        // timestamps (count, ptr) at +4, values (count, ptr -> per sequence (count, ptr)) at +0xC.
        bool FirstTrackValue(const uint8_t* track, void* out, size_t size)
        {
            uint32_t sequences = 0, count = 0;
            const uint8_t* perSequence = nullptr;
            const uint8_t* values = nullptr;
            if (!Get(track, 0x0C, sequences) || !Get(track, 0x10, perSequence) || !sequences || !perSequence) return false;
            if (!Get(perSequence, 0, count) || !Get(perSequence, 4, values) || !count || !values) return false;
            if (!dd::detail::Readable(values, size)) return false;
            std::memcpy(out, values, size);
            return true;
        }

        // One model light found by the scan. Position and colour are refreshed every frame from
        // its CM2Light (the client keeps animating it while in view and keeps the last values when
        // culled); the fallbacks were worked out at the scan.
        struct Source
        {
            const uint8_t* cm2 = nullptr;
            float worldPos[3] = {};     // file position through the doodad's world matrix (bone at rest)
            float fileColor[3] = {};    // first diffuse colour x first intensity from the file
            bool  haveFileColor = false;
        };
        std::vector<Source>      g_sources;
        std::vector<ActiveLight> g_active;
        Settings g_settings;
        Stats    g_stats;
        float    g_scanTime = kScanSeconds;

        void Mul(const float v[3], const float m[16], float out[3])
        {
            for (int c = 0; c < 3; ++c) out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c];
        }

        void ScanDoodads(const float center[3])
        {
            const double t0 = grassperf::Now();
            g_sources.clear();
            Stats st;
            // The terrain chunks within range, each once; their doodads, each once.
            const int reach = static_cast<int>(std::ceil(g_settings.range / kChunk));
            std::vector<void*> chunks, doodads;
            void* found[1024];
            for (int gy = -reach; gy <= reach; ++gy)
                for (int gx = -reach; gx <= reach; ++gx)
                {
                    float q[3] = { center[0] + gx * kChunk, center[1] + gy * kChunk, center[2] };
                    void* chunk = dd::ChunkAt(q);
                    if (!dd::detail::Plausible(chunk) || std::find(chunks.begin(), chunks.end(), chunk) != chunks.end()) continue;
                    chunks.push_back(chunk);
                    const int n = dd::EnumerateChunk(chunk, found, 0, 1024);
                    for (int k = 0; k < n; ++k)
                        if (std::find(doodads.begin(), doodads.end(), found[k]) == doodads.end()) doodads.push_back(found[k]);
                }
            st.chunks = static_cast<unsigned>(chunks.size());
            st.doodads = static_cast<unsigned>(doodads.size());

            for (void* d : doodads)
            {
                void* inst = dd::Instance(d);
                void* model = inst ? dd::detail::P(inst, dd::off::kInstModel) : nullptr;
                const uint8_t* hdr = model ? static_cast<const uint8_t*>(dd::detail::P(model, dd::off::kModelHeader)) : nullptr;
                uint32_t count = 0;
                const uint8_t* fileLights = nullptr;
                const uint8_t* records = nullptr;
                if (!hdr || !Get(hdr, kHdrLights, count) || !count || count > 64 || !Get(hdr, kHdrLights + 4, fileLights) || !fileLights) continue;
                Get(inst, kInstLights, records);
                float world[16];
                const bool haveWorld = dd::WorldMatrix(d, world);
                for (uint32_t l = 0; l < count; ++l)
                {
                    const uint8_t* fl = fileLights + l * kFileLight;
                    uint16_t type = 0;
                    float local[3];
                    if (!Get(fl, kFileType, type) || type != 1 || !Get(fl, kFilePos, local)) continue; // point lights only
                    ++st.modelLights;
                    Source s;
                    s.cm2 = records ? records + l * kLightRecord + kRecordLight : nullptr;
                    if (haveWorld) Mul(local, world, s.worldPos);
                    float color[3], intensity = 1.0f;
                    if (FirstTrackValue(fl + kFileDiffuse, color, sizeof(color)))
                    {
                        FirstTrackValue(fl + kFileIntensity, &intensity, sizeof(intensity));
                        for (int c = 0; c < 3; ++c) s.fileColor[c] = color[c] * intensity;
                        s.haveFileColor = true;
                    }
                    if (haveWorld || s.cm2) g_sources.push_back(s);
                }
            }
            st.scanMs = grassperf::Now() - t0;
            g_stats.chunks = st.chunks; g_stats.doodads = st.doodads; g_stats.modelLights = st.modelLights; g_stats.scanMs = st.scanMs;
        }

        struct Candidate { float pos[3]; float color[3]; float weight; };

        double g_time = 0.0;

        // Flame flicker, 0..1: three sines at unrelated rates (no visible repeat), each light its own
        // phase from its position so neighbouring torches don't pulse together.
        float Flicker(const float pos[3], double time)
        {
            const double phase = std::fmod(std::fabs(pos[0] * 12.9898 + pos[1] * 78.233 + pos[2] * 37.719), 1000.0);
            const double t = time + phase;
            const double s = 0.5 * std::sin(t * 7.13) + 0.3 * std::sin(t * 13.71 + 1.3) + 0.2 * std::sin(t * 23.17 + 2.1);
            return static_cast<float>(0.5 + 0.5 * s);
        }
    }

    Settings& Config() { return g_settings; }
    const std::vector<ActiveLight>& Active() { return g_active; }
    Stats GetStats() { return g_stats; }

    void Update(float dt, const world::Snapshot& snap)
    {
        g_active.clear();
        if (!snap.inWorld || !g_settings.enabled) { g_sources.clear(); return; }
        float eye[3] = { snap.playerPos[0], snap.playerPos[1], snap.playerPos[2] };
        wxl::game::camera::GetPosition(eye);

        g_scanTime += dt;
        if (g_scanTime >= kScanSeconds) { g_scanTime = 0.0f; ScanDoodads(eye); }
        g_time += dt * g_settings.flickerSpeed;

        // This frame's lights: the client's current values where it has them.
        std::vector<Candidate> cands;
        unsigned fromClient = 0, fromWorld = 0, fileColor = 0;
        const float range = g_settings.range;
        for (const Source& s : g_sources)
        {
            Candidate c{};
            float stored[3] = {}, color[3] = {};
            const bool placed = s.cm2 && Get(s.cm2, 0x0C, stored) && (stored[0] != 0.0f || stored[1] != 0.0f || stored[2] != 0.0f);
            std::memcpy(c.pos, placed ? stored : s.worldPos, sizeof(c.pos));
            (placed ? fromClient : fromWorld)++;
            const bool lit = s.cm2 && Get(s.cm2, 0x3C, color) && (color[0] > 0.0f || color[1] > 0.0f || color[2] > 0.0f);
            if (!lit)
            {
                if (!s.haveFileColor) continue;
                std::memcpy(color, s.fileColor, sizeof(color));
                ++fileColor;
            }
            std::memcpy(c.color, color, sizeof(c.color));
            const float dx = c.pos[0] - eye[0], dy = c.pos[1] - eye[1], dz = c.pos[2] - eye[2];
            if (dx * dx + dy * dy + dz * dz > range * range) continue;
            c.weight = 1.0f;
            cands.push_back(c);
        }
        g_stats.fromClient = fromClient; g_stats.fromWorldMatrix = fromWorld; g_stats.fileColor = fileColor;
        g_stats.inRange = static_cast<unsigned>(cands.size());

        // Merge lights closer than the merge distance (torch groups): summed colour, position
        // weighted by brightness.
        unsigned merged = 0;
        const float md2 = g_settings.mergeDistance * g_settings.mergeDistance;
        for (size_t a = 0; a < cands.size(); ++a)
        {
            if (cands[a].weight <= 0.0f) continue;
            float wa = cands[a].color[0] + cands[a].color[1] + cands[a].color[2];
            for (size_t b = a + 1; b < cands.size(); ++b)
            {
                if (cands[b].weight <= 0.0f) continue;
                const float dx = cands[a].pos[0] - cands[b].pos[0], dy = cands[a].pos[1] - cands[b].pos[1], dz = cands[a].pos[2] - cands[b].pos[2];
                if (dx * dx + dy * dy + dz * dz > md2) continue;
                const float wb = cands[b].color[0] + cands[b].color[1] + cands[b].color[2];
                const float share = wa + wb > 0.0f ? wb / (wa + wb) : 0.5f;
                for (int k = 0; k < 3; ++k)
                {
                    cands[a].pos[k] += (cands[b].pos[k] - cands[a].pos[k]) * share;
                    cands[a].color[k] += cands[b].color[k];
                }
                wa += wb;
                cands[b].weight = 0.0f;
                ++merged;
            }
        }
        g_stats.merged = merged;

        // The nearest to the camera, fading out over the last third of the range.
        std::vector<std::pair<float, size_t>> order;
        for (size_t k = 0; k < cands.size(); ++k)
            if (cands[k].weight > 0.0f)
            {
                const float dx = cands[k].pos[0] - eye[0], dy = cands[k].pos[1] - eye[1], dz = cands[k].pos[2] - eye[2];
                order.push_back({ dx * dx + dy * dy + dz * dz, k });
            }
        std::sort(order.begin(), order.end());
        const float fadeStart = range * (2.0f / 3.0f);
        for (size_t n = 0; n < order.size() && n < static_cast<size_t>(kMaxLights); ++n)
        {
            const Candidate& c = cands[order[n].second];
            const float d = std::sqrt(order[n].first);
            const float fade = d <= fadeStart ? 1.0f : std::max(0.0f, 1.0f - (d - fadeStart) / (range - fadeStart));
            const float flicker = 1.0f - g_settings.flicker * Flicker(c.pos, g_time);
            ActiveLight l{};
            std::memcpy(l.pos, c.pos, sizeof(l.pos));
            for (int k = 0; k < 3; ++k) l.color[k] = c.color[k] * g_settings.brightness * fade * flicker;
            l.radius = g_settings.radius;
            g_active.push_back(l);
        }
        g_stats.active = static_cast<unsigned>(g_active.size());
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
