// FogSettings.cdbc: per place and fog kind, how the ground fog behaves and looks (orchestration
// docs/r&d/immersion/ground-fog-design.md, "Phase 3 design"; docs/cdbc-tables.md).
//
// Scoped by the workspace rule (orchestration docs/principles.md): place first -- the area, its
// parent zones, the map, global -- then within each place the kind's own row before Kind 0 (all
// kinds). -1 in a field takes it from the next row in that order; values never add up. Fields no
// row sets keep the built-in default of the kind.
#pragma once

#include <cstdint>

struct WXL_SeyrisCdbcApi;

namespace wxl_livingazeroth::fogsettings
{
    enum ColorMode : int
    {
        kColorStock = 0,   // the zone's stock fog colour
        kColorHue = 1,     // the stock colour's brightness with Color's hue and saturation
        kColorReplace = 2, // Color as is
    };

    struct Values
    {
        float    strength = 1.0f;       // multiplies the kind's target from its rules (0 = never here)
        float    density = 0.04f;       // extinction per yd at the ground, full amount
        float    thickness = 4.0f;      // yd: layer height (or pool depth when heavy) at full amount
        float    buildUp = 300.0f;      // half-life (s) toward a higher target
        float    burnOff = 900.0f;      // half-life (s) toward a lower target
        float    windThin = 0.5f;       // wind strength (0..1) above which it can't settle
        float    windDrift = 0.5f;      // how strongly the wind moves it (0..1)
        float    patchiness = 0.6f;     // noise amount (0..1)
        float    weight = 0.0f;         // 0 rises/thins upward .. 1 heavy, flat-topped, flows downhill
        int      colorMode = kColorStock;
        float    color[3] = { 1.0f, 1.0f, 1.0f };
        float    saturation = 1.0f;     // on top of the mode: 0 grey .. 1 as is
        float    brightness = 1.0f;
        uint32_t flags = 0;
    };

    /// Built-in values for a fog kind (fog::Kind, 0-based).
    Values Defaults(int kind);

    /// (Re)loads the table. Without the file every place uses the defaults.
    void Load(const WXL_SeyrisCdbcApi* cdbc);
    void Reload();
    const char* Status();
    unsigned    RowCount();
    uint32_t    Generation();

    /// The values for a fog kind (fog::Kind, 0-based) at a place, or with kind = -1 the "all kinds"
    /// rows only (the colour of the whole fog). Cached per (area, map, kind).
    const Values& Resolve(uint32_t areaId, int mapId, int kind);
}
