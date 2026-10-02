# wxl-seyris-living-azeroth

A [WarcraftXL](https://github.com/WarcraftXL/wxl-core) v1.1 extension for the WoW 3.3.5a client
(build 12340) that makes the world feel more alive. Everything is client-side and purely cosmetic:
a wind model that the world reacts to, grass that moves with it and parts around characters, and
a grass renderer fast enough for long grass distances and higher density.

Features read the stock data they extend first (weather, areas, ground effects). Custom tables
only add what stock data can't express, and every feature can be switched off.

## Features

**Wind**
- Steady wind and travelling gusts per map and area (`WindProfile.cdbc`), stronger in bad
  weather.
- Shelter: roofs and overhangs keep the ground below calm.
- Lee: walls and cliffs cast a wind shadow on their downwind side.

**Grass motion**
- Grass sways with the local wind, with a little per-blade flutter.
- It parts around every nearby creature and player, sized by their own collision; mounts push
  wider.
- Each ground-effect doodad bends from its root: the root and tip are worked out from the model
  itself. Pebbles and other flat doodads stay still.
- Per-doodad overrides in `GroundEffectDoodadWind.cdbc`: wind/push off, stiffness, manual
  root/tip. They can be edited and saved from the in-game panel.

**Instanced grass renderer** (on by default)
- The client re-bakes most visible grass on the CPU every frame at long grass distances. This
  draws the same plants from static GPU buffers instead: one measurement at 1024 yards went from
  9 to 45+ fps.
- Grass looks the same as the client's own: plant positions, tilt, colour and shading match it
  exactly (a built-in check compares the two).
- A memory limit keeps it safe in the 32-bit client.
- **Density near the player:** extra plants drawn on the GPU (no extra memory), each on its own
  plant's terrain triangle so it sits exactly on the ground. Content decides how dense
  (`GroundEffectDoodadDensity.cdbc`, per area, map or globally, per doodad or ground effect). The
  player's own limits cap it, like a graphics setting.
- **Grass distance:** the client's hard limit of 140 yards for `groundEffectDist` is raised, with
  no exe patch. It's left alone if the exe or another module already allows more. The game's own
  options slider still stops at its old maximum; to go further, set the distance with the console,
  e.g. `/console groundEffectDist 1024`. The client saves it like any other setting.

**Snow cover** (test feature, off by default: switch it on in the F9 "surface cover" panel)
- A real layer of snow on top of the terrain, not just a texture: characters sink in, and they
  and nearby creatures carve trenches with a raised rim that slowly fill back in. Grass and small
  doodads are buried under it.
- Content decides where (`SurfaceCover.cdbc`, per area, map or globally, per ground effect or
  terrain material) and how deep. The shipped sample covers snowy ground and leaves sand off.
- Soft, ragged edges where snowy ground meets bare ground, bare steep slopes, gentle drifts.
- Lit and fogged like the terrain, so it follows the time of day and zone fog. Reaches 640 yards
  in rings that get coarser with distance; only the nearest 40 yards deform.

**Anti-aliasing**
- Our own FXAA pass on the world, drawn before the interface (on by default, `AntiAliasing` in the
  ini). It runs while the client's multisampling is off, which the readable scene depth needs
  anyway.

**Readable scene depth**
- The world's depth buffer made readable for later effects (fog, footprints, water edges). It's
  off while the client's multisampling is on.

## Requirements

- WoW 3.3.5a (12340) with **WarcraftXL v1.1**.
- **[wxl-seyris-tools](https://github.com/Morfium-G/wxl-seyris-tools)**: config settings and the
  custom tables. Without it the built-in defaults apply, and the log says so.
- A GPU with shader model 3.0, and `d3dcompiler_47.dll` (part of Windows 10/11).

## Installation

1. Put `wxl-seyris-living-azeroth.dll` into `<client>\Extensions\wxl-seyris-living-azeroth\`.
2. Copy the files from [`data/`](data/) into `<client>\DBFilesClient\`:
   - `WindProfile.cdbc`: wind per map/area. It needs a Global row for any wind at all; without
     the file, grass still parts around characters but doesn't sway.
   - `GroundEffectDoodadWind.cdbc`: per-doodad grass overrides (also editable and saved from the
     in-game panel).
   - `GroundEffectDoodadDensity.cdbc`: grass density near the player. Without it there are no
     extra plants.
   - `SurfaceCover.cdbc`: where snow (or sand) cover goes and how deep. Without it there is no
     cover.

   **These are sample data**, so that wind, grass and snow show up and work in some stock 3.3.5
   zones. They are in no way fine-tuned yet. The scripts in `tools/` regenerate a minimal baseline
   (`gen_default_density.py`, `gen_default_surface_cover.py`) or a small wind example
   (`gen_test_wind.py`).
3. Start the client. `Logs\wxl-core.log` lists what loaded.

## Configuration

Settings live in `WTF\WXL\WarcraftXL.ini`, section `[LivingAzeroth]`. Missing keys are written with
their defaults and a comment on first launch. All keys are listed in [docs/config.md](docs/config.md).

The in-game overlay (**F9**) has panels for wind, grass, grass performance, anti-aliasing and the
snow cover. Everything can be tuned live there; the ini sets what each launch starts with (the
snow cover has no ini keys yet: it starts off every launch).

## Custom tables

Plain WDBC files (`.cdbc` is only a naming convention for custom tables), documented in
[docs/cdbc-tables.md](docs/cdbc-tables.md). Editor definitions (WDBC editor XML) are in `docs/`.

Tables with several scopes (global, map, area) follow one rule: the most specific matching row
wins, and a `-1` field takes its value from the next, less specific row.

## Building from source

The extension builds inside the WarcraftXL core tree. Core picks up every folder under
`src/extensions/` as an extension:

1. Make this repository's `src` folder available as `<wxl-core>/src/extensions/wxl-seyris-living-azeroth`,
   e.g. with a directory junction:
   ```powershell
   New-Item -ItemType Junction -Path "<wxl-core>\src\extensions\wxl-seyris-living-azeroth" -Target "<this repo>\src"
   ```
2. Re-run CMake's configure step once (new extension folders are only found at configure time),
   then build as usual (Win32, Release). The DLL lands under `Extensions\wxl-seyris-living-azeroth\`.

## Compatibility

- **wxl-grasswind:** both patch the same grass shaders. If it's loaded, this module's grass motion
  stays off and logs why; the rest keeps working.
- **Multisampling:** fine for everything except the readable scene depth, which is off while
  multisampling is on. Changing multisampling in-game is handled.

## Planned

Weather and wetness, footprints and lasting trails in the snow, sand and mud, puddles, better
water and fog, wind on trees and bushes, a data-driven sky, foot placement on slopes.

## License and credits

GPL-3.0, see [LICENSE](LICENSE).

The approach of patching the client's own grass shaders (rather than replacing them) follows the
official [wxl-grasswind](https://github.com/WarcraftXL/wxl-grasswind) module (GPL-3.0).
