// Research helper: dumps the client's live shaders (raw bytecode + a readable disassembly) to
// Logs\living-azeroth\, so a replacement can match the stock shader's inputs and outputs exactly.
// Debug-only; grass from the wind panel, terrain from the surfaces panel.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    /// Writes one .bin + .txt per live grass vertex shader. Returns how many were dumped; details
    /// (including anything skipped and why) go to the core log.
    int DumpGrassShaders(const WXL_Api* api);

    /// Same for the terrain shaders: the "Terrain" vertex shaders and every terrain pixel-shader
    /// table (Terrain0, Terrain0_env, Terrain1/1w, Terrain2, Terrain3 and their PCF variants).
    int DumpTerrainShaders(const WXL_Api* api);

    /// Same for every M2 shader effect loaded so far (render/M2Effects): "m2vs_<name>_<slot>" and
    /// "m2ps_<name>_<slot>", plus one summary line per effect in the log.
    int DumpM2Shaders(const WXL_Api* api);
}
