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

    void RecountCoverTextures() { g_slotFrame = 0; }

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

    UploadStats g_uploadStats;

    // Material shaping: steeper with more breakup, and the fine edge noise moves the threshold, so
    // the transition breaks into patches. Fully covered and bare stay so.
    float Shape(uint8_t coverage, float b, uint8_t edgeNoise)
    {
        float m = (coverage / 255.0f - 0.5f) * (1.0f + 2.0f * b) + 0.5f + 1.5f * b * (edgeNoise / 255.0f - 0.5f);
        return m <= 0.0f ? 0.0f : (m >= 1.0f ? 1.0f : m * m * (3.0f - 2.0f * m));
    }

    // What a fold reads besides the slot: the multipliers, whether snow is around, the grid.
    struct FoldInputs
    {
        float depthMul, driftMul, breakupMul;
        int   slotI0, slotJ0;  // the grid's first cell, as texture slots
        bool  snowOn;
    };

    FoldInputs Inputs(const Level& L)
    {
        return { g_settings.depthMul, g_settings.driftMul, g_settings.breakupMul, Slot(L.gridI), Slot(L.gridJ), fields::SnowActive() };
    }

    // One slot's cover depth without trenches, from its parts, the multipliers and the snow.
    // The world position of a slot's cell (the apron sits just before the grid's first cell).
    void SlotWorld(const Level& L, const FoldInputs& in, int row, int col, float& wx, float& wy)
    {
        const int di = (col - in.slotI0 + kTex) % kTex, dj = (row - in.slotJ0 + kTex) % kTex;
        wx = static_cast<float>(L.gridI + (di >= kTex - kApron ? di - kTex : di)) * L.cell;
        wy = static_cast<float>(L.gridJ + (dj >= kTex - kApron ? dj - kTex : dj)) * L.cell;
    }

    // The ground's moisture (0..1) at a world position: the near grid's own value, else (beyond it,
    // or not filled yet) the ground's rest moisture plus its zone's excess (per terrain chunk).
    float MoistureAt(float x, float y, float rest)
    {
        static const int kMoisture = fields::Find("moisture");
        const float pos[3] = { x, y, 0.0f };
        float m;
        if (kMoisture >= 0 && fields::Sample(kMoisture, pos, m)) return m;
        const fields::WetGrid& g = fields::Wet();
        float excess = g.farExcess;
        if (g.farValues && g.farSize > 0 && g.farCellSize > 0.0f)
        {
            const int i = static_cast<int>(std::floor(x / g.farCellSize)), j = static_cast<int>(std::floor(y / g.farCellSize));
            if (i >= g.farFirstI && i < g.farFirstI + g.farSize && j >= g.farFirstJ && j < g.farFirstJ + g.farSize)
            {
                const int mi = ((i % g.farSize) + g.farSize) % g.farSize, mj = ((j % g.farSize) + g.farSize) % g.farSize;
                excess = g.farValues[mj * g.farSize + mi];
            }
        }
        m = rest + excess;
        return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
    }

    float FoldSlot(const Level& L, const FoldInputs& in, int row, int col)
    {
        const int s = row * kTex + col;
        float b = L.breakup[s] / 255.0f * in.breakupMul;
        b = b > 1.0f ? 1.0f : b;
        float driftF = 1.0f + L.driftAmp[s] / 255.0f * in.driftMul * (L.drift[s] / 127.5f - 1.0f);
        driftF = driftF < 0.0f ? 0.0f : driftF;
        const float slope = L.slope[s] / 255.0f;
        float cover = L.depth[s] > 0.0f ? in.depthMul * L.depth[s] * Shape(L.material[s], b, L.edgeNoise[s]) * slope * driftF : 0.0f;
        // Moisture-gated (mud): only where the ground is wet enough, full 0.1 above the threshold.
        if (L.wetGate[s] && cover > 0.0f)
        {
            float wx, wy;
            SlotWorld(L, in, row, col, wx, wy);
            float t = (MoistureAt(wx, wy, L.wetRest[s] / 255.0f) - L.wetMin[s] / 255.0f) * 10.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            cover *= 1.0f - L.wetGate[s] / 255.0f * (1.0f - t);
        }
        // Snow: painted snow thinned by its kept share (patchy: where the drift and edge noise are
        // low it opens first), fallen snow on top.
        if (in.snowOn && (L.snowShare[s] || L.paintedSnow[s]))
        {
            float wx, wy, fallen, keep;
            SlotWorld(L, in, row, col, wx, wy);
            fields::SnowAt(wx, wy, fallen, keep);
            if (L.paintedSnow[s] && keep < 1.0f && cover > 0.0f)
            {
                constexpr float kPatchy = 0.6f;
                const float noise = (0.6f * L.drift[s] + 0.4f * L.edgeNoise[s]) / 255.0f;
                float t = keep * (1.0f + kPatchy) - kPatchy * (1.0f - noise);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                cover *= 1.0f - L.paintedSnow[s] / 255.0f * (1.0f - t);
            }
            if (L.snowShare[s] && fallen > 0.0f) cover += fallen * Shape(L.snowShare[s], b, L.edgeNoise[s]) * slope * driftF;
        }
        return cover > 0.0f ? cover : 0.0f;
    }

    // Level 0's trench strength at a slot, from the grid as it is now: full near the centre,
    // fading out before the morph band (so the border matches the static coarser level), none
    // outside the grid.
    float DeformK(const Level& L, int s)
    {
        const int di = (s % kTex - Slot(L.gridI) + kTex) % kTex, dj = (s / kTex - Slot(L.gridJ) + kTex) % kTex;
        if (di >= kGridVerts || dj >= kGridVerts) return 0.0f;
        const int ci = std::abs(di - kHalfCells), cj = std::abs(dj - kHalfCells);
        const int cheb = ci > cj ? ci : cj;
        return cheb <= kDeformFullCells ? 1.0f
             : (cheb >= kDeformZeroCells ? 0.0f : float(kDeformZeroCells - cheb) / float(kDeformZeroCells - kDeformFullCells));
    }

    // Level 0's drawn depth at a slot: its static cover with the trench pressed in and the rim raised.
    float Trenched(const Level& L, int s)
    {
        const float cover = g_static0[s];
        if (cover <= 0.0f) return 0.0f;
        const float deformed = cover * (1.0f + g_rimCell[s] * g_settings.rimMul * g_rim[s]) * (1.0f - g_press[s] * (1.0f - L.stiff[s] / 255.0f));
        return cover + (deformed - cover) * DeformK(L, s);
    }

    float DrawnDepth(const Level& L, bool isLevel0, int row, int col, bool trenches)
    {
        if (!isLevel0) return FoldSlot(L, Inputs(L), row, col);
        const int s = row * kTex + col;
        return trenches ? Trenched(L, s) : g_static0[s];
    }

    // The depth as the cover texture holds it: negative where the cover sits below the terrain.
    float Stored(const Level& L, int s, float depth) { return L.sink[s] ? -depth : depth; }

    // A rectangle of slots, [c0, c1) x [r0, r1).
    struct Rect { int c0, r0, c1, r1; int Slots() const { return (c1 - c0) * (r1 - r0); } };
    constexpr Rect kWhole = { 0, 0, kTex, kTex };

    // Copies 4-byte texels of `data` (one per slot) into the texture's rectangle. A managed texture
    // only sends the locked rectangle to the GPU.
    template <class T>
    bool CopyRect(IDirect3DTexture9* tex, const std::vector<T>& data, const Rect& r)
    {
        static_assert(sizeof(T) == 4, "4-byte texels");
        if (!tex) return true;
        const RECT rect{ r.c0, r.r0, r.c1, r.r1 };
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(0, &lr, &rect, 0))) return false;
        for (int row = r.r0; row < r.r1; ++row)
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + (row - r.r0) * lr.Pitch, &data[row * kTex + r.c0], (r.c1 - r.c0) * 4);
        tex->UnlockRect(0);
        return true;
    }

    // Folds a rectangle into the cover texture (level 0: keeps the static cover and draws the trenches).
    bool FoldRect(Level& L, bool isLevel0, const FoldInputs& in, const Rect& r, bool& any)
    {
        const RECT rect{ r.c0, r.r0, r.c1, r.r1 };
        D3DLOCKED_RECT lr{};
        if (FAILED(L.coverTex->LockRect(0, &lr, &rect, 0))) return false;
        for (int row = r.r0; row < r.r1; ++row)
        {
            float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + (row - r.r0) * lr.Pitch);
            for (int col = r.c0; col < r.c1; ++col)
            {
                const float cover = FoldSlot(L, in, row, col);
                any = any || cover > 0.0f;
                const int s = row * kTex + col;
                if (!isLevel0) { out[col - r.c0] = Stored(L, s, cover); continue; }
                g_static0[s] = cover;
                out[col - r.c0] = Stored(L, s, Trenched(L, s));
            }
        }
        L.coverTex->UnlockRect(0);
        return true;
    }

    // Whose look a slot shows. The look textures are filtered between slots, and the mesh's border
    // vertices sit on the first slots without cover, so the cover's outermost edge took THEIR look:
    // ground fallen snow could lie on has fallen snow's look (plain, untextured), which drew the
    // border pure white wherever it showed above the terrain. A slot without painted cover, and
    // without fallen snow lying on it now, shows a painted neighbour's look instead; its depth still
    // tapers the cover out. (Per slot: the "snow active" flag is set by any zone's snow values, so it
    // can be on in a desert.) Snow appearing on a border slot later keeps the neighbour's look until
    // that slot's look is redone (one slot wide, while it snows there).
    bool Painted(const Level& L, int s) { return L.depth[s] > 0.0f || L.paintedSnow[s]; }

    bool SnowLiesOn(const Level& L, const FoldInputs& in, int row, int col)
    {
        const int s = row * kTex + col;
        if (!in.snowOn || !L.snowShare[s]) return false;
        float wx, wy, fallen, keep;
        SlotWorld(L, in, row, col, wx, wy);
        fields::SnowAt(wx, wy, fallen, keep);
        return fallen > 0.0f;
    }

    int LookSource(const Level& L, const FoldInputs& in, int row, int col)
    {
        const int s = row * kTex + col;
        if (Painted(L, s) || SnowLiesOn(L, in, row, col)) return s;
        static const int kAround[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
        for (const auto& d : kAround)
        {
            const int n = Slot(row + d[1]) * kTex + Slot(col + d[0]);
            if (Painted(L, n)) return n;
        }
        return s;
    }

    // The look textures for the rectangle: tint (VS), the cover texture weights (each cell's two
    // IDs -> whichever slots hold those IDs now) and props (PS).
    bool LookRect(Level& L, const Rect& r, const FoldInputs& in)
    {
        IDirect3DTexture9* const textures[4] = { L.lookTex, L.texWTex[0], L.texWTex[1], L.propsTex };
        D3DLOCKED_RECT lr[4] = {};
        const RECT rect{ r.c0, r.r0, r.c1, r.r1 };
        int locked = 0;
        for (; locked < 4; ++locked)
            if (textures[locked] && FAILED(textures[locked]->LockRect(0, &lr[locked], &rect, 0))) break;
        const bool ok = locked == 4;
        if (ok)
            for (int row = r.r0; row < r.r1; ++row)
            {
                uint32_t* out[4];
                for (int t = 0; t < 4; ++t)
                    out[t] = textures[t] ? reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr[t].pBits) + (row - r.r0) * lr[t].Pitch) : nullptr;
                for (int col = r.c0; col < r.c1; ++col)
                {
                    const int src = LookSource(L, in, row, col);
                    uint8_t w[8] = {};
                    for (int k = 0; k < 2; ++k)
                    {
                        const int id = static_cast<int>((L.texIds[src] >> (16 * k)) & 0xFFFF);
                        const int slot = id > 0 && id < static_cast<int>(g_slotOfId.size()) ? g_slotOfId[id] : -1;
                        if (slot >= 0) w[slot] = static_cast<uint8_t>((L.texShares[src] >> (8 * k)) & 0xFF);
                    }
                    const int c = col - r.c0;
                    if (out[0]) out[0][c] = L.tint[src];
                    for (int q = 0; q < 2; ++q)
                        out[1 + q][c] = (static_cast<uint32_t>(w[q * 4 + 3]) << 24) | (static_cast<uint32_t>(w[q * 4]) << 16) |
                                        (static_cast<uint32_t>(w[q * 4 + 1]) << 8) | w[q * 4 + 2];
                    out[3][c] = L.props[src];
                }
            }
        for (int t = 0; t < locked; ++t) if (textures[t]) textures[t]->UnlockRect(0);
        return ok;
    }

    // One changed region: whatever its kDirty* bits say.
    bool UploadRegion(Level& L, bool isLevel0, const FoldInputs& in, const Rect& r, uint8_t bits)
    {
        ++g_uploadStats.regions;
        g_uploadStats.regionSlots += r.Slots();
        if ((bits & kDirtyBase) && !CopyRect(L.baseTex, L.base, r)) return false;
        if (bits & kDirtyCover)
        {
            const double t0 = grassperf::Now();
            bool any = false;
            if (!FoldRect(L, isLevel0, in, r, any)) return false;
            L.anyCover = L.anyCover || any;
            g_uploadStats.foldMs += grassperf::Now() - t0;
        }
        if (bits & kDirtyLook)
        {
            const double t0 = grassperf::Now();
            if (!LookRect(L, r, in)) return false;
            g_uploadStats.lookMs += grassperf::Now() - t0;
        }
        return true;
    }

    // The marked rows (and columns), grouped into runs of neighbours, each run one rectangle
    // across the whole texture.
    bool UploadMarked(Level& L, bool isLevel0, const FoldInputs& in, std::vector<uint8_t>& marks, bool rows)
    {
        for (int a = 0; a < kTex;)
        {
            if (!marks[a]) { ++a; continue; }
            int b = a;
            uint8_t bits = 0;
            while (b < kTex && marks[b]) bits |= marks[b++];
            const Rect r = rows ? Rect{ 0, a, kTex, b } : Rect{ a, 0, b, kTex };
            if (!UploadRegion(L, isLevel0, in, r, bits)) return false;
            std::fill(marks.begin() + a, marks.begin() + b, 0);
            a = b;
        }
        return true;
    }

    bool UploadLevel(Level& L, bool isLevel0)
    {
        // No cell with cover of its own, and no fallen snow to settle where it could: nothing to fold
        // or upload. The dirty flags and marks stay, so the textures catch up the moment cover
        // appears (snow starting re-folds through the snow path).
        if (L.coverCells <= 0 && (L.snowCells <= 0 || !fields::SnowActive()))
        {
            L.anyCover = false;
            if (isLevel0) { g_changed.clear(); g_changedPending = false; } // no trenches without cover
            return true;
        }
        const FoldInputs in = Inputs(L);

        // Whole-texture passes (a refill, a setting, snow, the cover texture slots changing). The
        // marks stay while a fill job runs: its strips are folded against the grid it ends with.
        if (L.baseDirty)
        {
            if (!CopyRect(L.baseTex, L.base, kWhole)) return false;
            L.baseDirty = false;
            if (!L.jobActive) for (int k = 0; k < kTex; ++k) { L.dirtyRow[k] &= ~kDirtyBase; L.dirtyCol[k] &= ~kDirtyBase; }
        }
        if (L.coverDirty)
        {
            const double t0 = grassperf::Now();
            bool any = false;
            if (!FoldRect(L, isLevel0, in, kWhole, any)) return false;
            L.coverDirty = false;
            L.anyCover = any;
            L.foldMs = grassperf::Now() - t0;
            g_uploadStats.foldMs += L.foldMs;
            ++g_uploadStats.fullFolds;
            if (!L.jobActive) for (int k = 0; k < kTex; ++k) { L.dirtyRow[k] &= ~kDirtyCover; L.dirtyCol[k] &= ~kDirtyCover; }
        }
        if (L.lookDirty || L.slotsUploaded != g_slotGeneration)
        {
            const double t0 = grassperf::Now();
            if (!LookRect(L, kWhole, in)) return false;
            L.lookDirty = false;
            L.slotsUploaded = g_slotGeneration;
            g_uploadStats.lookMs += grassperf::Now() - t0;
            ++g_uploadStats.fullLooks;
            if (!L.jobActive) for (int k = 0; k < kTex; ++k) { L.dirtyRow[k] &= ~kDirtyLook; L.dirtyCol[k] &= ~kDirtyLook; }
        }

        // Changed regions: a finished recentre's strips, re-sampled rows.
        if (!L.jobActive && (!UploadMarked(L, isLevel0, in, L.dirtyRow, true) || !UploadMarked(L, isLevel0, in, L.dirtyCol, false)))
            return false;

        // Level 0's trenches: the changed cells (every active trench, every frame), in one rectangle.
        if (isLevel0)
        {
            if (!g_changed.empty())
            {
                int c0 = kTex, c1 = -1, r0 = kTex, r1 = -1;
                for (int sl : g_changed)
                {
                    const int c = sl % kTex, r = sl / kTex;
                    c0 = c < c0 ? c : c0; c1 = c > c1 ? c : c1;
                    r0 = r < r0 ? r : r0; r1 = r > r1 ? r : r1;
                }
                const RECT rect{ c0, r0, c1 + 1, r1 + 1 };
                D3DLOCKED_RECT lr{};
                if (FAILED(L.coverTex->LockRect(0, &lr, &rect, 0))) return false;
                for (int sl : g_changed)
                {
                    const int c = sl % kTex, r = sl / kTex;
                    *(reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + (r - r0) * lr.Pitch) + (c - c0)) = Stored(L, sl, Trenched(L, sl));
                }
                L.coverTex->UnlockRect(0);
                g_changed.clear();
            }
            g_changedPending = false;
        }
        return true;
    }
}
