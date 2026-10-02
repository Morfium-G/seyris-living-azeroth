#!/usr/bin/env python3
"""Generate SurfaceCover.cdbc for wxl-seyris-living-azeroth's surface cover (snow, sand, ...).

Classic WDBC binary ("cdbc" is only a naming convention for custom tables): 20-byte header, flat
4-byte columns, a trailing string block (string columns hold offsets into it; 0 = empty).

Columns (see docs/cdbc-tables.md for the full meaning):
  ID, ScopeType (0 global / 1 map / 2 area), ScopeID (Map or AreaTable ID, 0 for global),
  TexturePath (string, empty = any), GroundEffectID (0 = any), TerrainType (-1 = any; 0 is Dirt),
  Depth (yd, 0 = no cover), MaxSlope, SlopeFade (degrees), DriftNoise, EdgeBreakup (0..1),
  Rim (x depth), RelaxSeconds, TintColor (0xRRGGBB), TintStrength (0..1),
  CoverTexture (string), Opacity, Flatten -- reserved, not used yet --, Flags (0).
Floats; -1 = take this field from the next, less specific row (TintColor goes with
TintStrength). No row at all = no cover.

Resolution (orchestration docs/principles.md, "scoped override tables"): the most specific row
wins. Place first -- the terrain cell's area, its parent zones, the map, global -- then within
each place texture, ground effect, TerrainType, everything. Values are never combined.

Stock TerrainType IDs: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand,
8 Soggy, 9 Dusty Grass, 10 None, 11 Water.

DEFAULT_ROWS is the shipped baseline: snow on, sand off (set its Depth to e.g. 0.08 to try it).
EXAMPLE_ROWS show the other kinds of row; pass --examples to include them.

Usage:  python gen_default_surface_cover.py [out_dir] [--examples]      (default out_dir: ../data)
Then copy SurfaceCover.cdbc into <client>/DBFilesClient/ and press "Reload table" in the surface
cover panel. The panel's "here:" line shows the texture path, ground effect, TerrainType and
area under the player -- copy them from there into rows.
"""

import struct
import sys
from pathlib import Path

MAGIC = b"WDBC"
I = -1.0  # take this field from the next row
ANY = -1  # any TerrainType

GLOBAL, MAP, AREA = 0, 1, 2
SNOW, SAND, SOGGY = 3, 7, 8

# Row helper: everything not given inherits (-1) or is empty.
def row(id, scope, scope_id=0, texture="", effect=0, terrain=ANY, depth=I, max_slope=I, slope_fade=I,
        drift=I, breakup=I, rim=I, relax=I, tint_color=0, tint_strength=I, cover_texture="",
        opacity=I, flatten=I, flags=0):
    return (id, scope, scope_id, texture, effect, terrain, depth, max_slope, slope_fade, drift, breakup,
            rim, relax, tint_color, tint_strength, cover_texture, opacity, flatten, flags)


DEFAULT_ROWS = [
    # Snow everywhere it's painted: 0.35 yd, bare above ~40 degrees, gentle drifts, ragged edges.
    row(1, GLOBAL, terrain=SNOW, depth=0.35, max_slope=40.0, slope_fade=15.0, drift=0.35, breakup=0.5,
        rim=0.3, relax=30.0, tint_strength=0.0),
    # Sand: off for now (try Depth 0.08). Flatter, quicker to relax, sandy tint.
    row(2, GLOBAL, terrain=SAND, depth=0.0, max_slope=30.0, slope_fade=10.0, drift=0.25, breakup=0.7,
        rim=0.2, relax=8.0, tint_color=0xD8C08A, tint_strength=0.6),
]

EXAMPLE_ROWS = [
    # Icecrown (area 210): deeper snow, trenches stay longer; the rest from the global snow row.
    row(100, AREA, 210, terrain=SNOW, depth=0.6, relax=60.0),
    # One specific texture (copy the exact path from the panel's "here:" line): a thin, bluish crust.
    row(101, GLOBAL, texture="Tileset\\Example\\ExampleSnowCrust.blp", depth=0.15,
        tint_color=0xC8DCFF, tint_strength=0.3),
    # Kalimdor (map 1): a thin sand cover, everything else from the global sand row.
    row(102, MAP, 1, terrain=SAND, depth=0.08),
]

FIELDS = "<IIIIIi5f2fIfIffI"  # see row(): strings are packed as offsets


def build(rows) -> bytes:
    strings = bytearray(b"\x00")
    offsets = {"": 0}

    def string(s):
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s.encode("utf-8") + b"\x00")
        return offsets[s]

    records = bytearray()
    for r in sorted(rows, key=lambda r: r[0]):
        (rid, scope, scope_id, texture, effect, terrain, depth, max_slope, slope_fade, drift, breakup,
         rim, relax, tint_color, tint_strength, cover_texture, opacity, flatten, flags) = r
        records += struct.pack(FIELDS, rid, scope, scope_id, string(texture), effect, terrain,
                               depth, max_slope, slope_fade, drift, breakup, rim, relax,
                               tint_color, tint_strength, string(cover_texture), opacity, flatten, flags)
    field_count = 19
    record_size = field_count * 4
    assert len(records) == record_size * len(rows)
    header = struct.pack("<4sIIII", MAGIC, len(rows), field_count, record_size, len(strings))
    return header + bytes(records) + bytes(strings)


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rows = DEFAULT_ROWS + (EXAMPLE_ROWS if "--examples" in sys.argv else [])
    out_dir = Path(args[0]) if args else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "SurfaceCover.cdbc"
    path.write_bytes(build(rows))
    print(f"wrote {path} ({len(rows)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
