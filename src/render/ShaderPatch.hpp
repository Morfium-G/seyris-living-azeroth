// General stock-shader patching: recognize a stock shader when the client creates it, and create a
// patched version instead. The patch is applied to the client's own bytecode (disassemble,
// insert assembly, reassemble), so everything the stock shader does -- every shadow variant,
// every output -- stays exactly as it was; only the inserted block is ours.
//
// One detour per create entry (vertex, pixel) serves every rule. Two kinds of rule:
//  - VertexRule: recognized by version token + exact bytecode length (stable per client build),
//    edited by inserting text after fixed anchors.
//  - TableRule: recognized by the wrapper being in one of the client's shader tables (the set loader
//    fills a table before any of its shaders is created, so this holds for every variant, GPU class
//    and device reset), edited by a callback on the disassembly.
// Rules must be registered before Install(), which must run from WXL_Load.
#pragma once

#include "wxl/PluginApi.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
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

    struct TableRule
    {
        const char* name;
        bool        pixel;   // pixel shaders (else vertex)
        std::vector<std::pair<uintptr_t, int>> tables; // (address, entries) of wrapper-pointer tables
        /// Edits the disassembly in place. False = leave this shader stock (`why` says why: a variant
        /// the patch doesn't apply to is skipped, not failed).
        std::function<bool(std::string& source, std::string& why)> edit;
        /// Instead of fixed tables: whether a wrapper belongs to this rule (tables that live in heap
        /// objects, known only once the client builds them). Used when set.
        std::function<bool(const void* wrapper)> contains = nullptr;
    };

    struct RuleStatus
    {
        std::string name;
        unsigned    applied = 0;   // shaders created with the patch
        unsigned    skipped = 0;   // matched, but the edit didn't apply to this variant (left stock)
        unsigned    failed = 0;    // matched but couldn't be patched (see lastError)
        std::string lastError;
    };

    void Register(VertexRule rule);
    void Register(TableRule rule);

    /// Attaches the create detour. Call from WXL_Load (core arms detours once, after all loads).
    bool Install(const WXL_Api* api);

    std::vector<RuleStatus> Status();

    /// d3dcompiler_47 round trip, shared with other shader derivations. Both return false with a
    /// reason in `error` when the compiler is missing or the call fails.
    bool Disassemble(const void* code, size_t length, std::string& text, std::string& error);
    bool Assemble(const std::string& source, const char* name, std::vector<uint8_t>& code, std::string& error);
}
