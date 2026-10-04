#!/usr/bin/env python3
"""Generate GroundMaterial.cdbc, GroundMaterialSelector.cdbc and SurfaceCover.cdbc for
wxl-seyris-living-azeroth (see docs/cdbc-tables.md for every column, and the orchestration
design docs/r&d/immersion/world-fields.md).

  GroundMaterialSelector  WHERE a material lies: place (0 global / 1 map / 2 area, ScopeID) then
                          TexturePath (empty = any) -> GroundEffectID (0 = any) -> TerrainType
                          (-1 = any; 0 is Dirt) -> everything. The most specific row wins; MaterialID
                          0 = "no material here" (switches a broader row off). LiquidType: reserved, 0.
  GroundMaterial          WHAT a material is: Name, Parent (0 = none; -1 fields come from the
                          parent), RestMoisture (0 bone dry .. 1 soaked: what the texture already
                          shows), Stiffness (0 gives way completely .. 1 rigid), Flags (0),
                          Absorbency (how much rain and water nearby reach it: rock ~0.1 .. sand ~0.9),
                          Temperature (degC of the material itself; -1000 = none; liquids: magma ~1000).
  SurfaceCover            the cover a material carries, keyed by MaterialID; -1 fields come from the
                          parent material's row. CoverMaterial = what lies on top (0 = the ground
                          material itself; its Stiffness decides how far feet press the cover down).

Stock TerrainType IDs: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand,
8 Soggy, 9 Dusty Grass, 10 None, 11 Water.

The DEFAULT lists are the shipped baseline: snow on, sand off. EXAMPLE lists show the other kinds
of row; pass --examples to include them.

Usage:  python gen_default_ground_materials.py [out_dir] [--examples]      (default out_dir: ../data)
Then copy the three files into <client>/DBFilesClient/ and press "Reload table" in the surface
cover panel. Its "here:" lines show the texture, ground effect, TerrainType, area and material
under the player.
"""

import sys
from pathlib import Path

import cdbc_io as io

I = -1.0  # take this field from the parent
ANY = -1  # any TerrainType
GLOBAL, MAP, AREA = 0, 1, 2
SNOW, SAND, SOGGY = 3, 7, 8
# SurfaceCover flags (they come from the row that decides Depth; 0x8 belongs to the row's CoverTexture)
PREVENT_UNDERWATER, PREVENT_ON_LAND, USE_MCCV, IGNORE_SPECULAR = 0x1, 0x2, 0x4, 0x8


T = -1000.0  # signed columns: not set


def material(id, name, parent=0, rest_moisture=I, stiffness=I, flags=0, absorbency=I, temperature=T):
    return (id, name, parent, rest_moisture, stiffness, flags, absorbency, temperature)


def selector(id, material_id, scope=GLOBAL, scope_id=0, texture="", effect=0, terrain=ANY, liquid=0, flags=0):
    return (id, scope, scope_id, texture, effect, terrain, liquid, material_id, flags)


def cover(id, material_id, cover_material=0, depth=I, max_slope=I, slope_fade=I, drift=I, breakup=I, rim=I, relax=I,
          tint_color=0, tint_strength=I, cover_texture="", opacity=I, flatten=I, z_offset=I, wetness=I, flags=0,
          melt_kept=I, melt_gone=T):
    return (id, material_id, cover_material, depth, max_slope, slope_fade, drift, breakup, rim, relax,
            tint_color, tint_strength, cover_texture, opacity, flatten, z_offset, wetness, flags, melt_kept, melt_gone)


DEFAULT_MATERIALS = [
    material(1, "Snow", rest_moisture=0.5, stiffness=0.0, absorbency=0.3),
    material(2, "Sand", rest_moisture=0.2, stiffness=0.2, absorbency=0.9),
]
DEFAULT_SELECTORS = [
    selector(1, 1, terrain=SNOW),
    selector(2, 2, terrain=SAND),
]
DEFAULT_COVERS = [
    # Snow everywhere it's painted: 0.35 yd, bare above ~40 degrees, gentle drifts, ragged edges.
    cover(1, 1, depth=0.35, max_slope=40.0, slope_fade=15.0, drift=0.35, breakup=0.5, rim=0.3, relax=30.0, tint_strength=0.0),
    # Sand: off for now (try Depth 0.08). Flatter, quicker to relax, the Tanaris sand texture on it.
    cover(2, 2, depth=0.0, max_slope=30.0, slope_fade=10.0, drift=0.25, breakup=0.7, rim=0.2, relax=8.0,
          tint_color=0xD8C08A, tint_strength=0.0, cover_texture=r"Tileset\Tanaris\TanarisSandBase01.blp"),
]

EXAMPLE_MATERIALS = [
    # Icecrown's snow: a child of Snow; only the cover differs (below).
    material(100, "IcecrownSnow", parent=1),
    # A wet sand texture: same sand, but its texture already shows it wet.
    material(101, "WestfallSandWet", parent=2, rest_moisture=0.8),
    # Mud: soft but not bottomless -- feet press 60% of it away, so prints keep a wet floor.
    material(102, "Mud", rest_moisture=0.8, stiffness=0.4, absorbency=0.7),
    # Liquids (selected by LiquidType.dbc ID below). Without rows they're built in by category:
    # water, ocean and slime wet the ground around them, magma doesn't and is 1000 degC.
    material(200, "Magma", rest_moisture=0.0, temperature=1100.0),
    material(201, "HotSpring", rest_moisture=1.0, temperature=45.0),
]
EXAMPLE_SELECTORS = [
    selector(100, 100, scope=AREA, scope_id=210, terrain=SNOW),                       # Icecrown (area 210)
    selector(101, 101, scope=AREA, scope_id=40, texture=r"Tileset\Westwood\WestfallSandBaseWet.blp"),  # Westfall (area 40)
    selector(102, 102, terrain=SOGGY),
    # Kalimdor (map 1): no snow material at all (MaterialID 0 switches the global row off there).
    selector(103, 0, scope=MAP, scope_id=1, terrain=SNOW),
    # Liquids by LiquidType.dbc ID: 3 Magma everywhere; water (1) in Un'Goro Crater (490) is a hot spring.
    selector(200, 200, liquid=3),
    selector(201, 201, scope=AREA, scope_id=490, liquid=1),
]
EXAMPLE_COVERS = [
    # Deeper snow, trenches stay longer; the rest from Snow's row.
    cover(100, 100, depth=0.6, relax=60.0),
    # Muddy footprint outlines on grass (owner's design): the layer's top stays just below the terrain
    # (0.015 - 0.02 = -0.005 yd), so only trench rims poke through (0.015 x (1 + 4) - 0.02 = +0.055 yd).
    # No drift noise, or the lumps would poke through too. Not under water.
    cover(102, 102, depth=0.015, z_offset=-0.02, wetness=0.8, drift=0.0, rim=4.0, relax=120.0,
          tint_color=0x3A2A1A, tint_strength=0.6, flags=PREVENT_UNDERWATER),
]


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    examples = "--examples" in sys.argv
    out_dir = Path(args[0]) if args else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, columns, rows in (
            ("GroundMaterial", io.GROUND_MATERIAL, DEFAULT_MATERIALS + (EXAMPLE_MATERIALS if examples else [])),
            ("GroundMaterialSelector", io.GROUND_MATERIAL_SELECTOR, DEFAULT_SELECTORS + (EXAMPLE_SELECTORS if examples else [])),
            ("SurfaceCover", io.SURFACE_COVER, DEFAULT_COVERS + (EXAMPLE_COVERS if examples else []))):
        path = out_dir / f"{name}.cdbc"
        path.write_bytes(io.build(columns, rows))
        print(f"wrote {path} ({len(rows)} row(s))")
    print("copy them into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
