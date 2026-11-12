#pragma once
#include "TrenchTypes.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace trench {
// Owns all names and geometry borrowed by TrenchModule and queued placements.
// Load once, before installing callbacks; never reload a live catalog.
class Catalog {
public:
    Catalog();
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    void Load(const std::filesystem::path& entityRoot);
    const TrenchModule* FindEntity(const char* name) const noexcept;
    const TrenchModule* FindPreview(const char* name) const noexcept;
    const TrenchModule* FindEither(const char* name) const noexcept;
    bool BlocksPreview(const char* name) const noexcept;
    const std::vector<const TrenchModule*>& Modules() const noexcept { return modules; }
    const std::vector<std::string>& Diagnostics() const noexcept { return diagnostics; }
private:
    struct Entry;
    bool loaded = false;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<const TrenchModule*> modules;
    std::map<std::string, const TrenchModule*, std::less<>> entities, previews;
    std::vector<std::string> diagnostics;
};
} // namespace trench
