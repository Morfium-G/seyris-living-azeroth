#include "GrassDoodads.hpp"

#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wxl_livingazeroth::grassdoodads
{
    namespace
    {
        constexpr const char* kTag = "wxl-seyris-living-azeroth";

        // --- client landmarks (see header / ground-effects.md) --------------------------------
        constexpr uintptr_t kFillLayerSlotVB = 0x007B1B50; // thiscall(slot), no stack args
        constexpr uintptr_t kDrawLayerSlot   = 0x007B3390; // thiscall(slot), no stack args
        constexpr uintptr_t kDoodadTable     = 0x00D1C4FC; // -> entry*[maxId + 1]
        constexpr uintptr_t kDoodadTableSize = 0x00D1C4F8; // maxId + 1

        constexpr size_t kSlotInstanceCount = 0x18;
        constexpr size_t kSlotInstances     = 0x1C;
        constexpr size_t kInstanceStride    = 0x2C;
        constexpr size_t kInstanceDoodadId  = 0x04;

        constexpr size_t kEntryRow   = 0x00;
        constexpr size_t kEntryModel = 0x04;
        constexpr size_t kRowPath    = 0x04; // GroundEffectDoodad: ID, ModelPath, Flags

        constexpr size_t kModelShared   = 0x2C;
        constexpr size_t kSharedHeader  = 0x150;
        constexpr size_t kSharedSkin    = 0x170;
        constexpr size_t kHeaderVerts   = 0x40;
        constexpr size_t kSkinCount     = 0x04;
        constexpr size_t kSkinIndices   = 0x08;
        constexpr size_t kVertexStride  = 48;
        constexpr size_t kVertexUv      = 0x20;

        // Analysis tuning.
        constexpr float kRootTipBand   = 0.12f; // lowest/highest fraction of the height = roots/tips
        constexpr float kFlatHeight    = 0.15f; // below this a doodad doesn't sway (pebbles, shells)
        constexpr float kRectPad       = 0.002f;

        using SlotFn = void(__fastcall*)(void* slot, void* edx);

        struct SlotDoodads { uint32_t ids[kEntries]; unsigned count; };

        const WXL_Api* g_api = nullptr;
        SlotFn g_origFill = nullptr;
        SlotFn g_origDraw = nullptr;

        std::unordered_map<uint32_t, DoodadInfo>    g_doodads;
        std::unordered_map<const void*, SlotDoodads> g_slots;
        std::unordered_map<uint32_t, bool> g_windOverride, g_pushOverride, g_flipOverride;
        unsigned g_tooMany = 0;

        // --- guarded reads (no C++ objects in these frames: __try needs that) --------------------
        bool ReadU32(uintptr_t address, uint32_t& out)
        {
            __try { out = *reinterpret_cast<const uint32_t*>(address); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        struct ModelScan
        {
            float minZ, maxZ, uMin, vMin, uMax, vMax;
            float rootVSum, tipVSum;
            unsigned rootN, tipN, verts;
        };

        // Two passes over the skin-referenced vertices: bounds first, then the root/tip bands.
        bool ScanModel(uintptr_t verts, const uint16_t* indices, uint32_t count, ModelScan& s)
        {
            __try
            {
                s = ModelScan{ 1e30f, -1e30f, 1e30f, 1e30f, -1e30f, -1e30f, 0, 0, 0, 0, count };
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uintptr_t v = verts + static_cast<uintptr_t>(indices[i]) * kVertexStride;
                    const float z = *reinterpret_cast<const float*>(v + 8);
                    const float u = *reinterpret_cast<const float*>(v + kVertexUv);
                    const float t = *reinterpret_cast<const float*>(v + kVertexUv + 4);
                    s.minZ = std::min(s.minZ, z); s.maxZ = std::max(s.maxZ, z);
                    s.uMin = std::min(s.uMin, u); s.uMax = std::max(s.uMax, u);
                    s.vMin = std::min(s.vMin, t); s.vMax = std::max(s.vMax, t);
                }
                const float band = (s.maxZ - s.minZ) * kRootTipBand;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uintptr_t v = verts + static_cast<uintptr_t>(indices[i]) * kVertexStride;
                    const float z = *reinterpret_cast<const float*>(v + 8);
                    const float t = *reinterpret_cast<const float*>(v + kVertexUv + 4);
                    if (z <= s.minZ + band) { s.rootVSum += t; ++s.rootN; }
                    if (z >= s.maxZ - band) { s.tipVSum += t; ++s.tipN; }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        const char* SafePath(uintptr_t row)
        {
            uint32_t p = 0;
            if (!row || !ReadU32(row + kRowPath, p) || !p) return "";
            return reinterpret_cast<const char*>(p);
        }

        void ApplyOverrides(DoodadInfo& d)
        {
            d.windOn = d.valid && !d.autoFlat;
            d.pushOn = d.valid && !d.autoFlat;
            if (auto it = g_windOverride.find(d.id); it != g_windOverride.end()) d.windOn = it->second;
            if (auto it = g_pushOverride.find(d.id); it != g_pushOverride.end()) d.pushOn = it->second;
            auto f = g_flipOverride.find(d.id);
            d.flipped = f != g_flipOverride.end() && f->second;
        }

        void Analyze(uint32_t id)
        {
            DoodadInfo& d = g_doodads[id];
            d.id = id;
            if (d.analyzed) return;

            uint32_t table = 0, size = 0, entry = 0, row = 0, model = 0;
            if (!ReadU32(kDoodadTable, table) || !ReadU32(kDoodadTableSize, size) || !table || id >= size) return;
            if (!ReadU32(table + id * 4, entry) || !entry) return;
            ReadU32(entry + kEntryRow, row);
            d.modelPath = SafePath(row);
            if (!ReadU32(entry + kEntryModel, model) || !model) return; // not loaded yet: retry later

            uint32_t shared = 0, header = 0, skin = 0, verts = 0, count = 0, indices = 0;
            if (!ReadU32(model + kModelShared, shared) || !shared) return;
            if (!ReadU32(shared + kSharedHeader, header) || !header) return;
            if (!ReadU32(shared + kSharedSkin, skin) || !skin) return;
            if (!ReadU32(header + kHeaderVerts, verts) || !verts) return;
            if (!ReadU32(skin + kSkinCount, count) || !ReadU32(skin + kSkinIndices, indices) || !indices) return;
            if (count == 0 || count > 200000) return;

            d.analyzed = true;
            ModelScan s;
            if (!ScanModel(verts, reinterpret_cast<const uint16_t*>(indices), count, s) || !s.rootN || !s.tipN)
            {
                g_api->Log(WXL_LOG_WARN, kTag, "grass doodad %u (%s): couldn't read its model, falls back to the default bend.",
                           id, d.modelPath);
                ApplyOverrides(d);
                return;
            }

            d.height = s.maxZ - s.minZ;
            d.uMin = s.uMin - kRectPad; d.vMin = s.vMin - kRectPad;
            d.uMax = s.uMax + kRectPad; d.vMax = s.vMax + kRectPad;
            d.rootV = s.rootVSum / s.rootN;
            d.tipV  = s.tipVSum / s.tipN;
            d.autoFlat = d.height < kFlatHeight;
            d.valid = std::abs(d.tipV - d.rootV) > 0.01f || d.autoFlat;
            ApplyOverrides(d);

            g_api->Log(WXL_LOG_INFO, kTag, "grass doodad %u (%s): height %.2f, root v %.3f, tip v %.3f, uv [%.3f,%.3f]-[%.3f,%.3f]%s",
                       id, d.modelPath, d.height, d.rootV, d.tipV, d.uMin, d.vMin, d.uMax, d.vMax,
                       d.autoFlat ? " -> flat, no wind" : (d.valid ? "" : " -> no usable root/tip, default bend"));
        }

        // After the client (re)builds a slot's grass: note its doodads, analyse any new ones.
        void __fastcall hkFill(void* slot, void* edx)
        {
            g_origFill(slot, edx);

            uint32_t count = 0, instances = 0;
            const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            if (!ReadU32(s + kSlotInstanceCount, count) || !ReadU32(s + kSlotInstances, instances) || !instances) return;

            SlotDoodads sd{};
            bool overflow = false;
            for (uint32_t i = 0; i < count && i < 8192; ++i)
            {
                uint32_t id = 0;
                if (!ReadU32(instances + i * kInstanceStride + kInstanceDoodadId, id)) break;
                bool known = false;
                for (unsigned k = 0; k < sd.count; ++k) known |= sd.ids[k] == id;
                if (known) continue;
                if (sd.count < kEntries) sd.ids[sd.count++] = id;
                else overflow = true;
            }
            if (overflow) ++g_tooMany;

            for (unsigned k = 0; k < sd.count; ++k)
            {
                Analyze(sd.ids[k]);
                ++g_doodads[sd.ids[k]].seenInSlots;
            }
            if (g_slots.size() > 50000) g_slots.clear(); // slots get reused; don't grow forever
            g_slots[slot] = sd;
        }

        // Before each layer draw: hand the shader this slot's doodad table. Unused entries get an
        // impossible rectangle so no vertex matches them.
        void __fastcall hkDraw(void* slot, void* edx)
        {
            float table[kEntries * 2][4];
            for (unsigned k = 0; k < kEntries; ++k)
            {
                float* a = table[k * 2];
                float* b = table[k * 2 + 1];
                a[0] = 2.0f; a[1] = 2.0f; a[2] = -1.0f; a[3] = -1.0f;
                b[0] = 0.0f; b[1] = 0.0f; b[2] = 0.0f; b[3] = 0.0f;
            }

            auto it = g_slots.find(slot);
            if (it != g_slots.end())
            {
                for (unsigned k = 0; k < it->second.count; ++k)
                {
                    auto d = g_doodads.find(it->second.ids[k]);
                    if (d == g_doodads.end()) continue;
                    DoodadInfo& info = d->second;
                    if (!info.analyzed) Analyze(info.id); // its model may have loaded since
                    if (!info.valid) continue;             // falls back to the default bend

                    float* a = table[k * 2];
                    float* b = table[k * 2 + 1];
                    a[0] = info.uMin; a[1] = info.vMin; a[2] = info.uMax; a[3] = info.vMax;
                    const float rootV = info.flipped ? info.tipV : info.rootV;
                    const float tipV  = info.flipped ? info.rootV : info.tipV;
                    const float span = tipV - rootV;
                    b[0] = rootV;
                    b[1] = std::abs(span) > 1e-4f ? 1.0f / span : 0.0f;
                    b[2] = info.windOn ? 1.0f : 0.0f;
                    b[3] = info.pushOn ? 1.0f : 0.0f;
                }
            }

            if (auto* dev = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice()))
                dev->SetVertexShaderConstantF(kFirstReg, &table[0][0], kEntries * 2);

            g_origDraw(slot, edx);
        }
    }

    bool Install(const WXL_Api* api)
    {
        g_api = api;
        const int a = api->HookAttach("LivingAzeroth.GrassFillLayerSlot", kFillLayerSlotVB,
                                      reinterpret_cast<void*>(&hkFill), reinterpret_cast<void**>(&g_origFill),
                                      WXL_HOOK_DEFAULT_PRIORITY);
        const int b = api->HookAttach("LivingAzeroth.GrassDrawLayerSlot", kDrawLayerSlot,
                                      reinterpret_cast<void*>(&hkDraw), reinterpret_cast<void**>(&g_origDraw),
                                      WXL_HOOK_DEFAULT_PRIORITY);
        api->Log((a && b) ? WXL_LOG_INFO : WXL_LOG_WARN, kTag, "grass doodads: hooks %s.",
                 (a && b) ? "installed" : "FAILED (per-doodad bend falls back to the default)");
        return a && b;
    }

    std::vector<DoodadInfo> Seen()
    {
        std::vector<DoodadInfo> out;
        out.reserve(g_doodads.size());
        for (const auto& [id, d] : g_doodads) out.push_back(d);
        std::sort(out.begin(), out.end(), [](const DoodadInfo& x, const DoodadInfo& y) { return x.id < y.id; });
        return out;
    }

    void SetWind(uint32_t id, bool on)
    {
        g_windOverride[id] = on;
        if (auto it = g_doodads.find(id); it != g_doodads.end()) ApplyOverrides(it->second);
    }

    void SetPush(uint32_t id, bool on)
    {
        g_pushOverride[id] = on;
        if (auto it = g_doodads.find(id); it != g_doodads.end()) ApplyOverrides(it->second);
    }

    void SetFlip(uint32_t id, bool flipped)
    {
        g_flipOverride[id] = flipped;
        if (auto it = g_doodads.find(id); it != g_doodads.end()) ApplyOverrides(it->second);
    }

    unsigned SlotsTracked()            { return static_cast<unsigned>(g_slots.size()); }
    unsigned SlotsWithTooManyDoodads() { return g_tooMany; }
}
