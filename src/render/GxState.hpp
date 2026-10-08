// The client's Gx render-state cache, changed the way the client changes it itself (traced
// 2026-10-08, orchestration docs/r&d/immersion/ground-fog-design.md, "Phase 1: verification"):
//  - the cache: [[0xC5DF88] + 0x28F4] + state * 0x18 holds each state's value; usable while
//    [[0xC5DF88] + 0xF58] is non-zero;
//  - IRsDirty (0x685970, __thiscall(gxDevice, state), ret 4) queues the state for the next draw's
//    flush and saves its old value for the current push level, so a later RsPop restores it.
// Gx state 15 = depth writes (dispatched as D3DRS_ZWRITEENABLE).
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::gxstate
{
    constexpr int kDepthWrite = 15;

    /// Sets a Gx state for the next draw: dirty first (saves the old value for the push level),
    /// then the cache. Returns true if it changed something.
    inline bool Force(int state, uint32_t value)
    {
        constexpr uintptr_t kGxDevice = 0x00C5DF88, kIRsDirty = 0x00685970;
        constexpr size_t    kCache = 0x28F4, kStride = 0x18, kReady = 0xF58;
        using IRsDirtyFn = void(__fastcall*)(void* gxDevice, void* edx, int state);

        auto* gxDev = *reinterpret_cast<uint8_t* const*>(kGxDevice);
        if (!gxDev || !*reinterpret_cast<const uint32_t*>(gxDev + kReady)) return false;
        auto* cache = *reinterpret_cast<uint8_t* const*>(gxDev + kCache);
        if (!cache) return false;
        auto* slot = reinterpret_cast<uint32_t*>(cache + state * kStride);
        if (*slot == value) return false;
        reinterpret_cast<IRsDirtyFn>(kIRsDirty)(gxDev, nullptr, state);
        *slot = value;
        return true;
    }
}
