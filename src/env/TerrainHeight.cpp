#include "TerrainHeight.hpp"

#include "game/Adt.hpp"

#include <cmath>
#include <cstdint>

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

    bool TerrainTypeAt(float x, float y, int& outType)
    {
        const float pos[3] = { x, y, 0.0f };
        int type = -1;
        if (!reinterpret_cast<TerrainTypeQueryFn>(kTerrainTypeQuery)(pos, &type)) return false;
        outType = type;
        return true;
    }
}
