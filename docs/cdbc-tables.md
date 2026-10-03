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

Which ground gets a cover (snow, sand, ...), how deep, and how it looks and behaves. The cover is
a real layer on top of the terrain: units sink in and carve trenches with a rim, which fill back in
over time. Grass and small doodads are buried under it.

Looked up per terrain cell (about 4 yd) from what the cell is painted with: its dominant texture
layer's **texture**, that layer's **ground effect**, and the ground effect's **TerrainType**. Between
cells everything blends (depth, slope limits, tint, ...), so two materials meeting mix smoothly.
**The F9 surface cover panel shows these for the spot you stand on** ("here:" lines), plus the
values the table gives there, so rows can be written on the spot.

| Field | Meaning |
|---|---|
| ID | row id |
| ScopeType | 0 = Global, 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| TexturePath | the painted texture's path, e.g. as the panel shows it; **empty = any**. Case and `/` vs `\` don't matter |
| GroundEffectID | GroundEffectTexture.dbc ID (the effect painted with the texture); **0 = any** |
| TerrainType | TerrainType.dbc ID; **-1 = any** (0 is Dirt). Stock: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand, 8 Soggy, 9 Dusty Grass, 10 None, 11 Water |
| Depth | yards of cover; **0 = no cover** (switches a material off) |
| MaxSlope | degrees: steeper ground holds no cover |
| SlopeFade | degrees below MaxSlope over which the cover thins out |
| DriftNoise | depth variation (drifts and lumps), share of the depth (0.35 = ±35%) |
| EdgeBreakup | 0 = smooth, rounded edges where covered ground meets bare ground .. 1 = ragged, patchy ones |
| Rim | height of the rim pushed up beside a trench, share of the depth |
| RelaxSeconds | how long a trench takes to fill back in |
| TintColor | colour as 0xRRGGBB, used with TintStrength |
| TintStrength | 0 = the plain cover colour .. 1 = TintColor. TintColor comes from the same row |
| CoverTexture | a BLP drawn on the cover, tiled like the terrain's layers (one repeat per terrain cell), e.g. the same texture as the ground it covers. **Empty = from the next row, `-` = none.** Any number of paths per table; up to 8 different ones are drawn at once in view (the ones most present around you; the panel lists them and says when more are around). TintColor/TintStrength still apply on top |
| Opacity | **reserved, not used yet** (see-through covers: slush, goo) |
| Flatten | **reserved, not used yet** (liquid-like covers that fill hollows) |
| ZOffset | yards the cover's base sits above (+) or **below (−)** the terrain. Below the terrain only what rises above it shows. Muddy footprint outlines: Depth 0.015, ZOffset −0.02 (the top stays 0.005 yd under the grass), Rim 4 (rims rise 0.055 yd above it), DriftNoise 0. **Only exactly −1 inherits**; every other negative is a real offset |
| Wetness | 0 = dry .. 1 = soaked: darker, and the specular gets stronger (up to ×3) and tighter. The shine comes from the CoverTexture's specular mask, so a cover without a CoverTexture doesn't shine |
| Flags | 0x1 **prevent underwater**: the cover thins out over 0.3 yd below a terrain liquid's surface (rivers, lakes, sea; not WMO water). 0x2 **prevent on land**: cover only there (river and sea floors). 0x4 **use MCCV**: tinted by the terrain's vertex colours, like the terrain. 0x8 **ignore specular map**: no shine from the CoverTexture. Flags come from the row that decides Depth, except 0x8, which belongs to the CoverTexture named in the same row |

**Specular:** a CoverTexture's alpha is its specular mask, as on the terrain's layers. If a `_s.blp`
next to it exists (later expansions keep the mask there), that one is used instead. The shine uses the
terrain's own sun and specular colour, so a custom sky's sun applies too.

A float of **-1 takes that field from the next, less specific row** (ZOffset: exactly −1).
TintColor comes with TintStrength's row, Flags with Depth's row. Fields no row sets use the
defaults: MaxSlope 45, SlopeFade 15, DriftNoise 0.35, EdgeBreakup 0.5, Rim 0.3, RelaxSeconds 30,
no tint, no cover texture, ZOffset 0, Wetness 0, no flags. No row at all (or Depth 0) = no cover.

**Older files** (19 columns, before ZOffset/Wetness) don't load; the panel says so. Convert them with
`python tools/convert_surface_cover_v3.py <file>`: it writes `<file>-v3.cdbc` with both new
columns at −1, so every row behaves as before.

**The most specific row wins** (workspace rule for scoped tables): place first, then what.
Places: the terrain cell's own area (sub-zone), its parent zones, the map, global. Within each
place: TexturePath → GroundEffectID → TerrainType → everything (empty texture, GroundEffectID 0,
TerrainType -1). Values are never added or multiplied across rows. So "snow everywhere" is one
Global row with TerrainType 3; one texture that should look different gets a TexturePath row; a
zone that's different gets Area rows.

Rim and RelaxSeconds are taken from the row where the player stands (trenches are made around the
player). The panel's multipliers (Depth, Drift noise, Edge breakup, Rim, Relax time) scale the
table for testing or taste; at 1 the table applies exactly.

Shipped baseline: snow 0.35 yd everywhere it's painted (bare above ~40°), sand listed but off
(Depth 0, with a sandy tint ready). Generator: `tools/gen_default_surface_cover.py` (`--examples`
adds a deeper Icecrown, one example texture row, a thin Kalimdor sand and a muddy-footprint row). After editing, press
"Reload table" in the surface cover panel.
