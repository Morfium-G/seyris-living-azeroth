// Published as "seyris.settings" via WXL_Api::PublishInterface/GetInterface. Not a core header --
// copy this file into a consuming module's own repo, same as CdbcApi.hpp (see that file for the
// full rationale and the additive-growth/versioning convention this mirrors).
//
// GetBool is stateless from the caller's side: it does the create-or-read-existing logic in one
// call and returns the resolved value directly, rather than handing back a settings object --
// wxl_seyris::settings::BoolSetting (the C++ class this wraps) can't safely cross the DLL boundary.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SEYRIS_SETTINGS_INTERFACE_NAME    "seyris.settings"
#define WXL_SEYRIS_SETTINGS_INTERFACE_VERSION 1u

typedef struct WXL_SeyrisSettingsApi
{
    uint32_t structSize;
    uint32_t major, minor, patch;

    /// Named capability check, independent of major/minor/patch. Always safe to call.
    int (__cdecl* HasFeature)(const char* name);

    /**
     * @brief Reads (or, on first run, creates) a boolean setting in WTF\WXL\WarcraftXL.ini.
     * @param ns           section name.
     * @param key          key name within that section.
     * @param description  written as a leading comment when the key doesn't exist yet.
     * @param defaultValue used (and persisted) when the key doesn't exist yet.
     * @return the resolved value: whatever's already on disk, or defaultValue on first run.
     */
    int (__cdecl* GetBool)(const char* ns, const char* key, const char* description, int defaultValue);

    // --- 1.1.0 (feature "get-float") ---

    /**
     * @brief Reads (or, on first run, creates) a float setting in WTF\WXL\WarcraftXL.ini. Same
     *        create-or-read rules as GetBool; a present but unparsable value reads as defaultValue.
     */
    float (__cdecl* GetFloat)(const char* ns, const char* key, const char* description, float defaultValue);
} WXL_SeyrisSettingsApi;

#ifdef __cplusplus
}
#endif
