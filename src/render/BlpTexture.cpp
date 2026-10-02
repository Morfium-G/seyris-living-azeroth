#include "BlpTexture.hpp"

#include "game/Io.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace wxl_livingazeroth::blp
{
    namespace
    {
        namespace io = wxl::game::io;

        // BLP2 header (148 + 1024 bytes): magic, type, compression (1 palette, 2 DXT, 3 BGRA),
        // alpha depth (0/1/4/8 bits), alpha type (DXT: 0 DXT1, 1 DXT3, 7 DXT5), has mips, size,
        // 16 mip offsets and sizes, then the 256-entry BGRA palette.
#pragma pack(push, 1)
        struct Header
        {
            char     magic[4];
            uint32_t type;
            uint8_t  compression, alphaDepth, alphaType, hasMips;
            uint32_t width, height;
            uint32_t mipOffset[16];
            uint32_t mipSize[16];
            uint32_t palette[256];
        };
#pragma pack(pop)
        static_assert(sizeof(Header) == 148 + 1024, "BLP2 header");

        bool ReadFile(const char* path, std::vector<uint8_t>& out)
        {
            void* handle = nullptr;
            if (!io::FileOpen(path, 0, &handle) || !handle) return false;
            uint32_t high = 0;
            const uint32_t size = io::FileSize(handle, &high);
            bool ok = size > sizeof(Header) && size < (64u << 20) && !high;
            if (ok)
            {
                out.resize(size);
                uint32_t read = 0;
                ok = io::FileRead(handle, out.data(), size, &read) && read == size;
            }
            io::FileClose(handle);
            return ok;
        }

        // Palette index + separate alpha (alphaDepth bits per pixel) -> BGRA.
        void ExpandPalette(const Header& h, const uint8_t* src, size_t srcSize, uint32_t w, uint32_t hgt, uint32_t* out)
        {
            const size_t count = static_cast<size_t>(w) * hgt;
            const uint8_t* alpha = src + count;
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t c = i < srcSize ? h.palette[src[i]] & 0x00FFFFFFu : 0;
                uint32_t a = 255;
                switch (h.alphaDepth)
                {
                    case 1: a = (count + i / 8 < srcSize && (alpha[i / 8] >> (i % 8)) & 1) ? 255 : 0; break;
                    case 4: a = count + i / 2 < srcSize ? ((alpha[i / 2] >> ((i % 2) * 4)) & 0xF) * 17 : 255; break;
                    case 8: a = count + i < srcSize ? alpha[i] : 255; break;
                    default: break;
                }
                out[i] = c | (a << 24);
            }
        }
    }

    IDirect3DTexture9* Load(IDirect3DDevice9* dev, const char* path, std::string& error)
    {
        std::vector<uint8_t> file;
        if (!ReadFile(path, file)) { error = "can't read the file"; return nullptr; }
        Header h;
        std::memcpy(&h, file.data(), sizeof(h));
        if (std::memcmp(h.magic, "BLP2", 4) != 0) { error = "not a BLP2 file"; return nullptr; }
        if (!h.width || !h.height || h.width > 4096 || h.height > 4096) { error = "bad size"; return nullptr; }

        D3DFORMAT format = D3DFMT_A8R8G8B8;
        UINT block = 0; // bytes per 4x4 block for DXT
        if (h.compression == 2)
        {
            if (h.alphaType == 7)      { format = D3DFMT_DXT5; block = 16; }
            else if (h.alphaType == 1) { format = D3DFMT_DXT3; block = 16; }
            else                       { format = D3DFMT_DXT1; block = 8; }
        }
        else if (h.compression != 1 && h.compression != 3) { error = "unknown BLP compression"; return nullptr; }

        UINT mips = 1;
        if (h.hasMips)
            while (mips < 16 && h.mipOffset[mips] && h.mipSize[mips]) ++mips;

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(dev->CreateTexture(h.width, h.height, mips, 0, format, D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
        { error = "CreateTexture failed"; return nullptr; }

        std::vector<uint32_t> expanded;
        for (UINT m = 0; m < mips; ++m)
        {
            const uint32_t w = h.width >> m ? h.width >> m : 1, hh = h.height >> m ? h.height >> m : 1;
            const uint32_t offset = h.mipOffset[m], size = h.mipSize[m];
            if (!size || offset + static_cast<size_t>(size) > file.size()) { tex->Release(); error = "mip data out of range"; return nullptr; }
            const uint8_t* src = file.data() + offset;

            D3DLOCKED_RECT lr{};
            if (FAILED(tex->LockRect(m, &lr, nullptr, 0))) { tex->Release(); error = "LockRect failed"; return nullptr; }
            if (block)
            {
                const uint32_t rowBytes = ((w + 3) / 4) * block, rows = (hh + 3) / 4;
                for (uint32_t r = 0; r < rows; ++r)
                {
                    const size_t from = static_cast<size_t>(r) * rowBytes;
                    if (from + rowBytes > size) break;
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, src + from, rowBytes);
                }
            }
            else
            {
                const uint32_t* pixels = reinterpret_cast<const uint32_t*>(src);
                if (h.compression == 1)
                {
                    expanded.resize(static_cast<size_t>(w) * hh);
                    ExpandPalette(h, src, size, w, hh, expanded.data());
                    pixels = expanded.data();
                }
                else if (size < static_cast<size_t>(w) * hh * 4) { tex->UnlockRect(m); tex->Release(); error = "short BGRA mip"; return nullptr; }
                for (uint32_t r = 0; r < hh; ++r)
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, pixels + static_cast<size_t>(r) * w, w * 4);
            }
            tex->UnlockRect(m);
        }
        return tex;
    }
}
