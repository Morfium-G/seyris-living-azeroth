#!/usr/bin/env python3
"""Generate FogSettings.cdbc for wxl-seyris-living-azeroth's ground fog (see docs/cdbc-tables.md).

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID (Map or AreaTable ID, 0 for global),
Kind (0 all kinds, 1 mist, 2 water fog, 3 steam, 4 area fog, 5 haze), then Strength, Density,
Thickness, BuildUp, BurnOff, WindThin, WindDrift, Patchiness, Weight, ColorMode (0 stock, 1 hue,
2 replace; -1 inherit), ColorR, ColorG, ColorB, Saturation, Brightness, Flags (0).
-1 = take this field from the next, less specific row.

Resolution (orchestration docs/principles.md, "scoped override tables"): place first -- the area,
its parent zones, the map, global -- then within each place the kind's own row before Kind 0.
Fields no row sets keep the module's built-in values for the kind.

DEFAULT_ROWS is the shipped baseline (none: the built-in values). EXAMPLE_ROWS show colour rows for
two foggy zones (stock IDs); pass --examples to include them. Find values by eye with the fog
panel's "colour test" first.

Usage:  python gen_default_fog_settings.py [out_dir] [--examples]      (default out_dir: ../data)
Then copy FogSettings.cdbc into <client>/DBFilesClient/ and press "Reload FogSettings" in the fog panel.
"""

import sys
from pathlib import Path

import cdbc_io as io

I = -1.0          # inherit
GLOBAL, MAP, AREA = 0, 1, 2
ALL, MIST, WATER_FOG, STEAM, AREA_FOG, HAZE = 0, 1, 2, 3, 4, 5
STOCK, HUE, REPLACE = 0, 1, 2


def row(id, scope, scope_id=0, kind=ALL, strength=I, density=I, thickness=I, build_up=I, burn_off=I,
        wind_thin=I, wind_drift=I, patchiness=I, weight=I, color_mode=-1, color=(I, I, I),
        saturation=I, brightness=I, flags=0):
    return (id, scope, scope_id, kind, strength, density, thickness, build_up, burn_off, wind_thin,
            wind_drift, patchiness, weight, color_mode, color[0], color[1], color[2], saturation,
            brightness, flags)


DEFAULT_ROWS = []

EXAMPLE_ROWS = [
    # Swamp of Sorrows: its stock fog is a strong orange; keep its brightness, a murky green-grey.
    row(100, AREA, 8, color_mode=HUE, color=(0.55, 0.60, 0.50), saturation=0.5, strength=1.5),
    # Duskwood: cold blue-grey.
    row(101, AREA, 10, color_mode=HUE, color=(0.60, 0.65, 0.75), saturation=0.6),
]


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rows = DEFAULT_ROWS + (EXAMPLE_ROWS if "--examples" in sys.argv else [])
    out_dir = Path(args[0]) if args else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "FogSettings.cdbc"
    path.write_bytes(io.build(io.FOG_SETTINGS, rows))
    print(f"wrote {path} ({len(rows)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
