#include "SurfaceCoverTable.hpp"

#include "../env/WorldQuery.hpp"
#include "../wxl_seyris/CdbcApi.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::covertable
{
    namespace
    {
        constexpr const char* kFile = "DBFilesClient\\SurfaceCover.cdbc";
        constexpr WXL_SeyrisCdbcField kFields[] = {
            {"ID",             0,  WXL_CDBC_FIELD_VALUE},
            {"ScopeType",      1,  WXL_CDBC_FIELD_VALUE},
            {"ScopeID",        2,  WXL_CDBC_FIELD_VALUE},
            {"TexturePath",    3,  WXL_CDBC_FIELD_STRING},
            {"GroundEffectID", 4,  WXL_CDBC_FIELD_VALUE},
            {"TerrainType",    5,  WXL_CDBC_FIELD_VALUE},
            {"Depth",          6,  WXL_CDBC_FIELD_VALUE},
            {"MaxSlope",       7,  WXL_CDBC_FIELD_VALUE},
            {"SlopeFade",      8,  WXL_CDBC_FIELD_VALUE},
            {"DriftNoise",     9,  WXL_CDBC_FIELD_VALUE},
            {"EdgeBreakup",    10, WXL_CDBC_FIELD_VALUE},
            {"Rim",            11, WXL_CDBC_FIELD_VALUE},
            {"RelaxSeconds",   12, WXL_CDBC_FIELD_VALUE},
            {"TintColor",      13, WXL_CDBC_FIELD_VALUE},
            {"TintStrength",   14, WXL_CDBC_FIELD_VALUE},
            {"CoverTexture",   15, WXL_CDBC_FIELD_STRING},
            {"Opacity",        16, WXL_CDBC_FIELD_VALUE},  // reserved: not used yet
            {"Flatten",        17, WXL_CDBC_FIELD_VALUE},  // reserved: not used yet
            {"Flags",          18, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kDef = { "SurfaceCover", kFile, kFields, 19 };

        enum Scope : uint32_t { kGlobal = 0, kMap = 1, kArea = 2 };

        // The float fields that inherit with -1, in Values order. TintStrength carries TintColor
        // with it: the row that decides the strength also decides the colour.
        enum Field { kDepth, kMaxSlope, kSlopeFade, kDrift, kBreakup, kRim, kRelax, kTint, kFieldCount };
        constexpr const char* kFloatColumns[kFieldCount] = {
            "Depth", "MaxSlope", "SlopeFade", "DriftNoise", "EdgeBreakup", "Rim", "RelaxSeconds", "TintStrength",
        };

        struct Row
        {
            std::string texture;   // normalized (lower case, backslashes); empty = any texture
            uint32_t    effect = 0;   // 0 = any ground effect
            int32_t     terrain = -1; // -1 = any TerrainType (TerrainType 0 is Dirt, so 0 can't mean "any")
            float       value[kFieldCount];
            uint32_t    tintColor = 0;
            int         coverTexture = -1; // -1 = take it from the next row, 0 = none, 1.. = slot
        };

        // CoverTexture paths in the order the table first names them; slot = index + 1.
        std::vector<std::string> g_coverPaths;
        unsigned g_coverOverflow = 0; // rows naming a texture beyond the slots (drawn without one)

        // Rows by place: (scope type, scope id) -> rows there.
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Row>> g_rows;
        unsigned    g_rowCount = 0;
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
        std::unordered_map<Key, Values, KeyHash> g_cache;

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
        const Row* AtLevel(const std::vector<Row>& rows, int level, const std::string& texture, uint32_t effect, int terrain)
        {
            for (const Row& r : rows)
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
    }

    void Load(const void* cdbcApi)
    {
        g_rows.clear();
        g_cache.clear();
        g_rowCount = 0;
        g_coverPaths.clear();
        g_coverOverflow = 0;
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
            r.texture = Normalize(cdbc->GetString(table, rec, "TexturePath"));
            r.effect = cdbc->Value(table, rec, "GroundEffectID", 0);
            r.terrain = static_cast<int32_t>(cdbc->Value(table, rec, "TerrainType", 0));
            for (int f = 0; f < kFieldCount; ++f) r.value[f] = BitsToFloat(cdbc->Value(table, rec, kFloatColumns[f], 0));
            r.tintColor = cdbc->Value(table, rec, "TintColor", 0);
            // CoverTexture: empty = from the next row, "-" = none, else a path sharing a slot with
            // every row that names the same file.
            const std::string cover = cdbc->GetString(table, rec, "CoverTexture");
            if (cover == "-") r.coverTexture = 0;
            else if (!cover.empty())
            {
                const std::string key = Normalize(cover.c_str());
                int slot = 0;
                for (size_t k = 0; k < g_coverPaths.size(); ++k) if (Normalize(g_coverPaths[k].c_str()) == key) slot = static_cast<int>(k) + 1;
                if (!slot) { g_coverPaths.push_back(cover); slot = static_cast<int>(g_coverPaths.size()); }
                r.coverTexture = slot;
            }
            g_rows[{ scope, id }].push_back(r);
            ++g_rowCount;
        }
        cdbc->Release(table);

        char line[160];
        std::snprintf(line, sizeof(line), "%u row(s) loaded, %u cover texture(s)", g_rowCount, static_cast<unsigned>(g_coverPaths.size()));
        g_status = line;
    }

    unsigned    RowCount()   { return g_rowCount; }
    int         CoverTextureCount() { return static_cast<int>(g_coverPaths.size()); }
    const char* CoverTexturePath(int index)
    {
        return index >= 1 && index <= static_cast<int>(g_coverPaths.size()) ? g_coverPaths[index - 1].c_str() : "";
    }
    const char* Status()     { return g_status.c_str(); }
    uint32_t    Generation() { return g_generation; }

    Values Resolve(uint32_t areaId, int mapId, const char* texturePath, uint32_t groundEffectId, int terrainType)
    {
        Key key{ areaId, mapId, groundEffectId, terrainType, Normalize(texturePath) };
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;

        // Places, most specific first: the cell's area chain, its map, global.
        std::vector<std::pair<uint32_t, uint32_t>> places;
        uint32_t chain[world::kMaxAreaChain];
        const int n = world::AreaChain(areaId, chain, world::kMaxAreaChain);
        for (int i = 0; i < n; ++i) places.push_back({ kArea, chain[i] });
        if (mapId >= 0) places.push_back({ kMap, static_cast<uint32_t>(mapId) });
        places.push_back({ kGlobal, 0 });

        float value[kFieldCount];
        for (float& v : value) v = -1.0f;
        uint32_t tint = 0;
        int cover = -1;
        for (const auto& place : places)
        {
            auto rows = g_rows.find(place);
            if (rows == g_rows.end()) continue;
            for (int level = 0; level < 4; ++level)
                if (const Row* r = AtLevel(rows->second, level, key.texture, groundEffectId, terrainType))
                {
                    if (cover < 0 && r->coverTexture >= 0) cover = r->coverTexture;
                    for (int f = 0; f < kFieldCount; ++f)
                        if (value[f] < 0.0f && r->value[f] >= 0.0f)
                        {
                            value[f] = r->value[f];
                            if (f == kTint) tint = r->tintColor;
                        }
                }
        }

        Values v; // fields no row sets keep the defaults
        if (value[kDepth] >= 0.0f)    v.depth = value[kDepth];
        if (value[kMaxSlope] >= 0.0f) v.maxSlope = value[kMaxSlope];
        if (value[kSlopeFade] >= 0.0f) v.slopeFade = value[kSlopeFade];
        if (value[kDrift] >= 0.0f)    v.driftNoise = value[kDrift];
        if (value[kBreakup] >= 0.0f)  v.edgeBreakup = value[kBreakup];
        if (value[kRim] >= 0.0f)      v.rim = value[kRim];
        if (value[kRelax] >= 0.0f)    v.relaxSeconds = value[kRelax];
        if (value[kTint] >= 0.0f)     { v.tintStrength = value[kTint]; v.tintColor = tint; }
        if (cover > 0)                v.coverTexture = cover;
        if (g_cache.size() > 20000) g_cache.clear();
        g_cache.emplace(std::move(key), v);
        return v;
    }
}
