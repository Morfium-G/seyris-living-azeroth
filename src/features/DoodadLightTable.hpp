// Doodad lights by data (orchestration docs/r&d/immersion/point-lights-design.md, "Tables"): lights
// added to placed models without editing the models. Two tables, never named Light* (the sky's
// Light*.dbc files):
//  - DoodadLightProperties.cdbc: what a light is (colour, radius, flicker, spot cone, ...).
//  - DoodadLightAssignment.cdbc: where lights go -- a model path, an attachment on it (origin,
//    attachment point, bone, or a particle emitter's position), an offset, a light. A row's
//    SuppressModelLights flag hides the model's own light entries from our lights (LightID 0 = only
//    that).
// Read by env/Lights at its doodad scan; edited in-game by the light editor panel (debug/LightEditor),
// which saves back to the same files (backing up the old one as .bak first).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct WXL_SeyrisCdbcApi;

namespace wxl_livingazeroth::lighttable
{
    enum Type : uint32_t { kPoint = 0, kSpot = 1 };
    enum Attach : uint32_t { kOrigin = 0, kAttachmentPoint = 1, kBone = 2, kParticleEmitter = 3, kAttachCount };
    enum AssignmentFlag : uint32_t { kSuppressModelLights = 0x1 };
    /// DoodadLightProperties.Flags (the column was reserved, so the layout doesn't change).
    enum PropertiesFlag : uint32_t { kBakeOcclusion = 0x1 }; // walls/floors between it and a surface block it (lights::Settings bake*)
    enum FlickerMode : int32_t { kFlickerDefault = -1, kFlickerOff = 0, kFlickerSmooth = 1, kFlickerNoise = 2, kFlickerSteps = 3 };

    /// DoodadLightProperties.cdbc. A value of -1 takes the default (the lights panel's settings).
    struct Properties
    {
        uint32_t    id = 0;
        std::string name;
        uint32_t    type = kPoint;
        uint32_t    color = 0xFFA050;       // 0xRRGGBB
        float       intensity = 1.4f;       // x the colour (the stock torches: ~1.4)
        float       radius = -1.0f;         // yd; exactly 0 light here
        float       falloff = -1.0f;        // shape: 1 smooth (default), higher = tighter around the source
        float       innerAngle = 20.0f;     // spot: degrees from the axis at full strength...
        float       outerAngle = 35.0f;     // ...none beyond this
        int32_t     flickerMode = kFlickerDefault;
        float       flickerSpeed = -1.0f;   // x the default pace
        float       flickerAmount = -1.0f;  // 0..1 how much it dips
        uint32_t    flags = 0;              // reserved
    };

    /// DoodadLightAssignment.cdbc.
    struct Assignment
    {
        uint32_t    id = 0;
        std::string modelPath;              // as the panel shows it; case and / vs \ don't matter
        uint32_t    attachType = kOrigin;
        int32_t     attachIndex = 0;        // the attachment point's ID, bone index or emitter index
        std::string attachName;             // reserved: named attachment points (WXL-50)
        float       offset[3] = {};         // yd, in the model's space (scaled with the doodad)
        float       direction[3] = { 0.0f, 0.0f, -1.0f }; // spot lights: where the cone points (model space)
        uint32_t    lightId = 0;            // DoodadLightProperties ID; 0 = no light (suppress only)
        uint32_t    flags = 0;              // AssignmentFlag
    };

    /// (Re)loads both tables from DBFilesClient.
    void Load(const WXL_SeyrisCdbcApi* cdbc);
    const char* Status();
    uint32_t    Generation(); // changes on load and on every edit

    const std::vector<Properties>& AllProperties();
    const std::vector<Assignment>& AllAssignments();
    const Properties* FindProperties(uint32_t id);

    /// The assignments for a model path (normalized: lower case, backslashes).
    std::vector<const Assignment*> ForModel(const std::string& normalizedPath);
    std::string Normalize(const std::string& path);

    // --- editing (the light editor); every edit bumps Generation() so the lights update live ---
    Properties* EditProperties(uint32_t id);
    Assignment* EditAssignment(uint32_t id);
    uint32_t    AddProperties();                 // returns the new ID
    uint32_t    AddAssignment(const std::string& modelPath);
    void        RemoveProperties(uint32_t id);
    void        RemoveAssignment(uint32_t id);
    void        Touch();                         // after changing a field through Edit*()
    bool        Dirty();                         // edited since the last load/save

    /// Writes both tables to DBFilesClient (each old file kept as <file>.bak). Message for the panel.
    bool Save(char* message, size_t messageSize);
}
