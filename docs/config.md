# Config (`WarcraftXL.ini`)

Read through `wxl-seyris-tools` (needs settings 1.1.0) from `WTF\WXL\WarcraftXL.ini`, or from
`WarcraftXL.ini` in the client root if one exists there. Section `[LivingAzeroth]`. Missing keys
are written with their default and a comment on first launch.

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
| WindShelter | 1 | Roofs and overhangs shelter the ground below from wind. |
| WindLee | 1 | Walls and cliffs cast a wind shadow on their downwind side. |
| DebugWindArrows | 0 | Debug: draw the wind as arrows around the player. |

The grass distance itself is still the client's own setting (`groundEffectDist`, options or
console); the cap only decides how high it may go.
