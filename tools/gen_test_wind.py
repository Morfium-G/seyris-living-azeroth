#!/usr/bin/env python3
"""Generate a test WindProfile.cdbc for the first wind draft of wxl-seyris-living-azeroth.

Classic WDBC binary ("cdbc" is only a naming convention for custom tables): 20-byte header, flat
4-byte columns, a trailing string block (just the empty string here, no string columns).

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID, GroundMin, GroundMax, AloftMin,
AloftMax, AloftHeight, WeatherInfluence, GustStrength, GustFrequency, TerrainWeight,
LockedDirection, LockStrength, Flags. A negative float in a Map/Area row inherits from the scope
above. Strengths are 0..1; LockedDirection is the compass bearing the wind blows TOWARD
(0 = north, 90 = east).

Test rows, all around Orgrimmar so each behaviour can be checked on foot:
    Global          baseline everywhere
    Map 36          Deadmines: no wind at all
    Area 1637       Orgrimmar: sheltered city, weak and calm
    Area 14         Durotar: windy and very gusty
    Area 17         The Barrens: direction locked to east

Usage:  python gen_test_wind.py [out_dir]      (default: ../data)
Then copy WindProfile.cdbc into <client>/DBFilesClient/ and press "Reload" in the wind panel.
"""

import struct
import sys
from pathlib import Path

MAGIC = b"WDBC"
I = -1.0  # inherit

# (id, scopeType, scopeId, gMin, gMax, aMin, aMax, aloftH, weatherInf, gustStr, gustFreq, terrainW, lockDir, lockStr, flags)
ROWS = [
    (1, 0, 0,    0.10, 0.45, 0.30, 0.80, 40.0, 0.8, 0.6, 1.0, 1.0, 0.0, 0.0, 0),
    (2, 1, 36,   0.0,  0.0,  0.0,  0.0,  I,    I,   I,   I,   I,   I,   I,   0),
    (3, 2, 1637, 0.05, 0.20, I,    I,    I,    I,   0.3, I,   I,   I,   I,   0),
    (4, 2, 14,   0.30, 0.80, I,    I,    I,    I,   1.0, I,   I,   I,   I,   0),
    (5, 2, 17,   I,    I,    I,    I,    I,    I,   I,   I,   I,   90.0, 1.0, 0),
]


def build(rows) -> bytes:
    field_count = 15
    record_size = field_count * 4
    records = bytearray()
    for r in sorted(rows, key=lambda r: r[0]):
        records += struct.pack("<Iii11fI", *r)
    string_block = b"\x00"
    header = struct.pack("<4sIIII", MAGIC, len(rows), field_count, record_size, len(string_block))
    return header + bytes(records) + string_block


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "WindProfile.cdbc"
    path.write_bytes(build(ROWS))
    print(f"wrote {path} ({len(ROWS)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
