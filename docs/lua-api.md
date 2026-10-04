# Lua functions for addons

The module adds read-only global functions to the client's addon Lua. They exist from the moment
the interface loads (OnLoad handlers can use them) and after every `/reload`. Without the module,
they're simply missing, so check before calling: `if LivingAzeroth_GetTemperature then ... end`.

Everything is about **where the player stands**. Values are `nil` outside the world or where
nothing is known yet.

| Function | Returns |
|---|---|
| `LivingAzeroth_GetVersion()` | the API version (now **1**). Raised when functions are added; existing ones never change what they return |
| `LivingAzeroth_GetTemperature()` | `ground, air` in °C. **air**: the place's climate (AreaClimate.cdbc: time of day, season, weather). **ground**: near the ground right there, warmed by lava and hot ground nearby |
| `LivingAzeroth_GetHumidity()` | the air's humidity, 0..1 (AreaClimate.cdbc) |
| `LivingAzeroth_GetGroundMoisture()` | `moisture, rest`, 0..1: how wet the ground is now, and where it rests when nothing wets or dries it |
| `LivingAzeroth_GetWeather()` | `"fine"`, `"rain"`, `"snow"` or `"sand"`, and the intensity 0..1 |
| `LivingAzeroth_GetField(name)` | any world field by name, or `nil`: `"temperature"` (°C, the ground's), `"moisture"` (0..1), `"snow"` (yd of snow lying there: fallen + what's left of painted snow; painted depth as on flat open ground), and every field added later |
| `LivingAzeroth_GetFieldNames()` | the names of all world fields, as multiple returns |

Example: `addons/LivingAzerothClimate` (temperature, humidity and weather under the minimap clock,
details on hover). Copy the folder into `Interface\AddOns\`.

## Adding a function (module code)

1. Write it in `src/lua/EnvironmentLua.cpp` (or a new file for another topic), as
   `int __cdecl Name(void* L)`: read arguments with `wxl::game::script` (`ArgCount`, `IsString`,
   `ToString`, `ToNumber`, ...), push results (`PushNumber`, `PushString`, `PushNil`, ...), and return
   how many it pushed.
2. Add one line to that file's `Register()`: `luaapi::Add("LivingAzeroth_Name", &Name);`.
3. Raise `kApiVersion` and add a row to the table above.

`src/lua/LuaApi` does the rest: it registers every function each time the client builds a script
context, and lets the engine's callback check pass exactly these functions. A new field in
`env/Fields` needs no new function at all: `LivingAzeroth_GetField` finds it by name.

Rules, because any addon can call these (trusted or not): **read-only**, **numbers and strings
only** (no pointers, no handles), **validate every argument** as if the caller were hostile, keep
pushed results few (Lua guarantees room for 20 values per call), and never change what an existing
function returns. Add a new one instead.
