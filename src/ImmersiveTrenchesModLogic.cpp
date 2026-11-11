#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstddef>

#include "GemTrenchSnap.h"
#include "GemBranchRelay.h"

static_assert(sizeof(void*) == 8, "Build the mod logic for x64.");

void InstallTrenchMenu(unsigned char* host, bool isEditor, void (*log)(const char*)) noexcept;

extern "C" {
// Shared with the ASM trampolines: indices must match TRACE_INSTALL entries
// and the placement/spawn hooks below.
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

// Append one diagnostic record per call. Logging failures must not prevent
// the original engine operation; callers preserve Win32 last-error values.
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

// Version-specific RVAs for each executable. Never reuse game offsets for
// the editor; validate the PE identity before dereferencing these tables.
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

// Decode the verified MSVC string layout without assuming heap storage:
// short names live inline, longer names use the first pointer slot.
bool ReadEntityName(const void* object, SIZE_T offset, char (&actual)[64]) noexcept {
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
    const auto* field = static_cast<const unsigned char*>(object) + offset;
    const HANDLE process = GetCurrentProcess();
    if (!ReadProcessMemory(process, field, &name, sizeof(name), nullptr) ||
        name.size == 0 || name.capacity < name.size || name.size >= sizeof(actual)) {
        return false;
    }
    if (name.capacity < sizeof(name.storage)) {
        std::memcpy(actual, name.storage, name.size);
    } else {
        const char* data = nullptr;
        std::memcpy(&data, name.storage, sizeof(data));
        if (!ReadProcessMemory(process, data, actual, name.size, nullptr)) {
            return false;
        }
    }
    actual[name.size] = '\0';
    return true;
}

constexpr char straightTrench[] = "it_trench_straight_with_exits";
#include "GemTrenchCatalog.inc"

// Match only catalog entries so hooks also serving stock fortifications
// leave unrelated actions on their original execution path.
const TrenchModule* FindTrenchModule(const void* object, SIZE_T nameOffset) noexcept {
    char actual[64];
    if (!ReadEntityName(object, nameOffset, actual)) {
        return nullptr;
    }
    for (const auto& module : trenchModules) {
        if (std::strcmp(actual, nameOffset == 0x890 ? module.previewName : module.entityName) == 0) {
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

// Recover GEM's model-dependent quarter turn before calculating socket poses.
float PreviewBoundsYawOffset(const void* modelName) {
    GemPreviewBounds bounds{};
    readPreviewBounds(&bounds, modelName);
    // eFortify adds a quarter turn when the model's Y extent exceeds its X extent.
    return bounds.max.y - bounds.min.y > bounds.max.x - bounds.min.x ? 90.0f : 0.0f;
}

// Combine the action rotation with the bounds correction to match the
// orientation ultimately used by the native preview.
float PreviewYawOffset(const void* action) {
    const auto* bytes = static_cast<const unsigned char*>(action);
    float rotation = 0.0f;
    std::memcpy(&rotation, bytes + 0x8bc, sizeof(rotation));
    constexpr float degreesPerRadian = 57.2957795f;
    return rotation * degreesPerRadian + PreviewBoundsYawOffset(bytes + 0x890);
}

// Install queue guards before exposing placement hooks. If guard setup
// fails, keep native placement rather than enabling an unguarded snap path.
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

// Preview runs repeatedly while the cursor moves; throttle those records
// per module, but retain individual order and completion records.
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

using WriteNetworkCommand = void (*)(const void*, void*);
using WriteNetworkTarget = void (*)(const TrenchTarget*, void*);
using AddSceneEntity = void (*)(void*, bool, bool);
struct FortifyPreviewKey {
    std::uint64_t id;
    unsigned char local;
};
using AddFortifyPreview = void (*)(const FortifyPreviewKey*, void*);
using RemoveFortifyPreview = void (*)(const FortifyPreviewKey*);
WriteNetworkCommand originalWriteNetworkCommand = nullptr;
WriteNetworkTarget originalWriteNetworkTarget = nullptr;
AddSceneEntity originalAddSceneEntity = nullptr;
AddFortifyPreview originalAddFortifyPreview = nullptr;
RemoveFortifyPreview originalRemoveFortifyPreview = nullptr;
bool (*isNetworkClient)() = nullptr;
bool networkSnapEnabled = false;
bool networkPreviewEnabled = false;

// Borrow a corrected target only during synchronous command serialization.
// The command object itself remains unchanged.
struct NetworkTargetOverride {
    const TrenchTarget* original;
    const TrenchTarget* adjusted;
};
thread_local const NetworkTargetOverride* networkTargetOverride = nullptr;

// Replace only the exact target selected by the enclosing command hook;
// other target writes on the same thread retain their native input.
void WriteSnappedNetworkTarget(const TrenchTarget* target, void* stream) {
    const auto* replacement = networkTargetOverride;
    originalWriteNetworkTarget(replacement != nullptr && replacement->original == target
                                   ? replacement->adjusted : target, stream);
}

// Client commands travel through serialization before authoritative placement.
// Apply snapping here so the coordinates sent to the server match the preview.
void TraceNetworkCommand(const void* command, void* stream) {
    const DWORD lastError = GetLastError();
    const auto* bytes = static_cast<const unsigned char*>(command);
    TrenchTarget adjusted{};
    NetworkTargetOverride replacement{};
    // GEM's command writer serializes the target at +0x50 when flag 0x10 is set.
    const auto flags = *reinterpret_cast<const WORD*>(bytes + 0x18);
    if (bytes[8] == 1 && bytes[0x42] != 1 && bytes[0x42] != 2 && (flags & 0x18) == 0x18) {
        const auto* action = *reinterpret_cast<const void* const*>(bytes + 0x48);
        const auto* module = FindTrenchModule(action, 0x890);
        const auto* target = reinterpret_cast<const TrenchTarget*>(bytes + 0x50);
        const unsigned char mode = (flags & 0x80) != 0 ? bytes[0xf8] : 0;
        TrenchPlacement placement;
        const char* blocked = nullptr;
        if (module != nullptr && networkSnapEnabled && isNetworkClient()) {
            std::memcpy(&adjusted, target, sizeof(adjusted));
            if (PrepareTrenchPlacement(*module, PreviewYawOffset(action), &mode, adjusted, placement) &&
                placement.jointCount != 0) {
                blocked = CheckTrenchPlacement(placement);
                if (blocked == nullptr) {
                    replacement = {target, &adjusted};
                }
            }
        }
        LogPlacement(replacement.adjusted != nullptr ? "net_send_snap" : "net_send", false, module,
                     replacement.adjusted != nullptr ? replacement.adjusted : target, &mode, nullptr, blocked);
    }
    const auto* previousOverride = networkTargetOverride;
    networkTargetOverride = replacement.adjusted != nullptr ? &replacement : nullptr;
    SetLastError(lastError);
    originalWriteNetworkCommand(command, stream);
    networkTargetOverride = previousOverride;
}

// Validate the transform before using it to register world-space geometry.
bool ReadTrenchTransform(const void* entity, TrenchTransform& transform) noexcept {
    const auto finite = [](const TrenchVector& vector) {
        return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
    };
    return ReadProcessMemory(GetCurrentProcess(), static_cast<const unsigned char*>(entity) + 0x50,
                             &transform, sizeof(transform), nullptr) &&
           finite(transform.origin) && finite(transform.xAxis) && finite(transform.yAxis) &&
           (transform.xAxis.x != 0.0f || transform.xAxis.y != 0.0f || transform.xAxis.z != 0.0f);
}

// Observe the spawn registration call after GEM assigns the entity ID.
// Remote clients learn completed sections here, outside the install callback.
void TraceSceneEntity(void* entity, bool add, bool flag) {
    // GEM's third argument selects its client-local ID pool for a new entity.
    const bool clientCap = add && TakeClientTrenchCapRegistration();
    originalAddSceneEntity(entity, add, flag || clientCap);
    const DWORD lastError = GetLastError();
    const auto* module = FindTrenchModule(entity, 0xf0);
    TrenchTransform transform{};
    DWORD id = 0xffffffff;
    if (module != nullptr && ReadTrenchTransform(entity, transform) &&
        ReadProcessMemory(GetCurrentProcess(), static_cast<const unsigned char*>(entity) + 0xc0,
                          &id, sizeof(id), nullptr)) {
        unsigned joined = 0;
        const bool tracked = networkSnapEnabled && isNetworkClient();
        const unsigned open = tracked
            ? TrackTrenchSection(*module, transform, entity, joined, module->entityName == straightTrench, true)
            : 0;
        const auto& origin = transform.origin;
        const auto& axis = transform.xAxis;
        char message[256];
        std::snprintf(message, sizeof(message),
                      "[GEM network] scene_add %s pid=%lu tid=%lu id=%08lx "
                      "position=(%.3f,%.3f,%.3f) entity_yaw=%.2f sockets=%u tracked=%u open=%u joined=%u\r\n",
                      module->entityName, GetCurrentProcessId(), GetCurrentThreadId(), id,
                      origin.x, origin.y, origin.z, std::atan2(axis.y, axis.x) * 57.2957795f,
                      module->socketCount, static_cast<unsigned>(tracked), open, joined);
        WriteLog(message);
    }
    SetLastError(lastError);
}

struct FortifyPreviewSample {
    TrenchVector position;
    DWORD state;
};

void (*originalClearFortifyPreviews)() = nullptr;

// Let GEM clear its visualizer map first, then invalidate model-side remote
// reservations and sections so later scene updates can remove their caps.
void ClearNetworkTrenchCaps() {
    originalClearFortifyPreviews();
    const DWORD lastError = GetLastError();
    ClearNetworkTrenches();
    SetLastError(lastError);
}

// Capture decoded preview data before handing ownership to GEM. Register
// its temporary sockets only after GEM has inserted the preview key.
void TrackReceivedFortifyPreview(const FortifyPreviewKey* key, void* visual) {
    const DWORD lastError = GetLastError();
    const TrenchModule* module = nullptr;
    const auto id = key->id;
    FortifyPreviewSample samples[64];
    SIZE_T count = 0;
    float yaw = 0.0f;
    float wireYaw = 0.0f;
    if (networkPreviewEnabled && key->local == 0 && visual != nullptr && isNetworkClient()) {
        // The received visualizer begins with the model name and stores its
        // decoded positions and final model yaw at +0x70 and +0x88.
        char name[64];
        if (ReadEntityName(visual, 0, name)) {
            for (const auto& candidate : trenchModules) {
                if (std::strcmp(name, candidate.previewName) == 0 ||
                    std::strcmp(name, candidate.entityName) == 0) {
                    module = &candidate;
                    break;
                }
            }
        }
        if (module != nullptr) {
            auto* bytes = static_cast<unsigned char*>(visual);
            std::uintptr_t positions[2]{};
            if (ReadProcessMemory(GetCurrentProcess(), bytes + 0x70, positions, sizeof(positions), nullptr) &&
                positions[1] > positions[0] && positions[1] - positions[0] <= sizeof(samples) &&
                (positions[1] - positions[0]) % sizeof(samples[0]) == 0 &&
                ReadProcessMemory(GetCurrentProcess(), bytes + 0x88, &wireYaw, sizeof(wireYaw), nullptr) &&
                std::isfinite(wireYaw) &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(positions[0]),
                                  samples, positions[1] - positions[0], nullptr)) {
                count = (positions[1] - positions[0]) / sizeof(samples[0]);
                // The dedicated server sends the queued visualizer without the
                // bounds-based turn used by the client and the finished entity.
                constexpr float radiansPerDegree = 0.01745329252f;
                yaw = wireYaw + PreviewBoundsYawOffset(visual) * radiansPerDegree;
                std::memcpy(bytes + 0x88, &yaw, sizeof(yaw));
            }
        }
    }
    SetLastError(lastError);
    originalAddFortifyPreview(key, visual);
    const DWORD originalLastError = GetLastError();
    for (SIZE_T i = 0; i < count; ++i) {
        const auto position = samples[i].position;
        const auto open = TrackNetworkTrenchPreview(id, *module, position, yaw);
        char message[256];
        std::snprintf(message, sizeof(message),
                      "[GEM network] preview_received %s pid=%lu key=%016llx "
                      "position=(%.3f,%.3f,%.3f) wire_yaw=%.2f yaw=%.2f open=%u\r\n",
                      module->entityName, GetCurrentProcessId(), static_cast<unsigned long long>(id),
                      position.x, position.y, position.z, wireYaw * 57.2957795f, yaw * 57.2957795f, open);
        WriteLog(message);
    }
    SetLastError(originalLastError);
}

// Mirror native preview removal in the model; cap changes are deferred
// to scene synchronization.
void ReleaseReceivedFortifyPreview(const FortifyPreviewKey* key) {
    const auto id = key->id;
    const bool remote = key->local == 0;
    originalRemoveFortifyPreview(key);
    const DWORD lastError = GetLastError();
    if (networkPreviewEnabled && remote && RemoveNetworkTrenchPreview(id)) {
        char message[128];
        std::snprintf(message, sizeof(message), "[GEM network] preview_removed pid=%lu key=%016llx\r\n",
                      GetCurrentProcessId(), static_cast<unsigned long long>(id));
        WriteLog(message);
    }
    SetLastError(lastError);
}

// Patch a verified rel32 call through a nearby absolute-jump relay. The
// original function address is published before redirecting execution.
template <typename Hook>
bool InstallNetworkCall(unsigned char* image, DWORD callRva, DWORD originalRva, Hook hook,
                        Hook& original) noexcept {
    auto* call = image + callRva;
    const auto displacement = static_cast<std::int32_t>(originalRva) - static_cast<std::int32_t>(callRva + 5);
    if (*call != 0xe8 || std::memcmp(call + 1, &displacement, sizeof(displacement)) != 0) {
        return false;
    }
    auto* relay = AllocateGemRelayNear(call + 5);
    if (relay == nullptr) {
        return false;
    }
    constexpr unsigned char jump[]{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(relay, jump, sizeof(jump));
    std::memcpy(relay + sizeof(jump), &hook, sizeof(hook));
    DWORD oldProtection = 0;
    DWORD callProtection = 0;
    if (!VirtualProtect(relay, sizeof(jump) + sizeof(hook), PAGE_EXECUTE_READ, &oldProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof(jump) + sizeof(hook)) ||
        !VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &callProtection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    original = reinterpret_cast<Hook>(image + originalRva);
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(relay) -
                                                    reinterpret_cast<std::intptr_t>(call + 5));
    std::memcpy(call + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    VirtualProtect(call, 5, callProtection, &oldProtection);
    return true;
}

// Enable dependent features only when their required hooks succeeded.
// The editor does not use these game-only network addresses.
void InstallNetworkTrace(unsigned char* image) noexcept {
    void* const writer = image + 0xb78a90;
    originalWriteNetworkCommand = reinterpret_cast<WriteNetworkCommand>(writer);
    unsigned installed = 0;
    constexpr DWORD writerSlots[]{0x1024e90, 0x104ff48};
    for (const DWORD rva : writerSlots) {
        auto** slot = reinterpret_cast<void**>(image + rva);
        DWORD protection = 0;
        if (*slot != writer || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
            continue;
        }
        installed += InterlockedCompareExchangePointer(
            slot, reinterpret_cast<void*>(&TraceNetworkCommand), writer) == writer;
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    }
    // This predicate selects the multiplayer client branch in GEM's spawn path.
    isNetworkClient = reinterpret_cast<decltype(isNetworkClient)>(image + 0xc0b630);
    const bool sceneTrace = InstallNetworkCall(image, 0x7063ef, 0xa60150, &TraceSceneEntity, originalAddSceneEntity);
    networkSnapEnabled = installed == ARRAYSIZE(writerSlots) && sceneTrace &&
        InstallNetworkCall(image, 0xb78b6b, 0x794320, &WriteSnappedNetworkTarget, originalWriteNetworkTarget);
    // eOrderFortifyVisualizingCommand receives type 0 (positions) and type 2 (remove).
    networkPreviewEnabled = networkSnapEnabled &&
        InstallNetworkCall(image, 0xae717, 0xb76450, &TrackReceivedFortifyPreview, originalAddFortifyPreview) &&
        InstallNetworkCall(image, 0xae57c, 0xb76590, &ReleaseReceivedFortifyPreview, originalRemoveFortifyPreview);
    const bool networkCaps = networkSnapEnabled &&
        InstallNetworkCall(image, 0xaf5ca7, 0xb768b0, &ClearNetworkTrenchCaps, originalClearFortifyPreviews) &&
        EnableNetworkTrenchCaps();
    WriteLog(networkCaps ? "[GEM caps] Client-only scene entities enabled.\r\n"
                         : "[GEM caps] Client-only scene entities skipped.\r\n");
    char message[180];
    std::snprintf(message, sizeof(message), "[GEM network] pid=%lu writers=%u/2 scene_add=%s snap=%s previews=%s.\r\n",
                  GetCurrentProcessId(), installed, sceneTrace ? "enabled" : "skipped",
                  networkSnapEnabled ? "enabled" : "skipped", networkPreviewEnabled ? "enabled" : "skipped");
    WriteLog(message);
}

// This spawn vtable is separate from the install instruction tables;
// validate and change its slot independently.
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

// Read the final entity pose rather than trusting the queued preview pose;
// straight sections also expose bone diagnostics for terrain alignment.
bool LogCompletedPlacement(const void* entity, const TrenchModule& module,
                           TrenchTransform& transform) noexcept {
    if (!ReadTrenchTransform(entity, transform)) {
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

// Gate all native address use on a known PE build, then verify each
// instruction slot before replacing it. Roll back a partially changed table.
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
        if (std::strcmp(layout->host, "call_to_arms.exe") == 0) {
            InstallNetworkTrace(image);
        }
    } else {
        WriteLog("[GEM construction] Trace skipped: instruction table changed during setup.\r\n");
    }
}

// InitOnce owns process-wide setup. Pin before installing callbacks because
// Miles may unload and reload the small ASI loader independently.
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

// Called by ASM with captured native arguments; observe them without
// changing the original instruction's arguments or Win32 last-error value.
extern "C" void LogGemConstruction(unsigned instruction, void* self, void* arg2,
                                    void* arg3, void* arg4, const void* installArgs) noexcept {
    const DWORD lastError = GetLastError();
    constexpr const char* names[]{"install", "install_begin", "install_end"};
    char message[256];
    if (instruction == 4) {
        char name[64];
        if (arg2 != nullptr && ReadEntityName(self, 0x50, name) && std::strcmp(name, "it_trench_exit") == 0) {
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

// Prepare the snapped pose, let GEM build its visual, then mark blocked
// placements invalid in the native preview sample.
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

// Check placement before native order creation. The thread-local scope
// connects nested preview/enqueue calls to this exact placement.
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

// Run the native completion callback first, then transfer queued state
// to the final entity and synchronize its free-end caps.
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

// No engine calls under the Windows loader lock.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) noexcept {
    return TRUE;
}

// Repeated Miles loads share the same pinned DLL and initialization result.
extern "C" __declspec(dllexport) BOOL WINAPI GoHModInitialize() noexcept {
    return InitOnceExecuteOnce(&initializeOnce, InitializeMod, nullptr, nullptr);
}
