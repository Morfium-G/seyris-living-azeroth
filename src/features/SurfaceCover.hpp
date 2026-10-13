// Deformable surface cover, first spike: a dense mesh around the player that sits ON TOP of the
// terrain (snow/sand/mud as a real volume), lifted per vertex by vertex texture fetch.
//
// Two toroidal R32F textures, one texel per mesh vertex (0.25 yd), indexed by world cell:
//   - base height: the drawn terrain surface (terrain::HeightAt), holes marked;
//   - cover depth: a test depth (everywhere, or only on TerrainTypes with the footprint flag),
//     pressed down by the player and nearby units into trenches with a rim, relaxing over time.
// Drawn at the end of the world scene against the scene depth, plain-lit, to judge the look, the
// seams with the terrain and the cost. See orchestration docs/r&d/immersion/deformable-surfaces.md.
#pragma once

#include "wxl/PluginApi.h"

#include "../env/WorldQuery.hpp"

namespace wxl_livingazeroth::cover
{
    void Install(const WXL_Api* api);

    /// Start-up settings (WarcraftXL.ini); the panel can change them at runtime.
    void Configure(bool enabled, int levels, bool trenches, float depthMultiplier, float fillBudgetMs);
    bool  Enabled();
    int   Levels();
    bool  Trenches();
    float DepthMultiplier();
    float FillBudgetMs();

    /// Loads SurfaceCover.cdbc (which ground gets cover, how deep) through the cdbc reader.
    void LoadTable(const void* cdbcApi);

    /// Once per frame on the main thread, after the world snapshot and the actors are refreshed.
    void Update(float dt, const world::Snapshot& snap);

    /// The top of the cover as drawn at a world position (base + depth, level 0 with its trenches,
    /// bilinear over the finest drawn level holding it). False where no cover is drawn: off, outside
    /// the grid, a hole, or no depth there (z is then the terrain the cover sits on, if known).
    bool TopAt(float x, float y, float& z);

    /// Before IDirect3DDevice9::Reset: releases what has to go first (its textures are managed).
    void OnDeviceLost();

    /// Last drawn frame: the most point lights any patch had, and how many patches had more than
    /// lights::kMaxLights (the nearest to the camera kept).
    void PatchLightStats(unsigned& most, unsigned& full);

    /// The stock fog the terrain stage left on the device, as last read there (only while the cover
    /// draws in that pass with scene lighting on): VS c12 = (x, y, exponent, 0) with fog =
    /// min(pow(max(viewZ * x + y, 0), exponent), 1), and PS c2 = the fog colour. False if never read.
    bool SeenSceneFog(float vsFog[4], float psFogColor[4]);
}
