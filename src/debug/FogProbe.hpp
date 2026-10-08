// Research probe (debug), ground fog phase 1 (orchestration docs/r&d/immersion/ground-fog-design.md,
// "Phases"): checks in-client what the fog pass will rely on.
//  - One captured frame: the world render's passes in order (M2 scene passes with their argument,
//    liquid passes, weather) with the draws inside and between them, and whether the camera is in a
//    liquid. Confirms the draw point (right before the weather, after the water).
//  - Water depth: the depth-write value (Gx state 15) the liquid passes ask for, and a switch that
//    forces it on during the water pass (check with the scene depth panel's overlay over a lake).
//  - The day-night globals coa-vfog names (fog, light band colours, storm weight, interior blend,
//    glow), raw, next to the fog the terrain actually got (VS c12, PS c2), so which band is the fog
//    colour is matched, not guessed.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::debug
{
    /// Hooks and the panel. From WXL_Load.
    void InstallFogProbe(const WXL_Api* api);
}
