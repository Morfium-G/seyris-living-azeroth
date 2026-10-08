// Readable scene depth: swaps the world's depth-stencil surface for an INTZ texture, so later passes
// (fog, water, puddles, lighting) can sample the depth the world was drawn with.
//
// How the client holds its depth (disassembly, see orchestration/docs/r&d/immersion/
// rendering-foundation.md): it never creates the world depth buffer itself. It uses D3D9's automatic
// depth-stencil and caches the pointer on the graphics-device object (GxDevice::depthSurface). Its
// own render-target bind reads that field for every world pass, so writing our surface there
// redirects the whole world pass with no D3D hooks. After every device reset the client re-caches
// the automatic surface, which Update() notices and swaps again.
//
// With the client's multisampling on, the automatic surface is multisampled and INTZ can't be: then
// the client keeps its surface, and Refresh() copies it into our INTZ texture where a pass needs it
// (render/DepthCopy: NVAPI or the driver's RESZ resolve).
#pragma once

#include "wxl/PluginApi.h"

namespace wxl_livingazeroth::depth
{
    /// Why the swap is not active, for the debug panel.
    enum class State
    {
        NotStarted,     // no device yet
        Active,         // our INTZ surface is the world depth
        Copied,         // MSAA is on: the client keeps its multisampled depth, Refresh() copies it into
                        // our INTZ texture (render/DepthCopy)
        Disabled,       // turned off from the panel
        Unsupported,    // driver has no INTZ
        Multisampled,   // MSAA is on and no copy method works (see CopyUnavailable())
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

    /// Makes Texture() hold the depth drawn so far. Call right before sampling it, mid-frame, with the
    /// world's targets bound: Active needs nothing; Copied copies the multisampled depth now (one
    /// full-screen copy each call). False if Texture() can't be trusted.
    bool Refresh(void* device);

    /// The readable depth texture (IDirect3DTexture9*), or null while neither Active nor Copied.
    void* Texture();
    /// Our depth surface (IDirect3DSurface9*), or null while not Active (Copied has none).
    void* Surface();
    /// The client's own depth surface (Active: the one we replaced; Copied: its multisampled one).
    /// Bind this while sampling Texture(), since a surface can't be read while it is also the bound
    /// depth target.
    void* OriginalSurface();

    /// Copied mode: the method, how many copies succeeded / failed; why no method if Multisampled.
    const char* CopyMethod();
    unsigned    CopiesDone();
    unsigned    CopiesFailed();
    const char* CopyUnavailable();

    /// Details for the debug panel.
    unsigned Width();
    unsigned Height();
    unsigned OriginalFormat();   // D3DFORMAT of the automatic surface
    unsigned SwapCount();        // how many times the swap has been (re)applied, e.g. after resets
}
