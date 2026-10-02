#include "SurfaceCoverTable.hpp"

#include "../env/WorldQuery.hpp"
#include "../wxl_seyris/CdbcApi.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace wxl_livingazeroth::covertable
{
    namespace
    {
        constexpr const char* kFile = "DBFilesClient\\SurfaceCover.cdbc";
        constexpr WXL_SeyrisCdbcField kFields[] = {
            {"ID",             0, WXL_CDBC_FIELD_VALUE},
            {"ScopeType",      1, WXL_CDBC_FIELD_VALUE},
            {"ScopeID",        2, WXL_CDBC_FIELD_VALUE},
            {"GroundEffectID", 3, WXL_CDBC_FIELD_VALUE},
            {"TerrainType",    4, WXL_CDBC_FIELD_VALUE},
            {"Depth",          5, WXL_CDBC_FIELD_VALUE},
            {"Rim",            6, WXL_CDBC_FIELD_VALUE},
            {"RelaxSeconds",   7, WXL_CDBC_FIELD_VALUE},
            {"Flags",          8, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kDef = { "SurfaceCover", kFile, kFields, 9 };

        enum Scope : uint32_t { kGlobal = 0, kMap = 1, kArea = 2 };
        constexpr int kFieldCount = 3; // depth, rim, relax

        struct Row
        {
            uint32_t effect = 0;   // 0 = any ground effect
            int32_t  terrain = -1; // -1 = any TerrainType (TerrainType 0 is Dirt, so 0 can't mean "any")
            float    value[kFieldCount] = { -1.0f, -1.0f, -1.0f };
        };

        // Rows by place: (scope type, scope id) -> rows there.
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Row>> g_rows;
        unsigned    g_rowCount = 0;
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;

        std::map<std::tuple<uint32_t, int, uint32_t, int>, Values> g_cache;

        float BitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }

        // The row of one "what" level at one place, if any. Levels: 0 ground effect, 1 TerrainType,
        // 2 everything.
        const Row* AtLevel(const std::vector<Row>& rows, int level, uint32_t effect, int terrain)
        {
            for (const Row& r : rows)
            {
                switch (level)
                {
                    case 0: if (r.effect && effect && r.effect == effect) return &r; break;
                    case 1: if (!r.effect && r.terrain >= 0 && r.terrain == terrain) return &r; break;
                    case 2: if (!r.effect && r.terrain < 0) return &r; break;
                }
            }
            return nullptr;
        }
    }

    void Load(const void* cdbcApi)
    {
        g_rows.clear();
        g_cache.clear();
        g_rowCount = 0;
        ++g_generation;

        const auto* cdbc = static_cast<const WXL_SeyrisCdbcApi*>(cdbcApi);
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available: no cover rows"; return; }

        char err[256] = {};
        void* table = cdbc->Load(&kDef, err, sizeof(err));
        if (!table) { g_status = std::string("no rows (") + err + ")"; return; }

        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* rec = cdbc->RowAt(table, i);
            if (!rec) continue;
            Row r;
            const uint32_t scope = cdbc->Value(table, rec, "ScopeType", 0);
            const uint32_t id = scope == kGlobal ? 0 : cdbc->Value(table, rec, "ScopeID", 0);
            r.effect = cdbc->Value(table, rec, "GroundEffectID", 0);
            r.terrain = static_cast<int32_t>(cdbc->Value(table, rec, "TerrainType", 0));
            r.value[0] = BitsToFloat(cdbc->Value(table, rec, "Depth", 0));
            r.value[1] = BitsToFloat(cdbc->Value(table, rec, "Rim", 0));
            r.value[2] = BitsToFloat(cdbc->Value(table, rec, "RelaxSeconds", 0));
            g_rows[{ scope, id }].push_back(r);
            ++g_rowCount;
        }
        cdbc->Release(table);

        char line[96];
        std::snprintf(line, sizeof(line), "%u row(s) loaded", g_rowCount);
        g_status = line;
    }

    unsigned    RowCount()   { return g_rowCount; }
    const char* Status()     { return g_status.c_str(); }
    uint32_t    Generation() { return g_generation; }

    Values Resolve(uint32_t areaId, int mapId, uint32_t groundEffectId, int terrainType)
    {
        const auto key = std::make_tuple(areaId, mapId, groundEffectId, terrainType);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;

        // Places, most specific first: the cell's area chain, its map, global.
        std::vector<std::pair<uint32_t, uint32_t>> places;
        uint32_t chain[world::kMaxAreaChain];
        const int n = world::AreaChain(areaId, chain, world::kMaxAreaChain);
        for (int i = 0; i < n; ++i) places.push_back({ kArea, chain[i] });
        if (mapId >= 0) places.push_back({ kMap, static_cast<uint32_t>(mapId) });
        places.push_back({ kGlobal, 0 });

        float value[kFieldCount] = { -1.0f, -1.0f, -1.0f };
        for (const auto& place : places)
        {
            auto rows = g_rows.find(place);
            if (rows == g_rows.end()) continue;
            for (int level = 0; level < 3; ++level)
                if (const Row* r = AtLevel(rows->second, level, groundEffectId, terrainType))
                    for (int f = 0; f < kFieldCount; ++f)
                        if (value[f] < 0.0f && r->value[f] >= 0.0f) value[f] = r->value[f];
        }

        Values v;
        if (value[0] >= 0.0f) v.depth = value[0];
        v.rim = value[1];
        v.relaxSeconds = value[2];
        if (g_cache.size() > 20000) g_cache.clear();
        g_cache[key] = v;
        return v;
    }
}
