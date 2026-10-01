#include "WorldQuery.hpp"

#include "game/World.hpp"
#include "offsets/game/Weather.hpp"
#include "offsets/game/World.hpp"

namespace wxl_livingazeroth::world
{
    namespace
    {
        namespace gw  = wxl::game::world;
        namespace wo  = wxl::offsets::game::world;
        namespace wth = wxl::offsets::game::weather;

        // --- client landmarks not (yet) in the SDK. Verified in XWorkbench 2026-09-30. ------------
        // Every object keeps a world-location record at +0xB8: its position (+0x6C) and current WMO
        // group. The area resolver and the outdoor verdict both take that record, and the client's
        // own callers pass exactly `[object + 0xB8]`.
        constexpr uintptr_t kObjectLocationField = 0xB8;

        // A unit's current TerrainType ID (init -1 by the unit constructor 0x73F660, set from the
        // location record at unit setup; read by footstep/death-thud sounds, e.g. 0x746610).
        constexpr uintptr_t kUnitTerrainTypeField = 0xA40;

        // CMap::QueryAreaId(location, &areaId) -> non-zero on success. WMO area first (so interiors
        // resolve to their own areas), terrain area otherwise. __cdecl.
        using QueryAreaIdFn = int(__cdecl*)(void* location, uint32_t* outAreaId);
        // The outdoor verdict (1 = outdoors) that gates fog, sky and outdoor lighting: WMOAreaTable
        // flags inside a map object, AreaTable flags on terrain. __cdecl.
        using OutdoorsFn = int(__cdecl*)(void* location);

        // AreaTable's in-memory storage (read inline by the outdoor verdict at 0x77FC87..0x77FCA4).
        // Records keep the on-disk column layout: ID +0x00, ContinentID +0x04, ParentAreaID +0x08,
        // AreaBit +0x0C, Flags +0x10. The +0x10 flags read is confirmed by that code; the parent at
        // +0x08 follows from the same layout (verify via the debug panel's zone chain).
        constexpr uintptr_t kAreaMaxId   = 0x00AD3140;
        constexpr uintptr_t kAreaMinId   = 0x00AD3144;
        constexpr uintptr_t kAreaIdTable = 0x00AD3154;
        constexpr size_t    kAreaParentField = 0x08;

        // GetZoneText / GetSubZoneText return these char* globals (disassembly of 0x515570/0x5155D0).
        constexpr uintptr_t kZoneText    = 0x00BD0788;
        constexpr uintptr_t kSubZoneText = 0x00BD0784;

        Snapshot g_snap;

        const uint8_t* AreaRecord(uint32_t id)
        {
            const int32_t minId = *reinterpret_cast<const int32_t*>(kAreaMinId);
            const int32_t maxId = *reinterpret_cast<const int32_t*>(kAreaMaxId);
            const auto* table   = *reinterpret_cast<const uint8_t* const* const*>(kAreaIdTable);
            if (!table || static_cast<int32_t>(id) < minId || static_cast<int32_t>(id) > maxId) return nullptr;
            return table[static_cast<int32_t>(id) - minId];
        }

        const char* SafeText(uintptr_t global)
        {
            const char* s = *reinterpret_cast<const char* const*>(global);
            return s ? s : "";
        }

        void ReadWeather(Snapshot& s)
        {
            const auto* weather = *reinterpret_cast<const uint8_t* const*>(wth::kWorldWeather);
            if (!weather)
            {
                s.weatherRaw = s.weatherIntensity = 0.0f;
                s.weatherType = 0;
                return;
            }
            s.weatherRaw = *reinterpret_cast<const float*>(weather + wth::kIntensity);
            // The engine's own dead zone: nothing reacts below the knee, and knee..1 maps onto 0..1.
            const float knee = wth::kIntensityKnee;
            s.weatherIntensity = s.weatherRaw <= knee ? 0.0f
                               : (s.weatherRaw >= 1.0f ? 1.0f : (s.weatherRaw - knee) / (1.0f - knee));

            // Same order as the engine's type query: rain, snow, sand.
            if (*reinterpret_cast<const uint32_t*>(weather + wth::kRainCount))      s.weatherType = 1;
            else if (*reinterpret_cast<const uint32_t*>(weather + wth::kSnowCount)) s.weatherType = 2;
            else if (*reinterpret_cast<const uint32_t*>(weather + wth::kSandCount)) s.weatherType = 3;
            else                                                                     s.weatherType = 0;
        }
    }

    const Snapshot& Refresh()
    {
        Snapshot s;
        s.mapId = gw::CurrentMapId();

        const unsigned long long guid = gw::ActivePlayerGuid();
        void* player = guid ? gw::ResolveObject(guid, gw::kTypeMaskPlayer) : nullptr;
        if (player)
        {
            s.inWorld = true;
            gw::Position(player, s.playerPos);
            s.playerTerrainType = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(player) + kUnitTerrainTypeField);

            void* location = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(player) + kObjectLocationField);
            if (location)
            {
                uint32_t area = 0;
                if (reinterpret_cast<QueryAreaIdFn>(wo::kMapAreaIdQuery)(location, &area) && area)
                    s.areaCount = AreaChain(area, s.areaChain, kMaxAreaChain);
                s.outdoors = reinterpret_cast<OutdoorsFn>(wo::kOutdoorsQuery)(location) != 0;
            }

            s.zoneText    = SafeText(kZoneText);
            s.subZoneText = SafeText(kSubZoneText);
        }

        ReadWeather(s);
        g_snap = s;
        return g_snap;
    }

    const Snapshot& Current() { return g_snap; }

    int AreaChain(uint32_t area, uint32_t* out, int max)
    {
        // Walk up the parents; the cap guards against a malformed (cyclic) table.
        int n = 0;
        while (area && n < max)
        {
            out[n++] = area;
            const uint8_t* rec = AreaRecord(area);
            if (!rec) break;
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(rec + kAreaParentField);
            if (parent == area) break;
            area = parent;
        }
        return n;
    }
}
