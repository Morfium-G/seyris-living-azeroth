// Fallen snow and painted snow's melting: the rules (orchestration docs/r&d/immersion/
// regional-layer-and-snow.md, "Fallen snow" and "Melt model"). Shared by the zone records
// (env/Regional: open, flat, typical ground) and the near grid (env/Fields: each 2 yd spot), so both
// run the same numbers.
//
//  - Fallen snow builds up while it snows on ground below 0 degC and melts by degree-days above it.
//  - Painted snow (SurfaceCover rows whose CoverMaterial is snow) keeps its painted depth as a
//    baseline: warm weather melts it down to a cap (MeltCap), local heat down to nothing on top, and
//    it grows back toward the cap when it's colder again. Game-y on purpose: variety over a session,
//    not a simulation that ends with every snowfield gone.
#pragma once

namespace wxl_livingazeroth::snow
{
    // Snowfall at full intensity on open ground below 0 degC, in yd per second. Faster than real
    // heavy snowfall (~5 cm an hour), so a session shows it.
    constexpr float kSnowfallPerSecond = 0.2f / 3600.0f;
    constexpr float kMaxFallen = 0.6f; // yd: fallen snow stops building up here

    // Degree-day melting: ~1.5 cm of snow per degC above 0 per day (3-5 mm of water).
    constexpr float kMeltPerDegreeSecond = 0.0164f / 86400.0f;

    // Painted snow grows back toward its cap when that's above what's left: ~4.5 cm an hour, five
    // times that while it snows.
    constexpr float kRegrowPerSecond = 0.05f / 3600.0f;
    constexpr float kRegrowSnowingBoost = 4.0f;

    // Local heat (hot liquids, hot ground; later campfires) melts on top of the weather, by its reach
    // (0..1): a spot at full reach loses ~0.35 yd in ~20 s.
    constexpr float kHeatMeltPerSecond = 0.02f;
    constexpr float kHotSource = 30.0f; // degC: sources at least this warm melt snow around them

    // Melted snow becomes ground moisture: snow is ~0.1 water, and a yd of melt soaks open ground as
    // 1 - exp(-melt x this), so 5 cm of melted snow soaks it about two thirds. Above
    // kEvaporateTemperature the water evaporates instead (hot ground; steam will attach there).
    constexpr float kMeltToSoak = 20.0f;
    constexpr float kEvaporateTemperature = 40.0f;

    // Painted snow's melt cap where no row sets SurfaceCover's MeltKeptShare / MeltGoneTemperature.
    constexpr float kDefaultKeptShare = 0.6f, kDefaultGoneTemperature = 25.0f;
    constexpr float kMildEnd = 15.0f; // degC: mild weather melts down to the kept share by here

    // A spot warmer than its zone (heat nearby, a sunny slope, a warmer sub-area) holds less of the
    // zone's fallen snow: all of it at the zone's warmth, none this many degC above it.
    constexpr float kLocalMeltRange = 6.0f;

    // The painted depth a zone record assumes (its painted share is for typical ground).
    constexpr float kTypicalPaintedDepth = 0.35f;

    // Sublimation: fallen snow also turns straight into vapour while it isn't snowing, even below
    // 0 degC (no melt water), so always-cold zones don't keep it forever. Half of it goes in this many
    // seconds in calm, average-humidity air at night; wind, dry air and sunshine speed it up (strong
    // wind, dry air and full sun: ~1 hour).
    constexpr float kSublimationHalfLife = 8.0f * 3600.0f;

    /// Fallen snow's sublimation rate (share per second) at a steady wind (0..1), air humidity
    /// (0..1) and daylight (0..1).
    inline float SublimationRate(float wind, float humidity, float daylight)
    {
        const float speed = (1.0f + 2.0f * wind) * (1.3f - 0.6f * humidity) * (1.0f + daylight);
        return 0.6931472f / kSublimationHalfLife * speed;
    }

    /// The share of painted snow left in the long run at an effective temperature t (degC): all of
    /// it at or below 0, down to `kept` at 15 degC, then down to nothing at `gone`.
    inline float MeltCap(float t, float kept, float gone)
    {
        if (t <= 0.0f) return 1.0f;
        kept = kept < 0.0f ? 0.0f : (kept > 1.0f ? 1.0f : kept);
        if (t >= gone) return 0.0f;
        const float mildEnd = gone < kMildEnd ? gone : kMildEnd;
        if (t < mildEnd) return 1.0f + (kept - 1.0f) * t / kMildEnd;
        const float atMildEnd = 1.0f + (kept - 1.0f) * mildEnd / kMildEnd;
        return atMildEnd * (gone - t) / (gone - mildEnd);
    }

    /// One step of painted snow's share `keep` toward `cap`: melting by degree-days and local heat
    /// while above it, growing back while below it. `depth` = the painted depth (yd), `snowing` =
    /// the snowfall's intensity on it (0..1). Returns the yd of snow that melted.
    inline float StepKeep(float& keep, float cap, float t, float heatReach, float snowing, float depth, float dt)
    {
        if (depth <= 0.0f) { keep = 1.0f; return 0.0f; }
        if (keep > cap)
        {
            const float rate = (kMeltPerDegreeSecond * (t > 0.0f ? t : 0.0f) + kHeatMeltPerSecond * heatReach) / depth;
            float next = keep - rate * dt;
            next = next < cap ? cap : next;
            const float melted = (keep - next) * depth;
            keep = next;
            return melted;
        }
        const float rate = kRegrowPerSecond * (1.0f + kRegrowSnowingBoost * snowing) / depth;
        keep = keep + rate * dt > cap ? cap : keep + rate * dt;
        return 0.0f;
    }
}
