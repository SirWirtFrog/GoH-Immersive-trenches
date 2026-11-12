#include "../src/TrenchCatalog.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
constexpr char straightTrench[] = "it_trench_dirt_straight";
// Migration oracle only, never compiled into the production DLL.
#include "fixtures/LegacyTrenchCatalog.inc"

// Preserve the migration oracle while resolving the renamed resource identifiers.
const char* CurrentName(const char* name) {
    if (std::string(name) == "it_trench_bridge") return "it_trench_dirt_straight_bridge";
    if (std::string(name) == "it_trench_bridge_preview") return "it_trench_dirt_straight_bridge_preview";
    if (std::string(name) == "it_trench_end") return "it_trench_dirt_straight_end";
    if (std::string(name) == "it_trench_end_preview") return "it_trench_dirt_straight_end_preview";
    if (std::string(name) == "it_trench_fire_position1") return "it_trench_dirt_fire_position_through";
    if (std::string(name) == "it_trench_fire_position1_preview") return "it_trench_dirt_fire_position_through_preview";
    if (std::string(name) == "it_trench_fire_position2") return "it_trench_dirt_fire_position_dead_end";
    if (std::string(name) == "it_trench_fire_position2_preview") return "it_trench_dirt_fire_position_dead_end_preview";
    if (std::string(name) == "it_trench_intersection") return "it_trench_dirt_intersection_of_three";
    if (std::string(name) == "it_trench_intersection_preview") return "it_trench_dirt_intersection_of_three_preview";
    if (std::string(name) == "it_trench_mg_nest") return "it_trench_dirt_mg_nest";
    if (std::string(name) == "it_trench_mg_nest_preview") return "it_trench_dirt_mg_nest_preview";
    if (std::string(name) == "it_trench_straight_with_exits") return "it_trench_dirt_straight";
    if (std::string(name) == "it_trench_turn") return "it_trench_dirt_turn_20";
    if (std::string(name) == "it_trench_turn2") return "it_trench_dirt_turn_30";
    if (std::string(name) == "it_trench_turn2_preview") return "it_trench_dirt_turn_30_preview";
    if (std::string(name) == "it_trench_turn3") return "it_trench_dirt_turn_60";
    if (std::string(name) == "it_trench_turn3_preview") return "it_trench_dirt_turn_60_preview";
    if (std::string(name) == "it_trench_turn_preview") return "it_trench_dirt_turn_20_preview";
    if (std::string(name) == "it_trench_depot") return "it_trench_dirt_depot";
    if (std::string(name) == "it_trench_depot_preview") return "it_trench_dirt_depot_preview";
    if (std::string(name) == "it_trench_emplacement") return "it_trench_dirt_emplacement";
    if (std::string(name) == "it_trench_emplacement_preview") return "it_trench_dirt_emplacement_preview";
    if (std::string(name) == "it_trench_end_cap") return "it_trench_dirt_end_cap";
    if (std::string(name) == "it_trench_end_cap_mat_000_ground.003") return "it_trench_dirt_end_cap_mat_000_ground.003";
    if (std::string(name) == "it_trench_end_cap_mat_001_ground_alpha.003") return "it_trench_dirt_end_cap_mat_001_ground_alpha.003";
    if (std::string(name) == "it_trench_exit") return "it_trench_dirt_exit";
    if (std::string(name) == "it_trench_exit_preview") return "it_trench_dirt_exit_preview";
    if (std::string(name) == "it_trench_fire_position_dead_end") return "it_trench_dirt_fire_position_dead_end";
    if (std::string(name) == "it_trench_fire_position_dead_end_preview") return "it_trench_dirt_fire_position_dead_end_preview";
    if (std::string(name) == "it_trench_fire_position_through") return "it_trench_dirt_fire_position_through";
    if (std::string(name) == "it_trench_fire_position_through_preview") return "it_trench_dirt_fire_position_through_preview";
    if (std::string(name) == "it_trench_intersection_of_four") return "it_trench_dirt_intersection_of_four";
    if (std::string(name) == "it_trench_intersection_of_four_preview") return "it_trench_dirt_intersection_of_four_preview";
    if (std::string(name) == "it_trench_intersection_of_three") return "it_trench_dirt_intersection_of_three";
    if (std::string(name) == "it_trench_intersection_of_three_preview") return "it_trench_dirt_intersection_of_three_preview";
    if (std::string(name) == "it_trench_mg-nest") return "it_trench_dirt_mg_nest";
    if (std::string(name) == "it_trench_mg-nest_preview") return "it_trench_dirt_mg_nest_preview";
    if (std::string(name) == "it_trench_pillbox") return "it_trench_dirt_pillbox";
    if (std::string(name) == "it_trench_pillbox_preview") return "it_trench_dirt_pillbox_preview";
    if (std::string(name) == "it_trench_straight") return "it_trench_dirt_straight";
    if (std::string(name) == "it_trench_straight_preview") return "it_trench_dirt_straight_preview";
    if (std::string(name) == "it_trench_straight_bridge") return "it_trench_dirt_straight_bridge";
    if (std::string(name) == "it_trench_straight_bridge_preview") return "it_trench_dirt_straight_bridge_preview";
    if (std::string(name) == "it_trench_straight_end") return "it_trench_dirt_straight_end";
    if (std::string(name) == "it_trench_straight_end_preview") return "it_trench_dirt_straight_end_preview";
    if (std::string(name) == "it_trench_straight_short") return "it_trench_dirt_straight_short";
    if (std::string(name) == "it_trench_straight_short_preview") return "it_trench_dirt_straight_short_preview";
    if (std::string(name) == "it_trench_turn_20") return "it_trench_dirt_turn_20";
    if (std::string(name) == "it_trench_turn_20_preview") return "it_trench_dirt_turn_20_preview";
    if (std::string(name) == "it_trench_turn_30") return "it_trench_dirt_turn_30";
    if (std::string(name) == "it_trench_turn_30_preview") return "it_trench_dirt_turn_30_preview";
    if (std::string(name) == "it_trench_turn_60") return "it_trench_dirt_turn_60";
    if (std::string(name) == "it_trench_turn_60_preview") return "it_trench_dirt_turn_60_preview";
    if (std::string(name) == "it_trench_turn_90") return "it_trench_dirt_turn_90";
    if (std::string(name) == "it_trench_turn_90_preview") return "it_trench_dirt_turn_90_preview";
    return name;
}

const std::string sockets =
    "{socket 0 {position -100 0 0} {yaw 180}}\n"
    "{socket 1 {position 100 0 0} {yaw 0}}\n";
std::string Meta(const std::string& entity, const std::string& anchors = sockets, unsigned defaultId = 0) {
    return "{it_trench {version 1}\n{variant \"" + entity + "\" {preview \"" + entity + "_preview\"}}\n"
        "{default_socket " + std::to_string(defaultId) + "}\n" + anchors + "}\n";
}
std::string Model(const std::string& vertices) {
    return "{skeleton {bone \"trench\" {parameters \"commonmesh;land;align;\"}}}\n"
        "{area \"cut\" {polygon2 {radius 0}\n" + vertices + "}\n{rotate}}\n";
}
const std::string rectangle = "{vertex -100 -20}{vertex 100 -20}{vertex 100 20}{vertex -100 20}";
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    Require(static_cast<bool>(output), "cannot create catalog fixture");
}
void Fixture(const fs::path& folder, const std::string& entity, const std::string& metadata = {}, const std::string& vertices = rectangle) {
    Write(folder / "shape.trench", metadata.empty() ? Meta(entity) : metadata);
    Write(folder / "shape.mdl", Model(vertices));
    Write(folder / (entity + ".def"), "{ {extension \"shape.mdl\"} }");
    Write(folder / (entity + "_preview.def"), "{game_entity {Extension \"shape_preview.mdl\"}}");
    Write(folder / "shape_preview.mdl", "{skeleton}");
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    const auto root = fs::current_path();
    trench::Catalog actual;
    actual.Load(root / "mods/ImmersiveTrenches/resource/entity");
    for (const auto& diagnostic : actual.Diagnostics()) std::fprintf(stderr, "%s\n", diagnostic.c_str());
    Require(actual.Diagnostics().empty(), "real catalog has errors");
    Require(actual.Modules().size() == ARRAYSIZE(trenchModules) + 1 - 6, "active catalog variant count changed");
    for (const auto& old : trenchModules) {
        if (std::string(old.entityName).find("it_trench_mg_nest_") == 0) {
            Require(!actual.FindEntity(CurrentName(old.entityName)), "disabled MG faction variant is still active");
            continue;
        }
        const auto* value = actual.FindEntity(CurrentName(old.entityName));
        Require(value && value == actual.FindPreview(CurrentName(old.previewName)) && value == actual.FindEither(CurrentName(old.entityName)), "variant lookup changed");
        Require(value->cutSize == old.cutSize && value->socketCount == old.socketCount &&
            value->defaultSocket == old.defaultSocket, "migration changed geometry dimensions/default socket");
        for (unsigned i = 0; i < old.cutSize; ++i) {
            if (!Near(value->cut[i].x, old.cut[i].x) || !Near(value->cut[i].y, old.cut[i].y))
                std::fprintf(stderr, "%s: cut vertex %u differs from legacy geometry\n", value->entityName, i);
            Require(Near(value->cut[i].x, old.cut[i].x) && Near(value->cut[i].y, old.cut[i].y), "migration changed cut vertices");
        }
        for (unsigned i = 0; i < old.socketCount; ++i)
            Require(value->socketIds[i] == i && Near(value->sockets[i].center.x, old.sockets[i].center.x) &&
                Near(value->sockets[i].center.y, old.sockets[i].center.y) &&
                Near(value->sockets[i].outwardYawDegrees, old.sockets[i].outwardYawDegrees), "migration changed socket coordinates/IDs/yaw");
    }
    const auto* four = actual.FindEntity("it_trench_dirt_intersection_of_four");
    Require(four && four->socketCount == 4 && actual.FindPreview("it_trench_dirt_intersection_of_four_preview") == four &&
        !actual.BlocksPreview("it_trench_dirt_intersection_of_four_preview"), "four-way test variant is not buildable");
    Require(!actual.BlocksPreview("foxhole") && actual.BlocksPreview("it_trench_missing_preview") &&
        !actual.BlocksPreview("it_trench_dirt_straight_preview"), "stock/registered/unavailable routing changed");
    const auto* stable = actual.FindEntity(straightTrench);
    bool refusedReload = false;
    try { actual.Load(root); } catch (const std::logic_error&) { refusedReload = true; }
    Require(refusedReload && actual.FindEntity(straightTrench) == stable, "catalog reload invalidated a live module");
    Require(actual.FindEntity("it_trench_dirt_mg_nest") != nullptr, "empty MG nest section was removed");

    const auto fixtures = root / "out/catalog-tests" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    unsigned sequence = 0;
    auto Case = [&]() { return fixtures / std::to_string(sequence++); };
    auto Invalid = [&](const std::string& metadata, const std::string& vertices = rectangle) {
        const auto path = Case();
        Fixture(path, "it_trench_test", metadata, vertices);
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty() && !catalog.Diagnostics().empty() && catalog.BlocksPreview("it_trench_test_preview"),
            "invalid metadata/model reached the catalog");
        Require(catalog.Diagnostics()[0].find(path.u8string()) != std::string::npos, "diagnostic lost its source path");
    };
    {
        const auto path = Case();
        Write(path / "shape.trench", "{it_trench {version 1}{default_socket 0}" + sockets + "}");
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty() && catalog.BlocksPreview("it_trench_draft_preview"), "unregistered authoring draft was activated");
    }
    auto sparse = Meta("it_trench_test",
        "{socket 7 {position 100 0 0} {yaw 0}}\n{socket 2 {position -100 0 0} {yaw 180}}", 7);
    {
        const auto path = Case(); Fixture(path, "it_trench_test", "\xef\xbb\xbf; BOM and comments\n" + sparse);
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Diagnostics().empty() && catalog.Modules().size() == 1, "valid BOM/comments/sparse IDs rejected");
        const auto* value = catalog.Modules()[0];
        Require(value->socketIds[0] == 2 && value->socketIds[1] == 7 && value->defaultSocket == 1,
            "default ID was confused with a dense array index");
    }
    Invalid("{it_trench {version 1}");
    Invalid("{it_trench {version 2}{default_socket 0}" + sockets + "}");
    Invalid(Meta("it_trench_test", sockets + "{socket 0 {position 0 0 0}{yaw 0}}"));
    Invalid(Meta("it_trench_test", sockets, 17));
    Invalid(Meta("it_trench_mg-nest")); // GEM truncates panel identifiers at a hyphen.
    Invalid(Meta("it_trench_test", "{socket 0 {position nan 0 0}{yaw 0}}"));
    Invalid(Meta("it_trench_test", "{socket 0 {position 0 0 1}{yaw 0}}"));
    Invalid(Meta("it_trench_test", "{socket 0 {position 0 0 0}{yaw 0}{overlap 10}}"));
    Invalid(Meta("it_trench_test", sockets + "{socket 2 {position 0 0 0}{yaw 90}}{socket 3 {position 0 0 0}{yaw -90}}{socket 4 {position 0 0 0}{yaw 0}}"));
    Invalid(Meta("it_trench_test"), "{vertex 0 0}{vertex 10 10}{vertex 0 10}{vertex 10 0}");
    Invalid(Meta("it_trench_test"), "{vertex 0 0}{vertex 1 0}{vertex 2 0}");
    {
        const auto path = Case(); Fixture(path / "entity", "it_trench_test");
        Write(path / "entity/it_trench_test.def", "{{extension \"/entity/shape.mdl\"}}");
        trench::Catalog catalog; catalog.Load(path / "entity");
        Require(catalog.Diagnostics().empty() && catalog.Modules().size() == 1, "GEM root-relative extension resolved against the drive root");
    }
    {
        const auto path = Case(); Fixture(path, "it_trench_test", "{it_trench\n{version 1}\n{bogus 0}\n}");
        trench::Catalog catalog; catalog.Load(path);
        Require(!catalog.Diagnostics().empty() && catalog.Diagnostics()[0].find(":3: unknown field") != std::string::npos,
            "diagnostic lost the field line number");
    }
    {
        const auto path = Case(); Fixture(path / "good", "it_trench_good");
        Fixture(path / "bad", "it_trench_bad", "{it_trench {version 99}}");
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().size() == 1 && catalog.FindEntity("it_trench_good") &&
            catalog.BlocksPreview("it_trench_bad_preview") && !catalog.Diagnostics().empty(), "one bad file disabled healthy sections");
    }
    {
        const auto path = Case(); Fixture(path / "a", "it_trench_duplicate");
        Fixture(path / "b", "it_trench_duplicate"); Fixture(path / "c", "it_trench_good");
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().size() == 1 && !catalog.FindEntity("it_trench_duplicate") &&
            catalog.FindEntity("it_trench_good"), "duplicate resolution depends on filesystem order");
    }
    {
        const auto path = Case(); Fixture(path, "it_trench_test");
        Write(path / "it_trench_alias.def", "{{extension \"shape.mdl\"}}");
        Write(path / "it_trench_alias_preview.def", "{{extension \"shape_preview.mdl\"}}");
        auto metadata = Meta("it_trench_test");
        metadata.insert(metadata.find("{default_socket"), "{variant \"it_trench_alias\" {preview \"it_trench_test_preview\"}}\n");
        Write(path / "shape.trench", metadata);
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty() && catalog.BlocksPreview("it_trench_test_preview"), "ambiguous preview was registered");
    }
    for (const auto* missing : {"shape.mdl", "it_trench_test.def", "it_trench_test_preview.def", "shape_preview.mdl"}) {
        const auto path = Case(); Fixture(path, "it_trench_test");
        // A single fixture file, never a recursive cleanup.
        fs::remove(path / missing);
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty() && !catalog.Diagnostics().empty(), "missing dependency accepted");
    }
    {
        const auto path = Case(); Fixture(path, "it_trench_test");
        Write(path / "shape.mdl", "{area \"cut\" {polygon2 {radius 1}" + rectangle + "}{rotate}}");
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty(), "unsupported cut radius silently ignored");
    }
    {
        const auto path = Case(); Fixture(path, "it_trench_test");
        Write(path / "it_trench_test.def", "{{extension \"shape_preview.mdl\"}}");
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Modules().empty(), "variant points to another model");
    }
    {
        const auto path = Case();
        const auto anchors = sockets + "{socket 3 {position 0 20 0}{yaw 90}}{socket 9 {position 0 -20 0}{yaw -90}}";
        Fixture(path, "it_trench_test", Meta("it_trench_test", anchors, 9));
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Diagnostics().empty() && catalog.Modules()[0]->socketCount == 4 &&
            catalog.Modules()[0]->defaultSocket == 3, "four-way socket data rejected");
    }
    {
        const auto path = Case();
        const std::string concave = "{vertex 0 0}{vertex 4 0}{vertex 4 1}{vertex 1 1}{vertex 1 4}{vertex 0 4}";
        Fixture(path, "it_trench_test", {}, concave);
        trench::Catalog catalog; catalog.Load(path);
        Require(catalog.Diagnostics().empty(), "concave polygon rejected");
        const auto* value = catalog.Modules()[0];
        Require(value->triangleCount == 4, "concave cut not triangulated");
        double area = 0;
        for (unsigned i = 0; i < value->triangleCount; ++i) {
            const auto triangle = value->triangles[i];
            const auto a = value->cut[triangle.a], b = value->cut[triangle.b], c = value->cut[triangle.c];
            area += std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
        }
        Require(std::fabs(area - 7.0) < 0.001, "triangulation filled the L-shaped notch");
        auto Covered = [&](TrenchPoint p) {
            auto Cross = [](TrenchPoint a, TrenchPoint b, TrenchPoint c) {
                return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            };
            for (unsigned i = 0; i < value->triangleCount; ++i) {
                const auto t = value->triangles[i];
                const auto a = value->cut[t.a], b = value->cut[t.b], c = value->cut[t.c];
                const auto x = Cross(a, b, p), y = Cross(b, c, p), z = Cross(c, a, p);
                if ((x >= 0 && y >= 0 && z >= 0) || (x <= 0 && y <= 0 && z <= 0)) return true;
            }
            return false;
        };
        Require(!Covered({2, 2}) && Covered({0.5f, 2}), "triangles cover the notch or omit the actual trench arm");
    }
    for (unsigned count : {64u, 65u}) {
        std::ostringstream vertices;
        for (unsigned i = 0; i < count; ++i) {
            const auto angle = i * 6.283185307179586 / count;
            vertices << "{vertex " << std::cos(angle) * 100 << ' ' << std::sin(angle) * 100 << '}';
        }
        const auto path = Case(); Fixture(path, "it_trench_test", {}, vertices.str());
        trench::Catalog catalog; catalog.Load(path);
        Require((catalog.Modules().size() == 1) == (count == 64), "cut buffer limit not enforced");
    }
    std::printf("Catalog tests passed: %zu active variants, parser/geometry/errors/lookup/lifetime fixtures.\n", actual.Modules().size());
}
