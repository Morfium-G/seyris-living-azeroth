// Climate: the air's baseline temperature and humidity per place, and the clock it runs on
// (orchestration docs/r&d/immersion/world-fields.md, environment-state.md).
//
// AreaClimate.cdbc, scoped like WindProfile: Global < Map < Area (zone) < Area (sub-area), the more
// specific row wins field by field. A field of -1 inherits from the next place up; temperatures are
// signed, so for them -1000 and below inherit (-1 degC is a real value).
//
// Temperature (degC) = night .. day by the time of day (coldest ~05:00, warmest ~15:00)
//                    + season (SeasonAmplitude across the year, peak in late July, shifted by
//                      SeasonOffset months) + weather (rain and snow cool, sandstorms warm).
#pragma once

#include "WorldQuery.hpp"

#include <cstdint>

struct WXL_SeyrisCdbcApi;

namespace wxl_livingazeroth::climate
{
    struct Row
    {
        float    dayTemp = 18.0f;         // degC at the warmest time of day
        float    nightTemp = 8.0f;        // degC at the coldest
        float    seasonAmplitude = 12.0f; // degC between midsummer and midwinter
        float    seasonOffset = 0.0f;     // months the seasons are shifted (6 = flipped)
        float    humidity = 0.5f;         // 0 dry air .. 1 saturated (slows drying; later fog, dust)
        uint32_t flags = 0;               // reserved
    };

    /// The resolved row for a place (area chain + map), cached per (area, map).
    const Row& For(uint32_t areaId, int mapId);

    /// The clock the climate runs on: game date and time from the client (the server's), the time of
    /// day from the day-night system (what the sky shows).
    struct Clock
    {
        bool  valid = false;
        int   year = 0, month = 0, day = 0, hour = 0, minute = 0; // month 1..12, day 1..31
        float dayFraction = 0.0f;   // 0 = midnight .. 1
        float yearFraction = 0.0f;  // 0 = 1 January .. 1
    };
    const Clock& Now();

    /// The parts of a temperature, for the panel.
    struct Breakdown
    {
        float daily = 0.0f;   // night .. day by the time of day
        float season = 0.0f;  // +- half the amplitude
        float weather = 0.0f; // rain/snow cooling, sand warming
        float total = 0.0f;
    };
    Breakdown Temperature(const Row& row);
    float     TemperatureAt(uint32_t areaId, int mapId);

    /// Weather as the climate sees it (the live weather, or the panel's override).
    struct Weather { int type = 0; float intensity = 0.0f; }; // 0 fine, 1 rain, 2 snow, 3 sand
    const Weather& CurrentWeather();

    /// Once per frame, after world::Refresh().
    void Update(const world::Snapshot& snap);

    /// (Re)loads AreaClimate.cdbc.
    void Load(const WXL_SeyrisCdbcApi* cdbc);
    const char* Status();
    uint32_t    Generation();

    // --- panel overrides (testing; not saved) ---
    /// Weather: type < 0 follows the live weather.
    void SetWeatherOverride(int type, float intensity);
    int  WeatherOverrideType();
    float WeatherOverrideIntensity();
    /// Time of day: < 0 follows the client.
    void  SetTimeOverride(float dayFraction);
    float TimeOverride();
}
