// Terrain-only ground queries straight from the loaded map chunks: the exact height of the drawn
// terrain surface (from the chunk's MCVT heights, same triangles the client draws), and the
// client's own "which TerrainType is here" lookup. WMOs and doodads are ignored on purpose -- this
// is the surface the deformable cover sits on.
#pragma once

#include <cstdint>

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

    /// The painted strength of every texture layer at world (x, y), from the chunk's alpha maps
    /// (decoded once per chunk and cached; bilinear between the 64x64 texels, ~0.5 yd). Layer 0
    /// gets what the others leave (as the client blends). `serial` identifies the decoded chunk
    /// (changes when a chunk object is reused for another place), for caches keyed by chunk.
    /// False on holes or unloaded chunks.
    struct LayerWeights
    {
        unsigned serial = 0;
        int      layers = 0;
        float    weight[4] = {};
        Surface  surface[4];
        int      dominantLowRes = -1; // the client's own dominant layer for the cell (low-res map)
    };
    bool LayerWeightsAt(float x, float y, LayerWeights& out, bool swapAxes = false);

    /// The terrain's MCCV vertex colour at world (x, y), interpolated over the same triangles as
    /// HeightAt: rgb 0..1, where 0.5 is neutral (the terrain multiplies by 2 x the colour). False
    /// (and no change to `rgb`) when the chunk has no MCCV, isn't loaded, or the spot is a hole.
    bool VertexColorAt(float x, float y, float rgb[3]);

    /// The area ID of the loaded terrain chunk at world (x, y), holes included (unlike SurfaceAt).
    /// False when no chunk is loaded there.
    bool ChunkAreaAt(float x, float y, uint32_t& outArea);

    /// The surface height of terrain liquid (rivers, lakes, sea; not WMO liquid) at world (x, y),
    /// from the client's own liquid probe. False where the terrain cell has no liquid.
    bool LiquidHeightAt(float x, float y, float& outZ);

    /// The same, plus the liquid's LiquidType.dbc ID (as baked into the ADT).
    bool LiquidAt(float x, float y, float& outZ, uint32_t& outLiquidType);

    /// A LiquidType's category from the client's LiquidType table (its Type column): 0 water,
    /// 1 ocean, 2 magma, 3 slime; -1 for an unknown ID. And its Name ("" if unknown).
    enum LiquidCategory : int { kLiquidWater = 0, kLiquidOcean = 1, kLiquidMagma = 2, kLiquidSlime = 3 };
    int         LiquidCategoryOf(uint32_t liquidType);
    const char* LiquidNameOf(uint32_t liquidType);

    /// Forgets every decoded chunk (call when the map changes or the world is left). Texture
    /// names in LayerWeights from before are invalid afterwards.
    void ClearLayerCache();
}
