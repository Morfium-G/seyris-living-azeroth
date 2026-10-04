#include "EnvironmentLua.hpp"

#include "LuaApi.hpp"
#include "../env/Climate.hpp"
#include "../env/Fields.hpp"
#include "../env/WorldQuery.hpp"

#include "game/Script.hpp"

#include <cstring>

namespace wxl_livingazeroth::envlua
{
    namespace
    {
        namespace sc = wxl::game::script;

        // Bump when functions are added (addons check it); never change what an existing one returns.
        constexpr int kApiVersion = 1;

        const char* WeatherName(int type)
        {
            switch (type) { case 1: return "rain"; case 2: return "snow"; case 3: return "sand"; default: return "fine"; }
        }

        uint32_t PlayerArea(const world::Snapshot& s) { return s.areaCount > 0 ? s.areaChain[0] : 0; }

        // LivingAzeroth_GetVersion() -> version
        int __cdecl GetVersion(void* L)
        {
            sc::PushNumber(L, kApiVersion);
            return 1;
        }

        // LivingAzeroth_GetTemperature() -> ground degC, air degC   (nil, nil outside the world)
        // ground: near the ground where the player stands (warmed by lava etc.); air: the place's climate.
        int __cdecl GetTemperature(void* L)
        {
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) { sc::PushNil(L); sc::PushNil(L); return 2; }
            const float air = climate::TemperatureAt(PlayerArea(s), s.mapId);
            float ground = air;
            fields::Sample(fields::Find("temperature"), s.playerPos, ground);
            sc::PushNumber(L, ground);
            sc::PushNumber(L, air);
            return 2;
        }

        // LivingAzeroth_GetHumidity() -> 0..1 (the air's, from the place's climate)
        int __cdecl GetHumidity(void* L)
        {
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) { sc::PushNil(L); return 1; }
            sc::PushNumber(L, climate::For(PlayerArea(s), s.mapId).humidity);
            return 1;
        }

        // LivingAzeroth_GetGroundMoisture() -> moisture 0..1, the ground's resting moisture 0..1
        // (nil, nil where it isn't known yet)
        int __cdecl GetGroundMoisture(void* L)
        {
            const world::Snapshot& s = world::Current();
            const fields::MoistureDetail m = s.inWorld ? fields::Moisture(s.playerPos) : fields::MoistureDetail{};
            if (!m.known) { sc::PushNil(L); sc::PushNil(L); return 2; }
            sc::PushNumber(L, m.value);
            sc::PushNumber(L, m.rest);
            return 2;
        }

        // LivingAzeroth_GetWeather() -> "fine" | "rain" | "snow" | "sand", intensity 0..1
        int __cdecl GetWeather(void* L)
        {
            const climate::Weather& w = climate::CurrentWeather();
            sc::PushString(L, WeatherName(w.type));
            sc::PushNumber(L, w.intensity);
            return 2;
        }

        // LivingAzeroth_GetField(name) -> value (nil if the field is unknown or has no value here)
        // Every world field by name ("temperature", "moisture", later ones too), at the player.
        int __cdecl GetField(void* L)
        {
            const world::Snapshot& s = world::Current();
            const char* name = sc::ArgCount(L) >= 1 && sc::IsString(L, 1) ? sc::ToString(L, 1) : nullptr;
            float value = 0.0f;
            const int field = name && std::strlen(name) < 64 ? fields::Find(name) : -1;
            if (!s.inWorld || field < 0 || !fields::Sample(field, s.playerPos, value)) { sc::PushNil(L); return 1; }
            sc::PushNumber(L, value);
            return 1;
        }

        // LivingAzeroth_GetFieldNames() -> name1, name2, ...
        int __cdecl GetFieldNames(void* L)
        {
            const int n = fields::Count();
            for (int f = 0; f < n; ++f) sc::PushString(L, fields::Name(f));
            return n;
        }
    }

    void Register()
    {
        luaapi::Add("LivingAzeroth_GetVersion", &GetVersion);
        luaapi::Add("LivingAzeroth_GetTemperature", &GetTemperature);
        luaapi::Add("LivingAzeroth_GetHumidity", &GetHumidity);
        luaapi::Add("LivingAzeroth_GetGroundMoisture", &GetGroundMoisture);
        luaapi::Add("LivingAzeroth_GetWeather", &GetWeather);
        luaapi::Add("LivingAzeroth_GetField", &GetField);
        luaapi::Add("LivingAzeroth_GetFieldNames", &GetFieldNames);
    }
}
