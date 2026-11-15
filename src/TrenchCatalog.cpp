#include "TrenchCatalog.h"
#include "TrenchCapProfile.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
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

std::vector<Node> Document(const fs::path& path, std::size_t limit = 4 * 1024 * 1024) {
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) Fail(1, "cannot open file");
        const auto size = input.tellg();
        if (size < 0 || static_cast<std::uintmax_t>(size) > limit) Fail(1, "file size exceeds limit");
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
    std::vector<std::unique_ptr<CapProfile>> capProfiles;
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

bool IsInsensitive(const Node& node, const char* name) {
    if (node.words.empty()) return false;
    std::string value = node.words.front();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    });
    return value == name;
}
void CollectBones(const Node& node, std::set<std::string>& bones) {
    if (IsInsensitive(node, "bone") && node.words.size() == 2) bones.insert(node.words[1]);
    for (const auto& child : node.children) CollectBones(child, bones);
}
void CapFields(const Node& node, std::initializer_list<const char*> allowed) {
    for (const auto& child : node.children) {
        if (child.delimiter != '{' || std::none_of(allowed.begin(), allowed.end(), [&](const char* field) {
                return IsInsensitive(child, field);
            })) Fail(child.line, "unsupported cutting cap field");
    }
}
const Node& CapOne(const Node& node, const char* name) {
    const Node* result = nullptr;
    for (const auto& child : node.children) if (IsInsensitive(child, name)) {
        if (result) Fail(child.line, std::string("duplicate cap ") + name);
        result = &child;
    }
    if (!result) Fail(node.line, std::string("missing cap ") + name);
    return *result;
}
void CapNumbers(const Node& node, unsigned count, bool positive = false) {
    Shape(node, count + 1);
    for (unsigned i = 1; i <= count; ++i) {
        if (positive && Number(node, i) <= 0) Fail(node.line, "cap dimensions must be positive");
        if (!positive) Number(node, i); // Reject NaN/Inf and malformed numeric input.
    }
}
void CapMatrix(const Node& node, unsigned count) {
    CapNumbers(node, count);
    const double a = Number(node, 1), b = Number(node, 2), c = Number(node, 3);
    const double d = Number(node, 4), e = Number(node, 5), f = Number(node, 6);
    const double g = Number(node, 7), h = Number(node, 8), i = Number(node, 9);
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-8)
        Fail(node.line, "cap matrix must be finite and invertible");
}
void CheckCapBone(const Node& bone, std::set<std::string>& names, unsigned& meshCount) {
    Shape(bone, 2, true);
    if (bone.words[1].empty() || bone.words[1].size() > 127 ||
        !names.insert(bone.words[1]).second) Fail(bone.line, "empty, oversized or duplicate cap bone name");
    CapFields(bone, {"matrix34", "orientation", "position", "parameters", "volumeview", "bone"});
    bool matrix = false, orientation = false, position = false, parameters = false, view = false;
    for (const auto& child : bone.children) {
        if (IsInsensitive(child, "bone")) CheckCapBone(child, names, meshCount);
        else if (IsInsensitive(child, "matrix34")) {
            if (matrix || orientation || position) Fail(child.line, "duplicate or conflicting cap transform");
            CapMatrix(child, 12); matrix = true;
        } else if (IsInsensitive(child, "orientation")) {
            if (matrix || orientation) Fail(child.line, "duplicate or conflicting cap orientation");
            CapMatrix(child, 9); orientation = true;
        } else if (IsInsensitive(child, "position")) {
            if (matrix || position) Fail(child.line, "duplicate or conflicting cap position");
            CapNumbers(child, 3); position = true;
        } else if (IsInsensitive(child, "parameters")) {
            if (parameters) Fail(child.line, "duplicate cap parameters");
            Shape(child, 2); parameters = true;
            std::string token;
            for (const char c : child.words[1] + ";") {
                if (c == ';') {
                    if (!token.empty() && token != "commonmesh" && token != "land" && token != "align" && token != "ground")
                        Fail(child.line, "unsupported cutting cap bone parameter");
                    token.clear();
                } else token += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
            }
        } else if (IsInsensitive(child, "volumeview")) {
            if (view) Fail(child.line, "duplicate cap VolumeView");
            Shape(child, 2, true); view = true; ++meshCount;
            CapFields(child, {"decaltargetdynamic"});
            if (!child.children.empty()) Shape(CapOne(child, "decaltargetdynamic"), 1);
        }
    }
}
void CheckCapMeshes(const Node& node, const fs::path& model, const fs::path& entityRoot) {
    if (IsInsensitive(node, "volumeview")) {
        if (node.words.size() != 2) Fail(node.line, "cap VolumeView requires one mesh path");
        const auto reference = fs::u8path(node.words[1]);
        const auto mesh = reference.has_root_directory() ? entityRoot.parent_path() / reference.relative_path() : model.parent_path() / reference;
        if (reference.has_root_name() || reference.extension() != ".ply" ||
            !Inside(mesh, entityRoot) || !fs::is_regular_file(mesh))
            Fail(node.line, "cap mesh must exist inside entity root");
    }
    for (const auto& child : node.children) CheckCapMeshes(child, model, entityRoot);
}
fs::path CapDefinition(const fs::path& entityRoot) {
    fs::path result;
    for (const auto& entry : fs::recursive_directory_iterator(entityRoot)) {
        if (!entry.is_regular_file() || entry.path().filename() != "it_trench_dirt_end_cap.def") continue;
        if (!result.empty()) Fail(1, "ambiguous cutting cap definition");
        result = entry.path();
    }
    if (result.empty() || !Inside(result, entityRoot)) Fail(1, "missing cutting cap definition inside entity root");
    return result;
}
void ValidateCuttingCap(const fs::path& definition, const fs::path& entityRoot) {
    const auto document = Document(definition);
    if (document.size() != 1 || document[0].delimiter != '{' || !document[0].words.empty())
        Fail(1, "cutting cap must use the standard unnamed trench definition");
    bool include = false, land = false, volumeMacro = false, persistent = false, props = false, volume = false;
    for (const auto& node : document[0].children) {
        if (node.delimiter == '(') {
            if (!node.children.empty()) Fail(node.line, "nested cutting cap macro is unsupported");
            if (node.Is("include") && node.words.size() == 2 && node.words[1] == "/properties/trench.inc") {
                if (include) Fail(node.line, "duplicate cap trench include");
                include = true;
            } else if (node.Is("boneLand") && node.words.size() == 3 && node.words[1] == "args" && node.words[2] == "trench") {
                if (land) Fail(node.line, "duplicate cap boneLand macro");
                land = true;
            } else if (node.Is("volume") && node.words.size() == 3 && node.words[1] == "args" && node.words[2] == "body1") {
                if (volumeMacro) Fail(node.line, "duplicate cap volume macro");
                volumeMacro = true;
            } else Fail(node.line, "only trench.inc, boneLand trench and volume body1 cap macros are supported");
        } else if (IsInsensitive(node, "extension")) {
            Shape(node, 2);
            if (fs::u8path(node.words[1]).has_parent_path()) Fail(node.line, "cap MDL must be a neighboring file");
        } else if (IsInsensitive(node, "dyncmppersistent")) {
            if (persistent) Fail(node.line, "duplicate cap DynCmpPersistent");
            Shape(node, 1); persistent = true;
        } else if (IsInsensitive(node, "props")) {
            if (props) Fail(node.line, "duplicate cap props");
            Shape(node, 2); props = true;
            if (node.words[1] != "sand") Fail(node.line, "cutting cap props must be sand");
        } else if (IsInsensitive(node, "volume")) {
            if (volume) Fail(node.line, "duplicate cap volume override");
            Shape(node, 2); volume = true;
            if (node.words[1] != "body1") Fail(node.line, "cutting cap override must name body1");
        } else Fail(node.line, "unsupported cutting cap definition field");
    }
    if (!include || !land || !volumeMacro || !persistent || !props || !volume)
        Fail(1, "cutting cap requires trench.inc, boneLand trench, volume body1, DynCmpPersistent and props sand");
    const auto model = DefinitionModel(definition, entityRoot);
    const auto modelDocument = Document(model);
    const Node* skeleton = nullptr;
    const Node* obstacle = nullptr;
    const Node* modelVolume = nullptr;
    for (const auto& node : modelDocument) {
        if (IsInsensitive(node, "skeleton")) {
            if (skeleton) Fail(node.line, "duplicate cap skeleton");
            skeleton = &node;
        } else if (node.Is("area") && node.words.size() == 2 && node.words[1] == "cut") {
            // ReadCut below checks the complete polygon, finite values and duplicates.
        } else if (IsInsensitive(node, "obstacle")) {
            if (obstacle) Fail(node.line, "duplicate cap obstacle");
            Shape(node, 2, true);
            if (node.words[1] != "body1") Fail(node.line, "cap obstacle must name body1");
            obstacle = &node;
        } else if (IsInsensitive(node, "volume")) {
            if (modelVolume) Fail(node.line, "duplicate cap model volume");
            Shape(node, 2, true);
            if (node.words[1] != "body1") Fail(node.line, "cap model volume must name body1");
            modelVolume = &node;
        } else Fail(node.line, "unsupported cutting cap model block");
        CheckCapMeshes(node, model, entityRoot);
    }
    if (!skeleton || !obstacle || !modelVolume) Fail(1, "cutting cap requires skeleton, obstacle body1 and volume body1");
    Shape(*skeleton, 1, true); CapFields(*skeleton, {"bone"});
    const auto& root = CapOne(*skeleton, "bone");
    if (root.words.size() != 2 || root.words[1] != "trench") Fail(root.line, "cutting cap root bone must be trench");
    std::set<std::string> bones; unsigned meshes = 0;
    CheckCapBone(root, bones, meshes);
    if (meshes == 0) Fail(root.line, "cutting cap must contain a mesh");
    Geometry cut;
    ReadCut(model, cut);
    CapFields(*obstacle, {"obb2", "rotate"}); Shape(CapOne(*obstacle, "rotate"), 1);
    const auto& bounds = CapOne(*obstacle, "obb2"); Shape(bounds, 1, true);
    CapFields(bounds, {"center", "extent", "axis"});
    CapNumbers(CapOne(bounds, "center"), 2); CapNumbers(CapOne(bounds, "extent"), 2, true);
    const auto& axis = CapOne(bounds, "axis"); CapNumbers(axis, 2);
    if (std::fabs(std::hypot(Number(axis, 1), Number(axis, 2)) - 1.0) > 0.001)
        Fail(axis.line, "cap obstacle axis must be a unit vector");
    CapFields(*modelVolume, {"box", "bone", "matrix34"});
    CapNumbers(CapOne(*modelVolume, "box"), 3, true);
    const auto& volumeBone = CapOne(*modelVolume, "bone"); Shape(volumeBone, 2);
    if (!bones.count(volumeBone.words[1])) Fail(volumeBone.line, "cap volume references missing bone");
    CapMatrix(CapOne(*modelVolume, "matrix34"), 12);
}
TrenchVector Point3(const Node& node) {
    Shape(node, 4);
    return {Number(node, 1), Number(node, 2), Number(node, 3)};
}
void Label(const std::string& name, unsigned line) {
    if (name.empty() || name.size() > 127 || std::any_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 32 || c == 127;
    })) Fail(line, "invalid or oversized point/bone name");
}
std::unique_ptr<CapProfile> ReadCapSocket(const Node& node, unsigned id,
    const fs::path& entityRoot,
    const Geometry& geometry, const std::set<std::string>& bones) {
    Shape(node, 2, true);
    Fields(node, {"cap_entity", "cap_offset", "contact", "rim"});
    if (std::find(geometry.ids, geometry.ids + geometry.count, id) == geometry.ids + geometry.count)
        Fail(node.line, "cap socket ID does not exist in topology");
    auto result = std::make_unique<CapProfile>();
    result->socketId = id;
    const auto& entity = One(node, "cap_entity"); Shape(entity, 2);
    if (entity.words[1] != "it_trench_dirt_end_cap")
        Fail(entity.line, "cap_entity must be it_trench_dirt_end_cap");
    result->capEntity = entity.words[1];
    ValidateCuttingCap(CapDefinition(entityRoot), entityRoot);
    const Node* offset = nullptr;
    std::set<std::string> contactNames, rimNames;
    for (const auto& child : node.children) {
        if (child.Is("cap_offset")) {
            if (offset) Fail(child.line, "duplicate cap_offset");
            offset = &child; Shape(child, 3);
            result->capOffset = {Number(child, 1), Number(child, 2)};
        } else if (child.Is("contact")) {
            Shape(child, 2, true); Label(child.words[1], child.line);
            if (!contactNames.insert(child.words[1]).second) Fail(child.line, "duplicate contact name");
            Fields(child, {"section_bone", "section_bind", "cap_bind"});
            const auto& bone = One(child, "section_bone"); Shape(bone, 2); Label(bone.words[1], bone.line);
            if (!bones.count(bone.words[1])) Fail(bone.line, "section bone does not exist in model");
            result->contact.push_back({child.words[1], bone.words[1],
                Point3(One(child, "section_bind")), Point3(One(child, "cap_bind"))});
        } else if (child.Is("rim")) {
            Shape(child, 2, true); Label(child.words[1], child.line);
            if (!rimNames.insert(child.words[1]).second) Fail(child.line, "duplicate rim name");
            Fields(child, {"lip", "skirt"});
            result->rim.push_back({child.words[1], Point3(One(child, "lip")), Point3(One(child, "skirt"))});
        }
    }
    if (result->contact.size() < 4 || result->contact.size() > 32) Fail(node.line, "contact requires 4..32 points");
    std::string reason;
    if (!SampleCapRim(result->rim, result->rimSamples, reason)) Fail(node.line, reason);
    return result;
}
void ReadCapProfiles(const fs::path& metadata, const fs::path& model, const fs::path& entityRoot,
                     Geometry& geometry, std::vector<std::string>& diagnostics) {
    auto file = metadata; file.replace_extension(".trenchcaps");
    try {
        if (!fs::exists(file)) return;
        if (!Inside(file, entityRoot)) Fail(1, "cap profile is outside entity root");
        const auto document = Document(file, 1024 * 1024);
        if (document.size() != 1 || !document[0].Is("it_trench_caps")) Fail(1, "expected one it_trench_caps block");
        const auto& root = document[0]; Shape(root, 1, true);
        Fields(root, {"version", "section_model", "socket"});
        if (Unsigned(One(root, "version")) != 1) Fail(root.line, "unsupported cap profile version");
        const auto& section = One(root, "section_model"); Shape(section, 2);
        const auto reference = fs::u8path(section.words[1]);
        if (reference.has_parent_path() || reference.extension() != ".mdl" ||
            !Inside(file.parent_path() / reference, entityRoot) ||
            !fs::equivalent(file.parent_path() / reference, model))
            Fail(section.line, "section_model must match the neighboring variant model");
        std::set<std::string> bones;
        for (const auto& node : Document(model)) CollectBones(node, bones);
        std::map<unsigned, unsigned> counts;
        // Invalid IDs have no stable binding. Diagnose them independently;
        // duplicated valid IDs invalidate every definition for that socket.
        for (const auto& socket : root.children) if (socket.Is("socket")) try {
            Node id; id.words = socket.words; id.line = socket.line;
            ++counts[Unsigned(id)];
        } catch (const std::exception& error) { diagnostics.push_back(file.u8string() + ":" + error.what()); }
        for (const auto& socket : root.children) if (socket.Is("socket")) try {
            Node idNode; idNode.words = socket.words; idNode.line = socket.line;
            const auto id = Unsigned(idNode);
            if (counts[id] != 1) Fail(socket.line, "duplicate cap socket ID");
            geometry.capProfiles.push_back(ReadCapSocket(socket, id, entityRoot, geometry, bones));
        } catch (const std::exception& error) { diagnostics.push_back(file.u8string() + ":" + error.what()); }
    } catch (const std::exception& error) {
        geometry.capProfiles.clear();
        diagnostics.push_back(file.u8string() + ":" + error.what());
    }
}
struct Candidate {
    std::string entity, preview;
    fs::path file;
    unsigned line;
    std::shared_ptr<Geometry> geometry;
};
std::vector<Candidate> ReadMetadata(const fs::path& file, const fs::path& entityRoot, std::vector<std::string>& diagnostics) {
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
    ReadCapProfiles(file, model, entityRoot, *geometry, diagnostics);
    return variants;
}
} // namespace

bool ValidateTrenchCapResource(const fs::path& entityRoot, std::string& diagnostic) {
    diagnostic.clear();
    try {
        ValidateCuttingCap(CapDefinition(entityRoot), entityRoot);
        return true;
    } catch (const std::exception& error) {
        diagnostic = entityRoot.u8string() + ": cap resource: " + error.what();
        return false;
    }
}

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
        for (const auto& profile : geometry->capProfiles)
            for (unsigned i = 0; i < module.socketCount; ++i)
                if (module.socketIds[i] == profile->socketId) module.capProfiles[i] = profile.get();
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
        auto variants = ReadMetadata(file, entityRoot, diagnostics);
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
