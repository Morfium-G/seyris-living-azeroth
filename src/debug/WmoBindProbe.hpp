// Research probe (debug): which shader effect and which vertex/pixel permutation the client binds for
// WMO batches, split by the interior/exterior batch loop and the group's indoor flag. For the WMO
// interior lighting rule (orchestration docs/r&d/immersion/point-lights-design.md, "WMO interiors").
#pragma once

#include "wxl/PluginApi.h"

#include <string>

namespace wxl_livingazeroth::debug
{
    /// Hooks the WMO batch loops and the effect bind. From WXL_Load.
    void InstallWmoBindProbe(const WXL_Api* api);

    int& WmoBindProbeEnabled();
    void ClearWmoBindProbe();

    /// One line per (loop, indoor group, effect, vertex index, pixel index) seen since the last clear.
    std::string WmoBindProbeReport();
}
