#!/usr/bin/env python3
"""Generate AreaClimate.cdbc for wxl-seyris-living-azeroth (see docs/cdbc-tables.md).

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID, DayTemp, NightTemp (degC at the warmest
and coldest time of day), SeasonAmplitude (degC between midsummer and midwinter), SeasonOffset
(months the seasons are shifted; 6 = flipped), Humidity (0 dry air .. 1 saturated), Flags (0).

Lookup: Global < Map < Area (zone) < Area (sub-area); the more specific row wins field by field.
-1 inherits; the temperatures are signed, so for them -1000 inherits (-1 degC is a real value).

DEFAULT_ROWS is the shipped baseline (one Global row); EXAMPLE_ROWS show a few zones, pass
--examples to include them.

Usage:  python gen_default_climate.py [out_dir] [--examples]      (default out_dir: ../data)
"""

import sys
from pathlib import Path

import cdbc_io as io

I = -1.0          # inherit (non-negative columns)
T = -1000.0       # inherit (temperatures)
GLOBAL, MAP, AREA = 0, 1, 2


def row(id, scope, scope_id=0, day=T, night=T, amplitude=I, offset=I, humidity=I, flags=0):
    return (id, scope, scope_id, day, night, amplitude, offset, humidity, flags)


DEFAULT_ROWS = [
    row(1, GLOBAL, day=18.0, night=8.0, amplitude=12.0, offset=0.0, humidity=0.5),
]

EXAMPLE_ROWS = [
    row(100, MAP, 571, day=-2.0, night=-12.0, amplitude=10.0, humidity=0.6),   # Northrend: cold
    row(101, MAP, 530, amplitude=0.0),                                       # Outland: no seasons in the Nether
    row(102, AREA, 618, day=-6.0, night=-14.0, amplitude=6.0),               # Winterspring
    row(103, AREA, 1, day=-1.0, night=-9.0),                                 # Dun Morogh
    row(104, AREA, 440, day=38.0, night=16.0, amplitude=6.0, humidity=0.1),  # Tanaris
    row(105, AREA, 33, day=31.0, night=24.0, amplitude=2.0, humidity=0.9),   # Stranglethorn Vale
    row(106, AREA, 8, day=24.0, night=17.0, amplitude=6.0, humidity=0.95),   # Swamp of Sorrows
]


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rows = DEFAULT_ROWS + (EXAMPLE_ROWS if "--examples" in sys.argv else [])
    out_dir = Path(args[0]) if args else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "AreaClimate.cdbc"
    path.write_bytes(io.build(io.AREA_CLIMATE, rows))
    print(f"wrote {path} ({len(rows)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
