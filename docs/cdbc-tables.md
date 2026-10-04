# Custom tables (cdbc)

Plain WDBC files in the client's `DBFilesClient\` folder. Editor definitions (WDBC-editor XML) are
next to this file.

**Rules for every table here:**
- Columns are only ever **appended**. A file from before a column existed still loads: the missing
  column counts as "not set" (inherit), so adding a column never needs a converter. To get the new
  columns into a file for editing: `python tools/upgrade_tables.py <DBFilesClient folder>` (fills
  them with "not set", keeps a timestamped `.bak`). Layout changes that aren't appends ship with a
  converter in `tools/`.
- **−1 inherits** (takes the value from the next row up, as each table describes). **Signed columns**
  (temperatures, offsets) inherit at **−1000** and below instead, since −1 °C is a real value.

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

## Ground materials: three tables

What the ground is made of is decided once and shared by every feature that cares (the cover
now; moisture, temperature, footsteps and texture swaps later). Three roles:

1. **GroundMaterialSelector.cdbc**: *where* a material lies.
2. **GroundMaterial.cdbc**: *what* a material is (properties only).
3. **Feature tables keyed by material**: SurfaceCover.cdbc today. A new feature adds a new table
   and leaves the others alone.

Looked up per terrain cell from what it's painted with: each texture layer's **texture**, that
layer's **ground effect**, and the ground effect's **TerrainType**. Layers are weighted by their
painted strength, so two materials meeting mix smoothly. **The F9 surface cover panel shows all of
this for the spot you stand on** ("here:" lines: texture, effect, TerrainType, area, material and
its values), so rows can be written on the spot.

Generator for all three: `tools/gen_default_ground_materials.py` (`--examples` adds an Icecrown
snow child material, wet Westfall sand, mud with muddy footprint outlines, and a "no snow on
Kalimdor" selector). After editing, press "Reload table" in the surface cover panel: it reloads
all three.

### GroundMaterialSelector.cdbc — `GroundMaterialSelector.xml`

| Field | Meaning |
|---|---|
| ID | row id |
| ScopeType | 0 = Global, 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| TexturePath | the painted texture's path, e.g. as the panel shows it; **empty = any**. Case and `/` vs `\` don't matter |
| GroundEffectID | GroundEffectTexture.dbc ID (the effect painted with the texture); **0 = any** |
| TerrainType | TerrainType.dbc ID; **-1 = any** (0 is Dirt). Stock: 0 Dirt, 1 Metallic, 2 Stone, 3 Snow, 4 Wood, 5 Grass, 6 Leaves, 7 Sand, 8 Soggy, 9 Dusty Grass, 10 None, 11 Water |
| LiquidType | **0 for terrain rows.** A LiquidType.dbc ID (as baked into ADTs and WMOs) makes this a **liquid row**: that liquid is this material. Only ScopeType/ScopeID and MaterialID count on liquid rows (place: area chain → map → global, the most specific wins) |
| MaterialID | the GroundMaterial; **0 = no material here** (switches a broader row off, e.g. no snow in one zone) |
| Flags | reserved, 0 |

**The most specific row wins** (workspace rule for scoped tables): place first, then what. Places:
the terrain cell's own area (sub-zone), its parent zones, the map, global. Within each place:
TexturePath → GroundEffectID → TerrainType → everything (empty texture, GroundEffectID 0,
TerrainType -1). So "snow everywhere" is one Global row with TerrainType 3; one texture that
should be a different material gets a TexturePath row; a zone that's different gets Area rows.

### GroundMaterial.cdbc — `GroundMaterial.xml`

| Field | Meaning |
|---|---|
| ID | material id (what selectors and feature tables refer to) |
| Name | for authoring and the panel |
| Parent | material whose values a −1 field takes (0 = none). Feature tables follow the same chain |
| RestMoisture | 0 = bone dry .. 1 = soaked: **what the texture already shows** (wet-painted sand ≈ 0.8). Moisture rests here; later, visuals react to the difference from it. Not used yet |
| Stiffness | resistance to deformation: **0 = gives way completely .. 1 = rigid** (the same meaning as GroundEffectDoodadWind's Stiffness). For a cover: feet press away (1 − Stiffness) of its depth, so mud at 0.4 keeps a floor in every print |
| Flags | reserved, 0 |
| Absorbency | 0..1, how much outside moisture reaches it: **rain** on open ground and **water nearby**. Rock and ice ≈ 0.1 (rain runs off), sand and soil ≈ 0.9. Appended: older files without it still load |
| Temperature | °C of the material itself; **−1000 = none** (signed). A heat source: **liquid materials** (magma) and **terrain materials** (e.g. a painted lava-stream texture, where it's painted at least 50%) warm (or cool) the ground toward it, by 5% of the difference at the source, fading out by 12 yd and 4 yd above it. Liquid and ground heat don't add up; the stronger one counts. Hot ground dries out by itself (heat drying). Appended |

A float of **−1 takes the value from the Parent** (and so on up). Defaults where no material in the
chain sets one: RestMoisture 0.3, Stiffness 0, Absorbency 0.5.

**Ground moisture** (runtime, not stored; the climate panel shows it where you stand): every
2 yd around you (±128 yd) the ground's moisture settles toward its **equilibrium**:
- the materials' RestMoisture (mixed by painted strength),
- wetter near a liquid: Absorbency × the liquid's RestMoisture × how close (full within 2 yd,
  none beyond 6) × how low (full up to 0.5 yd above the surface, none from 2 yd),
- drier in hot, dry air (from 20 °C, up to 60% below rest at 40 °C with no humidity).

It moves there over minutes: faster when warm and windy, slower in humid air, very slowly
below freezing. **Rain** wets ground that's open to the sky (no roof above), at Absorbency × the rain's
intensity. Submerged ground takes the liquid's RestMoisture.

**What a liquid does:** its liquid row's material (RestMoisture: how wet it makes the ground;
Temperature: heat), else built in by the liquid's category (LiquidType.dbc's Type column): water,
ocean and slime RestMoisture 1 and no temperature; magma RestMoisture 0 and 1000 °C, so the ground
around lava is hot and dry. The climate panel names the nearest liquid, its category and what it does. Nothing shows it on the terrain itself yet (that needs the terrain
shader patch); covers already look wetter where it's above rest.

### SurfaceCover.cdbc — `SurfaceCover.xml`

Which materials carry a cover (snow, sand, ...), how deep, and how it looks and behaves. The cover
is a real layer on top of the terrain: units sink in and carve trenches with a rim, which fill back
in over time. Grass and small doodads are buried under it.

| Field | Meaning |
|---|---|
| ID | row id |
| MaterialID | the GroundMaterial that carries this cover (one row per material) |
| CoverMaterial | the material of what lies on top (snow on grass is still Snow); **0 = the ground material itself**. Its Stiffness decides how far feet press the cover down |
| Depth | yards of cover; **0 = no cover** (switches a material off) |
| MaxSlope | degrees: steeper ground holds no cover |
| SlopeFade | degrees below MaxSlope over which the cover thins out |
| DriftNoise | depth variation (drifts and lumps), share of the depth (0.35 = ±35%) |
| EdgeBreakup | 0 = smooth, rounded edges where covered ground meets bare ground .. 1 = ragged, patchy ones |
| Rim | height of the rim pushed up beside a trench, share of the depth |
| RelaxSeconds | how long a trench takes to fill back in |
| TintColor | colour as 0xRRGGBB, used with TintStrength |
| TintStrength | 0 = the plain cover colour .. 1 = TintColor. TintColor comes from the same row |
| CoverTexture | a BLP drawn on the cover, tiled like the terrain's layers (one repeat per terrain cell), e.g. the same texture as the ground it covers. **Empty = from the parent material's row, `-` = none.** Any number of paths per table; up to 8 different ones are drawn at once in view (the ones most present around you; the panel lists them and says when more are around). TintColor/TintStrength still apply on top |
| Opacity | **reserved, not used yet** (see-through covers: slush, goo) |
| Flatten | **reserved, not used yet** (liquid-like covers that fill hollows) |
| ZOffset | yards the cover's base sits above (+) or **below (−)** the terrain. Below the terrain only what rises above it shows. Muddy footprint outlines: Depth 0.015, ZOffset −0.02 (the top stays 0.005 yd under the grass), Rim 4 (rims rise 0.055 yd above it), DriftNoise 0. Signed: **−1000 inherits** (exactly −1 does too, as older files wrote it) |
| Wetness | how wet the cover **looks** at rest: 0 = as its texture .. 1 = soaked: darker, and the specular gets stronger (up to ×3) and tighter. On top of it, the ground moisture above its rest (rain, water nearby) makes it look wetter still. The shine comes from the CoverTexture's specular mask, so a cover without a CoverTexture doesn't shine |
| Flags | 0x1 **prevent underwater**: the cover thins out over 0.3 yd below a terrain liquid's surface (rivers, lakes, sea; not WMO water). 0x2 **prevent on land**: cover only there (river and sea floors). 0x4 **use MCCV**: tinted by the terrain's vertex colours, like the terrain. 0x8 **ignore specular map**: no shine from the CoverTexture. Flags come from the row that decides Depth, except 0x8, which belongs to the CoverTexture named in the same row |
| MeltKeptShare | **snow covers only**: the share of the painted depth mild weather melts it down to (at 15 °C; less above 0 °C, nothing at or below). 0.6 if no row sets it. Appended |
| MeltGoneTemperature | **snow covers only**: °C at which painted snow melts away completely (from MeltKeptShare at 15 °C down to nothing here). 25 if no row sets it; set it low (e.g. 16) for "no snow in summer" in a zone or on a texture. Signed: **−1000 inherits**. Appended |

**Specular:** a CoverTexture's alpha is its specular mask, as on the terrain's layers. If a `_s.blp`
next to it exists (later expansions keep the mask there), that one is used instead. The shine uses the
terrain's own sun and specular colour, so a custom sky's sun applies too.

A float of **−1 takes that field from the parent material's row** (ZOffset: exactly −1), and so on
up the chain; a material without a row takes everything from its parents'. TintColor comes with
TintStrength's row, Flags with Depth's row. Fields no row sets use the defaults: MaxSlope 45,
SlopeFade 15, DriftNoise 0.35, EdgeBreakup 0.5, Rim 0.3, RelaxSeconds 30, no tint, no cover texture,
ZOffset 0, Wetness 0, no flags. No material, no row along its chain, or Depth 0 = no cover.

**Trench size:** like the client's own footprints. CreatureModelData's FootprintTextureLength/Width
(in inches) × the unit's scale, the mount's while mounted; the trench radius is 1.2 × the print's
longer side (a human's 0.33 yd print → 0.4 yd). Models with FootprintTextureID −1 (bats, birds and
other fliers that "walk" in the air) leave no trench.

Rim and RelaxSeconds apply per spot, like every other value (blended between materials). The panel's multipliers (Depth, Drift noise, Edge breakup, Rim, Relax time) scale the
table for testing or taste; at 1 the table applies exactly.

Shipped baseline: snow 0.35 yd everywhere it's painted (bare above ~40°), sand listed but off
(Depth 0, with a sandy tint ready).

**Snow: fallen and painted** (runtime; the cover panel's "here (snow)" lines and the climate
panel's zone list show it). A cover is **snow** when its CoverMaterial is the fallback snow material
(what GroundMaterialSelector gives for **TerrainType 3** at the place, or globally) or a child of it.
- **Fallen snow** builds up while it snows on ground below 0 °C (0.2 yd an hour at full intensity, up
  to 0.6 yd) and melts above 0 °C (~1.5 cm per °C per day; its water soaks the ground). Snow falling
  on ground above 0 °C melts on arrival and soaks it, more slowly than rain. While it isn't
  snowing, fallen snow also **sublimates** (turns to vapour, no melt water), even below 0 °C: half
  of it in ~8 h in calm air at night, down to ~1 h in strong wind, dry air and sunshine. So
  always-cold zones don't keep fallen snow forever; their painted snow stays. It's kept
  per zone (a zone you enter while it snows catches up on half an hour of snowfall) and follows that
  zone near you, less where a spot is warmer than its zone (hot ground and liquids, sunny slopes),
  none under roofs and liquids. Where a **snow cover** is painted it adds to its depth and keeps its
  look. Elsewhere it takes the look of the layer's own row **if that row's CoverMaterial is snow, even
  with Depth 0** ("no snow until it snows, then it looks like this", per area or texture), else of
  the fallback snow's row, else built-in values (plain white, slope limits 45°/15°). It never lies
  on or right beside **other** covers (sand, mud).
- **Painted snow** keeps its Depth as a baseline. Warm weather melts it down, slowly (degree-days),
  toward a cap: all of it at 0 °C or below, MeltKeptShare at 15 °C, nothing at MeltGoneTemperature.
  It opens in small holes and patches first. Heat sources (hot ground, hot liquids) melt it down to
  nothing on top and less further away, leaving pits. Colder again (night), it grows back toward its
  cap (~4.5 cm an hour, faster while it snows). Melt water wets the ground.
- The **effective temperature** of a spot is the ground's (air, hot liquids and ground nearby,
  toward a hot material's own on lava textures) plus up to 4 °C of sun by day on ground facing it.
- Testing: the cover panel's **Deposit snow** adds fallen snow to your zone; the **Everywhere** mode
  paints nothing, so fallen snow lies anywhere with the plain look. The climate panel's **Set your
  zone's soak** does the same for moisture.

**Older files:**
- 21 columns (place keys inside SurfaceCover itself): the panel says so. Split them with
  `python tools/convert_surface_cover_v4.py <SurfaceCover.cdbc>`. It writes all three tables into
  `converted-v4/` next to the input: one material, selector and cover row per old row, same IDs,
  with Parents set so −1 fields resolve exactly as before.
- 19 columns (before ZOffset/Wetness): run `convert_surface_cover_v3.py` first.

## AreaClimate.cdbc — `AreaClimate.xml`

The air's baseline per place: temperature through the day and the year, and humidity. Read by the
ground moisture (drying) now; later by snow melting and freezing, frost, fog and breath.

| Field | Meaning |
|---|---|
| ID | row id |
| ScopeType | 0 = Global, 1 = Map, 2 = Area |
| ScopeID | Map.dbc ID or AreaTable ID (ignored for Global) |
| DayTemp | °C at the warmest time of day (15:00). Signed: −1000 inherits |
| NightTemp | °C at the coldest (05:00). Signed: −1000 inherits |
| SeasonAmplitude | °C between midsummer and midwinter (Elwynn ~12, jungle ~2, Outland 0) |
| SeasonOffset | months the seasons are shifted (6 = flipped, a "southern" continent) |
| Humidity | 0 = dry air .. 1 = saturated: humid places dry slowly (later: fog, dust) |
| Flags | reserved, 0 |

Lookup: Global < Map < Area (zone) < Area (sub-area); the more specific row wins **field by field**.
No row at all: 18 °C day, 8 °C night, ±6 °C over the year, humidity 0.5.

**Temperature** = night .. day by the time of day (the sky's clock) + the season (from the server's
game date: warmest around 20 July, shifted by SeasonOffset) + the weather (rain −3 °C, snow −6 °C,
sandstorm +2 °C, × its intensity). The climate panel shows each part, and can override the weather
and the time of day for testing.

Generator: `tools/gen_default_climate.py` (one Global row; `--examples` adds Northrend, Outland,
Winterspring, Dun Morogh, Tanaris, Stranglethorn and Swamp of Sorrows).
