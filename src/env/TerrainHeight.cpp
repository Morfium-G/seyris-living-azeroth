#include "TerrainHeight.hpp"

#include "game/Adt.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::terrain
{
    namespace
    {
        // --- client landmarks not (yet) in the SDK. Verified in XWorkbench 2026-10-02. -----------
        // Chunk object: the MCNK header at +0x110 (holes at +0x3C), the raw MCVT heights at +0x11C
        // (assigned by the sub-chunk walk 0x7C3A10; 145 floats inside the tile's file buffer, which
        // lives as long as the tile), and the chunk's corner at +0x7C/+0x80/+0x84 (chunk build
        // 0x7C64B0: x, y = 17066.67 - 33.33 * chunk index, z = the MCNK position's z). Local
        // coordinates run from that corner into the negatives.
        constexpr size_t kChunkCorner = 0x7C, kChunkHeader = 0x110, kChunkHeights = 0x11C;
        constexpr size_t kHeaderHoles = 0x3C;

        // MCVT layout (grass placement 0x7D3390): rows of 17 floats, 9 outer vertices then the 8
        // cell centres. The row runs along local x, the column along local y, 4.1667 yd apart.
        // Each cell is drawn as 4 triangles meeting in its centre.
        constexpr float kCellSize = 33.3333333f / 8.0f;
        constexpr int   kRowStride = 17, kCentreOffset = 9;

        // Hole bit per 2x2 cells, indexed (row / 2) * 4 + (col / 2) (as the TerrainType query
        // 0x7A0530 tests them).
        constexpr uintptr_t kHoleMaskTable = 0x00A3FAF0;

        // The client's terrain TerrainType query: (pos, &out) -> true when found. __cdecl.
        constexpr uintptr_t kTerrainTypeQuery = 0x007A0530;
        using TerrainTypeQueryFn = bool(__cdecl*)(const float* pos, int* out);

        // Height of the plane through a, b, c (each {u, v, h}) at (u, v).
        float PlaneHeight(const float a[3], const float b[3], const float c[3], float u, float v)
        {
            const float d  = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
            const float wa = ((b[1] - c[1]) * (u - c[0]) + (c[0] - b[0]) * (v - c[1])) / d;
            const float wb = ((c[1] - a[1]) * (u - c[0]) + (a[0] - c[0]) * (v - c[1])) / d;
            return wa * a[2] + wb * b[2] + (1.0f - wa - wb) * c[2];
        }
    }

    bool HeightAt(float x, float y, float& outZ)
    {
        float pos[3] = { x, y, 0.0f };
        const auto* chunk = static_cast<const uint8_t*>(wxl::game::adt::GetChunk(pos));
        if (!chunk) return false;
        const auto* header  = *reinterpret_cast<const uint8_t* const*>(chunk + kChunkHeader);
        const auto* heights = *reinterpret_cast<const float* const*>(chunk + kChunkHeights);
        if (!header || !heights) return false;
        const float* corner = reinterpret_cast<const float*>(chunk + kChunkCorner);

        // Position in cells from the corner, 0..8 along each axis.
        const float r = (corner[0] - x) / kCellSize, c = (corner[1] - y) / kCellSize;
        const int row = r < 0.0f ? 0 : (r >= 8.0f ? 7 : static_cast<int>(r));
        const int col = c < 0.0f ? 0 : (c >= 8.0f ? 7 : static_cast<int>(c));

        const uint16_t holes = *reinterpret_cast<const uint16_t*>(header + kHeaderHoles);
        const uint32_t holeBit = reinterpret_cast<const uint32_t*>(kHoleMaskTable)[(row >> 1) * 4 + (col >> 1)];
        if (holes & holeBit) return false;

        const float u = r - row, v = c - col; // inside the cell, 0..1
        const float* h = heights + row * kRowStride + col;
        const float p00[3] = { 0.0f, 0.0f, h[0] };
        const float p01[3] = { 0.0f, 1.0f, h[1] };
        const float p10[3] = { 1.0f, 0.0f, h[kRowStride] };
        const float p11[3] = { 1.0f, 1.0f, h[kRowStride + 1] };
        const float mid[3] = { 0.5f, 0.5f, h[kCentreOffset] };

        // The triangle whose outer edge is closest: u = 0, u = 1, v = 0 or v = 1.
        const float du0 = u, du1 = 1.0f - u, dv0 = v, dv1 = 1.0f - v;
        float z;
        if (du0 <= du1 && du0 <= dv0 && du0 <= dv1)  z = PlaneHeight(p00, p01, mid, u, v);
        else if (du1 <= dv0 && du1 <= dv1)           z = PlaneHeight(p10, p11, mid, u, v);
        else if (dv0 <= dv1)                         z = PlaneHeight(p00, p10, mid, u, v);
        else                                         z = PlaneHeight(p01, p11, mid, u, v);

        outZ = corner[2] + z;
        return std::isfinite(outZ);
    }

    namespace
    {
        // The client's TerrainType query (0x7A0530) walks the same chain as SurfaceAt: the chunk, the
        // hole test, the cell's dominant layer from the low-res layer map (chunk +0x114 -> 8 u16
        // rows, one per cell along local x; the 2-bit field per cell along local y via the
        // mask/shift tables), that layer's MCLY ground effect (+0x0C), GroundEffectTexture +0x28 =
        // TerrainType. The chunk's area ID is at +0xB0 (copied from the MCNK header by the chunk
        // build 0x7C64B0).
        constexpr size_t    kChunkLowRes = 0x114, kChunkLayers = 0x12C, kChunkAlpha = 0x130, kChunkArea = 0xB0;
        constexpr size_t    kHeaderFlags = 0x00, kHeaderLayerCount = 0x0C;
        constexpr size_t    kLayerStride = 0x10, kLayerFlags = 0x04, kLayerAlphaOffset = 0x08, kLayerEffect = 0x0C;
        constexpr uintptr_t kLowResMasks = 0x00A3FB88, kLowResShifts = 0x00A3FB98;
        constexpr uintptr_t kEffectMaxId = 0x00AD3AF4, kEffectMinId = 0x00AD3AF8, kEffectTable = 0x00AD3B08;
        constexpr size_t    kEffectTerrainType = 0x28;

        // The layer's texture: MCLY +0x00 indexes the tile's texture list. The chunk's tile is
        // [[chunk +0x20] +0x08] (the chunk build 0x7C64B0 registers the chunk in that tile's grid);
        // the list is a growable array at tile +0x58 (count +0x5C, entries +0x60, 8 bytes each: the
        // name, pointing into the MTEX block, then the texture handle -- 0x7D6D20 builds it).
        constexpr size_t kChunkTileLink = 0x20, kLinkTile = 0x08, kTileTexCount = 0x5C, kTileTexEntries = 0x60;

        // Alpha maps (MCAL), as the client reads them (0x7B7860 / 0x7B74A0): a layer has one when
        // its MCLY flags have 0x100, at MCAL + MCLY +0x08; 0x200 = RLE-compressed (8-bit, 64x64).
        // Otherwise 8-bit uncompressed when the map's MPHD flags have 0x4 ("big alpha"), else
        // 4-bit (2048 bytes, low nibble first) whose last row and column repeat the one before,
        // unless the MCNK flags have 0x8000. The client keeps no decoded copy (it re-reads rows
        // when it builds the blend texture), so we decode once per chunk and cache.
        constexpr uint32_t  kLayerHasAlpha = 0x100, kLayerCompressed = 0x200;
        constexpr uint32_t  kMphdBigAlpha = 0x4, kMcnkDoNotFixAlpha = 0x8000;
        constexpr uintptr_t kMphdFlags = 0x00CF08D0;

        void LayerSurface(const uint8_t* chunk, const uint8_t* layers, unsigned layer, Surface& out)
        {
            const uint32_t textureIndex = *reinterpret_cast<const uint32_t*>(layers + layer * kLayerStride);
            const uintptr_t link = *reinterpret_cast<const uintptr_t*>(chunk + kChunkTileLink);
            if (link && !(link & 1))
                if (const auto* tile = *reinterpret_cast<const uint8_t* const*>(link + kLinkTile))
                {
                    const uint32_t texCount = *reinterpret_cast<const uint32_t*>(tile + kTileTexCount);
                    const auto* entries = *reinterpret_cast<const char* const* const*>(tile + kTileTexEntries);
                    if (entries && textureIndex < texCount) out.texture = entries[textureIndex * 2];
                }

            out.groundEffect = *reinterpret_cast<const uint32_t*>(layers + layer * kLayerStride + kLayerEffect);
            const int32_t minId = *reinterpret_cast<const int32_t*>(kEffectMinId);
            const int32_t maxId = *reinterpret_cast<const int32_t*>(kEffectMaxId);
            const auto* table = *reinterpret_cast<const uint8_t* const* const*>(kEffectTable);
            const int32_t id = static_cast<int32_t>(out.groundEffect);
            if (table && id >= minId && id <= maxId)
                if (const uint8_t* rec = table[id - minId])
                    out.terrainType = *reinterpret_cast<const int32_t*>(rec + kEffectTerrainType);
        }

        // Where in the chunk (cells from its corner, 0..8 along local x and y), or false on a hole.
        struct ChunkSpot
        {
            const uint8_t* chunk = nullptr;
            const uint8_t* header = nullptr;
            float r = 0, c = 0;
            int   row = 0, col = 0;
        };

        bool Spot(float x, float y, ChunkSpot& out)
        {
            float pos[3] = { x, y, 0.0f };
            out.chunk = static_cast<const uint8_t*>(wxl::game::adt::GetChunk(pos));
            if (!out.chunk) return false;
            out.header = *reinterpret_cast<const uint8_t* const*>(out.chunk + kChunkHeader);
            if (!out.header) return false;
            const float* corner = reinterpret_cast<const float*>(out.chunk + kChunkCorner);
            out.r = (corner[0] - x) / kCellSize;
            out.c = (corner[1] - y) / kCellSize;
            out.row = out.r < 0.0f ? 0 : (out.r >= 8.0f ? 7 : static_cast<int>(out.r));
            out.col = out.c < 0.0f ? 0 : (out.c >= 8.0f ? 7 : static_cast<int>(out.c));
            const uint16_t holes = *reinterpret_cast<const uint16_t*>(out.header + kHeaderHoles);
            return !(holes & reinterpret_cast<const uint32_t*>(kHoleMaskTable)[(out.row >> 1) * 4 + (out.col >> 1)]);
        }

        // The client's dominant layer for the spot's cell (the low-res layer map), -1 if none.
        int DominantLayer(const ChunkSpot& at)
        {
            const auto* lowRes = *reinterpret_cast<const uint16_t* const*>(at.chunk + kChunkLowRes);
            if (!lowRes) return -1;
            const uint16_t mask = reinterpret_cast<const uint16_t*>(kLowResMasks)[at.col];
            const uint8_t shift = static_cast<uint8_t>(reinterpret_cast<const uint32_t*>(kLowResShifts)[at.col]);
            return static_cast<int>((lowRes[at.row] & mask) >> shift);
        }

        // --- decoded alpha per chunk -------------------------------------------------------------
        // A chunk object is reused for another place after a teleport or a tile reload, and the new
        // tile's buffers can land at the same addresses, so the cached decode is checked against the
        // chunk's place (its world corner), layer count and data pointers together. The texture names
        // are copied: the tile's MTEX block they point into is freed with the tile.
        struct DecodedChunk
        {
            const uint8_t* layers = nullptr;
            const uint8_t* alpha = nullptr;
            float    cornerX = 0.0f, cornerY = 0.0f;
            unsigned serial = 0;
            int      count = 0;
            std::vector<uint8_t> map;        // layers 1..count-1, 64x64 each (sized to what the chunk has)
            Surface  surface[4];
            std::string texture[4];          // own copies; surface[l].texture points here
        };
        std::unordered_map<const uint8_t*, DecodedChunk> g_decoded;
        unsigned g_serial = 0;

        void DecodeLayer(const uint8_t* src, uint32_t flags, bool fix, uint8_t* out)
        {
            if (flags & kLayerCompressed)
            {
                // RLE: a byte's top bit = fill (repeat the next byte) or copy (the next bytes), its
                // low 7 bits the count.
                int n = 0;
                while (n < 64 * 64)
                {
                    const uint8_t head = *src++;
                    int count = head & 0x7F;
                    if (!count) break; // malformed: don't loop forever
                    if (head & 0x80) { const uint8_t v = *src++; while (count-- && n < 64 * 64) out[n++] = v; }
                    else             { while (count-- && n < 64 * 64) out[n++] = *src++; }
                }
                return;
            }
            if (*reinterpret_cast<const uint32_t*>(kMphdFlags) & kMphdBigAlpha)
            {
                std::memcpy(out, src, 64 * 64);
                return;
            }
            for (int i = 0; i < 64 * 32; ++i)
            {
                out[i * 2]     = static_cast<uint8_t>((src[i] & 0x0F) * 17);
                out[i * 2 + 1] = static_cast<uint8_t>((src[i] >> 4) * 17);
            }
            if (fix)
            {
                for (int r = 0; r < 64; ++r) out[r * 64 + 63] = out[r * 64 + 62];
                std::memcpy(out + 63 * 64, out + 62 * 64, 64);
            }
        }

        const DecodedChunk* Decoded(const ChunkSpot& at)
        {
            const auto* layers = *reinterpret_cast<const uint8_t* const*>(at.chunk + kChunkLayers);
            const auto* alpha = *reinterpret_cast<const uint8_t* const*>(at.chunk + kChunkAlpha);
            auto found = g_decoded.find(at.chunk);
            const float* corner = reinterpret_cast<const float*>(at.chunk + kChunkCorner);
            const unsigned count = *reinterpret_cast<const uint32_t*>(at.header + kHeaderLayerCount);
            const int layerCount = layers ? static_cast<int>(count > 4 ? 4 : count) : 0;
            if (found != g_decoded.end() && found->second.layers == layers && found->second.alpha == alpha &&
                found->second.cornerX == corner[0] && found->second.cornerY == corner[1] && found->second.count == layerCount)
                return &found->second;

            // ~1500 chunks lie within the outermost level's reach; a few KB each.
            if (g_decoded.size() > 2500) g_decoded.clear();
            DecodedChunk& d = g_decoded[at.chunk];
            d = DecodedChunk{};
            d.layers = layers; d.alpha = alpha;
            d.cornerX = corner[0]; d.cornerY = corner[1];
            d.serial = ++g_serial;
            d.count = layerCount;
            const bool fix = !(*reinterpret_cast<const uint32_t*>(at.header + kHeaderFlags) & kMcnkDoNotFixAlpha);
            if (d.count > 1) d.map.assign(static_cast<size_t>(d.count - 1) * 64 * 64, 0);
            for (int l = 0; l < d.count; ++l)
            {
                LayerSurface(at.chunk, layers, static_cast<unsigned>(l), d.surface[l]);
                d.texture[l] = d.surface[l].texture ? d.surface[l].texture : "";
                d.surface[l].texture = d.texture[l].c_str();
                d.surface[l].area = *reinterpret_cast<const uint32_t*>(at.chunk + kChunkArea);
                if (l == 0) continue;
                const uint32_t flags = *reinterpret_cast<const uint32_t*>(layers + l * kLayerStride + kLayerFlags);
                if (!(flags & kLayerHasAlpha) || !alpha) continue; // no alpha map: not painted here
                DecodeLayer(alpha + *reinterpret_cast<const uint32_t*>(layers + l * kLayerStride + kLayerAlphaOffset), flags, fix, d.map.data() + (l - 1) * 64 * 64);
            }
            return &d;
        }

        // Bilinear texel lookup, coordinates in texels (0..63), clamped to the chunk. Unswapped,
        // the map's rows run along local x like the MCVT rows and the low-res layer map (confirmed
        // in-client 2026-10-02: the strongest layer matches the client's dominant layer).
        float Texel(const uint8_t* map, float u, float v, bool swapAxes)
        {
            u = u < 0.0f ? 0.0f : (u > 63.0f ? 63.0f : u);
            v = v < 0.0f ? 0.0f : (v > 63.0f ? 63.0f : v);
            const int u0 = static_cast<int>(u), v0 = static_cast<int>(v);
            const int u1 = u0 < 63 ? u0 + 1 : 63, v1 = v0 < 63 ? v0 + 1 : 63;
            const float fu = u - u0, fv = v - v0;
            auto at = [&](int a, int b) { return static_cast<float>(swapAxes ? map[b * 64 + a] : map[a * 64 + b]); };
            const float top = at(u0, v0) + (at(u0, v1) - at(u0, v0)) * fv;
            const float bottom = at(u1, v0) + (at(u1, v1) - at(u1, v0)) * fv;
            return (top + (bottom - top) * fu) / 255.0f;
        }
    }

    bool SurfaceAt(float x, float y, Surface& out)
    {
        ChunkSpot at;
        if (!Spot(x, y, at)) return false;
        out = Surface{};
        out.area = *reinterpret_cast<const uint32_t*>(at.chunk + kChunkArea);
        const auto* layers = *reinterpret_cast<const uint8_t* const*>(at.chunk + kChunkLayers);
        const int layer = DominantLayer(at);
        const unsigned count = *reinterpret_cast<const uint32_t*>(at.header + kHeaderLayerCount);
        if (!layers || layer < 0 || static_cast<unsigned>(layer) >= count) return true; // bare ground, no ground effect
        LayerSurface(at.chunk, layers, static_cast<unsigned>(layer), out);
        return true;
    }

    bool LayerWeightsAt(float x, float y, LayerWeights& out, bool swapAxes)
    {
        ChunkSpot at;
        if (!Spot(x, y, at)) return false;
        const DecodedChunk* d = Decoded(at);
        out = LayerWeights{};
        out.serial = d->serial;
        out.layers = d->count;
        out.dominantLowRes = DominantLayer(at);
        if (!d->count) return true;
        // Texel centres at (i + 0.5) / 8 cells.
        const float u = at.r * 8.0f - 0.5f, v = at.c * 8.0f - 0.5f;
        float rest = 1.0f;
        for (int l = 1; l < d->count; ++l)
        {
            out.weight[l] = Texel(d->map.data() + (l - 1) * 64 * 64, u, v, swapAxes);
            rest -= out.weight[l];
        }
        out.weight[0] = rest < 0.0f ? 0.0f : rest;
        for (int l = 0; l < d->count; ++l) out.surface[l] = d->surface[l];
        return true;
    }

    namespace
    {
        // MCCV: the sub-chunk walk (0x7C3A10) stores its data pointer at chunk +0x120 when the chunk
        // has one; MCNK flags 0x40 say it does (a reused chunk object can keep an old pointer, so the
        // flag is the guard). 145 entries in MCVT order, 4 bytes each, B G R A, 0x7F = neutral
        // [believed: wowdev's layout, check against the client's own terrain with the panel].
        constexpr size_t   kChunkVertexColors = 0x120;
        constexpr uint32_t kMcnkHasMccv = 0x40;

        // The client's terrain liquid probe (0x7A0820, verified in XWorkbench 2026-10-03):
        // bool __cdecl (const float pos[3], uint32_t* outType, float* outHeight, int checkTerrain).
        // It walks the chunk's liquid instances (chunk +0x108) whose tile exists at the cell, and takes
        // the first whose surface (+ a small margin) lies above pos.z; with checkTerrain it also wants
        // pos.z above the terrain. pos.z far below every surface therefore just asks "the surface here".
        constexpr uintptr_t kQueryTerrainLiquid = 0x007A0820;
        using QueryTerrainLiquidFn = bool(__cdecl*)(const float* pos, uint32_t* outType, float* outHeight, int checkTerrain);
    }

    bool VertexColorAt(float x, float y, float rgb[3])
    {
        ChunkSpot at;
        if (!Spot(x, y, at)) return false;
        if (!(*reinterpret_cast<const uint32_t*>(at.header + kHeaderFlags) & kMcnkHasMccv)) return false;
        const auto* colors = *reinterpret_cast<const uint8_t* const*>(at.chunk + kChunkVertexColors);
        if (!colors) return false;

        const float u = at.r - at.row, v = at.c - at.col;
        const int base = at.row * kRowStride + at.col;
        const int i00 = base, i01 = base + 1, i10 = base + kRowStride, i11 = base + kRowStride + 1, im = base + kCentreOffset;
        const float du0 = u, du1 = 1.0f - u, dv0 = v, dv1 = 1.0f - v;
        int a, b; float pa[2], pb[2];
        if (du0 <= du1 && du0 <= dv0 && du0 <= dv1) { a = i00; b = i01; pa[0] = 0; pa[1] = 0; pb[0] = 0; pb[1] = 1; }
        else if (du1 <= dv0 && du1 <= dv1)          { a = i10; b = i11; pa[0] = 1; pa[1] = 0; pb[0] = 1; pb[1] = 1; }
        else if (dv0 <= dv1)                        { a = i00; b = i10; pa[0] = 0; pa[1] = 0; pb[0] = 1; pb[1] = 0; }
        else                                        { a = i01; b = i11; pa[0] = 0; pa[1] = 1; pb[0] = 1; pb[1] = 1; }
        for (int ch = 0; ch < 3; ++ch)
        {
            const int byte = 2 - ch; // B G R A -> r = byte 2
            const float A[3] = { pa[0], pa[1], colors[a * 4 + byte] / 255.0f };
            const float B[3] = { pb[0], pb[1], colors[b * 4 + byte] / 255.0f };
            const float M[3] = { 0.5f, 0.5f, colors[im * 4 + byte] / 255.0f };
            rgb[ch] = PlaneHeight(A, B, M, u, v);
        }
        return true;
    }

    bool LiquidHeightAt(float x, float y, float& outZ)
    {
        const float pos[3] = { x, y, -100000.0f };
        uint32_t type = 0;
        float h = 0.0f;
        if (!reinterpret_cast<QueryTerrainLiquidFn>(kQueryTerrainLiquid)(pos, &type, &h, 0)) return false;
        outZ = h;
        return std::isfinite(h);
    }

    void ClearLayerCache() { g_decoded.clear(); }

    bool TerrainTypeAt(float x, float y, int& outType)
    {
        const float pos[3] = { x, y, 0.0f };
        int type = -1;
        if (!reinterpret_cast<TerrainTypeQueryFn>(kTerrainTypeQuery)(pos, &type)) return false;
        outType = type;
        return true;
    }
}
