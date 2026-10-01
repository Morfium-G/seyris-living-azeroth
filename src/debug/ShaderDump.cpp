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
        constexpr int kGrassEntries = 6;

        // Terrain shader tables, filled by the terrain shader load (0x79E970 onward, verified in
        // XWorkbench 2026-10-02). Each table is an array of shader wrappers; how many are filled
        // depends on the GPU class (caps +0xC4) and PCF shadows (0xD43014), so the dump walks the
        // largest size and skips empty slots. Terrain1's table can be 0x20 long and then overlaps
        // Terrain1w's (+0x20 bytes) -- the dump drops wrappers it has already written.
        struct Table { const char* name; uintptr_t address; int entries; bool pixel; };
        constexpr Table kTerrainTables[] = {
            { "terrain_vs",       0x00CE0008, 0x80, false }, // "Terrain"
            { "terrain0_ps",      0x00CE0488, 3,    true  }, // "Terrain0"
            { "terrain0_env_ps",  0x00CE0004, 1,    true  }, // "Terrain0_env"
            { "terrain1_ps",      0x00CE0408, 0x20, true  }, // "Terrain1", "Terrain1w", "Terrain1w_1..4"
            { "terrain2_ps",      0x00CE0388, 0x20, true  }, // "Terrain2" / "Terrain2_pcf"
            { "terrain3_ps",      0x00CE0208, 0x60, true  }, // "Terrain3" / "Terrain3_pcf"
        };

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
        // a readable pointer (+0x50) whose first dword is a D3D9 version token (0xFFFExxxx for a
        // vertex shader, 0xFFFFxxxx for a pixel shader) -- part of the bytecode format itself, so a
        // wrong guess can't pass.
        bool IsShaderWrapper(uintptr_t obj, bool pixel)
        {
            uintptr_t len = 0, ptr = 0, token = 0;
            if (!obj) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(obj + sh::off::kCgxShaderByteLen), len)) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(obj + sh::off::kCgxShaderBytePtr), ptr)) return false;
            if (len < 8 || len >= (1u << 20) || !ptr) return false;
            if (!SafeReadPtr(reinterpret_cast<void*>(ptr), token)) return false;
            return (token & 0xFFFF0000u) == (pixel ? 0xFFFF0000u : 0xFFFE0000u);
        }

        // Diagnostic: what obj actually is, per its own RTTI, and its first 24 dwords.
        void Describe(const WXL_Api* api, const char* table, int entry, const char* label, uintptr_t obj)
        {
            uintptr_t vtbl = 0, locator = 0, typeDesc = 0;
            char name[48] = "?";
            if (SafeReadPtr(reinterpret_cast<void*>(obj), vtbl) && vtbl &&
                SafeReadPtr(reinterpret_cast<void*>(vtbl - 4), locator) && locator &&
                SafeReadPtr(reinterpret_cast<void*>(locator + 0x0C), typeDesc) && typeDesc)
                SafeCopy(name, reinterpret_cast<const void*>(typeDesc + kTypeDescriptorNameOffset), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';

            api->Log(WXL_LOG_INFO, kTag, "shader dump: %s %d %s 0x%08X: vtbl 0x%08X, locator 0x%08X, typeDesc 0x%08X, name \"%s\"",
                     table, entry, label, static_cast<unsigned>(obj), static_cast<unsigned>(vtbl),
                     static_cast<unsigned>(locator), static_cast<unsigned>(typeDesc), name);

            uint32_t d[24] = {};
            if (SafeCopy(d, reinterpret_cast<const void*>(obj), sizeof(d)))
                for (int row = 0; row < 3; ++row)
                    api->Log(WXL_LOG_INFO, kTag, "shader dump:   +0x%02X: %08X %08X %08X %08X %08X %08X %08X %08X",
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

        // The live D3D shader's own bytecode, or the wrapper's copy when it isn't created yet.
        std::vector<uint8_t> Bytecode(uintptr_t wrapper, bool pixel)
        {
            std::vector<uint8_t> code;
            uintptr_t handle = 0;
            SafeReadPtr(reinterpret_cast<void*>(wrapper + sh::off::kCgxShaderHandle), handle);
            if (handle)
            {
                UINT size = 0;
                const bool sized = pixel
                    ? SUCCEEDED(reinterpret_cast<IDirect3DPixelShader9*>(handle)->GetFunction(nullptr, &size))
                    : SUCCEEDED(reinterpret_cast<IDirect3DVertexShader9*>(handle)->GetFunction(nullptr, &size));
                if (sized && size > 0 && size < (1u << 20))
                {
                    code.resize(size);
                    const bool read = pixel
                        ? SUCCEEDED(reinterpret_cast<IDirect3DPixelShader9*>(handle)->GetFunction(code.data(), &size))
                        : SUCCEEDED(reinterpret_cast<IDirect3DVertexShader9*>(handle)->GetFunction(code.data(), &size));
                    if (!read) code.clear();
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
            return code;
        }

        // Dumps every filled slot of one wrapper table as <name>_<slot>.bin/.txt. `seen` collects
        // the wrappers already written, so overlapping tables don't write the same shader twice.
        int DumpTable(const WXL_Api* api, const Table& t, DisassembleFn disassemble, std::vector<uintptr_t>& seen)
        {
            int dumped = 0, empty = 0, duplicate = 0, bad = 0;
            for (int i = 0; i < t.entries; ++i)
            {
                uintptr_t wrapper = 0;
                if (!SafeReadPtr(reinterpret_cast<void*>(t.address + i * 4), wrapper) || !wrapper) { ++empty; continue; }
                if (!IsShaderWrapper(wrapper, t.pixel))
                {
                    // Maybe the table holds a pointer to the wrapper rather than the wrapper itself.
                    uintptr_t inner = 0;
                    if (SafeReadPtr(reinterpret_cast<void*>(wrapper), inner) && IsShaderWrapper(inner, t.pixel))
                    {
                        api->Log(WXL_LOG_INFO, kTag, "shader dump: %s %d holds a pointer to the wrapper (one extra hop).", t.name, i);
                        wrapper = inner;
                    }
                    else
                    {
                        if (bad++ == 0) // one detailed look is enough to learn the layout
                        {
                            Describe(api, t.name, i, "object", wrapper);
                            if (inner) Describe(api, t.name, i, "deref", inner);
                        }
                        continue;
                    }
                }
                bool already = false;
                for (uintptr_t w : seen) already |= w == wrapper;
                if (already) { ++duplicate; continue; }
                seen.push_back(wrapper);

                const std::vector<uint8_t> code = Bytecode(wrapper, t.pixel);
                if (code.empty())
                {
                    api->Log(WXL_LOG_WARN, kTag, "shader dump: %s %d has no handle and no bytecode (not created yet?).", t.name, i);
                    continue;
                }

                char path[MAX_PATH];
                std::snprintf(path, sizeof(path), "%s\\%s_%d.bin", kOutDir, t.name, i);
                WriteFile(path, code.data(), code.size());

                if (disassemble)
                {
                    ID3DBlob* text = nullptr;
                    if (SUCCEEDED(disassemble(code.data(), code.size(), 0, nullptr, &text)) && text)
                    {
                        std::snprintf(path, sizeof(path), "%s\\%s_%d.txt", kOutDir, t.name, i);
                        // The blob is NUL-terminated; drop the terminator from the file.
                        size_t n = text->GetBufferSize();
                        if (n && static_cast<const char*>(text->GetBufferPointer())[n - 1] == '\0') --n;
                        WriteFile(path, text->GetBufferPointer(), n);
                        text->Release();
                    }
                }
                ++dumped;
            }
            api->Log(WXL_LOG_INFO, kTag, "shader dump: %s (0x%08X x %d): %d written, %d empty, %d already written, %d not a shader.",
                     t.name, static_cast<unsigned>(t.address), t.entries, dumped, empty, duplicate, bad);
            return dumped;
        }

        int DumpTables(const WXL_Api* api, const Table* tables, size_t count)
        {
            CreateDirectoryA("Logs", nullptr);
            CreateDirectoryA(kOutDir, nullptr);
            const DisassembleFn disassemble = ResolveDisassemble();

            std::vector<uintptr_t> seen;
            int dumped = 0;
            for (size_t i = 0; i < count; ++i)
                dumped += DumpTable(api, tables[i], disassemble, seen);

            api->Log(WXL_LOG_INFO, kTag, "shader dump: %d shader(s) written to %s\\ (disassembler %s).",
                     dumped, kOutDir, disassemble ? "available" : "MISSING, .bin only");
            return dumped;
        }
    }

    int DumpGrassShaders(const WXL_Api* api)
    {
        const Table grass = { "grass_vs", ge::kVertexShaderTable, kGrassEntries, false };
        return DumpTables(api, &grass, 1);
    }

    int DumpTerrainShaders(const WXL_Api* api)
    {
        return DumpTables(api, kTerrainTables, sizeof(kTerrainTables) / sizeof(kTerrainTables[0]));
    }
}
