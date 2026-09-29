#include "Wind.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wxl_livingazeroth::wind
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";
        constexpr float kPi = 3.14159265358979f;

        // --- WindProfile.cdbc ---------------------------------------------------------------------
        // Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID, the FieldCount profile floats in
        // Field order, Flags. See orchestration/docs/r&d/immersion/environment-state.md (draft v2).
        enum Scope { kScopeGlobal = 0, kScopeMap = 1, kScopeArea = 2 };

        const char* const kFieldNames[FieldCount] = {
            "GroundMin", "GroundMax", "AloftMin", "AloftMax", "AloftHeight",
            "WeatherInfluence", "GustStrength", "GustFrequency", "TerrainWeight",
            "LockedDirection", "LockStrength",
        };

        constexpr WXL_SeyrisCdbcField kProfileFields[] = {
            {"ID",               0,  WXL_CDBC_FIELD_VALUE},
            {"ScopeType",        1,  WXL_CDBC_FIELD_VALUE},
            {"ScopeID",          2,  WXL_CDBC_FIELD_VALUE},
            {"GroundMin",        3,  WXL_CDBC_FIELD_VALUE},
            {"GroundMax",        4,  WXL_CDBC_FIELD_VALUE},
            {"AloftMin",         5,  WXL_CDBC_FIELD_VALUE},
            {"AloftMax",         6,  WXL_CDBC_FIELD_VALUE},
            {"AloftHeight",      7,  WXL_CDBC_FIELD_VALUE},
            {"WeatherInfluence", 8,  WXL_CDBC_FIELD_VALUE},
            {"GustStrength",     9,  WXL_CDBC_FIELD_VALUE},
            {"GustFrequency",    10, WXL_CDBC_FIELD_VALUE},
            {"TerrainWeight",    11, WXL_CDBC_FIELD_VALUE},
            {"LockedDirection",  12, WXL_CDBC_FIELD_VALUE},
            {"LockStrength",     13, WXL_CDBC_FIELD_VALUE},
            {"Flags",            14, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kProfileDef = {
            "WindProfile", "DBFilesClient\\WindProfile.cdbc", kProfileFields,
            sizeof(kProfileFields) / sizeof(kProfileFields[0]),
        };

        struct Row
        {
            int32_t  scopeType = 0;
            int32_t  scopeId = 0;
            float    v[FieldCount] = {};
            uint32_t flags = 0;
        };

        const WXL_Api*   g_api = nullptr;
        std::vector<Row> g_rows;
        bool             g_loaded = false;
        char             g_error[256] = "not loaded yet";

        Resolved g_target;
        Resolved g_current;
        bool     g_haveCurrent = false;

        float g_time = 0.0f;
        float g_indoor = 1.0f;
        float g_weatherOverride = -1.0f;
        float g_weatherUsed = 0.0f;
        float g_steadyGround = 0.0f, g_steadyAloft = 0.0f, g_bearing = 0.0f;
        int   g_mapId = -1;

        float BitsToFloat(uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; }
        float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
        float Lerp(float a, float b, float t) { return a + (b - a) * t; }

        // Signed shortest angle from a to b, in degrees.
        float AngleDelta(float a, float b)
        {
            float d = std::fmod(b - a, 360.0f);
            if (d > 180.0f) d -= 360.0f;
            if (d < -180.0f) d += 360.0f;
            return d;
        }

        // --- value noise (deterministic, cheap) ---------------------------------------------------
        float Hash(int32_t x, int32_t y)
        {
            uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
        }
        float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

        float Noise1(float x)
        {
            const float fx = std::floor(x);
            const int32_t i = static_cast<int32_t>(fx);
            return Lerp(Hash(i, 0), Hash(i + 1, 0), Smooth(x - fx));
        }

        float Noise2(float x, float y)
        {
            const float fx = std::floor(x), fy = std::floor(y);
            const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
            const float tx = Smooth(x - fx), ty = Smooth(y - fy);
            const float a = Lerp(Hash(ix, iy),     Hash(ix + 1, iy),     tx);
            const float b = Lerp(Hash(ix, iy + 1), Hash(ix + 1, iy + 1), tx);
            return Lerp(a, b, ty);
        }

        float Fbm2(float x, float y)
        {
            return 0.55f * Noise2(x, y) + 0.3f * Noise2(x * 2.1f + 7.3f, y * 2.1f - 3.1f)
                 + 0.15f * Noise2(x * 4.3f - 1.7f, y * 4.3f + 5.9f);
        }

        // --- profile resolution ---------------------------------------------------------------------
        void Apply(Resolved& r, const Row& row, Source source, uint32_t id)
        {
            for (int f = 0; f < FieldCount; ++f)
            {
                if (row.v[f] < 0.0f) continue; // inherit
                r.value[f] = row.v[f];
                r.source[f] = source;
                r.sourceId[f] = id;
            }
            r.flags = row.flags;
        }

        const Row* FindRow(int scopeType, int32_t scopeId)
        {
            for (const Row& row : g_rows)
                if (row.scopeType == scopeType && (scopeType == kScopeGlobal || row.scopeId == scopeId))
                    return &row;
            return nullptr;
        }

        Resolved Resolve(const world::Snapshot& snap)
        {
            Resolved r; // built-in: everything 0 -> no wind without a Global row

            if (const Row* g = FindRow(kScopeGlobal, 0)) Apply(r, *g, Source::Global, 0);
            if (const Row* m = FindRow(kScopeMap, snap.mapId)) Apply(r, *m, Source::Map, static_cast<uint32_t>(snap.mapId));

            // Least specific first, so the sub-area has the last word.
            for (int i = snap.areaCount - 1; i >= 0; --i)
                if (const Row* a = FindRow(kScopeArea, static_cast<int32_t>(snap.areaChain[i])))
                    Apply(r, *a, Source::Area, snap.areaChain[i]);
            return r;
        }

        void BlendToward(float dt)
        {
            if (!g_haveCurrent) { g_current = g_target; g_haveCurrent = true; return; }

            const float k = 1.0f - std::exp(-dt / 1.5f); // ~1.5 s to settle after crossing a border
            for (int f = 0; f < FieldCount; ++f)
            {
                if (f == LockedDirection)
                    g_current.value[f] += AngleDelta(g_current.value[f], g_target.value[f]) * k;
                else
                    g_current.value[f] = Lerp(g_current.value[f], g_target.value[f], k);
                g_current.source[f] = g_target.source[f];
                g_current.sourceId[f] = g_target.sourceId[f];
            }
            g_current.flags = g_target.flags;
        }

        float MapBaseBearing(int mapId) { return Hash(mapId, 91) * 360.0f; }
    }

    const char* FieldName(int field) { return (field >= 0 && field < FieldCount) ? kFieldNames[field] : "?"; }

    void Init(const WXL_Api* api) { g_api = api; }

    void LoadProfiles(const WXL_SeyrisCdbcApi* cdbc)
    {
        g_rows.clear();
        g_loaded = false;

        if (!cdbc || !cdbc->HasFeature("cdbc-load"))
        {
            std::snprintf(g_error, sizeof(g_error), "seyris.cdbc unavailable (wxl-seyris-tools missing or too old)");
            g_api->Log(WXL_LOG_WARN, kTag, "wind: %s; wind stays off.", g_error);
            return;
        }

        char err[256] = {};
        void* table = cdbc->Load(&kProfileDef, err, sizeof(err));
        if (!table)
        {
            std::snprintf(g_error, sizeof(g_error), "WindProfile.cdbc: %s", err);
            g_api->Log(WXL_LOG_WARN, kTag, "wind: %s; wind stays off.", g_error);
            return;
        }

        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* rowPtr = cdbc->RowAt(table, i);
            if (!rowPtr) continue;
            Row row;
            row.scopeType = static_cast<int32_t>(cdbc->Value(table, rowPtr, "ScopeType", 0));
            row.scopeId   = static_cast<int32_t>(cdbc->Value(table, rowPtr, "ScopeID", 0));
            for (int f = 0; f < FieldCount; ++f)
                row.v[f] = BitsToFloat(cdbc->Value(table, rowPtr, kFieldNames[f], 0));
            row.flags = cdbc->Value(table, rowPtr, "Flags", 0);
            g_rows.push_back(row);
        }
        cdbc->Release(table);

        g_loaded = true;
        g_error[0] = '\0';
        g_haveCurrent = false; // snap to the new data instead of blending from the old
        g_api->Log(WXL_LOG_INFO, kTag, "wind: WindProfile.cdbc loaded, %u row(s).", count);
    }

    void Update(float dt, const world::Snapshot& snap)
    {
        if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f; // hitches and the first frame
        g_time += dt;
        g_mapId = snap.mapId;

        g_target = Resolve(snap);
        BlendToward(dt);
        const float* p = g_current.value;

        // Indoors (the client's own verdict): fade out over ~0.75 s.
        const float indoorTarget = (snap.inWorld && !snap.outdoors) ? 0.0f : 1.0f;
        g_indoor = Lerp(g_indoor, indoorTarget, 1.0f - std::exp(-dt / 0.75f));

        // Where in [min, max] the steady wind sits: a slow wander in the lower-middle part, pushed
        // toward max by weather in proportion to WeatherInfluence.
        g_weatherUsed = g_weatherOverride >= 0.0f ? g_weatherOverride : snap.weatherIntensity;
        float rangePos = 0.2f + 0.5f * Noise1(g_time / 40.0f + 17.3f);
        rangePos += (1.0f - rangePos) * Clamp01(g_weatherUsed * p[WeatherInfluence]);

        g_steadyGround = Lerp(p[GroundMin], p[GroundMax], rangePos) * g_indoor;
        g_steadyAloft  = Lerp(p[AloftMin],  p[AloftMax],  rangePos) * g_indoor;

        // Prevailing direction: a per-map base that wanders +-60 degrees over minutes, pulled toward
        // the locked direction by LockStrength.
        float bearing = MapBaseBearing(snap.mapId) + 60.0f * (Noise1(g_time / 90.0f + 3.1f) * 2.0f - 1.0f);
        bearing += AngleDelta(bearing, p[LockedDirection]) * Clamp01(p[LockStrength]);
        g_bearing = std::fmod(bearing + 360.0f, 360.0f);
    }

    Sample At(const float pos[3], float heightAboveGround)
    {
        Sample s;
        const float* p = g_current.value;

        const float aloftT = p[AloftHeight] > 0.0f ? Clamp01(heightAboveGround / p[AloftHeight]) : 0.0f;
        const float steady = Lerp(g_steadyGround, g_steadyAloft, aloftT);

        // Bearing (clockwise from north = +X) to a world vector: east is -Y.
        const float rad = g_bearing * kPi / 180.0f;
        const float dx = std::cos(rad), dy = -std::sin(rad);
        const float px = -dy, py = dx; // perpendicular

        // Gusts: a noise field stretched along the wind and scrolled downwind at a speed that grows
        // with the wind, so bursts visibly travel across the landscape.
        const float freq  = p[GustFrequency] > 0.05f ? p[GustFrequency] : 0.05f;
        const float speed = 4.0f + 16.0f * steady; // yards per second
        const float u = pos[0] * dx + pos[1] * dy;
        const float v = pos[0] * px + pos[1] * py;
        const float along = 50.0f / freq, across = 35.0f / freq;
        const float n = Fbm2((u - g_time * speed) / along, v / across);
        s.gust = Clamp01((n - 0.5f) * 3.0f);

        // Gusts veer the direction a little too.
        const float veer = (Noise2((u - g_time * speed) / 80.0f + 31.0f, v / 80.0f) - 0.5f)
                         * 20.0f * (0.5f + s.gust) * kPi / 180.0f;
        s.dirX = dx * std::cos(veer) - dy * std::sin(veer);
        s.dirY = dx * std::sin(veer) + dy * std::cos(veer);
        s.strength = steady * (1.0f + p[GustStrength] * s.gust);
        return s;
    }

    bool        ProfilesLoaded()  { return g_loaded; }
    uint32_t    ProfileRowCount() { return static_cast<uint32_t>(g_rows.size()); }
    const char* ProfileError()    { return g_error; }
    const Resolved& Target()      { return g_target; }
    const Resolved& Current()     { return g_current; }
    float SteadyGround()          { return g_steadyGround; }
    float SteadyAloft()           { return g_steadyAloft; }
    float BearingDegrees()        { return g_bearing; }
    float IndoorFactor()          { return g_indoor; }
    float EffectiveWeather()      { return g_weatherUsed; }

    void  SetWeatherOverride(float intensity) { g_weatherOverride = intensity; }
    float WeatherOverride()                   { return g_weatherOverride; }
}
