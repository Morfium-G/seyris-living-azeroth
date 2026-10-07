#include "Lights.hpp"

#include "../features/DoodadLightTable.hpp"
#include "../features/GrassPerf.hpp"

#include "game/Camera.hpp"
#include "game/Doodad.hpp"
#include "game/Pick.hpp"
#include "offsets/game/WMO.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

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

        // Guarded by an exception handler instead of a VirtualQuery per read: the scan walks every model
        // the scene holds (tens of thousands in a city), and dd::detail::Readable's small region cache
        // (4 regions, reset every millisecond) turned most reads into a kernel call there: the
        // owner's profiler showed one scan at 222 ms in Dalaran (2026-10-08). The pointers come from
        // the client's own lists; a bad one still fails cleanly.
        bool ReadGuarded(const void* p, void* out, size_t n)
        {
            if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
            __try
            {
                std::memcpy(out, p, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        template <class T> bool Get(const void* base, size_t off, T& out)
        {
            return base && ReadGuarded(static_cast<const uint8_t*>(base) + off, &out, sizeof(T));
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
            return ReadGuarded(values, out, size);
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
            // Attached models (a torch in a hand): the model, and the light in its own space, so the
            // light follows it every frame instead of only at the scan.
            const void* follow = nullptr;
            float localPos[3] = {}, localDir[3] = { 0.0f, 0.0f, -1.0f };
            const void* model = nullptr;  // the instance it belongs to
            bool  bake = false;           // its DoodadLightProperties ask for baked occlusion
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

        // A table light on one placed doodad (or attached model: `follow`).
        void AddTableLight(const lighttable::Assignment& a, const uint8_t* hdr, const float world[16], const void* model, const void* follow, Stats& st)
        {
            const lighttable::Properties* p = lighttable::FindProperties(a.lightId);
            if (!p) return;
            float local[3];
            if (!AttachmentPosition(hdr, a.attachType, a.attachIndex, a.offset, local)) ++st.attachFallbacks;
            Source s;
            s.table = true;
            s.follow = follow;
            s.model = model;
            s.bake = (p->flags & lighttable::kBakeOcclusion) != 0;
            std::memcpy(s.localPos, local, sizeof(local));
            std::memcpy(s.localDir, a.direction, sizeof(s.localDir));
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

        // DoodadLightAssignment rows per model file, keyed by the file's path pointer (shared by every
        // instance of that file, so a city's hundreds of identical lamps look it up once); the path's
        // text is kept to notice a pointer reused by another file. Rebuilt when the tables change.
        struct RowCache { std::string path; uint32_t generation = 0; std::vector<const lighttable::Assignment*> rows; };
        std::unordered_map<const char*, RowCache> g_rowCache;

        const std::vector<const lighttable::Assignment*>& RowsFor(const char* path)
        {
            if (g_rowCache.size() > 8192) g_rowCache.clear();
            const uint32_t generation = lighttable::Generation();
            RowCache& rc = g_rowCache[path];
            if (rc.generation != generation || rc.path != path)
            {
                rc.path = path;
                rc.generation = generation;
                rc.rows = lighttable::ForModel(lighttable::Normalize(rc.path));
            }
            return rc.rows;
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
                if (haveTable && m.path[0])
                    for (const lighttable::Assignment* a : RowsFor(m.path))
                    {
                        if (a->flags & lighttable::kSuppressModelLights) suppress = true;
                        if (a->lightId) AddTableLight(*a, hdr, m.world, m.model, m.parent ? m.model : nullptr, st);
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
                    s.follow = m.parent ? m.model : nullptr;
                    s.model = m.model;
                    std::memcpy(s.localPos, local, sizeof(local));
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
            unsigned attached = 0, atRest = 0;
            for (const SceneModel& m : models) if (m.parent) { ++attached; atRest += m.animated ? 0 : 1; }
            g_stats.attachedModels = attached; g_stats.attachedAtRest = atRest;
        }

        // A light this frame, before merging and choosing.
        struct Candidate
        {
            float pos[3]; float color[3]; float weight;
            float radius, falloff;
            float spotDir[3]; float cosOuter, spotScale;
            int   flickerMode; float flickerSpeed, flickerAmount;
            float scale;
            const void* model;
            bool  bake;     // wants baked occlusion
            bool  moving;   // attached (follows a model): never baked
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

        // --- the cell grid (clustered list) ---------------------------------------------------------
        Grid g_grid;
        std::vector<float> g_baseLum;   // per active light: its brightness before fade and flicker (ranks it in full cells)

        struct CellEntry { uint32_t cell; float score; uint16_t light; };

        // The grid snaps to whole cells around the camera, so a cell only changes when lights do.
        // A light goes into every cell its radius reaches (circle vs square). A cell holding more than
        // perCell keeps the strongest at its centre: brightness x (1 - d / reach)^2 with the light's
        // base brightness (no flicker or fade, so the choice doesn't flip from frame to frame).
        void BuildGrid(const float eye[3])
        {
            Grid next;
            next.cellSize = std::max(4.0f, g_settings.cellSize);
            next.perCell = std::max(1, std::min(g_settings.maxPerCell, kMaxPerCellCap));
            const int half = std::min(static_cast<int>(std::ceil(g_settings.range / next.cellSize)), kMaxCells / 2);
            next.cells = std::max(1, half * 2);
            next.originX = (std::floor(eye[0] / next.cellSize) - half) * next.cellSize;
            next.originY = (std::floor(eye[1] / next.cellSize) - half) * next.cellSize;
            const size_t cellCount = static_cast<size_t>(next.cells) * next.cells;
            next.count.assign(cellCount, 0);
            next.index.assign(cellCount * next.perCell, 0);

            static std::vector<CellEntry> entries;
            entries.clear();
            const float cs = next.cellSize, halfDiag = cs * 0.70710678f;
            for (size_t i = 0; i < g_active.size(); ++i)
            {
                const ActiveLight& l = g_active[i];
                const float r = l.radius;
                if (r <= 0.0f) continue;
                const float lx = l.pos[0] - next.originX, ly = l.pos[1] - next.originY;
                const int x0 = std::max(0, static_cast<int>(std::floor((lx - r) / cs)));
                const int y0 = std::max(0, static_cast<int>(std::floor((ly - r) / cs)));
                const int x1 = std::min(next.cells - 1, static_cast<int>(std::floor((lx + r) / cs)));
                const int y1 = std::min(next.cells - 1, static_cast<int>(std::floor((ly + r) / cs)));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        const float nx = std::max(x * cs, std::min(lx, (x + 1) * cs)) - lx;
                        const float ny = std::max(y * cs, std::min(ly, (y + 1) * cs)) - ly;
                        if (nx * nx + ny * ny > r * r) continue;
                        const float cx = (x + 0.5f) * cs - lx, cy = (y + 0.5f) * cs - ly;
                        const float t = 1.0f - std::min(std::sqrt(cx * cx + cy * cy) / (r + halfDiag), 1.0f);
                        entries.push_back({ static_cast<uint32_t>(y * next.cells + x), g_baseLum[i] * t * t, static_cast<uint16_t>(i) });
                    }
            }
            std::sort(entries.begin(), entries.end(), [](const CellEntry& a, const CellEntry& b)
                      { return a.cell != b.cell ? a.cell < b.cell : (a.score != b.score ? a.score > b.score : a.light < b.light); });

            unsigned busiest = 0, full = 0, dropped = 0;
            for (size_t e = 0; e < entries.size();)
            {
                const uint32_t c = entries[e].cell;
                unsigned n = 0;
                for (; e < entries.size() && entries[e].cell == c; ++e, ++n)
                    if (n < static_cast<unsigned>(next.perCell)) next.index[c * next.perCell + n] = entries[e].light;
                busiest = std::max(busiest, n);
                if (n > static_cast<unsigned>(next.perCell)) { ++full; dropped += n - next.perCell; }
                next.count[c] = static_cast<uint8_t>(std::min(n, static_cast<unsigned>(next.perCell)));
            }
            g_stats.busiestCell = busiest; g_stats.fullCells = full; g_stats.droppedFromCells = dropped;

            const bool same = next.cells == g_grid.cells && next.perCell == g_grid.perCell && next.originX == g_grid.originX &&
                              next.originY == g_grid.originY && next.cellSize == g_grid.cellSize &&
                              next.count == g_grid.count && next.index == g_grid.index;
            next.generation = same ? g_grid.generation : g_grid.generation + 1;
            g_grid = std::move(next);
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

    namespace
    {
        // Instance fields (core offsets/game/M2.hpp): parent +0x48, the attachment it hangs on +0x54
        // (an index into the parent's attachments, 0xFFFF = none), first attached child +0x58, next
        // sibling +0x60, scene +0x28, last animated scene frame +0x3C, placement (model -> world, roots)
        // +0xB4, model -> view root +0xF4. Scene: current frame +0x14.
        constexpr size_t kInstScene = 0x28, kInstLastAnim = 0x3C, kInstParent = 0x48, kInstAttachSlot = 0x54;
        constexpr size_t kInstChildHead = 0x58, kInstChildNext = 0x60, kInstPlacement = 0xB4, kInstViewRoot = 0xF4;
        constexpr size_t kSceneFrame = 0x14;
        constexpr int    kMaxDepth = 4;          // weapon on a mount's rider is about as deep as it goes
        constexpr unsigned kMaxChildren = 64;    // per parent: a broken chain stops here

        // view -> camera-relative world (the inverse of the captured view), and the camera then.
        float g_viewInv[16] = {};
        float g_viewCamera[3] = {};
        bool  g_viewValid = false;

        void Mul4(const float a[16], const float b[16], float out[16])
        {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
        }

        bool ParentWorldAtRest(const uint8_t* inst, float out[16], int depth);

        bool WorldOf(const uint8_t* inst, float out[16], bool& animated, int depth)
        {
            const uint8_t* parent = nullptr;
            if (!Get(inst, kInstParent, parent)) return false;
            if (!parent) { animated = true; return Get(inst, kInstPlacement, *reinterpret_cast<float(*)[16]>(out)); }
            if (depth > kMaxDepth) return false;
            // Animated lately: its own view root, back to world.
            const uint8_t* scene = nullptr;
            uint32_t frame = 0, last = 0;
            float root[16];
            if (g_viewValid && Get(inst, kInstScene, scene) && scene && Get(scene, kSceneFrame, frame) && Get(inst, kInstLastAnim, last) &&
                frame - last <= 2u && Get(inst, kInstViewRoot, root))
            {
                Mul4(root, g_viewInv, out);
                for (int c = 0; c < 3; ++c) out[12 + c] += g_viewCamera[c];
                animated = true;
                return true;
            }
            animated = false;
            return ParentWorldAtRest(inst, out, depth);
        }

        // The parent's world matrix moved to the attachment the instance hangs on (rest position; the
        // attachment's bone isn't animated here).
        bool ParentWorldAtRest(const uint8_t* inst, float out[16], int depth)
        {
            const uint8_t* parent = nullptr;
            uint32_t slot = 0xFFFF;
            bool parentAnimated = false;
            if (!Get(inst, kInstParent, parent) || !parent || !WorldOf(parent, out, parentAnimated, depth + 1)) return false;
            Get(inst, kInstAttachSlot, slot);
            const uint8_t* shared = nullptr;
            const uint8_t* hdr = nullptr;
            uint32_t count = 0;
            const uint8_t* items = nullptr;
            float at[3];
            if (slot != 0xFFFF && Get(parent, dd::off::kInstModel, shared) && shared && Get(shared, dd::off::kModelHeader, hdr) && hdr &&
                Get(hdr, 0xF0, count) && slot < count && Get(hdr, 0xF4, items) && items && Get(items + slot * 0x28, 8, at))
            {
                float moved[3];
                Mul(at, out, moved);
                for (int c = 0; c < 3; ++c) out[12 + c] = moved[c];
            }
            return true;
        }

        void Describe(const uint8_t* m, SceneModel& sm)
        {
            const uint8_t* shared = nullptr;
            if (Get(m, dd::off::kInstModel, shared) && shared)
            {
                Get(shared, dd::off::kModelHeader, sm.header);
                const char* path = static_cast<const dd::off::M2ModelCache*>(static_cast<const void*>(shared))->fullPath;
                char first = 0;
                if (ReadGuarded(path, &first, 1)) sm.path = path;
            }
        }

        void AddChildren(const uint8_t* parent, const float center[3], float range, std::vector<SceneModel>& out, int depth, unsigned& attached)
        {
            if (depth > kMaxDepth) return;
            const uint8_t* c = nullptr;
            if (!Get(parent, kInstChildHead, c)) return;
            for (unsigned n = 0; c && n < kMaxChildren; ++n)
            {
                SceneModel sm;
                sm.model = c;
                sm.parent = parent;
                if (WorldOf(c, sm.world, sm.animated, depth))
                {
                    const float dx = sm.world[12] - center[0], dy = sm.world[13] - center[1], dz = sm.world[14] - center[2];
                    sm.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (sm.distance <= range) { Describe(c, sm); out.push_back(sm); ++attached; }
                }
                AddChildren(c, center, range, out, depth + 1, attached);
                const uint8_t* next = nullptr;
                if (!Get(c, kInstChildNext, next)) break;
                c = next;
            }
        }

        unsigned g_lastAttached = 0;
    }

    void NoteSceneView(const float view[16], const float camera[3])
    {
        // Affine, row-vector: view = rel * R + T, so rel = (view - T) * R^-1.
        const float* v = view;
        const float a = v[0], b = v[1], c = v[2], d = v[4], e = v[5], f = v[6], g = v[8], h = v[9], i = v[10];
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        if (std::fabs(det) < 1e-12f) { g_viewValid = false; return; }
        const float k = 1.0f / det;
        float* o = g_viewInv;
        o[0] = A * k;                 o[1] = -(b * i - c * h) * k;  o[2] = (b * f - c * e) * k;   o[3] = 0.0f;
        o[4] = B * k;                 o[5] = (a * i - c * g) * k;   o[6] = -(a * f - c * d) * k;  o[7] = 0.0f;
        o[8] = C * k;                 o[9] = -(a * h - b * g) * k;  o[10] = (a * e - b * d) * k;  o[11] = 0.0f;
        for (int col = 0; col < 3; ++col) o[12 + col] = -(v[12] * o[col] + v[13] * o[4 + col] + v[14] * o[8 + col]);
        o[15] = 1.0f;
        std::memcpy(g_viewCamera, camera, sizeof(g_viewCamera));
        g_viewValid = true;
    }

    bool AttachedWorld(const void* instance, float out[16], bool& animated)
    {
        return WorldOf(static_cast<const uint8_t*>(instance), out, animated, 0);
    }

    namespace
    {
        struct IndoorEntry { bool indoor; uint64_t expires; };
        std::unordered_map<uint64_t, IndoorEntry> g_indoorCache;
        unsigned g_indoorTests = 0;
        constexpr unsigned kIndoorTestsPerFrame = 48;

        bool LocateIndoor(const float pos[3])
        {
            namespace wo = wxl::offsets::game::wmo;
            using LocateFn = char(__cdecl*)(const float* a, const float* b, float fraction, void** instanceOut, uint32_t* groupOut);
            using GroupFlagsFn = uint32_t(__fastcall*)(const void* wmo, void* edx, uint32_t group);
            constexpr uintptr_t kGroupFlags = 0x007AE7B0;
            constexpr size_t kInstanceWmo = 0xF4;
            constexpr float kDown = 2.0f;   // yd: the caller probes down from the camera; a light only needs its own spot
            const float b[3] = { pos[0], pos[1], pos[2] - kDown };
            void* instances[2] = {};
            uint32_t groups[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
            __try
            {
                if (!reinterpret_cast<LocateFn>(wo::kLocateViewerMapObjs)(pos, b, 1.0f, instances, groups) || !instances[0] || groups[0] == 0xFFFF) return false;
                const void* wmo = nullptr;
                if (!Get(instances[0], kInstanceWmo, wmo) || !wmo) return false;
                uint32_t flags = reinterpret_cast<GroupFlagsFn>(kGroupFlags)(wmo, nullptr, groups[0]);
                if (groups[1] != 0xFFFF) flags |= reinterpret_cast<GroupFlagsFn>(kGroupFlags)(wmo, nullptr, groups[1]);
                return (flags & wo::kGroupFlagIndoor) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    bool IsIndoor(const float pos[3])
    {
        const auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(v * 2.0f)) & 0x1FFFFF); };
        const uint64_t key = q(pos[0]) | (q(pos[1]) << 21) | (q(pos[2]) << 42);
        // A WMO still loading reads as outdoor, so every answer expires after 5-7 s (spread by key, so
        // a city's lights don't all expire in the same frame) and is asked again, at most
        // kIndoorTestsPerFrame a frame: past that, the old answer (or outdoor, if none yet) stands.
        const uint64_t now = GetTickCount64();
        if (g_indoorCache.size() > 16384) g_indoorCache.clear();
        auto it = g_indoorCache.find(key);
        if (it != g_indoorCache.end() && now < it->second.expires) return it->second.indoor;
        if (g_indoorTests >= kIndoorTestsPerFrame) return it != g_indoorCache.end() && it->second.indoor;
        ++g_indoorTests;
        const bool indoor = LocateIndoor(pos);
        g_indoorCache[key] = IndoorEntry{ indoor, now + 5000 + key % 2000 };
        return indoor;
    }

    namespace
    {
        // --- baked occlusion ----------------------------------------------------------------------
        // Per light (by position + radius, so it survives rescans and merges): for each of kOccCells
        // directions on an octahedral map, how far the light reaches before terrain or a WMO
        // (pick::TraceLine: models don't block, so a light's own torch or firewood never does). Each
        // cell keeps the FARTHEST of 5 rays (its centre and 4 points near its corners): a surface the
        // light faces at a grazing angle stays lit across the whole cell, and only what lies behind
        // the nearest wall in every sub-ray goes dark. A hit in the first kEmbedded yards counts as the
        // light sitting inside that surface (a torch half in a wall): the ray carries on past it.
        // Baked incrementally under a ray budget, nearest lights first; only lights that stayed in
        // place for kSettleFrames (moving ones never finish, so they're never queued).
        constexpr float    kEmbedded = 0.3f;
        constexpr unsigned kSettleFrames = 15;
        constexpr int      kSubRays = 5;
        constexpr size_t   kMaxEntries = 4096;

        struct OccEntry
        {
            float    dist[kOccCells];
            int      cellsDone = 0;
            unsigned settled = 0;      // consecutive frames requested
            uint64_t lastFrame = 0;
            float    pos[3] = {};
            float    reach = 0.0f;
        };
        std::unordered_map<uint64_t, OccEntry> g_occ;
        uint64_t g_frame = 0;
        std::vector<char> g_wantBake;  // per active light, this frame

        uint64_t OccKey(const float p[3], float radius)
        {
            const auto q = [](float v) { return static_cast<uint64_t>((static_cast<int64_t>(std::floor(v * 4.0f)) + 131072) & 0x3FFFF); };
            const uint64_t r = static_cast<uint64_t>(std::min(1023.0f, std::max(0.0f, radius * 8.0f)));
            return q(p[0]) | (q(p[1]) << 18) | (q(p[2]) << 36) | (r << 54);
        }

        // Octahedral map: (u, v) in [-1, 1] -> unit direction. The shaders encode the other way:
        // n = d / (|x| + |y| + |z|), lower half folded with (1 - |n.yx|) * sign(n.xy).
        void OctDecode(float u, float v, float out[3])
        {
            float x = u, y = v, z = 1.0f - std::fabs(u) - std::fabs(v);
            const float t = std::max(-z, 0.0f);
            x += x >= 0.0f ? -t : t;
            y += y >= 0.0f ? -t : t;
            const float len = std::sqrt(x * x + y * y + z * z);
            out[0] = x / len; out[1] = y / len; out[2] = z / len;
        }

        float CastRay(const float from[3], const float dir[3], float reach, unsigned& rays)
        {
            float start = 0.0f;
            for (int tries = 0; tries < 3; ++tries)
            {
                const float a[3] = { from[0] + dir[0] * start, from[1] + dir[1] * start, from[2] + dir[2] * start };
                const float b[3] = { from[0] + dir[0] * reach, from[1] + dir[1] * reach, from[2] + dir[2] * reach };
                wxl::game::world::WorldHit hit;
                ++rays;
                if (!wxl::game::world::TraceLine(a, b, hit)) return reach;
                const float d = start + (reach - start) * hit.t;
                if (d > kEmbedded) return d;
                start = d + 0.05f;
                if (start >= reach) return reach;
            }
            return reach;
        }

        void BakeCell(OccEntry& e, int cell, unsigned& rays)
        {
            const int ix = cell % kOccSide, iy = cell / kOccSide;
            static const float kSub[kSubRays][2] = { { 0.5f, 0.5f }, { 0.15f, 0.15f }, { 0.85f, 0.15f }, { 0.15f, 0.85f }, { 0.85f, 0.85f } };
            float best = 0.0f;
            for (const auto& s : kSub)
            {
                float dir[3];
                OctDecode((ix + s[0]) / kOccSide * 2.0f - 1.0f, (iy + s[1]) / kOccSide * 2.0f - 1.0f, dir);
                best = std::max(best, CastRay(e.pos, dir, e.reach, rays));
            }
            e.dist[cell] = best;
        }

        void Bake(const float eye[3])
        {
            ++g_frame;
            const double t0 = grassperf::Now();
            unsigned wanted = 0, done = 0, rays = 0;
            std::vector<std::pair<float, OccEntry*>> work;
            for (size_t i = 0; i < g_active.size(); ++i)
            {
                ActiveLight& l = g_active[i];
                l.occlusion = nullptr;
                if (!g_settings.bakeEnabled || !g_wantBake[i] || l.radius <= 0.0f) continue;
                ++wanted;
                const uint64_t key = OccKey(l.pos, l.radius);
                auto it = g_occ.find(key);
                if (it == g_occ.end())
                {
                    if (g_occ.size() >= kMaxEntries) continue;
                    it = g_occ.emplace(key, OccEntry{}).first;
                    std::memcpy(it->second.pos, l.pos, sizeof(it->second.pos));
                    it->second.reach = l.radius + 0.5f;
                }
                OccEntry& e = it->second;
                e.settled = e.lastFrame + 1 == g_frame ? e.settled + 1 : 1;
                e.lastFrame = g_frame;
                if (e.cellsDone == kOccCells) { l.occlusion = e.dist; ++done; continue; }
                if (e.settled < kSettleFrames) continue;
                const float dx = l.pos[0] - eye[0], dy = l.pos[1] - eye[1], dz = l.pos[2] - eye[2];
                work.push_back({ dx * dx + dy * dy + dz * dz, &e });
            }
            std::sort(work.begin(), work.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const unsigned budget = static_cast<unsigned>(std::max(0, g_settings.bakeRaysPerFrame));
            for (auto& w : work)
            {
                OccEntry& e = *w.second;
                while (e.cellsDone < kOccCells && rays + kSubRays * 3 <= budget) BakeCell(e, e.cellsDone++, rays);
                if (rays + kSubRays * 3 > budget) break;
            }
            // Forget lights not seen for a while (left the area, or moved: a new position is a new entry).
            if (g_frame % 300 == 0)
                for (auto it = g_occ.begin(); it != g_occ.end();)
                    it = it->second.lastFrame + 600 < g_frame ? g_occ.erase(it) : std::next(it);
            g_stats.bakeWanted = wanted; g_stats.bakeDone = done; g_stats.bakeRays = rays;
            g_stats.bakeCached = static_cast<unsigned>(g_occ.size());
            g_stats.bakeMs = grassperf::Now() - t0;
        }
    }

    const char* ModelPath(const void* instance)
    {
        SceneModel sm;
        if (instance) Describe(static_cast<const uint8_t*>(instance), sm);
        return sm.path;
    }

    unsigned SceneModels(const float center[3], float range, std::vector<SceneModel>& out)
    {
        // The M2 scene's model list (CM2Model_AttachToScene 0x834540): head at scene +0x08, next at
        // model +0x0C. Model: shared model at +0x2C (path +0x3C, header +0x150; core's doodad SDK),
        // world matrix at +0xB4. Attached models are walked from their roots (AddChildren); one that
        // also shows up in the list itself is skipped there, so it's listed once.
        constexpr size_t kSceneModelHead = 0x08, kModelNext = 0x0C;
        constexpr unsigned kMaxModels = 100000; // a broken (cyclic) list stops here
        constexpr float kChildReach = 30.0f;    // a root this much past the range can still hold an item inside it
        out.clear();
        const void* scene = *reinterpret_cast<const void* const*>(kScene);
        const uint8_t* m = nullptr;
        if (!scene || !Get(scene, kSceneModelHead, m)) return 0;
        unsigned total = 0, attached = 0;
        for (; m && total < kMaxModels; ++total)
        {
            SceneModel sm;
            sm.model = m;
            const uint8_t* parent = nullptr;
            if (Get(m, kInstParent, parent) && !parent && Get(m, kInstPlacement, sm.world))
            {
                const float dx = sm.world[12] - center[0], dy = sm.world[13] - center[1], dz = sm.world[14] - center[2];
                sm.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (sm.distance <= range) { Describe(m, sm); out.push_back(sm); }
                if (sm.distance <= range + kChildReach) AddChildren(m, center, range, out, 1, attached);
            }
            const uint8_t* next = nullptr;
            if (!Get(m, kModelNext, next)) break;
            m = next;
        }
        g_lastAttached = attached;
        std::sort(out.begin(), out.end(), [](const SceneModel& a, const SceneModel& b) { return a.distance < b.distance; });
        return total;
    }

    Settings& Config() { return g_settings; }
    const std::vector<ActiveLight>& Active() { return g_active; }
    const Grid& CellGrid() { return g_grid; }
    Stats GetStats() { return g_stats; }

    void Update(float dt, const world::Snapshot& snap)
    {
        g_active.clear();
        if (!snap.inWorld || !g_settings.enabled)
        {
            g_sources.clear();
            if (g_grid.cells) { g_grid = Grid{ 0.0f, 0.0f, 16.0f, 0, 0, {}, {}, g_grid.generation + 1 }; }
            return;
        }
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
        const double t0 = grassperf::Now();
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
            c.model = s.model;
            c.bake = s.bake;
            c.moving = s.follow != nullptr;
            // Attached models move: their matrix again this frame (gone, e.g. unequipped: skip it).
            float worldPos[3];
            std::memcpy(worldPos, s.worldPos, sizeof(worldPos));
            if (s.follow)
            {
                float w[16];
                bool animated = false;
                if (!AttachedWorld(s.follow, w, animated)) continue;
                Mul(s.localPos, w, worldPos);
                if (s.cosOuter > -1.5f) MulDir(s.localDir, w, c.spotDir);
                c.scale = MatrixScale(w) > 0.0f ? MatrixScale(w) : 1.0f;
            }
            if (s.table)
            {
                std::memcpy(c.pos, worldPos, sizeof(c.pos));
                std::memcpy(c.color, s.color, sizeof(c.color));
            }
            else
            {
                float stored[3] = {}, color[3] = {};
                const bool placed = s.cm2 && Get(s.cm2, 0x0C, stored) && (stored[0] != 0.0f || stored[1] != 0.0f || stored[2] != 0.0f);
                std::memcpy(c.pos, placed ? stored : worldPos, sizeof(c.pos));
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

        const double tCand = grassperf::Now();
        g_stats.candMs = tCand - t0;

        // Merge point lights closer than the merge distance (torch groups): summed colour, position
        // weighted by brightness, the larger radius. Spot lights never merge (their cones differ).
        // Neighbours are found through a grid of merge-distance cells (sorted keys), compared at their
        // positions before merging: a city holds thousands of lights, and comparing every pair cost
        // hundreds of milliseconds there (Dalaran, owner's profiler 2026-10-08).
        unsigned merged = 0;
        const float md = g_settings.mergeDistance;
        const float md2 = md * md;
        static std::vector<std::pair<uint64_t, uint32_t>> cells;
        static std::vector<float> original;
        cells.clear();
        original.resize(cands.size() * 3);
        const float inv = md > 1e-3f ? 1.0f / md : 0.0f;
        const auto cellOf = [&](const float p[3], int out[3]) { for (int k = 0; k < 3; ++k) out[k] = static_cast<int>(std::floor(p[k] * inv)); };
        const auto keyOf = [](int x, int y, int z)
        {
            return (static_cast<uint64_t>(x + 1048576) & 0x1FFFFF) | ((static_cast<uint64_t>(y + 1048576) & 0x1FFFFF) << 21) |
                   ((static_cast<uint64_t>(z + 1048576) & 0x1FFFFF) << 42);
        };
        if (inv > 0.0f)
        {
            for (size_t k = 0; k < cands.size(); ++k)
            {
                std::memcpy(&original[k * 3], cands[k].pos, 3 * sizeof(float));
                if (cands[k].cosOuter > -1.5f) continue;
                int c[3];
                cellOf(cands[k].pos, c);
                cells.push_back({ keyOf(c[0], c[1], c[2]), static_cast<uint32_t>(k) });
            }
            std::sort(cells.begin(), cells.end());
        }
        for (size_t a = 0; a < cands.size() && inv > 0.0f; ++a)
        {
            if (cands[a].weight <= 0.0f || cands[a].cosOuter > -1.5f) continue;
            float wa = cands[a].color[0] + cands[a].color[1] + cands[a].color[2];
            int ca[3];
            cellOf(&original[a * 3], ca);
            for (int nz = -1; nz <= 1; ++nz)
            for (int ny = -1; ny <= 1; ++ny)
            for (int nx = -1; nx <= 1; ++nx)
            {
                const uint64_t key = keyOf(ca[0] + nx, ca[1] + ny, ca[2] + nz);
                auto it = std::lower_bound(cells.begin(), cells.end(), std::make_pair(key, 0u));
                for (; it != cells.end() && it->first == key; ++it)
            {
                const size_t b = it->second;
                if (b <= a || cands[b].weight <= 0.0f) continue;
                const float* pa = &original[a * 3];
                const float* pb = &original[b * 3];
                const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
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
                cands[a].bake = cands[a].bake || cands[b].bake;
                cands[a].moving = cands[a].moving || cands[b].moving;
                wa += wb;
                cands[b].weight = 0.0f;
                ++merged;
            }
            }
        }
        g_stats.merged = merged;
        const double tMerge = grassperf::Now();
        g_stats.mergeMs = tMerge - tCand;

        // Every light in range, fading out over the last third of the range. They stay in the scan's
        // order (stable between scans, so the grid doesn't reshuffle every frame); only past kMaxPool
        // do the farthest from the camera drop out.
        std::vector<std::pair<float, size_t>> order;
        for (size_t k = 0; k < cands.size(); ++k)
            if (cands[k].weight > 0.0f)
            {
                const float dx = cands[k].pos[0] - eye[0], dy = cands[k].pos[1] - eye[1], dz = cands[k].pos[2] - eye[2];
                order.push_back({ dx * dx + dy * dy + dz * dz, k });
            }
        g_stats.poolDropped = 0;
        if (order.size() > static_cast<size_t>(kMaxPool))
        {
            g_stats.poolDropped = static_cast<unsigned>(order.size() - kMaxPool);
            std::nth_element(order.begin(), order.begin() + kMaxPool, order.end());
            order.resize(kMaxPool);
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        }
        g_baseLum.clear();
        g_wantBake.clear();
        unsigned indoorCount = 0;
        g_indoorTests = 0;
        const float fadeStart = range * (2.0f / 3.0f);
        for (size_t n = 0; n < order.size(); ++n)
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
            l.model = c.model;
            l.indoor = g_settings.indoorSkipsTerrain && IsIndoor(l.pos);
            indoorCount += l.indoor ? 1 : 0;
            g_active.push_back(l);
            g_baseLum.push_back((c.color[0] + c.color[1] + c.color[2]) * g_settings.brightness);
            g_wantBake.push_back(!c.moving && (c.bake || g_settings.bakeAll) ? 1 : 0);
        }
        g_stats.active = static_cast<unsigned>(g_active.size());
        g_stats.indoor = indoorCount;
        g_stats.indoorTests = g_indoorTests;
        const double tActive = grassperf::Now();
        g_stats.activeMs = tActive - tMerge;
        Bake(eye);
        const double tGrid = grassperf::Now();
        BuildGrid(eye);
        g_stats.gridMs = grassperf::Now() - tGrid;
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
