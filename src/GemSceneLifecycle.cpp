#define WIN32_LEAN_AND_MEAN
#include "GemSceneLifecycle.h"
#include "GemBranchRelay.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
using SceneMethod = void (*)(void*);
struct Layout {
    DWORD timestamp, imageSize, initCall, initEntry, destroyCall, destroyEntry;
    std::array<unsigned char, 7> initPrefix;
    std::array<unsigned char, 5> destroyPrefix;
    DWORD lifetimeCall, lifetimeEntry, lifetimeVtable;
};
constexpr Layout layouts[]{
    {0x6afc6d57, 0x1737000, 0xa8b1ac, 0xa39120, 0xa87614, 0xa38940,
     {0x40, 0x55, 0x56, 0x41, 0x57, 0x48, 0x8d}, {0x48, 0x89, 0x5c, 0x24, 0x18},
     0xa7ef34, 0x663360, 0x11237d0},
    {0x6afc6e14, 0x1557000, 0x90bff4, 0x8b8430, 0x907e7b, 0x8b7c50,
     {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08}, {0x48, 0x89, 0x5c, 0x24, 0x08},
     0x8ff129, 0x4d0940, 0xf84738},
};
SceneMethod originalInit = nullptr, originalDestroy = nullptr;
GemSceneLifecycleCallbacks activeCallbacks{};
const unsigned char* installedImage = nullptr;
using EventDispatcher = void (*)(void* event, void* context);
EventDispatcher originalLifetimeDispatcher = nullptr;
GemEntityLifetimeCallback lifetimeCallback = nullptr;
const unsigned char* lifetimeImage = nullptr;
DWORD lifetimeVtableRva = 0;

template <typename T> bool Read(const void* p, T& value) noexcept {
    return ReadProcessMemory(GetCurrentProcess(), p, &value, sizeof(value), nullptr) != FALSE;
}

void InitScene(void* scene) {
    const DWORD incomingError = GetLastError();
    activeCallbacks.beforeInit(scene);
    SetLastError(incomingError);
    originalInit(scene);
    const DWORD nativeError = GetLastError();
    activeCallbacks.afterInit(scene);
    SetLastError(nativeError);
}

void DestroyScene(void* scene) {
    const DWORD incomingError = GetLastError();
    activeCallbacks.beforeDestroy(scene);
    SetLastError(incomingError);
    originalDestroy(scene);
}

void EntityLifetime(void* event, void* context) {
    const DWORD incomingError = GetLastError();
    const auto* bytes = static_cast<const unsigned char*>(event);
    const void* vtable = nullptr;
    DWORD type = 0;
    unsigned char added = 0;
    const void* entity = nullptr;
    if (bytes == nullptr || !Read(bytes, vtable) || vtable != lifetimeImage + lifetimeVtableRva ||
        !Read(bytes + 0x08, type) || type != 0x1056 || !Read(bytes + 0x38, added) || added > 1 ||
        !Read(bytes + 0x40, entity) || entity == nullptr) {
        SetLastError(incomingError);
        lifetimeCallback(nullptr, false);
    } else {
        SetLastError(incomingError);
        lifetimeCallback(entity, added != 0);
    }
    SetLastError(incomingError);
    originalLifetimeDispatcher(event, context);
}

struct Patch {
    unsigned char* call = nullptr;
    unsigned char* relay = nullptr;
    std::array<unsigned char, 5> original{}, replacement{};
    DWORD protection = 0;
    bool writable = false;
};

bool CheckCall(const unsigned char* image, DWORD call, DWORD entry,
               std::array<unsigned char, 5>& bytes) noexcept {
    if (!Read(image + call, bytes) || bytes[0] != 0xe8) return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    return static_cast<std::int64_t>(call) + 5 + displacement == entry;
}

template <typename Hook> bool Prepare(Patch& patch, unsigned char* call, Hook hook) noexcept {
    patch.call = call;
    patch.relay = AllocateGemRelayNear(call + 5);
    if (patch.relay == nullptr) return false;
    constexpr unsigned char absoluteJump[]{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(patch.relay, absoluteJump, sizeof(absoluteJump));
    std::memcpy(patch.relay + sizeof(absoluteJump), &hook, sizeof(hook));
    DWORD ignored = 0;
    if (!VirtualProtect(patch.relay, sizeof(absoluteJump) + sizeof(hook), PAGE_EXECUTE_READ, &ignored) ||
        !FlushInstructionCache(GetCurrentProcess(), patch.relay, sizeof(absoluteJump) + sizeof(hook))) return false;
    const auto delta = reinterpret_cast<std::intptr_t>(patch.relay) - reinterpret_cast<std::intptr_t>(call + 5);
    if (delta < (std::numeric_limits<std::int32_t>::min)() || delta > (std::numeric_limits<std::int32_t>::max)()) return false;
    const auto relative = static_cast<std::int32_t>(delta);
    patch.replacement[0] = 0xe8;
    std::memcpy(patch.replacement.data() + 1, &relative, sizeof(relative));
    return true;
}

const Layout* Select(const unsigned char* image, DWORD timestamp) noexcept {
    const Layout* selected = nullptr;
    for (const auto& entry : layouts) if (entry.timestamp == timestamp) selected = &entry;
    if (selected == nullptr || image == nullptr) return nullptr;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(image, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x100000 ||
        !Read(image + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.TimeDateStamp != timestamp || nt.OptionalHeader.SizeOfImage != selected->imageSize) return nullptr;
    return selected;
}

void Release(Patch& patch) noexcept {
    if (patch.relay != nullptr) VirtualFree(patch.relay, 0, MEM_RELEASE);
}
}

const char* InstallGemSceneLifecycle(unsigned char* image, DWORD timestamp,
                                    GemSceneLifecycleCallbacks callbacks) noexcept {
    if (installedImage != nullptr) return "scene lifecycle hooks already installed";
    if (image == nullptr || callbacks.beforeInit == nullptr || callbacks.afterInit == nullptr ||
        callbacks.beforeDestroy == nullptr) return "scene lifecycle callbacks unavailable";
    const Layout* selected = Select(image, timestamp);
    if (selected == nullptr) return "unexpected scene lifecycle PE identity";

    Patch init{}, destroy{};
    std::array<unsigned char, 7> initPrefix{};
    std::array<unsigned char, 5> destroyPrefix{};
    // Verify both calls and both targets before allocating or writing hooks.
    if (!CheckCall(image, selected->initCall, selected->initEntry, init.original) ||
        !CheckCall(image, selected->destroyCall, selected->destroyEntry, destroy.original) ||
        !Read(image + selected->initEntry, initPrefix) || initPrefix != selected->initPrefix ||
        !Read(image + selected->destroyEntry, destroyPrefix) || destroyPrefix != selected->destroyPrefix)
        return "unexpected scene lifecycle call or entry";
    if (!Prepare(init, image + selected->initCall, &InitScene) ||
        !Prepare(destroy, image + selected->destroyCall, &DestroyScene)) {
        Release(init); Release(destroy);
        return "could not prepare scene lifecycle relays";
    }
    init.writable = VirtualProtect(init.call, 5, PAGE_EXECUTE_READWRITE, &init.protection) != FALSE;
    if (init.writable) destroy.writable = VirtualProtect(destroy.call, 5, PAGE_EXECUTE_READWRITE, &destroy.protection) != FALSE;
    DWORD ignored = 0;
    if (!init.writable || !destroy.writable) {
        if (init.writable) VirtualProtect(init.call, 5, init.protection, &ignored);
        Release(init); Release(destroy);
        return "could not unlock scene lifecycle call pair";
    }
    // Fail before publication if another patcher changed either verified call.
    if (std::memcmp(init.call, init.original.data(), 5) != 0 ||
        std::memcmp(destroy.call, destroy.original.data(), 5) != 0) {
        VirtualProtect(destroy.call, 5, destroy.protection, &ignored);
        VirtualProtect(init.call, 5, init.protection, &ignored);
        Release(init); Release(destroy);
        return "scene lifecycle calls changed during setup";
    }
    originalInit = reinterpret_cast<SceneMethod>(image + selected->initEntry);
    originalDestroy = reinterpret_cast<SceneMethod>(image + selected->destroyEntry);
    activeCallbacks = callbacks;
    std::memcpy(init.call + 1, init.replacement.data() + 1, 4);
    std::memcpy(destroy.call + 1, destroy.replacement.data() + 1, 4);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), init.call, 5) != FALSE &&
                         FlushInstructionCache(GetCurrentProcess(), destroy.call, 5) != FALSE;
    if (!flushed) {
        // Both pages remain writable until the complete pair is committed.
        std::memcpy(init.call + 1, init.original.data() + 1, 4);
        std::memcpy(destroy.call + 1, destroy.original.data() + 1, 4);
        FlushInstructionCache(GetCurrentProcess(), init.call, 5);
        FlushInstructionCache(GetCurrentProcess(), destroy.call, 5);
    }
    VirtualProtect(destroy.call, 5, destroy.protection, &ignored);
    VirtualProtect(init.call, 5, init.protection, &ignored);
    if (!flushed) {
        Release(init); Release(destroy);
        originalInit = nullptr; originalDestroy = nullptr; activeCallbacks = {};
        return "could not flush scene lifecycle call pair";
    }
    installedImage = image;
    return nullptr;
}

const char* InstallGemEntityLifetime(unsigned char* image, DWORD timestamp,
                                    GemEntityLifetimeCallback callback) noexcept {
    if (lifetimeImage != nullptr) return "entity lifetime hook already installed";
    if (callback == nullptr) return "entity lifetime callback unavailable";
    const Layout* selected = Select(image, timestamp);
    if (selected == nullptr) return "unexpected entity lifetime PE identity";
    Patch patch{};
    constexpr std::array<unsigned char, 6> expectedPrefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x20};
    std::array<unsigned char, 6> prefix{};
    if (!CheckCall(image, selected->lifetimeCall, selected->lifetimeEntry, patch.original) ||
        !Read(image + selected->lifetimeEntry, prefix) || prefix != expectedPrefix)
        return "unexpected entity lifetime call or dispatcher";
    if (!Prepare(patch, image + selected->lifetimeCall, &EntityLifetime)) {
        Release(patch);
        return "could not prepare entity lifetime relay";
    }
    if (!VirtualProtect(patch.call, 5, PAGE_EXECUTE_READWRITE, &patch.protection)) {
        Release(patch);
        return "could not unlock entity lifetime call";
    }
    DWORD ignored = 0;
    if (std::memcmp(patch.call, patch.original.data(), 5) != 0) {
        VirtualProtect(patch.call, 5, patch.protection, &ignored);
        Release(patch);
        return "entity lifetime call changed during setup";
    }
    lifetimeCallback = callback;
    lifetimeVtableRva = selected->lifetimeVtable;
    originalLifetimeDispatcher = reinterpret_cast<EventDispatcher>(image + selected->lifetimeEntry);
    lifetimeImage = image;
    std::memcpy(patch.call + 1, patch.replacement.data() + 1, 4);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), patch.call, 5) != FALSE;
    if (!flushed) {
        std::memcpy(patch.call + 1, patch.original.data() + 1, 4);
        FlushInstructionCache(GetCurrentProcess(), patch.call, 5);
    }
    VirtualProtect(patch.call, 5, patch.protection, &ignored);
    if (!flushed) {
        Release(patch); lifetimeImage = nullptr;
        originalLifetimeDispatcher = nullptr; lifetimeCallback = nullptr; lifetimeVtableRva = 0;
        return "could not flush entity lifetime call";
    }
    return nullptr;
}
