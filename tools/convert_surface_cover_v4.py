#!/usr/bin/env python3
"""Split a 21-column SurfaceCover.cdbc (place keys in the table itself) into the material layout:

  GroundMaterialSelector.cdbc  where a material lies (the old rows' place + texture/effect/TerrainType keys)
  GroundMaterial.cdbc          one material per old row (Name from its texture or keys)
  SurfaceCover.cdbc            the old rows' cover values, keyed by MaterialID

Every old row becomes material, selector and cover row with the same ID, so nothing renumbers.
Inheritance: an old row's -1 fields came from the next less specific row that also matched. Each
new material gets that row as its Parent, so -1 fields resolve as before:
  - same place: the row's own GroundEffectID, then its own TerrainType, then the "everything" row;
  - then the map (for area rows) and global, at every level including the same texture.
Area rows can't see parent zones offline (the client knows the zone chain): those parents are
left out and reported. New material columns (RestMoisture, Stiffness) are written as -1 (inherit
-> defaults), which behaves exactly like before.

Never modifies the input. Output goes to a folder (default: <input folder>/converted-v4/).

Usage:  python convert_surface_cover_v4.py <SurfaceCover.cdbc> [out_dir]
"""

import sys
from pathlib import Path

import cdbc_io as io

GLOBAL, MAP, AREA = 0, 1, 2
TERRAIN_NAMES = ["Dirt", "Metallic", "Stone", "Snow", "Wood", "Grass", "Leaves", "Sand", "Soggy", "DustyGrass", "None", "Water"]


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else src.parent / "converted-v4"
    fields, raw, s = io.read(src.read_bytes())
    if fields == 19:
        raise SystemExit("19-column file: run convert_surface_cover_v3.py on it first")
    if fields != 21:
        raise SystemExit(f"expected the 21-column layout, got {fields} columns")

    rows = []
    for r in raw:
        rows.append(dict(
            id=r[0], scope=r[1], scope_id=0 if r[1] == GLOBAL else r[2], texture=s(r[3]), effect=r[4],
            terrain=io.as_int(r[5]), values=r[6:], cover_texture=s(r[15])))

    def norm(t):
        return t.replace("/", "\\").lower()

    def level_of(r):
        return 0 if r["texture"] else (1 if r["effect"] else (2 if r["terrain"] >= 0 else 3))

    def find(scope, scope_id, level, texture="", effect=0, terrain=-1):
        for r in rows:
            if r["scope"] != scope or r["scope_id"] != scope_id or level_of(r) != level:
                continue
            if level == 0 and norm(r["texture"]) == norm(texture): return r
            if level == 1 and r["effect"] == effect: return r
            if level == 2 and r["terrain"] == terrain: return r
            if level == 3: return r
        return None

    def parent_of(r):
        places = [(r["scope"], r["scope_id"])]
        if r["scope"] == AREA:
            places += [(GLOBAL, 0)]  # the map isn't known for an area offline; global still is
        elif r["scope"] == MAP:
            places += [(GLOBAL, 0)]
        first = True
        for scope, scope_id in places:
            start = level_of(r) + 1 if first else 0
            first = False
            for level in range(start, 4):
                if level == 1 and not r["effect"]: continue
                if level == 2 and r["terrain"] < 0: continue
                p = find(scope, scope_id, level, r["texture"], r["effect"], r["terrain"])
                if p and p is not r:
                    return p
        return None

    def name_of(r):
        if r["texture"]:
            name = r["texture"].replace("/", "\\").rsplit("\\", 1)[-1].rsplit(".", 1)[0]
        elif r["effect"]:
            name = f"Effect{r['effect']}"
        elif r["terrain"] >= 0:
            name = TERRAIN_NAMES[r["terrain"]] if r["terrain"] < len(TERRAIN_NAMES) else f"TerrainType{r['terrain']}"
        else:
            name = "Everything"
        if r["scope"] == MAP: name += f"@Map{r['scope_id']}"
        if r["scope"] == AREA: name += f"@Area{r['scope_id']}"
        return name

    materials, selectors, covers = [], [], []
    print(f"{src}: {len(rows)} row(s)")
    for r in rows:
        p = parent_of(r)
        materials.append((r["id"], name_of(r), p["id"] if p else 0, -1.0, -1.0, 0, -1.0, -1000.0))
        selectors.append((r["id"], r["scope"], r["scope_id"], r["texture"], r["effect"], r["terrain"], 0, r["id"], 0))
        v = r["values"]  # Depth .. Flags (15 raw values)
        f = io.as_float
        covers.append((r["id"], r["id"], 0, f(v[0]), f(v[1]), f(v[2]), f(v[3]), f(v[4]), f(v[5]), f(v[6]),
                       v[7], f(v[8]), r["cover_texture"], f(v[10]), f(v[11]), f(v[12]), f(v[13]), v[14]))
        note = f"parent {p['id']} ({name_of(p)})" if p else "no parent"
        if r["scope"] == AREA: note += "  [area row: parent zones not checked offline]"
        print(f"  {r['id']:4}  {name_of(r):40} {note}")

    out_dir.mkdir(parents=True, exist_ok=True)
    for name, columns, data in (("GroundMaterial", io.GROUND_MATERIAL, materials),
                                ("GroundMaterialSelector", io.GROUND_MATERIAL_SELECTOR, selectors),
                                ("SurfaceCover", io.SURFACE_COVER, covers)):
        path = out_dir / f"{name}.cdbc"
        if path.resolve() == src.resolve():
            raise SystemExit("refusing to overwrite the input; give a different output folder")
        path.write_bytes(io.build(columns, data))
        print(f"wrote {path}")
    print("copy all three into <client>/DBFilesClient/ and press \"Reload table\" in the surface cover panel")


if __name__ == "__main__":
    main()
