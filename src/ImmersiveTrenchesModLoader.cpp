#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cwchar>

static_assert(sizeof(void*) == 8, "Build the Miles plugin for x64.");

namespace {
void ReportLoadError(const char* operation, DWORD error) noexcept {
    char message[128];
    std::snprintf(message, sizeof(message), "ImmersiveTrenchesMod.asi: %s failed (Win32 %lu)\r\n",
                  operation, error);
    OutputDebugStringA(message);
}

// Resolve paths from the ASI itself, not the current directory: Miles and F5
// can start the process with different working directories.
void LoadMod() noexcept {
    HMODULE loader = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadMod), &loader)) {
        ReportLoadError("GetModuleHandleExW", GetLastError());
        return;
    }

    wchar_t dllPath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(loader, dllPath, ARRAYSIZE(dllPath));
    if (length == 0 || length >= ARRAYSIZE(dllPath)) {
        ReportLoadError("GetModuleFileNameW", GetLastError());
        return;
    }

    // Strip the ASI filename and the mss, x64, and binaries directories.
    wchar_t* gameRootEnd = nullptr;
    for (int component = 0; component < 4; ++component) {
        gameRootEnd = std::wcsrchr(dllPath, L'\\');
        if (gameRootEnd == nullptr) {
            ReportLoadError("Resolve game root from ASI path", ERROR_BAD_PATHNAME);
            return;
        }
        *gameRootEnd = L'\0';
    }

    constexpr wchar_t logicPath[] = L"\\mods\\ImmersiveTrenches\\ImmersiveTrenchesModLogic.dll";
    if (gameRootEnd - dllPath > ARRAYSIZE(dllPath) - ARRAYSIZE(logicPath)) {
        ReportLoadError("Resolve ImmersiveTrenchesModLogic.dll path", ERROR_FILENAME_EXCED_RANGE);
        return;
    }
    CopyMemory(gameRootEnd, logicPath, sizeof(logicPath));

    // Restrict dependency lookup to the logic DLL directory and Windows system
    // directory; the game root is already encoded in the absolute DLL path.
    const HMODULE logic = LoadLibraryExW(dllPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (logic == nullptr) {
        ReportLoadError("LoadLibraryExW(ImmersiveTrenchesModLogic.dll)", GetLastError());
        return;
    }

    using InitializeMod = BOOL (WINAPI*)();
    const auto initialize = reinterpret_cast<InitializeMod>(GetProcAddress(logic, "GoHModInitialize"));
    if (initialize == nullptr) {
        ReportLoadError("GetProcAddress(GoHModInitialize)", GetLastError());
    } else if (!initialize()) {
        ReportLoadError("GoHModInitialize", GetLastError());
    }
    // Release this load reference. Successful initialization pins the logic DLL,
    // so installed callbacks remain valid even when Miles unloads the ASI.
    FreeLibrary(logic);
}
}

// Keep loader-lock work empty; initialization belongs to the Miles callback.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) noexcept {
    return TRUE;
}

// Miles calls this on attach/detach. Only attach loads and initializes logic.
extern "C" __declspec(dllexport) int WINAPI RIB_Main(
    void*, unsigned int upDown, void*, void*, void*) noexcept {
    if (upDown == 1) {
        LoadMod();
    }
    return 1;
}
