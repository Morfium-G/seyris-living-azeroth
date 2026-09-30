#include "ShaderPatch.hpp"

#include "game/Shader.hpp"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstring>

namespace wxl_livingazeroth::shaderpatch
{
    namespace
    {
        namespace sh = wxl::game::shader;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // D3DAssemble is exported by d3dcompiler_47 but not declared in the SDK header.
        using AssembleFn    = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                               UINT, ID3DBlob**, ID3DBlob**);
        using DisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

        struct CachedBlob { uint32_t stockLength; std::vector<uint8_t> code; };

        struct Rule
        {
            VertexRule              def;
            RuleStatus              status;
            std::vector<CachedBlob> cache; // one patched blob per stock length, kept for the process
        };

        const WXL_Api*    g_api = nullptr;
        std::vector<Rule> g_rules;
        sh::off::ShaderCreateVertexFn g_origCreate = nullptr;

        HMODULE Compiler()
        {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            return m ? m : LoadLibraryA("d3dcompiler_47.dll");
        }

        Rule* Match(const uint8_t* code, uint32_t length)
        {
            if (!code || length < 8) return nullptr;
            uint32_t token;
            std::memcpy(&token, code, sizeof(token));
            for (Rule& r : g_rules)
            {
                if (r.def.versionToken != token) continue;
                for (uint32_t l : r.def.stockLengths)
                    if (l == length) return &r;
            }
            return nullptr;
        }

        void Fail(Rule& r, const char* what, const char* detail)
        {
            ++r.status.failed;
            r.status.lastError = std::string(what) + (detail ? std::string(": ") + detail : std::string());
            g_api->Log(WXL_LOG_WARN, kTag, "shader patch '%s': %s", r.def.name, r.status.lastError.c_str());
        }

        const std::vector<uint8_t>* BuildPatched(Rule& r, const uint8_t* stock, uint32_t length)
        {
            for (const CachedBlob& c : r.cache)
                if (c.stockLength == length) return &c.code;

            std::string src, error;
            if (!Disassemble(stock, length, src, error)) { Fail(r, error.c_str(), nullptr); return nullptr; }

            // Prologue after the version line; body after the anchor line.
            size_t versionEnd = src.find('\n');
            if (versionEnd == std::string::npos) { Fail(r, "no version line", nullptr); return nullptr; }
            src.insert(versionEnd + 1, r.def.prologue);

            size_t at = src.find(r.def.anchor);
            if (at == std::string::npos) { Fail(r, "anchor not found", r.def.anchor); return nullptr; }
            at = src.find('\n', at);
            if (at == std::string::npos) { Fail(r, "anchor has no line end", nullptr); return nullptr; }
            src.insert(at + 1, r.def.body);

            std::vector<uint8_t> code;
            if (!Assemble(src, r.def.name, code, error)) { Fail(r, error.c_str(), nullptr); return nullptr; }

            g_api->Log(WXL_LOG_INFO, kTag, "shader patch '%s': patched stock shader (%u -> %u bytes).",
                       r.def.name, length, static_cast<unsigned>(code.size()));
            r.cache.push_back({ length, std::move(code) });
            return &r.cache.back().code;
        }

        // The client's per-shader create: swap the wrapper's bytecode to the patched version for this
        // one call, then put the stock pointer/length back so the wrapper stays exactly as it was.
        void __fastcall hkCreateVertex(void* device, void* edx, void* wrapper)
        {
            if (wrapper)
            {
                auto* base    = static_cast<uint8_t*>(wrapper);
                auto* lenPtr  = reinterpret_cast<uint32_t*>(base + sh::off::kCgxShaderByteLen);
                auto* codePtr = reinterpret_cast<const uint8_t**>(base + sh::off::kCgxShaderBytePtr);

                if (Rule* r = Match(*codePtr, *lenPtr))
                {
                    if (const std::vector<uint8_t>* patched = BuildPatched(*r, *codePtr, *lenPtr))
                    {
                        const uint8_t* savedCode = *codePtr;
                        const uint32_t savedLen  = *lenPtr;
                        *codePtr = patched->data();
                        *lenPtr  = static_cast<uint32_t>(patched->size());
                        g_origCreate(device, edx, wrapper);
                        *codePtr = savedCode;
                        *lenPtr  = savedLen;
                        ++r->status.applied;
                        return;
                    }
                }
            }
            g_origCreate(device, edx, wrapper);
        }
    }

    void Register(VertexRule rule)
    {
        Rule r;
        r.status.name = rule.name;
        r.def = std::move(rule);
        g_rules.push_back(std::move(r));
    }

    bool Install(const WXL_Api* api)
    {
        g_api = api;
        if (g_rules.empty()) return true;
        const int ok = api->HookAttach("LivingAzeroth.ShaderCreateVertex", sh::off::kShaderCreateVertex,
                                       reinterpret_cast<void*>(&hkCreateVertex),
                                       reinterpret_cast<void**>(&g_origCreate), WXL_HOOK_DEFAULT_PRIORITY);
        api->Log(ok ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "shader patch: create hook %s (%u rule(s)).",
                 ok ? "installed" : "FAILED to install", static_cast<unsigned>(g_rules.size()));
        return ok != 0;
    }

    std::vector<RuleStatus> Status()
    {
        std::vector<RuleStatus> out;
        for (const Rule& r : g_rules) out.push_back(r.status);
        return out;
    }

    bool Disassemble(const void* code, size_t length, std::string& text, std::string& error)
    {
        HMODULE comp = Compiler();
        auto disassemble = comp ? reinterpret_cast<DisassembleFn>(GetProcAddress(comp, "D3DDisassemble")) : nullptr;
        if (!disassemble) { error = "d3dcompiler_47 missing D3DDisassemble"; return false; }

        ID3DBlob* blob = nullptr;
        if (FAILED(disassemble(code, length, 0, nullptr, &blob)) || !blob) { error = "disassembly failed"; return false; }
        text.assign(static_cast<const char*>(blob->GetBufferPointer()));
        blob->Release();
        return true;
    }

    bool Assemble(const std::string& source, const char* name, std::vector<uint8_t>& code, std::string& error)
    {
        HMODULE comp = Compiler();
        auto assemble = comp ? reinterpret_cast<AssembleFn>(GetProcAddress(comp, "D3DAssemble")) : nullptr;
        if (!assemble) { error = "d3dcompiler_47 missing D3DAssemble"; return false; }

        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = assemble(source.c_str(), source.size(), name, nullptr, nullptr, 0, &blob, &errors);
        if (FAILED(hr) || !blob)
        {
            error = std::string("assembly failed: ") + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            if (errors) errors->Release();
            if (blob) blob->Release();
            return false;
        }
        if (errors) errors->Release();
        const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
        code.assign(bytes, bytes + blob->GetBufferSize());
        blob->Release();
        return true;
    }
}
