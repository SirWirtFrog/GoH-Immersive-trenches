#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "GemBranchRelay.h"

#include <cstddef>
#include <cstring>

namespace {
constexpr unsigned emplacementPage = 5;
constexpr unsigned mainPage = 1;
constexpr unsigned buttonInitialize = 1;
constexpr unsigned buttonClick = 9;
constexpr unsigned buttonMethods = 14;

struct MenuLayout {
    DWORD pageNames, pages, loadPage, pageVtable, parserVtable, destroyPage;
    DWORD allocate, registerButton;
    DWORD openFactory, openVtable, openClick;
    DWORD initializeButton, addIcon;
    DWORD attachLegacyBranch, attachLegacyPage;
    DWORD attachGuiBranch, attachGuiPage;
};

constexpr MenuLayout game{
    0x148b798, 0x159fd50, 0xb428c0, 0x1062110, 0x10621b0, 0x3ab910,
    0x36b6a0, 0xb41890, 0xb43e00, 0x111d0a0, 0xb4a050,
    0xb39a90, 0xb3fb40, 0xb3cbf2, 0xb3b810, 0x2195e3, 0x215b50,
};
constexpr MenuLayout editor{
    0x1300180, 0x140c450, 0xa72120, 0xec8a58, 0xec8af8, 0x29f890,
    0x25f630, 0xa70fc0, 0xa735e0, 0xf9e028, 0xa79a50,
    0xa69030, 0xa6f1b0, 0xa6c1b2, 0xa6add0, 0x194eb3, 0x1913b0,
};

// GEM's panel configuration reader embeds a three-pointer entry vector.
struct PanelPage {
    void** vtable;
    DWORD flags;
    DWORD padding;
    void* parser;
    void** parserVtable;
    const unsigned char** begin;
    const unsigned char** end;
    const unsigned char** capacity;
};
static_assert(sizeof(PanelPage) == 0x38 && offsetof(PanelPage, begin) == 0x20);

struct GemString {
    char data[16]{};
    SIZE_T length = 0;
    SIZE_T capacity = 15;
};

using ButtonFactory = void* (*)();
struct ButtonRegistration {
    const char* name;
    ButtonFactory factory;
};

using Log = void (*)(const char*);
using DestroyPage = void* (*)(PanelPage*, unsigned);
unsigned char* image = nullptr;
const MenuLayout* layout = nullptr;
Log writeLog = nullptr;
const char** pageNames = nullptr;
PanelPage** pages = nullptr;
PanelPage* stockOwner = nullptr;
PanelPage* trenchPage = nullptr;
void* openButtonTable[1 + buttonMethods]{};
ButtonRegistration openRegistration{};
bool menuReady = false;

template <typename Function>
Function Gem(DWORD rva) noexcept {
    return reinterpret_cast<Function>(image + rva);
}

void* OnDestroyPage(PanelPage* page, unsigned flags) {
    const auto destroy = Gem<DestroyPage>(layout->destroyPage);
    if (page == stockOwner) {
        auto* owned = trenchPage;
        trenchPage = nullptr;
        stockOwner = nullptr;
        if (owned != nullptr) {
            destroy(owned, 1);
        }
    }
    return destroy(page, flags);
}

template <typename T>
class ScopedSlot {
public:
    ScopedSlot(T& slot, T value) noexcept : slot_(slot), saved_(slot) { slot_ = value; }
    ~ScopedSlot() { slot_ = saved_; }
    ScopedSlot(const ScopedSlot&) = delete;
    ScopedSlot& operator=(const ScopedSlot&) = delete;

private:
    T& slot_;
    T saved_;
};

bool LoadTrenchPage() {
    if (trenchPage != nullptr) {
        return true;
    }
    auto* stock = pages[emplacementPage];
    if (stock == nullptr || stock->vtable != Gem<void**>(layout->pageVtable) ||
        stock->parserVtable != Gem<void**>(layout->parserVtable) ||
        stock->parser != &stock->parserVtable || stock->flags != 0xb5) {
        writeLog("[GEM menu] Unexpected stock panel reader.\r\n");
        return false;
    }
    auto* page = Gem<PanelPage* (*)(SIZE_T)>(layout->allocate)(sizeof(PanelPage));
    if (page == nullptr) {
        return false;
    }
    *page = {stock->vtable, stock->flags, 0, &page->parserVtable, stock->parserVtable,
             nullptr, nullptr, nullptr};

    {
        // The stock loader resolves /set/panel_control/<name>.set and fills
        // the reader at the same index. Restore both entries after it returns.
        const ScopedSlot<const char*> name(pageNames[emplacementPage], "right_it_trenches");
        const ScopedSlot<PanelPage*> reader(pages[emplacementPage], page);
        Gem<void (*)(unsigned)>(layout->loadPage)(emplacementPage);
    }
    if (page->begin == page->end) {
        Gem<DestroyPage>(layout->destroyPage)(page, 1);
        writeLog("[GEM menu] Separate trench page is empty.\r\n");
        return false;
    }
    stockOwner = stock;
    trenchPage = page;
    writeLog("[GEM menu] Loaded right_it_trenches.set.\r\n");
    return true;
}

bool MainPanelHasTrenchButton() noexcept {
    const auto* main = pages[mainPage];
    if (main == nullptr) {
        return false;
    }
    constexpr char name[] = "gc_cp_browse_it_trenches";
    for (auto entry = main->begin; entry != main->end; ++entry) {
        // Both stock panel builders read an MSVC string at entry +0x48.
        const auto* field = reinterpret_cast<const GemString*>(*entry + 0x48);
        if (field->length == sizeof(name) - 1 && field->capacity >= field->length) {
            const char* text = nullptr;
            std::memcpy(&text, field->data, sizeof(text));
            if (text != nullptr && std::memcmp(text, name, sizeof(name) - 1) == 0) {
                return true;
            }
        }
    }
    return false;
}

void AttachPanelPages(void* panel, PanelPage* stock, DWORD target) {
    const auto attach = Gem<void (*)(void*, PanelPage*)>(target);
    attach(panel, stock);
    if (menuReady && MainPanelHasTrenchButton() && LoadTrenchPage()) {
        attach(panel, trenchPage);
    }
}

void AttachLegacyPages(void* panel, PanelPage* stock) {
    AttachPanelPages(panel, stock, layout->attachLegacyPage);
}

void AttachGuiPages(void* panel, PanelPage* stock) {
    AttachPanelPages(panel, stock, layout->attachGuiPage);
}

bool HookPageAttachment(DWORD site, DWORD original, unsigned char opcode,
                        void (*hook)(void*, PanelPage*)) noexcept {
    auto* branch = image + site;
    const auto expected = static_cast<std::int32_t>(original) - static_cast<std::int32_t>(site + 5);
    if (*branch != opcode || std::memcmp(branch + 1, &expected, sizeof(expected)) != 0) {
        return false;
    }
    auto* relay = AllocateGemRelayNear(branch + 5);
    if (relay == nullptr) {
        return false;
    }
    constexpr unsigned char jump[]{0xff, 0x25, 0, 0, 0, 0};
    constexpr SIZE_T relaySize = sizeof(jump) + sizeof(hook);
    std::memcpy(relay, jump, sizeof(jump));
    std::memcpy(relay + sizeof(jump), &hook, sizeof(hook));
    DWORD protection = 0;
    if (!VirtualProtect(relay, relaySize, PAGE_EXECUTE_READ, &protection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, relaySize) ||
        !VirtualProtect(branch, 5, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(relay) -
                                                    reinterpret_cast<std::intptr_t>(branch + 5));
    std::memcpy(branch + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), branch, 5);
    DWORD ignored = 0;
    VirtualProtect(branch, 5, protection, &ignored);
    return true;
}

void OpenTrenchPage() {
    if (menuReady && trenchPage != nullptr) {
        // Browse dispatch is synchronous. Each listener caches the chosen
        // reader, so only this event sees the alternate entry.
        const ScopedSlot<PanelPage*> reader(pages[emplacementPage], trenchPage);
        Gem<void (*)()>(layout->openClick)();
        writeLog("[GEM menu] Dispatched browse to trench page.\r\n");
    }
}

void InitializeOpenButton(void* button, void* parent, void* setup, void* slot, int flags) {
    using Initialize = void (*)(void*, void*, void*, void*, const char*, int);
    Gem<Initialize>(layout->initializeButton)(button, parent, setup, slot, "it_trench_menu", flags);

    // GEM copies the icon label. Both inputs fit inline and own no heap buffer.
    constexpr GemString label{"it_trench_menu", sizeof("it_trench_menu") - 1, 15};
    constexpr GemString detail{};
    auto* host = *reinterpret_cast<void**>(static_cast<unsigned char*>(button) + 8);
    using AddIcon = void (*)(void*, const GemString*, const GemString*);
    Gem<AddIcon>(layout->addIcon)(host, &label, &detail);
}

void* MakeOpenButton() {
    auto* button = Gem<ButtonFactory>(layout->openFactory)();
    if (button != nullptr) {
        *static_cast<void***>(button) = openButtonTable + 1;
    }
    return button;
}

} // namespace

void InstallTrenchMenu(unsigned char* host, bool isEditor, Log log) noexcept {
    image = host;
    layout = isEditor ? &editor : &game;
    writeLog = log;
    pageNames = Gem<const char**>(layout->pageNames);
    pages = Gem<PanelPage**>(layout->pages);
    auto** pageTable = Gem<void**>(layout->pageVtable);
    auto** openTable = Gem<void**>(layout->openVtable);
    if (std::strcmp(pageNames[emplacementPage], "right_emplacement") != 0 ||
        pageTable[0] != host + layout->destroyPage || openTable[buttonClick] != host + layout->openClick) {
        log("[GEM menu] Skipped: unexpected panel tables.\r\n");
        return;
    }

    // Preserve the native RTTI locator at index -1 for the derived button objects.
    std::memcpy(openButtonTable, openTable - 1, sizeof(openButtonTable));
    openButtonTable[buttonInitialize + 1] = reinterpret_cast<void*>(&InitializeOpenButton);
    openButtonTable[buttonClick + 1] = reinterpret_cast<void*>(&OpenTrenchPage);

    DWORD protection = 0;
    if (!VirtualProtect(pageTable, sizeof(void*), PAGE_READWRITE, &protection)) {
        log("[GEM menu] Skipped: could not unlock the panel reader destructor.\r\n");
        return;
    }
    const auto installed = InterlockedCompareExchangePointer(
        pageTable, reinterpret_cast<void*>(&OnDestroyPage), host + layout->destroyPage);
    DWORD ignored = 0;
    VirtualProtect(pageTable, sizeof(void*), protection, &ignored);
    if (installed != host + layout->destroyPage) {
        log("[GEM menu] Skipped: panel reader destructor changed during setup.\r\n");
        return;
    }

    using RegisterButton = void (*)(ButtonRegistration*, const char*, const ButtonFactory*);
    const auto registerButton = Gem<RegisterButton>(layout->registerButton);
    const ButtonFactory open = &MakeOpenButton;
    registerButton(&openRegistration, "gc_cp_browse_it_trenches", &open);
    // The panels create all button instances up front and later match them by
    // the address of each parsed entry. Register our reader in both builders.
    menuReady = HookPageAttachment(layout->attachLegacyBranch, layout->attachLegacyPage, 0xe9, AttachLegacyPages) &&
                HookPageAttachment(layout->attachGuiBranch, layout->attachGuiPage, 0xe8, AttachGuiPages);
    log(menuReady ? "[GEM menu] Registered trench page in both panel builders.\r\n"
                  : "[GEM menu] Skipped: could not hook both panel builders.\r\n");
}
