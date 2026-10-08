#include "DepthCopy.hpp"

#include <windows.h>
#include <d3d9.h>

#include <climits>
#include <cwchar>

namespace wxl_livingazeroth::depthcopy
{
    namespace
    {
        constexpr unsigned  kNvidiaVendorId = 0x10DE;
        constexpr D3DFORMAT kResz = static_cast<D3DFORMAT>(MAKEFOURCC('R', 'E', 'S', 'Z'));
        constexpr DWORD     kReszCopyCode = 0x7FA05000;

        // nvapi_QueryInterface ids from NVIDIA's public headers.
        constexpr unsigned kNvInitialize = 0x0150E828;
        constexpr unsigned kNvRegister   = 0xA064BDFC;
        constexpr unsigned kNvUnregister = 0xBB2B17AA;
        constexpr unsigned kNvStretchEx  = 0x22DE03AA;
        constexpr int      kNvOk = 0;
        constexpr int      kNvFaulted = INT_MIN;

        using NvQueryFn    = void*(__cdecl*)(unsigned);
        using NvInitFn     = int(__cdecl*)();
        using NvResourceFn = int(__cdecl*)(IDirect3DResource9*);
        using NvStretchFn  = int(__cdecl*)(IDirect3DDevice9*, IDirect3DResource9*, const RECT*, IDirect3DResource9*,
                                           const RECT*, D3DTEXTUREFILTERTYPE);

        struct Nvapi
        {
            bool         loaded = false;
            const char*  why = "";
            NvResourceFn reg = nullptr, unreg = nullptr;
            NvStretchFn  stretch = nullptr;
        };

        int GuardedInit(NvInitFn f)
        {
            __try { return f(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return kNvFaulted; }
        }

        int GuardedResource(NvResourceFn f, IDirect3DResource9* r)
        {
            __try { return f(r); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return kNvFaulted; }
        }

        int GuardedStretch(NvStretchFn f, IDirect3DDevice9* dev, IDirect3DResource9* src, IDirect3DResource9* dst)
        {
            __try { return f(dev, src, nullptr, dst, nullptr, D3DTEXF_POINT); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return kNvFaulted; }
        }

        const Nvapi& LoadNvapi()
        {
            static Nvapi nv;
            static bool tried = false;
            if (tried) return nv;
            tried = true;
            HMODULE lib = LoadLibraryExW(L"nvapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            auto query = lib ? reinterpret_cast<NvQueryFn>(GetProcAddress(lib, "nvapi_QueryInterface")) : nullptr;
            if (!query) { nv.why = "nvapi.dll is missing"; return nv; }
            auto init = reinterpret_cast<NvInitFn>(query(kNvInitialize));
            if (!init || GuardedInit(init) != kNvOk) { nv.why = "NvAPI_Initialize failed"; return nv; }
            nv.reg     = reinterpret_cast<NvResourceFn>(query(kNvRegister));
            nv.unreg   = reinterpret_cast<NvResourceFn>(query(kNvUnregister));
            nv.stretch = reinterpret_cast<NvStretchFn>(query(kNvStretchEx));
            if (!nv.reg || !nv.unreg || !nv.stretch) { nv.why = "this NVAPI has no D3D9 depth copy"; return nv; }
            nv.loaded = true;
            return nv;
        }

        // NVAPI only copies the depth of the system d3d9.dll's device, not a wrapper's (DXVK etc.).
        bool FromSystemD3D9(IDirect3DDevice9* dev)
        {
            const void* firstMethod = (*reinterpret_cast<void* const* const*>(dev))[0];
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(firstMethod), &module))
                return false;
            wchar_t path[MAX_PATH] = {}, folder[MAX_PATH] = {};
            if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
            const wchar_t* slash = std::wcsrchr(path, L'\\');
            if (!slash) return false;
            const size_t length = static_cast<size_t>(slash - path);
            auto same = [&](UINT n) { return n == length && _wcsnicmp(path, folder, length) == 0; };
            return same(GetSystemDirectoryW(folder, MAX_PATH)) || same(GetSystemWow64DirectoryW(folder, MAX_PATH));
        }

        Method             g_method = Method::None;
        IDirect3DSurface9* g_source = nullptr;
        IDirect3DTexture9* g_destination = nullptr;

        bool ResolveThroughResz(IDirect3DDevice9* dev)
        {
            // Everything the trigger touches is put back by the state block (sampler 0, shaders,
            // vertex format, stream 0, the four render states), so the client's own cache stays true.
            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return false;
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(D3DFVF_XYZ);
            dev->SetTexture(0, g_destination);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
            const float point[3] = {};
            const bool drawn = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_POINTLIST, 1, point, sizeof(point)));
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
            dev->SetRenderState(D3DRS_POINTSIZE, kReszCopyCode); // the resolve happens here
            saved->Apply();
            saved->Release();
            return drawn;
        }
    }

    const char* MethodName(Method m)
    {
        switch (m)
        {
            case Method::Nvapi: return "NVAPI";
            case Method::Resz:  return "RESZ";
            default:            return "none";
        }
    }

    Method Probe(IDirect3DDevice9* dev, const char*& whyNone)
    {
        whyNone = "";
        IDirect3D9* d3d = nullptr;
        if (!dev || FAILED(dev->GetDirect3D(&d3d)) || !d3d) { whyNone = "no Direct3D object"; return Method::None; }
        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        const D3DFORMAT adapterFormat = SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode)) ? mode.Format : D3DFMT_X8R8G8B8;
        const bool resz = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapterFormat,
                                                           D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, kResz));
        D3DADAPTER_IDENTIFIER9 id{};
        const bool nvidia = SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)) && id.VendorId == kNvidiaVendorId;
        d3d->Release();

        if (resz) return Method::Resz;
        if (!nvidia) { whyNone = "the driver has no RESZ resolve and the adapter isn't NVIDIA"; return Method::None; }
        if (!FromSystemD3D9(dev)) { whyNone = "d3d9 isn't the system copy (DXVK or another wrapper): NVAPI can't copy its depth"; return Method::None; }
        const Nvapi& nv = LoadNvapi();
        if (!nv.loaded) { whyNone = nv.why; return Method::None; }
        return Method::Nvapi;
    }

    bool Attach(Method m, IDirect3DSurface9* source, IDirect3DTexture9* destination)
    {
        Detach();
        if (m == Method::None || !source || !destination) return false;
        if (m == Method::Nvapi)
        {
            const Nvapi& nv = LoadNvapi();
            if (!nv.loaded) return false;
            const int a = GuardedResource(nv.reg, source);
            const int b = GuardedResource(nv.reg, destination);
            if (a != kNvOk || b != kNvOk)
            {
                if (a == kNvOk) GuardedResource(nv.unreg, source);
                if (b == kNvOk) GuardedResource(nv.unreg, destination);
                return false;
            }
        }
        source->AddRef();
        destination->AddRef();
        g_method = m;
        g_source = source;
        g_destination = destination;
        return true;
    }

    void Detach()
    {
        if (g_method == Method::Nvapi)
        {
            const Nvapi& nv = LoadNvapi();
            GuardedResource(nv.unreg, g_destination);
            GuardedResource(nv.unreg, g_source);
        }
        g_method = Method::None;
        if (g_source) { g_source->Release(); g_source = nullptr; }
        if (g_destination) { g_destination->Release(); g_destination = nullptr; }
    }

    bool Attached() { return g_method != Method::None; }

    bool Copy(IDirect3DDevice9* dev)
    {
        if (!dev) return false;
        switch (g_method)
        {
            case Method::Nvapi:
                return GuardedStretch(LoadNvapi().stretch, dev, g_source, g_destination) == kNvOk;
            case Method::Resz:
            {
                IDirect3DSurface9* bound = nullptr;
                if (FAILED(dev->GetDepthStencilSurface(&bound))) return false;
                const bool sourceBound = bound == g_source;
                if (bound) bound->Release();
                return sourceBound && ResolveThroughResz(dev);
            }
            default:
                return false;
        }
    }
}
