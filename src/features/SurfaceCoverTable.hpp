// SurfaceCover.cdbc: which ground gets a cover (snow, sand, ...) and how it looks and behaves, per
// place and per painted texture.
//
// Scoped like every override table in this workspace (orchestration docs/principles.md): the most
// specific row wins, place first -- the terrain cell's area, its parent zones, the map, global --
// then within each place TexturePath, GroundEffectID, TerrainType, everything. A field of -1 takes
// that field from the next row in that order (ZOffset: exactly -1, since other negatives are real
// offsets). No row at all = no cover; Depth 0 switches it off.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::covertable
{
    /// SurfaceCover.cdbc Flags bits. They come from the row that decides Depth, except
    /// kFlagIgnoreSpecular, which applies to the CoverTexture named in the same row.
    enum Flag : uint32_t
    {
        kFlagPreventUnderwater = 0x1, // no cover below a liquid surface
        kFlagPreventOnLand     = 0x2, // cover only below a liquid surface (river/sea floors)
        kFlagUseVertexColor    = 0x4, // tint by the terrain's MCCV vertex colours
        kFlagIgnoreSpecular    = 0x8, // don't load/use the CoverTexture's _s specular map
    };

    struct Values
    {
        float    depth = 0.0f;         // yd; 0 = no cover
        float    maxSlope = 45.0f;     // degrees: steeper ground holds no cover...
        float    slopeFade = 15.0f;    // ...thinning out over this many degrees below that
        float    driftNoise = 0.35f;   // depth variation, share of the depth
        float    edgeBreakup = 0.5f;   // 0 smooth .. 1 ragged, patchy edges
        float    rim = 0.3f;           // trench rim height, share of the depth
        float    relaxSeconds = 30.0f; // trench back to flat
        uint32_t tintColor = 0;        // ARGB (alpha unused)
        float    tintStrength = 0.0f;  // 0 = the plain cover colour .. 1 = the tint colour
        int      coverTexture = 0;     // 0 = none, 1.. = CoverTexturePath(id)
        float    zOffset = 0.0f;       // yd the cover's base sits above (+) or below (-) the terrain
        float    wetness = 0.0f;       // 0 dry .. 1 soaked: darker, glossier
        uint32_t flags = 0;            // Flag bits
    };

    /// Cover textures drawn at once: the slots go to the textures most present in view.
    constexpr int kMaxCoverTextures = 8;

    /// How many distinct CoverTexture paths the loaded table names (no limit), and the path of id 1..that.
    int         CoverTextureCount();
    const char* CoverTexturePath(int index);
    /// Whether a row naming texture `index` sets kFlagIgnoreSpecular (that flag belongs to the
    /// CoverTexture named in the same row; the other flags come with Depth).
    bool        CoverTextureIgnoresSpecular(int index);

    /// (Re)loads DBFilesClient\SurfaceCover.cdbc through wxl-seyris-tools' cdbc reader.
    void Load(const void* cdbcApi);

    /// The row values for a terrain cell: its chunk's area, the map, its dominant layer's texture
    /// path (may be null), ground effect and that effect's TerrainType (-1 = none). Fields no row
    /// sets keep the defaults above. Cached; Generation() changes on reload.
    Values Resolve(uint32_t areaId, int mapId, const char* texturePath, uint32_t groundEffectId, int terrainType);

    unsigned    RowCount();
    const char* Status();
    uint32_t    Generation();
}
