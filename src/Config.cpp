#include "Config.hpp"

#include "debug/WindView.hpp"
#include "env/Shelter.hpp"
#include "features/GrassInstanced.hpp"
#include "features/GrassMotion.hpp"
#include "render/PostAA.hpp"
#include "wxl_seyris/SettingsApi.hpp"

namespace wxl_livingazeroth::config
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        const WXL_SeyrisSettingsApi* Settings(const WXL_Api* api)
        {
            static const WXL_SeyrisSettingsApi* settings = nullptr;
            static bool resolved = false;
            if (resolved) return settings;
            resolved = true;

            const auto* s = static_cast<const WXL_SeyrisSettingsApi*>(
                api->GetInterface(WXL_SEYRIS_SETTINGS_INTERFACE_NAME, WXL_SEYRIS_SETTINGS_INTERFACE_VERSION));
            if (!s)
                api->Log(WXL_LOG_WARN, kTag, "config: wxl-seyris-tools isn't loaded; using built-in defaults (no WarcraftXL.ini settings).");
            else if (!s->HasFeature("get-float"))
                api->Log(WXL_LOG_WARN, kTag, "config: wxl-seyris-tools %u.%u.%u is too old (needs settings 1.1.0, \"get-float\"); using built-in defaults.",
                         s->major, s->minor, s->patch);
            else
                settings = s;
            return settings;
        }

        bool Bool(const WXL_SeyrisSettingsApi* s, const char* key, const char* description, bool fallback)
        {
            return s->GetBool(kSection, key, description, fallback ? 1 : 0) != 0;
        }

        float Float(const WXL_SeyrisSettingsApi* s, const char* key, const char* description, float fallback)
        {
            return s->GetFloat(kSection, key, description, fallback);
        }
    }

    float GetFloat(const WXL_Api* api, const char* key, const char* description, float fallback)
    {
        const WXL_SeyrisSettingsApi* s = Settings(api);
        return s ? Float(s, key, description, fallback) : fallback;
    }

    void Apply(const WXL_Api* api)
    {
        const WXL_SeyrisSettingsApi* s = Settings(api);
        if (!s) return;

        // Built-in values are the fallbacks, so the ini starts out matching the code.
        grassinst::Settings& inst = grassinst::Tunables();
        inst.enabled = Bool(s, "InstancedGrass",
            "Draw grass with the instanced renderer (much faster at long grass distances). 0 = the client's own path.", inst.enabled);
        inst.buildBudgetMs = Float(s, "InstancedGrassBuildBudgetMs",
            "Milliseconds per frame spent preparing grass coming into view; the rest is drawn the client's way until a later frame.",
            inst.buildBudgetMs);
        inst.memoryLimitMB = Float(s, "InstancedGrassMemoryMB",
            "Most memory (MB) the instanced grass may use. It shares the client's 32-bit address space; too high can crash the client.",
            inst.memoryLimitMB);
        inst.maxMultiplier = Float(s, "GrassDensityMaxMultiplier",
            "Instanced grass only: your limit on grass density (plants drawn per placed plant, 1 = never denser, up to 8). How dense comes from GroundEffectDoodadDensity.cdbc.",
            inst.maxMultiplier);
        inst.maxRadius = Float(s, "GrassDensityMaxRadius", "Your limit on how far around you grass is made denser, in yards.", inst.maxRadius);
        inst.maxSpread = Float(s, "GrassDensityMaxSpread", "Your limit on how far an extra plant may move from its plant, in yards.", inst.maxSpread);

        grass::Settings& g = grass::Tunables();
        g.enabled = Bool(s, "GrassMotion", "Grass sways in the wind and parts around characters.", g.enabled);
        g.amplitude = Float(s, "GrassSway", "Grass tip movement in yards at full wind.", g.amplitude);
        g.flutter = Float(s, "GrassFlutter", "Extra per-blade shimmer in yards.", g.flutter);
        g.flutterSpeed = Float(s, "GrassFlutterSpeed", "Shimmer speed in radians per second.", g.flutterSpeed);
        g.anchor = Float(s, "GrassStiffBase", "Bottom fraction of each blade that never moves (0..0.9).", g.anchor);
        g.pushStrength = Float(s, "GrassPushStrength", "How far grass leans away from characters, in yards.", g.pushStrength);
        g.radiusScale = Float(s, "GrassPushRadiusScale", "Push radius as a multiple of a character's bounding radius.", g.radiusScale);
        g.minRadius = Float(s, "GrassMinPushRadius", "Smallest push radius in yards.", g.minRadius);
        g.mountedScale = Float(s, "GrassMountedRadiusScale", "Extra push radius factor while mounted.", g.mountedScale);

        postaa::Settings& aa = postaa::Tunables();
        const bool fxaa = Bool(s, "AntiAliasing",
            "Our own anti-aliasing (FXAA). Turn the client's multisampling off to use it: readable depth (for fog etc.) needs it off.",
            aa.mode != postaa::Mode::Off);
        aa.mode = fxaa ? postaa::Mode::Fxaa : postaa::Mode::Off;
        aa.subpixel = Float(s, "AntiAliasingSubpixel", "FXAA subpixel smoothing, 0 (sharpest) .. 1 (softest).", aa.subpixel);

        shelter::SetEnabled(Bool(s, "WindShelter", "Roofs and overhangs shelter the ground below from wind.", shelter::Enabled()));
        shelter::SetLeeEnabled(Bool(s, "WindLee", "Walls and cliffs cast a wind shadow on their downwind side.", shelter::LeeEnabled()));

        debug::SetShowArrows(Bool(s, "DebugWindArrows", "Debug: draw the wind as arrows around the player.", debug::ShowArrows()));

        api->Log(WXL_LOG_INFO, kTag, "config: settings read from [%s] in WarcraftXL.ini.", kSection);
    }
}
