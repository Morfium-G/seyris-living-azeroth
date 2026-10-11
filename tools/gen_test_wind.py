#!/usr/bin/env python3
"""Generate a test WindProfile.cdbc for the first wind draft of wxl-seyris-living-azeroth.

Columns: ID, ScopeType (0 global / 1 map / 2 area), ScopeID, GroundMin, GroundMax, AloftMin,
AloftMax, AloftHeight, WeatherInfluence, GustStrength, GustFrequency, TerrainWeight,
LockedDirection, LockStrength, Flags, Comment (free text for the editor; never read by the module).
A negative float in a Map/Area row inherits from the scope above. Strengths are 0..1;
LockedDirection is the compass bearing the wind blows TOWARD (0 = north, 90 = east).

Test rows, all around Orgrimmar so each behaviour can be checked on foot:
    Global          baseline everywhere
    Map 36          Deadmines: no wind at all
    Area 1637       Orgrimmar: sheltered city, weak and calm
    Area 14         Durotar: windy and very gusty
    Area 17         The Barrens: direction locked to east

Usage:  python gen_test_wind.py [out_dir]      (default: ../data)
Then copy WindProfile.cdbc into <client>/DBFilesClient/ and press "Reload" in the wind panel.
"""

import sys
from pathlib import Path

import cdbc_io as io

I = -1.0  # inherit

# (id, scopeType, scopeId, gMin, gMax, aMin, aMax, aloftH, weatherInf, gustStr, gustFreq, terrainW, lockDir, lockStr, flags, comment)
ROWS = [
    (1, 0, 0,    0.10, 0.45, 0.30, 0.80, 40.0, 0.8, 0.6, 1.0, 1.0, 0.0, 0.0, 0, "Global baseline"),
    (2, 1, 36,   0.0,  0.0,  0.0,  0.0,  I,    I,   I,   I,   I,   I,   I,   0, "Deadmines: no wind"),
    (3, 2, 1637, 0.05, 0.20, I,    I,    I,    I,   0.3, I,   I,   I,   I,   0, "Orgrimmar: sheltered, calm"),
    (4, 2, 14,   0.30, 0.80, I,    I,    I,    I,   1.0, I,   I,   I,   I,   0, "Durotar: windy, gusty"),
    (5, 2, 17,   I,    I,    I,    I,    I,    I,   I,   I,   I,   90.0, 1.0, 0, "The Barrens: locked to east"),
]


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "WindProfile.cdbc"
    path.write_bytes(io.build(io.WIND_PROFILE, ROWS))
    print(f"wrote {path} ({len(ROWS)} row(s))")
    print("copy it into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
