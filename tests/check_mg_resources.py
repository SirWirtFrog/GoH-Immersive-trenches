"""Check MG resource wiring against the installed game's archive definitions.

Does not launch GEM or substitute for its parser, interaction or network tests.
Run from the repository root: python tests/check_mg_resources.py
"""
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "mods/ImmersiveTrenches/resource"
GAME = ROOT / "gameFiles/resource"
NEST = RES / "entity/landscape/cut_entity/immersiveTrenches/dirt/trench_dirt_mg-nest"
WEAPONS = {
    "ger": "heavy_mg_mg34", "rus": "heavy_mg_maxim",
    "fin": "heavy_mg_maxim_m32_33", "usa": "heavy_mg_m1917_30cal",
    "eng": "heavy_mg_vickers_mg",
}
NATIONS = {"ger": "ger", "spa": "ger", "rus": "rus", "fin": "fin",
           "usa": "usa", "eng": "eng", "sco": "eng", "pol": "eng", "can": "eng"}


def normalized(data):
    return data.decode("utf-8-sig").replace("\r\r\n", "\n").replace("\r\n", "\n")


def text(path):
    return normalized(path.read_bytes())


def check_brackets(path):
    # Ignore comments and quoted strings, including escaped quotes.
    source = re.sub(r'"(?:\\.|[^"\\])*"|;[^\n]*', '', text(path))
    stack = []
    for char in source:
        if char in "({":
            stack.append(char)
        elif char in ")}":
            assert stack and stack.pop() == {')': '(', '}': '{'}[char], path
    assert not stack, path


def main():
    stock = zipfile.ZipFile(GAME / "gamelogic.pak")
    vehicles = zipfile.ZipFile(GAME / "entity/-vehicle.pak")
    construction = zipfile.ZipFile(GAME / "entity/construction.pak")
    native_names = set(stock.namelist())
    actions = text(RES / "interface/action/it_fortify.inc")
    menu = text(RES / "set/panel_control/right_it_trenches.set")
    # The legacy action has no grant in the faction engineer presets. If it
    # precedes them in the shared cell, GEM can preview it but reject its order.
    assert not re.search(r'\{it_trench_mg_nest\s*\n', menu), \
        'Legacy MG action shadows faction actions in the shared menu cell'
    skills = text(RES / "set/breed/presets/perks/skills.inc")
    events = text(RES / "set/interaction_entity/it_mg_sites.inc")
    catalog = text(ROOT / "src/GemTrenchCatalog.inc")
    model = text(NEST / "it_trench_mg_nest.mdl")
    fixed_rules = text(RES / "properties/it_trench_fixed_mg.inc")
    assert '{switch ""}' in fixed_rules
    for driver in ['driver1', 'driver2']:
        assert f'{{place "{driver}" remove}}' in fixed_rules
    assert '{move {workers clear}' in fixed_rules
    assert '{trailing {workers clear}}' in fixed_rules
    for speed in ['Slow', 'Normal', 'Fast', 'MaxSpeed']:
        assert f'{{{speed} 0}}' in fixed_rules
    assert '{bone hide "it_mg_crate"}' in events and '{volumes "body" disable all}' in events
    assert '{if tagged "it_mg_built"' in events, 'Finished crate must stay hidden after loading'
    registry = text(RES / "set/interaction_entity/entity.set")
    native_registry = normalized(stock.read("set/interaction_entity/entity.set"))
    assert registry.replace('\t(include "it_mg_sites.inc")\n', '') == native_registry
    assert not (RES / "interface/action/it_install_mg.set").exists()
    assert "effect_object" not in events and "it_mg_engineer" not in events
    native_crate = "construction/_military/boxes/ammo_box_01/ammo_box_01.mdl"
    assert native_crate in construction.namelist()
    crate_folder = NEST / 'mg_build_site'
    crate_model = text(crate_folder / 'it_mg_site.mdl')
    assert '{bone "it_mg_origin"' in crate_model
    assert '{bone "it_mg_crate"' in crate_model
    click_volume = crate_model.split('{volume "body"', 1)[1]
    assert '{bone "it_mg_crate"}' in click_volume, 'Clickable volume must follow the crate bone'
    # Keep the visible mesh, LOD and its material intact when adding the bones.
    for asset in ['box1.ply', 'box1_lod1.ply', 'material_#0.mtl']:
        assert (crate_folder / asset).read_bytes() == construction.read(
            'construction/_military/boxes/ammo_box_01/' + asset), asset
    for nation, weapon in WEAPONS.items():
        name = f"it_trench_mg_nest_{nation}"
        definition = text(NEST / f"{name}.def")
        preview = text(NEST / f"{name}_preview.def")
        site = text(NEST / f"it_mg_site_{nation}.def")
        assert f'"it_mg_nest_{nation}"' in definition
        assert "it_mg_nest_" not in preview, "preview must not spawn the build site"
        assert f'name({name}) preview({name}_preview) entity({name})' in actions
        action_line = next(line for line in actions.splitlines() if f'name({name})' in line)
        assert f'item(it_mg_build_{nation})' in action_line, \
            'Shared tools expose another faction action before native order validation'
        shovel_path = f'set/stuff/melee/shovel_{nation}'
        shovel = text(RES / shovel_path)
        assert shovel.replace(f'foxhole it_mg_build_{nation}', 'foxhole') == \
            normalized(stock.read(shovel_path)), 'Shovel stats or existing tags changed'
        assert f'"{name}", "{name}_preview"' in catalog
        assert f'{{{name}\n' in menu and f'mg_nest({name})' in skills
        fixed_name = f'it_trench_fixed_mg_{nation}'
        assert f'{{entity "{fixed_name}"}}' in site and '{buildTime 30}' in site
        assert '{actions' in site and '{call "it_mg_site_completed"}' in site
        assert '"it_mg_build_site"' in site
        assert '{extender "fortification"' in site and '{actor' in site
        assert any(Path(n).stem == weapon for n in vehicles.namelist() if n.endswith('.def'))
        stock_path = next(n for n in vehicles.namelist() if n.endswith(f'/{weapon}.def'))
        stock_definition = normalized(vehicles.read(stock_path))
        parent = '/entity/' + Path(stock_path).parent.as_posix() + '/'
        stock_definition = re.sub(r'(\(include ")([^"/][^"]*)("\))',
                                  lambda m: m[1] + parent + m[2] + m[3], stock_definition)
        stock_definition = re.sub(r'(\{extension ")([^"/][^"]*)("\})',
                                  lambda m: m[1] + parent + m[2] + m[3], stock_definition, flags=re.I)
        fixed_definition = text(NEST / f'{fixed_name}.def')
        expected = stock_definition.rsplit('}', 1)[0] + '\t(include "/properties/it_trench_fixed_mg.inc")\n}\n'
        assert fixed_definition == expected, 'Weapon stats/ammunition must stay stock'
        assert '{extension "mg_build_site/it_mg_site.mdl"}' in site
        mount = "it_mg_mount_maxim" if nation == "rus" else "it_mg_mount"
        assert f'{{spawn "it_mg_site_{nation}" "{mount}" x}}' in events
        assert f'{{bone "{mount}"' in model
        for state in range(4):
            icons = RES / "interface/scene/panel_control"
            assert (icons / f'{name}_{state:02d}.tga').read_bytes() == (
                icons / f'it_trench_mg_nest_{state:02d}.tga').read_bytes()
        for lang, extension in [('ru', 'po'), ('default', 'pot')]:
            local = text(RES.parent / 'localizations' / lang / 'interface/text/desc' /
                         f'dlg_immersive_trenches.{extension}')
            for suffix in ['', '/action', '/usage']:
                assert f'msgctxt "actionbar/{name}{suffix}"' in local

    count = 0
    for name in native_names:
        if not name.startswith('set/breed/') or not name.endswith('.set'):
            continue
        original = normalized(stock.read(name))
        if not re.search(r'\("engineer(?:_heavy_fin)?"\)', original):
            continue
        nation = re.search(r'\{nationality\s+(\w+)\}', original)[1]
        expected = original.replace('("engineer")', f'("it_engineer_{NATIONS[nation]}")')
        assert text(RES / name) == expected, f'Unexpected equipment/perk changes: {name}'
        tool = f'shovel_{NATIONS[nation]}'
        if '{item "shovel_engineer_fin"}' in original:
            tool = 'shovel_engineer_fin'
        assert f'{{item "{tool}"}}' in original, f'Missing faction construction tool: {name}'
        # All relative native includes must still resolve after the override.
        for include in re.findall(r'\(include "([^"]+)"\)', expected):
            relative = (Path(name).parent / include).as_posix()
            assert relative in native_names or (RES / relative).exists(), relative
        count += 1
    assert count == 93, f'Review engineer coverage for this game build: {count}'
    heavy_path = 'set/stuff/special/shovel_engineer_fin'
    assert text(RES / heavy_path).replace(' "it_mg_build_fin"', '') == \
        normalized(stock.read(heavy_path)), 'Heavy Finnish shovel stats changed'
    for path in RES.rglob('*'):
        if path.suffix in ['.def', '.mdl', '.inc', '.set']:
            check_brackets(path)
    print(f'PASS: 5 faction nests, native weapons/stages, menu/icons/locales, {count} unchanged engineer loadouts')


if __name__ == '__main__':
    main()
