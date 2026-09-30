// General stock-shader patching: recognize a stock vertex shader when the client creates it, and
// create a patched version instead. The patch is applied to the client's own bytecode (disassemble,
// insert assembly, reassemble), so everything the stock shader does -- every shadow variant,
// every output -- stays exactly as it was; only the inserted block is ours.
//
// One detour on the client's per-shader create entry serves every rule. Recognition is by version
// token + exact bytecode length (stable per client build). Rules must be registered before
// Install(), which must run from WXL_Load.
#pragma once

#include "wxl/PluginApi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wxl_livingazeroth::shaderpatch
{
    struct VertexRule
    {
        const char*           name;          // for logs and the debug panel
        uint32_t              versionToken;  // e.g. 0xFFFE0300 for vs_3_0
        std::vector<uint32_t> stockLengths;  // exact bytecode sizes of the shaders to patch
        std::string           prologue;      // inserted right after the version line (defs)
        const char*           anchor;        // insert `body` after the first line containing this
        std::string           body;          // the inserted instructions
    };

    struct RuleStatus
    {
        std::string name;
        unsigned    applied = 0;   // shaders created with the patch
        unsigned    failed = 0;    // matched but couldn't be patched (see lastError)
        std::string lastError;
    };

    void Register(VertexRule rule);

    /// Attaches the create detour. Call from WXL_Load (core arms detours once, after all loads).
    bool Install(const WXL_Api* api);

    std::vector<RuleStatus> Status();
}
