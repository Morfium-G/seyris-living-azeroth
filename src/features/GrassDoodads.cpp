#include "GrassDoodads.hpp"

#include "GrassInstanced.hpp"
#include "GrassPerf.hpp"

#include "../wxl_seyris/CdbcApi.hpp"

#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

        // Colour bytes: blue 0..7, green 8..15, red 16..23 (red/blue may swap for the GPU; green
        // doesn't move, and the high bits go into red AND blue so the swap is harmless).
        constexpr size_t   kInstanceColor = 0x28;

        bool WriteU32(uintptr_t address, uint32_t value)
        {
            __try { *reinterpret_cast<uint32_t*>(address) = value; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        const WXL_Api* g_api = nullptr;
        SlotFn g_origFill = nullptr;
        SlotFn g_origDraw = nullptr;

        std::unordered_map<uint32_t, DoodadInfo>    g_doodads;
        std::unordered_map<const void*, SlotDoodads> g_slots;
        std::unordered_map<uint32_t, Override> g_overrides;

        constexpr const char* kOverrideFile = "DBFilesClient\\GroundEffectDoodadWind.cdbc";
        constexpr WXL_SeyrisCdbcField kOverrideFields[] = {
            {"ID",        0, WXL_CDBC_FIELD_VALUE},
            {"Flags",     1, WXL_CDBC_FIELD_VALUE},
            {"Stiffness", 2, WXL_CDBC_FIELD_VALUE},
            {"RootV",     3, WXL_CDBC_FIELD_VALUE},
            {"TipV",      4, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kOverrideDef = {
            "GroundEffectDoodadWind", kOverrideFile, kOverrideFields, 5,
        };

        float    BitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }
        uint32_t FloatToBits(float f)    { uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b; }
        unsigned g_tooMany = 0;

        constexpr unsigned kHighlightReg  = 152;   // {entry index or -1, lift in yards, 0, 0}
        constexpr float    kHighlightLift = 1.5f;
        uint32_t           g_highlightId = 0;
        unsigned           g_drawsTracked = 0, g_drawsUntracked = 0;

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

        // Effective values: automatic, unless an override row exists for this doodad.
        void ApplyOverrides(DoodadInfo& d)
        {
            d.effRootV = d.rootV;
            d.effTipV = d.tipV;
            auto it = g_overrides.find(d.id);
            d.hasOverride = it != g_overrides.end();
            if (!d.hasOverride)
            {
                d.windOn = d.valid && !d.autoFlat;
                d.pushOn = d.valid && !d.autoFlat;
                d.flipped = false;
                d.stiffness = 0.0f;
                return;
            }
            const Override& o = it->second;
            if (o.rootV >= 0.0f) d.effRootV = o.rootV;
            if (o.tipV >= 0.0f)  d.effTipV = o.tipV;
            // A manual mapping makes the doodad usable even if the automatic one failed.
            const bool usable = d.valid || (o.rootV >= 0.0f && o.tipV >= 0.0f);
            d.windOn = usable && !(o.flags & kNoWind);
            d.pushOn = usable && !(o.flags & kNoPush);
            d.flipped = (o.flags & kFlip) != 0;
            d.stiffness = o.stiffness < 0.0f ? 0.0f : (o.stiffness > 1.0f ? 1.0f : o.stiffness);
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

        // Around the client's (re)build of a slot's grass: tag every instance's colour with its entry
        // index (green low bits, copied into all its vertices by the build), then restore the
        // colours. Also records the slot's doodads and analyses new ones.
        void __fastcall hkFill(void* slot, void* edx)
        {
            uint32_t count = 0, instances = 0;
            const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            if (!ReadU32(s + kSlotInstanceCount, count) || !ReadU32(s + kSlotInstances, instances) || !instances
                || count > 65536)
            {
                g_origFill(slot, edx);
                return;
            }

            SlotDoodads sd{};
            bool overflow = false;
            std::vector<uint32_t> savedColors(count);
            uint32_t tagged = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uintptr_t rec = instances + i * kInstanceStride;
                uint32_t id = 0, color = 0;
                if (!ReadU32(rec + kInstanceDoodadId, id) || !ReadU32(rec + kInstanceColor, color)) break;

                const unsigned index = EntryFor(sd, id, overflow);
                savedColors[i] = color;
                if (!WriteU32(rec + kInstanceColor, (color & ~kIndexMask) | IndexTag(index))) break;
                ++tagged;
            }

            grassinst::BeforeStockFill(slot);
            const double t0 = grassperf::Now();
            g_origFill(slot, edx);
            uint32_t vertices = 0;
            ReadU32(s + 0x08, vertices); // slot vertex count
            grassperf::OnBuild(grassperf::Now() - t0, count, vertices);
            grassinst::AfterStockFill(slot); // still tagged: the verification compares against this

            for (uint32_t i = 0; i < tagged; ++i)
                WriteU32(instances + i * kInstanceStride + kInstanceColor, savedColors[i]);

            NoteSlot(sd, overflow);
            if (g_slots.size() > 50000) g_slots.clear(); // slots get reused; don't grow forever
            g_slots[slot] = sd;
        }

        // Before each layer draw: hand the shader this slot's doodad table, one register per entry:
        // {rootV, 1/(tipV-rootV), windScale, pushScale}. The fallback entry reproduces the old rule
        // (root at v = 1, tip at v = 0, full wind and push); unused entries get it too.
        void __fastcall hkDraw(void* slot, void* edx)
        {
            float table[kEntries][4];
            for (unsigned k = 0; k < kEntries; ++k)
            {
                table[k][0] = 1.0f; table[k][1] = -1.0f; table[k][2] = 1.0f; table[k][3] = 1.0f;
            }

            // Instanced renderer: the slot's plants sit in our own static buffers (built on first
            // sight), so the client's per-frame re-bake is skipped. Null = this slot draws stock.
            const SlotDoodads* sd = grassinst::Prepare(slot);
            const bool instanced = sd != nullptr;
            if (!sd)
            {
                auto it = g_slots.find(slot);
                if (it != g_slots.end()) sd = &it->second;
            }

            if (sd)
            {
                for (unsigned k = 0; k < sd->count; ++k)
                {
                    auto d = g_doodads.find(sd->ids[k]);
                    if (d == g_doodads.end()) continue;
                    DoodadInfo& info = d->second;
                    if (!info.analyzed) Analyze(info.id); // its model may have loaded since
                    if (!info.analyzed || (!info.valid && !info.hasOverride)) continue; // fallback rule

                    const float rootV = info.flipped ? info.effTipV : info.effRootV;
                    const float tipV  = info.flipped ? info.effRootV : info.effTipV;
                    const float span = tipV - rootV;
                    const float flex = 1.0f - info.stiffness; // stiff plants bend less from anything
                    table[k][0] = rootV;
                    table[k][1] = std::abs(span) > 1e-4f ? 1.0f / span : 0.0f;
                    table[k][2] = info.windOn ? flex : 0.0f;
                    table[k][3] = info.pushOn ? flex : 0.0f;
                }
            }

            if (sd) ++g_drawsTracked; else ++g_drawsUntracked;

            // Debug highlight: which entry (if any) of this slot is the highlighted doodad.
            float highlight[4] = { -1.0f, kHighlightLift, 0.0f, 0.0f };
            if (g_highlightId == kHighlightFallbackId)
                highlight[0] = static_cast<float>(kFallbackEntry);
            else if (g_highlightId && sd)
                for (unsigned k = 0; k < sd->count; ++k)
                    if (sd->ids[k] == g_highlightId) highlight[0] = static_cast<float>(k);

            if (auto* dev = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice()))
            {
                dev->SetVertexShaderConstantF(kFirstReg, &table[0][0], kEntries);
                dev->SetVertexShaderConstantF(kHighlightReg, highlight, 1);
            }

            const double t0 = grassperf::Now();
            if (instanced && grassinst::Draw(slot))
            {
                grassperf::OnDraw(grassperf::Now() - t0);
                return;
            }
            g_origDraw(slot, edx);
            grassperf::OnDraw(grassperf::Now() - t0);
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
        // The instanced renderer runs inside these two hooks.
        grassinst::Install(api);
        return a && b;
    }

    void NoteSlot(const SlotDoodads& sd, bool overflow)
    {
        if (overflow) ++g_tooMany;
        for (unsigned k = 0; k < sd.count; ++k)
        {
            Analyze(sd.ids[k]);
            ++g_doodads[sd.ids[k]].seenInSlots;
        }
    }

    std::vector<DoodadInfo> Seen()
    {
        std::vector<DoodadInfo> out;
        out.reserve(g_doodads.size());
        for (const auto& [id, d] : g_doodads) out.push_back(d);
        std::sort(out.begin(), out.end(), [](const DoodadInfo& x, const DoodadInfo& y) { return x.id < y.id; });
        return out;
    }

    void SetOverride(uint32_t id, const Override& o)
    {
        g_overrides[id] = o;
        if (auto it = g_doodads.find(id); it != g_doodads.end()) ApplyOverrides(it->second);
    }

    void ClearOverride(uint32_t id)
    {
        g_overrides.erase(id);
        if (auto it = g_doodads.find(id); it != g_doodads.end()) ApplyOverrides(it->second);
    }

    bool GetOverride(uint32_t id, Override& out)
    {
        auto it = g_overrides.find(id);
        if (it == g_overrides.end()) return false;
        out = it->second;
        return true;
    }

    Override CurrentAsOverride(uint32_t id)
    {
        Override o;
        if (GetOverride(id, o)) return o;
        auto it = g_doodads.find(id);
        if (it != g_doodads.end())
        {
            const DoodadInfo& d = it->second;
            if (!d.windOn) o.flags |= kNoWind;
            if (!d.pushOn) o.flags |= kNoPush;
        }
        return o;
    }

    void LoadOverrides(const void* cdbcApi)
    {
        const auto* cdbc = static_cast<const WXL_SeyrisCdbcApi*>(cdbcApi);
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) return;

        char err[256] = {};
        void* table = cdbc->Load(&kOverrideDef, err, sizeof(err));
        if (!table)
        {
            g_api->Log(WXL_LOG_INFO, kTag, "grass doodads: no overrides loaded (%s).", err);
            return;
        }
        g_overrides.clear();
        const uint32_t count = cdbc->RowCount(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            const void* row = cdbc->RowAt(table, i);
            if (!row) continue;
            Override o;
            const uint32_t id = cdbc->Value(table, row, "ID", 0);
            o.flags     = cdbc->Value(table, row, "Flags", 0);
            o.stiffness = BitsToFloat(cdbc->Value(table, row, "Stiffness", 0));
            o.rootV     = BitsToFloat(cdbc->Value(table, row, "RootV", 0));
            o.tipV      = BitsToFloat(cdbc->Value(table, row, "TipV", 0));
            g_overrides[id] = o;
        }
        cdbc->Release(table);
        for (auto& [id, d] : g_doodads) ApplyOverrides(d);
        g_api->Log(WXL_LOG_INFO, kTag, "grass doodads: %u override row(s) loaded from GroundEffectDoodadWind.cdbc.", count);
    }

    bool SaveOverrides(char* message, size_t messageSize)
    {
        // Plain WDBC: 20-byte header, 5 x 4-byte columns per row sorted by ID, an empty string block.
        std::vector<uint32_t> ids;
        for (const auto& [id, o] : g_overrides) ids.push_back(id);
        std::sort(ids.begin(), ids.end());

        std::vector<uint8_t> file;
        auto put = [&](uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); file.insert(file.end(), b, b + 4); };
        file.insert(file.end(), { 'W', 'D', 'B', 'C' });
        put(static_cast<uint32_t>(ids.size())); // records
        put(5);                                 // fields
        put(20);                                // record size
        put(1);                                 // string block size
        for (uint32_t id : ids)
        {
            const Override& o = g_overrides[id];
            put(id); put(o.flags); put(FloatToBits(o.stiffness)); put(FloatToBits(o.rootV)); put(FloatToBits(o.tipV));
        }
        file.push_back(0); // string block: just the empty string

        CreateDirectoryA("DBFilesClient", nullptr);
        const std::string backup = std::string(kOverrideFile) + ".bak";
        CopyFileA(kOverrideFile, backup.c_str(), FALSE); // ignore failure: no previous file

        FILE* f = nullptr;
        if (fopen_s(&f, kOverrideFile, "wb") != 0 || !f)
        {
            std::snprintf(message, messageSize, "couldn't open %s for writing", kOverrideFile);
            return false;
        }
        const size_t written = std::fwrite(file.data(), 1, file.size(), f);
        std::fclose(f);
        if (written != file.size())
        {
            std::snprintf(message, messageSize, "short write to %s", kOverrideFile);
            return false;
        }
        std::snprintf(message, messageSize, "saved %u row(s) to %s", static_cast<unsigned>(ids.size()), kOverrideFile);
        g_api->Log(WXL_LOG_INFO, kTag, "grass doodads: %s", message);
        return true;
    }

    unsigned OverrideCount() { return static_cast<unsigned>(g_overrides.size()); }

    void     SetHighlight(uint32_t id) { g_highlightId = id; }
    uint32_t Highlighted()             { return g_highlightId; }

    void FrameCounters(unsigned& tracked, unsigned& untracked)
    {
        tracked = g_drawsTracked;
        untracked = g_drawsUntracked;
        g_drawsTracked = g_drawsUntracked = 0;
    }

    unsigned SlotsTracked()            { return static_cast<unsigned>(g_slots.size()); }
    unsigned SlotsWithTooManyDoodads() { return g_tooMany; }
}
