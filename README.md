# wxl-seyris-living-azeroth

Purely cosmetic, client-side features for WoW 3.3.5 that make the world feel more alive: wind and
weather that everything reacts to, snow/sand/mud with footprints, puddles, wetness, better water,
fog, clouds and lighting, and characters that plant their feet on the ground.

Features share one environment state (wind, temperature, precipitation), so each one makes the
others better. Every feature reads the stock DBC/ADT settings it extends first; custom cdbc tables
(through `wxl-seyris-tools`) only add what stock data can't express. Each feature can be toggled
on its own.

Planning and research: `orchestration/docs/r&d/immersion/` (`README.md` is the index).

## Status

Skeleton only. First planned spike: a readable scene depth buffer (INTZ), shown as a greyscale
image in a debug panel. See `orchestration/docs/r&d/immersion/rendering-foundation.md`.
