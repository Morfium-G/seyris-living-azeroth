// Addon Lua functions: the module's registry of global functions in the client's script context
// (the one FrameXML and addons run in).
//
// A feature adds a function with one line, before Install():
//     luaapi::Add("LivingAzeroth_GetSomething", &MyFunction);
// and the registry takes care of the rest:
//  - registering every function again whenever the client builds a script context (login screen,
//    world, every /reload), right before the interface loads, so addons see them in OnLoad already;
//  - letting the engine call them: it refuses script callbacks outside Wow.exe's own code, so the
//    pointer check is detoured to pass exactly the functions registered here (everything else
//    still goes through the client's own check).
//
// Every function here is reachable by any addon, trusted or not: read-only, numbers and strings
// only, arguments validated as if the caller were hostile (see the Lua exposure principle).
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::luaapi
{
    /// A script function: reads its arguments from `state`, pushes its results, returns how many.
    using Function = int(__cdecl*)(void* state);

    /// Adds a global function. Call before Install(); the name should start with "LivingAzeroth_".
    void Add(const char* name, Function function);

    /// Attaches the two detours. From WXL_Load (core arms detours once, after all loads).
    bool Install(const WXL_Api* api);

    /// How many functions are registered, and whether the hooks are in.
    unsigned Count();
    bool     Installed();
}
