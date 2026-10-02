// SurfaceCover.cdbc: which ground gets a cover (snow, sand, ...) and how deep, per place.
//
// Scoped like every override table in this workspace (orchestration docs/principles.md): the most
// specific row wins, place first -- the terrain cell's area, its parent zones, the map, global --
// then within each place GroundEffectID, TerrainType, everything. A field of -1 takes that field
// from the next row in that order. No row at all = no cover; Depth 0 switches a material off.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::covertable
{
    struct Values
    {
        float depth = 0.0f;         // yd; 0 = no cover
        float rim = -1.0f;          // rim height as a share of the depth; < 0 = the panel's default
        float relaxSeconds = -1.0f; // trench back to flat; < 0 = the panel's default
    };

    /// (Re)loads DBFilesClient\SurfaceCover.cdbc through wxl-seyris-tools' cdbc reader.
    void Load(const void* cdbcApi);

    /// The row values for a terrain cell: its chunk's area, the map, its dominant layer's ground
    /// effect and that effect's TerrainType (-1 = none). Cached; Generation() changes on reload.
    Values Resolve(uint32_t areaId, int mapId, uint32_t groundEffectId, int terrainType);

    unsigned    RowCount();
    const char* Status();
    uint32_t    Generation();
}
