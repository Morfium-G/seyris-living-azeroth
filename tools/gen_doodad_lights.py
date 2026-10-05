#!/usr/bin/env python3
"""Generate DoodadLightProperties.cdbc and DoodadLightAssignment.cdbc for wxl-seyris-living-azeroth
(see docs/cdbc-tables.md for every column, and the orchestration design
docs/r&d/immersion/point-lights-design.md).

  DoodadLightProperties   WHAT a light is: Name, Type (0 point, 1 spot), Color (0xRRGGBB), Intensity,
                          Radius (yd), Falloff (shape: 1 smooth, higher = tighter), InnerAngle/OuterAngle
                          (spot, degrees), FlickerMode (-1 default, 0 off, 1 smooth, 2 noise, 3 steps),
                          FlickerSpeed, FlickerAmount, Flags (0). -1 = the lights panel's default.
  DoodadLightAssignment   WHERE lights go: ModelPath, AttachType (0 origin, 1 attachment point ID,
                          2 bone index, 3 particle emitter index), AttachIndex, AttachName (reserved),
                          Offset (yd, model space), Direction (spot, model space), LightID (0 = none),
                          Flags (0x1 suppress the model's own light entries).

The lights panel ("doodads near you") and the light editor show model paths, bones, attachment
points and emitters, so rows can be written on the spot; the light editor can also write these
files itself.

Usage:  python gen_doodad_lights.py [out_dir]      (default out_dir: ../data)
Then copy the two files into <client>/DBFilesClient/ and press "Reload light tables from disk".
"""

import sys
from pathlib import Path

import cdbc_io as io

D = -1.0  # the panel's default
POINT, SPOT = 0, 1
ORIGIN, ATTACHMENT_POINT, BONE, EMITTER = 0, 1, 2, 3
FLICKER_DEFAULT, FLICKER_OFF, FLICKER_SMOOTH, FLICKER_NOISE, FLICKER_STEPS = -1, 0, 1, 2, 3
SUPPRESS_MODEL_LIGHTS = 0x1


def light(id, name, type=POINT, color=0xFFA050, intensity=1.4, radius=D, falloff=D, inner=20.0, outer=35.0,
          flicker_mode=FLICKER_DEFAULT, flicker_speed=D, flicker_amount=D, flags=0):
    return (id, name, type, color, intensity, radius, falloff, inner, outer, flicker_mode, flicker_speed, flicker_amount, flags)


def assign(id, model, light_id, attach=ORIGIN, index=0, offset=(0.0, 0.0, 0.0), direction=(0.0, 0.0, -1.0), flags=0):
    return (id, model, attach, index, "", *offset, *direction, light_id, flags)


LIGHTS = [
    # A torch flame: the stock colour (1.40/0.87/0.40 = 0xFF9F49 x 1.4), a little further, livelier.
    light(1, "Torch flame", color=0xFF9F49, intensity=1.4, radius=18.0, flicker_mode=FLICKER_SMOOTH, flicker_amount=0.2),
    # A closed lantern: warmer, steadier.
    light(2, "Lantern", color=0xFFB060, intensity=1.1, radius=12.0, flicker_mode=FLICKER_NOISE, flicker_amount=0.08),
    # A spot light pointing down (e.g. under a hanging lamp).
    light(3, "Downlight", type=SPOT, color=0xFFE0B0, intensity=1.6, radius=14.0, inner=25.0, outer=45.0, flicker_mode=FLICKER_OFF),
]
ASSIGNMENTS = [
    # generaltorch01: its own light replaced by "Torch flame" at the flame (particle emitter 0, bone 8).
    assign(1, r"world\generic\passivedoodads\lights\generaltorch01.m2", 1, attach=EMITTER, index=0, flags=SUPPRESS_MODEL_LIGHTS),
]


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "data"
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, columns, rows in (("DoodadLightProperties", io.DOODAD_LIGHT_PROPERTIES, LIGHTS),
                                ("DoodadLightAssignment", io.DOODAD_LIGHT_ASSIGNMENT, ASSIGNMENTS)):
        path = out_dir / f"{name}.cdbc"
        path.write_bytes(io.build(columns, rows))
        print(f"wrote {path} ({len(rows)} row(s))")
    print("copy them into <client>/DBFilesClient/")


if __name__ == "__main__":
    main()
