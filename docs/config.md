# Config (`WarcraftXL.ini`)

Read through `wxl-seyris-tools` (needs settings 1.1.0) from `WTF\WXL\WarcraftXL.ini`, or from
`WarcraftXL.ini` in the client root if one exists there. Section `[LivingAzeroth]`, and the
sub-sections `[LivingAzeroth.Fog]` and `[LivingAzeroth.FogWake]` (an ini has no real nesting; the
dotted names keep them together). Missing keys are written with their default and a comment on
first launch.

The ini sets what each launch starts with. The debug panels (F9) still change everything at
runtime, without writing back. Without `wxl-seyris-tools` the defaults below apply and the log
says why.

| Key | Default | Meaning |
|---|---|---|
| InstancedGrass | 1 | Draw grass with the instanced renderer (much faster at long grass distances). 0 = the client's own path. |
| InstancedGrassBuildBudgetMs | 3 | Milliseconds per frame spent preparing grass coming into view; the rest is drawn the client's way until a later frame. |
| InstancedGrassMemoryMB | 256 | Most memory (MB) the instanced grass may use. It shares the client's 32-bit address space; too high can crash the client. At the limit, grass not seen for about a second is dropped. |
| GrassDensityMaxMultiplier | 4 | Instanced grass only: your limit on grass density (plants drawn per placed plant, 1 = never denser, up to 8). How dense grass is comes from `GroundEffectDoodadDensity.cdbc`; this only caps it, like a graphics setting. |
| GrassDensityMaxRadius | 150 | Your limit on how far around you grass is made denser, in yards. |
| GrassDensityMaxSpread | 3 | Your limit on how far an extra plant may move from its plant, in yards. |
| GrassDistanceCap | 10000 | Highest grass distance (`groundEffectDist`) the client accepts. The exe's own cap is 140. Only raised, never lowered: if the exe or another module already allows at least this much, it's left alone. |
| AntiAliasing | 1 | Our own anti-aliasing (FXAA), drawn before the UI. It only runs with the client's multisampling off, which readable depth (for upcoming effects like fog) needs anyway. |
| AntiAliasingSubpixel | 0.75 | FXAA subpixel smoothing, 0 (sharpest) .. 1 (softest). |
| GrassMotion | 1 | Grass sways in the wind and parts around characters. |
| GrassSway | 0.35 | Grass tip movement in yards at full wind. |
| GrassFlutter | 0.08 | Extra per-blade shimmer in yards. |
| GrassFlutterSpeed | 3 | Shimmer speed in radians per second. |
| GrassStiffBase | 0.15 | Bottom fraction of each blade that never moves (0..0.9). |
| GrassPushStrength | 0.5 | How far grass leans away from characters, in yards. |
| GrassPushRadiusScale | 2 | Push radius as a multiple of a character's bounding radius. |
| GrassMinPushRadius | 0.5 | Smallest push radius in yards. |
| GrassMountedRadiusScale | 2 | Extra push radius factor while mounted. |
| SurfaceCover | 1 | Snow (and other) cover on the ground that characters sink into and carve trenches in. Where and how comes from `SurfaceCover.cdbc`. |
| SurfaceCoverLevels | 5 | How far the cover reaches: 1..5 rings of 40, 80, 160, 320, 640 yards. |
| SurfaceCoverTrenches | 1 | Characters and creatures carve trenches in the cover. |
| SurfaceCoverDepth | 1 | Your multiplier on the cover depth from `SurfaceCover.cdbc` (1 = as the table says). |
| SurfaceCoverFillBudgetMs | 2 | Milliseconds per frame spent filling the cover around you (on login, teleports, moving). Lower = smoother, slower to appear. |
| TerrainWetness | 1 | The terrain itself looks darker where the ground is wet (rain, water nearby). The client's own terrain shaders are patched for it when they're created. |
| TerrainWetnessStrength | 0.45 | How much darker soaked ground gets, 0..1: the terrain and the surface cover alike. |
| WindShelter | 1 | Roofs and overhangs shelter the ground below from wind. |
| WindLee | 1 | Walls and cliffs cast a wind shadow on their downwind side. |
| DebugWindArrows | 0 | Debug: draw the wind as arrows around the player. |

## `[LivingAzeroth.Fog]`: the ground fog renderer

Where fog forms, and each kind's density, thickness, colour and so on, come from
`FogSettings.cdbc` ([cdbc-tables.md](cdbc-tables.md)); these are your limits and quality.

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | Draw the ground fog. |
| DensityScale | 1 | Your multiplier on every fog kind's density (1 = as `FogSettings.cdbc` says). |
| MaxDistance | 250 | How far the fog is drawn, in yards. |
| FarFog | 1 | Beyond 128 yards the fog continues at each zone's typical level (no wall where the near fog ends). |
| HalfResolution | 1 | Draw the fog at half resolution, upscaled by depth (sharp at silhouettes): about a quarter of the GPU time. |
| StepBudget | 48 | Steps per pixel through the fog (quality, not the look): more = smoother far away, slower. |
| StepLength | 0.25 | Step length inside the fog near you, in yards (grows with distance). Smaller = finer, slower. |
| Dither | 0 | 0 = smooth (may show faint bands) .. 1 = grainy. |
| FloorReach | 2 | Light fog starts on the lowest ground this many 2-yard cells around (0..4): higher keeps it off walls, thins it on slopes. |
| Patchiness | 0.6 | How patchy the fog is, 0 (smooth layers) .. 1. |
| PatchSize | 64 | Size of the patch pattern's tile in yards (the patches are about 1/8 of it). |
| Brightness | 1 | Your multiplier on the fog's colour brightness. |
| Lights | 1 | Our lights (torches, braziers, lamps) light the fog around them. |
| LightStrength | 1 | Your multiplier on our lights' light in the fog. |
| LightRange | 128 | Lights farther than this from the camera (yards) don't light the fog. Lower = cheaper. |
| PoolSpeed | 1 | How fast heavy fog (each kind's Weight share) runs downhill into hollows: 1 = default, 0 = it stays where it formed. The light share never runs off. |
| SteepStart | 30 | Degrees: from this slope a growing share of the ground-hugging fog (mist, area fog, haze) runs off; it doesn't hang on steep ground. Water fog and steam keep their own weight. |
| SteepFull | 45 | Degrees: from this slope all of it runs off. |
| WaterWritesDepth | 1 | While fog is drawn, water writes depth so lakes are fogged at their surface (not their bed). |

## `[LivingAzeroth.FogWake]`: characters and creatures pushing the fog aside

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | Characters and creatures push the ground fog aside and leave a swirling trail. |
| PushMoving | 0.9 | How much fog a moving unit pushes out, 0..1. |
| PushStanding | 0.35 | How much fog a standing unit pushes out, 0..1. |
| FillSeconds | 3 | Seconds until a trail fills back in. |
| Radius | 2.25 | Wake radius as a multiple of the unit's radius (at least 0.6 yards). |
| Widening | 1 | How much the trail widens as it fades, times its radius. |
| Height | 1 | How high the push reaches, as a multiple of the unit's height. |
| Rim | 0.25 | Fog piled up at the wake's edge, as a share of the push. |
| Swirl | 1 | How far the fog's pattern is dragged and curled along the path. |

The grass distance itself is still the client's own setting (`groundEffectDist`, options or
console); the cap only decides how high it may go.
