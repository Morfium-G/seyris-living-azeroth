#include "SurfaceCover.hpp"
#include "SurfaceCoverInternal.hpp"

#include "../debug/Pages.hpp"
#include "../env/Fields.hpp"
#include "GrassPerf.hpp"

#include <cstring>

namespace wxl_livingazeroth::cover::detail
{
    const WXL_Api* g_api = nullptr;
    Settings       g_settings;

    Level g_levels[kMaxLevels];

    unsigned             g_frame = 0;
    int                  g_snowCursor = 0;                    // the level the next snow re-fold starts looking at

    // What the table sees where the player is (for the panel, so rows can be authored on the spot).
    terrain::Surface   g_hereSurface;
    covertable::Values g_hereValues;
    std::string        g_hereTexture;
    bool               g_hereValid = false;
    float              g_hereMccv[3] = {}, g_hereLiquid = 0.0f, g_hereLiquidDepth = 0.0f;
    bool               g_hereMccvValid = false, g_hereLiquidValid = false;

    const void* g_cdbcApi = nullptr;
    uint32_t    g_tableGeneration = 0;

    int   g_lastCoverage = -1, g_lastMaterialSource = -1, g_lastAlphaSwap = -1;
    int   g_lastMap = -1;
    float g_lastDepthMul = -1.0f, g_lastDriftMul = -1.0f, g_lastBreakupMul = -1.0f;

    // Under the player, for the panel: the layer weights both ways round and the client's own
    // dominant layer (the check that the alpha maps are read the right way round).
    terrain::LayerWeights g_hereLayers, g_hereLayersSwapped;
    std::string g_hereLayerTexture[4]; // own copies for the panel (the cache can drop the originals)
    bool g_hereLayersValid = false;
    bool g_wasInWorld = false;
    int g_mapNow = -1;

    double g_simMs = 0;
}

namespace wxl_livingazeroth::cover
{
    using namespace detail;

    void Configure(bool enabled, int levels, bool trenches, float depthMultiplier, float fillBudgetMs)
    {
        g_settings.enabled = enabled ? 1 : 0;
        g_settings.levels = levels < 1 ? 1 : (levels > kMaxLevels ? kMaxLevels : levels);
        g_settings.stamp = trenches ? 1 : 0;
        g_settings.depthMul = depthMultiplier < 0.0f ? 0.0f : depthMultiplier;
        g_settings.fillBudgetMs = fillBudgetMs < 0.1f ? 0.1f : fillBudgetMs;
    }

    bool Enabled() { return g_settings.enabled != 0; }
    int  Levels() { return g_settings.levels; }
    bool Trenches() { return g_settings.stamp != 0; }
    float DepthMultiplier() { return g_settings.depthMul; }
    float FillBudgetMs() { return g_settings.fillBudgetMs; }

    void LoadTable(const void* cdbcApi)
    {
        g_cdbcApi = cdbcApi;
        covertable::Load(cdbcApi);
    }

    void Install(const WXL_Api* api)
    {
        g_api = api;
        for (int k = 0; k < kMaxLevels; ++k) g_levels[k].cell = kCell0 * static_cast<float>(1 << k);
        detail::InstallDraw(api);
        pages::AddPanel(kPanelTitle, &Panel);
    }

    void PatchLightStats(unsigned& most, unsigned& full) { most = g_patchLightsMost; full = g_patchLightsFull; }

    bool SeenSceneFog(float vsFog[4], float psFogColor[4])
    {
        std::memcpy(vsFog, g_seenFog, sizeof(g_seenFog));
        std::memcpy(psFogColor, g_seenFogColor, sizeof(g_seenFogColor));
        return g_seenScene;
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        // Leaving the world (logout, loading screens without a player) unloads every tile: drop
        // everything that refers to them, and start over on the way back.
        if (!snap.inWorld)
        {
            if (g_wasInWorld)
            {
                terrain::ClearLayerCache();
                ClearLayerValues();
                ClearCellCache();
                g_hereLayersValid = g_hereValid = false;
                for (Level& L : g_levels) { L.haveGrid = false; L.jobActive = false; }
                g_lastMap = -1;
            }
            g_wasInWorld = false;
            return;
        }
        g_wasInWorld = true;
        if (!g_settings.enabled) return;
        if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
        const double t0 = grassperf::Now();

        g_mapNow = snap.mapId;
        const bool newMap = g_lastMap != snap.mapId;
        const bool refill = newMap || g_lastCoverage != g_settings.coverage || g_tableGeneration != covertable::Generation() ||
                            g_lastMaterialSource != g_settings.materialSource || g_lastAlphaSwap != g_settings.alphaSwap;
        if (refill) { ClearCellCache(); ClearLayerValues(); }
        if (newMap) terrain::ClearLayerCache();
        g_lastMaterialSource = g_settings.materialSource;
        g_lastAlphaSwap = g_settings.alphaSwap;
        g_lastCoverage = g_settings.coverage;
        g_lastMap = snap.mapId;
        g_tableGeneration = covertable::Generation();
        if (g_lastDepthMul != g_settings.depthMul || g_lastDriftMul != g_settings.driftMul)
        {
            g_lastDepthMul = g_settings.depthMul; g_lastDriftMul = g_settings.driftMul;
            for (Level& L : g_levels) L.coverDirty = true;
        }
        if (g_lastBreakupMul != g_settings.breakupMul)
        {
            g_lastBreakupMul = g_settings.breakupMul;
            for (Level& L : g_levels) { L.coverDirty = true; L.boostRows = kFilled; } // the warp is sampled
        }

        ++g_frame;
        const int levels = ActiveLevels();
        for (int k = 0; k < levels; ++k)
        {
            Level& L = g_levels[k];
            // A refill or a new map replaces whatever job was running; a recentre waits for it.
            if (refill) PlanFill(L, snap.playerPos, newMap || !L.haveGrid ? Plan::NewPlace : Plan::RefillInPlace);
            else if (!L.jobActive) PlanFill(L, snap.playerPos, Plan::Recentre);
            if (L.jobActive) continue; // re-sampling and hole counting wait for the fill
            // Chunks stream in after the grid saw them: re-sample a few rows per frame, keeping trenches.
            // Once nothing is missing one row is enough, except for a pass at full speed after a
            // setting that's applied while sampling changed.
            const int fast = k == 0 ? 8 : 4;
            const bool idle = !L.holes && L.boostRows <= 0;
            const int rows = !idle ? fast : ((g_frame % 8) == static_cast<unsigned>(k) ? 1 : 0);
            if (L.boostRows > 0) L.boostRows -= rows;
            for (int r = 0; r < rows; ++r)
            {
                const int row = L.gridJ - kApron + L.refreshRow;
                SampleRect(L, k == 0, L.gridI - kApron, L.gridI - kApron + kFilled - 1, row, row, false);
                L.refreshRow = (L.refreshRow + 1) % kFilled;
            }
            if (rows && L.refreshRow < rows) // once per pass: count what's still missing
            {
                unsigned holes = 0;
                for (int j = 0; j < kGridVerts; ++j)
                    for (int i = 0; i < kGridVerts; ++i) holes += L.base[SlotOf(L.gridI + i, L.gridJ + j)] == kHole;
                L.holes = holes;
            }
        }
        // A level switched off is refilled from scratch when it comes back.
        for (int k = levels; k < kMaxLevels; ++k) { g_levels[k].haveGrid = false; g_levels[k].jobActive = false; }

        // Fills within the budget. Levels with nothing on screen go first, coarsest first (a coarse
        // level covers the most ground for the same work, so there's always cover under the player);
        // the rest of the budget is shared evenly by the visible levels' jobs, so fast travel can't
        // starve any of them.
        const double end = grassperf::Now() + (g_settings.fillBudgetMs > 0.1f ? g_settings.fillBudgetMs : 0.1f);
        int order[kMaxLevels], jobs = 0;
        for (int k = levels - 1; k >= 0; --k) if (g_levels[k].jobActive && g_levels[k].jobHidden) order[jobs++] = k;
        for (int k = 0; k < levels; ++k)      if (g_levels[k].jobActive && !g_levels[k].jobHidden) order[jobs++] = k;
        for (int n = 0; n < jobs; ++n)
        {
            const double now = grassperf::Now();
            if (now >= end) break;
            const int k = order[n];
            const bool hidden = g_levels[k].jobHidden;
            const double deadline = hidden ? end : now + (end - now) / (jobs - n);
            WorkFill(g_levels[k], k == 0, deadline);
        }

        // New snow values (fallen snow, painted snow melting): one level re-folds its depth per frame.
        // While there's no snow anywhere and the last fold had none either, there's nothing to redo;
        // nor in a level without a single cell snow can change (its fold would come out the same:
        // it re-folded every frame in zones without cover, ~2.8 ms CPU + GPU for nothing).
        {
            const uint32_t version = fields::SnowVersion();
            const bool active = fields::SnowActive();
            for (int n = 0; n < levels; ++n)
            {
                const int k = (g_snowCursor + n) % levels;
                Level& L = g_levels[k];
                if (!L.haveGrid || L.snowVersion == version) continue;
                if (L.snowCells <= 0 && L.coverCells <= 0) { L.snowVersion = version; continue; } // nothing snow can change here
                L.snowVersion = version;
                if (active || L.snowFolded) L.coverDirty = true;
                L.snowFolded = active;
                g_snowCursor = k + 1;
                break;
            }
        }

        // The table's values where the player is, for the panel's "here" lines.
        covertable::Values here;
        g_hereValid = terrain::SurfaceAt(snap.playerPos[0], snap.playerPos[1], g_hereSurface);
        if (g_hereValid)
        {
            g_hereTexture = g_hereSurface.texture ? g_hereSurface.texture : "";
            if (static_cast<Coverage>(g_settings.coverage) == Coverage::Table)
                here = covertable::Resolve(g_hereSurface.area, snap.mapId, g_hereSurface.texture, g_hereSurface.groundEffect, g_hereSurface.terrainType);
        }
        g_hereValues = here;
        float terrainZ;
        g_hereMccvValid = terrain::VertexColorAt(snap.playerPos[0], snap.playerPos[1], g_hereMccv);
        g_hereLiquidValid = terrain::LiquidHeightAt(snap.playerPos[0], snap.playerPos[1], g_hereLiquid) &&
                            terrain::HeightAt(snap.playerPos[0], snap.playerPos[1], terrainZ);
        if (g_hereLiquidValid) g_hereLiquidDepth = g_hereLiquid - terrainZ;
        g_hereLayersValid = terrain::LayerWeightsAt(snap.playerPos[0], snap.playerPos[1], g_hereLayers, false) &&
                            terrain::LayerWeightsAt(snap.playerPos[0], snap.playerPos[1], g_hereLayersSwapped, true);
        for (int l = 0; l < 4; ++l)
        {
            g_hereLayerTexture[l] = g_hereLayersValid && l < g_hereLayers.layers && g_hereLayers.surface[l].texture ? g_hereLayers.surface[l].texture : "";
            g_hereLayers.surface[l].texture = g_hereLayersSwapped.surface[l].texture = nullptr;
        }

        Simulate(dt);
        g_simMs = grassperf::Now() - t0;
    }
}
