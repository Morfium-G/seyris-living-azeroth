#!/usr/bin/env python3
"""Append missing columns to this module's tables, so they open in an editor with every column.

Columns are only ever appended (docs/cdbc-tables.md), and the module already reads older files
(missing columns count as "not set"). This only matters for editing: it rewrites a file with the
current layout, the new columns filled with their "not set" value, so every row behaves exactly as
before. Rows, IDs and strings are copied unchanged.

Each changed file is backed up first as <name>.cdbc.<timestamp>.bak next to it. Files already up
to date, and files this tool doesn't know, are left alone.

Usage:  python upgrade_tables.py <DBFilesClient folder or .cdbc file> [...]
"""

import struct
import sys
import time
from pathlib import Path

import cdbc_io as io


def upgrade(path: Path) -> None:
    name = path.stem
    if name not in io.TABLES:
        return
    columns, missing = io.TABLES[name]
    data = path.read_bytes()
    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", data, 0)
    if magic != io.MAGIC:
        print(f"{path}: not a WDBC file, skipped")
        return
    if fields == len(columns):
        print(f"{path}: up to date ({fields} columns)")
        return
    if fields > len(columns):
        print(f"{path}: has {fields} columns, more than this tool knows ({len(columns)}): skipped (a different layout?)")
        return
    pad = b"".join(struct.pack("<I", io.encode(columns[c], missing[c])) for c in range(fields, len(columns)))
    records = data[20:20 + count * record_size]
    strings = data[20 + count * record_size:20 + count * record_size + string_size]
    out = bytearray()
    for i in range(count):
        out += records[i * record_size:(i + 1) * record_size] + pad
    header = struct.pack("<4sIIII", magic, count, len(columns), len(columns) * 4, len(strings))
    backup = path.with_name(f"{path.name}.{time.strftime('%Y%m%d-%H%M%S')}.bak")
    backup.write_bytes(data)
    path.write_bytes(header + bytes(out) + strings)
    print(f"{path}: {fields} -> {len(columns)} columns ({count} rows); backup {backup.name}")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for arg in sys.argv[1:]:
        p = Path(arg)
        for f in (sorted(p.glob("*.cdbc")) if p.is_dir() else [p]):
            upgrade(f)


if __name__ == "__main__":
    main()
