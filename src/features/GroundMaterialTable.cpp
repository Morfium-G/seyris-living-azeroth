#include "GroundMaterialTable.hpp"

#include "../env/WorldQuery.hpp"
#include "../wxl_seyris/CdbcApi.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::materials
{
    namespace
    {
        constexpr WXL_SeyrisCdbcField kMaterialFields[] = {
            {"ID",           0, WXL_CDBC_FIELD_VALUE},
            {"Name",         1, WXL_CDBC_FIELD_STRING},
            {"Parent",       2, WXL_CDBC_FIELD_VALUE},
            {"RestMoisture", 3, WXL_CDBC_FIELD_VALUE},
            {"Stiffness",    4, WXL_CDBC_FIELD_VALUE},
            {"Flags",        5, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kMaterialDef = { "GroundMaterial", "DBFilesClient\\GroundMaterial.cdbc", kMaterialFields, 6 };

        constexpr WXL_SeyrisCdbcField kSelectorFields[] = {
            {"ID",             0, WXL_CDBC_FIELD_VALUE},
            {"ScopeType",      1, WXL_CDBC_FIELD_VALUE},
            {"ScopeID",        2, WXL_CDBC_FIELD_VALUE},
            {"TexturePath",    3, WXL_CDBC_FIELD_STRING},
            {"GroundEffectID", 4, WXL_CDBC_FIELD_VALUE},
            {"TerrainType",    5, WXL_CDBC_FIELD_VALUE},
            {"LiquidType",     6, WXL_CDBC_FIELD_VALUE}, // reserved for liquid materials: rows with one aren't terrain rows
            {"MaterialID",     7, WXL_CDBC_FIELD_VALUE},
            {"Flags",          8, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kSelectorDef = { "GroundMaterialSelector", "DBFilesClient\\GroundMaterialSelector.cdbc", kSelectorFields, 9 };

        enum Scope : uint32_t { kGlobal = 0, kMap = 1, kArea = 2 };

        struct Material
        {
            std::string name;
            uint32_t    parent = 0;
            float       restMoisture = -1.0f, stiffness = -1.0f;
            uint32_t    flags = 0;
        };
        std::unordered_map<uint32_t, Material> g_materials;

        struct Selector
        {
            std::string texture;      // normalized; empty = any texture
            uint32_t    effect = 0;   // 0 = any ground effect
            int32_t     terrain = -1; // -1 = any TerrainType (0 is Dirt)
            uint32_t    material = 0;
        };
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Selector>> g_selectors; // (scope type, scope id) -> rows
        unsigned    g_selectorCount = 0;
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;

        struct Key
        {
            uint32_t area; int map; uint32_t effect; int terrain; std::string texture;
            bool operator==(const Key& o) const
            { return area == o.area && map == o.map && effect == o.effect && terrain == o.terrain && texture == o.texture; }
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const
            {
                size_t h = std::hash<std::string>()(k.texture);
                h ^= (static_cast<size_t>(k.area) * 0x9E3779B1u) ^ (static_cast<size_t>(k.map) << 7) ^ (static_cast<size_t>(k.effect) << 13) ^ static_cast<size_t>(k.terrain + 1);
                return h;
            }
        };
        std::unordered_map<Key, uint32_t, KeyHash> g_cache;

        float BitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }

        // Lower case, forward slashes to backslashes: texture paths compare the way the client's
        // archive lookups do.
        std::string Normalize(const char* path)
        {
            std::string s = path ? path : "";
            for (char& c : s)
            {
                if (c == '/') c = '\\';
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        }

        // The row of one "what" level at one place, if any. Levels: 0 texture, 1 ground effect,
        // 2 TerrainType, 3 everything.
        const Selector* AtLevel(const std::vector<Selector>& rows, int level, const std::string& texture, uint32_t effect, int terrain)
        {
            for (const Selector& r : rows)
            {
                switch (level)
                {
                    case 0: if (!r.texture.empty() && r.texture == texture) return &r; break;
                    case 1: if (r.texture.empty() && r.effect && effect && r.effect == effect) return &r; break;
                    case 2: if (r.texture.empty() && !r.effect && r.terrain >= 0 && r.terrain == terrain) return &r; break;
                    case 3: if (r.texture.empty() && !r.effect && r.terrain < 0) return &r; break;
                }
            }
            return nullptr;
        }

        const Material* Find(uint32_t id)
        {
            auto it = g_materials.find(id);
            return it == g_materials.end() ? nullptr : &it->second;
        }
    }

    void Load(const void* cdbcApi)
    {
        g_materials.clear();
        g_selectors.clear();
        g_cache.clear();
        g_selectorCount = 0;
        ++g_generation;

        const auto* cdbc = static_cast<const WXL_SeyrisCdbcApi*>(cdbcApi);
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available: no materials"; return; }

        char err[256] = {};
        std::string materialStatus, selectorStatus;
        if (void* table = cdbc->Load(&kMaterialDef, err, sizeof(err)))
        {
            const uint32_t count = cdbc->RowCount(table);
            for (uint32_t i = 0; i < count; ++i)
            {
                const void* rec = cdbc->RowAt(table, i);
                if (!rec) continue;
                Material m;
                m.name = cdbc->GetString(table, rec, "Name");
                m.parent = cdbc->Value(table, rec, "Parent", 0);
                m.restMoisture = BitsToFloat(cdbc->Value(table, rec, "RestMoisture", 0));
                m.stiffness = BitsToFloat(cdbc->Value(table, rec, "Stiffness", 0));
                m.flags = cdbc->Value(table, rec, "Flags", 0);
                if (m.parent == 0xFFFFFFFFu) m.parent = 0; // -1 written for "none" works too
                g_materials[cdbc->Value(table, rec, "ID", 0)] = std::move(m);
            }
            cdbc->Release(table);
        }
        else materialStatus = std::string(" (GroundMaterial: ") + err + ")";

        err[0] = 0;
        if (void* table = cdbc->Load(&kSelectorDef, err, sizeof(err)))
        {
            const uint32_t count = cdbc->RowCount(table);
            for (uint32_t i = 0; i < count; ++i)
            {
                const void* rec = cdbc->RowAt(table, i);
                if (!rec) continue;
                if (cdbc->Value(table, rec, "LiquidType", 0)) continue; // liquid materials: not used yet
                Selector s;
                const uint32_t scope = cdbc->Value(table, rec, "ScopeType", 0);
                const uint32_t id = scope == kGlobal ? 0 : cdbc->Value(table, rec, "ScopeID", 0);
                s.texture = Normalize(cdbc->GetString(table, rec, "TexturePath"));
                s.effect = cdbc->Value(table, rec, "GroundEffectID", 0);
                s.terrain = static_cast<int32_t>(cdbc->Value(table, rec, "TerrainType", 0));
                s.material = cdbc->Value(table, rec, "MaterialID", 0);
                g_selectors[{ scope, id }].push_back(s);
                ++g_selectorCount;
            }
            cdbc->Release(table);
        }
        else selectorStatus = std::string(" (GroundMaterialSelector: ") + err + ")";

        char line[160];
        std::snprintf(line, sizeof(line), "%u material(s), %u selector(s)", static_cast<unsigned>(g_materials.size()), g_selectorCount);
        g_status = line + materialStatus + selectorStatus;
    }

    uint32_t Select(uint32_t areaId, int mapId, const char* texturePath, uint32_t groundEffectId, int terrainType)
    {
        Key key{ areaId, mapId, groundEffectId, terrainType, Normalize(texturePath) };
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;

        // Places, most specific first: the cell's area chain, its map, global. The first matching
        // row wins (place first, then what).
        std::vector<std::pair<uint32_t, uint32_t>> places;
        uint32_t chain[world::kMaxAreaChain];
        const int n = world::AreaChain(areaId, chain, world::kMaxAreaChain);
        for (int i = 0; i < n; ++i) places.push_back({ kArea, chain[i] });
        if (mapId >= 0) places.push_back({ kMap, static_cast<uint32_t>(mapId) });
        places.push_back({ kGlobal, 0 });

        // A matching row with MaterialID 0 means "no material here" and still ends the search, so a
        // zone can switch off a broader row.
        uint32_t material = 0;
        bool found = false;
        for (const auto& place : places)
        {
            auto rows = g_selectors.find(place);
            if (rows == g_selectors.end()) continue;
            for (int level = 0; level < 4 && !found; ++level)
                if (const Selector* s = AtLevel(rows->second, level, key.texture, groundEffectId, terrainType)) { material = s->material; found = true; }
            if (found) break;
        }
        if (g_cache.size() > 20000) g_cache.clear();
        g_cache.emplace(std::move(key), material);
        return material;
    }

    Values Get(uint32_t materialId)
    {
        Values v;
        bool moisture = false, stiffness = false;
        ForChain(materialId, [&](uint32_t id)
        {
            const Material* m = Find(id);
            if (!m) return false;
            if (!moisture && m->restMoisture >= 0.0f) { v.restMoisture = m->restMoisture; moisture = true; }
            if (!stiffness && m->stiffness >= 0.0f)   { v.stiffness = m->stiffness > 1.0f ? 1.0f : m->stiffness; stiffness = true; }
            if (id == materialId) v.flags = m->flags;
            return !(moisture && stiffness);
        });
        return v;
    }

    uint32_t Parent(uint32_t materialId)
    {
        const Material* m = Find(materialId);
        return m && m->parent != materialId ? m->parent : 0;
    }

    bool        Exists(uint32_t materialId) { return Find(materialId) != nullptr; }
    const char* Name(uint32_t materialId)   { const Material* m = Find(materialId); return m ? m->name.c_str() : ""; }
    unsigned    MaterialCount()             { return static_cast<unsigned>(g_materials.size()); }
    unsigned    SelectorCount()             { return g_selectorCount; }
    const char* Status()                    { return g_status.c_str(); }
    uint32_t    Generation()                { return g_generation; }
}
