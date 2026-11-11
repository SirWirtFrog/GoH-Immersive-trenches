#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

// A five-byte x64 call/jump reaches only a signed 32-bit displacement.
// Find a nearby page for an indirect absolute jump to the DLL hook.
// The caller writes the stub and changes this RW page to executable/read-only.
inline unsigned char* AllocateGemRelayNear(const void* branchEnd) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto address = reinterpret_cast<std::uintptr_t>(branchEnd);
    constexpr std::uintptr_t reach = 0x7fff0000;
    const auto minimum = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    const auto lower = address > reach ? (std::max)(minimum, address - reach) : minimum;
    const auto upper = (std::min)(maximum, address + reach);
    unsigned char* relay = nullptr;
    for (auto cursor = lower; cursor < upper && relay == nullptr;) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) == 0) {
            break;
        }
        const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        // VirtualAlloc reservations require allocation-granularity alignment,
        // which is coarser than the page size used for the relay itself.
        const auto aligned = (cursor + granularity - 1) & ~(granularity - 1);
        if (region.State == MEM_FREE && aligned < upper && aligned < end && end - aligned >= info.dwPageSize) {
            relay = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(aligned), info.dwPageSize,
                                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        }
        cursor = end;
    }
    return relay;
}
