// Nearby actors: every unit (players, NPCs, creatures) close to the player, nearest first, with a
// size taken from the server-sent bounding radius. Refreshed once per frame on the main thread.
// Consumers: grass parting now; later fog swirl, snow/sand footprints, water ripples.
#pragma once

#include <cstdint>
#include <vector>

namespace wxl_livingazeroth::actors
{
    struct Actor
    {
        unsigned long long guid = 0;
        float    pos[3] = {};
        float    boundingRadius = 0.0f; // as the server sends it (UNIT_FIELD_BOUNDINGRADIUS)
        float    combatReach = 0.0f;    // UNIT_FIELD_COMBATREACH; players usually 1.5 (sanity check)
        bool     mounted = false;       // UNIT_FIELD_MOUNTDISPLAYID != 0
        uint32_t displayId = 0, nativeDisplayId = 0;
        float    widthNow = 0.0f, widthNative = 0.0f; // collision widths, only filled when morphed
        float    effectiveRadius = 0.0f;              // bounding radius, scaled for morphs/shapeshifts
        bool     isPlayer = false;
        float    distance = 0.0f;       // to the player, yards
    };

    /// Main thread only (object enumeration requirement). `center` is usually the player.
    void Refresh(const float center[3], float range);

    /// Nearest first.
    const std::vector<Actor>& Nearby();
}
