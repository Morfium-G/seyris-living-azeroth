// The environment for addons: read-only Lua functions about where the player stands (temperature,
// humidity, ground moisture, weather, any world field by name). Documented in docs/lua-api.md.
#pragma once

namespace wxl_livingazeroth::envlua
{
    /// Adds the functions to luaapi. From WXL_Load, before luaapi::Install().
    void Register();
}
