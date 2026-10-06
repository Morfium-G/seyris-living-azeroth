#include "Lights.hpp"

#include "../features/DoodadLightTable.hpp"
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

        // One light found by the scan. A model's own light (cm2 set): position and colour refreshed every
        // frame from its CM2Light (the client keeps animating it while in view and keeps the last values
        // when culled), the fallbacks worked out at the scan. A table light (DoodadLightAssignment): all
        // fixed at the scan (static doodads, bone at rest).
        struct Source
        {
            const uint8_t* cm2 = nullptr;
            float worldPos[3] = {};     // file position through the doodad's world matrix (bone at rest)
            float fileColor[3] = {};    // first diffuse colour x first intensity from the file
            bool  haveFileColor = false;
            // Table lights only:
            bool  table = false;
            float color[3] = {};
            float radius = -1.0f, falloff = -1.0f;
            float spotDir[3] = { 0.0f, 0.0f, -1.0f };
            float cosOuter = -2.0f, spotScale = 1.0f;
            int   flickerMode = lighttable::kFlickerDefault;
            float flickerSpeed = -1.0f, flickerAmount = -1.0f;
            float scale = 1.0f;         // the model's: the radius scales with it
        };
        std::vector<Source>      g_sources;
        std::vector<ActiveLight> g_active;
        Settings g_settings;
        Stats    g_stats;
        float    g_scanTime = kScanSeconds;
        uint32_t g_tableGeneration = 0;

        void Mul(const float v[3], const float m[16], float out[3])
        {
            for (int c = 0; c < 3; ++c) out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c];
        }
        void MulDir(const float v[3], const float m[16], float out[3])
        {
            for (int c = 0; c < 3; ++c) out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c];
            const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
            if (len > 1e-6f) for (int c = 0; c < 3; ++c) out[c] /= len;
            else { out[0] = 0.0f; out[1] = 0.0f; out[2] = -1.0f; }
        }

        float MatrixScale(const float m[16]) { return std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]); }

        // A table light on one placed doodad.
        void AddTableLight(const lighttable::Assignment& a, const uint8_t* hdr, const float world[16], Stats& st)
        {
            const lighttable::Properties* p = lighttable::FindProperties(a.lightId);
            if (!p) return;
            float local[3];
            if (!AttachmentPosition(hdr, a.attachType, a.attachIndex, a.offset, local)) ++st.attachFallbacks;
            Source s;
            s.table = true;
            s.scale = MatrixScale(world);
            Mul(local, world, s.worldPos);
            const float inten = p->intensity;
            s.color[0] = ((p->color >> 16) & 0xFF) / 255.0f * inten;
            s.color[1] = ((p->color >> 8) & 0xFF) / 255.0f * inten;
            s.color[2] = (p->color & 0xFF) / 255.0f * inten;
            s.radius = p->radius; s.falloff = p->falloff;
            if (p->type == lighttable::kSpot)
            {
                MulDir(a.direction, world, s.spotDir);
                constexpr float kRad = 3.14159265f / 180.0f;
                const float inner = std::cos(std::min(p->innerAngle, p->outerAngle) * kRad);
                s.cosOuter = std::cos(p->outerAngle * kRad);
                s.spotScale = inner - s.cosOuter > 1e-4f ? 1.0f / (inner - s.cosOuter) : 1e4f;
            }
            s.flickerMode = p->flickerMode; s.flickerSpeed = p->flickerSpeed; s.flickerAmount = p->flickerAmount;
            g_sources.push_back(s);
            ++st.tableLights;
        }

        void ScanModels(const float center[3])
        {
            const double t0 = grassperf::Now();
            g_sources.clear();
            Stats st;
            static std::vector<SceneModel> models;
            st.models = SceneModels(center, g_settings.range, models);
            st.modelsInRange = static_cast<unsigned>(models.size());
            const bool haveTable = !lighttable::AllAssignments().empty();

            for (const SceneModel& m : models)
            {
                if (!m.header) continue;
                const uint8_t* hdr = m.header;

                // Table rows for this model: its lights, and whether its own are suppressed.
                bool suppress = false;
                if (haveTable)
                    for (const lighttable::Assignment* a : lighttable::ForModel(lighttable::Normalize(m.path)))
                    {
                        if (a->flags & lighttable::kSuppressModelLights) suppress = true;
                        if (a->lightId) AddTableLight(*a, hdr, m.world, st);
                    }
                if (suppress) { ++st.suppressedModels; continue; }

                // The model's own lights.
                uint32_t count = 0;
                const uint8_t* fileLights = nullptr;
                const uint8_t* records = nullptr;
                if (!Get(hdr, kHdrLights, count) || !count || count > 64 || !Get(hdr, kHdrLights + 4, fileLights) || !fileLights) continue;
                Get(m.model, kInstLights, records);
                for (uint32_t l = 0; l < count; ++l)
                {
                    const uint8_t* fl = fileLights + l * kFileLight;
                    uint16_t type = 0;
                    float local[3];
                    if (!Get(fl, kFileType, type) || type != 1 || !Get(fl, kFilePos, local)) continue; // point lights only
                    ++st.modelLights;
                    Source s;
                    s.cm2 = records ? records + l * kLightRecord + kRecordLight : nullptr;
                    Mul(local, m.world, s.worldPos);
                    s.scale = MatrixScale(m.world);
                    float color[3], intensity = 1.0f;
                    if (FirstTrackValue(fl + kFileDiffuse, color, sizeof(color)))
                    {
                        FirstTrackValue(fl + kFileIntensity, &intensity, sizeof(intensity));
                        for (int c = 0; c < 3; ++c) s.fileColor[c] = color[c] * intensity;
                        s.haveFileColor = true;
                    }
                    g_sources.push_back(s);
                }
            }
            st.scanMs = grassperf::Now() - t0;
            g_stats.models = st.models; g_stats.modelsInRange = st.modelsInRange; g_stats.modelLights = st.modelLights; g_stats.scanMs = st.scanMs;
            g_stats.tableLights = st.tableLights; g_stats.suppressedModels = st.suppressedModels; g_stats.attachFallbacks = st.attachFallbacks;
        }

        // A light this frame, before merging and choosing.
        struct Candidate
        {
            float pos[3]; float color[3]; float weight;
            float radius, falloff;
            float spotDir[3]; float cosOuter, spotScale;
            int   flickerMode; float flickerSpeed, flickerAmount;
            float scale;
        };

        double g_time = 0.0;

        uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }
        float    Unit(uint32_t x) { return static_cast<float>(Hash(x) >> 8) * (1.0f / 16777216.0f); }

        // Flame flicker, 0..1 (how much it dips), each light its own phase from its position so
        // neighbouring torches don't pulse together. Smooth: three sines at unrelated rates (no visible
        // repeat). Noise: random levels ~6 a second, eased into each other. Steps: the same levels, held.
        float Flicker(int mode, const float pos[3], double time)
        {
            const double phase = std::fmod(std::fabs(pos[0] * 12.9898 + pos[1] * 78.233 + pos[2] * 37.719), 1000.0);
            const double t = time + phase;
            if (mode == lighttable::kFlickerOff) return 0.0f;
            if (mode == lighttable::kFlickerNoise || mode == lighttable::kFlickerSteps)
            {
                const double u = t * 6.0;
                const uint32_t seed = static_cast<uint32_t>(phase * 1000.0);
                const uint32_t k = static_cast<uint32_t>(static_cast<int64_t>(std::floor(u)));
                const float a = Unit(k * 2654435761u + seed), b = Unit((k + 1) * 2654435761u + seed);
                if (mode == lighttable::kFlickerSteps) return a;
                float f = static_cast<float>(u - std::floor(u));
                f = f * f * (3.0f - 2.0f * f);
                return a + (b - a) * f;
            }
            const double s = 0.5 * std::sin(t * 7.13) + 0.3 * std::sin(t * 13.71 + 1.3) + 0.2 * std::sin(t * 23.17 + 2.1);
            return static_cast<float>(0.5 + 0.5 * s);
        }
    }

    bool AttachmentPosition(const void* header, uint32_t attachType, int32_t attachIndex, const float offset[3], float out[3])
    {
        // Header arrays (count, pointer once parsed; core M2Format): bones +0x2C (0x58 each, pivot +0x4C),
        // attachments +0xF0 (0x28 each: id +0, bone +4, position +8; the layout plausible in-client
        // 2026-10-05), particle emitters +0x128 (0x1DC each, position +8, bone +0x14 confirmed).
        const auto* hdr = static_cast<const uint8_t*>(header);
        out[0] = offset[0]; out[1] = offset[1]; out[2] = offset[2];
        if (!hdr || attachType == lighttable::kOrigin) return attachType == lighttable::kOrigin;
        uint32_t count = 0;
        const uint8_t* items = nullptr;
        float at[3];
        if (attachType == lighttable::kAttachmentPoint)
        {
            if (!Get(hdr, 0xF0, count) || !Get(hdr, 0xF4, items) || !items) return false;
            for (uint32_t k = 0; k < count && k < 256; ++k)
            {
                uint32_t id = 0;
                if (Get(items + k * 0x28, 0, id) && id == static_cast<uint32_t>(attachIndex) && Get(items + k * 0x28, 8, at))
                { for (int c = 0; c < 3; ++c) out[c] += at[c]; return true; }
            }
            return false;
        }
        const size_t arrayAt = attachType == lighttable::kBone ? 0x2C : (attachType == lighttable::kParticleEmitter ? 0x128 : 0);
        const size_t stride = attachType == lighttable::kBone ? 0x58 : 0x1DC;
        const size_t field = attachType == lighttable::kBone ? 0x4C : 0x08;
        if (!arrayAt || attachIndex < 0 || !Get(hdr, arrayAt, count) || !Get(hdr, arrayAt + 4, items) || !items) return false;
        if (static_cast<uint32_t>(attachIndex) >= count || !Get(items + attachIndex * stride, field, at)) return false;
        for (int c = 0; c < 3; ++c) out[c] += at[c];
        return true;
    }

    unsigned SceneModels(const float center[3], float range, std::vector<SceneModel>& out)
    {
        // The M2 scene's model list (CM2Model_AttachToScene 0x834540): head at scene +0x08, next at
        // model +0x0C. Model: shared model at +0x2C (path +0x3C, header +0x150; core's doodad SDK),
        // world matrix at +0xB4.
        constexpr size_t kSceneModelHead = 0x08, kModelNext = 0x0C, kModelWorld = 0xB4;
        constexpr unsigned kMaxModels = 100000; // a broken (cyclic) list stops here
        out.clear();
        const void* scene = *reinterpret_cast<const void* const*>(kScene);
        const uint8_t* m = nullptr;
        if (!scene || !Get(scene, kSceneModelHead, m)) return 0;
        unsigned total = 0;
        for (; m && total < kMaxModels; ++total)
        {
            SceneModel sm;
            sm.model = m;
            if (Get(m, kModelWorld, sm.world))
            {
                const float dx = sm.world[12] - center[0], dy = sm.world[13] - center[1], dz = sm.world[14] - center[2];
                sm.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (sm.distance <= range)
                {
                    const uint8_t* shared = nullptr;
                    if (Get(m, dd::off::kInstModel, shared) && shared)
                    {
                        Get(shared, dd::off::kModelHeader, sm.header);
                        const char* path = static_cast<const dd::off::M2ModelCache*>(static_cast<const void*>(shared))->fullPath;
                        if (dd::detail::Readable(path, 1)) sm.path = path;
                    }
                    out.push_back(sm);
                }
            }
            const uint8_t* next = nullptr;
            if (!Get(m, kModelNext, next)) break;
            m = next;
        }
        std::sort(out.begin(), out.end(), [](const SceneModel& a, const SceneModel& b) { return a.distance < b.distance; });
        return total;
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

        // Rescan a few times a second, and at once when the tables changed (editor, reload).
        g_scanTime += dt;
        if (g_scanTime >= kScanSeconds || g_tableGeneration != lighttable::Generation())
        {
            g_scanTime = 0.0f;
            g_tableGeneration = lighttable::Generation();
            ScanModels(eye);
        }
        g_time += dt;

        // This frame's lights: the client's current values where it has them.
        std::vector<Candidate> cands;
        unsigned fromClient = 0, fromWorld = 0, fileColor = 0;
        const float range = g_settings.range;
        for (const Source& s : g_sources)
        {
            Candidate c{};
            c.radius = s.radius; c.falloff = s.falloff;
            std::memcpy(c.spotDir, s.spotDir, sizeof(c.spotDir));
            c.cosOuter = s.cosOuter; c.spotScale = s.spotScale;
            c.flickerMode = s.flickerMode; c.flickerSpeed = s.flickerSpeed; c.flickerAmount = s.flickerAmount;
            c.scale = s.scale > 0.0f ? s.scale : 1.0f;
            if (s.table)
            {
                std::memcpy(c.pos, s.worldPos, sizeof(c.pos));
                std::memcpy(c.color, s.color, sizeof(c.color));
            }
            else
            {
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
            }
            const float dx = c.pos[0] - eye[0], dy = c.pos[1] - eye[1], dz = c.pos[2] - eye[2];
            if (dx * dx + dy * dy + dz * dz > range * range) continue;
            c.weight = 1.0f;
            cands.push_back(c);
        }
        g_stats.fromClient = fromClient; g_stats.fromWorldMatrix = fromWorld; g_stats.fileColor = fileColor;
        g_stats.inRange = static_cast<unsigned>(cands.size());

        // Merge point lights closer than the merge distance (torch groups): summed colour, position
        // weighted by brightness, the larger radius. Spot lights never merge (their cones differ).
        unsigned merged = 0;
        const float md2 = g_settings.mergeDistance * g_settings.mergeDistance;
        for (size_t a = 0; a < cands.size(); ++a)
        {
            if (cands[a].weight <= 0.0f || cands[a].cosOuter > -1.5f) continue;
            float wa = cands[a].color[0] + cands[a].color[1] + cands[a].color[2];
            for (size_t b = a + 1; b < cands.size(); ++b)
            {
                if (cands[b].weight <= 0.0f || cands[b].cosOuter > -1.5f) continue;
                const float dx = cands[a].pos[0] - cands[b].pos[0], dy = cands[a].pos[1] - cands[b].pos[1], dz = cands[a].pos[2] - cands[b].pos[2];
                if (dx * dx + dy * dy + dz * dz > md2) continue;
                const float wb = cands[b].color[0] + cands[b].color[1] + cands[b].color[2];
                const float share = wa + wb > 0.0f ? wb / (wa + wb) : 0.5f;
                for (int k = 0; k < 3; ++k)
                {
                    cands[a].pos[k] += (cands[b].pos[k] - cands[a].pos[k]) * share;
                    cands[a].color[k] += cands[b].color[k];
                }
                cands[a].radius = std::max(cands[a].radius, cands[b].radius);
                cands[a].scale = std::max(cands[a].scale, cands[b].scale);
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
            // -1 = the panel's defaults.
            const int   mode = c.flickerMode == lighttable::kFlickerDefault ? lighttable::kFlickerSmooth : c.flickerMode;
            const float amount = c.flickerAmount >= 0.0f ? c.flickerAmount : g_settings.flicker;
            const float speed = c.flickerSpeed >= 0.0f ? c.flickerSpeed : 1.0f;
            const float flicker = 1.0f - amount * Flicker(mode, c.pos, g_time * speed * g_settings.flickerSpeed);
            ActiveLight l{};
            std::memcpy(l.pos, c.pos, sizeof(l.pos));
            for (int k = 0; k < 3; ++k) l.color[k] = c.color[k] * g_settings.brightness * fade * flicker;
            l.radius = (c.radius > 0.0f ? c.radius : g_settings.radius) * c.scale; // a scaled-up fire reaches further
            l.falloff = c.falloff > 0.0f ? c.falloff : 1.0f;
            std::memcpy(l.spotDir, c.spotDir, sizeof(l.spotDir));
            l.cosOuter = c.cosOuter; l.spotScale = c.spotScale;
            l.dip = 1.0f - flicker;
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
