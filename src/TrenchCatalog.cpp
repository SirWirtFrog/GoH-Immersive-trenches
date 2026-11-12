#include "TrenchCatalog.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace trench {
namespace {
namespace fs = std::filesystem;
struct Node {
    std::vector<std::string> words;
    std::vector<Node> children;
    unsigned line = 1;
    char delimiter = '{';
    bool Is(const char* name) const { return !words.empty() && words[0] == name; }
};
[[noreturn]] void Fail(unsigned line, const std::string& reason) {
    throw std::runtime_error(std::to_string(line) + ": " + reason);
}

// The same lexer reads metadata and skips unrelated GEM model/DEF blocks.
// Semicolons inside strings are data, and parentheses delimit GEM macros.
class Parser {
    const std::string& source;
    std::size_t cursor = 0;
    unsigned line = 1;
    void Space() {
        while (cursor < source.size()) {
            const char c = source[cursor];
            if (c == ';') {
                while (cursor < source.size() && source[cursor] != '\n') ++cursor;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (c == '\n') ++line;
                ++cursor;
            } else break;
        }
    }
    std::string Word() {
        std::string result;
        if (source[cursor] == '"') {
            ++cursor;
            while (cursor < source.size()) {
                char c = source[cursor++];
                if (c == '"') return result;
                if (c == '\n') ++line;
                if (c == '\\') {
                    if (cursor == source.size()) Fail(line, "unfinished string escape");
                    c = source[cursor++];
                    if (c != '\\' && c != '"') Fail(line, "unsupported string escape");
                }
                result += c;
            }
            Fail(line, "unterminated string");
        }
        while (cursor < source.size()) {
            const char c = source[cursor];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                c == '{' || c == '}' || c == '(' || c == ')' || c == ';') break;
            result += c;
            ++cursor;
        }
        return result;
    }
    Node Block(unsigned depth) {
        if (depth > 64) Fail(line, "block nesting exceeds 64");
        Node node;
        node.line = line;
        node.delimiter = source[cursor++];
        const char close = node.delimiter == '{' ? '}' : ')';
        for (;;) {
            Space();
            if (cursor == source.size()) Fail(node.line, "unclosed block");
            const char c = source[cursor];
            if (c == close) { ++cursor; return node; }
            if (c == '}' || c == ')') Fail(line, "mismatched closing delimiter");
            if (c == '{' || c == '(') node.children.push_back(Block(depth + 1));
            else node.words.push_back(Word());
        }
    }
public:
    explicit Parser(const std::string& text) : source(text) {
        if (source.compare(0, 3, "\xef\xbb\xbf") == 0) cursor = 3;
    }
    std::vector<Node> Read() {
        std::vector<Node> nodes;
        Space();
        while (cursor < source.size()) {
            if (source[cursor] != '{' && source[cursor] != '(') Fail(line, "expected a block");
            nodes.push_back(Block(0));
            Space();
        }
        return nodes;
    }
};

std::vector<Node> Document(const fs::path& path) {
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) Fail(1, "cannot open file");
        const auto size = input.tellg();
        if (size < 0 || size > 4 * 1024 * 1024) Fail(1, "file size exceeds 4 MiB");
        std::string text(static_cast<std::size_t>(size), '\0');
        input.seekg(0);
        if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) Fail(1, "cannot read file");
        if (text.find('\0') != std::string::npos) Fail(1, "embedded NUL");
        return Parser(text).Read();
    } catch (const std::exception& error) {
        throw std::runtime_error(path.u8string() + ":" + error.what());
    }
}
void Shape(const Node& node, std::size_t words, bool children = false) {
    if (node.delimiter != '{' || node.words.size() != words || (!children && !node.children.empty()))
        Fail(node.line, "invalid block arguments");
}
void Fields(const Node& node, std::initializer_list<const char*> allowed) {
    for (const auto& child : node.children) {
        if (child.delimiter != '{' || child.words.empty() ||
            std::none_of(allowed.begin(), allowed.end(), [&](const char* field) { return child.Is(field); }))
            Fail(child.line, "unknown field");
    }
}
const Node& One(const Node& node, const char* field) {
    const Node* result = nullptr;
    for (const auto& child : node.children) if (child.Is(field)) {
        if (result) Fail(child.line, std::string("duplicate ") + field);
        result = &child;
    }
    if (!result) Fail(node.line, std::string("missing ") + field);
    return *result;
}
unsigned Unsigned(const Node& node) {
    Shape(node, 2);
    unsigned value = 0;
    const auto& text = node.words[1];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == noTrenchSocket)
        Fail(node.line, "invalid unsigned integer");
    return value;
}
float Number(const Node& node, unsigned index) {
    auto text = std::string_view(node.words[index]);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    float value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value) || std::fabs(value) > 1000000.0f) Fail(node.line, "invalid or out-of-range number");
    return value;
}
void Name(const std::string& name, unsigned line) {
    if (name.size() >= 64 || name.compare(0, 10, "it_trench_") != 0 || name.size() == 10 ||
        !std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        })) Fail(line, "entity/preview name must use it_trench_ and fit 63 ASCII characters");
}
double Cross(TrenchPoint a, TrenchPoint b, TrenchPoint c) {
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
           (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
}
bool OnSegment(TrenchPoint a, TrenchPoint b, TrenchPoint p) {
    return std::fabs(Cross(a, b, p)) < 1e-7 && p.x >= (std::min)(a.x, b.x) &&
        p.x <= (std::max)(a.x, b.x) && p.y >= (std::min)(a.y, b.y) && p.y <= (std::max)(a.y, b.y);
}
bool Intersects(TrenchPoint a, TrenchPoint b, TrenchPoint c, TrenchPoint d) {
    const double abC = Cross(a, b, c), abD = Cross(a, b, d), cdA = Cross(c, d, a), cdB = Cross(c, d, b);
    return ((abC > 0) != (abD > 0) && (cdA > 0) != (cdB > 0)) ||
        OnSegment(a, b, c) || OnSegment(a, b, d) || OnSegment(c, d, a) || OnSegment(c, d, b);
}
struct Geometry {
    std::vector<TrenchPoint> cut;
    std::vector<TrenchTriangle> triangles;
    TrenchSocket sockets[maxTrenchSockets]{};
    unsigned ids[maxTrenchSockets]{};
    unsigned count = 0, defaultSocket = 0;
};

void Triangulate(Geometry& geometry, unsigned line) {
    auto& points = geometry.cut;
    if (points.size() > 1 && points.front().x == points.back().x && points.front().y == points.back().y) points.pop_back();
    const unsigned count = static_cast<unsigned>(points.size());
    if (count < 3 || count > maxTrenchCutVertices) Fail(line, "cut must have 3..64 vertices");
    double area = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto a = points[i], b = points[(i + 1) % count];
        if (a.x == b.x && a.y == b.y) Fail(line, "duplicate adjacent cut vertex");
        area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
        for (unsigned j = i + 1; j < count; ++j) {
            if (j == i + 1 || (i == 0 && j == count - 1)) continue;
            if (Intersects(a, b, points[j], points[(j + 1) % count])) Fail(line, "self-intersecting cut");
        }
    }
    if (std::fabs(area) < 1e-7) Fail(line, "zero-area cut");
    const double sign = area > 0 ? 1.0 : -1.0;
    bool convex = true;
    for (unsigned i = 0; i < count; ++i)
        convex &= sign * Cross(points[i], points[(i + 1) % count], points[(i + 2) % count]) >= -1e-7;
    if (convex) return;
    std::vector<unsigned char> remaining(count);
    std::iota(remaining.begin(), remaining.end(), static_cast<unsigned char>(0));
    while (remaining.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto a = remaining[(i + remaining.size() - 1) % remaining.size()];
            const auto b = remaining[i], c = remaining[(i + 1) % remaining.size()];
            if (sign * Cross(points[a], points[b], points[c]) <= 1e-7) continue;
            bool contains = false;
            for (auto p : remaining) if (p != a && p != b && p != c)
                contains |= sign * Cross(points[a], points[b], points[p]) >= -1e-7 &&
                    sign * Cross(points[b], points[c], points[p]) >= -1e-7 &&
                    sign * Cross(points[c], points[a], points[p]) >= -1e-7;
            if (contains) continue;
            geometry.triangles.push_back({a, b, c});
            remaining.erase(remaining.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) Fail(line, "cannot triangulate cut");
    }
    geometry.triangles.push_back({remaining[0], remaining[1], remaining[2]});
}

void ReadCut(const fs::path& model, Geometry& geometry) {
    const auto document = Document(model);
    try {
        const Node* cut = nullptr;
        for (const auto& node : document) if (node.Is("area") && node.words.size() >= 2 && node.words[1] == "cut") {
            if (cut) Fail(node.line, "duplicate cut area");
            cut = &node;
        }
        if (!cut) Fail(1, "missing top-level area cut");
        Shape(*cut, 2, true);
        Fields(*cut, {"polygon2", "rotate"});
        Shape(One(*cut, "rotate"), 1);
        const auto& polygon = One(*cut, "polygon2");
        Shape(polygon, 1, true);
        Fields(polygon, {"radius", "vertex"});
        const auto& radius = One(polygon, "radius");
        Shape(radius, 2);
        if (Number(radius, 1) != 0) Fail(radius.line, "only polygon2 radius 0 is supported");
        for (const auto& vertex : polygon.children) if (vertex.Is("vertex")) {
            Shape(vertex, 3);
            geometry.cut.push_back({Number(vertex, 1), Number(vertex, 2)});
        }
        Triangulate(geometry, polygon.line);
    } catch (const std::exception& error) { throw std::runtime_error(model.u8string() + ":" + error.what()); }
}

bool Inside(const fs::path& path, const fs::path& root) {
    const auto relative = fs::weakly_canonical(path).lexically_relative(fs::weakly_canonical(root));
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
fs::path DefinitionModel(const fs::path& definition, const fs::path& entityRoot) {
    const auto document = Document(definition);
    try {
        if (document.size() != 1) Fail(1, "expected one entity definition");
        const Node* extension = nullptr;
        for (const auto& node : document[0].children) {
            // GEM resources use both Extension and extension.
            if (node.Is("extension") || node.Is("Extension")) {
                if (extension) Fail(node.line, "duplicate extension");
                extension = &node;
            }
        }
        if (!extension) Fail(1, "missing direct model extension");
        Shape(*extension, 2);
        fs::path reference = fs::u8path(extension->words[1]);
        auto model = reference.has_root_directory() ? entityRoot.parent_path() / reference.relative_path() : definition.parent_path() / reference;
        if (reference.has_root_name() || !Inside(model, entityRoot) || model.extension() != ".mdl" || !fs::is_regular_file(model))
            Fail(extension->line, "model extension must resolve to an existing MDL inside this mod");
        return model;
    } catch (const std::exception& error) { throw std::runtime_error(definition.u8string() + ":" + error.what()); }
}
struct Candidate {
    std::string entity, preview;
    fs::path file;
    unsigned line;
    std::shared_ptr<Geometry> geometry;
};
std::vector<Candidate> ReadMetadata(const fs::path& file, const fs::path& entityRoot) {
    const auto document = Document(file);
    std::vector<Candidate> variants;
    auto geometry = std::make_shared<Geometry>();
    try {
        if (document.size() != 1 || !document[0].Is("it_trench")) Fail(1, "expected one it_trench block");
        const auto& root = document[0];
        Shape(root, 1, true);
        Fields(root, {"version", "variant", "default_socket", "socket"});
        if (Unsigned(One(root, "version")) != 1) Fail(root.line, "unsupported metadata version");
        const auto defaultId = Unsigned(One(root, "default_socket"));
        std::map<unsigned, TrenchSocket> sockets;
        for (const auto& node : root.children) {
            if (node.Is("socket")) {
                Shape(node, 2, true);
                Fields(node, {"position", "yaw"});
                Node idNode; idNode.words = node.words; idNode.line = node.line;
                const auto id = Unsigned(idNode);
                const auto& position = One(node, "position"), &yaw = One(node, "yaw");
                Shape(position, 4); Shape(yaw, 2);
                const auto x = Number(position, 1), y = Number(position, 2), z = Number(position, 3);
                if (z != 0) Fail(position.line, "socket Z must be 0 for planar snapping");
                if (!sockets.emplace(id, TrenchSocket{{x, y}, Number(yaw, 1)}).second) Fail(node.line, "duplicate socket ID");
            } else if (node.Is("variant")) {
                Shape(node, 2, true);
                Fields(node, {"preview"});
                const auto& preview = One(node, "preview");
                Shape(preview, 2);
                Name(node.words[1], node.line); Name(preview.words[1], preview.line);
                variants.push_back({node.words[1], preview.words[1], file, node.line, geometry});
            }
        }
        if (sockets.empty() || sockets.size() > maxTrenchSockets) Fail(root.line, "expected 1..4 sockets");
        if (!sockets.count(defaultId)) Fail(root.line, "default_socket ID does not exist");
        for (const auto& socket : sockets) {
            const auto i = geometry->count++;
            geometry->ids[i] = socket.first;
            geometry->sockets[i] = socket.second;
            if (socket.first == defaultId) geometry->defaultSocket = i;
        }
    } catch (const std::exception& error) { throw std::runtime_error(file.u8string() + ":" + error.what()); }
    // No registrations means an authoring draft: validate anchors but do not
    // activate it (the four-way experimental model currently uses this path).
    if (variants.empty()) return variants;
    auto model = file; model.replace_extension(".mdl");
    ReadCut(model, *geometry);
    for (const auto& variant : variants) {
        const auto actual = DefinitionModel(file.parent_path() / (variant.entity + ".def"), entityRoot);
        if (!fs::equivalent(actual, model))
            throw std::runtime_error(file.u8string() + ":" + std::to_string(variant.line) + ": variant uses a different model");
        DefinitionModel(file.parent_path() / (variant.preview + ".def"), entityRoot);
    }
    return variants;
}
} // namespace

struct Catalog::Entry {
    std::string entity, preview;
    std::shared_ptr<Geometry> geometry;
    TrenchModule module{};
    explicit Entry(const Candidate& source) : entity(source.entity), preview(source.preview), geometry(source.geometry) {
        module.entityName = entity.c_str(); module.previewName = preview.c_str();
        module.cut = geometry->cut.data(); module.cutSize = static_cast<unsigned>(geometry->cut.size());
        module.triangles = geometry->triangles.data(); module.triangleCount = static_cast<unsigned>(geometry->triangles.size());
        module.socketCount = geometry->count; module.defaultSocket = geometry->defaultSocket;
        std::copy_n(geometry->sockets, geometry->count, module.sockets);
        std::copy_n(geometry->ids, geometry->count, module.socketIds);
    }
};
Catalog::Catalog() = default;
Catalog::~Catalog() = default;
void Catalog::Load(const fs::path& entityRoot) {
    if (loaded) throw std::logic_error("cannot reload a published trench catalog");
    loaded = true;
    std::vector<fs::path> files;
    std::vector<Candidate> candidates;
    std::error_code error;
    fs::recursive_directory_iterator iterator(entityRoot, error), end;
    if (error) { diagnostics.push_back(entityRoot.u8string() + ":1: " + error.message()); return; }
    while (iterator != end) {
        const auto path = iterator->path();
        if (iterator->is_regular_file() && path.extension() == ".trench") files.push_back(path);
        iterator.increment(error);
        if (error) { diagnostics.push_back(path.u8string() + ":1: " + error.message()); break; }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) try {
        auto variants = ReadMetadata(file, entityRoot);
        candidates.insert(candidates.end(), variants.begin(), variants.end());
    } catch (const std::exception& failure) { diagnostics.push_back(failure.what()); }
    std::map<std::string, unsigned> names;
    for (const auto& candidate : candidates) { ++names[candidate.entity]; ++names[candidate.preview]; }
    // Publish the complete registry atomically with respect to initialization:
    // an allocation failure must not leave entity/preview lookups half filled.
    decltype(entries) newEntries;
    decltype(modules) newModules;
    decltype(entities) newEntities, newPreviews;
    for (const auto& candidate : candidates) {
        if (names[candidate.entity] != 1 || names[candidate.preview] != 1) {
            diagnostics.push_back(candidate.file.u8string() + ":" + std::to_string(candidate.line) + ": conflicting variant/preview " + candidate.entity);
            continue;
        }
        auto entry = std::make_unique<Entry>(candidate);
        const auto* module = &entry->module;
        newEntries.push_back(std::move(entry));
        newModules.push_back(module);
        newEntities.emplace(module->entityName, module);
        newPreviews.emplace(module->previewName, module);
    }
    if (newModules.empty()) diagnostics.push_back(entityRoot.u8string() + ":1: no registered trench variants");
    entries.swap(newEntries); modules.swap(newModules);
    entities.swap(newEntities); previews.swap(newPreviews);
}
const TrenchModule* Catalog::FindEntity(const char* name) const noexcept {
    if (!name) return nullptr;
    const auto found = entities.find(name);
    return found == entities.end() ? nullptr : found->second;
}
const TrenchModule* Catalog::FindPreview(const char* name) const noexcept {
    if (!name) return nullptr;
    const auto found = previews.find(name);
    return found == previews.end() ? nullptr : found->second;
}
const TrenchModule* Catalog::FindEither(const char* name) const noexcept {
    const auto* module = FindEntity(name);
    return module ? module : FindPreview(name);
}
bool Catalog::BlocksPreview(const char* name) const noexcept {
    return name && std::string_view(name).substr(0, 10) == "it_trench_" && !FindPreview(name);
}
} // namespace trench
