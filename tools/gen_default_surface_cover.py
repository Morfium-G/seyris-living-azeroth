#!/usr/bin/env python3
"""Generate SurfaceCover.cdbc for wxl-seyris-living-azeroth's surface cover (snow, sand, ...).

Classic WDBC binary ("cdbc" is only a naming convention for custom tables): 20-byte header, flat
4-byte columns, a trailing string block (just the empty string here, no string columns).

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID (Map or AreaTable ID, 0 for global),
GroundEffectID (GroundEffectTexture ID, 0 = any), TerrainType (TerrainType.dbc ID, -1 = any;
0 is Dirt), Depth (yd, 0 = no cover), Rim (x depth), RelaxSeconds, Flags (reserved, 0).
Floats; -1 = take this field from the next, less specific row. No row at all = no cover.

Resolution (orchestration docs/principles.md, "scoped override tables"): the most specific row
wins. Place first -- the terrain cell's area, its parent zones, the map, global -- then within
each place ground effect, TerrainType, everything. Values are never combined.

Stock TerrainType IDs: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand,
8 Soggy, 9 Dusty Grass, 10 None, 11 Water.

DEFAULT_ROWS is the shipped baseline: snow on, sand off (set its Depth to e.g. 0.08 to try it).
EXAMPLE_ROWS show the other kinds of row; pass --examples to include them.

Usage:  python gen_default_surface_cover.py [out_dir] [--examples]      (default out_dir: ../data)
Then copy SurfaceCover.cdbc into <client>/DBFilesClient/ and press "Reload table" in the surface
cover panel.
"""

import struct
import sys
from pathlib import Path

MAGIC = b"WDBC"
I = -1.0  # take this field from the next row
ANY = -1  # any TerrainType

GLOBAL, MAP, AREA = 0, 1, 2
SNOW, SAND = 3, 7

# (id, scopeType, scopeId, groundEffectId, terrainType, depth, rim, relaxSeconds, flags)
DEFAULT_ROWS = [
    (1, GLOBAL, 0, 0, SNOW, 0.35, 0.3, 30.0, 0),   # snow everywhere: 0.35 yd, rim 0.3x, 30 s to relax
    (2, GLOBAL, 0, 0, SAND, 0.0,  0.2, 8.0,  0),   # sand: off for now (try Depth 0.08)
]

EXAMPLE_ROWS = [
    (100, AREA, 210, 0, SNOW, 0.6, I, 60.0, 0),    # Icecrown: deeper snow, trenches stay longer
    (101, MAP, 1, 0, SAND, 0.08, I, I, 0),         # Kalimdor: a thin sand cover, rim/relax from global
]


def build(rows) -> bytes:
    field_count = 9
    record_size = field_count * 4
    records = bytearray()
    for r in sorted(rows, key=lambda r: r[0]):
        records += struct.pack("<IIIIi3fI", *r)
    string_block = b"\x00"
    header = struct.pack("<4sIIII", MAGIC, len(rows), field_count, record_size, len(string_block))
    return header + bytes(records) + string_block


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
