#include "FogFar.hpp"

#include "../env/Fog.hpp"
#include "../env/Regional.hpp"
#include "../env/TerrainHeight.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace wxl_livingazeroth::fogfar
{
    namespace
    {
        constexpr int   kGroundPerFrame = 96;    // chunks whose ground is sampled per frame (5 heights each)
        constexpr ULONGLONG kRebuildMs = 500;     // rebuilds at most twice a second (zones change slowly)
        constexpr float kNoTop = -1.0e5f;
        constexpr int   kFarKinds[] = { fog::kMist, fog::kAreaFog, fog::kHaze }; // the zone-wide kinds

        struct Chunk { int i = 0x7FFFFFFF, j = 0; bool groundKnown = false; float ground = 0.0f; };
        std::vector<Chunk> g_chunks;
        int       g_size = 0, g_cursor = 0;
        int       g_firstI = 0x7FFFFFFF, g_firstJ = 0;
        ULONGLONG g_lastBuild = 0;
        float     g_lastRef = -1.0e9f;
        bool      g_any = false;
        float     g_highestTop = -1.0e9f;

        IDirect3DTexture9* g_field = nullptr;
        IDirect3DTexture9* g_colour = nullptr;
        IDirect3DDevice9*  g_device = nullptr;

        uint16_t ToHalf(float f)
        {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
            const uint32_t mant = x & 0x7FFFFFu;
            if (exp <= 0) return static_cast<uint16_t>(sign);
            if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
            const uint32_t m = (mant + 0x1000u) >> 13;
            return static_cast<uint16_t>(sign | (exp << 10) | (m > 0x3FFu ? 0x3FFu : m));
        }

        int Mod(int v) { const int m = v % g_size; return m < 0 ? m + g_size : m; }

        /// A chunk's lowest ground: its centre and four points halfway to its corners (the layer
        /// lies on the low parts, so it doesn't float over dips).
        bool LowestGround(int i, int j, float cell, float& out)
        {
            static const float kAt[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
            bool any = false;
            for (const auto& p : kAt)
            {
                float z;
                float liquid = 0.0f;
                uint32_t type = 0;
                const float x = (i + p[0]) * cell, y = (j + p[1]) * cell;
                if (!terrain::HeightAt(x, y, z)) continue;
                if (terrain::LiquidAt(x, y, liquid, type) && liquid > z) z = liquid; // fog lies on water, as near
                out = any ? std::fmin(out, z) : z;
                any = true;
            }
            return any;
        }
    }

    bool Update(IDirect3DDevice9* dev, float groundRef, ColourFn colour, Maps& out)
    {
        const regional::ZoneMap& map = regional::Map();
        if (!map.zone || map.size <= 0) return false;
        if (dev != g_device) { Release(); g_device = dev; }
        if (g_size != map.size)
        {
            g_size = map.size;
            g_chunks.assign(static_cast<size_t>(g_size) * g_size, Chunk{});
        }

        // Ground: chunks that now hold another world chunk, or whose terrain wasn't loaded yet.
        int sampled = 0;
        for (int n = 0; n < g_size * g_size && sampled < kGroundPerFrame; ++n)
        {
            const int k = g_cursor;
            g_cursor = (g_cursor + 1) % (g_size * g_size);
            const int i = map.firstI + k % g_size, j = map.firstJ + k / g_size;
            Chunk& c = g_chunks[Mod(j) * g_size + Mod(i)];
            if (c.i == i && c.j == j && c.groundKnown) continue;
            c.i = i; c.j = j;
            c.groundKnown = LowestGround(i, j, map.cellSize, c.ground);
            ++sampled;
        }

        const bool moved = map.firstI != g_firstI || map.firstJ != g_firstJ;
        const ULONGLONG now = GetTickCount64();
        if (!moved && g_field && now - g_lastBuild < kRebuildMs && std::fabs(groundRef - g_lastRef) < 64.0f)
        {
            if (!g_any) return false;
        }
        else
        {
            g_firstI = map.firstI; g_firstJ = map.firstJ;
            g_lastBuild = now;
            g_lastRef = groundRef;
            if (!g_field && FAILED(dev->CreateTexture(g_size, g_size, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &g_field, nullptr))) { g_field = nullptr; return false; }
            if (!g_colour && FAILED(dev->CreateTexture(g_size, g_size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_colour, nullptr))) { g_colour = nullptr; return false; }

            // Per zone (a handful around): its fog's light-layer density, thickness and colour.
            struct ZoneLook { float density = 0.0f, thickness = 1.0f, rgb[3] = {}; };
            std::unordered_map<uint32_t, ZoneLook> zones;
            auto lookOf = [&](uint32_t zone) -> const ZoneLook& {
                auto found = zones.find(zone);
                if (found != zones.end()) return found->second;
                ZoneLook l;
                float weighted = 0.0f, mix[3] = {};
                for (int k : kFarKinds)
                {
                    const fog::KindSettings& s = fog::SettingsOfArea(k, zone);
                    if (!s.enabled) continue;
                    const float d = fog::ZoneAmount(zone, k) * s.density;
                    if (d <= 0.0f) continue;
                    float c[3];
                    colour(k, zone, c);
                    l.density += d;
                    weighted += d * s.thickness;
                    for (int ch = 0; ch < 3; ++ch) mix[ch] += d * c[ch];
                }
                if (l.density > 0.0f)
                {
                    l.thickness = weighted / l.density;
                    for (int ch = 0; ch < 3; ++ch) l.rgb[ch] = mix[ch] / l.density;
                }
                return zones.emplace(zone, l).first->second;
            };

            const int N = g_size;
            static std::vector<float> top;
            top.assign(static_cast<size_t>(N) * N, kNoTop);
            static std::vector<ZoneLook> looks;
            looks.assign(static_cast<size_t>(N) * N, ZoneLook{});
            static std::vector<float> ground;
            ground.assign(static_cast<size_t>(N) * N, 0.0f);
            g_any = false;
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c)
                {
                    const int i = g_firstI + c, j = g_firstJ + r;           // world chunk (texel = its slot)
                    const int slot = Mod(j) * N + Mod(i);
                    const Chunk& ch = g_chunks[slot];
                    const uint32_t zone = map.zone[slot];
                    if (!zone || ch.i != i || ch.j != j || !ch.groundKnown) continue;
                    const ZoneLook& l = lookOf(zone);
                    if (l.density <= 1.0e-5f) continue;
                    looks[slot] = l;
                    ground[slot] = ch.ground;
                    top[slot] = ch.ground + l.thickness * 4.0f;
                    g_any = true;
                }
            g_highestTop = -1.0e9f;
            D3DLOCKED_RECT lf{}, lc{};
            if (FAILED(g_field->LockRect(0, &lf, nullptr, 0))) return false;
            if (FAILED(g_colour->LockRect(0, &lc, nullptr, 0))) { g_field->UnlockRect(0); return false; }
            auto byteOf = [](float v) { v *= 0.5f; return static_cast<uint8_t>((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f); };
            for (int r = 0; r < N; ++r)
            {
                auto* texel = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lf.pBits) + r * lf.Pitch);
                auto* rgb = static_cast<uint8_t*>(lc.pBits) + r * lc.Pitch;
                for (int c = 0; c < N; ++c, texel += 4, rgb += 4)
                {
                    const int slot = r * N + c; // texel (c, r) holds slot (c, r): the zone map's own layout
                    float most = kNoTop;
                    for (int dr = -1; dr <= 1; ++dr)
                        for (int dc = -1; dc <= 1; ++dc)
                            most = std::fmax(most, top[Mod(r + dr) * N + Mod(c + dc)]);
                    const ZoneLook& l = looks[slot];
                    // The layer lies on the lowest ground among the chunk and its 8 neighbours (~100 yd),
                    // like the near field's fog floor: level in valleys, cliffs rising out of it. On each
                    // chunk's own lowest ground the floor jumped tens of yards between chunks on cliffs,
                    // and the march's shells around the camera cut it into flickering rings.
                    float low = 1.0e9f;
                    for (int dr = -1; dr <= 1; ++dr)
                        for (int dc = -1; dc <= 1; ++dc)
                        {
                            const Chunk& n = g_chunks[Mod(r + dr) * N + Mod(c + dc)];
                            if (n.groundKnown) low = std::fmin(low, n.ground);
                        }
                    // Chunks without fog take their foggy neighbour's colour (the filter fades the
                    // density to them), on the same low floor.
                    float g = low < 1.0e8f ? low : ground[slot];
                    const ZoneLook* colourFrom = &l;
                    if (l.density <= 1.0e-5f)
                        for (int dr = -1; dr <= 1 && colourFrom == &l; ++dr)
                            for (int dc = -1; dc <= 1; ++dc)
                            {
                                const int n = Mod(r + dr) * N + Mod(c + dc);
                                if (looks[n].density > 1.0e-5f) { colourFrom = &looks[n]; break; }
                            }
                    texel[0] = ToHalf(l.density);
                    texel[1] = ToHalf(colourFrom->thickness);
                    texel[2] = ToHalf(g - groundRef);
                    texel[3] = ToHalf(most > kNoTop ? most - groundRef : kNoTop);
                    rgb[0] = byteOf(colourFrom->rgb[2]);
                    rgb[1] = byteOf(colourFrom->rgb[1]);
                    rgb[2] = byteOf(colourFrom->rgb[0]);
                    rgb[3] = 255;
                    if (top[slot] > g_highestTop) g_highestTop = top[slot];
                }
            }
            g_field->UnlockRect(0);
            g_colour->UnlockRect(0);
            if (!g_any) return false;
        }

        out.field = g_field;
        out.colour = g_colour;
        // The texture wraps like the zone map: texel (Mod(i), Mod(j)) holds world chunk (i, j), so
        // world x maps to u = x / (size x chunk) with wrapping; the valid window is firstI .. +size.
        out.originX = g_firstI * map.cellSize;
        out.originY = g_firstJ * map.cellSize;
        out.invExtent = 1.0f / (g_size * map.cellSize);
        out.highestTop = g_highestTop;
        return true;
    }

    void Release()
    {
        if (g_field) { g_field->Release(); g_field = nullptr; }
        if (g_colour) { g_colour->Release(); g_colour = nullptr; }
        g_lastBuild = 0;
    }
}
