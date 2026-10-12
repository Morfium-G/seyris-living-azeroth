#include "GrassInstanced.hpp"

#include "GrassDensity.hpp"
#include "GrassPerf.hpp"

#include "../debug/Profiler.hpp"
#include "../env/WorldQuery.hpp"
#include "../render/ShaderPatch.hpp"

#include "game/Camera.hpp"
#include "game/Gx.hpp"
#include "offsets/game/GroundEffect.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
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

        // Chunk context (placement 0x7D3390): the grass object's 4 layer slots start at +4 (0x24 each)
        // and it points to its map chunk at +0x98 (the chunk points back at +0xA4). The chunk's MCNK
        // header is at +0x110 (layer count +0x0C, area ID +0x34 -- the holes the placement reads at
        // +0x3C fix the layout), its MCLY layer table at +0x12C (16 bytes each, ground effect ID
        // at +0x0C). GroundEffectTexture storage: max ID 0xAD3AF4, min ID 0xAD3AF8, row table
        // 0xAD3B08; a row's 4 doodad IDs at +0x04.
        constexpr size_t    kGrassSlotsStart = 0x04, kGrassSlotStride = 0x24;
        constexpr size_t    kGrassChunk = 0x98, kChunkGrass = 0xA4;
        constexpr size_t    kChunkHeader = 0x110, kHeaderLayers = 0x0C, kHeaderArea = 0x34;
        constexpr size_t    kChunkLayers = 0x12C, kLayerStride = 0x10, kLayerEffect = 0x0C;
        constexpr uintptr_t kEffectMaxId = 0x00AD3AF4, kEffectMinId = 0x00AD3AF8, kEffectTable = 0x00AD3B08;
        constexpr size_t    kEffectDoodads = 0x04;

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
        // Batched mode keeps the plants in float textures instead (read by the vertex shader), each
        // plant 4 texels in a row: its three transform rows, then {normal x, normal y, colour low
        // 16 bits, colour high 16 bits} (the normal's z is positive: plants only stand on slopes up
        // to normal.z >= 0.4). D3DPOOL_DEFAULT textures sit in video memory, filled through small
        // system-memory staging surfaces.
        constexpr uint32_t kTexWidth = 1024, kTexHeight = 1024;
        constexpr uint32_t kTexelsPerPlant = 4;
        constexpr uint32_t kTexPlantsPerRow = kTexWidth / kTexelsPerPlant;
        constexpr uint32_t kTexPagePlants = kTexPlantsPerRow * kTexHeight; // 256 K plants, 16 MB
        bool g_poolBatched = false; // the pool's pages are textures

        uint32_t PagePlants() { return g_poolBatched ? kTexPagePlants : kPageInstances; }
        uint32_t PageBytes()  { return PagePlants() * 64; } // both layouts: 64 bytes per plant

        struct Block { uint32_t offset, size; }; // in instances
        struct Page                               // both null = released
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            IDirect3DTexture9*      tex = nullptr;
            std::vector<Block>      free;
            bool Live() const { return vb || tex; }
        };
        struct Retired { uint32_t page; Block block; uint32_t frame; };
        std::vector<Page>    g_pages;
        std::vector<Retired> g_retired;

        struct Range
        {
            uint32_t  id;                   // GroundEffectDoodad
            Geometry* geo;
            uint32_t  start, count;         // start: instance index in the page
            grassdensity::Values density;   // from GroundEffectDoodadDensity.cdbc (uncapped)
        };
        // A slot's change check: its first, middle and last record, compared as they are (checked
        // every draw, so a plain compare instead of a hash).
        struct SlotSample { uint8_t recs[3][kRecStride]; };

        struct SlotEntry
        {
            uintptr_t instances = 0;
            uint32_t  count = 0;
            SlotSample sample{};
            int       page = -1; // -1 = nothing allocated
            uint32_t  offset = 0;
            uint32_t  allocated = 0;  // instances in the page range
            float     center[3] = {}; // chunk-local centre of the plants (density and thinning distance)
            uint32_t  densityGeneration = 0; // GroundEffectDoodadDensity.cdbc load it resolved against
            uintptr_t slot = 0;              // the client's layer slot (for its chunk context)
            gd::SlotDoodads sd{};
            std::vector<Range> ranges;
            uint32_t  lastFrame = 0;
        };
        std::unordered_map<const void*, SlotEntry> g_slots;

        struct Derived { IDirect3DVertexShader9* shader = nullptr; };
        std::unordered_map<IDirect3DVertexShader9*, Derived> g_shaders; // key: the client's grass shader (held)
        std::unordered_map<IDirect3DVertexShader9*, Derived> g_batchShaders; // the batched versions
        IDirect3DVertexDeclaration9* g_decl = nullptr;
        IDirect3DVertexDeclaration9* g_batchDecl = nullptr;
        bool  g_declFailed = false;
        BYTE  g_colorType = D3DDECLTYPE_D3DCOLOR; // the client's grass colour element type

        // --- batched mode state ----------------------------------------------------------------
        const char* g_batchProblem = nullptr; // batched draws unavailable on this device (reset with it)
        const char* g_hookProblem = nullptr;  // the end-of-pass hook is missing (permanent)
        bool  g_vtfChecked = false;
        IDirect3DTexture9*      g_copyTex = nullptr;  // random copy entries (a, b, turn, 1), kCopyTexSize x 1
        IDirect3DVertexBuffer9* g_iota = nullptr;     // 0, 1, 2, ... (float): the instance counter
        IDirect3DStateBlock9*   g_grassBlock = nullptr; // the client's grass state, captured each frame
        IDirect3DStateBlock9*   g_savedBlock = nullptr; // whatever was bound when the batches draw
        bool  g_frameCaptured = false;                  // g_grassBlock holds this frame's grass state
        IDirect3DVertexShader9* g_frameVs = nullptr;    // this frame's batched shader

        // Per layer texture, learned once a frame through one client state flush: the D3D texture
        // and the sampler states the client sets for it.
        constexpr D3DSAMPLERSTATETYPE kTexSamplerStates[] = {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY };
        constexpr size_t kTexSamplerCount = sizeof(kTexSamplerStates) / sizeof(kTexSamplerStates[0]);
        struct TexInfo { IDirect3DBaseTexture9* tex = nullptr; DWORD sampler[kTexSamplerCount] = {}; };
        std::unordered_map<void*, TexInfo> g_texInfo; // key: the client's texture object; cleared each frame

        // One layer range waiting for the combined draw.
        struct BatchItem
        {
            void*     gxTex;
            Geometry* geo;
            int       page;
            uint32_t  doodad;
            uint32_t  first, plants, multiplier; // first plant in the page, plants drawn, copies per plant
            float     spread;
            float     origin[3];                 // the chunk's c3: its origin in view space
        };
        std::vector<BatchItem> g_items;

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
        bool ReadSample(uintptr_t instances, uint32_t count, SlotSample& out)
        {
            const uint32_t pick[3] = { 0, count / 2, count - 1 };
            for (int i = 0; i < 3; ++i)
                if (!SafeCopy(out.recs[i], instances + pick[i] * kRecStride, kRecStride)) return false;
            return true;
        }

        unsigned LivePages();
        unsigned PageLimit();

        bool Allocate(IDirect3DDevice9* d3d, uint32_t count, int& page, uint32_t& offset)
        {
            for (size_t p = 0; p < g_pages.size(); ++p)
            {
                if (!g_pages[p].Live()) continue;
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
            IDirect3DTexture9* tex = nullptr;
            if (g_poolBatched)
            {
                if (FAILED(d3d->CreateTexture(kTexWidth, kTexHeight, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &tex, nullptr)))
                    return false;
            }
            else if (FAILED(d3d->CreateVertexBuffer(PageBytes(), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb, nullptr)))
                return false;
            size_t p = 0;
            while (p < g_pages.size() && g_pages[p].Live()) ++p; // reuse a released page's index
            if (p == g_pages.size()) g_pages.emplace_back();
            g_pages[p].vb = vb;
            g_pages[p].tex = tex;
            g_pages[p].free.assign(1, { count, PagePlants() - count });
            page = static_cast<int>(p);
            offset = 0;
            return true;
        }

        unsigned LivePages()
        {
            unsigned n = 0;
            for (const Page& p : g_pages) n += p.Live() ? 1 : 0;
            return n;
        }

        unsigned PageLimit()
        {
            const float mb = g_settings.memoryLimitMB < 16.0f ? 16.0f : g_settings.memoryLimitMB;
            const unsigned n = static_cast<unsigned>(mb * 1024.0f * 1024.0f / PageBytes());
            return n ? n : 1;
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
            for (Page& p : g_pages) { Release(p.vb); Release(p.tex); }
            g_pages.clear();
            g_retired.clear();
        }

        // Staging surfaces of texture uploads: kept until the copies they feed have surely run.
        struct Staging { IDirect3DSurface9* surface; uint32_t frame; };
        std::vector<Staging> g_staging;

        void ReleaseStaging(bool all)
        {
            for (size_t i = 0; i < g_staging.size();)
                if (all || g_frame - g_staging[i].frame > kReuseDelay)
                {
                    Release(g_staging[i].surface);
                    g_staging[i] = g_staging.back();
                    g_staging.pop_back();
                }
                else ++i;
        }

        // Plants [offset, offset + n) of a texture page, 4 texels each, rows of kTexPlantsPerRow:
        // written into a staging surface covering the rows they touch, then copied in at most
        // three rectangles (first row's tail, full rows, last row's head) so neighbouring ranges
        // are never overwritten.
        bool UploadToTexture(IDirect3DDevice9* d3d, Page& page, uint32_t offset, const GpuInstance* plants, uint32_t n)
        {
            constexpr uint32_t P = kTexPlantsPerRow;
            const uint32_t row0 = offset / P, row1 = (offset + n - 1) / P;
            IDirect3DSurface9* staging = nullptr;
            if (FAILED(d3d->CreateOffscreenPlainSurface(kTexWidth, row1 - row0 + 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                                        &staging, nullptr)))
                return false;
            D3DLOCKED_RECT lr{};
            if (FAILED(staging->LockRect(&lr, nullptr, 0)) || !lr.pBits) { staging->Release(); return false; }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t p = offset + i;
                float* t = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + (p / P - row0) * lr.Pitch) + (p % P) * 16;
                const GpuInstance& g = plants[i];
                std::memcpy(t, g.x, 16); std::memcpy(t + 4, g.y, 16); std::memcpy(t + 8, g.z, 16);
                t[12] = g.normal[0];
                t[13] = g.normal[1];
                t[14] = static_cast<float>(g.color & 0xFFFFu);
                t[15] = static_cast<float>(g.color >> 16);
            }
            staging->UnlockRect();

            IDirect3DSurface9* dst = nullptr;
            if (FAILED(page.tex->GetSurfaceLevel(0, &dst)) || !dst) { staging->Release(); return false; }
            bool ok = true;
            auto copy = [&](uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1) // rows [r0, r1), plants [c0, c1)
            {
                const RECT src = { static_cast<LONG>(c0 * kTexelsPerPlant), static_cast<LONG>(r0 - row0),
                                   static_cast<LONG>(c1 * kTexelsPerPlant), static_cast<LONG>(r1 - row0) };
                const POINT at = { static_cast<LONG>(c0 * kTexelsPerPlant), static_cast<LONG>(r0) };
                ok &= SUCCEEDED(d3d->UpdateSurface(staging, &src, dst, &at));
            };
            const uint32_t c0 = offset % P, c1 = (offset + n - 1) % P + 1;
            if (row0 == row1) copy(row0, row0 + 1, c0, c1);
            else
            {
                copy(row0, row0 + 1, c0, P);
                if (row1 > row0 + 1) copy(row0 + 1, row1, 0, P);
                copy(row1, row1 + 1, 0, c1);
            }
            dst->Release();
            g_staging.push_back({ staging, g_frame });
            return ok;
        }

        struct ChunkContext
        {
            bool     valid = false;
            uint32_t area = 0;
            uint32_t effects[4] = {};
            unsigned effectCount = 0;
        };

        // Which chunk a layer slot belongs to: the slot is one of its grass object's 4; the right
        // one is the grass object whose chunk points back at it.
        ChunkContext ReadChunkContext(const void* slot)
        {
            ChunkContext c;
            const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            for (uintptr_t k = 0; k < 4; ++k)
            {
                const uintptr_t grass = s - kGrassSlotsStart - k * kGrassSlotStride;
                uint32_t chunk = 0, back = 0, header = 0, layers = 0, count = 0;
                if (!ReadU32(grass + kGrassChunk, chunk) || !chunk || !ReadU32(chunk + kChunkGrass, back) || back != grass) continue;
                if (!ReadU32(chunk + kChunkHeader, header) || !header || !ReadU32(header + kHeaderArea, c.area)) return c;
                if (ReadU32(header + kHeaderLayers, count) && ReadU32(chunk + kChunkLayers, layers) && layers)
                    for (uint32_t i = 0; i < count && i < 4; ++i)
                        if (ReadU32(layers + i * kLayerStride + kLayerEffect, c.effects[c.effectCount]) && c.effects[c.effectCount])
                            ++c.effectCount;
                c.valid = true;
                return c;
            }
            return c;
        }

        // The ground effect (of the chunk's layers) that lists this doodad; 0 = none found.
        uint32_t EffectForDoodad(const ChunkContext& c, uint32_t doodad)
        {
            int32_t minId = 0, maxId = 0;
            uint32_t table = 0;
            if (!ReadU32(kEffectMinId, reinterpret_cast<uint32_t&>(minId)) || !ReadU32(kEffectMaxId, reinterpret_cast<uint32_t&>(maxId))
                || !ReadU32(kEffectTable, table) || !table)
                return 0;
            for (unsigned i = 0; i < c.effectCount; ++i)
            {
                const int32_t id = static_cast<int32_t>(c.effects[i]);
                if (id < minId || id > maxId) continue;
                uint32_t row = 0;
                if (!ReadU32(table + static_cast<uint32_t>(id - minId) * 4, row) || !row) continue;
                for (uint32_t d = 0; d < 4; ++d)
                {
                    uint32_t listed = 0;
                    if (ReadU32(row + kEffectDoodads + d * 4, listed) && listed == doodad) return c.effects[i];
                }
            }
            return 0;
        }

        // --- debug: the terrain under the player ---------------------------------------------
        // Placement (0x7D3390) picks each cell's layer from the chunk's low-res layer map at
        // chunk +0x114: one u16 per cell row, 2 bits per cell, with a mask/shift table per column
        // (0xA3FB88 u16 masks, 0xA3FB98 shift bytes at stride 4). Row index from the cell along
        // local x, column from local y (as the placement's own reads pair them).
        constexpr size_t    kChunkLowResLayers = 0x114;
        constexpr uintptr_t kLowResMasks = 0x00A3FB88, kLowResShifts = 0x00A3FB98;
        constexpr float     kCellYards = 33.3333333f / 8.0f;
        constexpr size_t    kEffectTerrainType = 0x28;
        TerrainProbe        g_probe;

        int EffectTerrainType(uint32_t effectId)
        {
            int32_t minId = 0, maxId = 0;
            uint32_t table = 0, row = 0, value = 0;
            if (!ReadU32(kEffectMinId, reinterpret_cast<uint32_t&>(minId)) || !ReadU32(kEffectMaxId, reinterpret_cast<uint32_t&>(maxId))
                || !ReadU32(kEffectTable, table) || !table)
                return -1;
            const int32_t id = static_cast<int32_t>(effectId);
            if (id < minId || id > maxId) return -1;
            if (!ReadU32(table + static_cast<uint32_t>(id - minId) * 4, row) || !row) return -1;
            return ReadU32(row + kEffectTerrainType, value) ? static_cast<int>(value) : -1;
        }

        int LowResLayer(uint32_t chunk, int row, int col)
        {
            uint32_t map = 0, word = 0, mask = 0, shift = 0;
            if (row < 0 || row > 7 || col < 0 || col > 7) return -1;
            if (!ReadU32(chunk + kChunkLowResLayers, map) || !map) return -1;
            if (!ReadU32(map + row * 2, word) || !ReadU32(kLowResMasks + col * 2, mask) || !ReadU32(kLowResShifts + col * 4, shift))
                return -1;
            return static_cast<int>(((word & 0xFFFF) & (mask & 0xFFFF)) >> (shift & 0xFF));
        }

        void ProbeIfPlayerChunk(const void* slot)
        {
            if (!g_chunkOriginValid || g_probe.frame == g_frame) return;
            const world::Snapshot& w = world::Current();
            if (!w.inWorld) return;
            const float lx = w.playerPos[0] - g_chunkOrigin[0], ly = w.playerPos[1] - g_chunkOrigin[1];
            if (lx > 0.0f || ly > 0.0f || lx <= -33.3334f || ly <= -33.3334f) return;

            const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            for (uintptr_t k = 0; k < 4; ++k)
            {
                const uintptr_t grass = s - kGrassSlotsStart - k * kGrassSlotStride;
                uint32_t chunk = 0, back = 0, header = 0, layers = 0, count = 0;
                if (!ReadU32(grass + kGrassChunk, chunk) || !chunk || !ReadU32(chunk + kChunkGrass, back) || back != grass) continue;
                TerrainProbe p;
                p.valid = true;
                p.frame = g_frame;
                p.chunk = chunk;
                p.local[0] = lx; p.local[1] = ly;
                p.cellA[0] = static_cast<int>(-lx / kCellYards);
                p.cellA[1] = static_cast<int>(-ly / kCellYards);
                if (ReadU32(chunk + kChunkHeader, header) && header && ReadU32(header + kHeaderLayers, count)
                    && ReadU32(chunk + kChunkLayers, layers) && layers)
                {
                    p.layers = count > 4 ? 4 : count;
                    for (unsigned i = 0; i < p.layers; ++i)
                        if (ReadU32(layers + i * kLayerStride + kLayerEffect, p.effect[i]) && p.effect[i])
                            p.effectTerrain[i] = EffectTerrainType(p.effect[i]);
                }
                p.dominantA = LowResLayer(chunk, p.cellA[0], p.cellA[1]);
                p.dominantB = LowResLayer(chunk, p.cellA[1], p.cellA[0]);
                g_probe = p;
                return;
            }
        }

        ChunkContext g_lastContext; // of the last layer built (panel)
        unsigned g_lastEffectsFound = 0, g_lastDoodads = 0;

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

            // The plants' centre, for the per-draw density and thinning distances.
            float center[3] = {};
            for (uint32_t i = 0; i < count; ++i) { center[0] += plants[i].x[3]; center[1] += plants[i].y[3]; center[2] += plants[i].z[3]; }
            center[0] /= count; center[1] /= count; center[2] /= count;
            const uint32_t total = static_cast<uint32_t>(plants.size());

            // Group the plants by doodad so each doodad's plants are one contiguous instance range.
            std::vector<uint32_t> start(order.size(), 0), fill(order.size(), 0);
            for (uint32_t g : group) ++fill[g];
            for (size_t g = 1; g < order.size(); ++g) start[g] = start[g - 1] + fill[g - 1];
            std::vector<GpuInstance> sorted(total);
            std::copy(start.begin(), start.end(), fill.begin());
            for (uint32_t i = 0; i < total; ++i) sorted[fill[group[i]]++] = plants[i];

            // Shuffled within each doodad's range (the draw order doesn't matter otherwise), so
            // drawing only the first part of a range thins it evenly (distance thinning). Seeded by
            // the slot, so a rebuild of the same layer keeps the same plants at a given share.
            uint32_t seed = Mix(static_cast<uint32_t>(e.slot) ^ count);
            for (size_t g = 0; g < order.size(); ++g)
            {
                const uint32_t first = start[g], n = (g + 1 < order.size() ? start[g + 1] : total) - first;
                for (uint32_t i = n; i > 1; --i)
                {
                    seed = Mix(seed + 0x9E3779B9u);
                    std::swap(sorted[first + i - 1], sorted[first + seed % i]);
                }
            }

            // Into a pool range: the range is free, so the GPU isn't reading it (NOOVERWRITE).
            const double t0 = grassperf::Now();
            int page = -1;
            uint32_t offset = 0;
            bool uploaded = Allocate(d3d, total, page, offset);
            if (uploaded && g_poolBatched)
            {
                uploaded = UploadToTexture(d3d, g_pages[page], offset, sorted.data(), total);
                if (!uploaded) g_retired.push_back({ static_cast<uint32_t>(page), { offset, total }, g_frame });
            }
            else if (uploaded)
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
            e.center[2] = center[2];
            e.sd = sd;
            // Density content per doodad, from the chunk's area, the map and the ground effect that
            // placed it (resolved once per build; the player's caps apply per draw).
            const ChunkContext ctx = ReadChunkContext(reinterpret_cast<const void*>(e.slot));
            const int mapId = world::Current().mapId;
            unsigned effectsFound = 0;
            e.ranges.clear();
            for (size_t g = 0; g < order.size(); ++g)
            {
                Range r{ order[g], orderGeo[g], offset + start[g], (g + 1 < order.size() ? start[g + 1] : total) - start[g], {} };
                const uint32_t effect = ctx.valid ? EffectForDoodad(ctx, order[g]) : 0;
                effectsFound += effect ? 1 : 0;
                r.density = grassdensity::Resolve(ctx.valid ? ctx.area : 0, mapId, order[g], effect);
                e.ranges.push_back(r);
            }
            e.densityGeneration = grassdensity::Generation();
            g_lastContext = ctx;
            g_lastEffectsFound = effectsFound;
            g_lastDoodads = static_cast<unsigned>(order.size());
            gd::NoteSlot(sd, overflow);
            return Need::Ok;
        }

        // --- the instanced vertex shader ----------------------------------------------------------
        bool IsTokenChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        // A register name as a whole token: "v1" but not "v10", "c3" but not "c35".
        std::string ReplaceToken(const std::string& line, const char* token, const char* with)
        {
            const size_t n = std::strlen(token);
            std::string out;
            for (size_t i = 0; i < line.size();)
            {
                const bool match = line.compare(i, n, token) == 0
                                && (i == 0 || !IsTokenChar(line[i - 1]))
                                && (i + n >= line.size() || !(line[i + n] >= '0' && line[i + n] <= '9'));
                if (match) { out += with; i += n; }
                else out += line[i++];
            }
            return out;
        }

        std::string ReplaceV0(const std::string& line) { return ReplaceToken(line, "v0", "r31"); }

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

        // --- the batched vertex shader ------------------------------------------------------------
        // One draw holds up to 32 layer ranges of one doodad model. Stream 0 is the model vertex
        // (v0, uv v3), stream 1 an instance counter (v8). The shader finds its range in the table
        // (c160 + 2r: {instances end, instances start, first plant, multiplier},
        //  c161 + 2r: {chunk origin in view space xyz (the client's c3), copy spread})
        // by a 5-step binary search, works out its plant and copy, reads the plant's 4 texels from
        // the page texture (s0) and the copy's entry from a random table (s1), then runs the same
        // copy/transform code as the instanced shader and the client's own code, with the normal,
        // colour and c3 coming from registers.
        //   c156 = {1/plants per row, plants per row, 4/width, 1/height}
        //   c157 = {0.5, 1.5, 2.5, 3.5} / width (the 4 texels of a plant)
        //   c158 = {colour scale (1/255, or 1 for UBYTE4), swap x/z (1 for D3DCOLOR), 0, 0}
        constexpr unsigned kBatchReg = 156, kTableReg = 160, kTableRanges = 32;
        constexpr uint32_t kCopyTexSize = 4096;
        constexpr uint32_t kMaxBatchInstances = 262144;

        const char* kBatchedDcl =
            "    dcl_texcoord5 v8\n"
            "    dcl_2d s0\n"
            "    dcl_2d s1\n";

        const char* kBatchedDefs =
            "    def c224, 0, 1, 0.5, 2\n"
            "    def c225, 16, 8, 4, 1\n"
            "    def c226, 15, 7, 3, 0.000244140625\n"
            "    def c227, 0.61803399, 0.41421356, 4096, 0\n"
            "    def c228, 0.00390625, 256, 0.5, 0.00000001\n";

        std::string SearchStep(const char* minusOne, const char* step)
        {
            return std::string("    add r0.y, r0.x, ") + minusOne + "\n"
                   "    add r0.y, r0.y, r0.y\n"
                   "    mova a0.x, r0.y\n"
                   "    sge r0.z, v8.x, c160[a0.x].x\n"
                   "    mad r0.x, r0.z, " + step + ", r0.x\n";
        }

        std::string BatchedPrologue()
        {
            std::string s =
                // range: lo = number of ranges ending at or before this instance
                "    mov r0.x, c224.x\n";
            s += SearchStep("c226.x", "c225.x");
            s += SearchStep("c226.y", "c225.y");
            s += SearchStep("c226.z", "c225.z");
            s += SearchStep("c224.y", "c224.w");
            s +=
                "    add r0.y, r0.x, r0.x\n"
                "    mova a0.x, r0.y\n"
                "    sge r0.z, v8.x, c160[a0.x].x\n"
                "    add r0.x, r0.x, r0.z\n"
                "    add r0.y, r0.x, r0.x\n"
                "    mova a0.x, r0.y\n"
                "    mov r1, c160[a0.x]\n"
                "    mov r18, c161[a0.x]\n"
                "    mov r15.x, r18.w\n"            // copy spread (the instanced shader's c153.x)
                "    mov r18.w, c224.y\n"           // the client's c3 for this layer's chunk
                // plant and copy within the range: plant = floor((i - start + 0.5) / m)
                "    add r2.x, v8.x, -r1.y\n"
                "    add r2.y, r2.x, c224.z\n"
                "    rcp r2.z, r1.w\n"
                "    mul r2.y, r2.y, r2.z\n"
                "    frc r2.z, r2.y\n"
                "    add r2.y, r2.y, -r2.z\n"
                "    mad r2.z, -r2.y, r1.w, r2.x\n"  // copy index k (0 = the plant itself)
                "    add r2.w, r1.z, r2.y\n"         // plant index in the page
                // its texels
                "    add r3.x, r2.w, c224.z\n"
                "    mul r3.x, r3.x, c156.x\n"
                "    frc r3.y, r3.x\n"
                "    add r3.x, r3.x, -r3.y\n"        // row
                "    mad r3.y, -r3.x, c156.y, r2.w\n" // plant in the row
                "    mul r4.x, r3.y, c156.z\n"
                "    add r4.y, r3.x, c224.z\n"
                "    mul r4.y, r4.y, c156.w\n"
                "    mov r4.zw, c224.x\n"
                "    mov r5, r4\n"
                "    add r5.x, r4.x, c157.x\n"
                "    texldl r10, r5, s0\n"
                "    add r5.x, r4.x, c157.y\n"
                "    texldl r11, r5, s0\n"
                "    add r5.x, r4.x, c157.z\n"
                "    texldl r12, r5, s0\n"
                "    add r5.x, r4.x, c157.w\n"
                "    texldl r6, r5, s0\n"
                // normal, z rebuilt (positive)
                "    mov r14.xy, r6.xy\n"
                "    mul r14.z, r6.x, r6.x\n"
                "    mad r14.z, r6.y, r6.y, r14.z\n"
                "    add r14.z, c224.y, -r14.z\n"
                "    max r14.z, r14.z, c228.w\n"
                "    rsq r14.w, r14.z\n"
                "    mul r14.z, r14.z, r14.w\n"
                "    mov r14.w, c224.y\n"
                "    mov r16, r14\n"
                // colour: bytes from the two 16-bit halves, then as the client's colour type delivers them
                "    add r7.xy, r6.zw, c228.z\n"
                "    mul r7.xy, r7, c228.x\n"
                "    frc r7.zw, r7.xxxy\n"
                "    add r7.xy, r7, -r7.zwzw\n"
                "    mad r8.xy, -r7, c228.y, r6.zw\n"
                "    mov r9.x, r8.x\n"
                "    mov r9.y, r7.x\n"
                "    mov r9.z, r8.y\n"
                "    mov r9.w, r7.y\n"
                "    lrp r17, c158.y, r9.zyxw, r9\n"
                "    mul r17, r17, c158.x\n"
                // copy entry: a fixed random pick per (plant, copy)
                "    mul r8.x, r2.w, c226.w\n"
                "    frc r8.x, r8.x\n"
                "    mul r8.x, r8.x, c227.z\n"
                "    mul r8.x, r8.x, c227.x\n"
                "    mad r8.x, r2.z, c227.y, r8.x\n"
                "    frc r8.x, r8.x\n"
                "    mov r8.y, c224.z\n"
                "    mov r8.zw, c224.x\n"
                "    texldl r13, r8, s1\n"
                // the plant itself (k = 0): no move AND no turn, as the client placed it
                "    min r8.y, r2.z, c224.y\n"
                "    mul r13, r13, r8.y\n";
            return s;
        }

        // The client's grass shader for batched draws: its normal/colour inputs and its per-chunk
        // c3 become registers filled by the prologue; v0 becomes r31 as in the instanced version.
        bool MakeBatched(const std::string& src, std::string& out, std::string& error)
        {
            for (const char* reg : { "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27",
                                     "r28", "r29", "r30", "r31", "c153", "c154", "c155", "c156", "c157", "c158",
                                     "c160", "c161", "c224", "c225", "c226", "c227", "c228", "v8", "s0", "s1" })
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
                if (t.find(" v") == std::string::npos) continue;
                if (t == "dcl_position v0") pos = true;
                else if (t == "dcl_normal v1") nrm = true;
                else if (t == "dcl_color v2") col = true;
                else if (t == "dcl_texcoord v3") tex = true;
                else { error = "unexpected input: " + t; return false; }
            }
            if (!(pos && nrm && col && tex)) { error = "not the grass vertex layout"; return false; }

            // The instanced copy/transform code, fed from registers: rows r10..r12, copy entry r13,
            // normal r14, spread r15.x. Its defs go up front with ours.
            std::string instDefs, instCode;
            {
                const std::string inst = kInstancedInputs;
                for (size_t at = 0; at < inst.size();)
                {
                    size_t end = inst.find('\n', at);
                    if (end == std::string::npos) end = inst.size();
                    std::string line = inst.substr(at, end - at);
                    at = end + 1;
                    const std::string t = Trim(line);
                    if (t.compare(0, 4, "dcl_") == 0) continue;
                    if (t.compare(0, 4, "def ") == 0) { instDefs += line + "\n"; continue; }
                    for (const auto& [from, to] : { std::pair<const char*, const char*>{ "v4", "r10" }, { "v5", "r11" },
                                                    { "v6", "r12" }, { "v7", "r13" }, { "v1", "r14" }, { "c153", "r15" } })
                        line = ReplaceToken(line, from, to);
                    instCode += line + "\n";
                }
            }

            out.clear();
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string t = Trim(lines[i]);
                if (i <= lastDcl)
                {
                    if (t != "dcl_normal v1" && t != "dcl_color v2") out += lines[i] + "\n";
                }
                else
                {
                    std::string line = lines[i];
                    for (const auto& [from, to] : { std::pair<const char*, const char*>{ "v0", "r31" }, { "v1", "r16" },
                                                    { "v2", "r17" }, { "c3", "r18" } })
                        line = ReplaceToken(line, from, to);
                    out += line + "\n";
                }
                if (i == lastDcl)
                {
                    out += kBatchedDcl;
                    out += kBatchedDefs;
                    out += instDefs;
                    out += BatchedPrologue();
                    out += instCode;
                }
            }
            return true;
        }

        IDirect3DVertexShader9* DerivedShader(IDirect3DDevice9* d3d, IDirect3DVertexShader9* clientVs, bool batched = false)
        {
            if (!clientVs) return nullptr;
            auto& shaders = batched ? g_batchShaders : g_shaders;
            auto it = shaders.find(clientVs);
            if (it != shaders.end()) return it->second.shader;

            // Held for as long as we map it, so its address can't come back as a different shader.
            clientVs->AddRef();
            Derived& d = shaders[clientVs];

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
                && (batched ? MakeBatched(text, source, error) : MakeInstanced(text, source, error))
                && shaderpatch::Assemble(source, batched ? "grass-batched" : "grass-instanced", bin, error))
            {
                if (FAILED(d3d->CreateVertexShader(reinterpret_cast<const DWORD*>(bin.data()), &d.shader)))
                {
                    d.shader = nullptr;
                    error = "CreateVertexShader failed";
                }
            }
            const char* kind = batched ? "batched" : "instanced";
            if (d.shader)
                g_api->Log(WXL_LOG_INFO, kTag, "instanced grass: %s shader for the client's %u-byte grass shader (%u bytes).",
                           kind, size, static_cast<unsigned>(bin.size()));
            else
            {
                g_api->Log(WXL_LOG_WARN, kTag, "instanced grass: no %s version of the bound %u-byte shader (%s); those layers draw another way.",
                           kind, size, error.c_str());
                // The generated source, to see what the assembler refused.
                if (!source.empty())
                    if (FILE* f = std::fopen(batched ? "Logs\\grass_batched_failed.txt" : "Logs\\grass_instanced_failed.txt", "w"))
                    {
                        std::fputs(source.c_str(), f);
                        std::fclose(f);
                    }
            }
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
            g_colorType = el[col].Type;
            // Batched: stream 0 as above, stream 1 a plain instance counter.
            const D3DVERTEXELEMENT9 batched[] = {
                { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                { 0, 12, el[tex].Type,       D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
                { 1, 0,  D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
                D3DDECL_END()
            };
            if (FAILED(d3d->CreateVertexDeclaration(batched, &g_batchDecl))) g_batchDecl = nullptr;
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

        // A doodad range's multiplier this frame: its content values capped by the player's limits;
        // full within half the radius, fading to 1 at the radius.
        unsigned RangeMultiplier(const SlotEntry& e, const Range& r)
        {
            const Settings& cap = g_settings;
            const float mult = r.density.multiplier < cap.maxMultiplier ? r.density.multiplier : cap.maxMultiplier;
            const float radius = r.density.radius < cap.maxRadius ? r.density.radius : cap.maxRadius;
            if (mult < 1.5f || radius <= 1.0f || !g_chunkOriginValid) return 1;
            const world::Snapshot& w = world::Current();
            if (!w.inWorld) return 1;
            const float dx = g_chunkOrigin[0] + e.center[0] - w.playerPos[0];
            const float dy = g_chunkOrigin[1] + e.center[1] - w.playerPos[1];
            const float d = std::sqrt(dx * dx + dy * dy);
            const float full = radius * 0.5f;
            const float t = d <= full ? 1.0f : (d >= radius ? 0.0f : 1.0f - (d - full) / (radius - full));
            const unsigned m = static_cast<unsigned>(1.0f + (mult - 1.0f) * t + 0.5f);
            return m < 1 ? 1 : (m > kMaxMultiplier ? kMaxMultiplier : m);
        }

        // Distance thinning: the share of a layer's plants drawn, by the camera's distance to their
        // centre (3D, so flying high thins too): all up to thinStart, down to thinMinShare at thinEnd.
        float    g_camera[3] = {};
        uint32_t g_cameraFrame = 0;

        float ThinShare(const SlotEntry& e)
        {
            const Settings& s = g_settings;
            if (!s.thinEnabled || !g_chunkOriginValid) return 1.0f;
            if (g_cameraFrame != g_frame) { wxl::game::camera::GetPosition(g_camera); g_cameraFrame = g_frame; }
            const float dx = g_chunkOrigin[0] + e.center[0] - g_camera[0];
            const float dy = g_chunkOrigin[1] + e.center[1] - g_camera[1];
            const float dz = g_chunkOrigin[2] + e.center[2] - g_camera[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= s.thinStart) return 1.0f;
            const float minShare = s.thinMinShare < 0.01f ? 0.01f : (s.thinMinShare > 1.0f ? 1.0f : s.thinMinShare);
            if (d >= s.thinEnd || s.thinEnd <= s.thinStart) return minShare;
            const float t = (d - s.thinStart) / (s.thinEnd - s.thinStart);
            return 1.0f + (minShare - 1.0f) * t;
        }

        // The device everything here was made on. A graphics restart (e.g. changing multisampling)
        // can replace the device without the reset events; objects from the old one must never
        // reach the new one, so a changed device drops all of them.
        IDirect3DDevice9* g_device = nullptr;

        void DropDeviceObjects()
        {
            for (auto* shaders : { &g_shaders, &g_batchShaders })
            {
                for (auto& [clientVs, d] : *shaders) { Release(d.shader); Release(clientVs); }
                shaders->clear();
            }
            Release(g_decl);
            g_decl = nullptr;
            Release(g_batchDecl);
            g_batchDecl = nullptr;
            g_batchProblem = nullptr;
            g_vtfChecked = false;
            g_declFailed = false;
            g_capsChecked = false;
            g_problem = nullptr;
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

        // --- batched draws ---------------------------------------------------------------------------
        bool BatchesWanted() { return g_settings.batched && !g_batchProblem && !g_hookProblem; }

        // Vertex textures in the plant format (every vs_3_0 GPU since ~2006 reads float4).
        bool CheckBatchSupport(IDirect3DDevice9* d3d)
        {
            if (g_vtfChecked) return !g_batchProblem;
            g_vtfChecked = true;
            IDirect3D9* d3d9 = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DDISPLAYMODE mode{};
            if (FAILED(d3d->GetDirect3D(&d3d9)) || !d3d9) { g_batchProblem = "couldn't query the device"; return false; }
            const bool ok = SUCCEEDED(d3d->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetDisplayMode(0, &mode))
                         && SUCCEEDED(d3d9->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                              D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F));
            d3d9->Release();
            if (!ok) g_batchProblem = "the GPU can't read float textures in vertex shaders";
            return ok;
        }

        bool EnsureBatchStatic(IDirect3DDevice9* d3d)
        {
            if (!g_copyTex)
            {
                IDirect3DSurface9* staging = nullptr;
                IDirect3DSurface9* dst = nullptr;
                D3DLOCKED_RECT lr{};
                bool ok = SUCCEEDED(d3d->CreateTexture(kCopyTexSize, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_copyTex, nullptr))
                       && SUCCEEDED(d3d->CreateOffscreenPlainSurface(kCopyTexSize, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &staging, nullptr))
                       && SUCCEEDED(staging->LockRect(&lr, nullptr, 0)) && lr.pBits;
                if (ok)
                {
                    float* t = static_cast<float*>(lr.pBits);
                    uint32_t state = 0x6A09E667u;
                    for (uint32_t i = 0; i < kCopyTexSize; ++i)
                    {
                        t[i * 4 + 0] = Unit(state);
                        t[i * 4 + 1] = Unit(state);
                        t[i * 4 + 2] = (Unit(state) - 0.5f) * 6.2831853f;
                        t[i * 4 + 3] = 1.0f;
                    }
                    staging->UnlockRect();
                    ok = SUCCEEDED(g_copyTex->GetSurfaceLevel(0, &dst)) && SUCCEEDED(d3d->UpdateSurface(staging, nullptr, dst, nullptr));
                }
                Release(dst);
                if (staging) g_staging.push_back({ staging, g_frame });
                if (!ok) { Release(g_copyTex); g_copyTex = nullptr; return false; }
            }
            if (!g_iota)
            {
                std::vector<float> n(kMaxBatchInstances);
                for (uint32_t i = 0; i < kMaxBatchInstances; ++i) n[i] = static_cast<float>(i);
                const UINT bytes = kMaxBatchInstances * sizeof(float);
                if (FAILED(d3d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_iota, nullptr))
                    || !Upload(g_iota, n.data(), bytes))
                {
                    Release(g_iota);
                    g_iota = nullptr;
                    return false;
                }
            }
            return true;
        }

        void ReleaseFrameTextures()
        {
            for (auto& [gxTex, ti] : g_texInfo) Release(ti.tex);
            g_texInfo.clear();
        }

        void ReleaseBatchObjects()
        {
            ReleaseFrameTextures();
            Release(g_copyTex);  g_copyTex = nullptr;
            Release(g_iota);     g_iota = nullptr;
            Release(g_grassBlock); g_grassBlock = nullptr;
            Release(g_savedBlock); g_savedBlock = nullptr;
            g_frameCaptured = false;
            g_frameVs = nullptr;
            g_items.clear();
            ReleaseStaging(true);
        }

        // The first time a texture comes up in a frame: one client flush with it bound (exactly what
        // the layer draw does), so its D3D texture is created/updated and its sampler states set;
        // both are read back. The frame's first flush also captures the whole grass state and
        // picks the batched shader for the client's grass shader of this frame.
        const TexInfo* LearnTexture(void* gxDev, IDirect3DDevice9* d3d, void* gxTex)
        {
            auto found = g_texInfo.find(gxTex);
            if (found != g_texInfo.end()) return &found->second;

            prof::Scope scope(prof::kOursGrassBind);
            if (!BindAnchor(gxDev)) return nullptr;
            reinterpret_cast<RsSetFn>(kGxRsSet)(gxDev, nullptr, kGxStateTexture0, gxTex);
            reinterpret_cast<FlushFn>(kGxFlushState)(gxDev, nullptr);

            if (!g_frameCaptured)
            {
                IDirect3DVertexShader9* clientVs = nullptr;
                d3d->GetVertexShader(&clientVs);
                g_frameVs = DerivedShader(d3d, clientVs, true);
                Release(clientVs);
                if (!g_frameVs || !EnsureDecl(d3d) || !g_batchDecl || !EnsureBatchStatic(d3d)) return nullptr;
                if (g_grassBlock) { if (FAILED(g_grassBlock->Capture())) return nullptr; }
                else if (FAILED(d3d->CreateStateBlock(D3DSBT_ALL, &g_grassBlock)) || !g_grassBlock) { g_grassBlock = nullptr; return nullptr; }
                g_frameCaptured = true;
            }

            TexInfo ti;
            if (FAILED(d3d->GetTexture(0, &ti.tex)) || !ti.tex) return nullptr;
            for (size_t k = 0; k < kTexSamplerCount; ++k) d3d->GetSamplerState(0, kTexSamplerStates[k], &ti.sampler[k]);
            // Check of the reading (engine bind 0x6A4900: SetTexture(stage, [gxTex + 0x38])).
            uint32_t apiTex = 0;
            if (!ReadU32(reinterpret_cast<uintptr_t>(gxTex) + 0x38, apiTex) || apiTex != reinterpret_cast<uintptr_t>(ti.tex))
                ++g_frameStats.textureMismatch;
            ++g_frameStats.batchedTextures;
            return &(g_texInfo[gxTex] = ti);
        }

        // At the end of the grass pass (the client already popped its grass state, but nothing has
        // been flushed since): bring back this frame's grass state, draw every recorded range, put
        // back what was bound.
        void FlushBatches()
        {
            if (g_items.empty()) return;
            prof::Scope scope(prof::kOursGrassDraw);
            auto* d3d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
            if (!d3d || d3d != g_device || !g_frameCaptured || !g_grassBlock || !g_frameVs || !g_batchDecl || !g_iota || !g_copyTex)
            {
                g_items.clear();
                return;
            }

            std::sort(g_items.begin(), g_items.end(), [](const BatchItem& a, const BatchItem& b)
            {
                if (a.gxTex != b.gxTex) return a.gxTex < b.gxTex;
                if (a.geo != b.geo) return a.geo < b.geo;
                return a.page < b.page;
            });

            if (g_savedBlock) g_savedBlock->Capture();
            else if (FAILED(d3d->CreateStateBlock(D3DSBT_ALL, &g_savedBlock))) g_savedBlock = nullptr;
            if (!g_savedBlock) { g_items.clear(); return; }
            g_grassBlock->Apply();

            d3d->SetVertexShader(g_frameVs);
            d3d->SetVertexDeclaration(g_batchDecl);
            const float colourScale = g_colorType == D3DDECLTYPE_UBYTE4 ? 1.0f : 1.0f / 255.0f;
            const float consts[3][4] = {
                { 1.0f / kTexPlantsPerRow, static_cast<float>(kTexPlantsPerRow), 4.0f / kTexWidth, 1.0f / kTexHeight },
                { 0.5f / kTexWidth, 1.5f / kTexWidth, 2.5f / kTexWidth, 3.5f / kTexWidth },
                { colourScale, g_colorType == D3DDECLTYPE_D3DCOLOR ? 1.0f : 0.0f, 0.0f, 0.0f },
            };
            d3d->SetVertexShaderConstantF(kBatchReg, &consts[0][0], 3);
            const float highlight[4] = { -1.0f, 0.0f, 0.0f, 0.0f };
            d3d->SetVertexShaderConstantF(152, highlight, 1);
            for (DWORD s : { D3DVERTEXTEXTURESAMPLER0, D3DVERTEXTEXTURESAMPLER1 })
            {
                d3d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                d3d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                d3d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                d3d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                d3d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            }
            d3d->SetTexture(D3DVERTEXTEXTURESAMPLER1, g_copyTex);
            d3d->SetStreamSource(1, g_iota, 0, sizeof(float));
            d3d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);

            float table[kTableRanges * 2][4];
            unsigned used = 0, calls = 0;
            uint32_t instances = 0;
            uint64_t triangles = 0;
            const Geometry* geo = nullptr;
            auto emit = [&]()
            {
                if (!used) return;
                for (unsigned u = used; u < kTableRanges; ++u)
                {
                    table[2 * u][0] = table[2 * u][1] = 1.0e9f; table[2 * u][2] = 0.0f; table[2 * u][3] = 1.0f;
                    table[2 * u + 1][0] = table[2 * u + 1][1] = table[2 * u + 1][2] = table[2 * u + 1][3] = 0.0f;
                }
                d3d->SetVertexShaderConstantF(kTableReg, &table[0][0], kTableRanges * 2);
                d3d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | instances);
                d3d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, geo->vertices, 0, geo->indices / 3);
                ++calls;
                triangles += static_cast<uint64_t>(geo->indices / 3) * instances;
                used = 0;
                instances = 0;
            };

            for (size_t at = 0; at < g_items.size();)
            {
                // One group: same texture, model and page.
                const BatchItem& head = g_items[at];
                size_t end = at + 1;
                while (end < g_items.size() && g_items[end].gxTex == head.gxTex && g_items[end].geo == head.geo
                       && g_items[end].page == head.page)
                    ++end;
                auto ti = g_texInfo.find(head.gxTex);
                if (ti == g_texInfo.end() || head.page < 0 || head.page >= static_cast<int>(g_pages.size())
                    || !g_pages[head.page].tex || !head.geo->vb)
                {
                    at = end;
                    continue;
                }

                d3d->SetTexture(0, ti->second.tex);
                for (size_t k = 0; k < kTexSamplerCount; ++k) d3d->SetSamplerState(0, kTexSamplerStates[k], ti->second.sampler[k]);
                // One doodad per draw: every table entry is its own, whatever index a colour decodes to.
                float entry[4], entries[grassdoodads::kEntries][4];
                grassdoodads::DoodadEntry(head.doodad, entry);
                for (unsigned k = 0; k < grassdoodads::kEntries; ++k) std::memcpy(entries[k], entry, sizeof(entry));
                d3d->SetVertexShaderConstantF(grassdoodads::kFirstReg, &entries[0][0], grassdoodads::kEntries);
                d3d->SetTexture(D3DVERTEXTEXTURESAMPLER0, g_pages[head.page].tex);
                geo = head.geo;
                d3d->SetStreamSource(0, geo->vb, 0, sizeof(GpuVertex));
                d3d->SetIndices(geo->ib);

                for (size_t i = at; i < end; ++i)
                {
                    const BatchItem& it = g_items[i];
                    const uint32_t m = it.multiplier ? it.multiplier : 1;
                    for (uint32_t first = 0; first < it.plants;)
                    {
                        const uint32_t room = (kMaxBatchInstances - instances) / m;
                        if (!room || used == kTableRanges) { emit(); continue; }
                        const uint32_t n = (it.plants - first < room) ? it.plants - first : room;
                        float* a = table[2 * used];
                        float* b = table[2 * used + 1];
                        a[0] = static_cast<float>(instances + n * m);
                        a[1] = static_cast<float>(instances);
                        a[2] = static_cast<float>(it.first + first);
                        a[3] = static_cast<float>(m);
                        b[0] = it.origin[0]; b[1] = it.origin[1]; b[2] = it.origin[2]; b[3] = it.spread;
                        ++used;
                        instances += n * m;
                        first += n;
                    }
                }
                emit();
                at = end;
            }

            d3d->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
            d3d->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
            d3d->SetStreamSourceFreq(0, 1);
            d3d->SetStreamSourceFreq(1, 1);
            g_savedBlock->Apply();

            g_frameStats.batchedDraws += calls;
            g_frameStats.drawCalls += calls;
            prof::CountDraws(calls, triangles);
            g_items.clear();
        }

        using PassEndFn = void(__cdecl*)();
        PassEndFn g_origPassEnd = nullptr;

        // 0x797280: called only by the grass pass (0x7984A0), right after its chunk loop and state
        // pop -- the end of the grass pass.
        constexpr uintptr_t kGrassPassTail = 0x00797280;
        void __cdecl hkGrassPassEnd()
        {
            FlushBatches();
            g_origPassEnd();
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
        // The end of the grass pass, where batched layers are drawn.
        if (!api->HookAttach("LivingAzeroth.GrassPassEnd", kGrassPassTail, reinterpret_cast<void*>(&hkGrassPassEnd),
                             reinterpret_cast<void**>(&g_origPassEnd), WXL_HOOK_DEFAULT_PRIORITY))
        {
            g_hookProblem = "the end-of-grass-pass hook couldn't be installed";
            api->Log(WXL_LOG_WARN, kTag, "instanced grass: %s; batched draws off.", g_hookProblem);
        }
    }

    Settings& Tunables() { return g_settings; }

    const grassdoodads::SlotDoodads* Prepare(void* slot)
    {
        if (!g_settings.enabled || g_problem || g_cap.pending) return nullptr;
        ProbeIfPlayerChunk(slot);
        auto* d3d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!d3d) return nullptr;
        if (d3d != g_device)
        {
            if (g_device)
            {
                g_api->Log(WXL_LOG_INFO, kTag, "instanced grass: the graphics device was replaced; rebuilding everything on the new one.");
                OnDeviceLost();
                DropDeviceObjects();
            }
            g_device = d3d;
        }
        if (!CheckCaps(d3d)) return nullptr;
        if (g_settings.batched) CheckBatchSupport(d3d);
        // Pool pages are vertex buffers or textures: switching drops everything (rebuilt on demand).
        if (BatchesWanted() != g_poolBatched)
        {
            g_slots.clear();
            ReleasePool();
            g_items.clear();
            g_poolBatched = BatchesWanted();
            g_memoryTight = false;
        }

        const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
        uint32_t count = 0, instances = 0;
        if (!ReadU32(s + kSlotInstanceCount, count) || !ReadU32(s + kSlotInstances, instances) || !instances
            || count == 0 || count > 65536)
            return nullptr;
        SlotSample sample;
        if (!ReadSample(instances, count, sample)) return nullptr;

        auto found = g_slots.find(slot);
        const bool current = found != g_slots.end() && found->second.page >= 0 && found->second.instances == instances
                          && found->second.count == count && std::memcmp(&found->second.sample, &sample, sizeof(sample)) == 0
                          && found->second.densityGeneration == grassdensity::Generation();
        if (!current)
        {
            // Layers coming into view all want building at once (turning the camera at long
            // range): over the frame's budget they draw the client's way until a later frame.
            if (g_frameBuildMs >= g_settings.buildBudgetMs) { ++g_frameStats.fallbackBudget; return nullptr; }

            SlotEntry& e = g_slots[slot];
            ReleaseSlot(e);
            e.slot = s;
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
            e.sample = sample;
            ++g_frameStats.builds;
            grassperf::OnInstanceBuild(ms, deviceMs);
            found = g_slots.find(slot);
        }
        SlotEntry& e = found->second;
        e.lastFrame = g_frame;
        return &e.sd;
    }

    bool Record(void* slot)
    {
        if (!g_poolBatched || !BatchesWanted() || grassdoodads::Highlighted()) return false;
        auto it = g_slots.find(slot);
        if (it == g_slots.end() || it->second.page < 0) return false;
        SlotEntry& e = it->second;

        void* gxDev = GxDevice();
        auto* d3d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!gxDev || !d3d) return false;
        const auto* base = static_cast<const uint8_t*>(gxDev);
        // As Draw: nothing is drawn in these device states, or without a texture.
        if (!*reinterpret_cast<const uint32_t*>(base + kDeviceReady) || *reinterpret_cast<const uint32_t*>(base + kDeviceBusy))
            return true;
        void* texture = *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + kSlotTexture);
        if (!texture) return true;
        void* gxTex = reinterpret_cast<GetGxTexFn>(kTextureGetGxTex)(texture, 0, 0);
        if (!gxTex) return true;

        if (!LearnTexture(gxDev, d3d, gxTex))
        {
            // Can't be made on this device at all: back to per-layer draws from the next layer on.
            if (!g_batchProblem && (!g_frameVs || !g_batchDecl))
                g_batchProblem = "the batched shader or vertex layout couldn't be made (see the log)";
            else if (!g_batchProblem && (!g_copyTex || !g_iota))
                g_batchProblem = "couldn't create the batched draw's buffers";
            ++g_frameStats.fallbackFailed;
            return false;
        }

        // This chunk's c3 (its origin in view space): the block still holds its upload.
        const float* block = reinterpret_cast<const float*>(wxl::offsets::game::groundeffect::kVsConstantBlock);
        const float share = ThinShare(e);
        unsigned drawn = 0, copies = 0;
        for (const Range& r : e.ranges)
        {
            if (!r.count || !r.geo->vb) continue;
            const uint32_t keep = share >= 1.0f ? r.count : static_cast<uint32_t>(std::ceil(static_cast<float>(r.count) * share));
            const unsigned m = RangeMultiplier(e, r);
            const float spread = r.density.spread < g_settings.maxSpread ? r.density.spread : g_settings.maxSpread;
            g_items.push_back({ gxTex, r.geo, e.page, r.id, r.start, keep, m, spread, { block[12], block[13], block[14] } });
            drawn += keep;
            copies += keep * (m - 1);
        }
        ++g_frameStats.slotsInstanced;
        ++g_frameStats.batchedLayers;
        g_frameStats.plantsPlaced += e.allocated;
        g_frameStats.plantsDrawn += drawn;
        if (copies) { ++g_frameStats.densifiedLayers; g_frameStats.densityCopies += copies; }
        return true;
    }

    bool Draw(void* slot)
    {
        if (g_poolBatched) return false; // texture pages: only the batched draw reads them
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

        prof::Enter(prof::kOursGrassBind);
        if (!BindAnchor(gxDev)) { prof::Leave(prof::kOursGrassBind); ++g_frameStats.fallbackFailed; return false; }
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
            prof::Leave(prof::kOursGrassBind);
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
        prof::Leave(prof::kOursGrassBind);
        prof::Scope drawScope(prof::kOursGrassDraw);

        d3d->SetVertexShader(ours);
        d3d->SetVertexDeclaration(g_decl);
        // A per-layer start in the table, so layers don't all repeat the same copy pattern.
        const uint32_t group0 = Mix(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot))) % (kCopyTableSpan - kCopyPlants);

        unsigned calls = 0, copies = 0, drawn = 0;
        uint64_t triangles = 0;
        bool densified = false;
        const float share = ThinShare(e);
        float lastSpread = -1.0f;
        d3d->SetStreamSourceFreq(2, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        for (const Range& r : e.ranges)
        {
            if (!r.count || !r.geo->vb) continue;
            const unsigned m = RangeMultiplier(e, r);
            const float spread = r.density.spread < g_settings.maxSpread ? r.density.spread : g_settings.maxSpread;
            if (spread != lastSpread)
            {
                const float copyReg[4] = { spread, 0.0f, 0.0f, 0.0f };
                d3d->SetVertexShaderConstantF(kCopyReg, copyReg, 1);
                lastSpread = spread;
            }
            densified |= m > 1;
            // The range is shuffled: its first `keep` plants are an even sample of it.
            const uint32_t keep = share >= 1.0f ? r.count
                                : static_cast<uint32_t>(std::ceil(static_cast<float>(r.count) * share));
            drawn += keep;
            d3d->SetStreamSource(0, r.geo->vb, 0, sizeof(GpuVertex));
            d3d->SetIndices(r.geo->ib);
            for (uint32_t first = 0; first < keep; first += kCopyPlants)
            {
                const uint32_t n = (keep - first < kCopyPlants) ? keep - first : kCopyPlants;
                d3d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | (n * m));
                d3d->SetStreamSource(1, g_pages[e.page].vb, (r.start + first) * sizeof(GpuInstance), sizeof(GpuInstance));
                d3d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | m);
                d3d->SetStreamSource(2, g_copyTable, (g_copyBase[m] + group0 * m) * sizeof(CopyEntry), sizeof(CopyEntry));
                d3d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, r.geo->vertices, 0, r.geo->indices / 3);
                ++calls;
                copies += n * (m - 1);
                triangles += static_cast<uint64_t>(r.geo->indices / 3) * n * m;
            }
        }
        d3d->SetVertexShaderConstantF(kCopyReg, savedCopyReg, 1);
        if (densified) { ++g_frameStats.densifiedLayers; g_frameStats.densityCopies += copies; }

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
        g_frameStats.plantsPlaced += e.allocated;
        g_frameStats.plantsDrawn += drawn;
        prof::CountDraws(calls, triangles);
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
        // Batched: anything the pass didn't draw is dropped; textures are learned again next frame.
        g_items.clear();
        ReleaseFrameTextures();
        g_frameCaptured = false;
        g_frameVs = nullptr;
        ReleaseStaging(false);

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
            if (p.Live() && p.free.size() == 1 && p.free[0].offset == 0 && p.free[0].size == PagePlants())
            {
                Release(p.vb);
                Release(p.tex);
                p.vb = nullptr;
                p.tex = nullptr;
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
        ReleaseBatchObjects();
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
        s.lastArea = g_lastContext.valid ? g_lastContext.area : 0;
        s.lastEffects = g_lastContext.effectCount;
        s.lastDoodads = g_lastDoodads;
        s.lastDoodadsWithEffect = g_lastEffectsFound;
        s.lastContextValid = g_lastContext.valid;
        s.poolLimitPages = PageLimit();
        s.batchedMode = g_poolBatched;
        s.memoryTight = g_memoryTight;
        for (const auto& [id, g] : g_geometry)
            if (g.vb) { ++s.geometries; geometryBytes += g.vertices * sizeof(GpuVertex) + g.indices * sizeof(uint16_t); }
        s.instanceMB = instanceBytes / (1024.0 * 1024.0);
        s.geometryKB = geometryBytes / 1024.0;
        for (const auto& [vs, d] : g_shaders) if (d.shader) ++s.shaders;
        return s;
    }

    const char* Problem() { return g_problem; }
    const char* BatchProblem() { return g_hookProblem ? g_hookProblem : g_batchProblem; }

    const TerrainProbe& Probe() { return g_probe; }

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
