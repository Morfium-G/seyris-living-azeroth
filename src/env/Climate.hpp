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
        float    fogginess = 0.0f;        // 0 .. 1: how foggy the place is by nature (area fog, more mist)
        uint32_t flags = 0;               // reserved
    };

    /// The resolved row for a place (area chain + map), cached per (area, map). A hard step at every
    /// area border: for "the climate here", use At().
    const Row& For(uint32_t areaId, int mapId);

    /// The climate at a world position, blended across area borders: each terrain chunk (33 yd)
    /// takes its area's row, averaged with its 8 neighbours, interpolated between chunk centres, so
    /// crossing a border changes temperature, humidity and fogginess over ~100 yd instead of at
    /// once. Within ~800 yd of the player (the blend map follows); beyond it, or before its chunks
    /// are known, the area's own row. Flags are the chunk's own.
    Row At(float x, float y);
    /// At() where the player is.
    Row Here();

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
        float test = 0.0f;    // the panel's test offset
        float total = 0.0f;
    };
    Breakdown Temperature(const Row& row);
    /// The same at another time of day (0..1, wraps): today's season and weather, e.g. an hour ago
    /// (the ground fog's "is it cooling?").
    Breakdown TemperatureAtTime(const Row& row, float dayFraction);
    /// degC of standing water: the day's mean plus the season, without the weather's chill (water
    /// lags the air, so cool evenings over lakes make fog).
    float WaterTemperature(const Row& row);
    float     TemperatureAt(uint32_t areaId, int mapId);

    /// The sun: the direction toward it (world space, z up) and how much it shines (0 at night .. 1
    /// by day, from the time of day). The direction is the client's active celestial light (the
    /// day-night block the sky and model lighting use); without it, a sun computed from the time of
    /// day (east at 06:00, south at noon).
    struct Sun { float dir[3] = { 0.0f, 0.0f, 1.0f }; float daylight = 0.0f; bool fromClient = false; };
    const Sun& SunNow();

    /// degC the sun adds on ground with this (unit, world) normal, open to the sky by `open` (0..1):
    /// up to kSunDegrees by day on ground facing it, nothing at night or under a roof.
    constexpr float kSunDegrees = 4.0f;
    float SunWarming(const float normal[3], float open);

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
    /// degC added to every place's temperature (0 = off).
    void  SetTemperatureOffset(float degrees);
    float TemperatureOffset();
    /// Time of day: < 0 follows the client.
    void  SetTimeOverride(float dayFraction);
    float TimeOverride();
}
