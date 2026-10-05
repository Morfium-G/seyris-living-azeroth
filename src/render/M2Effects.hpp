// The client's M2 shader effects: one per (vertex set, pixel set) pair a model batch asks for,
// created on first use and cached for the process. Each effect holds its own shader-wrapper tables,
// filled by one set load (0x872D30) that creates every shader in them, so recording the effect when
// that load runs tells which wrappers are M2 shaders (for dumps, and later for shader patches).
#pragma once

#include "wxl/PluginApi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wxl_livingazeroth::m2effects
{
    // The effect's tables (XWorkbench 2026-10-05, the set load 0x872D30): vertex shaders at +0x2C
    // (0x5A wrappers, "Shaders\Vertex\<vs>"), pixel shaders at +0x194 (0x10 wrappers).
    constexpr size_t kVertexTable = 0x2C;
    constexpr int    kVertexEntries = 0x5A;
    constexpr size_t kPixelTable = 0x194;
    constexpr int    kPixelEntries = 0x10;

    struct Effect
    {
        uintptr_t   object = 0;
        std::string vertexName;  // e.g. "Diffuse_T1_Env"
        std::string pixelName;   // e.g. "Combiners_Opaque_Mod2x"
    };

    /// Hooks the set load. Call from WXL_Load.
    bool Install(const WXL_Api* api);

    /// Every effect loaded so far, in load order.
    const std::vector<Effect>& Effects();

    /// Whether a shader wrapper is in a recorded effect's vertex table. Effects are recorded before
    /// their set load creates the shaders, so this already holds while they're being created (and
    /// again when a device reset recreates them).
    bool IsVertexWrapper(const void* wrapper);
}
