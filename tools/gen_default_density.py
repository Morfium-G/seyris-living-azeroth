#!/usr/bin/env python3
"""Generate GroundEffectDoodadDensity.cdbc for wxl-seyris-living-azeroth's instanced grass.

Classic WDBC binary ("cdbc" is only a naming convention for custom tables): 20-byte header, flat
4-byte columns, a trailing string block (just the empty string here, no string columns).

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID (Map or AreaTable ID, 0 for global),
DoodadID (GroundEffectDoodad ID, 0 = every doodad), GroundEffectID (GroundEffectTexture ID,
0 = any), Multiplier, Radius, Spread. Floats; -1 = take this field from the next, less specific
row. No row at all = no extra plants.

Resolution (orchestration docs/principles.md, "scoped override tables"): the most specific row
wins. Place first -- the chunk's area, its parent zones, the map, global -- then within each place
doodad+ground effect, doodad, ground effect, every doodad (0). Values are never combined.
The player's limits (WarcraftXL.ini GrassDensityMax*) cap the result.

DEFAULT_ROWS is the shipped baseline. EXAMPLE_ROWS show the other kinds of row; pass --examples
to include them (they use stock Elwynn Forest IDs: map 0, area 12).

Usage:  python gen_default_density.py [out_dir] [--examples]      (default out_dir: ../data)
Then copy GroundEffectDoodadDensity.cdbc into <client>/DBFilesClient/ and press "Reload" in the
wind panel's instanced grass section.
"""

import struct
import sys
from pathlib import Path

MAGIC = b"WDBC"
I = -1.0  # take this field from the next row

GLOBAL, MAP, AREA = 0, 1, 2

# (id, scopeType, scopeId, doodadId, groundEffectId, multiplier, radius, spread)
DEFAULT_ROWS = [
    (1, GLOBAL, 0, 0, 0, 3.0, 80.0, 1.5),   # every doodad, everywhere: 3x within 80 yd
]

EXAMPLE_ROWS = [
    (100, AREA, 12, 0, 0, 5.0, I, I),        # Elwynn Forest: every doodad 5x, radius/spread from global
]


def build(rows) -> bytes:
    field_count = 8
    record_size = field_count * 4
    records = bytearray()
    for r in sorted(rows, key=lambda r: r[0]):
        records += struct.pack("<IIIII3f", *r)
    string_block = b"\x00"
    header = struct.pack("<4sIIII", MAGIC, len(rows), field_count, record_size, len(string_block))
    return header + bytes(records) + string_block


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rows = DEFAULT_ROWS + (EXAMPLE_ROWS if "--examples" in sys.argv else [])
    out_dir = Path(args[0]) if args else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "GroundEffectDoodadDensity.cdbc"
    path.write_bytes(build(rows))
    print(f"wrote {path} ({len(rows)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
