// Fog wake: units pushing the ground fog aside (orchestration docs/r&d/immersion/ground-fog-design.md,
// "Creatures and wind moving the fog"). The fog field's 2 yd cells are too coarse for a person-sized
// hole, so the wake is its own small map around the player, drawn on the CPU each frame and read
// once per march step by the fog shader (features/GroundFog):
//
// every nearby unit (env/Actors) and a trail of its recent positions is drawn as soft capsules. Per
// texel: how much fog is pushed out (fading with the trail's age, a slight pile-up at the rim), the
// height the push reaches (the unit's body, not the whole column), and a displacement that drags the
// fog's patch pattern along the path and curls it to both sides (the swirl).
#pragma once

#include "../env/WorldQuery.hpp"

#include "wxl/PluginApi.h"

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl_livingazeroth::fogwake
{
    struct Settings
    {
        int   enabled = 1;
        float strength = 0.9f;        // how much fog a moving unit pushes out at its body (0..1)
        float standing = 0.35f;       // ... while standing still (its body still displaces some)
        float trailSeconds = 3.0f;    // how long the wake takes to fill back in
        float radiusScale = 2.25f;    // wake radius = the unit's radius x this (min 0.6 yd); owner found 2..2.5 reads best
        float spread = 1.0f;          // the wake widens by this x its radius over its lifetime
        float heightScale = 1.0f;     // the wake's height = the unit's height x this
        float rim = 0.25f;            // fog piled up at the wake's rim (share of the push)
        float swirl = 1.0f;           // how far the fog's pattern is dragged and curled (x ~2 yd)
    };
    Settings& Config();

    constexpr int   kSize = 64;       // texels per side
    constexpr float kTexel = 1.0f;    // yd per texel: the map covers +-32 yd around the player

    /// Once per frame, main thread, after actors::Refresh: trails and the wake map (CPU).
    void Update(float dt, const world::Snapshot& snap);

    /// The wake texture for this frame (A16B16G16R16F: r = push (rim < 0), g = the push's top z -
    /// `groundRef`, b/a = pattern displacement in yd), uploaded on demand. nullptr when off or empty.
    IDirect3DTexture9* Texture(IDirect3DDevice9* dev, float groundRef);

    /// The map's origin (world x, y of its texel (0, 0) corner) and 1 / its extent in yd.
    void Placement(float out[3]);

    void Release();
    void DrawPanelSection(const WXL_Api* api);
}
