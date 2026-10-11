#include "FogSettings.hpp"

#include "CdbcLoad.hpp"
#include "Fog.hpp"
#include "WorldQuery.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace wxl_livingazeroth::fogsettings
{
    namespace
    {
        constexpr WXL_SeyrisCdbcField kFields[] = {
            {"ID",          0, WXL_CDBC_FIELD_VALUE},
            {"ScopeType",   1, WXL_CDBC_FIELD_VALUE},
            {"ScopeID",     2, WXL_CDBC_FIELD_VALUE},
            {"Kind",        3, WXL_CDBC_FIELD_VALUE},
            {"Strength",    4, WXL_CDBC_FIELD_VALUE},
            {"Density",     5, WXL_CDBC_FIELD_VALUE},
            {"Thickness",   6, WXL_CDBC_FIELD_VALUE},
            {"BuildUp",     7, WXL_CDBC_FIELD_VALUE},
            {"BurnOff",     8, WXL_CDBC_FIELD_VALUE},
            {"WindThin",    9, WXL_CDBC_FIELD_VALUE},
            {"WindDrift",  10, WXL_CDBC_FIELD_VALUE},
            {"Patchiness", 11, WXL_CDBC_FIELD_VALUE},
            {"Weight",     12, WXL_CDBC_FIELD_VALUE},
            {"ColorMode",  13, WXL_CDBC_FIELD_VALUE},
            {"ColorR",     14, WXL_CDBC_FIELD_VALUE},
            {"ColorG",     15, WXL_CDBC_FIELD_VALUE},
            {"ColorB",     16, WXL_CDBC_FIELD_VALUE},
            {"Saturation", 17, WXL_CDBC_FIELD_VALUE},
            {"Brightness", 18, WXL_CDBC_FIELD_VALUE},
            {"Flags",      19, WXL_CDBC_FIELD_VALUE},
            {"Comment",    20, WXL_CDBC_FIELD_STRING}, // appended 2026-10-11: for the editor, not read
        };
        constexpr WXL_SeyrisCdbcDefinition kDef = { "FogSettings", "DBFilesClient\\FogSettings.cdbc", kFields, 21 };

        enum Scope : uint32_t { kGlobal = 0, kMap = 1, kArea = 2 };

        // The float columns in the order they're stored in Raw::value.
        constexpr const char* kFloatColumns[] = { "Strength", "Density", "Thickness", "BuildUp", "BurnOff", "WindThin",
                                                  "WindDrift", "Patchiness", "Weight", "ColorR", "ColorG", "ColorB",
                                                  "Saturation", "Brightness" };
        constexpr int kFloats = sizeof(kFloatColumns) / sizeof(kFloatColumns[0]);

        struct Raw
        {
            uint32_t kind = 0;            // table numbering: 0 all kinds, 1..5 = fog::Kind + 1
            float    value[kFloats];
            int      colorMode = -1;      // -1 = take from the next row
            uint32_t flags = 0;
        };

        std::map<std::pair<uint32_t, uint32_t>, std::vector<Raw>> g_rows; // (scope type, id) -> rows
        std::map<std::tuple<uint32_t, int, int>, Values> g_cache;
        const WXL_SeyrisCdbcApi* g_cdbc = nullptr;
        unsigned    g_rowCount = 0;
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;

        const Raw* RowFor(const std::vector<Raw>& rows, uint32_t kind)
        {
            for (const Raw& r : rows) if (r.kind == kind) return &r;
            return nullptr;
        }

        void Fill(Values& v, const float raw[kFloats], int colorMode, bool set[kFloats + 1])
        {
            float* targets[kFloats] = { &v.strength, &v.density, &v.thickness, &v.buildUp, &v.burnOff, &v.windThin,
                                        &v.windDrift, &v.patchiness, &v.weight, &v.color[0], &v.color[1], &v.color[2],
                                        &v.saturation, &v.brightness };
            for (int f = 0; f < kFloats; ++f)
                if (!set[f] && raw[f] >= 0.0f) { *targets[f] = raw[f]; set[f] = true; }
            if (!set[kFloats] && colorMode >= 0) { v.colorMode = colorMode; set[kFloats] = true; }
        }
    }

    Values Defaults(int kind)
    {
        Values v;
        switch (kind)
        {
            case fog::kMist:     v.buildUp = 300.0f; v.burnOff = 900.0f;  v.thickness = 1.5f;  v.density = 0.06f; v.weight = 0.7f; v.windThin = 0.4f; break;
            case fog::kWaterFog: v.buildUp = 300.0f; v.burnOff = 1200.0f; v.thickness = 3.0f;  v.density = 0.05f; v.weight = 0.4f; v.windThin = 0.5f; break;
            case fog::kSteam:    v.buildUp = 10.0f;  v.burnOff = 30.0f;   v.thickness = 2.0f;  v.density = 0.08f; v.weight = 0.0f; v.windThin = 0.8f; break;
            case fog::kAreaFog:  v.buildUp = 600.0f; v.burnOff = 1800.0f; v.thickness = 8.0f;  v.density = 0.03f; v.weight = 0.3f; v.windThin = 0.7f; break;
            case fog::kHaze:     v.buildUp = 120.0f; v.burnOff = 600.0f;  v.thickness = 20.0f; v.density = 0.01f; v.weight = 0.0f; v.windThin = 1.0f; break;
            default: break;
        }
        return v;
    }

    void Load(const WXL_SeyrisCdbcApi* cdbc)
    {
        g_cdbc = cdbc;
        g_rows.clear();
        g_cache.clear();
        g_rowCount = 0;
        ++g_generation;
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available: built-in fog settings"; return; }
        char err[256] = {};
        void* table = cdbcload::LoadAppendOnly(cdbc, kDef, err, sizeof(err));
        if (!table) { g_status = std::string("built-in fog settings (") + err + ")"; return; }
        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* rec = cdbc->RowAt(table, i);
            if (!rec) continue;
            const uint32_t scope = cdbc->Value(table, rec, "ScopeType", 0);
            const uint32_t id = scope == kGlobal ? 0 : cdbc->Value(table, rec, "ScopeID", 0);
            Raw r;
            r.kind = cdbc->Value(table, rec, "Kind", 0);
            for (int f = 0; f < kFloats; ++f) r.value[f] = cdbcload::Float(cdbc, table, rec, kFloatColumns[f], -1.0f);
            r.colorMode = cdbcload::Has(cdbc, table, "ColorMode") ? static_cast<int>(cdbc->Value(table, rec, "ColorMode", 0)) : -1;
            r.flags = cdbcload::Has(cdbc, table, "Flags") ? cdbc->Value(table, rec, "Flags", 0) : 0;
            g_rows[{ scope, id }].push_back(r);
            ++g_rowCount;
        }
        cdbc->Release(table);
        char line[96];
        std::snprintf(line, sizeof(line), "%u row(s) loaded", g_rowCount);
        g_status = line;
    }

    void Reload() { Load(g_cdbc); }
    const char* Status()     { return g_status.c_str(); }
    unsigned    RowCount()   { return g_rowCount; }
    uint32_t    Generation() { return g_generation; }

    const Values& Resolve(uint32_t areaId, int mapId, int kind)
    {
        const auto key = std::make_tuple(areaId, mapId, kind);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;

        std::pair<uint32_t, uint32_t> places[world::kMaxAreaChain + 2];
        int n = 0;
        uint32_t chain[world::kMaxAreaChain];
        const int c = world::AreaChain(areaId, chain, world::kMaxAreaChain);
        for (int i = 0; i < c; ++i) places[n++] = { kArea, chain[i] };
        if (mapId >= 0) places[n++] = { kMap, static_cast<uint32_t>(mapId) };
        places[n++] = { kGlobal, 0 };

        Values v = Defaults(kind < 0 ? -1 : kind);
        bool set[kFloats + 1] = {};
        bool flagsSet = false;
        for (int i = 0; i < n; ++i)
        {
            auto rows = g_rows.find(places[i]);
            if (rows == g_rows.end()) continue;
            // The kind's own row first, then the "all kinds" row, at each place.
            const Raw* order[2] = { kind >= 0 ? RowFor(rows->second, static_cast<uint32_t>(kind + 1)) : nullptr,
                                    RowFor(rows->second, 0) };
            for (const Raw* r : order)
            {
                if (!r) continue;
                Fill(v, r->value, r->colorMode, set);
                if (!flagsSet) { v.flags = r->flags; flagsSet = true; }
            }
        }
        // A colour without a mode means "use this colour": hue mode (keeps the stock colour's brightness).
        // Stock mode would ignore the colour, which reads as "the colour does nothing".
        if (!set[kFloats] && (set[9] || set[10] || set[11])) v.colorMode = kColorHue;
        if (v.colorMode < kColorStock || v.colorMode > kColorReplace) v.colorMode = kColorStock;
        if (g_cache.size() > 4096) g_cache.clear();
        return g_cache.emplace(key, v).first->second;
    }
}
