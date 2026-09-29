#include "ShaderDump.hpp"

#include "game/Shader.hpp"
#include "offsets/game/GroundEffect.hpp"

#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        namespace sh = wxl::game::shader;
        namespace ge = wxl::offsets::game::groundeffect;

        constexpr const char* kTag = "wxl-seyris-living-azeroth";
        constexpr const char* kOutDir = "Logs\\living-azeroth";

        // Grass vertex-shader table: 2 groups x 3 shadow variants (core GroundEffect.hpp).
        constexpr int kTableEntries = 6;

        using DisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

        DisassembleFn ResolveDisassemble()
        {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            if (!m) m = LoadLibraryA("d3dcompiler_47.dll");
            return m ? reinterpret_cast<DisassembleFn>(GetProcAddress(m, "D3DDisassemble")) : nullptr;
        }

        // Reads a pointer-sized value, or returns false if the address isn't readable. Keeps a wrong
        // guess about a table's layout from crashing the client.
        bool SafeReadPtr(const void* address, uintptr_t& out)
        {
            __try { out = *static_cast<const uintptr_t*>(address); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool SafeCopy(void* dst, const void* src, size_t len)
        {
            __try { std::memcpy(dst, src, len); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Core's kCgxShaderRtti is the decorated NAME (".?AVCGxShader@@", 0xAD8CA8), 8 bytes into
        // its type descriptor. Only used by the diagnostic below.
        constexpr uintptr_t kTypeDescriptorNameOffset = 8;

        // Structural check, not RTTI: this client's shader-wrapper vtable has no RTTI locator in
        // front of it (confirmed 2026-09-30: vtbl[-1] is a code address), so a type-descriptor
        // compare can never match. Instead the wrapper must hold a sane bytecode length (+0x4C) and
        // a readable pointer (+0x50) whose first dword is a D3D9 vertex-shader version token
        // (0xFFFExxxx) -- part of the bytecode format itself, so a wrong guess can't pass.
        bool IsShaderWrapper(uintptr_t obj)
        {
            uintptr_t len = 0, ptr = 0, token = 0;
            if (!obj) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(obj + sh::off::kCgxShaderByteLen), len)) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(obj + sh::off::kCgxShaderBytePtr), ptr)) return false;
            if (len < 8 || len >= (1u << 20) || !ptr) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(ptr), token)) return false;
            return (token & 0xFFFF0000u) == 0xFFFE0000u;
        }

        // Diagnostic: what obj actually is, per its own RTTI, and its first 24 dwords.
        void Describe(const WXL_Api* api, int entry, const char* label, uintptr_t obj)
        {
            uintptr_t vtbl = 0, locator = 0, typeDesc = 0;
            char name[48] = "?";
            if (SafeReadPtr(reinterpret_cast<void*>(obj), vtbl) && vtbl &&
                SafeReadPtr(reinterpret_cast<void*>(vtbl - 4), locator) && locator &&
                SafeReadPtr(reinterpret_cast<void*>(locator + 0x0C), typeDesc) && typeDesc)
                SafeCopy(name, reinterpret_cast<const void*>(typeDesc + kTypeDescriptorNameOffset), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';

            api->Log(WXL_LOG_INFO, kTag, "grass dump: entry %d %s 0x%08X: vtbl 0x%08X, locator 0x%08X, typeDesc 0x%08X, name \"%s\"",
                     entry, label, static_cast<unsigned>(obj), static_cast<unsigned>(vtbl),
                     static_cast<unsigned>(locator), static_cast<unsigned>(typeDesc), name);

            uint32_t d[24] = {};
            if (SafeCopy(d, reinterpret_cast<const void*>(obj), sizeof(d)))
                for (int row = 0; row < 3; ++row)
                    api->Log(WXL_LOG_INFO, kTag, "grass dump:   +0x%02X: %08X %08X %08X %08X %08X %08X %08X %08X",
                             row * 32, d[row * 8 + 0], d[row * 8 + 1], d[row * 8 + 2], d[row * 8 + 3],
                             d[row * 8 + 4], d[row * 8 + 5], d[row * 8 + 6], d[row * 8 + 7]);
        }

        bool WriteFile(const char* path, const void* data, size_t size)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, path, "wb") != 0 || !f) return false;
            std::fwrite(data, 1, size, f);
            std::fclose(f);
            return true;
        }
    }

    int DumpGrassShaders(const WXL_Api* api)
    {
        CreateDirectoryA("Logs", nullptr);
        CreateDirectoryA(kOutDir, nullptr);
        const DisassembleFn disassemble = ResolveDisassemble();

        int dumped = 0;
        for (int i = 0; i < kTableEntries; ++i)
        {
            uintptr_t wrapper = 0;
            if (!SafeReadPtr(reinterpret_cast<void*>(ge::kVertexShaderTable + i * 4), wrapper) || !wrapper)
            {
                api->Log(WXL_LOG_INFO, kTag, "grass dump: entry %d empty.", i);
                continue;
            }
            if (!IsShaderWrapper(wrapper))
            {
                // Maybe the table holds a pointer to the wrapper rather than the wrapper itself.
                uintptr_t inner = 0;
                if (SafeReadPtr(reinterpret_cast<void*>(wrapper), inner) && IsShaderWrapper(inner))
                {
                    api->Log(WXL_LOG_INFO, kTag, "grass dump: entry %d holds a pointer to the wrapper (one extra hop).", i);
                    wrapper = inner;
                }
                else
                {
                    api->Log(WXL_LOG_WARN, kTag, "grass dump: entry %d (0x%08X) is not a shader wrapper (no valid bytecode), skipped.",
                             i, static_cast<unsigned>(wrapper));
                    if (i == 0) // one detailed look is enough to learn the layout
                    {
                        Describe(api, i, "object", wrapper);
                        if (inner) Describe(api, i, "deref", inner);
                    }
                    continue;
                }
            }

            // Prefer the live D3D shader's own bytecode; fall back to the wrapper's copy.
            std::vector<uint8_t> code;
            uintptr_t handle = 0;
            SafeReadPtr(reinterpret_cast<void*>(wrapper + sh::off::kCgxShaderHandle), handle);
            if (handle)
            {
                auto* vs = reinterpret_cast<IDirect3DVertexShader9*>(handle);
                UINT size = 0;
                if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size > 0 && size < (1u << 20))
                {
                    code.resize(size);
                    if (FAILED(vs->GetFunction(code.data(), &size))) code.clear();
                }
            }
            if (code.empty())
            {
                uintptr_t len = 0, ptr = 0;
                SafeReadPtr(reinterpret_cast<void*>(wrapper + sh::off::kCgxShaderByteLen), len);
                SafeReadPtr(reinterpret_cast<void*>(wrapper + sh::off::kCgxShaderBytePtr), ptr);
                if (ptr && len > 0 && len < (1u << 20))
                {
                    code.resize(len);
                    if (!SafeCopy(code.data(), reinterpret_cast<const void*>(ptr), len)) code.clear();
                }
            }
            if (code.empty())
            {
                api->Log(WXL_LOG_WARN, kTag, "grass dump: entry %d has no handle and no bytecode (not created yet?).", i);
                continue;
            }

            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\grass_vs_%d.bin", kOutDir, i);
            WriteFile(path, code.data(), code.size());

            if (disassemble)
            {
                ID3DBlob* text = nullptr;
                if (SUCCEEDED(disassemble(code.data(), code.size(), 0, nullptr, &text)) && text)
                {
                    std::snprintf(path, sizeof(path), "%s\\grass_vs_%d.txt", kOutDir, i);
                    // The blob is NUL-terminated; drop the terminator from the file.
                    size_t n = text->GetBufferSize();
                    if (n && static_cast<const char*>(text->GetBufferPointer())[n - 1] == '\0') --n;
                    WriteFile(path, text->GetBufferPointer(), n);
                    text->Release();
                }
            }

            api->Log(WXL_LOG_INFO, kTag, "grass dump: entry %d -> %zu bytes (version token 0x%08X).",
                     i, code.size(), code.size() >= 4 ? *reinterpret_cast<const uint32_t*>(code.data()) : 0u);
            ++dumped;
        }

        api->Log(WXL_LOG_INFO, kTag, "grass dump: %d shader(s) written to %s\\ (disassembler %s).",
                 dumped, kOutDir, disassemble ? "available" : "MISSING, .bin only");
        return dumped;
    }
}
