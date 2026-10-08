#include "ClientWeather.hpp"

#include <windows.h>

namespace wxl_livingazeroth::clientweather
{
    namespace
    {
        constexpr uintptr_t kWorldWeather = 0x00CD7544; // pointer to the weather object (core Weather.hpp)
        constexpr uintptr_t kStateTable   = 0x00AD4EB0;
        constexpr uintptr_t kStateLookup  = 0x0065C290;
        constexpr uintptr_t kWeatherSet   = 0x007846A0;
        constexpr size_t    kRecType = 0x08, kRecFloat = 0x0C;

        using LookupFn = void*(__fastcall*)(void* table, void* edx, uint32_t state);
        using SetFn    = void(__fastcall*)(void* weather, void* edx, int type, float intensity, void* record, int smooth, float extra);

        bool     g_pending = false;
        uint32_t g_state = 0;
        float    g_intensity = 0.0f;
        bool     g_abrupt = false;
        Result   g_last;

        bool CallSet(void* weather, uint32_t state, float intensity, bool abrupt, Result& r)
        {
            __try
            {
                void* rec = reinterpret_cast<LookupFn>(kStateLookup)(reinterpret_cast<void*>(kStateTable), nullptr, state);
                int   type = 0;
                float extra = 1.0f;
                if (rec)
                {
                    type  = *reinterpret_cast<const int*>(static_cast<const uint8_t*>(rec) + kRecType);
                    extra = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(rec) + kRecFloat);
                }
                r.haveRecord = rec != nullptr;
                r.recordType = rec ? type : -1;
                reinterpret_cast<SetFn>(kWeatherSet)(weather, nullptr, type, intensity, rec, abrupt ? 0 : 1, extra);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    uint32_t StateFor(int type, float intensity)
    {
        const int level = intensity < 0.34f ? 0 : (intensity < 0.67f ? 1 : 2);
        switch (type)
        {
            case 1: { const uint32_t s[] = { kLightRain, kMediumRain, kHeavyRain }; return s[level]; }
            case 2: { const uint32_t s[] = { kLightSnow, kMediumSnow, kHeavySnow }; return s[level]; }
            case 3: { const uint32_t s[] = { kLightSand, kMediumSand, kHeavySand }; return s[level]; }
            default: return kFine;
        }
    }

    void Request(uint32_t state, float intensity, bool abrupt)
    {
        g_pending = true;
        g_state = state;
        g_intensity = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
        g_abrupt = abrupt;
    }

    void Apply()
    {
        if (!g_pending) return;
        g_pending = false;
        Result r;
        r.state = g_state;
        void* weather = *reinterpret_cast<void* const*>(kWorldWeather);
        if (!weather) { r.note = "no weather object (not in the world)"; g_last = r; return; }
        r.applied = CallSet(weather, g_state, g_intensity, g_abrupt, r);
        r.note = r.applied ? (r.haveRecord ? "applied" : "applied without a record (unknown state id)") : "the client's weather setter faulted";
        g_last = r;
    }

    const Result& Last() { return g_last; }
}
