#pragma once

#include <windows.h>

// These events delimit native scene ownership. Init completion does not certify
// that terrain, rendering, contact samples or a fitted-cap backend are ready.
struct GemSceneLifecycleCallbacks {
    void (*beforeInit)(void* scene) noexcept = nullptr;
    void (*afterInit)(void* scene) noexcept = nullptr;
    void (*beforeDestroy)(void* scene) noexcept = nullptr;
};

// Installs the two verified rel32 call sites only as a complete pair. Failure
// leaves the original calls in place. Called once during DLL initialization.
const char* InstallGemSceneLifecycle(unsigned char* image, DWORD timestamp,
                                    GemSceneLifecycleCallbacks callbacks) noexcept;

// Observes the common native add/remove notification after ID-tree mutation,
// before its original dispatcher runs. The adapter must not dereference the
// entity in this callback. nullptr,false means an unreadable/invalid event:
// invalidate all current leases until the next proven scene initialization.
using GemEntityLifetimeCallback = void (*)(const void* entity, bool added) noexcept;
const char* InstallGemEntityLifetime(unsigned char* image, DWORD timestamp,
                                    GemEntityLifetimeCallback callback) noexcept;
