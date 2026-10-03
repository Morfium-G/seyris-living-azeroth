#include "SurfaceCoverTable.hpp"

#include "GroundMaterialTable.hpp"
#include "../wxl_seyris/CdbcApi.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::covertable
{
    namespace
    {
        constexpr const char* kFile = "DBFilesClient\\SurfaceCover.cdbc";
        constexpr WXL_SeyrisCdbcField kFields[] = {
            {"ID",             0,  WXL_CDBC_FIELD_VALUE},
            {"MaterialID",     1,  WXL_CDBC_FIELD_VALUE},
            {"CoverMaterial",  2,  WXL_CDBC_FIELD_VALUE},
            {"Depth",          3,  WXL_CDBC_FIELD_VALUE},
            {"MaxSlope",       4,  WXL_CDBC_FIELD_VALUE},
            {"SlopeFade",      5,  WXL_CDBC_FIELD_VALUE},
            {"DriftNoise",     6,  WXL_CDBC_FIELD_VALUE},
            {"EdgeBreakup",    7,  WXL_CDBC_FIELD_VALUE},
            {"Rim",            8,  WXL_CDBC_FIELD_VALUE},
            {"RelaxSeconds",   9,  WXL_CDBC_FIELD_VALUE},
            {"TintColor",      10, WXL_CDBC_FIELD_VALUE},
            {"TintStrength",   11, WXL_CDBC_FIELD_VALUE},
            {"CoverTexture",   12, WXL_CDBC_FIELD_STRING},
            {"Opacity",        13, WXL_CDBC_FIELD_VALUE},  // reserved: not used yet
            {"Flatten",        14, WXL_CDBC_FIELD_VALUE},  // reserved: not used yet
            {"ZOffset",        15, WXL_CDBC_FIELD_VALUE},
            {"Wetness",        16, WXL_CDBC_FIELD_VALUE},
            {"Flags",          17, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kDef = { "SurfaceCover", kFile, kFields, 18 };

        // The reader accepts files with more columns than a definition names, so an older 21-column
        // SurfaceCover.cdbc (place keys in the table itself) would load as garbage under this layout.
        // A one-column definition at index 20 loads only from such a file: refuse it.
        constexpr WXL_SeyrisCdbcField kOldLayoutProbe[] = { {"Probe", 20, WXL_CDBC_FIELD_VALUE} };
        constexpr WXL_SeyrisCdbcDefinition kOldLayoutDef = { "SurfaceCover", kFile, kOldLayoutProbe, 1 };

        // The float fields that inherit with -1, in Values order. TintStrength carries TintColor
        // with it: the row that decides the strength also decides the colour. Depth carries Flags
        // the same way (0 is a valid Flags value, so it can't inherit on its own).
        enum Field { kDepth, kMaxSlope, kSlopeFade, kDrift, kBreakup, kRim, kRelax, kTint, kZOffset, kWetness, kFieldCount };
        constexpr const char* kFloatColumns[kFieldCount] = {
            "Depth", "MaxSlope", "SlopeFade", "DriftNoise", "EdgeBreakup", "Rim", "RelaxSeconds", "TintStrength",
            "ZOffset", "Wetness",
        };

        // Whether a row sets field f. ZOffset is signed: -1000 and below inherit (the rule for every
        // signed column), and so does exactly -1 (what older files and the v3 converter wrote).
        bool IsSet(int f, float v) { return f == kZOffset ? (v != -1.0f && v > -1000.0f) : v >= 0.0f; }

        struct Row
        {
            float    value[kFieldCount];
            uint32_t tintColor = 0;
            uint32_t flags = 0;
            uint32_t coverMaterial = 0;  // 0 = from the parent material's row
            int      coverTexture = -1;  // -1 = from the parent material's row, 0 = none, 1.. = ID
        };

        // CoverTexture paths in the order the table first names them; ID = index + 1.
        std::vector<std::string> g_coverPaths;
        std::vector<bool>        g_coverIgnoreSpecular; // per path: a row naming it has kFlagIgnoreSpecular

        std::unordered_map<uint32_t, Row>    g_rows;   // by MaterialID
        std::unordered_map<uint32_t, Values> g_cache;  // by MaterialID, resolved
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;

        float BitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }

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

        // A material's cover: each field from the first row along the material's Parent chain that
        // sets it. A material without a row of its own takes everything from its parents'.
        Values ForMaterial(uint32_t material)
        {
            if (auto it = g_cache.find(material); it != g_cache.end()) return it->second;

            float value[kFieldCount] = {};
            bool set[kFieldCount] = {};
            uint32_t tint = 0, flags = 0, coverMaterial = 0;
            int cover = -1;
            materials::ForChain(material, [&](uint32_t id)
            {
                auto it = g_rows.find(id);
                if (it == g_rows.end()) return true;
                const Row& r = it->second;
                if (cover < 0 && r.coverTexture >= 0) cover = r.coverTexture;
                if (!coverMaterial && r.coverMaterial) coverMaterial = r.coverMaterial;
                for (int f = 0; f < kFieldCount; ++f)
                    if (!set[f] && IsSet(f, r.value[f]))
                    {
                        set[f] = true;
                        value[f] = r.value[f];
                        if (f == kTint) tint = r.tintColor;
                        if (f == kDepth) flags = r.flags;
                    }
                return true;
            });

            Values v; // fields no row sets keep the defaults
            v.material = material;
            if (set[kDepth])     { v.depth = value[kDepth]; v.flags = flags; }
            if (set[kMaxSlope])  v.maxSlope = value[kMaxSlope];
            if (set[kSlopeFade]) v.slopeFade = value[kSlopeFade];
            if (set[kDrift])     v.driftNoise = value[kDrift];
            if (set[kBreakup])   v.edgeBreakup = value[kBreakup];
            if (set[kRim])       v.rim = value[kRim];
            if (set[kRelax])     v.relaxSeconds = value[kRelax];
            if (set[kTint])      { v.tintStrength = value[kTint]; v.tintColor = tint; }
            if (set[kZOffset])   v.zOffset = value[kZOffset];
            if (set[kWetness])   v.wetness = value[kWetness] > 1.0f ? 1.0f : value[kWetness];
            if (cover > 0)       v.coverTexture = cover;
            // What lies on top is the cover's own material; without one it's the ground's. Its
            // stiffness decides how far feet press the cover down.
            v.coverMaterial = coverMaterial ? coverMaterial : material;
            v.stiffness = materials::Get(v.coverMaterial).stiffness;
            g_cache.emplace(material, v);
            return v;
        }
    }

    void Load(const void* cdbcApi)
    {
        materials::Load(cdbcApi);
        g_rows.clear();
        g_cache.clear();
        g_coverPaths.clear();
        g_coverIgnoreSpecular.clear();
        ++g_generation;

        const auto* cdbc = static_cast<const WXL_SeyrisCdbcApi*>(cdbcApi);
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available: no cover rows"; return; }

        char err[256] = {};
        if (void* old = cdbc->Load(&kOldLayoutDef, err, sizeof(err)))
        {
            cdbc->Release(old);
            g_status = "an older layout (place keys in SurfaceCover itself): split it with tools/convert_surface_cover_v4.py";
            return;
        }
        err[0] = 0;
        void* table = cdbc->Load(&kDef, err, sizeof(err));
        if (!table)
        {
            g_status = std::string("no rows (") + err + ")";
            if (std::strstr(err, "only has 19 columns")) g_status += " -- an older layout: convert it with tools/convert_surface_cover_v3.py, then _v4.py";
            return;
        }

        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* rec = cdbc->RowAt(table, i);
            if (!rec) continue;
            Row r;
            for (int f = 0; f < kFieldCount; ++f) r.value[f] = BitsToFloat(cdbc->Value(table, rec, kFloatColumns[f], 0));
            r.tintColor = cdbc->Value(table, rec, "TintColor", 0);
            r.flags = cdbc->Value(table, rec, "Flags", 0);
            r.coverMaterial = cdbc->Value(table, rec, "CoverMaterial", 0);
            if (r.coverMaterial == 0xFFFFFFFFu) r.coverMaterial = 0;
            // CoverTexture: empty = from the parent material, "-" = none, else a path sharing an ID
            // with every row that names the same file.
            const std::string cover = cdbc->GetString(table, rec, "CoverTexture");
            if (cover == "-") r.coverTexture = 0;
            else if (!cover.empty())
            {
                const std::string key = Normalize(cover.c_str());
                int id = 0;
                for (size_t k = 0; k < g_coverPaths.size(); ++k) if (Normalize(g_coverPaths[k].c_str()) == key) id = static_cast<int>(k) + 1;
                if (!id) { g_coverPaths.push_back(cover); g_coverIgnoreSpecular.push_back(false); id = static_cast<int>(g_coverPaths.size()); }
                r.coverTexture = id;
                if (r.flags & kFlagIgnoreSpecular) g_coverIgnoreSpecular[id - 1] = true;
            }
            g_rows[cdbc->Value(table, rec, "MaterialID", 0)] = r;
        }
        cdbc->Release(table);

        char line[200];
        std::snprintf(line, sizeof(line), "%u cover row(s), %u cover texture(s); %s",
                      static_cast<unsigned>(g_rows.size()), static_cast<unsigned>(g_coverPaths.size()), materials::Status());
        g_status = line;
    }

    unsigned    RowCount()   { return static_cast<unsigned>(g_rows.size()); }
    int         CoverTextureCount() { return static_cast<int>(g_coverPaths.size()); }
    const char* CoverTexturePath(int index)
    {
        return index >= 1 && index <= static_cast<int>(g_coverPaths.size()) ? g_coverPaths[index - 1].c_str() : "";
    }
    bool CoverTextureIgnoresSpecular(int index)
    {
        return index >= 1 && index <= static_cast<int>(g_coverIgnoreSpecular.size()) && g_coverIgnoreSpecular[index - 1];
    }
    const char* Status()     { return g_status.c_str(); }
    uint32_t    Generation() { return g_generation; }

    Values Resolve(uint32_t areaId, int mapId, const char* texturePath, uint32_t groundEffectId, int terrainType)
    {
        const uint32_t material = materials::Select(areaId, mapId, texturePath, groundEffectId, terrainType);
        if (!material) return Values{};
        return ForMaterial(material);
    }
}
