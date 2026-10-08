// Copies a multisampled depth-stencil surface into a single-sampled INTZ texture, so passes can read
// the scene depth while the client's multisampling is on (D3D9 textures can't be multisampled).
// One sample per pixel. The approach follows coa-vfog's msaa_depth (jealous-sound, GPL-3.0):
//  - NVIDIA on the system d3d9.dll: NvAPI_D3D9_StretchRectEx, both resources registered first;
//  - other drivers that offer it (AMD, Intel, DXVK [believed]): the driver's RESZ resolve, triggered
//    by a dummy point draw and a magic point-size value with the INTZ texture on sampler 0 and the
//    multisampled surface bound as depth.
// Neither exists everywhere: without one, readable depth stays off under multisampling.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;

namespace wxl_livingazeroth::depthcopy
{
    enum class Method { None, Nvapi, Resz };
    const char* MethodName(Method m);

    /// Which method this device can use, and why none if none (asked once per device).
    Method Probe(IDirect3DDevice9* dev, const char*& whyNone);

    /// Pairs a multisampled source with an INTZ destination (registers them for NVAPI). False if
    /// the method refuses them. Holds a reference on both until Detach.
    bool Attach(Method m, IDirect3DSurface9* source, IDirect3DTexture9* destination);
    void Detach();
    bool Attached();

    /// Copies now. RESZ needs the source bound as the depth-stencil surface (it is during the world
    /// pass). False on failure.
    bool Copy(IDirect3DDevice9* dev);
}
