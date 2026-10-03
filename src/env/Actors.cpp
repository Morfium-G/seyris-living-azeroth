#include "Actors.hpp"

#include "game/World.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace wxl_livingazeroth::actors
{
    namespace
    {
        namespace gw = wxl::game::world;

        // Object descriptor (update fields) block, from the object header pointer at +0x08.
        // 3.3.5 (12340) update-field layout: OBJECT_END = 6 fields, then ... RANGEDATTACKTIME at
        // OBJECT_END + 0x3A, BOUNDINGRADIUS + 0x3B, COMBATREACH + 0x3C, DISPLAYID + 0x3D,
        // NATIVEDISPLAYID + 0x3E, MOUNTDISPLAYID + 0x3F. Byte offset = (6 + n) * 4.
        // A first guess one field early read the ranged attack time (int) as the radius and the
        // native display id as the mount -- both symptoms seen in-client, 2026-10-01.
        constexpr size_t kObjectDescriptors   = 0x08;
        constexpr size_t kFieldBoundingRadius = 0x104;
        constexpr size_t kFieldCombatReach    = 0x108;
        constexpr size_t kFieldDisplayId      = 0x10C;
        constexpr size_t kFieldNativeDisplay  = 0x110;
        constexpr size_t kFieldMountDisplayId = 0x114;

        // Client DB storages (core offsets/game/DB2.hpp gives the storage objects; the generic storage
        // layout -- +0x0C max id, +0x10 min id, +0x20 id table -- matches Map, AreaTable and ChrRaces).
        constexpr uintptr_t kDisplayInfoStorage = 0x00AD34B8;
        constexpr uintptr_t kModelDataStorage   = 0x00AD3500;
        constexpr size_t    kStorageMaxId = 0x0C, kStorageMinId = 0x10, kStorageIdTable = 0x20;
        // Row columns (3.3.5 DBC order; strings are resolved pointers, other columns raw):
        constexpr size_t kDisplayModelId    = 0x04; // CreatureDisplayInfo.ModelID (core-confirmed)
        constexpr size_t kDisplayModelScale = 0x10; // CreatureDisplayInfo.CreatureModelScale [believed]
        constexpr size_t kModelScale        = 0x10; // CreatureModelData.ModelScale [believed]
        constexpr size_t kModelCollisionW   = 0x38; // CreatureModelData.CollisionWidth [believed]

        // Footprints, as the client's own step code reads them (XWorkbench 2026-10-03). Model setup
        // (0x73E5C0) copies from the unit's CreatureModelData row (unit +0x970): +0x18
        // FootprintTextureID -> unit +0xA3C, +0x20 FootprintTextureWidth / 36 -> +0xA44, +0x1C
        // FootprintTextureLength / 36 -> +0xA48 (the columns are in inches; 1/36 at 0xA3F854). The
        // step (0x71FB60) multiplies them by the unit's scale (virtual +0x7C); while mounted
        // (+0x9C0 > 0, +0x98C set, not +0xA30 & 0x10000000) the mount's copies at +0xAD8/+0xADC/+0xAE0
        // are used instead. A texture ID outside FootprintTextures (-1) makes no print.
        constexpr size_t   kUnitFootTexture = 0xA3C, kUnitFootWidth = 0xA44, kUnitFootLength = 0xA48;
        constexpr size_t   kUnitMountFootTexture = 0xAD8, kUnitMountFootWidth = 0xADC, kUnitMountFootLength = 0xAE0;
        constexpr size_t   kUnitMountState = 0x9C0, kUnitMountModel = 0x98C, kUnitFlags = 0xA30;
        constexpr uint32_t kUnitFlagNoMountFootprint = 0x10000000;
        constexpr size_t   kVtScale = 0x7C / 4;
        using ScaleFn = float(__fastcall*)(void* unit, void* edx);

        bool ReadFootprint(void* obj, int32_t& texture, float& length, float& width)
        {
            __try
            {
                const auto* u = static_cast<const uint8_t*>(obj);
                const bool mounted = *reinterpret_cast<const int32_t*>(u + kUnitMountState) > 0 &&
                                     !(*reinterpret_cast<const uint32_t*>(u + kUnitFlags) & kUnitFlagNoMountFootprint) &&
                                     *reinterpret_cast<const uint32_t*>(u + kUnitMountModel) != 0;
                texture = *reinterpret_cast<const int32_t*>(u + (mounted ? kUnitMountFootTexture : kUnitFootTexture));
                const float w = *reinterpret_cast<const float*>(u + (mounted ? kUnitMountFootWidth : kUnitFootWidth));
                const float l = *reinterpret_cast<const float*>(u + (mounted ? kUnitMountFootLength : kUnitFootLength));
                const auto* vtable = *reinterpret_cast<void* const* const*>(u);
                const float scale = vtable ? reinterpret_cast<ScaleFn>(vtable[kVtScale])(obj, nullptr) : 1.0f;
                width = w * scale;
                length = l * scale;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        std::unordered_map<uint32_t, float> g_widthCache; // display id -> effective collision width

        std::vector<Actor> g_actors;

        bool ReadU32(uintptr_t address, uint32_t& out)
        {
            __try { out = *reinterpret_cast<const uint32_t*>(address); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        float BitsToFloat(uint32_t bits) { float f; memcpy(&f, &bits, sizeof(f)); return f; }

        uint32_t Row(uintptr_t storage, uint32_t id)
        {
            uint32_t maxId = 0, minId = 0, table = 0, row = 0;
            if (!ReadU32(storage + kStorageMaxId, maxId) || !ReadU32(storage + kStorageMinId, minId)) return 0;
            if (!ReadU32(storage + kStorageIdTable, table) || !table) return 0;
            if (static_cast<int32_t>(id) < static_cast<int32_t>(minId) || static_cast<int32_t>(id) > static_cast<int32_t>(maxId)) return 0;
            return ReadU32(table + (id - minId) * 4, row) ? row : 0;
        }

        // Collision width of a display: model width x model scale x display scale. 0 when unknown.
        float EffectiveWidth(uint32_t displayId)
        {
            if (!displayId) return 0.0f;
            if (auto it = g_widthCache.find(displayId); it != g_widthCache.end()) return it->second;

            float width = 0.0f;
            uint32_t display = Row(kDisplayInfoStorage, displayId), modelId = 0, dScale = 0;
            if (display && ReadU32(display + kDisplayModelId, modelId) && ReadU32(display + kDisplayModelScale, dScale))
            {
                uint32_t model = Row(kModelDataStorage, modelId), mScale = 0, w = 0;
                if (model && ReadU32(model + kModelScale, mScale) && ReadU32(model + kModelCollisionW, w))
                {
                    const float ds = BitsToFloat(dScale), ms = BitsToFloat(mScale);
                    width = BitsToFloat(w) * (ds > 0.0f ? ds : 1.0f) * (ms > 0.0f ? ms : 1.0f);
                    if (!(width > 0.0f && width < 100.0f)) width = 0.0f;
                }
            }
            g_widthCache[displayId] = width;
            return width;
        }
    }

    void Refresh(const float center[3], float range)
    {
        g_actors.clear();
        const float range2 = range * range;
        const unsigned long long playerGuid = gw::ActivePlayerGuid();

        gw::ForEachObject(gw::kTypeMaskUnit, [&](unsigned long long guid, void* obj) {
            if (!obj) return true;
            Actor a;
            a.guid = guid;
            gw::Position(obj, a.pos);
            const float dx = a.pos[0] - center[0], dy = a.pos[1] - center[1], dz = a.pos[2] - center[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > range2) return true;
            a.distance = std::sqrt(d2);
            a.isPlayer = guid == playerGuid;

            uint32_t descriptors = 0, radiusBits = 0, reachBits = 0, mount = 0;
            if (ReadU32(reinterpret_cast<uintptr_t>(obj) + kObjectDescriptors, descriptors) && descriptors)
            {
                if (ReadU32(descriptors + kFieldBoundingRadius, radiusBits)) a.boundingRadius = BitsToFloat(radiusBits);
                if (ReadU32(descriptors + kFieldCombatReach, reachBits)) a.combatReach = BitsToFloat(reachBits);
                if (ReadU32(descriptors + kFieldMountDisplayId, mount)) a.mounted = mount != 0;
                ReadU32(descriptors + kFieldDisplayId, a.displayId);
                ReadU32(descriptors + kFieldNativeDisplay, a.nativeDisplayId);
            }
            // Guard against garbage if the field layout were ever off.
            if (!(a.boundingRadius > 0.01f && a.boundingRadius < 20.0f)) a.boundingRadius = 0.5f;

            // Morphs and shapeshifts change only the display: the server keeps the old radius. Scale it
            // by how much wider the current model is than the native one.
            a.effectiveRadius = a.boundingRadius;
            if (a.displayId && a.nativeDisplayId && a.displayId != a.nativeDisplayId)
            {
                a.widthNow = EffectiveWidth(a.displayId);
                a.widthNative = EffectiveWidth(a.nativeDisplayId);
                if (a.widthNow > 0.0f && a.widthNative > 0.0f)
                {
                    float ratio = a.widthNow / a.widthNative;
                    ratio = ratio < 0.25f ? 0.25f : (ratio > 6.0f ? 6.0f : ratio);
                    a.effectiveRadius = a.boundingRadius * ratio;
                }
            }
            int32_t footTexture = 0;
            float footLength = 0.0f, footWidth = 0.0f;
            if (ReadFootprint(obj, footTexture, footLength, footWidth))
            {
                a.hasFootprint = footTexture >= 0;
                // Guard against garbage (a field layout off, a half-built model).
                if (std::isfinite(footLength) && std::isfinite(footWidth) && footLength >= 0.0f && footLength < 20.0f && footWidth >= 0.0f && footWidth < 20.0f)
                { a.footprintLength = footLength; a.footprintWidth = footWidth; }
            }
            g_actors.push_back(a);
            return true;
        });

        std::sort(g_actors.begin(), g_actors.end(),
                  [](const Actor& x, const Actor& y) { return x.distance < y.distance; });
    }

    const std::vector<Actor>& Nearby() { return g_actors; }
}
