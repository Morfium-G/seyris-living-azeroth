#!/usr/bin/env python3
"""Small WDBC ("cdbc") reader/writer shared by this module's table tools.

Classic WDBC: 20-byte header (magic, record count, field count, record size, string block size),
flat 4-byte columns, a trailing string block (string columns hold offsets into it; 0 = empty).
"""

import struct

MAGIC = b"WDBC"


def read(data: bytes):
    """Returns (field_count, rows as lists of raw uint32, string_at(offset))."""
    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", data, 0)
    if magic != MAGIC:
        raise SystemExit("not a WDBC/cdbc file")
    if record_size != fields * 4:
        raise SystemExit(f"unexpected layout: {fields} fields, {record_size}-byte records")
    base = 20 + count * record_size
    strings = data[base:base + string_size]

    def string_at(offset):
        end = strings.index(b"\x00", offset)
        return strings[offset:end].decode("utf-8")

    rows = [list(struct.unpack_from("<%dI" % fields, data, 20 + i * record_size)) for i in range(count)]
    return fields, rows, string_at


def as_float(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


def as_int(u: int) -> int:
    return struct.unpack("<i", struct.pack("<I", u))[0]


def build(columns, rows) -> bytes:
    """columns: one type per column, "u" (uint32), "i" (int32), "f" (float) or "s" (string).
    rows: tuples of Python values in that order; written sorted by the first column (ID)."""
    strings = bytearray(b"\x00")
    offsets = {"": 0}

    def string(s):
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s.encode("utf-8") + b"\x00")
        return offsets[s]

    fmt = "<" + "".join({"u": "I", "i": "i", "f": "f", "s": "I"}[c] for c in columns)
    records = bytearray()
    for r in sorted(rows, key=lambda r: r[0]):
        if len(r) != len(columns):
            raise SystemExit(f"row {r[0]}: {len(r)} values for {len(columns)} columns")
        records += struct.pack(fmt, *(string(v) if c == "s" else v for c, v in zip(columns, r)))
    header = struct.pack("<4sIIII", MAGIC, len(rows), len(columns), len(columns) * 4, len(strings))
    return header + bytes(records) + bytes(strings)


# The three ground tables' layouts (see docs/cdbc-tables.md). Append-only.
GROUND_MATERIAL = ["u", "s", "u", "f", "f", "u"]
#                  ID  Name Parent RestMoisture Stiffness Flags
GROUND_MATERIAL_SELECTOR = ["u", "u", "u", "s", "u", "i", "u", "u", "u"]
#                  ID  ScopeType ScopeID TexturePath GroundEffectID TerrainType LiquidType MaterialID Flags
SURFACE_COVER = ["u", "u", "u", "f", "f", "f", "f", "f", "f", "f", "u", "f", "s", "f", "f", "f", "f", "u"]
#  ID MaterialID CoverMaterial Depth MaxSlope SlopeFade DriftNoise EdgeBreakup Rim RelaxSeconds
#  TintColor TintStrength CoverTexture Opacity Flatten ZOffset Wetness Flags
