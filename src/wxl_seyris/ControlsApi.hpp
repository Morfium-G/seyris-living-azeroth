// Published as "seyris.controls" via WXL_Api::PublishInterface/GetInterface. Not a core header --
// copy this file into a consuming module's own repo, same as CdbcApi.hpp (see that file for the
// versioning/growth convention).
//
// A module-agnostic control panel for the F9 overlay. Modules register:
//  - quick features: a short strip of a feature's most used toggles and options, drawn by the
//    module's own callback (with the WXL_Api Ui* calls) when a strip of a quick view shows it.
//    Quick views (several strips, each with a feature dropdown) are composed and saved by the user;
//  - pages: a feature's full panel, drawn by the module's callback inside one shared page window
//    (a page dropdown instead of one overlay window per panel).
// wxl-seyris-tools only hosts and remembers; it knows nothing about what a feature does.
//
// Callbacks run inside the overlay's frame, like a WXL_PanelFn: only the Ui* calls are valid there.
// Strings are copied. Register from the first frame on (GetInterface isn't reliable in WXL_Load:
// extensions load in name order).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SEYRIS_CONTROLS_INTERFACE_NAME    "seyris.controls"
#define WXL_SEYRIS_CONTROLS_INTERFACE_VERSION 1u

typedef void (__cdecl* WXL_SeyrisDrawFn)(void* user);

typedef struct WXL_SeyrisControlsApi
{
    uint32_t structSize;
    uint32_t major, minor, patch;

    /// Named capability check ("quick-features", "pages"). Always safe to call.
    int (__cdecl* HasFeature)(const char* name);

    /**
     * @brief Registers a quick feature for the quick views.
     * @param group  the module or area it belongs to (e.g. "Living Azeroth"), for the dropdown.
     * @param name   the feature (e.g. "Fog"); (group, name) is its saved identity, keep it stable.
     * @return 1 if registered, 0 if (group, name) exists already or an argument is missing.
     */
    int (__cdecl* AddQuickFeature)(const char* group, const char* name, WXL_SeyrisDrawFn draw, void* user);

    /**
     * @brief Registers a page for the page window. Same rules as AddQuickFeature.
     */
    int (__cdecl* AddPage)(const char* group, const char* name, WXL_SeyrisDrawFn draw, void* user);
} WXL_SeyrisControlsApi;

#ifdef __cplusplus
}
#endif
