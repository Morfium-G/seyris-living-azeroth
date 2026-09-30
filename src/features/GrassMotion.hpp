// Grass motion: the stock grass (ground-effect) vertex shaders are patched to bend each blade with
// our wind model -- an 8x8 grid of wind samples around the player (direction, strength, gusts,
// roof shelter and lee already applied), bilinearly sampled per blade -- plus a per-blade flutter
// and parting around the player. Design and stock-shader analysis:
// orchestration/docs/r&d/immersion/ground-effects.md.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::grass
{
    /// Runtime tunables (debug panel; not saved).
    struct Settings
    {
        bool  enabled       = true;
        float amplitude     = 0.35f;  // tip displacement in yards at wind strength 1
        float flutter       = 0.08f;  // extra per-blade shimmer, yards
        float flutterSpeed  = 3.0f;   // shimmer speed, radians per second
        float anchor        = 0.15f;  // bottom fraction of a blade that never moves
        float pushStrength  = 0.5f;   // how far blades lean away from the player, yards
        float pushRadius    = 1.6f;   // player influence radius, yards
    };

    /// Registers the shader patch and attaches the constant hooks. Call from WXL_Load. Does nothing
    /// (and says why) when the official wxl-grasswind module is loaded, since both patch the same
    /// shaders and registers.
    void Install(const WXL_Api* api);

    Settings& Tunables();

    /// Why grass motion is off, or null when it's installed.
    const char* DisabledReason();
    unsigned ChunkUploadsLastFrame();
}
