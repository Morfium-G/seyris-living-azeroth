#include "MoistureTexture.hpp"

#include "../env/Fields.hpp"

#include <cstdint>

namespace wxl_livingazeroth::moisturetex
{
    namespace
    {
        IDirect3DDevice9*  g_device = nullptr;
        IDirect3DTexture9* g_texture = nullptr;
        int                g_size = 0;
        uint32_t           g_version = 0;
        bool               g_uploaded = false;
    }

    void Release()
    {
        if (g_texture) g_texture->Release();
        g_texture = nullptr;
        g_device = nullptr;
        g_size = 0;
        g_uploaded = false;
    }

    IDirect3DTexture9* Get(IDirect3DDevice9* device)
    {
        const fields::WetGrid& wet = fields::Wet();
        if (!device || !wet.excess || wet.size <= 0) return nullptr;
        if (device != g_device || wet.size != g_size) Release();
        if (!g_texture)
        {
            if (FAILED(device->CreateTexture(wet.size, wet.size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_texture, nullptr)))
            {
                g_texture = nullptr;
                return nullptr;
            }
            g_device = device;
            g_size = wet.size;
        }
        if (!g_uploaded || wet.version != g_version)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(g_texture->LockRect(0, &lr, nullptr, 0)))
            {
                for (int row = 0; row < wet.size; ++row)
                {
                    uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                    for (int col = 0; col < wet.size; ++col) out[col] = static_cast<uint32_t>(wet.excess[row * wet.size + col]) << 24;
                }
                g_texture->UnlockRect(0);
                g_version = wet.version;
                g_uploaded = true;
            }
        }
        return g_texture;
    }

    bool Bind(IDirect3DDevice9* device, DWORD sampler)
    {
        IDirect3DTexture9* tex = Get(device);
        if (!tex) return false;
        device->SetTexture(sampler, tex);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, FALSE);
        return true;
    }

    float FarExcess() { return fields::Wet().farExcess; }

    bool Mapping(float& inverseExtent, float box[4])
    {
        const fields::WetGrid& wet = fields::Wet();
        if (!wet.excess || wet.size <= 0) return false;
        const float extent = wet.size * wet.cellSize;
        inverseExtent = 1.0f / extent;
        box[0] = wet.firstI * wet.cellSize;
        box[1] = wet.firstJ * wet.cellSize;
        box[2] = (wet.firstI + wet.size) * wet.cellSize;
        box[3] = (wet.firstJ + wet.size) * wet.cellSize;
        return true;
    }
}
