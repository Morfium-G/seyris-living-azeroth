#include "MoistureTexture.hpp"

#include "../env/Fields.hpp"

#include <cstdint>
#include <cstring>

namespace wxl_livingazeroth::moisturetex
{
    namespace
    {
        IDirect3DDevice9*  g_device = nullptr;
        IDirect3DTexture9* g_texture = nullptr;
        IDirect3DTexture9* g_farPixel = nullptr;   // A8R8G8B8, alpha = excess
        IDirect3DTexture9* g_farVertex = nullptr;  // R32F
        int                g_farSize = 0;
        uint32_t           g_farVersion = 0;
        bool               g_farUploaded = false;

        // The far textures, current (created on first use, re-uploaded when the data changes).
        bool EnsureFar(IDirect3DDevice9* device)
        {
            const fields::WetGrid& wet = fields::Wet();
            if (!device || !wet.farBytes || wet.farSize <= 0) return false;
            if (device != g_device && g_device) Release();
            g_device = device;
            if (wet.farSize != g_farSize)
            {
                if (g_farPixel) { g_farPixel->Release(); g_farPixel = nullptr; }
                if (g_farVertex) { g_farVertex->Release(); g_farVertex = nullptr; }
                g_farUploaded = false;
            }
            if (!g_farPixel && FAILED(device->CreateTexture(wet.farSize, wet.farSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_farPixel, nullptr))) { g_farPixel = nullptr; return false; }
            if (!g_farVertex && FAILED(device->CreateTexture(wet.farSize, wet.farSize, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &g_farVertex, nullptr))) { g_farVertex = nullptr; return false; }
            g_farSize = wet.farSize;
            if (!g_farUploaded || wet.version != g_farVersion)
            {
                D3DLOCKED_RECT lr{};
                if (SUCCEEDED(g_farPixel->LockRect(0, &lr, nullptr, 0)))
                {
                    for (int row = 0; row < wet.farSize; ++row)
                    {
                        uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                        for (int col = 0; col < wet.farSize; ++col) out[col] = static_cast<uint32_t>(wet.farBytes[row * wet.farSize + col]) << 24;
                    }
                    g_farPixel->UnlockRect(0);
                }
                if (SUCCEEDED(g_farVertex->LockRect(0, &lr, nullptr, 0)))
                {
                    for (int row = 0; row < wet.farSize; ++row)
                        std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, wet.farValues + row * wet.farSize, wet.farSize * sizeof(float));
                    g_farVertex->UnlockRect(0);
                }
                g_farVersion = wet.version;
                g_farUploaded = true;
            }
            return true;
        }
        int                g_size = 0;
        uint32_t           g_version = 0;
        bool               g_uploaded = false;
    }

    void Release()
    {
        if (g_texture) g_texture->Release();
        if (g_farPixel) g_farPixel->Release();
        if (g_farVertex) g_farVertex->Release();
        g_texture = g_farPixel = g_farVertex = nullptr;
        g_device = nullptr;
        g_size = g_farSize = 0;
        g_uploaded = g_farUploaded = false;
    }

    IDirect3DTexture9* Get(IDirect3DDevice9* device)
    {
        const fields::WetGrid& wet = fields::Wet();
        if (!device || !wet.excess || wet.size <= 0) return nullptr;
        if (device != g_device && g_device) Release();
        g_device = device;
        if (g_texture && wet.size != g_size) { g_texture->Release(); g_texture = nullptr; g_uploaded = false; }
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

    bool BindFarPixel(IDirect3DDevice9* device, DWORD sampler)
    {
        if (!EnsureFar(device)) return false;
        device->SetTexture(sampler, g_farPixel);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, FALSE);
        return true;
    }

    bool BindFarVertex(IDirect3DDevice9* device, DWORD vertexSampler)
    {
        if (!EnsureFar(device)) return false;
        device->SetTexture(vertexSampler, g_farVertex);
        device->SetSamplerState(vertexSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(vertexSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(vertexSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(vertexSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(vertexSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        return true;
    }

    bool FarMapping(float& inverseExtent, float box[4], int& size, float& outside)
    {
        const fields::WetGrid& wet = fields::Wet();
        outside = wet.farExcess;
        if (!wet.farBytes || wet.farSize <= 0) return false;
        const float extent = wet.farSize * wet.farCellSize;
        inverseExtent = 1.0f / extent;
        size = wet.farSize;
        box[0] = wet.farFirstI * wet.farCellSize;
        box[1] = wet.farFirstJ * wet.farCellSize;
        box[2] = (wet.farFirstI + wet.farSize) * wet.farCellSize;
        box[3] = (wet.farFirstJ + wet.farSize) * wet.farCellSize;
        return true;
    }

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
