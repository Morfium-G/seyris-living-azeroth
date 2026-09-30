// Config defaults for every living-azeroth feature, from WTF\WXL\WarcraftXL.ini ([LivingAzeroth])
// through wxl-seyris-tools' "seyris.settings". The ini sets what each launch starts with; the debug
// panels still change everything at runtime (not written back). Missing keys are created with
// the built-in default and a comment on first run. Without wxl-seyris-tools (or an older one
// without floats) the built-in defaults apply and the log says why.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::config
{
    constexpr const char* kSection = "LivingAzeroth";

    /// Reads the runtime defaults into the features' tunables. Call once, after every extension
    /// has loaded (first OnFrame): the settings interface belongs to another extension.
    void Apply(const WXL_Api* api);

    /// One float from the section, or `fallback` without the settings interface. Also safe from a
    /// client hook that runs after the extensions loaded.
    float GetFloat(const WXL_Api* api, const char* key, const char* description, float fallback);
}
