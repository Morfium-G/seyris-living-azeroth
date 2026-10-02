// Terrain-only ground queries straight from the loaded map chunks: the exact height of the drawn
// terrain surface (from the chunk's MCVT heights, same triangles the client draws), and the
// client's own "which TerrainType is here" lookup. WMOs and doodads are ignored on purpose -- this
// is the surface the deformable cover sits on.
#pragma once

namespace wxl_livingazeroth::terrain
{
    /// Height of the terrain surface at world (x, y). False when the chunk isn't loaded or the spot
    /// is a terrain hole.
    bool HeightAt(float x, float y, float& outZ);

    /// The client's terrain TerrainType query at world (x, y): the cell's dominant layer -> ground
    /// effect -> GroundEffectTexture's last column. False on holes, unloaded chunks or no row.
    bool TerrainTypeAt(float x, float y, int& outType);

    /// What the terrain cell at world (x, y) is made of, for scoped tables: the chunk's area, the
    /// cell's dominant layer's texture path (points into the tile's file data, valid while the tile
    /// is loaded; null if unknown), ground effect (GroundEffectTexture ID, 0 = none) and its
    /// TerrainType (-1 = none). Same chain as the client's own query. False on holes or unloaded
    /// chunks.
    struct Surface { unsigned area = 0; const char* texture = nullptr; unsigned groundEffect = 0; int terrainType = -1; };
    bool SurfaceAt(float x, float y, Surface& out);
}
