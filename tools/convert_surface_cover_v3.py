#!/usr/bin/env python3
"""Convert a SurfaceCover.cdbc from the 19-column layout to the 21-column layout.

The 21-column layout inserts two float columns after Flatten, before Flags:
  ... CoverTexture(15), Opacity(16), Flatten(17), ZOffset(18), Wetness(19), Flags(20)
Both new columns are written as -1 ("take this field from the next row"), so every row behaves
exactly as before. Strings (TexturePath, CoverTexture) and all other values are copied as they are.

The input file is never modified: the result goes to a new file (default: <input>-v3.cdbc).

Usage:  python convert_surface_cover_v3.py <SurfaceCover.cdbc> [out.cdbc]
"""

import struct
import sys
from pathlib import Path

OLD_FIELDS, NEW_FIELDS = 19, 21
INSERT_AT = 18  # the new columns go here (before Flags)
INHERIT = struct.pack("<f", -1.0)


def convert(data: bytes) -> bytes:
    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", data, 0)
    if magic != b"WDBC":
        raise SystemExit("not a WDBC/cdbc file")
    if fields == NEW_FIELDS:
        raise SystemExit("already in the 21-column layout")
    if fields != OLD_FIELDS or record_size != OLD_FIELDS * 4:
        raise SystemExit(f"unexpected layout: {fields} fields, {record_size}-byte records (expected 19 / 76)")

    records = data[20:20 + count * record_size]
    strings = data[20 + count * record_size:20 + count * record_size + string_size]
    out = bytearray()
    for i in range(count):
        rec = records[i * record_size:(i + 1) * record_size]
        out += rec[:INSERT_AT * 4] + INHERIT + INHERIT + rec[INSERT_AT * 4:]
    header = struct.pack("<4sIIII", magic, count, NEW_FIELDS, NEW_FIELDS * 4, len(strings))
    return header + bytes(out) + strings


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_name(src.stem + "-v3" + src.suffix)
    if dst.resolve() == src.resolve():
        raise SystemExit("refusing to overwrite the input; give a different output path")
    dst.write_bytes(convert(src.read_bytes()))
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
