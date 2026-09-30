// Grass draw-distance cap. The client rejects any groundEffectDist above a float in the exe (140.0;
// the CVar callback 0x78DB10 accepts 0 < value <= cap, anything else is refused with a console
// message and never applied). The instanced grass renderer makes long distances affordable, so
// the cap is raised to the configured value ([LivingAzeroth] GrassDistanceCap) -- unless
// something (a patched exe, another module) already raised it at least that far, in which case
// it is left alone.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::grassdistance
{
    /// Hooks the world CVar registration (0x78E400): the cap is raised right before groundEffectDist
    /// and its Config.wtf value are validated, which is after every extension has loaded, so the
    /// configured value can be read. Call from WXL_Load.
    void Install(const WXL_Api* api);

    /// First frame: applies the cap if the registration ran before the hook could (then a saved
    /// value above the old cap was already reset, and the log says so).
    void OnFirstFrame();

    /// The cap in effect (for the panel).
    float CurrentCap();
}
