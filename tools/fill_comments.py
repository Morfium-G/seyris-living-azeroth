#!/usr/bin/env python3
"""Fill the empty Comment column of this module's scoped tables with each row's place name.

AreaClimate, FogSettings and WindProfile rows name a place by ScopeType (0 global, 1 map, 2 area) and
ScopeID; the Comment column (free text, never read by the module) shows which place that is in an
editor. This writes "Global", the map's or the area's name (FogSettings: plus the kind), but only
into comments that are empty: anything typed by hand is kept.

The names come from AreaTable.dbc and Map.dbc in the given folders, in order: a later folder's file
wins over an earlier one's (e.g. the client's Data\\Patch-A.mpq\\DBFilesClient, then its
DBFilesClient). Files without the Comment column yet: run upgrade_tables.py first.

Each changed file is backed up first as <name>.cdbc.<timestamp>.bak next to it, and checked after
writing: every column except Comment must read back unchanged.

Usage:  python fill_comments.py <cdbc folder or file> [...] --dbc <folder> [<folder> ...]
"""

import sys
from pathlib import Path

import cdbc_io as io

SCOPED = {"AreaClimate", "FogSettings", "WindProfile"}
KIND_NAMES = {0: "all kinds", 1: "mist", 2: "water fog", 3: "steam", 4: "area fog", 5: "haze"}

# Name columns (3.3.5 layouts): AreaTable AreaName_lang at 11 (enUS) .. 26, Map MapName_lang at 5 .. 20.
NAME_COLUMNS = {"AreaTable": (11, 16), "Map": (5, 16)}


def load_names(table: str, folders):
    """ID -> name from the last folder that has <table>.dbc (empty dict if none)."""
    path = None
    for folder in folders:
        candidate = Path(folder) / f"{table}.dbc"
        if candidate.is_file():
            path = candidate
    if not path:
        print(f"no {table}.dbc in {', '.join(map(str, folders))}: names from it stay empty")
        return {}
    fields, rows, string_at = io.read(path.read_bytes())
    first, count = NAME_COLUMNS[table]
    names = {}
    for r in rows:
        name = ""
        for c in range(first, min(first + count, fields)):  # enUS first, then any locale that has one
            name = string_at(r[c]) if r[c] else ""
            if name:
                break
        names[r[0]] = name
    print(f"{table}: {len(names)} names from {path}")
    return names


def place_name(scope_type: int, scope_id: int, areas, maps) -> str:
    if scope_type == 0:
        return "Global"
    if scope_type == 1:
        return f"{maps.get(scope_id) or f'Map {scope_id}'} (map)"
    if scope_type == 2:
        return areas.get(scope_id) or f"Area {scope_id}"
    return f"scope {scope_type}:{scope_id}"


def fill(path: Path, areas, maps) -> None:
    name = path.stem
    if name not in SCOPED:
        return
    columns, _ = io.TABLES[name]
    data = path.read_bytes()
    fields, rows, string_at = io.read(data)
    if fields != len(columns):
        print(f"{path}: {fields} columns, expected {len(columns)}: run upgrade_tables.py first; skipped")
        return
    comment = len(columns) - 1  # Comment is the last column of all three
    values = []
    changed = 0
    for r in rows:
        row = []
        for c, t in enumerate(columns):
            if t == "s":
                row.append(string_at(r[c]) if r[c] else "")
            elif t == "f":
                row.append(io.as_float(r[c]))
            elif t == "i":
                row.append(io.as_int(r[c]))
            else:
                row.append(r[c])
        if not row[comment]:
            text = place_name(row[1], row[2], areas, maps)
            if name == "FogSettings":
                text += " - " + KIND_NAMES.get(row[3], f"kind {row[3]}")  # plain ASCII: any editor shows it
            row[comment] = text
            changed += 1
        values.append(row)
    if not changed:
        print(f"{path}: every comment already set ({len(rows)} rows)")
        return

    out = io.build(columns, [tuple(v) for v in values])
    # Check: every column but Comment reads back as before (same rows, same order by ID).
    _, new_rows, _ = io.read(out)
    old_by_id = {r[0]: r for r in rows}
    for r in new_rows:
        old = old_by_id.get(r[0])
        if old is None or any(old[c] != r[c] for c in range(len(columns)) if c != comment):
            raise SystemExit(f"{path}: row {r[0]} would change outside Comment; nothing written")
    if len(new_rows) != len(rows):
        raise SystemExit(f"{path}: row count would change; nothing written")

    backup = io.backup_path(path)
    backup.write_bytes(data)
    path.write_bytes(out)
    print(f"{path}: {changed} of {len(rows)} comments filled; backup {backup.name}")


def main() -> None:
    args = sys.argv[1:]
    if "--dbc" not in args:
        raise SystemExit(__doc__)
    split = args.index("--dbc")
    targets, folders = args[:split], args[split + 1:]
    if not targets or not folders:
        raise SystemExit(__doc__)
    areas = load_names("AreaTable", folders)
    maps = load_names("Map", folders)
    for arg in targets:
        p = Path(arg)
        for f in (sorted(p.glob("*.cdbc")) if p.is_dir() else [p]):
            fill(f, areas, maps)


if __name__ == "__main__":
    main()
