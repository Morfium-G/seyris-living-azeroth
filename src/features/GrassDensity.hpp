// Grass density content: GroundEffectDoodadDensity.cdbc. Which doodads get how many GPU copies,
// how far around the player and how spread out -- per place and per "what".
//
// Resolution follows the workspace rule for scoped override tables (orchestration
// docs/principles.md): the most specific matching row wins. Place first -- the chunk's area
// chain (sub-area, zone, ...) -> map -> global -- then, within each place, the most specific
// "what": doodad+ground effect -> doodad -> ground effect -> everything (0). A -1 field takes its
// value from the next row in that order; values are never combined. No row at all = no copies.
// The player's limits (ini / panel) cap the result afterwards; they are not part of the table.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::grassdensity
{
    struct Values
    {
        float multiplier = 1.0f; // plants drawn per placed plant (1 = no copies)
        float radius = 0.0f;     // yards around the player
        float spread = 1.0f;     // most yards a copy moves from its plant
    };

    /// Loads (or reloads) DBFilesClient\GroundEffectDoodadDensity.cdbc. Missing file = no rows.
    /// cdbcApi may be null.
    void Load(const void* cdbcApi);

    unsigned    RowCount();
    const char* Status();     // load result, for the panel
    uint32_t    Generation(); // changes on every load: layers built against an older one rebuild

    /// The values for one doodad drawn in a chunk. `effectId` 0 = not known.
    Values Resolve(uint32_t chunkAreaId, int mapId, uint32_t doodadId, uint32_t effectId);
}
