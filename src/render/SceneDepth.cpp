#include "SceneDepth.hpp"

#include "DepthCopy.hpp"

#include "game/Gx.hpp"

#include <d3d9.h>

namespace wxl_livingazeroth::depth
{
    namespace
    {
        namespace gx = wxl::game::gx;

        constexpr D3DFORMAT kFormatIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        const WXL_Api* g_api = nullptr;

        bool  g_enabled  = true;
        State g_state    = State::NotStarted;
        int   g_intzSupported = -1; // -1 unknown, 0 no, 1 yes; asked once per device

        IDirect3DTexture9* g_texture  = nullptr;
        IDirect3DSurface9* g_surface  = nullptr;
        IDirect3DSurface9* g_original = nullptr; // one reference, owned by us while swapped

        // The automatic surface we last declined to swap (MSAA, create failure), so the reason is
        // logged once instead of every frame. Compared by address only, never dereferenced.
        const void* g_declinedFor = nullptr;

        unsigned g_width = 0, g_height = 0, g_originalFormat = 0, g_swapCount = 0;

        // Copied mode (multisampling on): the method probed for this device (-1 = not yet), why none.
        int         g_probedMethod = -1;
        const char* g_copyWhy = "";
        unsigned    g_copiesDone = 0, g_copiesFailed = 0;

        gx::off::GxDevice* GraphicsDevice()
        {
            return static_cast<gx::off::GxDevice*>(gx::RawGraphicsDevice());
        }

        IDirect3DDevice9* D3DDevice()
        {
            return static_cast<IDirect3DDevice9*>(gx::RawDevice());
        }

        bool IntzSupported(IDirect3DDevice9* dev)
        {
            if (g_intzSupported >= 0) return g_intzSupported == 1;

            IDirect3D9* d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DDISPLAYMODE mode{};
            bool ok = false;
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
            {
                dev->GetCreationParameters(&cp);
                d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
                ok = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                      D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kFormatIntz));
                d3d->Release();
            }
            g_intzSupported = ok ? 1 : 0;
            g_api->Log(ok ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "depth: INTZ %s by the driver.",
                       ok ? "supported" : "NOT supported, readable depth disabled");
            return ok;
        }

        /// Binds `to` as depth, but only if `from` is what's bound now. Leaves any other pass's
        /// depth (portraits, minimap) alone.
        void RebindIfBound(IDirect3DDevice9* dev, IDirect3DSurface9* from, IDirect3DSurface9* to)
        {
            IDirect3DSurface9* bound = nullptr;
            if (FAILED(dev->GetDepthStencilSurface(&bound))) return;
            if (bound == from) dev->SetDepthStencilSurface(to);
            if (bound) bound->Release();
        }

        void ReleaseOurs()
        {
            depthcopy::Detach();
            if (g_surface) { g_surface->Release(); g_surface = nullptr; }
            if (g_texture) { g_texture->Release(); g_texture = nullptr; }
        }

        /// Puts the client's own surface back into its cache field and drops ours.
        void Restore()
        {
            auto* gd  = GraphicsDevice();
            auto* dev = D3DDevice();

            if (gd && g_surface && gd->depthSurface == g_surface)
            {
                gd->depthSurface = g_original; // our reference on the original becomes the field's
                g_original = nullptr;
                g_surface->Release();          // the field's reference on ours
                if (dev) RebindIfBound(dev, g_surface, static_cast<IDirect3DSurface9*>(gd->depthSurface));
            }
            if (g_original) { g_original->Release(); g_original = nullptr; }
            ReleaseOurs();
        }

        void Decline(State why, const void* forSurface, const char* message)
        {
            g_state = why;
            if (g_declinedFor == forSurface) return;
            g_declinedFor = forSurface;
            g_api->Log(WXL_LOG_WARN, kTag, "depth: %s", message);
        }

        /// Multisampled world depth: leave it bound and copy it into our INTZ texture on demand.
        void SetUpCopy(IDirect3DDevice9* dev, IDirect3DSurface9* current, const D3DSURFACE_DESC& desc)
        {
            if (g_probedMethod < 0)
            {
                const depthcopy::Method m = depthcopy::Probe(dev, g_copyWhy);
                g_probedMethod = static_cast<int>(m);
                g_api->Log(m != depthcopy::Method::None ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "depth: multisampled depth copy: %s%s%s.",
                           depthcopy::MethodName(m), m == depthcopy::Method::None ? ", " : "", g_copyWhy);
            }
            const auto method = static_cast<depthcopy::Method>(g_probedMethod);
            if (method == depthcopy::Method::None)
            {
                Decline(State::Multisampled, current, "multisampling is on and no depth copy method works; readable depth is off.");
                return;
            }
            if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kFormatIntz,
                                          D3DPOOL_DEFAULT, &g_texture, nullptr)) || !g_texture)
            {
                g_texture = nullptr;
                Decline(State::CreateFailed, current, "creating the INTZ copy texture failed.");
                return;
            }
            if (!depthcopy::Attach(method, current, g_texture))
            {
                ReleaseOurs();
                g_copyWhy = "the copy method refused the depth surfaces";
                Decline(State::Multisampled, current, "the depth copy method refused the multisampled surface; readable depth is off.");
                return;
            }
            g_original = current; // our own reference: the client's field keeps its own
            g_original->AddRef();
            g_width = desc.Width;
            g_height = desc.Height;
            g_declinedFor = nullptr;
            g_state = State::Copied;
            ++g_swapCount;
            g_api->Log(WXL_LOG_INFO, kTag, "depth: multisampled world depth (%ux%u, %u samples) is copied into INTZ by %s (setup #%u).",
                       g_width, g_height, static_cast<unsigned>(desc.MultiSampleType), depthcopy::MethodName(method), g_swapCount);
        }

        void Swap(gx::off::GxDevice* gd, IDirect3DDevice9* dev)
        {
            auto* current = static_cast<IDirect3DSurface9*>(gd->depthSurface);
            if (!current || current == g_surface) return;
            if (current == g_declinedFor) return;
            if (g_state == State::Copied && current == g_original) return;

            // A new automatic surface (first frame, or re-cached after a reset). Anything we still
            // hold belongs to the old device state.
            if (g_original) { g_original->Release(); g_original = nullptr; }
            ReleaseOurs();

            if (!IntzSupported(dev)) { g_state = State::Unsupported; g_declinedFor = current; return; }

            D3DSURFACE_DESC desc{};
            if (FAILED(current->GetDesc(&desc))) return;
            g_originalFormat = static_cast<unsigned>(desc.Format);

            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            {
                SetUpCopy(dev, current, desc);
                return;
            }

            if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kFormatIntz,
                                          D3DPOOL_DEFAULT, &g_texture, nullptr)) || !g_texture)
            {
                g_texture = nullptr;
                Decline(State::CreateFailed, current, "creating the INTZ depth texture failed.");
                return;
            }
            g_texture->GetSurfaceLevel(0, &g_surface);

            // The field's reference on the automatic surface becomes ours; the field gets its own
            // reference on our surface, which the client releases itself before the next Reset.
            g_original = current;
            g_surface->AddRef();
            gd->depthSurface = g_surface;

            // The client only rebinds depth when its render target changes, so bind ours now too.
            RebindIfBound(dev, g_original, g_surface);

            g_width = desc.Width;
            g_height = desc.Height;
            g_declinedFor = nullptr;
            g_state = State::Active;
            ++g_swapCount;
            g_api->Log(WXL_LOG_INFO, kTag, "depth: world depth is now readable (INTZ %ux%u, replaced format %u, swap #%u).",
                       g_width, g_height, g_originalFormat, g_swapCount);
        }
    }

    void Init(const WXL_Api* api) { g_api = api; }

    void Update()
    {
        auto* gd  = GraphicsDevice();
        auto* dev = D3DDevice();
        if (!gd || !dev) return;

        if (!g_enabled)
        {
            if (g_surface || g_original) Restore();
            g_state = State::Disabled;
            return;
        }
        Swap(gd, dev);
    }

    void OnDeviceLost()
    {
        auto* gd  = GraphicsDevice();
        auto* dev = D3DDevice();

        // Normally the client has already released and cleared its field before calling Reset. If a
        // reset path ever skips that, hand the field its original surface back instead of leaving a
        // pointer to a texture we're about to free.
        if (gd && g_surface && gd->depthSurface == g_surface)
        {
            gd->depthSurface = g_original;
            g_original = nullptr;
            g_surface->Release();
        }
        if (dev && g_surface)
            RebindIfBound(dev, g_surface, gd ? static_cast<IDirect3DSurface9*>(gd->depthSurface) : nullptr);

        if (g_original) { g_original->Release(); g_original = nullptr; }
        ReleaseOurs();

        g_intzSupported = -1;
        g_probedMethod = -1;
        g_declinedFor = nullptr;
        if (g_state == State::Active || g_state == State::Copied) g_state = State::NotStarted;
    }

    void SetEnabled(bool enabled) { g_enabled = enabled; }
    bool Enabled() { return g_enabled; }
    State CurrentState() { return g_state; }

    bool Refresh(void* device)
    {
        if (g_state == State::Active) return true;
        if (g_state != State::Copied) return false;
        const bool ok = depthcopy::Copy(static_cast<IDirect3DDevice9*>(device));
        ++(ok ? g_copiesDone : g_copiesFailed);
        return ok;
    }

    void* Texture()         { return g_state == State::Active || g_state == State::Copied ? g_texture : nullptr; }
    void* Surface()         { return g_state == State::Active ? g_surface : nullptr; }
    void* OriginalSurface() { return g_state == State::Active || g_state == State::Copied ? g_original : nullptr; }

    const char* CopyMethod()      { return g_probedMethod >= 0 ? depthcopy::MethodName(static_cast<depthcopy::Method>(g_probedMethod)) : "not probed"; }
    unsigned    CopiesDone()      { return g_copiesDone; }
    unsigned    CopiesFailed()    { return g_copiesFailed; }
    const char* CopyUnavailable() { return g_copyWhy; }

    unsigned Width()          { return g_width; }
    unsigned Height()         { return g_height; }
    unsigned OriginalFormat() { return g_originalFormat; }
    unsigned SwapCount()      { return g_swapCount; }
}
