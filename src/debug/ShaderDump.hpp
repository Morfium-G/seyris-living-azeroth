// Research helper: dumps the client's live grass vertex shaders (raw bytecode + a readable
// disassembly) to Logs\living-azeroth\, so a wind-aware replacement can match the stock shader's
// inputs and outputs exactly. Debug-only; triggered from the wind panel.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    /// Writes one .bin + .txt per live grass vertex shader. Returns how many were dumped; details
    /// (including anything skipped and why) go to the core log.
    int DumpGrassShaders(const WXL_Api* api);
}
