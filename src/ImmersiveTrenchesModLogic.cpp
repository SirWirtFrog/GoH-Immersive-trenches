#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstddef>

#include "GemTrenchSnap.h"

static_assert(sizeof(void*) == 8, "Build the mod logic for x64.");

void InstallTrenchMenu(unsigned char* host, bool isEditor, void (*log)(const char*)) noexcept;

extern "C" {
void* GemConstructionOriginal[7]{};
void GemTraceInstall();
void GemTraceInstallBegin();
void GemTraceInstallEnd();
void GemTraceSectionCreated(void* instruction, void* builder, void* entity);
void GemTraceExitSpawn();
void GemTracePlacementPreview(void* action, void* output, void* selection,
                              const TrenchTarget* target, const void* options);
void GemTracePlacementOrder(void* action, void* selection, const TrenchTarget* target,
                            const void* options, void* context);
}

namespace {
INIT_ONCE initializeOnce = INIT_ONCE_STATIC_INIT;

void WriteLog(const char* message) noexcept {
    OutputDebugStringA(message);

    constexpr wchar_t logDir[] = L"GoH";
    constexpr wchar_t logName[] = L"\\ImmersiveTrenchesMod.log";
    wchar_t path[MAX_PATH];
    const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    constexpr auto suffixLength = ARRAYSIZE(logDir) - 1 + ARRAYSIZE(logName);
    if (length == 0 || length > ARRAYSIZE(path) - suffixLength) {
        return;
    }
    CopyMemory(path + length, logDir, sizeof(logDir));
    if (!CreateDirectoryW(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    CopyMemory(path + length + ARRAYSIZE(logDir) - 1, logName, sizeof(logName));

    const HANDLE log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(log, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
        CloseHandle(log);
    }
}

void LogInitialization() noexcept {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "ImmersiveTrenchesModLogic initialized from Miles RIB_Main(upDown=1), pid=%lu\r\n",
                  GetCurrentProcessId());
    WriteLog(message);
}

struct ConstructionLayout {
    const char* host;
    DWORD timestamp;
    DWORD imageSize;
    DWORD slots[4];
    DWORD originals[4];
    DWORD spawnSlot;
    DWORD spawnOriginal;
    DWORD previewSlot;
    DWORD previewOriginal;
    DWORD orderSlot;
    DWORD orderOriginal;
    DWORD entityTree;
    DWORD queueCall;
    DWORD queueEntry;
    DWORD orderVtable;
    DWORD orderDestructor;
    DWORD previewBounds;
    DWORD sceneQuantJump;
    DWORD sceneQuantEntry;
};

// Execution slots for the three install instructions, followed by the
// eInstruction_install callback reached only after an entity has been created.
constexpr ConstructionLayout constructionLayouts[]{
    {"call_to_arms.exe", 0x6ab3dcc7, 0x1707000,
     {0x11203e0, 0x1120420, 0x1120468, 0x11203f0}, {0xb63070, 0xb63070, 0xb63630, 0xb63020},
     0x10d4ce8, 0x7538f0, 0x111fa80, 0x1b08e0, 0x111fa58, 0x1b0200,
     0x159c390, 0x1b07d7, 0x7efa10, 0x10e8a38, 0x8ea070, 0x984f70, 0xa1d859, 0xa1b320},
    {"call_to_arms_ed.exe", 0x6ab3dd83, 0x1533000,
     {0xfa14f8, 0xfa1530, 0xfa1570, 0xfa1500}, {0xa927c0, 0xa927c0, 0xa92c80, 0xa92770},
     0xf36ba8, 0x5d0930, 0xfa0b90, 0x139ac0, 0xfa0b68, 0x139420,
     0x1404120, 0x1399f7, 0x671b00, 0xf4e030, 0x770260, 0x80d9e0, 0x8a5169, 0x8a2fb0},
};

bool HasEntityName(const void* object, SIZE_T offset, const char* expected) noexcept {
    if (object == nullptr) {
        return false;
    }
    // The verified engine paths read an MSVC std::string at the supplied offset.
    struct EntityName {
        char storage[16];
        SIZE_T size;
        SIZE_T capacity;
    };
    EntityName name{};
    char actual[64];
    const SIZE_T length = std::strlen(expected);
    const auto* field = static_cast<const unsigned char*>(object) + offset;
    const HANDLE process = GetCurrentProcess();
    if (!ReadProcessMemory(process, field, &name, sizeof(name), nullptr) ||
        name.size != length || name.capacity < name.size || length > sizeof(actual)) {
        return false;
    }
    if (name.capacity < sizeof(name.storage)) {
        return std::memcmp(name.storage, expected, length) == 0;
    }
    const char* data = nullptr;
    std::memcpy(&data, name.storage, sizeof(data));
    return ReadProcessMemory(process, data, actual, length, nullptr) &&
           std::memcmp(actual, expected, length) == 0;
}

constexpr char straightTrench[] = "it_trench_straight_with_exits";
#include "GemTrenchCatalog.inc"

const TrenchModule* FindTrenchModule(const void* object, SIZE_T nameOffset) noexcept {
    for (const auto& module : trenchModules) {
        if (HasEntityName(object, nameOffset, nameOffset == 0x890 ? module.previewName : module.entityName)) {
            return &module;
        }
    }
    return nullptr;
}

struct GemPreviewBounds {
    void* vtable;
    TrenchVector min, max;
};
static_assert(offsetof(GemPreviewBounds, min) == 0x08 && offsetof(GemPreviewBounds, max) == 0x14);
using ReadBounds = GemPreviewBounds* (*)(GemPreviewBounds*, const void*);
ReadBounds readPreviewBounds = nullptr;

float PreviewYawOffset(const void* action) {
    GemPreviewBounds bounds{};
    const auto* bytes = static_cast<const unsigned char*>(action);
    readPreviewBounds(&bounds, bytes + 0x890);
    float rotation = 0.0f;
    std::memcpy(&rotation, bytes + 0x8bc, sizeof(rotation));
    constexpr float degreesPerRadian = 57.2957795f;
    // eFortify adds a quarter turn when the model's Y extent exceeds its X extent.
    const float quarterTurn = bounds.max.y - bounds.min.y > bounds.max.x - bounds.min.x ? 90.0f : 0.0f;
    return rotation * degreesPerRadian + quarterTurn;
}

void InstallPlacementTrace(unsigned char* image, const ConstructionLayout& layout) noexcept {
    // eFortify::Action vtable, shared by the three modded fortification actions.
    auto** preview = reinterpret_cast<void**>(image + layout.previewSlot);
    auto** order = reinterpret_cast<void**>(image + layout.orderSlot);
    void* const previewOriginal = image + layout.previewOriginal;
    void* const orderOriginal = image + layout.orderOriginal;
    if (*preview != previewOriginal || *order != orderOriginal) {
        WriteLog("[GEM placement] Trace skipped: unexpected action table.\r\n");
        return;
    }

    const SIZE_T span = layout.previewSlot - layout.orderSlot + sizeof(void*);
    DWORD protection = 0;
    if (!VirtualProtect(order, span, PAGE_READWRITE, &protection)) {
        WriteLog("[GEM placement] Trace skipped: could not unlock action table.\r\n");
        return;
    }

    GemConstructionOriginal[5] = previewOriginal;
    GemConstructionOriginal[6] = orderOriginal;
    readPreviewBounds = reinterpret_cast<ReadBounds>(image + layout.previewBounds);
    if (const auto* failure = InitializeTrenchSnap(image, layout.timestamp, layout.entityTree, layout.queueCall,
                                                   layout.queueEntry, layout.orderVtable, layout.orderDestructor,
                                                   WriteLog)) {
        DWORD ignored = 0;
        VirtualProtect(order, span, protection, &ignored);
        char message[192];
        std::snprintf(message, sizeof(message), "[GEM placement] Placement guards skipped: %s.\r\n", failure);
        WriteLog(message);
        return;
    }
    void* const previewHook = reinterpret_cast<void*>(&GemTracePlacementPreview);
    void* const orderHook = reinterpret_cast<void*>(&GemTracePlacementOrder);
    bool installed = false;
    if (InterlockedCompareExchangePointer(preview, previewHook, previewOriginal) == previewOriginal) {
        installed = InterlockedCompareExchangePointer(order, orderHook, orderOriginal) == orderOriginal;
        if (!installed) {
            InterlockedCompareExchangePointer(preview, previewOriginal, previewHook);
        }
    }
    DWORD ignoredProtection = 0;
    if (!VirtualProtect(order, span, protection, &ignoredProtection)) {
        WriteLog("[GEM placement] Could not restore action table protection.\r\n");
    }
    WriteLog(installed ? "[GEM placement] Trench snapping, cut validation and queue reservations enabled.\r\n"
                       : "[GEM placement] Trace skipped: action table changed during setup.\r\n");
    if (installed) {
        WriteLog(InstallTrenchHeightRefresh(image, layout.sceneQuantJump, layout.sceneQuantEntry)
                     ? "[GEM caps] Post-scene height refresh enabled.\r\n"
                     : "[GEM caps] Post-scene height refresh skipped: scene jump unavailable.\r\n");
    }
}

void LogPlacement(const char* stage, bool preview, const TrenchModule* module,
                  const void* target, const void* options, const void* output = nullptr,
                  const char* blocked = nullptr) noexcept {
    if (module == nullptr || target == nullptr) {
        return;
    }
    struct TargetGeometry {
        DWORD flags;
        float first[3];
        float second[3];
    } geometry{};
    static_assert(offsetof(TargetGeometry, first) == 0x04);
    static_assert(offsetof(TargetGeometry, second) == 0x10);

    if (preview) {
        static volatile LONG64 nextLog[ARRAYSIZE(trenchModules)]{};
        auto* deadline = &nextLog[module - trenchModules];
        const auto now = static_cast<LONG64>(GetTickCount64());
        const auto previous = InterlockedCompareExchange64(deadline, 0, 0);
        if (now < previous || InterlockedCompareExchange64(deadline, now + 750, previous) != previous) {
            return;
        }
    }
    if (!ReadProcessMemory(GetCurrentProcess(), target, &geometry, sizeof(geometry), nullptr)) {
        return;
    }

    unsigned char mode = 0;
    const bool haveMode = ReadProcessMemory(GetCurrentProcess(), options, &mode, sizeof(mode), nullptr) != FALSE;
    float outputYaw = 0.0f;
    char rendered[48]{};
    char restriction[48]{};
    if (blocked != nullptr) {
        std::snprintf(restriction, sizeof(restriction), " blocked=%s", blocked);
    }
    if (output != nullptr &&
        ReadProcessMemory(GetCurrentProcess(), static_cast<const unsigned char*>(output) + 0x88,
                          &outputYaw, sizeof(outputYaw), nullptr)) {
        std::snprintf(rendered, sizeof(rendered), " preview_yaw=%.2f", outputYaw * 57.2957795f);
    }
    const auto& a = geometry.first;
    const auto& b = geometry.second;
    const float heading = std::atan2(b[1] - a[1], b[0] - a[0]) * 57.2957795f;
    char message[400];
    std::snprintf(message, sizeof(message),
                  "[GEM placement] %s %s pid=%lu tid=%lu flags=%08lx mode=%s%02x "
                  "first=(%.3f,%.3f,%.3f) second=(%.3f,%.3f,%.3f) input_yaw=%.2f%s%s\r\n",
                  stage, module->entityName, GetCurrentProcessId(),
                  GetCurrentThreadId(), geometry.flags, haveMode ? "" : "unavailable/", static_cast<unsigned>(mode),
                  a[0], a[1], a[2], b[0], b[1], b[2], heading, rendered, restriction);
    WriteLog(message);
}

void InstallExitSpawnTrace(unsigned char* image, const ConstructionLayout& layout) noexcept {
    // eAction_Spawn's ia:spawn implementation is on a separate vtable page.
    auto** slot = reinterpret_cast<void**>(image + layout.spawnSlot);
    void* const original = image + layout.spawnOriginal;
    if (*slot != original) {
        WriteLog("[GEM construction] Exit spawn trace skipped: unexpected spawn slot.\r\n");
        return;
    }
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
        WriteLog("[GEM construction] Exit spawn trace skipped: could not unlock spawn slot.\r\n");
        return;
    }

    GemConstructionOriginal[4] = original;
    const bool installed = InterlockedCompareExchangePointer(
        slot, reinterpret_cast<void*>(&GemTraceExitSpawn), original) == original;
    DWORD ignoredProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), protection, &ignoredProtection)) {
        WriteLog("[GEM construction] Could not restore spawn slot protection.\r\n");
    }
    WriteLog(installed ? "[GEM construction] Exit spawn trace enabled.\r\n"
                       : "[GEM construction] Exit spawn trace skipped: spawn slot changed during setup.\r\n");
}

bool LogCompletedPlacement(const void* entity, const TrenchModule& module,
                           TrenchTransform& transform) noexcept {
    // ia:spawn uses the entity's basis at +0x50 and origin at +0x74 in both verified hosts.
    const auto* address = static_cast<const unsigned char*>(entity) + 0x50;
    const auto finite = [](const TrenchVector& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!ReadProcessMemory(GetCurrentProcess(), address, &transform, sizeof(transform), nullptr) ||
        !finite(transform.origin) || !finite(transform.xAxis) || !finite(transform.yAxis) ||
        (transform.xAxis.x == 0.0f && transform.xAxis.y == 0.0f && transform.xAxis.z == 0.0f)) {
        WriteLog("[GEM placement] Completion transform unavailable.\r\n");
        return false;
    }

    const auto& x = transform.xAxis;
    const auto& p = transform.origin;
    char message[320];
    std::snprintf(message, sizeof(message),
                  "[GEM placement] completed %s pid=%lu entity=%p "
                  "position=(%.3f,%.3f,%.3f) entity_yaw=%.2f\r\n",
                  module.entityName, GetCurrentProcessId(), entity, p.x, p.y, p.z,
                  std::atan2(x.y, x.x) * 57.2957795f);
    WriteLog(message);

    if (module.entityName != straightTrench) {
        return true;
    }
    std::snprintf(message, sizeof(message),
                  "[GEM construction] seam_anchors pid=%lu entity=%p "
                  "left=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) left_x=(%.3f,%.3f,%.3f)\r\n",
                  GetCurrentProcessId(), entity,
                  p.x + module.sockets[0].center.x * x.x, p.y + module.sockets[0].center.x * x.y,
                  p.z + module.sockets[0].center.x * x.z,
                  p.x + module.sockets[1].center.x * x.x, p.y + module.sockets[1].center.x * x.y,
                  p.z + module.sockets[1].center.x * x.z,
                  x.x, x.y, x.z);
    WriteLog(message);
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    TrenchVector boneOrigin{}, boneX{};
    if (ReadStraightTrenchLandPose(image, pe->FileHeader.TimeDateStamp, entity, boneOrigin, boneX)) {
        std::snprintf(message, sizeof(message),
                      "[GEM construction] land_pose pid=%lu entity=%p origin=(%.3f,%.3f,%.3f) "
                      "bone_x=(%.4f,%.4f,%.4f) entity_x_z=%.4f\r\n",
                      GetCurrentProcessId(), entity, boneOrigin.x, boneOrigin.y, boneOrigin.z,
                      boneX.x, boneX.y, boneX.z, x.z);
        WriteLog(message);
    }
    return true;
}

void InstallConstructionTrace() noexcept {
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);

    const ConstructionLayout* layout = nullptr;
    for (const auto& candidate : constructionLayouts) {
        if (pe->FileHeader.TimeDateStamp == candidate.timestamp &&
            pe->OptionalHeader.SizeOfImage == candidate.imageSize) {
            layout = &candidate;
            break;
        }
    }
    if (layout == nullptr) {
        WriteLog("[GEM construction] Trace skipped: unsupported executable build.\r\n");
        return;
    }
    InstallTrenchMenu(image, std::strcmp(layout->host, "call_to_arms_ed.exe") == 0, WriteLog);

    void** slots[ARRAYSIZE(layout->slots)]{};
    for (unsigned i = 0; i < ARRAYSIZE(slots); ++i) {
        slots[i] = reinterpret_cast<void**>(image + layout->slots[i]);
        if (*slots[i] != image + layout->originals[i]) {
            WriteLog("[GEM construction] Trace skipped: unexpected instruction table.\r\n");
            return;
        }
        GemConstructionOriginal[i] = *slots[i];
    }

    void* const hooks[]{
        reinterpret_cast<void*>(&GemTraceInstall),
        reinterpret_cast<void*>(&GemTraceInstallBegin),
        reinterpret_cast<void*>(&GemTraceInstallEnd),
        reinterpret_cast<void*>(&GemTraceSectionCreated),
    };
    // install_end is the highest-addressed slot, including the callback in install's table.
    const SIZE_T span = layout->slots[2] - layout->slots[0] + sizeof(void*);
    DWORD oldProtection = 0;
    if (!VirtualProtect(slots[0], span, PAGE_READWRITE, &oldProtection)) {
        WriteLog("[GEM construction] Trace skipped: could not unlock instruction table.\r\n");
        return;
    }

    unsigned installed = 0;
    for (; installed < ARRAYSIZE(slots); ++installed) {
        const auto original = GemConstructionOriginal[installed];
        if (InterlockedCompareExchangePointer(slots[installed], hooks[installed], original) != original) {
            break;
        }
    }
    const bool installedAll = installed == ARRAYSIZE(slots);
    if (!installedAll) {
        while (installed != 0) {
            --installed;
            InterlockedCompareExchangePointer(slots[installed], GemConstructionOriginal[installed],
                                              hooks[installed]);
        }
    }

    DWORD ignoredProtection = 0;
    if (!VirtualProtect(slots[0], span, oldProtection, &ignoredProtection)) {
        WriteLog("[GEM construction] Could not restore instruction table protection.\r\n");
    }
    if (installedAll) {
        char message[160];
        std::snprintf(message, sizeof(message),
                      "[GEM construction] Trace enabled in %s, pid=%lu.\r\n",
                      layout->host, GetCurrentProcessId());
        WriteLog(message);
        InstallExitSpawnTrace(image, *layout);
        InstallPlacementTrace(image, *layout);
    } else {
        WriteLog("[GEM construction] Trace skipped: instruction table changed during setup.\r\n");
    }
}

BOOL CALLBACK InitializeMod(PINIT_ONCE, PVOID, PVOID*) noexcept {
    // Miles can unload and reload the ASI. Keep the logic resident across those loads.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&initializeOnce), &self)) {
        return FALSE;
    }

    LogInitialization();
    LoadTrenchSnapSettings(self);
    InstallConstructionTrace();
    return TRUE;
}
}

extern "C" void LogGemConstruction(unsigned instruction, void* self, void* arg2,
                                    void* arg3, void* arg4, const void* installArgs) noexcept {
    const DWORD lastError = GetLastError();
    constexpr const char* names[]{"install", "install_begin", "install_end"};
    char message[256];
    if (instruction == 4) {
        if (arg2 != nullptr && HasEntityName(self, 0x50, "it_trench_exit")) {
            void* parent = nullptr;
            if (ReadProcessMemory(GetCurrentProcess(), arg2, &parent, sizeof(parent), nullptr)) {
                std::snprintf(message, sizeof(message),
                              "[GEM construction] exit_spawn it_trench_exit "
                              "pid=%lu tid=%lu parent=%p action=%p context=%p\r\n",
                              GetCurrentProcessId(), GetCurrentThreadId(), parent, self, arg2);
                WriteLog(message);
            }
        }
    } else if (instruction < ARRAYSIZE(names)) {
        std::snprintf(message, sizeof(message),
                      "[GEM construction] %s pid=%lu tid=%lu this=%p arg2=%p arg3=%p arg4=%p\r\n",
                      names[instruction], GetCurrentProcessId(), GetCurrentThreadId(), self,
                      arg2, arg3, arg4);
        WriteLog(message);
        if (instruction == 0) {
            const void* args[2]{};
            if (ReadProcessMemory(GetCurrentProcess(), installArgs, args, sizeof(args), nullptr)) {
                LogPlacement("install_target", false, FindTrenchModule(self, 0x10), args[0], args[1]);
            }
        }
    }
    SetLastError(lastError);
}

extern "C" void GemTracePlacementPreview(void* action, void* output, void* selection,
                                          const TrenchTarget* target, const void* options) {
    using Preview = decltype(&GemTracePlacementPreview);
    const DWORD lastError = GetLastError();
    const auto* module = FindTrenchModule(action, 0x890);
    TrenchTarget adjusted{};
    TrenchPlacement placement;
    if (activeTrenchOrder != nullptr && activeTrenchOrder->action == action) {
        placement = activeTrenchOrder->placement;
    } else if (module != nullptr &&
               ReadProcessMemory(GetCurrentProcess(), target, &adjusted, sizeof(adjusted), nullptr) &&
               PrepareTrenchPlacement(*module, PreviewYawOffset(action), options, adjusted, placement)) {
        target = &adjusted;
    }
    SetLastError(lastError);
    reinterpret_cast<Preview>(GemConstructionOriginal[5])(action, output, selection, target, options);
    const DWORD originalLastError = GetLastError();
    const char* blocked = placement.module != nullptr ? CheckTrenchPlacement(placement) : nullptr;
    if (blocked != nullptr && output != nullptr) {
        // The native placer appends 16-byte samples to this vector. The last byte
        // of the position record is the native placement-valid flag at +0x0c.
        struct PreviewSample {
            TrenchVector position;
            unsigned char valid;
            unsigned char padding[3];
        };
        static_assert(sizeof(PreviewSample) == 16 && offsetof(PreviewSample, valid) == 0xc);
        PreviewSample* samples[2]{};
        if (ReadProcessMemory(GetCurrentProcess(), static_cast<const unsigned char*>(output) + 0x70,
                              samples, sizeof(samples), nullptr) &&
            samples[0] != nullptr && samples[1] == samples[0] + 1) {
            samples[0]->valid = 0;
        }
    }
    LogPlacement(placement.jointCount != 0 ? "preview_snap" : "preview", true,
                 module, target, options, output, blocked);
    SetLastError(originalLastError);
}

extern "C" void GemTracePlacementOrder(void* action, void* selection, const TrenchTarget* target,
                                        const void* options, void* context) {
    using Order = decltype(&GemTracePlacementOrder);
    const DWORD lastError = GetLastError();
    const auto* module = FindTrenchModule(action, 0x890);
    TrenchTarget adjusted{};
    TrenchPlacement placement;
    if (module == nullptr ||
        !ReadProcessMemory(GetCurrentProcess(), target, &adjusted, sizeof(adjusted), nullptr) ||
        !PrepareTrenchPlacement(*module, PreviewYawOffset(action), options, adjusted, placement)) {
        SetLastError(lastError);
        reinterpret_cast<Order>(GemConstructionOriginal[6])(action, selection, target, options, context);
        return;
    }
    if (const char* blocked = CheckTrenchPlacement(placement)) {
        LogPlacement("order_blocked", false, module, &adjusted, options, nullptr, blocked);
        SetLastError(lastError);
        return;
    }

    TrenchOrderScope scope{action, placement};
    auto* previousOrder = activeTrenchOrder;
    activeTrenchOrder = &scope;
    SetLastError(lastError);
    reinterpret_cast<Order>(GemConstructionOriginal[6])(action, selection, &adjusted, options, context);
    const DWORD originalLastError = GetLastError();
    activeTrenchOrder = previousOrder;
    const char* stage = scope.reserved ? "order_reserved" :
        (scope.blocked != nullptr ? "order_blocked" : "order_not_queued");
    LogPlacement(stage, false, module, &adjusted, options, nullptr, scope.blocked);
    SetLastError(originalLastError);
}

extern "C" void GemTraceSectionCreated(void* instruction, void* builder, void* entity) {
    using OnCreated = void (*)(void*, void*, void*);
    reinterpret_cast<OnCreated>(GemConstructionOriginal[3])(instruction, builder, entity);

    const DWORD lastError = GetLastError();
    const TrenchModule* module = FindTrenchModule(instruction, 0x10);
    if (entity != nullptr && module != nullptr) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "[GEM construction] section_created %s "
                      "pid=%lu tid=%lu entity=%p arg2=%p\r\n",
                      module->entityName, GetCurrentProcessId(), GetCurrentThreadId(), entity, builder);
        WriteLog(message);
        TrenchTransform transform{};
        const bool hasTransform = LogCompletedPlacement(entity, *module, transform);
        unsigned joinedCount = 0;
        const unsigned tracked = hasTransform
            ? TrackTrenchSection(*module, transform, entity, joinedCount, module->entityName == straightTrench)
            : 0;
        if (joinedCount != 0) {
            std::snprintf(message, sizeof(message),
                          "[GEM placement] joined %s entity=%p count=%u\r\n",
                          module->entityName, entity, joinedCount);
            WriteLog(message);
        }
        std::snprintf(message, sizeof(message),
                      "[GEM construction] trench_anchors pid=%lu entity=%p tracked=%u type=%s\r\n",
                      GetCurrentProcessId(), entity, tracked, module->entityName);
        WriteLog(message);
    }
    SetLastError(lastError);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) noexcept {
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GoHModInitialize() noexcept {
    return InitOnceExecuteOnce(&initializeOnce, InitializeMod, nullptr, nullptr);
}
