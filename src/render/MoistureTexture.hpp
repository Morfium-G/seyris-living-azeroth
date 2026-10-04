// The ground moisture grid (env/Fields, moisture above each material's rest) as one texture, shared
// by everything that draws wet ground (the cover, the patched terrain). Re-uploaded when the grid
// changes; recreated when the device does.
//
// Layout: world-aligned and toroidal. World cell i = floor(x / cellSize) is column mod(i, size), so
// sampling at u = x / (size * cellSize) with WRAP addressing finds it; alpha = the excess (0..1).
// Only the grid's own box (Box()) is valid -- outside it the wrap repeats other cells.
#pragma once

#include <d3d9.h>

namespace wxl_livingazeroth::moisturetex
{
    /// The texture, current (null if unavailable).
    IDirect3DTexture9* Get(IDirect3DDevice9* device);

    /// Binds it to `sampler` with linear filtering and wrap addressing; false if unavailable.
    bool Bind(IDirect3DDevice9* device, DWORD sampler);

    /// 1 / the grid's extent in yards (u = x * this), and its valid box in world yards
    /// (min x, min y, max x, max y). False while there's no grid.
    bool Mapping(float& inverseExtent, float box[4]);

    /// Beyond the grid: one regional value (the grid's outer ring), and the yards over which the grid
    /// fades into it inside its edge.
    float FarExcess();
    constexpr float kEdgeFadeYards = 64.0f;

    /// Beyond the grid, per terrain chunk (the zone each belongs to): the same world-aligned toroidal
    /// layout at chunk size. BindFarPixel puts the filtered A8R8G8B8 copy (alpha) on a pixel
    /// sampler; BindFarVertex the R32F copy (point sampled: the cover's vertex shader filters it
    /// itself) on a vertex sampler. FarMapping: 1 / extent, the valid box (world yd), the map's
    /// size in chunks, and the value outside the box.
    bool BindFarPixel(IDirect3DDevice9* device, DWORD sampler);
    bool BindFarVertex(IDirect3DDevice9* device, DWORD vertexSampler);
    bool FarMapping(float& inverseExtent, float box[4], int& size, float& outside);

    /// Drops the textures (device lost or replaced).
    void Release();
}
