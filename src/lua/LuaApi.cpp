#include "LuaApi.hpp"

#include "game/Script.hpp"

#include <string>
#include <vector>

namespace wxl_livingazeroth::luaapi
{
    namespace
    {
        namespace sc = wxl::game::script;
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        struct Entry { std::string name; Function function; };
        std::vector<Entry> g_functions;
        bool               g_installed = false;

        sc::ValidateCallbackFn g_origValidate = nullptr;
        sc::InterfaceLoadFn    g_origInterfaceLoad = nullptr;

        // The engine's check that a script callback lies inside Wow.exe: ours pass, the rest goes
        // to the client's own check unchanged.
        void __cdecl hkValidate(uintptr_t function)
        {
            for (const Entry& e : g_functions)
                if (reinterpret_cast<uintptr_t>(e.function) == function) return;
            g_origValidate(function);
        }

        // Right before an interface loads into a (new) script context: (re)register everything, so
        // the functions exist for that context's whole life, OnLoad handlers included.
        int __cdecl hkInterfaceLoad(const char* tocPath, const char* addOnName, void* md5Context, void* status)
        {
            if (sc::Context())
                for (const Entry& e : g_functions) sc::Register(e.name.c_str(), e.function);
            return g_origInterfaceLoad(tocPath, addOnName, md5Context, status);
        }
    }

    void Add(const char* name, Function function)
    {
        if (name && function) g_functions.push_back({ name, function });
    }

    bool Install(const WXL_Api* api)
    {
        if (g_functions.empty()) return true;
        const int validate = api->HookAttach("LivingAzeroth.LuaValidateCallback", sc::kValidateCallbackSeam,
                                             reinterpret_cast<void*>(&hkValidate), reinterpret_cast<void**>(&g_origValidate),
                                             WXL_HOOK_DEFAULT_PRIORITY);
        const int load = api->HookAttach("LivingAzeroth.LuaInterfaceLoad", sc::kInterfaceLoadSeam,
                                         reinterpret_cast<void*>(&hkInterfaceLoad), reinterpret_cast<void**>(&g_origInterfaceLoad),
                                         WXL_HOOK_DEFAULT_PRIORITY);
        g_installed = validate && load;
        api->Log(g_installed ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "lua: %u addon function(s) %s.",
                 static_cast<unsigned>(g_functions.size()), g_installed ? "ready" : "NOT available (hook failed)");
        return g_installed;
    }

    unsigned Count()     { return static_cast<unsigned>(g_functions.size()); }
    bool     Installed() { return g_installed; }
}
