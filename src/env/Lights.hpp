// The client's own lights (orchestration docs/r&d/immersion/point-lights-design.md, "Sources";
// offsets traced in lighting.md, "The client's light selection, traced"). Every light the client
// knows -- M2 model lights (torches, braziers) and a few global ones -- is a CM2Light in the M2
// scene: a global list and a 64 x 64 grid of chains (cells of 20 yd, hashed by position & 63).
// Read only; nothing here changes the client's lighting.
#pragma once

#include <cstdint>
#include <vector>

namespace wxl_livingazeroth::lights
{
    /// One CM2Light as the client holds it. Colours are linear floats as the client stores them.
    struct ClientLight
    {
        const void* address = nullptr;
        bool     global = false;        // from the scene's global list (else a grid cell)
        int      cell = -1;             // grid cell (y * 64 + x), -1 for global ones
        uint32_t type = 0;              // 1 = point; others are summed into ambient/diffuse
        bool     visible = false;       // +0x60
        bool     stale = false;         // +0x04 differs from the scene's generation (the client hides these)
        float    pos[3] = {};           // +0x0C, world
        float    dir[3] = {};           // +0x24
        float    ambient[3] = {};       // +0x30 [believed: summed as ambient for non-point lights]
        float    diffuse[3] = {};       // +0x3C, what a point light's device light gets as its colour
        float    extra[3] = {};         // +0x48 [unknown: summed for non-point lights]
        float    attenuation[3] = {};   // +0x54/+0x58/+0x5C: 1 / (a + b d + c d^2); constructor default (0, 0.7, 0.03)
    };

    struct Scan
    {
        bool     sceneFound = false;
        uint32_t globalCount = 0, gridCount = 0, pointCount = 0, visibleCount = 0, staleCount = 0;
        uint32_t defaultAttenuation = 0; // point lights still at the constructor's (0, 0.7, 0.03)
        bool     truncated = false;      // a chain was cut off (cycle guard)
    };

    /// Every light in the scene; `out` gets those within `range` yd of `center` (all if range <= 0),
    /// nearest first.
    Scan Gather(const float center[3], float range, std::vector<ClientLight>& out);

    /// The distance at which 1 / (a + b d + c d^2) drops to `share` of its value at 1 yd (-1 if never).
    float Reach(const float attenuation[3], float share);
}
