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

    /// Last drawn frame: the most point lights any patch had, and how many patches had more than
    /// lights::kMaxLights (the nearest to the camera kept).
    void PatchLightStats(unsigned& most, unsigned& full);
}
