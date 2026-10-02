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

## GroundEffectDoodadDensity.cdbc — `GroundEffectDoodadDensity.xml`

How dense grass is drawn near the player (instanced grass only): each placed plant is drawn
`Multiplier` times, the extra copies made on the GPU on the plant's own terrain triangle. Full
density within half of `Radius`, fading to normal at `Radius`. The player's limits
(`GrassDensityMax*` in `config.md`) cap the result.

| Field | Meaning |
|---|---|
| ID | row id |
| ScopeType | 0 = Global, 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| DoodadID | GroundEffectDoodad.dbc ID; 0 = every doodad |
| GroundEffectID | GroundEffectTexture.dbc ID (what the map designer painted); 0 = any |
| Multiplier | plants drawn per placed plant (1 = no copies) |
| Radius | yards around the player |
| Spread | most yards a copy moves from its plant (copies never leave its terrain triangle) |

A float of **-1 takes that field from the next, less specific row**. No row at all = no copies.

**The most specific row wins** (workspace rule for scoped tables): place first, then what.
Places: the chunk's own area (sub-zone), its parent zones, the map, global. Within each place:
DoodadID + GroundEffectID → DoodadID → GroundEffectID → every doodad (0). Values are never
added or multiplied across rows. So an area-wide row (DoodadID 0) beats a global rule for one
doodad; to keep one doodad different inside such an area, give it its own Area row.

Area and ground effect come from the chunk each grass layer belongs to, not from where the
player stands, so a forest can be thinner even while you're outside it.

Generator: `tools/gen_default_density.py` (shipped baseline; `--examples` adds example rows).

## SurfaceCover.cdbc — `SurfaceCover.xml`

Which ground gets a cover (snow, sand, ...) and how deep. The cover is a real layer on top of the
terrain: units sink in and carve trenches with a rim, which relax over time. Looked up per terrain
cell (about 4 yd) from what the cell is painted with: its dominant texture layer's ground effect and
that effect's TerrainType. Edges between cells blend, and the slope and drift-noise settings shape
the rest.

| Field | Meaning |
|---|---|
| ID | row id |
| ScopeType | 0 = Global, 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| GroundEffectID | GroundEffectTexture.dbc ID (what the map designer painted); 0 = any |
| TerrainType | TerrainType.dbc ID; **-1 = any** (0 is Dirt). Stock: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand, 8 Soggy, 9 Dusty Grass, 10 None, 11 Water |
| Depth | yards of cover; **0 = no cover** (switches a material off) |
| Rim | height of the rim pushed up beside a trench, as a share of the depth |
| RelaxSeconds | how long a trench takes to fill back in |
| Flags | reserved, 0 |

A float of **-1 takes that field from the next, less specific row**. No row at all = no cover.

**The most specific row wins** (workspace rule for scoped tables): place first, then what.
Places: the terrain cell's own area (sub-zone), its parent zones, the map, global. Within each
place: GroundEffectID → TerrainType → everything (GroundEffectID 0 and TerrainType -1). Values are
never added or multiplied across rows.

Rim and RelaxSeconds are taken from the row where the player stands (trenches are made around the
player); -1 all the way down falls back to the panel's defaults. The panel's "Depth scale" multiplies
every depth.

Shipped baseline: snow 0.35 yd everywhere, sand off (Depth 0). Generator:
`tools/gen_default_surface_cover.py` (`--examples` adds a deeper Icecrown and a thin Kalimdor sand
row). After editing, press "Reload table" in the surface cover panel.
