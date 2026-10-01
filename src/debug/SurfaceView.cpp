#include "SurfaceView.hpp"

#include "ShaderDump.hpp"
#include "../env/TerrainHeight.hpp"
#include "../env/WorldQuery.hpp"
#include "../features/GrassInstanced.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: surfaces";

        // TerrainType storage (WowClientDB at 0xAD4C34): min/max ID and the ID index; a row's
        // Desc string pointer is at +0x04, Flags at +0x14 (0x1 = footprints).
        constexpr uintptr_t kTerrainMinId = 0x00AD4C44, kTerrainMaxId = 0x00AD4C40, kTerrainIndex = 0x00AD4C54;

        const WXL_Api* g_api = nullptr;

        const uint8_t* TerrainRow(int id)
        {
            const int32_t minId = *reinterpret_cast<const int32_t*>(kTerrainMinId);
            const int32_t maxId = *reinterpret_cast<const int32_t*>(kTerrainMaxId);
            const auto* index = *reinterpret_cast<const uint8_t* const* const*>(kTerrainIndex);
            if (!index || id < minId || id > maxId) return nullptr;
            return index[id - minId];
        }

        void Describe(int id, char* out, size_t size)
        {
            const uint8_t* row = id >= 0 ? TerrainRow(id) : nullptr;
            if (!row) { std::snprintf(out, size, "%d (none)", id); return; }
            const char* name = *reinterpret_cast<const char* const*>(row + 0x04);
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(row + 0x14);
            std::snprintf(out, size, "%d %s%s", id, name ? name : "?", (flags & 1) ? " [footprints]" : "");
        }

        void Details(void* /*user*/)
        {
            char line[256], a[96], b[96];
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) { g_api->UiText("not in world"); return; }

            Describe(s.playerTerrainType, a, sizeof(a));
            std::snprintf(line, sizeof(line), "client says the player stands on: TerrainType %s", a);
            g_api->UiText(line);

            // The client's own terrain query (0x7A0530) at the player: matches the line above on
            // terrain, differs on WMOs (those come from the WMO material instead).
            int queried = -1;
            if (terrain::TerrainTypeAt(s.playerPos[0], s.playerPos[1], queried)) Describe(queried, a, sizeof(a));
            else std::snprintf(a, sizeof(a), "(no terrain here: hole or not loaded)");
            std::snprintf(line, sizeof(line), "client terrain query at the player: TerrainType %s", a);
            g_api->UiText(line);

            // Terrain height from the chunk's MCVT under the player vs the player's own z: on open
            // ground these should agree to a few centimetres.
            float ground = 0.0f;
            if (terrain::HeightAt(s.playerPos[0], s.playerPos[1], ground))
                std::snprintf(line, sizeof(line), "terrain height (MCVT) %.3f, player z %.3f, player - terrain %+.3f yd",
                              ground, s.playerPos[2], s.playerPos[2] - ground);
            else
                std::snprintf(line, sizeof(line), "terrain height (MCVT): none here (hole or chunk not loaded)");
            g_api->UiText(line);

            const grassinst::TerrainProbe& p = grassinst::Probe();
            if (!p.valid)
            {
                g_api->UiTextWrapped("terrain under the player: not seen (needs instanced grass on and ground effects in this chunk)");
                return;
            }
            std::snprintf(line, sizeof(line), "chunk %08X, player at local (%.1f, %.1f), cell (%d, %d)",
                          p.chunk, p.local[0], p.local[1], p.cellA[0], p.cellA[1]);
            g_api->UiText(line);

            bool anyMatch = false;
            for (unsigned i = 0; i < p.layers; ++i)
            {
                Describe(p.effectTerrain[i], a, sizeof(a));
                const bool match = p.effectTerrain[i] >= 0 && p.effectTerrain[i] == s.playerTerrainType;
                anyMatch |= match;
                std::snprintf(line, sizeof(line), "  layer %u: ground effect %u -> TerrainType %s%s", i, p.effect[i], a,
                              match ? "   <== same as the client's" : "");
                g_api->UiText(line);
            }

            const int ta = (p.dominantA >= 0 && p.dominantA < static_cast<int>(p.layers)) ? p.effectTerrain[p.dominantA] : -1;
            const int tb = (p.dominantB >= 0 && p.dominantB < static_cast<int>(p.layers)) ? p.effectTerrain[p.dominantB] : -1;
            Describe(ta, a, sizeof(a));
            Describe(tb, b, sizeof(b));
            std::snprintf(line, sizeof(line), "cell's dominant layer: A = layer %d -> %s   B (axes swapped) = layer %d -> %s",
                          p.dominantA, a, p.dominantB, b);
            g_api->UiTextWrapped(line);

            const char* verdict =
                ta >= 0 && ta == s.playerTerrainType ? "MATCH (A): the client's value is the cell's dominant layer -> ground effect -> last column" :
                tb >= 0 && tb == s.playerTerrainType ? "MATCH (B): same, with the axes the other way round" :
                anyMatch                             ? "PARTIAL: one of the chunk's layers matches, but not the cell's dominant one" :
                                                       "NO MATCH: the client's value doesn't come from these layers (or you're on a WMO/object)";
            g_api->UiTextWrapped(verdict);
        }

        void __cdecl Panel(void* user)
        {
            Details(user);
            g_api->UiSeparator();
            // Research: the terrain shaders, for the surface-cover and terrain-relief patches.
            if (g_api->UiButton("Dump terrain shaders (Logs\\living-azeroth)"))
                DumpTerrainShaders(g_api);
        }
    }

    void RegisterSurfacePanel(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }
}
