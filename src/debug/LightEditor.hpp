// The light editor (orchestration docs/r&d/immersion/point-lights-design.md, step 4): the placed
// models near the player with their bones, attachment points, particle emitters and own lights; their
// DoodadLightAssignment rows and every DoodadLightProperties row, edited live (the lights update at
// once) and saved back to DBFilesClient (old files kept as .bak), or reloaded from disk.
#pragma once

#include "wxl/PluginApi.h"

struct WXL_SeyrisCdbcApi;

namespace wxl_livingazeroth::debug
{
    void RegisterLightEditor(const WXL_Api* api);
    /// For "Reload from disk" (resolved on the first frame).
    void SetLightEditorCdbc(const WXL_SeyrisCdbcApi* cdbc);
}
