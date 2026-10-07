// The client's own lights (orchestration docs/r&d/immersion/point-lights-design.md, "Sources";
// offsets traced in lighting.md, "The client's light selection, traced"). Every light the client
// knows -- M2 model lights (torches, braziers) and a few global ones -- is a CM2Light in the M2
// scene: a global list and a 64 x 64 grid of chains (cells of 20 yd, hashed by position & 63).
// Read only; nothing here changes the client's lighting.
#pragma once

#include "WorldQuery.hpp"

#include <cstdint>
#include <vector>

namespace wxl_livingazeroth::lights
{
    /// One CM2Light as the client holds it. Colours are linear floats as the client stores them.
    struct ClientLight
    {
        const void* address = nullptr;
        bool     global = false;        // from the scene's global list (else a grid cell)
        int      cell = -1;             // grid cell (y * 64 + x), -1 for global ones
        uint32_t type = 0;              // 1 = point; others are summed into ambient/diffuse
        bool     visible = false;       // +0x60
        bool     stale = false;         // +0x04 differs from the scene's generation (the client hides these)
        float    pos[3] = {};           // +0x0C, world
        float    dir[3] = {};           // +0x24
        float    ambient[3] = {};       // +0x30 [believed: summed as ambient for non-point lights]
        float    diffuse[3] = {};       // +0x3C, what a point light's device light gets as its colour
        float    extra[3] = {};         // +0x48 [unknown: summed for non-point lights]
        float    attenuation[3] = {};   // +0x54/+0x58/+0x5C: 1 / (a + b d + c d^2); constructor default (0, 0.7, 0.03)
    };

    struct Scan
    {
        bool     sceneFound = false;
        uint32_t globalCount = 0, gridCount = 0, pointCount = 0, visibleCount = 0, staleCount = 0;
        uint32_t defaultAttenuation = 0; // point lights still at the constructor's (0, 0.7, 0.03)
        bool     truncated = false;      // a chain was cut off (cycle guard)
    };

    /// Every light in the scene; `out` gets those within `range` yd of `center` (all if range <= 0),
    /// nearest first.
    Scan Gather(const float center[3], float range, std::vector<ClientLight>& out);

    /// The distance at which 1 / (a + b d + c d^2) drops to `share` of its value at 1 yd (-1 if never).
    float Reach(const float attenuation[3], float share);

    // --- our light list (point-lights-design.md) ------------------------------------------------
    // Built from the placed doodads near the player, not from the scene grid (the grid only holds
    // lights of models in view). Per model light: the position the client last gave it, or (never
    // placed: the model hasn't been in view) its file position through the doodad's world matrix;
    // its colour from the client, or the file's first diffuse colour x intensity. Lights closer than
    // the merge distance merge (Blizzard's torch groups). Every light within the range is used (up to
    // kMaxPool), fading out toward the range; a grid of cells around the camera lists, per cell, the
    // lights that reach into it (clustered list, point-lights-design.md "Clustered light list"), so
    // each surface only loops over its own cell's lights.

    constexpr int kMaxLights = 24;      // per surface-cover patch (vertex constants c20..c115)
    constexpr int kMaxPool = 512;       // lights in range, in all
    constexpr int kMaxPerCellCap = 32;  // the shaders' loop bound; the setting below picks up to this
    constexpr int kMaxCells = 64;       // per side

    struct Settings
    {
        int   enabled = 1;           // our lights on the cover (and later the terrain)
        float radius = 16.0f;        // yd: the light is exactly 0 here (stock lights reach ~13 yd to 5%)
        float brightness = 1.0f;     // x the client's colour
        float range = 256.0f;        // yd from the camera: no lights beyond, fading over the last third
        float cellSize = 16.0f;      // yd per grid cell; the grid reaches the range (at most kMaxCells per side)
        int   maxPerCell = 16;       // lights a cell can hold (the strongest at its centre win)
        float mergeDistance = 1.0f;  // yd: closer lights merge
        float flicker = 0.15f;       // 0..1: how much a flame's light dips (each light its own phase)
        float flickerSpeed = 1.0f;   // x the default pace (~3 dips a second)
        int   models = 1;            // our lights on M2s and WMOs too (per vertex, like their stock lights)
        int   modelStockOff = 1;     // ... replacing the client's up to 4 per model (else added to them)
        float bakedAdd = 1.0f;       // WMO surfaces with baked light (interiors): x ours before filling up to it
        int   indoorSkipsTerrain = 1; // lights inside an indoor WMO group don't light the terrain and the cover
                                      // (cellars, crypts: no glow on the ground above; door-frame torches lose the entrance)
        // Baked occlusion (point-lights-design.md, "Light leaking through floors and walls"): per light,
        // rays out to its radius against terrain and WMOs (not models), once, cached by position.
        int   bakeAll = 0;            // debug: bake every light, not only those whose DoodadLightProperties flag asks for it
        int   bakeRaysPerFrame = 2000; // ray budget per frame for the baking
        int   bakeEnabled = 1;        // apply baked occlusion at all
    };

    constexpr int kOccSide = 8;                       // directions per side of a light's octahedral tile
    constexpr int kOccCells = kOccSide * kOccSide;    // 64 distances per light
    Settings& Config();

    /// One light as the renderers get it: world position, colour (already x brightness x fade x
    /// flicker), radius, falloff shape (1 = smooth default, higher = tighter), and a spot cone: the
    /// direction it points (world, unit) with cos of the outer angle and 1 / (cos inner - cos outer);
    /// a point light has cosOuter -2 (always inside).
    struct ActiveLight
    {
        float pos[3];
        float color[3];
        float radius;
        float falloff = 1.0f;
        float spotDir[3] = { 0.0f, 0.0f, -1.0f };
        float cosOuter = -2.0f;
        float spotScale = 1.0f;
        float dip = 0.0f;   // this frame's flicker dip (0 = full, the colour is already scaled by 1 - dip)
        bool  indoor = false; // inside an indoor WMO group (IsIndoor)
        const void* model = nullptr; // the model instance it comes from (the editor's markers); a merged light keeps the first
        // Baked occlusion, when complete: per direction (octahedral, kOccSide x kOccSide, from the light
        // outward), how far it reaches before terrain or a WMO. Valid for this frame. Null = none.
        const float* occlusion = nullptr;
    };

    /// Whether a point lies in an indoor WMO group: the client's own viewer locate (core
    /// wmo::kLocateViewerMapObjs, called the way its caller 0x795D40 does, on a short segment straight
    /// down from the point) and the group's MOGI flags (0x7AE7B0: [wmo +0x130] + group x 0x20, 0 while
    /// the WMO isn't loaded) & 0x2000. Cached per half yard.
    bool IsIndoor(const float pos[3]);

    /// Every few frames: rescan the doodads (they don't move), refresh positions and colours.
    void Update(float dt, const world::Snapshot& snap);

    /// The lights to draw this frame (at most kMaxPool), in a stable order between scans (so the
    /// grid only changes when the scan or the camera's cell does).
    const std::vector<ActiveLight>& Active();

    /// The cell grid over Active(): cells x cells, cell (x, y) covers world
    /// [origin + (x, y) * cellSize, + cellSize). Cell c = y * cells + x holds count[c] light indices
    /// at index[c * perCell ..]. Empty (cells 0) while our lights are off.
    struct Grid
    {
        float originX = 0.0f, originY = 0.0f, cellSize = 16.0f;
        int   cells = 0, perCell = 0;
        std::vector<uint16_t> index;
        std::vector<uint8_t>  count;
        unsigned generation = 0;      // changes whenever index/count do
    };
    const Grid& CellGrid();

    struct Stats
    {
        unsigned models = 0, modelsInRange = 0, modelLights = 0, fromClient = 0, fromWorldMatrix = 0,
                 fileColor = 0, merged = 0, inRange = 0, active = 0;
        unsigned busiestCell = 0, fullCells = 0, droppedFromCells = 0, poolDropped = 0; // the grid
        unsigned tableLights = 0, suppressedModels = 0, attachFallbacks = 0; // DoodadLightAssignment
        unsigned attachedModels = 0, attachedAtRest = 0;                     // models hanging on another, of those not animated
        unsigned indoor = 0, indoorTests = 0;                                // drawn lights inside an indoor WMO group; locate calls this frame
        unsigned bakeWanted = 0, bakeDone = 0, bakeRays = 0, bakeCached = 0; // drawn lights asking for occlusion, of those ready; rays this frame; cache entries
        double   bakeMs = 0.0;                                                // time spent casting this frame
        double   scanMs = 0.0;
    };

    /// Every model the client's M2 scene holds near a point: map doodads, WMO doodads, game objects
    /// (campfires, portals), creatures. The scene's model list (head at scene +0x08, next at model
    /// +0x0C; CM2Model_AttachToScene 0x834540 links every new model in), not culled by the camera.
    /// Models attached to another (items in a character's hands, on its back, a mount's rider gear)
    /// aren't roots of the scene, so they are reached through their parent's attached-child chain
    /// (instance +0x58 first child, +0x60 next sibling; core M2.hpp).
    struct SceneModel
    {
        const void*    model = nullptr;   // the model instance (CM2Model)
        const uint8_t* header = nullptr;  // its parsed M2 header (null while loading)
        const char*    path = "";         // its file path as the client has it
        float          world[16] = {};    // its world matrix (roots: instance +0xB4; attached: AttachedWorld)
        float          distance = 0.0f;   // from the point
        const void*    parent = nullptr;  // attached models: the model it hangs on
        bool           animated = true;   // attached models: the world matrix follows its animation (else at the parent's rest pose)
    };
    /// `out` gets those within `range`, nearest first; returns how many the scene holds in all.
    unsigned SceneModels(const float center[3], float range, std::vector<SceneModel>& out);

    /// An attached model's world matrix. While it animated in the last couple of frames: its own
    /// model -> view root (instance +0xF4, the matrix its bones are built from) taken back to world
    /// space through the scene view NoteSceneView last saw, so it follows the hand. Otherwise (culled):
    /// its parent's world matrix moved to the attachment point it hangs on, at rest. `animated` says
    /// which. False when neither works.
    bool AttachedWorld(const void* instance, float out[16], bool& animated);

    /// A model instance's file path as the client has it ("" while loading or unreadable).
    const char* ModelPath(const void* instance);

    /// The scene view and camera (gfx::SceneMatrices, camera-relative world -> view), captured while
    /// the world draws (TerrainLights), for AttachedWorld.
    void NoteSceneView(const float view[16], const float camera[3]);

    /// Where an assignment row puts its light on a model, at rest, in the model's space (before the
    /// doodad's world matrix). False = the attachment wasn't found (then the origin + offset).
    /// `header` is the model's parsed M2 header.
    bool AttachmentPosition(const void* header, uint32_t attachType, int32_t attachIndex, const float offset[3], float out[3]);
    Stats GetStats();
}
