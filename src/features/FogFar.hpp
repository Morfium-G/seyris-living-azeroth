// The far fog map (orchestration docs/r&d/immersion/ground-fog-design.md, "Phase 3 step 5", part 4):
// the ground fog beyond the near field (env/Fog, +-128 yd), so it doesn't end in a wall. One texel
// per terrain chunk (33 yd), 64 x 64 chunks (+-1 km) laid out like the regional zone map
// (regional::Map): per chunk its zone's typical fog (env/Fog's zone memory: mist, area fog, haze)
// with that zone's own density, thickness and colour, on the chunk's lowest ground, and the highest
// fog top among the chunk and its neighbours (the march skips the air above it). No near detail:
// hollows, pools, water fog, steam, units and lights stay in the near field.
#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl_livingazeroth::fogfar
{
    /// A kind's colour at a place (the renderer's FogColor), for the far map's zones.
    using ColourFn = void (*)(int kind, uint32_t area, float out[3]);

    struct Maps
    {
        IDirect3DTexture9* field = nullptr;   // A16B16G16R16F: r density per yd, g thickness, b ground - ref, a top - ref (dilated)
        IDirect3DTexture9* colour = nullptr;  // A8R8G8B8: rgb colour / 2
        float originX = 0.0f, originY = 0.0f; // world x, y of the map's (0, 0) corner
        float invExtent = 0.0f;               // 1 / its extent (yd)
        float highestTop = -1.0e9f;           // world z of its highest fog top (the ray's height clip)
    };

    /// Each frame before the fog draws: samples some chunks' ground, rebuilds and uploads the map at
    /// most twice a second (at once when the window moved). False: nothing to draw far (no zone map
    /// yet, or no fog in any zone around).
    bool Update(IDirect3DDevice9* dev, float groundRef, ColourFn colour, Maps& out);

    /// The far map's fog top at a world position (its chunk's, dilated; world z), as last built;
    /// -1e5 where there's none. For the near field's top map where its cells stand in for the far map.
    float TopAt(float x, float y);

    void Release();
}
