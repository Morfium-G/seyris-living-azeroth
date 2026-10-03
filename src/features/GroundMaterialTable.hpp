// Ground materials: what the ground is made of, and where (orchestration
// docs/r&d/immersion/world-fields.md, "Kind 1").
//
// Two tables:
//  - GroundMaterialSelector.cdbc: WHERE a material lies. Scoped like every override table in this
//    workspace (docs/principles.md): the most specific row wins, place first -- the terrain cell's
//    area, its parent zones, the map, global -- then within each place TexturePath, GroundEffectID,
//    TerrainType, everything. The result is a material ID.
//  - GroundMaterial.cdbc: WHAT a material is, properties only. A field of -1 takes the value from
//    the row's Parent material (0 = no parent).
// Feature tables (SurfaceCover, ...) are keyed by material ID and inherit along the same Parent chain.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::materials
{
    struct Values
    {
        float    restMoisture = 0.3f; // 0 bone dry .. 1 soaked: what the texture already shows
        float    stiffness = 0.0f;    // resistance to deformation: 0 gives way completely .. 1 rigid
        uint32_t flags = 0;           // reserved
    };

    /// (Re)loads both tables through wxl-seyris-tools' cdbc reader.
    void Load(const void* cdbcApi);

    /// The material for a terrain cell: its chunk's area, the map, the layer's texture path (may be
    /// null), ground effect and that effect's TerrainType (-1 = none). 0 = no material (no selector
    /// matched, or the winning one says MaterialID 0).
    /// Cached; Generation() changes on reload.
    uint32_t Select(uint32_t areaId, int mapId, const char* texturePath, uint32_t groundEffectId, int terrainType);

    /// The material's values, with -1 fields taken along its Parent chain; defaults where no row sets them.
    Values Get(uint32_t materialId);

    /// The material's Parent (0 = none, or an unknown ID).
    uint32_t Parent(uint32_t materialId);

    /// Whether a GroundMaterial row with this ID exists.
    bool Exists(uint32_t materialId);

    /// The row's Name ("" for none/unknown).
    const char* Name(uint32_t materialId);

    unsigned    MaterialCount();
    unsigned    SelectorCount();
    const char* Status();
    uint32_t    Generation();

    /// Walks a Parent chain safely (stops at 0, unknown IDs and cycles): calls fn(id) for id, its
    /// parent, ... until fn returns false.
    template <class Fn> void ForChain(uint32_t materialId, Fn fn)
    {
        for (int depth = 0; materialId && depth < 16; ++depth)
        {
            if (!fn(materialId)) return;
            materialId = Parent(materialId);
        }
    }
}
