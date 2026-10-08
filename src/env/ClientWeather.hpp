// The client's visible weather, set locally: what the server's weather message does, without the
// server. For testing (rain where the server never sends any) and, later, our own weather handler.
// It lasts until the server sends its next weather message (zone change, weather change).
//
// Traced 2026-10-08 (XWorkbench): the weather message (opcode 0x2F4, case 0x526A69 of the shared
// handler 0x526530) reads (uint32 state, float intensity, uint8 abrupt), looks the state up as a row
// ID with the client's DBC record-by-ID accessor (0x65C290, __thiscall on the table at 0xAD4EB0,
// [believed] Weather.dbc: record +0x08 = type 0..3, +0x0C = a float, +0x10 colour, +0x1C texture
// path), then calls 0x7846A0 __thiscall(weather [0xCD7544], type, intensity, record, smooth =
// !abrupt, record +0x0C or 1.0), ret 0x14.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::clientweather
{
    /// Server weather states = Weather.dbc row IDs [believed: TrinityCore's WeatherState list; the
    /// lookup result is shown in the panel, so a wrong id shows as "no record"].
    enum State : uint32_t
    {
        kFine = 0, kFog = 1,
        kLightRain = 3, kMediumRain = 4, kHeavyRain = 5,
        kLightSnow = 6, kMediumSnow = 7, kHeavySnow = 8,
        kLightSand = 22, kMediumSand = 41, kHeavySand = 42,
    };

    /// The state for one of our weather types (0 fine, 1 rain, 2 snow, 3 sand) at an intensity 0..1.
    uint32_t StateFor(int type, float intensity);

    /// Queues the weather; applied on the next Apply() (main thread, outside rendering).
    void Request(uint32_t state, float intensity, bool abrupt);

    /// Once per frame from OnUpdate.
    void Apply();

    /// The last application, for the panel.
    struct Result
    {
        bool     applied = false;
        uint32_t state = 0;
        bool     haveRecord = false;
        int      recordType = -1;  // the record's type (0 fine, 1 rain, 2 snow, 3 sand)
        const char* note = "";
    };
    const Result& Last();
}
