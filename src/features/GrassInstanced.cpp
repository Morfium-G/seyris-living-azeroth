#include "GrassInstanced.hpp"

#include "GrassPerf.hpp"

#include "../env/WorldQuery.hpp"
#include "../render/ShaderPatch.hpp"

#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::grassinst
{
    namespace
    {
        namespace gd = grassdoodads;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // --- client landmarks (disassembly; see the plan doc) ------------------------------------
        constexpr uintptr_t kGxDevicePtr     = 0x00C5DF88;
        constexpr uintptr_t kGxDeviceVtable  = 0x00A2E718;
        constexpr size_t    kVtBufLock       = 0xD8;       // thiscall(device, buffer) -> write pointer
        constexpr size_t    kVtBufUnlock     = 0xDC;       // thiscall(device, buffer, 0)
        constexpr size_t    kDeviceReady     = 0xF58;      // the device draw's own guard: ready != 0 ...
        constexpr size_t    kDeviceBusy      = 0xF5C;      // ... and busy == 0
        constexpr uintptr_t kBufStream       = 0x00684850; // thiscall(device, kind 0 = vertex, stride, count)
        constexpr uintptr_t kPrimVertexPtr   = 0x00681B00; // cdecl(buffer, format): the current vertex stream
        constexpr int       kGrassFormat     = 4;          // what the layer draw binds its buffer as
        constexpr int       kGrassStride     = 0x24;
        constexpr size_t    kBufBuilt        = 0x1C;       // stream buffer: filled by its owner
        constexpr uintptr_t kTextureGetGxTex = 0x004B6CB0; // cdecl(texture, 0, 0)
        constexpr uintptr_t kGxRsSet         = 0x00685F50; // thiscall(device, state, value)
        constexpr int       kGxStateTexture0 = 0x15;
        constexpr uintptr_t kGxFlushState    = 0x006A5940; // thiscall(device): the engine's cached state -> D3D
        constexpr uintptr_t kGxCaps          = 0x00532AF0; // thiscall(device) -> caps
        constexpr size_t    kCapsSwapColor   = 0x14;       // == 1: the bake swaps red and blue

        constexpr uintptr_t kDoodadTable     = 0x00D1C4FC; // -> entry*[maxId + 1]
        constexpr uintptr_t kDoodadTableSize = 0x00D1C4F8;
        constexpr uintptr_t kKeepColorAlpha  = 0x00D1C4F0; // 0 = the bake folds alpha into rgb
        constexpr uintptr_t kTiltBasisX      = 0x009E418C; // float the tilt basis starts from (0.0 shipped)

        // layer slot
        constexpr size_t kSlotTexture = 0x00, kSlotVertexCount = 0x04, kSlotVB = 0x0C;
        constexpr size_t kSlotInstanceCount = 0x18, kSlotInstances = 0x1C;
        // instance record
        constexpr size_t kRecStride = 0x2C, kRecFlags = 0x02, kRecDoodad = 0x04, kRecPos = 0x08;
        constexpr size_t kRecAngle = 0x14, kRecScale = 0x18, kRecUp = 0x1C, kRecColor = 0x28;
        // doodad entry -> GroundEffectDoodad row / model
        constexpr size_t   kEntryRow = 0x00, kEntryModel = 0x04, kRowFlags = 0x08;
        constexpr uint32_t kRowFlagTilt = 0x01; // plants lean to the terrain normal
        constexpr size_t   kModelFlags = 0x10;
        constexpr uint32_t kModelLoaded = 0x01;
        constexpr size_t   kModelShared = 0x2C, kSharedHeader = 0x150, kSharedSkin = 0x170, kHeaderVerts = 0x40;
        constexpr size_t   kSkinLookupCount = 0x04, kSkinLookup = 0x08, kSkinTriCount = 0x0C, kSkinTris = 0x10;
        constexpr size_t   kM2VertexStride = 48, kM2VertexUv = 0x20;
        constexpr size_t   kBakedStride = 36; // the client's grass vertex: pos, normal, colour, uv

        using BufStreamFn     = void*(__fastcall*)(void* dev, void* edx, int kind, int stride, int count);
        using BufLockFn       = void*(__fastcall*)(void* dev, void* edx, void* buf);
        using BufUnlockFn     = uint32_t(__fastcall*)(void* dev, void* edx, void* buf, int arg);
        using PrimVertexPtrFn = void(__cdecl*)(void* buf, int format);
        using GetGxTexFn      = void*(__cdecl*)(void* texture, int, int);
        using RsSetFn         = void(__fastcall*)(void* dev, void* edx, int state, void* value);
        using FlushFn         = void(__fastcall*)(void* dev, void* edx);
        using CapsFn          = const uint8_t*(__fastcall*)(void* dev, void* edx);

        // Evict slot buffers not drawn for this many frames (checked every kEvictEvery frames).
        constexpr uint32_t kEvictAfter = 900, kEvictEvery = 128;
        constexpr uint32_t kEvictAfterTight = 60; // at the memory cap: anything not drawn for ~1 s

        // --- GPU layouts ---------------------------------------------------------------------------
        struct GpuVertex { float pos[3]; float uv[2]; };
        static_assert(sizeof(GpuVertex) == 20, "GpuVertex");

        // One plant: model -> chunk-local transform as three rows (scale folded in, translation in
        // w), then the normal and colour the client's bake writes into every vertex of the plant.
        struct GpuInstance { float x[4], y[4], z[4]; float normal[3]; uint32_t color; };
        static_assert(sizeof(GpuInstance) == 64, "GpuInstance");

        // The instanced shader's extra inputs (see MakeInstanced), inserted after the last dcl.
        // Density copies are made on the GPU: stream 1 repeats each plant for `multiplier`
        // instances (frequency divider) and stream 2 gives every instance a copy entry
        // (a, b, turn, weight); weight 0 = the original. A copy turns the model about its own up
        // axis and moves toward a random point of the plant's OWN terrain triangle -- at most
        // c153.x yards -- so the triangle's plane (normal = v1) gives its exact height. Placement
        // (0x7D3390): chunk-local coordinates run from the chunk corner into the negatives, the 8x8
        // cells are 4.1667 yd, each cell split into 4 triangles by its diagonals; a triangle is
        // convex, so moving toward a point inside it never leaves it.
        constexpr unsigned kCopyReg = 153;
        const char* kInstancedInputs =
            "    dcl_texcoord1 v4\n"
            "    dcl_texcoord2 v5\n"
            "    dcl_texcoord3 v6\n"
            "    dcl_texcoord4 v7\n"
            "    def c154, 1, 0, 0.0001, -2\n"
            "    def c155, 4.16666651, 0.24, 0.5, 0\n"
            // the copy's turn about the model's up axis, then the plant transform
            "    mov r30, v0\n"
            "    sincos r29.xy, v7.z\n"
            "    mul r28.x, r30.x, r29.x\n"
            "    mad r28.x, -r30.y, r29.y, r28.x\n"
            "    mul r28.y, r30.x, r29.y\n"
            "    mad r28.y, r30.y, r29.x, r28.y\n"
            "    mov r28.zw, r30\n"
            "    dp4 r31.x, r28, v4\n"
            "    dp4 r31.y, r28, v5\n"
            "    dp4 r31.z, r28, v6\n"
            "    mov r31.w, r30.w\n"
            // the plant's cell and triangle (in |chunk-local| cell units)
            "    mov r20.x, v4.w\n"
            "    mov r20.y, v5.w\n"
            "    slt r20.zw, r20.xxxy, c154.y\n"
            "    mad r20.zw, r20, c154.w, c154.x\n"     // sign of each coordinate
            "    abs r21.xy, r20\n"
            "    mul r21.xy, r21, c155.y\n"
            "    frc r22.xy, r21\n"                     // position in the cell, 0..1
            "    add r21.xy, r21, -r22\n"               // cell corner
            "    slt r23.x, r22.y, r22.x\n"             // d1: below the diagonal y = x
            "    add r23.w, c154.x, -r22.x\n"
            "    slt r23.y, r22.y, r23.w\n"             // d2: below the diagonal y = 1 - x
            "    add r24.x, c154.x, -r23.y\n"           // corner A = (1 - d2, 1 - d1)
            "    add r24.y, c154.x, -r23.x\n"
            "    mov r25.x, r23.x\n"                    // corner B = (d1, 1 - d2); C = cell centre
            "    mov r25.y, r24.x\n"
            "    add r24.xy, r24, -c155.z\n"
            "    add r25.xy, r25, -c155.z\n"
            // a uniform point of the triangle from (a, b), folded when a + b > 1
            "    add r26.x, v7.x, v7.y\n"
            "    slt r26.y, c154.x, r26.x\n"
            "    add r27.xy, c154.x, -v7\n"
            "    lrp r27.xy, r26.y, r27, v7\n"
            "    mad r26.xy, r27.x, r24, c155.z\n"
            "    mad r26.xy, r27.y, r25, r26\n"
            "    add r26.xy, r21, r26\n"
            "    mul r26.xy, r26, c155.x\n"
            "    mul r26.xy, r26, r20.zwzw\n"           // back to chunk-local
            "    add r26.xy, r26, -r20\n"               // offset from the plant
            // at most c153.x yards of it; none for the original (weight 0)
            "    mul r27.zw, r26.xxxy, r26.xxxy\n"
            "    add r27.z, r27.z, r27.w\n"
            "    add r27.z, r27.z, c154.z\n"
            "    rsq r27.w, r27.z\n"
            "    mul r27.w, r27.w, c153.x\n"
            "    min r27.w, r27.w, c154.x\n"
            "    mul r27.w, r27.w, v7.w\n"
            "    mul r29.xy, r26, r27.w\n"
            // height from the triangle's plane
            "    mul r29.z, v1.x, r29.x\n"
            "    mad r29.z, v1.y, r29.y, r29.z\n"
            "    rcp r29.w, v1.z\n"
            "    mul r29.z, -r29.z, r29.w\n"
            "    add r31.xyz, r31, r29\n";

        // --- state ---------------------------------------------------------------------------------
        const WXL_Api* g_api = nullptr;
        Settings       g_settings;
        const char*    g_problem = nullptr;
        bool           g_capsChecked = false;
        uint32_t       g_frame = 1;

        struct Geometry
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            IDirect3DIndexBuffer9*  ib = nullptr;
            uint32_t vertices = 0, indices = 0;
            bool     tilt = false;
            bool     bad = false; // unusable model data: its slots stay stock
        };
        std::unordered_map<uint32_t, Geometry> g_geometry;

        // Instance pool: creating a D3D buffer per layer cost ~2.5 ms each (up to 37 ms), so layers
        // get ranges in a few large dynamic pages, filled with NOOVERWRITE locks (a ranged lock on
        // a static buffer measured 17-27 ms: it waits for the GPU). A freed range is only handed
        // out again after kReuseDelay frames, since the GPU may still be reading it.
        constexpr uint32_t kPageInstances = 65536; // 4 MB; also the most plants a slot can hold
        constexpr uint32_t kReuseDelay = 4;

        // Dynamic buffers usually live in system memory mapped into this 32-bit process, so the pool
        // has a hard cap (Settings::memoryLimitMB): past it, no new pages; stale layers are dropped
        // and pages that empty out are released. Growing unchecked (800 MB after teleporting) left
        // the client's own heap without address space (MapMem.cpp out-of-memory).
        constexpr uint32_t kPageBytes = kPageInstances * 64;

        struct Block { uint32_t offset, size; }; // in instances
        struct Page { IDirect3DVertexBuffer9* vb = nullptr; std::vector<Block> free; }; // vb null = released
        struct Retired { uint32_t page; Block block; uint32_t frame; };
        std::vector<Page>    g_pages;
        std::vector<Retired> g_retired;

        struct Range { Geometry* geo; uint32_t start, count; }; // start: instance index in the page
        struct SlotEntry
        {
            uintptr_t instances = 0;
            uint32_t  count = 0, fingerprint = 0;
            int       page = -1; // -1 = nothing allocated
            uint32_t  offset = 0;
            uint32_t  allocated = 0;  // instances in the page range
            float     center[2] = {}; // chunk-local centre of the plants (density distance)
            gd::SlotDoodads sd{};
            std::vector<Range> ranges;
            uint32_t  lastFrame = 0;
        };
        std::unordered_map<const void*, SlotEntry> g_slots;

        struct Derived { IDirect3DVertexShader9* shader = nullptr; };
        std::unordered_map<IDirect3DVertexShader9*, Derived> g_shaders; // key: the client's grass shader (held)
        IDirect3DVertexDeclaration9* g_decl = nullptr;
        bool  g_declFailed = false;

        // Density near the player: each layer draws (multiplier) instances per plant, full within
        // half the radius and fading to 1 at the radius, by the distance of its plants' centre.
        // Decided per draw, so nothing is rebuilt when the player moves or a setting changes.
        constexpr unsigned kMaxMultiplier = 8;
        constexpr unsigned kCopyPlants = 8192;     // plants per instanced draw (bigger ranges split)
        constexpr unsigned kCopyTableSpan = 16384; // table entries per multiplier, in plants
        struct CopyEntry { float a, b, turn, weight; };
        IDirect3DVertexBuffer9* g_copyTable = nullptr; // per multiplier m: kCopyTableSpan * m entries
        uint32_t g_copyBase[kMaxMultiplier + 1] = {};  // first entry of multiplier m's region
        bool     g_memoryTight = false;    // at the instance pool's page cap
        int      g_lastMapId = -1;
        float    g_chunkOrigin[3] = {};    // world position of the chunk whose layers draw next
        bool     g_chunkOriginValid = false;


        Stats  g_frameStats, g_lastStats;
        double g_frameBuildMs = 0; // against Settings::buildBudgetMs

        // Verification capture.
        struct Capture
        {
            bool pending = false, armed = false, done = false;
            void* buf = nullptr;
            uint8_t* ptr = nullptr;
            uint32_t vertices = 0;
            std::vector<uint8_t> data;
        };
        Capture     g_cap;
        std::string g_verifyReport = "not run yet";
        BufLockFn   g_origLock = nullptr;
        BufUnlockFn g_origUnlock = nullptr;
        bool        g_captureHooked = false;

        // --- guarded reads (no C++ objects in these frames: __try needs that) --------------------
        bool ReadU32(uintptr_t address, uint32_t& out)
        {
            __try { out = *reinterpret_cast<const uint32_t*>(address); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool SafeCopy(void* dst, uintptr_t src, size_t bytes)
        {
            __try { std::memcpy(dst, reinterpret_cast<const void*>(src), bytes); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void* GxDevice() { return *reinterpret_cast<void**>(kGxDevicePtr); }

        template <class Fn> Fn EngineVirtual(void* gxDev, size_t byteOffset)
        {
            return reinterpret_cast<Fn>((*reinterpret_cast<void***>(gxDev))[byteOffset / 4]);
        }

        void Release(IUnknown* p) { if (p) p->Release(); }

        // --- the client's bake, reproduced (DetailDoodad_FillLayerSlotVB 0x7B1B50) ---------------
        struct BakeConstants { bool keepAlpha = true; bool swapRedBlue = false; float basisX = 0.0f; };

        bool ReadBakeConstants(BakeConstants& k)
        {
            uint32_t keep = 0, basis = 0;
            if (!ReadU32(kKeepColorAlpha, keep) || !ReadU32(kTiltBasisX, basis)) return false;
            void* dev = GxDevice();
            if (!dev) return false;
            const uint8_t* caps = reinterpret_cast<CapsFn>(kGxCaps)(dev, nullptr);
            uint32_t swap = 0;
            if (!caps || !ReadU32(reinterpret_cast<uintptr_t>(caps) + kCapsSwapColor, swap)) return false;
            k.keepAlpha = keep != 0;
            k.swapRedBlue = swap == 1;
            std::memcpy(&k.basisX, &basis, sizeof(float));
            return true;
        }

        // 0x7B1D43..0x7B1DBF: unless the global says otherwise, rgb is scaled by
        // (0xB332 + 0x4C * alpha) / 65536 and alpha becomes opaque; then red/blue swap per caps.
        uint32_t BakeColor(uint32_t c, const BakeConstants& k)
        {
            if (!k.keepAlpha)
            {
                const uint32_t f = (c >> 24) * 0x4Cu + 0xB332u;
                const uint32_t b0 = ((c & 0xFFu) * f) >> 16;
                const uint32_t b1 = (((c >> 8) & 0xFFu) * f) >> 16;
                const uint32_t b2 = (((c >> 16) & 0xFFu) * f) >> 16;
                c = b0 | (b1 << 8) | (b2 << 16) | 0xFF000000u;
            }
            if (k.swapRedBlue)
                c = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
            return c;
        }

        // Tilted plants (0x7B1E04 + 0x7B1000): a basis from the up vector u -- p = (K, uz, -uy) /
        // |(uy, uz)|, q = u x p, rows (q, p, u) -- turned about z by the plant's angle. Intermediate
        // precision follows the x87 code: values it keeps on the stack stay double here, values it
        // stores go through float.
        void TiltRotation(const float up[3], float angle, float basisX, float out[9])
        {
            const double ux = up[0], uy = up[1], uz = up[2];
            const double inv = 1.0 / std::sqrt(uy * uy + uz * uz);
            const double pk = basisX * inv, pb = uz * inv, pa = -uy * inv;
            const float m[9] = {
                static_cast<float>(uy * pa - uz * pb), static_cast<float>(uz * pk - ux * pa),
                static_cast<float>(ux * pb - uy * pk),
                static_cast<float>(pk), static_cast<float>(pb), static_cast<float>(pa),
                up[0], up[1], up[2],
            };
            const float c = static_cast<float>(std::cos(static_cast<double>(angle)));
            const float s = static_cast<float>(std::sin(static_cast<double>(angle)));
            const double t8 = static_cast<float>((1.0 - c) + c);
            out[0] = static_cast<float>(double(c) * m[0] + double(s) * m[3]);
            out[1] = static_cast<float>(double(c) * m[1] + double(s) * m[4]);
            out[2] = static_cast<float>(double(c) * m[2] + double(s) * m[5]);
            out[3] = static_cast<float>(double(c) * m[3] - double(s) * m[0]);
            out[4] = static_cast<float>(double(c) * m[4] - double(s) * m[1]);
            out[5] = static_cast<float>(double(c) * m[5] - double(s) * m[2]);
            out[6] = static_cast<float>(t8 * m[6]);
            out[7] = static_cast<float>(t8 * m[7]);
            out[8] = static_cast<float>(t8 * m[8]);
        }

        // The bake caches the tilt matrix per (flags & 3) and only recomputes it when (flags & 0xFC)
        // changes from one tilted plant to the next, so a plant can reuse an earlier plant's
        // matrix. Reproduced, not "fixed": the aim is the client's picture.
        struct TiltCache { uint16_t group = 0xFFFF; uint8_t valid = 0; float m[4][9] = {}; };

        void PlantTransform(const uint8_t* rec, bool tilt, TiltCache& cache, const BakeConstants& k, GpuInstance& g)
        {
            uint16_t flags; float pos[3], angle, scale, up[3];
            std::memcpy(&flags, rec + kRecFlags, sizeof(flags));
            std::memcpy(pos, rec + kRecPos, sizeof(pos));
            std::memcpy(&angle, rec + kRecAngle, sizeof(angle));
            std::memcpy(&scale, rec + kRecScale, sizeof(scale));
            std::memcpy(up, rec + kRecUp, sizeof(up));

            if (tilt)
            {
                const uint16_t group = flags & 0xFC;
                const unsigned idx = flags & 3u;
                if (group != cache.group) { cache.group = group; cache.valid = 0; }
                if (!(cache.valid & (1u << idx)))
                {
                    TiltRotation(up, angle, k.basisX, cache.m[idx]);
                    cache.valid |= static_cast<uint8_t>(1u << idx);
                }
                // vertex' = v.x * row0 + v.y * row1 + v.z * row2 of the 3x3 (0x7B1EF3).
                const float* o = cache.m[idx];
                const float x[4] = { o[0] * scale, o[3] * scale, o[6] * scale, pos[0] };
                const float y[4] = { o[1] * scale, o[4] * scale, o[7] * scale, pos[1] };
                const float z[4] = { o[2] * scale, o[5] * scale, o[8] * scale, pos[2] };
                std::memcpy(g.x, x, sizeof(x)); std::memcpy(g.y, y, sizeof(y)); std::memcpy(g.z, z, sizeof(z));
            }
            else
            {
                // Upright plants (0x7B22FE): a plain turn about z.
                const float c = static_cast<float>(std::cos(static_cast<double>(angle)));
                const float s = static_cast<float>(std::sin(static_cast<double>(angle)));
                const float x[4] = { c * scale, -s * scale, 0.0f, pos[0] };
                const float y[4] = { s * scale, c * scale, 0.0f, pos[1] };
                const float z[4] = { 0.0f, 0.0f, scale, pos[2] };
                std::memcpy(g.x, x, sizeof(x)); std::memcpy(g.y, y, sizeof(y)); std::memcpy(g.z, z, sizeof(z));
            }
            std::memcpy(g.normal, up, sizeof(up));
        }

        // --- density preview: extra plants of our own ------------------------------------------
        // A plant's up vector is the normal of the terrain triangle it stands on, and its position
        // lies on that triangle's plane, so a copy moved by (dx, dy) gets its height from the
        // plane: exact while it stays on the triangle (~2 yd across), very close just past it.
        // The shader does the moving (kInstancedInputs); this builds the table of copy entries.
        uint32_t Mix(uint32_t x)
        {
            x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
            return x;
        }

        float Unit(uint32_t& state)
        {
            state = Mix(state + 0x9E3779B9u);
            return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
        }


        // --- doodad model geometry ----------------------------------------------------------------
        // The bake writes, per plant, one vertex per skin vertex-lookup entry (skin +4 count, +8 u16
        // model vertex indices), and 0x7B12B0 indexes them with the skin's triangle list (+0xC
        // count, +0x10 u16) -- so that pair is the model's geometry.
        struct ModelData { std::vector<GpuVertex> verts; std::vector<uint16_t> tris; uint32_t rowFlags = 0; };
        enum class ModelRead { Ok, NotLoaded, Bad };

        ModelRead ReadModel(uint32_t id, ModelData& out)
        {
            uint32_t table = 0, size = 0, entry = 0, row = 0, model = 0, modelFlags = 0;
            if (!ReadU32(kDoodadTable, table) || !ReadU32(kDoodadTableSize, size) || !table || id >= size) return ModelRead::Bad;
            if (!ReadU32(table + id * 4, entry) || !entry) return ModelRead::Bad;
            if (!ReadU32(entry + kEntryRow, row) || !row || !ReadU32(row + kRowFlags, out.rowFlags)) return ModelRead::Bad;
            if (!ReadU32(entry + kEntryModel, model) || !model || !ReadU32(model + kModelFlags, modelFlags)) return ModelRead::Bad;
            if (!(modelFlags & kModelLoaded)) return ModelRead::NotLoaded;

            uint32_t shared = 0, header = 0, skin = 0, verts = 0, lookupCount = 0, lookup = 0, triCount = 0, tris = 0;
            if (!ReadU32(model + kModelShared, shared) || !shared) return ModelRead::Bad;
            if (!ReadU32(shared + kSharedHeader, header) || !header) return ModelRead::Bad;
            if (!ReadU32(shared + kSharedSkin, skin) || !skin) return ModelRead::Bad;
            if (!ReadU32(header + kHeaderVerts, verts) || !verts) return ModelRead::Bad;
            if (!ReadU32(skin + kSkinLookupCount, lookupCount) || !ReadU32(skin + kSkinLookup, lookup) || !lookup) return ModelRead::Bad;
            if (!ReadU32(skin + kSkinTriCount, triCount) || !ReadU32(skin + kSkinTris, tris) || !tris) return ModelRead::Bad;
            if (lookupCount == 0 || lookupCount > 0xFFFF || triCount == 0 || triCount % 3 != 0 || triCount > 0x30000)
                return ModelRead::Bad;

            std::vector<uint16_t> lk(lookupCount);
            out.tris.resize(triCount);
            if (!SafeCopy(lk.data(), lookup, lk.size() * 2) || !SafeCopy(out.tris.data(), tris, out.tris.size() * 2))
                return ModelRead::Bad;
            for (uint16_t t : out.tris)
                if (t >= lookupCount) return ModelRead::Bad;

            out.verts.resize(lookupCount);
            for (uint32_t i = 0; i < lookupCount; ++i)
            {
                uint8_t v[kM2VertexStride];
                if (!SafeCopy(v, verts + static_cast<uintptr_t>(lk[i]) * kM2VertexStride, sizeof(v))) return ModelRead::Bad;
                std::memcpy(out.verts[i].pos, v, sizeof(out.verts[i].pos));
                std::memcpy(out.verts[i].uv, v + kM2VertexUv, sizeof(out.verts[i].uv));
            }
            return ModelRead::Ok;
        }

        template <class Buffer>
        bool Upload(Buffer* buf, const void* data, size_t bytes)
        {
            void* p = nullptr;
            if (FAILED(buf->Lock(0, 0, &p, 0)) || !p) return false;
            std::memcpy(p, data, bytes);
            buf->Unlock();
            return true;
        }

        enum class Need { Ok, NotLoaded, Failed };

        Geometry* EnsureGeometry(IDirect3DDevice9* d3d, uint32_t id, Need& why)
        {
            Geometry& g = g_geometry[id];
            if (g.vb) { why = Need::Ok; return &g; }
            if (g.bad) { why = Need::Failed; return nullptr; }

            ModelData m;
            const ModelRead r = ReadModel(id, m);
            if (r == ModelRead::NotLoaded) { why = Need::NotLoaded; return nullptr; }
            if (r == ModelRead::Bad)
            {
                g.bad = true;
                why = Need::Failed;
                g_api->Log(WXL_LOG_WARN, kTag, "instanced grass: doodad %u has unusable model data; its layers stay on the client's path.", id);
                return nullptr;
            }

            const UINT vbBytes = static_cast<UINT>(m.verts.size() * sizeof(GpuVertex));
            const UINT ibBytes = static_cast<UINT>(m.tris.size() * sizeof(uint16_t));
            if (FAILED(d3d->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g.vb, nullptr))
                || FAILED(d3d->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &g.ib, nullptr))
                || !Upload(g.vb, m.verts.data(), vbBytes) || !Upload(g.ib, m.tris.data(), ibBytes))
            {
                Release(g.vb); Release(g.ib);
                g.vb = nullptr; g.ib = nullptr;
                why = Need::Failed; // device trouble, not the model: try again later
                return nullptr;
            }
            g.vertices = static_cast<uint32_t>(m.verts.size());
            g.indices = static_cast<uint32_t>(m.tris.size());
            g.tilt = (m.rowFlags & kRowFlagTilt) != 0;
            why = Need::Ok;
            return &g;
        }

        // --- slot instance buffers ----------------------------------------------------------------
        uint32_t Fingerprint(uintptr_t instances, uint32_t count, bool& ok)
        {
            uint8_t recs[3][kRecStride];
            const uint32_t pick[3] = { 0, count / 2, count - 1 };
            ok = true;
            for (int i = 0; i < 3 && ok; ++i)
                ok = SafeCopy(recs[i], instances + pick[i] * kRecStride, kRecStride);
            uint32_t h = 2166136261u;
            auto mix = [&h](const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; } };
            mix(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
            mix(&recs[0][0], sizeof(recs));
            return h;
        }

        unsigned LivePages();
        unsigned PageLimit();

        bool Allocate(IDirect3DDevice9* d3d, uint32_t count, int& page, uint32_t& offset)
        {
            for (size_t p = 0; p < g_pages.size(); ++p)
            {
                if (!g_pages[p].vb) continue;
                std::vector<Block>& free = g_pages[p].free;
                for (size_t i = 0; i < free.size(); ++i)
                {
                    if (free[i].size < count) continue;
                    page = static_cast<int>(p);
                    offset = free[i].offset;
                    free[i].offset += count;
                    free[i].size -= count;
                    if (!free[i].size) free.erase(free.begin() + i);
                    return true;
                }
            }
            if (LivePages() >= PageLimit()) { g_memoryTight = true; return false; }

            IDirect3DVertexBuffer9* vb = nullptr;
            if (FAILED(d3d->CreateVertexBuffer(kPageBytes, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb, nullptr)))
                return false;
            size_t p = 0;
            while (p < g_pages.size() && g_pages[p].vb) ++p; // reuse a released page's index
            if (p == g_pages.size()) g_pages.emplace_back();
            g_pages[p].vb = vb;
            g_pages[p].free.assign(1, { count, kPageInstances - count });
            page = static_cast<int>(p);
            offset = 0;
            return true;
        }

        unsigned LivePages()
        {
            unsigned n = 0;
            for (const Page& p : g_pages) n += p.vb ? 1 : 0;
            return n;
        }

        unsigned PageLimit()
        {
            const float mb = g_settings.memoryLimitMB < 16.0f ? 16.0f : g_settings.memoryLimitMB;
            return static_cast<unsigned>(mb * 1024.0f * 1024.0f / kPageBytes);
        }

        // Back into the page's free list (sorted by offset), merged with its neighbours.
        void Reclaim(uint32_t page, Block b)
        {
            std::vector<Block>& free = g_pages[page].free;
            auto at = std::lower_bound(free.begin(), free.end(), b.offset,
                                       [](const Block& x, uint32_t off) { return x.offset < off; });
            at = free.insert(at, b);
            if (at + 1 != free.end() && at->offset + at->size == (at + 1)->offset)
            {
                at->size += (at + 1)->size;
                free.erase(at + 1);
            }
            if (at != free.begin() && (at - 1)->offset + (at - 1)->size == at->offset)
            {
                (at - 1)->size += at->size;
                free.erase(at);
            }
        }

        void ReleaseSlot(SlotEntry& e)
        {
            if (e.page >= 0) g_retired.push_back({ static_cast<uint32_t>(e.page), { e.offset, e.allocated }, g_frame });
            e.page = -1;
            e.ranges.clear();
        }

        void ReleasePool()
        {
            for (Page& p : g_pages) Release(p.vb);
            g_pages.clear();
            g_retired.clear();
        }

        Need BuildSlot(IDirect3DDevice9* d3d, uintptr_t instances, uint32_t count, SlotEntry& e, double& deviceMs)
        {
            deviceMs = 0;
            std::vector<uint8_t> recs(static_cast<size_t>(count) * kRecStride);
            BakeConstants k;
            if (!SafeCopy(recs.data(), instances, recs.size()) || !ReadBakeConstants(k)) return Need::Failed;

            gd::SlotDoodads sd{};
            bool overflow = false;
            std::vector<uint32_t>  order;    // distinct doodads, first-appearance order
            std::vector<Geometry*> orderGeo;
            std::vector<uint32_t>  group(count);
            std::vector<GpuInstance> plants(count);
            TiltCache tilt;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* rec = &recs[static_cast<size_t>(i) * kRecStride];
                uint32_t id, color;
                std::memcpy(&id, rec + kRecDoodad, sizeof(id));
                std::memcpy(&color, rec + kRecColor, sizeof(color));

                uint32_t g = 0;
                while (g < order.size() && order[g] != id) ++g;
                if (g == order.size())
                {
                    Need why;
                    Geometry* geo = EnsureGeometry(d3d, id, why);
                    if (!geo) return why;
                    order.push_back(id);
                    orderGeo.push_back(geo);
                }
                group[i] = g;

                // Same tag as grassdoodads' build hook, applied before the bake's colour step.
                const unsigned entry = gd::EntryFor(sd, id, overflow);
                PlantTransform(rec, orderGeo[g]->tilt, tilt, k, plants[i]);
                plants[i].color = BakeColor((color & ~gd::kIndexMask) | gd::IndexTag(entry), k);
            }

            // The plants' centre, for the per-draw density distance.
            float center[2] = {};
            for (uint32_t i = 0; i < count; ++i) { center[0] += plants[i].x[3]; center[1] += plants[i].y[3]; }
            center[0] /= count; center[1] /= count;
            const uint32_t total = static_cast<uint32_t>(plants.size());

            // Group the plants by doodad so each doodad's plants are one contiguous instance range.
            std::vector<uint32_t> start(order.size(), 0), fill(order.size(), 0);
            for (uint32_t g : group) ++fill[g];
            for (size_t g = 1; g < order.size(); ++g) start[g] = start[g - 1] + fill[g - 1];
            std::vector<GpuInstance> sorted(total);
            std::copy(start.begin(), start.end(), fill.begin());
            for (uint32_t i = 0; i < total; ++i) sorted[fill[group[i]]++] = plants[i];

            // Into a pool range: the range is free, so the GPU isn't reading it (NOOVERWRITE).
            const double t0 = grassperf::Now();
            int page = -1;
            uint32_t offset = 0;
            bool uploaded = Allocate(d3d, total, page, offset);
            if (uploaded)
            {
                void* p = nullptr;
                const UINT bytes = total * static_cast<UINT>(sizeof(GpuInstance));
                uploaded = SUCCEEDED(g_pages[page].vb->Lock(offset * static_cast<UINT>(sizeof(GpuInstance)), bytes, &p,
                                                             D3DLOCK_NOOVERWRITE)) && p;
                if (uploaded)
                {
                    std::memcpy(p, sorted.data(), bytes);
                    g_pages[page].vb->Unlock();
                }
                else g_retired.push_back({ static_cast<uint32_t>(page), { offset, total }, g_frame });
            }
            deviceMs = grassperf::Now() - t0;
            if (!uploaded) return Need::Failed;

            e.page = page;
            e.offset = offset;
            e.allocated = total;
            e.center[0] = center[0];
            e.center[1] = center[1];
            e.sd = sd;
            e.ranges.clear();
            for (size_t g = 0; g < order.size(); ++g)
                e.ranges.push_back({ orderGeo[g], offset + start[g], (g + 1 < order.size() ? start[g + 1] : total) - start[g] });
            gd::NoteSlot(sd, overflow);
            return Need::Ok;
        }

        // --- the instanced vertex shader ----------------------------------------------------------
        bool IsTokenChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        std::string ReplaceV0(const std::string& line)
        {
            std::string out;
            for (size_t i = 0; i < line.size();)
            {
                const bool match = line.compare(i, 2, "v0") == 0
                                && (i == 0 || !IsTokenChar(line[i - 1]))
                                && (i + 2 >= line.size() || !(line[i + 2] >= '0' && line[i + 2] <= '9'));
                if (match) { out += "r31"; i += 2; }
                else out += line[i++];
            }
            return out;
        }

        std::string Trim(const std::string& s)
        {
            const size_t a = s.find_first_not_of(" \t\r");
            if (a == std::string::npos) return std::string();
            const size_t b = s.find_last_not_of(" \t\r");
            return s.substr(a, b - a + 1);
        }

        // The grass shader reads its position only as v0 (the chunk-local baked position). The
        // instanced version computes that from the model vertex (stream 0, still v0) and the
        // plant's rows (v4..v6), then runs the client's code unchanged on r31. Normal (v1), colour
        // (v2) and uv (v3) keep their registers: stream 1 carries the first two, stream 0 the uv.
        bool MakeInstanced(const std::string& src, std::string& out, std::string& error)
        {
            for (const char* reg : { "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31",
                                     "c153", "c154", "c155" })
                if (src.find(reg) != std::string::npos) { error = std::string("the shader already uses ") + reg; return false; }

            std::vector<std::string> lines;
            for (size_t at = 0; at <= src.size();)
            {
                size_t end = src.find('\n', at);
                if (end == std::string::npos) end = src.size();
                lines.push_back(src.substr(at, end - at));
                at = end + 1;
            }

            size_t lastDcl = std::string::npos;
            bool pos = false, nrm = false, col = false, tex = false;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string t = Trim(lines[i]);
                if (t.compare(0, 4, "dcl_") != 0) continue;
                lastDcl = i;
                if (t.find(" v") == std::string::npos) continue; // an output or sampler
                if (t == "dcl_position v0") pos = true;
                else if (t == "dcl_normal v1") nrm = true;
                else if (t == "dcl_color v2") col = true;
                else if (t == "dcl_texcoord v3") tex = true;
                else { error = "unexpected input: " + t; return false; }
            }
            if (!(pos && nrm && col && tex)) { error = "not the grass vertex layout"; return false; }

            out.clear();
            for (size_t i = 0; i < lines.size(); ++i)
            {
                out += i > lastDcl ? ReplaceV0(lines[i]) : lines[i];
                out += '\n';
                if (i == lastDcl) out += kInstancedInputs;
            }
            return true;
        }

        IDirect3DVertexShader9* DerivedShader(IDirect3DDevice9* d3d, IDirect3DVertexShader9* clientVs)
        {
            if (!clientVs) return nullptr;
            auto it = g_shaders.find(clientVs);
            if (it != g_shaders.end()) return it->second.shader;

            // Held for as long as we map it, so its address can't come back as a different shader.
            clientVs->AddRef();
            Derived& d = g_shaders[clientVs];

            UINT size = 0;
            std::vector<uint8_t> code;
            std::string text, source, error;
            std::vector<uint8_t> bin;
            if (FAILED(clientVs->GetFunction(nullptr, &size)) || size == 0) error = "GetFunction failed";
            else
            {
                code.resize(size);
                if (FAILED(clientVs->GetFunction(code.data(), &size))) error = "GetFunction failed";
            }
            if (error.empty() && shaderpatch::Disassemble(code.data(), code.size(), text, error)
                && MakeInstanced(text, source, error) && shaderpatch::Assemble(source, "grass-instanced", bin, error))
            {
                if (FAILED(d3d->CreateVertexShader(reinterpret_cast<const DWORD*>(bin.data()), &d.shader)))
                {
                    d.shader = nullptr;
                    error = "CreateVertexShader failed";
                }
            }
            if (d.shader)
                g_api->Log(WXL_LOG_INFO, kTag, "instanced grass: derived shader for the client's %u-byte grass shader (%u bytes).",
                           size, static_cast<unsigned>(bin.size()));
            else
                g_api->Log(WXL_LOG_WARN, kTag, "instanced grass: no instanced version of the bound %u-byte shader (%s); those layers draw the client's way.",
                           size, error.c_str());
            return d.shader;
        }

        // Stream 0: model vertex (position, uv). Stream 1: the plant (rows, normal, colour). Normal,
        // colour and uv keep the client's own element types, so the shader sees the same values.
        bool EnsureDecl(IDirect3DDevice9* d3d)
        {
            if (g_decl) return true;
            if (g_declFailed) return false;

            IDirect3DVertexDeclaration9* cur = nullptr;
            if (FAILED(d3d->GetVertexDeclaration(&cur)) || !cur) return false;
            D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
            UINT n = 0;
            const bool read = SUCCEEDED(cur->GetDeclaration(nullptr, &n)) && n <= MAXD3DDECLLENGTH + 1
                           && SUCCEEDED(cur->GetDeclaration(el, &n));
            cur->Release();
            if (!read) return false;

            int pos = -1, nrm = -1, col = -1, tex = -1, others = 0;
            for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
            {
                const D3DVERTEXELEMENT9& e = el[i];
                if (e.Stream == 0 && e.UsageIndex == 0 && e.Usage == D3DDECLUSAGE_POSITION && e.Offset == 0 && e.Type == D3DDECLTYPE_FLOAT3) pos = i;
                else if (e.Stream == 0 && e.UsageIndex == 0 && e.Usage == D3DDECLUSAGE_NORMAL && e.Offset == 12 && e.Type == D3DDECLTYPE_FLOAT3) nrm = i;
                else if (e.Stream == 0 && e.UsageIndex == 0 && e.Usage == D3DDECLUSAGE_COLOR && e.Offset == 24
                         && (e.Type == D3DDECLTYPE_D3DCOLOR || e.Type == D3DDECLTYPE_UBYTE4N || e.Type == D3DDECLTYPE_UBYTE4)) col = i;
                else if (e.Stream == 0 && e.UsageIndex == 0 && e.Usage == D3DDECLUSAGE_TEXCOORD && e.Offset == 28 && e.Type == D3DDECLTYPE_FLOAT2) tex = i;
                else ++others;
            }
            if (pos < 0 || nrm < 0 || col < 0 || tex < 0 || others)
            {
                g_declFailed = true;
                g_problem = "the client's grass vertex layout isn't the expected one (see the log)";
                g_api->Log(WXL_LOG_WARN, kTag, "instanced grass: unexpected grass vertex declaration (%u elements, pos %d normal %d colour %d uv %d, %d other); instanced path off.",
                           n, pos, nrm, col, tex, others);
                return false;
            }

            const D3DVERTEXELEMENT9 ours[] = {
                { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                { 0, 12, el[tex].Type,       D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
                { 1, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
                { 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
                { 1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
                { 1, 48, el[nrm].Type,       D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
                { 1, 60, el[col].Type,       D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
                { 2, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
                D3DDECL_END()
            };
            if (FAILED(d3d->CreateVertexDeclaration(ours, &g_decl)) || !g_decl)
            {
                g_decl = nullptr;
                g_declFailed = true;
                g_problem = "couldn't create the instanced vertex declaration";
                return false;
            }
            g_api->Log(WXL_LOG_INFO, kTag, "instanced grass: vertex declaration ready (colour type %u).", el[col].Type);
            return true;
        }

        // Before flushing, the engine's current vertex stream must be a live, filled buffer: the
        // flush re-binds it. BufStream does NOT create a buffer -- it hands every caller the
        // device's one shared stream object for that kind and re-sizes it -- so this does exactly
        // what the layer draw does each time: declare 3 grass vertices, fill them, bind. Nothing is
        // drawn from them.
        bool BindAnchor(void* gxDev)
        {
            void* buf = reinterpret_cast<BufStreamFn>(kBufStream)(gxDev, nullptr, 0, kGrassStride, 3);
            if (!buf) return false;
            void* p = EngineVirtual<BufLockFn>(gxDev, kVtBufLock)(gxDev, nullptr, buf);
            if (!p) return false;
            std::memset(p, 0, 3 * kGrassStride);
            EngineVirtual<BufUnlockFn>(gxDev, kVtBufUnlock)(gxDev, nullptr, buf, 0);
            static_cast<uint8_t*>(buf)[kBufBuilt] = 1; // as the layer build does after its unlock
            reinterpret_cast<PrimVertexPtrFn>(kPrimVertexPtr)(buf, kGrassFormat);
            return true;
        }

        // Multiplier m's region holds kCopyTableSpan groups of m entries: entry 0 of a group is the
        // original (weight 0), the rest a random point of the triangle (a, b in 0..1) and a random
        // turn in -pi..pi (what sincos takes).
        bool EnsureCopyTable(IDirect3DDevice9* d3d)
        {
            if (g_copyTable) return true;
            uint32_t total = 0;
            for (unsigned m = 1; m <= kMaxMultiplier; ++m) { g_copyBase[m] = total; total += kCopyTableSpan * m; }
            std::vector<CopyEntry> table(total);
            uint32_t state = 0x6A09E667u;
            for (unsigned m = 1; m <= kMaxMultiplier; ++m)
                for (uint32_t g = 0; g < kCopyTableSpan; ++g)
                    for (unsigned k = 0; k < m; ++k)
                    {
                        CopyEntry& c = table[g_copyBase[m] + g * m + k];
                        if (k == 0) { c = { 0.0f, 0.0f, 0.0f, 0.0f }; continue; }
                        c = { Unit(state), Unit(state), (Unit(state) - 0.5f) * 6.2831853f, 1.0f };
                    }
            const UINT bytes = total * static_cast<UINT>(sizeof(CopyEntry));
            if (FAILED(d3d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_copyTable, nullptr))
                || !Upload(g_copyTable, table.data(), bytes))
            {
                Release(g_copyTable);
                g_copyTable = nullptr;
                return false;
            }
            return true;
        }

        // Full multiplier within half the radius, fading to 1 at the radius.
        unsigned LayerMultiplier(const SlotEntry& e)
        {
            const float mult = g_settings.densityMultiplier;
            if (mult < 1.5f || !g_chunkOriginValid) return 1;
            const world::Snapshot& w = world::Current();
            if (!w.inWorld) return 1;
            const float dx = g_chunkOrigin[0] + e.center[0] - w.playerPos[0];
            const float dy = g_chunkOrigin[1] + e.center[1] - w.playerPos[1];
            const float r = g_settings.densityRadius > 1.0f ? g_settings.densityRadius : 1.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float full = r * 0.5f;
            const float t = d <= full ? 1.0f : (d >= r ? 0.0f : 1.0f - (d - full) / (r - full));
            const unsigned m = static_cast<unsigned>(1.0f + (mult - 1.0f) * t + 0.5f);
            return m < 1 ? 1 : (m > kMaxMultiplier ? kMaxMultiplier : m);
        }

        bool CheckCaps(IDirect3DDevice9* d3d)
        {
            if (g_capsChecked) return g_problem == nullptr;
            g_capsChecked = true;
            D3DCAPS9 caps{};
            if (FAILED(d3d->GetDeviceCaps(&caps))) { g_problem = "couldn't read the device caps"; return false; }
            if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) { g_problem = "hardware instancing needs vs_3_0"; return false; }
            if (!(caps.DevCaps2 & D3DDEVCAPS2_STREAMOFFSET)) { g_problem = "the device can't offset vertex streams"; return false; }
            return true;
        }

        // --- the bake verification ----------------------------------------------------------------
        void* __fastcall hkLock(void* dev, void* edx, void* buf)
        {
            void* p = g_origLock(dev, edx, buf);
            if (g_cap.armed && buf == g_cap.buf) g_cap.ptr = static_cast<uint8_t*>(p);
            return p;
        }

        uint32_t __fastcall hkUnlock(void* dev, void* edx, void* buf, int arg)
        {
            if (g_cap.armed && buf == g_cap.buf && g_cap.ptr)
            {
                g_cap.data.resize(static_cast<size_t>(g_cap.vertices) * kBakedStride);
                g_cap.done = SafeCopy(g_cap.data.data(), reinterpret_cast<uintptr_t>(g_cap.ptr), g_cap.data.size());
                g_cap.ptr = nullptr;
            }
            return g_origUnlock(dev, edx, buf, arg);
        }

        void Compare(void* slot)
        {
            char line[512];
            const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            uint32_t count = 0, instances = 0;
            BakeConstants k;
            if (!ReadU32(s + kSlotInstanceCount, count) || !ReadU32(s + kSlotInstances, instances) || !instances
                || !ReadBakeConstants(k))
            { g_verifyReport = "couldn't read the slot back"; return; }
            std::vector<uint8_t> recs(static_cast<size_t>(count) * kRecStride);
            if (!SafeCopy(recs.data(), instances, recs.size())) { g_verifyReport = "couldn't read the instances"; return; }

            std::unordered_map<uint32_t, ModelData> models;
            TiltCache tilt;
            size_t v = 0;
            const size_t captured = g_cap.data.size() / kBakedStride;
            double worstPos = 0; uint32_t worstPlant = 0;
            unsigned badNormal = 0, badColor = 0, badUv = 0, tilted = 0;
            uint32_t firstExpected = 0, firstGot = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* rec = &recs[static_cast<size_t>(i) * kRecStride];
                uint32_t id, color;
                std::memcpy(&id, rec + kRecDoodad, sizeof(id));
                std::memcpy(&color, rec + kRecColor, sizeof(color));
                auto mit = models.find(id);
                if (mit == models.end())
                {
                    ModelData m;
                    if (ReadModel(id, m) != ModelRead::Ok)
                    { std::snprintf(line, sizeof(line), "couldn't read doodad %u's model", id); g_verifyReport = line; return; }
                    mit = models.emplace(id, std::move(m)).first;
                }
                const ModelData& m = mit->second;
                const bool isTilted = (m.rowFlags & kRowFlagTilt) != 0;
                tilted += isTilted ? 1 : 0;

                GpuInstance g;
                PlantTransform(rec, isTilted, tilt, k, g);
                g.color = BakeColor(color, k); // the records are already tagged during the client's build

                for (const GpuVertex& mv : m.verts)
                {
                    if (v >= captured) break;
                    const uint8_t* got = &g_cap.data[v * kBakedStride];
                    float p[3], n[3], uv[2]; uint32_t c;
                    std::memcpy(p, got, 12); std::memcpy(n, got + 12, 12); std::memcpy(&c, got + 24, 4); std::memcpy(uv, got + 28, 8);
                    const double e[3] = {
                        double(g.x[0]) * mv.pos[0] + double(g.x[1]) * mv.pos[1] + double(g.x[2]) * mv.pos[2] + g.x[3],
                        double(g.y[0]) * mv.pos[0] + double(g.y[1]) * mv.pos[1] + double(g.y[2]) * mv.pos[2] + g.y[3],
                        double(g.z[0]) * mv.pos[0] + double(g.z[1]) * mv.pos[1] + double(g.z[2]) * mv.pos[2] + g.z[3],
                    };
                    for (int a = 0; a < 3; ++a)
                    {
                        const double d = std::fabs(e[a] - p[a]);
                        if (!(d <= worstPos)) { worstPos = d; worstPlant = i; } // NaN counts as worst
                    }
                    if (std::memcmp(n, g.normal, 12) != 0) ++badNormal;
                    if (c != g.color) { if (!badColor) { firstExpected = g.color; firstGot = c; } ++badColor; }
                    if (std::memcmp(uv, mv.uv, 8) != 0) ++badUv;
                    ++v;
                }
            }

            const bool countOk = v == captured;
            const bool match = countOk && worstPos < 1e-3 && !badNormal && !badColor && !badUv;
            std::snprintf(line, sizeof(line),
                          "%s -- %u plants (%u tilted), %u of %u vertices compared; worst position error %.6f yd (plant %u); "
                          "normals off %u, colours off %u, uvs off %u%s",
                          match ? "MATCH" : "MISMATCH", count, tilted, static_cast<unsigned>(v), static_cast<unsigned>(captured),
                          worstPos, worstPlant, badNormal, badColor, badUv,
                          countOk ? "" : " -- VERTEX COUNT DIFFERS");
            g_verifyReport = line;
            if (badColor)
            {
                std::snprintf(line, sizeof(line), " (first colour: expected %08X, client wrote %08X)", firstExpected, firstGot);
                g_verifyReport += line;
            }
            g_api->Log(match ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "instanced grass verification: %s", g_verifyReport.c_str());
        }
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        // Engine buffer lock/unlock, only to capture one slot's bake for the verification.
        const uintptr_t* vtbl = reinterpret_cast<const uintptr_t*>(kGxDeviceVtable);
        const int a = api->HookAttach("LivingAzeroth.GxBufLock", vtbl[kVtBufLock / 4], reinterpret_cast<void*>(&hkLock),
                                      reinterpret_cast<void**>(&g_origLock), WXL_HOOK_DEFAULT_PRIORITY);
        const int b = api->HookAttach("LivingAzeroth.GxBufUnlock", vtbl[kVtBufUnlock / 4], reinterpret_cast<void*>(&hkUnlock),
                                      reinterpret_cast<void**>(&g_origUnlock), WXL_HOOK_DEFAULT_PRIORITY);
        g_captureHooked = a && b;
        if (!g_captureHooked)
            api->Log(WXL_LOG_WARN, kTag, "instanced grass: buffer capture hooks failed; the bake verification is unavailable.");
    }

    Settings& Tunables() { return g_settings; }

    const grassdoodads::SlotDoodads* Prepare(void* slot)
    {
        if (!g_settings.enabled || g_problem || g_cap.pending) return nullptr;
        auto* d3d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!d3d || !CheckCaps(d3d)) return nullptr;

        const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
        uint32_t count = 0, instances = 0;
        if (!ReadU32(s + kSlotInstanceCount, count) || !ReadU32(s + kSlotInstances, instances) || !instances
            || count == 0 || count > 65536)
            return nullptr;
        bool ok = false;
        const uint32_t fp = Fingerprint(instances, count, ok);
        if (!ok) return nullptr;

        auto found = g_slots.find(slot);
        const bool current = found != g_slots.end() && found->second.page >= 0 && found->second.instances == instances
                          && found->second.count == count && found->second.fingerprint == fp;
        if (!current)
        {
            // Layers coming into view all want building at once (turning the camera at long
            // range): over the frame's budget they draw the client's way until a later frame.
            if (g_frameBuildMs >= g_settings.buildBudgetMs) { ++g_frameStats.fallbackBudget; return nullptr; }

            SlotEntry& e = g_slots[slot];
            ReleaseSlot(e);
            const double t0 = grassperf::Now();
            double deviceMs = 0;
            const Need r = BuildSlot(d3d, instances, count, e, deviceMs);
            const double ms = grassperf::Now() - t0;
            g_frameBuildMs += ms;
            g_frameStats.buildMs += ms;
            g_frameStats.buildDeviceMs += deviceMs;
            if (r != Need::Ok)
            {
                g_slots.erase(slot);
                ++(r == Need::NotLoaded ? g_frameStats.fallbackNotLoaded : g_frameStats.fallbackFailed);
                return nullptr;
            }
            e.instances = instances;
            e.count = count;
            e.fingerprint = fp;
            ++g_frameStats.builds;
            grassperf::OnInstanceBuild(ms, deviceMs);
            found = g_slots.find(slot);
        }
        SlotEntry& e = found->second;
        e.lastFrame = g_frame;
        return &e.sd;
    }

    bool Draw(void* slot)
    {
        auto it = g_slots.find(slot);
        if (it == g_slots.end() || it->second.page < 0) return false;
        SlotEntry& e = it->second;

        void* gxDev = GxDevice();
        auto* d3d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!gxDev || !d3d) return false;
        const auto* base = static_cast<const uint8_t*>(gxDev);
        // The client's device draw does nothing in these states; neither do we.
        if (!*reinterpret_cast<const uint32_t*>(base + kDeviceReady) || *reinterpret_cast<const uint32_t*>(base + kDeviceBusy))
            return true;

        // As the layer draw (0x7B3390): no texture, nothing drawn.
        void* texture = *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + kSlotTexture);
        if (!texture) return true;
        void* gxTex = reinterpret_cast<GetGxTexFn>(kTextureGetGxTex)(texture, 0, 0);
        if (!gxTex) return true;

        if (!BindAnchor(gxDev)) { ++g_frameStats.fallbackFailed; return false; }
        reinterpret_cast<RsSetFn>(kGxRsSet)(gxDev, nullptr, kGxStateTexture0, gxTex);
        reinterpret_cast<FlushFn>(kGxFlushState)(gxDev, nullptr);

        // Everything the client would draw with is on the device now; swap in the instanced
        // shader and streams, and put back exactly what was there so the engine's view of the
        // device stays true.
        IDirect3DVertexShader9* clientVs = nullptr;
        d3d->GetVertexShader(&clientVs);
        IDirect3DVertexShader9* ours = DerivedShader(d3d, clientVs);
        if (!ours || !EnsureDecl(d3d) || !EnsureCopyTable(d3d))
        {
            Release(clientVs);
            ++g_frameStats.fallbackFailed;
            return false;
        }

        IDirect3DVertexDeclaration9* savedDecl = nullptr;
        IDirect3DVertexBuffer9* savedVb[3] = {};
        UINT savedOffset[3] = {}, savedStride[3] = {}, savedFreq[3] = { 1, 1, 1 };
        float savedCopyReg[4] = {};
        d3d->GetVertexShaderConstantF(kCopyReg, savedCopyReg, 1);
        IDirect3DIndexBuffer9* savedIb = nullptr;
        d3d->GetVertexDeclaration(&savedDecl);
        for (UINT i = 0; i < 3; ++i)
        {
            d3d->GetStreamSource(i, &savedVb[i], &savedOffset[i], &savedStride[i]);
            d3d->GetStreamSourceFreq(i, &savedFreq[i]);
        }
        d3d->GetIndices(&savedIb);

        d3d->SetVertexShader(ours);
        d3d->SetVertexDeclaration(g_decl);
        const unsigned m = LayerMultiplier(e);
        const float copyReg[4] = { g_settings.densitySpread, 0.0f, 0.0f, 0.0f };
        d3d->SetVertexShaderConstantF(kCopyReg, copyReg, 1);
        // A per-layer start in the table, so layers don't all repeat the same copy pattern.
        const uint32_t group0 = Mix(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot))) % (kCopyTableSpan - kCopyPlants);

        unsigned calls = 0, copies = 0;
        for (const Range& r : e.ranges)
        {
            if (!r.count || !r.geo->vb) continue;
            d3d->SetStreamSource(0, r.geo->vb, 0, sizeof(GpuVertex));
            d3d->SetIndices(r.geo->ib);
            for (uint32_t first = 0; first < r.count; first += kCopyPlants)
            {
                const uint32_t n = (r.count - first < kCopyPlants) ? r.count - first : kCopyPlants;
                d3d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | (n * m));
                d3d->SetStreamSource(1, g_pages[e.page].vb, (r.start + first) * sizeof(GpuInstance), sizeof(GpuInstance));
                d3d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | m);
                d3d->SetStreamSource(2, g_copyTable, (g_copyBase[m] + group0 * m) * sizeof(CopyEntry), sizeof(CopyEntry));
                d3d->SetStreamSourceFreq(2, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
                d3d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, r.geo->vertices, 0, r.geo->indices / 3);
                ++calls;
                copies += n * (m - 1);
            }
        }
        d3d->SetVertexShaderConstantF(kCopyReg, savedCopyReg, 1);
        if (m > 1) { ++g_frameStats.densifiedLayers; g_frameStats.densityCopies += copies; }

        for (UINT i = 0; i < 3; ++i)
        {
            d3d->SetStreamSourceFreq(i, savedFreq[i]);
            d3d->SetStreamSource(i, savedVb[i], savedOffset[i], savedStride[i]);
            Release(savedVb[i]);
        }
        d3d->SetIndices(savedIb);
        d3d->SetVertexDeclaration(savedDecl);
        d3d->SetVertexShader(clientVs);
        Release(savedIb);
        Release(savedDecl);
        Release(clientVs);

        ++g_frameStats.slotsInstanced;
        g_frameStats.drawCalls += calls;
        grassperf::OnInstancedDraw(calls, e.allocated);
        return true;
    }

    void BeforeStockFill(void* slot)
    {
        if (!g_cap.pending) return;
        if (!g_captureHooked)
        {
            g_verifyReport = "unavailable: the buffer capture hooks aren't installed";
            g_cap.pending = false;
            return;
        }
        void* gxDev = GxDevice();
        const uintptr_t* vtbl = reinterpret_cast<const uintptr_t*>(kGxDeviceVtable);
        if (!gxDev || (*reinterpret_cast<uintptr_t**>(gxDev))[kVtBufLock / 4] != vtbl[kVtBufLock / 4])
        {
            g_verifyReport = "unavailable: the live graphics device isn't the class the capture hooks cover";
            g_cap.pending = false;
            return;
        }
        const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
        uint32_t vb = 0, vertices = 0;
        if (!ReadU32(s + kSlotVB, vb) || !vb || !ReadU32(s + kSlotVertexCount, vertices) || !vertices || vertices > 1000000)
            return; // try the next build
        g_cap.armed = true;
        g_cap.done = false;
        g_cap.buf = reinterpret_cast<void*>(vb);
        g_cap.vertices = vertices;
        g_cap.ptr = nullptr;
        g_cap.data.clear();
    }

    void AfterStockFill(void* slot)
    {
        if (!g_cap.armed) return;
        g_cap.armed = false;
        g_cap.pending = false;
        if (!g_cap.done) { g_verifyReport = "capture failed: the build didn't write the expected buffer"; return; }
        Compare(slot);
    }

    void OnFrameEnd()
    {
        g_lastStats = g_frameStats;
        g_frameStats = Stats{};
        g_frameBuildMs = 0;
        ++g_frame;

        if (!g_settings.enabled)
        {
            if (!g_slots.empty() || !g_pages.empty())
            {
                g_slots.clear();
                ReleasePool();
            }
            return;
        }
        // A new map: nothing cached is coming back.
        const int mapId = world::Current().mapId;
        if (mapId != g_lastMapId)
        {
            g_lastMapId = mapId;
            g_slots.clear();
            ReleasePool();
            g_memoryTight = false;
            return;
        }

        // At the cap, layers not drawn for a second go at once; otherwise the slow sweep.
        const uint32_t evictAfter = g_memoryTight ? kEvictAfterTight : kEvictAfter;
        if (g_memoryTight || g_frame % kEvictEvery == 0)
            for (auto it = g_slots.begin(); it != g_slots.end();)
            {
                if (g_frame - it->second.lastFrame > evictAfter) { ReleaseSlot(it->second); it = g_slots.erase(it); }
                else ++it;
            }

        // Ranges freed long enough ago that no queued frame still reads them.
        for (size_t i = 0; i < g_retired.size();)
        {
            if (g_frame - g_retired[i].frame > kReuseDelay)
            {
                Reclaim(g_retired[i].page, g_retired[i].block);
                g_retired[i] = g_retired.back();
                g_retired.pop_back();
            }
            else ++i;
        }

        // Pages with nothing left in them go back to the driver.
        for (Page& p : g_pages)
            if (p.vb && p.free.size() == 1 && p.free[0].offset == 0 && p.free[0].size == kPageInstances)
            {
                Release(p.vb);
                p.vb = nullptr;
                p.free.clear();
            }
        g_memoryTight = LivePages() >= PageLimit();
    }

    void OnDeviceLost()
    {
        // Our buffers are D3DPOOL_DEFAULT: all go before the device resets, and get rebuilt on
        // demand. Shaders and the declaration survive a reset.
        g_slots.clear();
        ReleasePool();
        Release(g_copyTable);
        g_copyTable = nullptr;
        for (auto& [id, g] : g_geometry) { Release(g.vb); Release(g.ib); }
        g_geometry.clear();
    }

    Stats GetStats()
    {
        Stats s = g_lastStats;
        s.slotsCached = static_cast<unsigned>(g_slots.size());
        size_t instanceBytes = 0, geometryBytes = 0;
        for (const auto& [slot, e] : g_slots) instanceBytes += static_cast<size_t>(e.allocated) * sizeof(GpuInstance);
        s.poolPages = LivePages();
        s.poolLimitPages = PageLimit();
        s.memoryTight = g_memoryTight;
        for (const auto& [id, g] : g_geometry)
            if (g.vb) { ++s.geometries; geometryBytes += g.vertices * sizeof(GpuVertex) + g.indices * sizeof(uint16_t); }
        s.instanceMB = instanceBytes / (1024.0 * 1024.0);
        s.geometryKB = geometryBytes / 1024.0;
        for (const auto& [vs, d] : g_shaders) if (d.shader) ++s.shaders;
        return s;
    }

    const char* Problem() { return g_problem; }

    void SetChunkOrigin(const float origin[3])
    {
        std::memcpy(g_chunkOrigin, origin, sizeof(g_chunkOrigin));
        g_chunkOriginValid = true;
    }



    void RequestVerify()
    {
        g_cap.pending = true;
        g_verifyReport = "waiting for a layer build...";
    }

    bool VerifyPending() { return g_cap.pending; }
    const std::string& VerifyReport() { return g_verifyReport; }
}
