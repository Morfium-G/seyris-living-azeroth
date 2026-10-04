#include "Climate.hpp"

#include "CdbcLoad.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

namespace wxl_livingazeroth::climate
{
    namespace
    {
        constexpr WXL_SeyrisCdbcField kFields[] = {
            {"ID",              0, WXL_CDBC_FIELD_VALUE},
            {"ScopeType",       1, WXL_CDBC_FIELD_VALUE},
            {"ScopeID",         2, WXL_CDBC_FIELD_VALUE},
            {"DayTemp",         3, WXL_CDBC_FIELD_VALUE},
            {"NightTemp",       4, WXL_CDBC_FIELD_VALUE},
            {"SeasonAmplitude", 5, WXL_CDBC_FIELD_VALUE},
            {"SeasonOffset",    6, WXL_CDBC_FIELD_VALUE},
            {"Humidity",        7, WXL_CDBC_FIELD_VALUE},
            {"Flags",           8, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kDef = { "AreaClimate", "DBFilesClient\\AreaClimate.cdbc", kFields, 9 };

        enum Scope : uint32_t { kGlobal = 0, kMap = 1, kArea = 2 };
        enum Field { kDay, kNight, kAmplitude, kOffset, kHumidity, kFieldCount };
        constexpr const char* kColumns[kFieldCount] = { "DayTemp", "NightTemp", "SeasonAmplitude", "SeasonOffset", "Humidity" };
        constexpr bool kSigned[kFieldCount] = { true, true, false, false, false };

        // Signed columns inherit at -1000 and below, the others at any negative value.
        bool IsSet(int f, float v) { return kSigned[f] ? v > -1000.0f : v >= 0.0f; }

        struct Raw { float value[kFieldCount]; uint32_t flags = 0; };
        std::map<std::pair<uint32_t, uint32_t>, Raw> g_rows; // (scope type, scope id) -> row
        std::map<std::pair<uint32_t, int>, Row> g_cache;     // (area, map) -> resolved
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;

        // --- the client's clock (XWorkbench 2026-10-03) ----------------------------------------------
        // Game date and time as the server sent them, the block GetGameTime (0x608230) and
        // CalendarGetDate (0x5B8160) read: minute 0xD37F98, hour +0x04, weekday +0x08 (0-based),
        // day of month +0x0C (0-based), month +0x10 (0-based), year +0x14 (since 2000).
        constexpr uintptr_t kGameMinute = 0x00D37F98, kGameHour = 0x00D37F9C, kGameMonthDay = 0x00D37FA4,
                            kGameMonth = 0x00D37FA8, kGameYear = 0x00D37FAC;
        // The day-night info block (core offsets/engine/Sky.hpp): __cdecl() -> block, +0x04 = how far
        // the day has run, 0..1.
        constexpr uintptr_t kDayNightGetInfo = 0x007ECEF0;
        constexpr size_t    kInfoDayFraction = 0x04;
        // +0x19C: vec3, the direction of the active celestial light, world space (core
        // offsets/engine/Sky.hpp: the vector the engine hands its own model lighting). [believed:
        // whether it points toward the light or away from it -- either is turned to point up.]
        constexpr size_t    kInfoLightDir = 0x19C;
        using DayNightGetInfoFn = void*(__cdecl*)();

        Clock   g_clock;
        Sun     g_sun;
        Weather g_weather;
        int     g_overrideType = -1;
        float   g_overrideIntensity = 1.0f;
        float   g_timeOverride = -1.0f;
        float   g_temperatureOffset = 0.0f;

        template <class T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        float Wrap01(float v) { v -= std::floor(v); return v; }

        // 0 at the coldest time of day (05:00), 1 at the warmest (15:00); cosine in between, warming
        // over 10 hours and cooling over 14.
        float DailyCurve(float f)
        {
            constexpr float kMin = 5.0f / 24.0f, kMax = 15.0f / 24.0f, kPi = 3.14159265f;
            const float t = Wrap01(f - kMin); // since the coldest moment
            const float warm = kMax - kMin;
            return t < warm ? 0.5f - 0.5f * std::cos(kPi * t / warm)
                            : 0.5f + 0.5f * std::cos(kPi * (t - warm) / (1.0f - warm));
        }
    }

    void Load(const WXL_SeyrisCdbcApi* cdbc)
    {
        g_rows.clear();
        g_cache.clear();
        ++g_generation;
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available: built-in climate"; return; }
        char err[256] = {};
        void* table = cdbcload::LoadAppendOnly(cdbc, kDef, err, sizeof(err));
        if (!table) { g_status = std::string("built-in climate (") + err + ")"; return; }
        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* rec = cdbc->RowAt(table, i);
            if (!rec) continue;
            const uint32_t scope = cdbc->Value(table, rec, "ScopeType", 0);
            const uint32_t id = scope == kGlobal ? 0 : cdbc->Value(table, rec, "ScopeID", 0);
            Raw r;
            for (int f = 0; f < kFieldCount; ++f) r.value[f] = cdbcload::Float(cdbc, table, rec, kColumns[f], -1000.0f);
            r.flags = cdbc->Value(table, rec, "Flags", 0);
            g_rows[{ scope, id }] = r;
        }
        cdbc->Release(table);
        char line[96];
        std::snprintf(line, sizeof(line), "%u row(s) loaded", count);
        g_status = line;
    }

    const Row& For(uint32_t areaId, int mapId)
    {
        const auto key = std::make_pair(areaId, mapId);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;

        std::pair<uint32_t, uint32_t> places[world::kMaxAreaChain + 2];
        int n = 0;
        uint32_t chain[world::kMaxAreaChain];
        const int c = world::AreaChain(areaId, chain, world::kMaxAreaChain);
        for (int i = 0; i < c; ++i) places[n++] = { kArea, chain[i] };
        if (mapId >= 0) places[n++] = { kMap, static_cast<uint32_t>(mapId) };
        places[n++] = { kGlobal, 0 };

        float value[kFieldCount];
        bool set[kFieldCount] = {};
        uint32_t flags = 0;
        bool flagsSet = false;
        for (int i = 0; i < n; ++i)
        {
            auto it = g_rows.find(places[i]);
            if (it == g_rows.end()) continue;
            if (!flagsSet) { flags = it->second.flags; flagsSet = true; }
            for (int f = 0; f < kFieldCount; ++f)
                if (!set[f] && IsSet(f, it->second.value[f])) { value[f] = it->second.value[f]; set[f] = true; }
        }
        Row r; // built-in defaults where nothing sets a field
        if (set[kDay])       r.dayTemp = value[kDay];
        if (set[kNight])     r.nightTemp = value[kNight];
        if (set[kAmplitude]) r.seasonAmplitude = value[kAmplitude];
        if (set[kOffset])    r.seasonOffset = value[kOffset];
        if (set[kHumidity])  r.humidity = value[kHumidity] > 1.0f ? 1.0f : value[kHumidity];
        r.flags = flags;
        if (g_cache.size() > 4096) g_cache.clear();
        return g_cache.emplace(key, r).first->second;
    }

    void Update(const world::Snapshot& snap)
    {
        Clock c;
        c.valid = snap.inWorld;
        const void* info = nullptr;
        if (snap.inWorld)
        {
            c.minute = Read<int32_t>(kGameMinute);
            c.hour = Read<int32_t>(kGameHour);
            c.day = Read<int32_t>(kGameMonthDay) + 1;
            c.month = Read<int32_t>(kGameMonth) + 1;
            c.year = Read<int32_t>(kGameYear) + 2000;
            info = reinterpret_cast<DayNightGetInfoFn>(kDayNightGetInfo)();
            c.dayFraction = info ? Read<float>(reinterpret_cast<uintptr_t>(info) + kInfoDayFraction)
                                 : (c.hour * 60 + c.minute) / 1440.0f;
            if (!(c.dayFraction >= 0.0f && c.dayFraction <= 1.0f)) c.dayFraction = (c.hour * 60 + c.minute) / 1440.0f;
            const int month = c.month < 1 ? 1 : (c.month > 12 ? 12 : c.month);
            c.yearFraction = ((month - 1) + (c.day - 1) / 31.0f) / 12.0f;
        }
        if (g_timeOverride >= 0.0f) c.dayFraction = g_timeOverride;
        g_clock = c;

        // The sun. Daylight from the time of day (0 at 06:00 and 18:00, full from ~08:00 to ~16:00),
        // so the moon (the active light at night) never warms anything.
        constexpr float kPi = 3.14159265f;
        Sun sun;
        const float s = std::sin(2.0f * kPi * (c.dayFraction - 0.25f));
        sun.daylight = s * 3.0f < 0.0f ? 0.0f : (s * 3.0f > 1.0f ? 1.0f : s * 3.0f);
        bool fromClient = false;
        if (info && g_timeOverride < 0.0f)
        {
            const float* d = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(info) + kInfoLightDir);
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0.5f && len < 2.0f)
            {
                const float flip = d[2] < 0.0f ? -1.0f : 1.0f;
                for (int k = 0; k < 3; ++k) sun.dir[k] = flip * d[k] / len;
                fromClient = true;
            }
        }
        if (!fromClient)
        {
            // East (-y) at 06:00, south (-x) at noon, west (+y) at 18:00; up to ~60 degrees high.
            const float a = 2.0f * kPi * (c.dayFraction - 0.5f);
            const float up = 0.85f * (s > 0.0f ? s : 0.0f);
            const float flat = std::sqrt(1.0f - up * up);
            sun.dir[0] = -std::cos(a) * flat; sun.dir[1] = std::sin(a) * flat; sun.dir[2] = up;
        }
        sun.fromClient = fromClient;
        g_sun = sun;

        if (g_overrideType >= 0) { g_weather.type = g_overrideType; g_weather.intensity = g_overrideIntensity; }
        else { g_weather.type = snap.weatherType; g_weather.intensity = snap.weatherIntensity; }
    }

    Breakdown Temperature(const Row& row)
    {
        Breakdown b;
        b.daily = row.nightTemp + (row.dayTemp - row.nightTemp) * DailyCurve(g_clock.dayFraction);
        // Peak around 20 July (0.55 of the year), shifted by SeasonOffset months.
        constexpr float kPi = 3.14159265f;
        const float y = Wrap01(g_clock.yearFraction - 0.55f - row.seasonOffset / 12.0f);
        b.season = g_clock.valid ? 0.5f * row.seasonAmplitude * std::cos(2.0f * kPi * y) : 0.0f;
        // Weather: rules in code for now (world-fields.md: rules are code, numbers can move to data).
        switch (g_weather.type)
        {
            case 1: b.weather = -3.0f * g_weather.intensity; break; // rain
            case 2: b.weather = -6.0f * g_weather.intensity; break; // snow
            case 3: b.weather = 2.0f * g_weather.intensity;  break; // sand
            default: break;
        }
        b.test = g_temperatureOffset;
        b.total = b.daily + b.season + b.weather + b.test;
        return b;
    }

    float TemperatureAt(uint32_t areaId, int mapId) { return Temperature(For(areaId, mapId)).total; }

    const Sun& SunNow() { return g_sun; }

    float SunWarming(const float normal[3], float open)
    {
        const float facing = normal[0] * g_sun.dir[0] + normal[1] * g_sun.dir[1] + normal[2] * g_sun.dir[2];
        if (facing <= 0.0f || g_sun.daylight <= 0.0f || open <= 0.0f) return 0.0f;
        return kSunDegrees * g_sun.daylight * (open > 1.0f ? 1.0f : open) * facing;
    }

    const Clock&   Now()            { return g_clock; }
    const Weather& CurrentWeather() { return g_weather; }
    const char*    Status()         { return g_status.c_str(); }
    uint32_t       Generation()     { return g_generation; }

    void  SetWeatherOverride(int type, float intensity) { g_overrideType = type; g_overrideIntensity = intensity; }
    int   WeatherOverrideType()      { return g_overrideType; }
    float WeatherOverrideIntensity() { return g_overrideIntensity; }
    void  SetTimeOverride(float f)   { g_timeOverride = f; }
    void  SetTemperatureOffset(float degrees) { g_temperatureOffset = degrees; }
    float TemperatureOffset()        { return g_temperatureOffset; }
    float TimeOverride()             { return g_timeOverride; }
}
