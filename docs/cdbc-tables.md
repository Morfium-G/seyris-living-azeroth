# Custom tables (cdbc)

Plain WDBC files in the client's `DBFilesClient\` folder. Editor definitions (WDBC-editor XML) are
next to this file.

## WindProfile.cdbc — `WindProfile.xml`

Wind per scope. Lookup: Global < Map < Area (zone) < Area (sub-area); the more specific row wins
field by field. **A negative value inherits** from the scope above. No Global row = no wind.

| Field | Meaning |
|---|---|
| ScopeType | 0 = Global (one row), 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| GroundMin / GroundMax | strength range (0..1) at ground level |
| AloftMin / AloftMax | strength range at and above AloftHeight |
| AloftHeight | yards above ground where wind is fully unobstructed |
| WeatherInfluence | 0..1, how far bad weather pushes strength toward the max |
| GustStrength | 0..1, gust amplitude on top of the steady wind |
| GustFrequency | relative, 1 = default |
| TerrainWeight | 0..1, reserved for terrain shaping |
| LockedDirection | compass degrees the wind blows **toward** (0 north, 90 east) |
| LockStrength | 0..1, pull toward LockedDirection (0 = no lock) |
| Flags | reserved, 0 |

Test data generator: `tools/gen_test_wind.py`.

## GroundEffectDoodadWind.cdbc — `GroundEffectDoodadWind.xml`

Per-doodad grass overrides. **A row replaces the automatic values for that doodad.** Written by
the in-game "Save overrides" button (keeps a `.bak`).

| Field | Meaning |
|---|---|
| ID | GroundEffectDoodad.dbc ID |
| Flags | 0x1 no wind, 0x2 no push, 0x10 flip root/tip (0x4 droop when wet, 0x8 water current: reserved) |
| Stiffness | 0 flexible .. 1 rigid (scales wind and push) |
| RootV / TipV | manual bend mapping in texture V; -1 = automatic from the model geometry |
