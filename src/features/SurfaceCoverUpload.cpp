#include "SurfaceCoverInternal.hpp"

#include "../env/Fields.hpp"
#include "../render/BlpTexture.hpp"
#include "GrassPerf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace wxl_livingazeroth::cover::detail
{
    // CoverTexture slots from the table, loaded from the client's archives into textures of
    // our own; reloaded when the table is.
    IDirect3DTexture9* g_coverTextures[covertable::kMaxCoverTextures] = {}; // per slot (owned by the cache)
    bool               g_slotSpecular[covertable::kMaxCoverTextures] = {};    // per slot: its alpha is a specular mask
    int                g_slotId[covertable::kMaxCoverTextures] = {};          // the ID each slot holds (0 = free)
    std::vector<int>   g_slotOfId;                                            // ID -> slot, -1 = none
    uint32_t           g_slotGeneration = 1;
    int                g_slotFrame = 0;
    unsigned           g_texturesInView = 0;
    std::unordered_map<int, IDirect3DTexture9*> g_textureCache;
    std::unordered_map<int, std::string>       g_textureStatus;
    uint32_t           g_coverTexturesGeneration = 0;

    void ReleaseCoverTextures()
    {
        for (auto& e : g_textureCache) if (e.second) e.second->Release();
        g_textureCache.clear();
        for (IDirect3DTexture9*& t : g_coverTextures) t = nullptr;
        for (bool& b : g_slotSpecular) b = false;
        for (int& t : g_slotId) t = 0;
        g_slotOfId.clear();
        g_coverTexturesGeneration = 0;
    }

    // The slots follow what's in view: about once a second the loaded rings are counted
    // (sparsely) by cover texture ID, and the 8 most present IDs hold the slots. An ID keeps
    // its slot while it stays among them, so nothing jumps. Textures load on demand and stay in
    // a small cache, so walking back and forth doesn't reload them.
    IDirect3DTexture9* CachedTexture(IDirect3DDevice9* dev, int id)
    {
        auto found = g_textureCache.find(id);
        if (found != g_textureCache.end()) return found->second;
        if (g_textureCache.size() >= 16)
            for (auto it = g_textureCache.begin(); it != g_textureCache.end(); ++it)
            {
                bool resident = false;
                for (int t : g_slotId) resident |= t == it->first;
                if (resident) continue;
                if (it->second) it->second->Release();
                g_textureCache.erase(it);
                break;
            }
        // The _s variant first (later expansions keep the specular mask in its alpha; the plain one
        // has none), unless the row says to ignore specular. Stock 3.3.5 textures keep it in the
        // plain texture's alpha, like the terrain's layers.
        std::string error;
        IDirect3DTexture9* tex = nullptr;
        const std::string path = covertable::CoverTexturePath(id);
        if (!covertable::CoverTextureIgnoresSpecular(id) && path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".blp") == 0)
        {
            std::string unused;
            tex = blp::Load(dev, (path.substr(0, path.size() - 4) + "_s.blp").c_str(), unused);
            if (tex) g_textureStatus[id] = "loaded (_s)";
        }
        if (!tex)
        {
            tex = blp::Load(dev, path.c_str(), error);
            g_textureStatus[id] = tex ? "loaded" : error;
        }
        if (!tex) g_api->Log(WXL_LOG_WARN, kTag, "surface cover: CoverTexture \"%s\": %s", covertable::CoverTexturePath(id), error.c_str());
        g_textureCache[id] = tex;
        return tex;
    }

    void EnsureCoverTextures(IDirect3DDevice9* dev)
    {
        if (g_coverTexturesGeneration != covertable::Generation())
        {
            g_coverTexturesGeneration = covertable::Generation();
            for (auto& e : g_textureCache) if (e.second) e.second->Release();
            g_textureCache.clear();
            g_textureStatus.clear();
            for (int& t : g_slotId) t = 0;
            g_slotFrame = 0; // recount now
        }
        if (g_slotFrame > 0 && --g_slotFrame > 0) return;
        g_slotFrame = 60;

        const int ids = covertable::CoverTextureCount();
        std::vector<unsigned> count(static_cast<size_t>(ids) + 1, 0);
        for (const Level& L : g_levels)
        {
            if (!L.haveGrid) continue;
            for (int sl = 0; sl < kTex * kTex; sl += 7) // sparse is plenty for "which are around"
                for (int k = 0; k < 2; ++k)
                {
                    const int id = static_cast<int>((L.texIds[sl] >> (16 * k)) & 0xFFFF);
                    if (id > 0 && id <= ids && ((L.texShares[sl] >> (8 * k)) & 0xFF)) ++count[id];
                }
        }
        std::vector<int> wanted;
        for (int id = 1; id <= ids; ++id) if (count[id]) wanted.push_back(id);
        std::sort(wanted.begin(), wanted.end(), [&](int x, int y) { return count[x] > count[y]; });
        g_texturesInView = static_cast<unsigned>(wanted.size());
        if (wanted.size() > static_cast<size_t>(covertable::kMaxCoverTextures)) wanted.resize(covertable::kMaxCoverTextures);

        bool changed = false;
        for (int& t : g_slotId) // residents not wanted any more free their slot
            if (t && std::find(wanted.begin(), wanted.end(), t) == wanted.end()) { t = 0; changed = true; }
        for (int id : wanted)
        {
            if (std::find(std::begin(g_slotId), std::end(g_slotId), id) != std::end(g_slotId)) continue;
            for (int& t : g_slotId) if (!t) { t = id; changed = true; break; }
        }
        if (!changed && g_slotOfId.size() == static_cast<size_t>(ids) + 1) return;

        g_slotOfId.assign(static_cast<size_t>(ids) + 1, -1);
        for (int sl = 0; sl < covertable::kMaxCoverTextures; ++sl)
        {
            g_coverTextures[sl] = g_slotId[sl] ? CachedTexture(dev, g_slotId[sl]) : nullptr;
            g_slotSpecular[sl] = g_coverTextures[sl] && !covertable::CoverTextureIgnoresSpecular(g_slotId[sl]);
            if (g_slotId[sl] && g_slotId[sl] <= ids) g_slotOfId[g_slotId[sl]] = sl;
        }
        ++g_slotGeneration;
    }

    bool UploadLook(Level& L)
    {
        if (L.slotsUploaded != g_slotGeneration) L.lookDirty = true;
        if (!L.lookDirty) return true;
        // The cells' cover texture IDs -> weights of whichever slots hold those IDs now.
        for (int sl = 0; sl < kTex * kTex; ++sl)
        {
            uint8_t w[8] = {};
            for (int k = 0; k < 2; ++k)
            {
                const int id = static_cast<int>((L.texIds[sl] >> (16 * k)) & 0xFFFF);
                const int slot = id > 0 && id < static_cast<int>(g_slotOfId.size()) ? g_slotOfId[id] : -1;
                if (slot >= 0) w[slot] = static_cast<uint8_t>((L.texShares[sl] >> (8 * k)) & 0xFF);
            }
            for (int q = 0; q < 2; ++q)
                L.texWeights[q][sl] = (static_cast<uint32_t>(w[q * 4 + 3]) << 24) | (static_cast<uint32_t>(w[q * 4]) << 16) |
                                      (static_cast<uint32_t>(w[q * 4 + 1]) << 8) | w[q * 4 + 2];
        }
        L.slotsUploaded = g_slotGeneration;
        IDirect3DTexture9* const textures[4] = { L.lookTex, L.texWTex[0], L.texWTex[1], L.propsTex };
        const std::vector<uint32_t>* const data[4] = { &L.tint, &L.texWeights[0], &L.texWeights[1], &L.props };
        for (int t = 0; t < 4; ++t)
        {
            if (!textures[t]) continue;
            D3DLOCKED_RECT lr{};
            if (FAILED(textures[t]->LockRect(0, &lr, nullptr, 0))) return false;
            for (int row = 0; row < kTex; ++row)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &(*data[t])[row * kTex], kTex * 4);
            textures[t]->UnlockRect(0);
        }
        L.lookDirty = false;
        return true;
    }

    bool UploadLevel(Level& L, bool isLevel0)
    {
        // No cell with cover of its own, and no fallen snow to settle where it could: nothing to fold
        // or upload. The dirty flags stay, so the textures catch up the moment cover appears (snow
        // starting re-folds through the snow path). (A recentre marks everything dirty: walking
        // re-folded and re-uploaded whole empty levels, ~3 ms CPU + ~4 ms GPU.)
        if (L.coverCells <= 0 && (L.snowCells <= 0 || !fields::SnowActive()))
        {
            L.anyCover = false;
            if (isLevel0) { g_changed.clear(); g_changedPending = false; } // no trenches without cover
            return true;
        }
        D3DLOCKED_RECT lr{};
        if (L.baseDirty)
        {
            if (FAILED(L.baseTex->LockRect(0, &lr, nullptr, 0))) return false;
            for (int row = 0; row < kTex; ++row)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &L.base[row * kTex], kTex * 4);
            L.baseTex->UnlockRect(0);
            L.baseDirty = false;
        }
        // Level 0's trenches: only the changed cells, inside one dirty rectangle.
        if (isLevel0 && !L.coverDirty)
        {
            if (g_changed.empty()) { g_changedPending = false; return UploadLook(L); }
            int c0 = kTex, c1 = -1, r0 = kTex, r1 = -1;
            for (int sl : g_changed)
            {
                const int c = sl % kTex, r = sl / kTex;
                c0 = c < c0 ? c : c0; c1 = c > c1 ? c : c1;
                r0 = r < r0 ? r : r0; r1 = r > r1 ? r : r1;
            }
            RECT rect{ c0, r0, c1 + 1, r1 + 1 };
            if (FAILED(L.coverTex->LockRect(0, &lr, &rect, 0))) return false;
            for (int sl : g_changed)
            {
                const int c = sl % kTex, r = sl / kTex;
                float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + (r - r0) * lr.Pitch) + (c - c0);
                const float cover = g_static0[sl];
                const float deformed = cover * (1.0f + g_rimCell[sl] * g_settings.rimMul * g_rim[sl]) * (1.0f - g_press[sl] * (1.0f - L.stiff[sl] / 255.0f));
                *out = cover + (deformed - cover) * g_deformK[sl];
            }
            L.coverTex->UnlockRect(0);
            g_changed.clear();
            g_changedPending = false;
            return UploadLook(L);
        }
        // The others only change when their coverage or a multiplier does.
        if (!L.coverDirty) return UploadLook(L);
        const double foldStart = grassperf::Now();
        if (FAILED(L.coverTex->LockRect(0, &lr, nullptr, 0))) return false;
        const float depthMul = g_settings.depthMul, driftMul = g_settings.driftMul, breakupMul = g_settings.breakupMul;
        const int slotI0 = Slot(L.gridI), slotJ0 = Slot(L.gridJ);
        const bool snowOn = fields::SnowActive();
        // Material shaping: steeper with more breakup, and the fine edge noise moves the
        // threshold, so the transition breaks into patches. Fully covered and bare stay so.
        auto shape = [](uint8_t coverage, float b, uint8_t edgeNoise)
        {
            float m = (coverage / 255.0f - 0.5f) * (1.0f + 2.0f * b) + 0.5f + 1.5f * b * (edgeNoise / 255.0f - 0.5f);
            return m <= 0.0f ? 0.0f : (m >= 1.0f ? 1.0f : m * m * (3.0f - 2.0f * m));
        };
        bool any = false;
        for (int row = 0; row < kTex; ++row)
        {
            float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
            // Where this row sits in the grid (Chebyshev distance from the centre feeds the trench fade).
            const int dj = (row - slotJ0 + kTex) % kTex;
            const int cj = std::abs(dj - kHalfCells);
            // The world cell this row holds (the apron sits just before the grid's first cell).
            const float wy = static_cast<float>(L.gridJ + (dj >= kTex - kApron ? dj - kTex : dj)) * L.cell;
            for (int col = 0; col < kTex; ++col)
            {
                const int s = row * kTex + col;
                float b = L.breakup[s] / 255.0f * breakupMul;
                b = b > 1.0f ? 1.0f : b;
                float driftF = 1.0f + L.driftAmp[s] / 255.0f * driftMul * (L.drift[s] / 127.5f - 1.0f);
                driftF = driftF < 0.0f ? 0.0f : driftF;
                const float slope = L.slope[s] / 255.0f;
                float cover = L.depth[s] > 0.0f ? depthMul * L.depth[s] * shape(L.material[s], b, L.edgeNoise[s]) * slope * driftF : 0.0f;
                // Snow: painted snow thinned by its kept share (patchy: where the drift and edge
                // noise are low it opens first), fallen snow on top.
                if (snowOn && (L.snowShare[s] || L.paintedSnow[s]))
                {
                    const int di = (col - slotI0 + kTex) % kTex;
                    const float wx = static_cast<float>(L.gridI + (di >= kTex - kApron ? di - kTex : di)) * L.cell;
                    float fallen, keep;
                    fields::SnowAt(wx, wy, fallen, keep);
                    if (L.paintedSnow[s] && keep < 1.0f && cover > 0.0f)
                    {
                        constexpr float kPatchy = 0.6f;
                        const float noise = (0.6f * L.drift[s] + 0.4f * L.edgeNoise[s]) / 255.0f;
                        float t = keep * (1.0f + kPatchy) - kPatchy * (1.0f - noise);
                        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                        cover *= 1.0f - L.paintedSnow[s] / 255.0f * (1.0f - t);
                    }
                    if (L.snowShare[s] && fallen > 0.0f) cover += fallen * shape(L.snowShare[s], b, L.edgeNoise[s]) * slope * driftF;
                }
                any = any || cover > 0.0f;
                if (!isLevel0) { out[col] = cover > 0.0f ? cover : 0.0f; continue; }
                g_static0[s] = cover > 0.0f ? cover : 0.0f;
                if (cover <= 0.0f) { g_deformK[s] = 0.0f; out[col] = 0.0f; continue; }
                const int di = (col - slotI0 + kTex) % kTex;
                const int ci = std::abs(di - kHalfCells);
                const int cheb = ci > cj ? ci : cj;
                // Trenches fade out before the morph band, so the border matches the static coarser level.
                float k = cheb <= kDeformFullCells ? 1.0f
                        : (cheb >= kDeformZeroCells ? 0.0f : float(kDeformZeroCells - cheb) / float(kDeformZeroCells - kDeformFullCells));
                if (dj >= kGridVerts || di >= kGridVerts) k = 0.0f;
                g_deformK[s] = k;
                const float deformed = cover * (1.0f + g_rimCell[s] * g_settings.rimMul * g_rim[s]) * (1.0f - g_press[s] * (1.0f - L.stiff[s] / 255.0f));
                out[col] = cover + (deformed - cover) * k;
            }
        }
        L.coverTex->UnlockRect(0);
        L.coverDirty = false;
        L.anyCover = any;
        L.foldMs = grassperf::Now() - foldStart;
        if (isLevel0) { g_changed.clear(); g_changedPending = false; } // the full pass included them
        return UploadLook(L);
    }
}
