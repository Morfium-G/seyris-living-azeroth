#include "SurfaceCoverInternal.hpp"

#include "../env/Actors.hpp"

#include <cmath>
#include <vector>

namespace wxl_livingazeroth::cover::detail
{
    // Level 0's trench state, slot = (world cell mod kTex): how far pressed down (0..1) and the
    // rim's strength (0..1; its height is a share of the local cover depth). Both relax over time.
    std::vector<float> g_press(kTex * kTex, 0.0f);
    std::vector<float> g_rim(kTex * kTex, 0.0f);

    // Only cells with a trench change from frame to frame. They're kept in a list (relaxed, and
    // re-uploaded with a dirty rectangle) instead of walking all 147k cells twice per frame. The
    // rest of level 0 keeps its static cover and the trench fade-out factor from the last full
    // pass (a full pass runs whenever coverDirty is set: recentre, refill, settings).
    std::vector<int>     g_active;                            // slots with press or rim > 0
    std::vector<uint8_t> g_isActive(kTex * kTex, 0);
    std::vector<int>     g_changed;                           // slots to re-upload this frame
    bool                 g_changedPending = false;            // changes not uploaded yet
    std::vector<float>   g_static0(kTex * kTex, 0.0f);        // level 0 cover without trenches
    std::vector<float>   g_deformK(kTex * kTex, 0.0f);        // level 0 trench strength (fade-out)

    void MarkActive(int s)
    {
        if (g_isActive[s]) return;
        g_isActive[s] = 1;
        g_active.push_back(s);
        g_changed.push_back(s);
    }

    // Level 0's rim share and relax time per slot, from the rows of the cell's materials (blended
    // like everything else; the panel's multipliers apply on top). Per cell, not per player: a
    // cover whose prints only show through their rims (mud below the terrain) must keep its rims
    // when the player steps onto other ground.
    std::vector<float> g_rimCell(kTex * kTex, 0.3f);
    std::vector<float> g_relaxCell(kTex * kTex, 30.0f);

    unsigned g_stamps = 0;

    // A trench under one grounded unit: pressed flat inside r, a rim between r and 1.6 r.
    void Stamp(const float pos[3], float radius)
    {
        const Level& L = g_levels[0];
        float ground;
        if (!terrain::HeightAt(pos[0], pos[1], ground) || std::fabs(pos[2] - ground) > 0.35f) return; // airborne, swimming, on a WMO
        ++g_stamps;
        const float outer = radius * 1.6f, rimPeak = radius * 1.25f, rimHalf = radius * 0.35f;
        const int i0 = static_cast<int>(std::floor((pos[0] - outer) / L.cell)), i1 = static_cast<int>(std::ceil((pos[0] + outer) / L.cell));
        const int j0 = static_cast<int>(std::floor((pos[1] - outer) / L.cell)), j1 = static_cast<int>(std::ceil((pos[1] + outer) / L.cell));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
            {
                if (i < L.gridI || i >= L.gridI + kGridVerts || j < L.gridJ || j >= L.gridJ + kGridVerts) continue;
                const float dx = i * L.cell - pos[0], dy = j * L.cell - pos[1];
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d >= outer) continue;
                const int s = SlotOf(i, j);
                if (d < radius)
                {
                    const float t = d < radius * 0.7f ? 1.0f : (radius - d) / (radius * 0.3f);
                    if (t > g_press[s]) { g_press[s] = t; MarkActive(s); }
                }
                const float r = 1.0f - std::fabs(d - rimPeak) / rimHalf;
                if (r > g_rim[s]) { g_rim[s] = r; MarkActive(s); }
            }
    }

    void Simulate(float dt)
    {
        // Changes the last frame didn't upload (the cover wasn't drawn) would be lost: redo level 0
        // in full instead.
        if (g_changedPending) g_levels[0].coverDirty = true;
        g_changed.clear();

        for (size_t k = 0; k < g_active.size();)
        {
            const int s = g_active[k];
            const float seconds = g_relaxCell[s] * g_settings.relaxMul;
            const float relax = seconds > 0.1f ? dt / seconds : 1.0f;
            g_press[s] = g_press[s] > relax ? g_press[s] - relax : 0.0f;
            g_rim[s]   = g_rim[s] > relax ? g_rim[s] - relax : 0.0f;
            g_changed.push_back(s);
            if (g_press[s] > 0.0f || g_rim[s] > 0.0f) { ++k; continue; }
            g_isActive[s] = 0; // flat again: uploaded once more, then left alone
            g_active[k] = g_active.back();
            g_active.pop_back();
        }

        g_stamps = 0;
        if (!g_settings.stamp) return;
        // Sized like the client's own footprints (CreatureModelData, x the unit's scale): a frog
        // leaves a small trench, a tauren a wide one. Models without footprints (bats, birds and
        // other fliers that "walk" in the air) leave none.
        for (const actors::Actor& a : actors::Nearby())
        {
            if (!a.hasFootprint) continue;
            const float foot = a.footprintLength > a.footprintWidth ? a.footprintLength : a.footprintWidth;
            float r = (foot > 0.0f ? foot * kFootprintToTrench : (a.effectiveRadius > 0.0f ? a.effectiveRadius : 0.4f)) * g_settings.stampScale;
            if (r < 0.06f) r = 0.06f;
            if (r > 2.5f) r = 2.5f;
            Stamp(a.pos, r);
        }
        g_changedPending = !g_changed.empty();
    }
}
