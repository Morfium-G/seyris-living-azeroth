// Readable scene depth: swaps the world's depth-stencil surface for an INTZ texture, so later passes
// (fog, water, puddles, lighting) can sample the depth the world was drawn with.
//
// How the client holds its depth (disassembly, see orchestration/docs/r&d/immersion/
// rendering-foundation.md): it never creates the world depth buffer itself. It uses D3D9's automatic
// depth-stencil and caches the pointer on the graphics-device object (GxDevice::depthSurface). Its
// own render-target bind reads that field for every world pass, so writing our surface there
// redirects the whole world pass with no D3D hooks. After every device reset the client re-caches
// the automatic surface, which Update() notices and swaps again.
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::depth
{
    /// Why the swap is not active, for the debug panel.
    enum class State
    {
        NotStarted,     // no device yet
        Active,         // our INTZ surface is the world depth
        Disabled,       // turned off from the panel
        Unsupported,    // driver has no INTZ
        Multisampled,   // MSAA is on; a non-MSAA INTZ surface can't pair with a multisampled target
        CreateFailed,   // CreateTexture refused
    };

    void Init(const WXL_Api* api);

    /// Once per frame, between frames (OnFrame): performs or re-performs the swap as needed.
    void Update();

    /// Before IDirect3DDevice9::Reset: releases everything we own (all D3DPOOL_DEFAULT).
    void OnDeviceLost();

    void SetEnabled(bool enabled);
    bool Enabled();
    State CurrentState();

    /// The readable depth texture (IDirect3DTexture9*), or null while not Active.
    void* Texture();
    /// Our depth surface (IDirect3DSurface9*), or null while not Active.
    void* Surface();
    /// The client's automatic depth surface we replaced. Bind this while sampling Texture(), since a
    /// surface can't be read while it is also the bound depth target.
    void* OriginalSurface();

    /// Details for the debug panel.
    unsigned Width();
    unsigned Height();
    unsigned OriginalFormat();   // D3DFORMAT of the automatic surface
    unsigned SwapCount();        // how many times the swap has been (re)applied, e.g. after resets
}
