#include "SurfaceCoverInternal.hpp"

namespace wxl_livingazeroth::cover::detail
{
    // VS: c0..c3 view-projection columns; c4 = (patch first cell i, j, cell size, 1 / texture
    // size); c5 = (level centre cell i, j, half extent, morph band) in cells; c6 = the camera
    // position (the scene is drawn about the camera); c7 = (fade start, fade end, fade on) in cells;
    // c8 = (far lift start, end, height) in yd from the camera; c9..c11 = the view rotation rows
    // (identity when not lit like the scene); c12 = the terrain's fog parameters; c13.x = tint on.
    const char* kVsHlsl = R"(
float4 vp0 : register(c0); float4 vp1 : register(c1); float4 vp2 : register(c2); float4 vp3 : register(c3);
float4 grid : register(c4);
float4 lod : register(c5);
float4 eye : register(c6);
float4 fade : register(c7);
float4 lift : register(c8);
float4 vr0 : register(c9); float4 vr1 : register(c10); float4 vr2 : register(c11);
float4 fogp : register(c12);
float4 look : register(c13);
float4 farMap : register(c14);   // 1 / chunk size, map size, 1 / map size, on
float4 farBox : register(c15);   // the far map's box, world yd (min x, min y, max x, max y)
float4 farMisc : register(c16);  // x = the value outside that box
float4 pointInfo : register(c17);   // x = point lights in use (env/Lights)
float4 points[96] : register(c20);  // per light: (view-space position, 1 / radius), (colour, falloff shape),
                                    // (view-space spot direction, cos outer angle), (1 / (cos inner - cos outer))
sampler2D baseTex : register(s0);
sampler2D coverTex : register(s1);
sampler2D lookTex : register(s2);
sampler2D farTex : register(s3);  // ground moisture beyond the near grid, per terrain chunk (R32F)

// The far map at a world position, filtered here (vertex textures aren't).
float FarAt(float2 w)
{
    float2 t = w * farMap.x - 0.5;
    float2 f = frac(t);
    float4 uv = float4((t - f + 0.5) * farMap.z, 0, 0);
    float a = tex2Dlod(farTex, uv).r;
    float b = tex2Dlod(farTex, uv + float4(farMap.z, 0, 0, 0)).r;
    float c = tex2Dlod(farTex, uv + float4(0, farMap.z, 0, 0)).r;
    float d = tex2Dlod(farTex, uv + float4(farMap.z, farMap.z, 0, 0)).r;
    float v = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    float inside = (w.x >= farBox.x && w.y >= farBox.y && w.x <= farBox.z && w.y <= farBox.w) ? farMap.w : 0;
    return lerp(farMisc.x, v, inside);
}

struct VOut { float4 pos : POSITION; float3 n : TEXCOORD0; float2 d : TEXCOORD1; float2 rel : TEXCOORD2; float fog : TEXCOORD3; float4 tint : TEXCOORD4; float2 wuv : TEXCOORD5; float3 vpos : TEXCOORD6; float far : TEXCOORD7; float3 lamps : TEXCOORD8; };

// Point lights (torches; env/Lights) at a view-space position with a view-space unit normal:
// colour x a falloff like the client's 1 / (0.7 d + 0.03 d^2), windowed to exactly 0 at the
// light's radius. The light has a size (~1 yd, like a flame): the falloff uses sqrt(d^2 + 1), so
// right under it it tops out at the client's value at 1 yd instead of a hot spot with a hard edge
// (a torch flame ends up just above deep snow). Wrapped Lambert ((N.L + 0.5) / 1.5), so a low light
// still lights the surface around it, not just at grazing angles. Falloff shape: the window to the
// power of the light's falloff (1 = smooth, higher = tighter). Spot lights: x saturate((cos to the
// axis - cos outer) / (cos inner - cos outer)); point lights have cos outer -2 (always inside). Per
// vertex: the cover's mesh is dense (0.25 yd near the player), and pixel shaders can't index constants.
float3 Lamps(float3 vp, float3 nv)
{
    float3 sum = 0;
    [loop] for (int k = 0; k < 24; ++k)
    {
        if (k >= pointInfo.x) break;
        float4 p = points[k * 4], col = points[k * 4 + 1], spot = points[k * 4 + 2];
        float spotScale = points[k * 4 + 3].x;
        float3 dv = p.xyz - vp;
        float d2 = dot(dv, dv);
        float d = sqrt(d2);
        float3 l = dv / max(d, 0.001);
        float t = saturate(d * p.w);
        float win = 1 - t * t;
        win = pow(win * win, col.w);
        float de = sqrt(d2 + 1);
        float wrap = saturate((dot(nv, l) + 0.5) / 1.5);
        float cone = saturate((dot(-l, spot.xyz) - spot.w) * spotScale);
        sum += col.rgb * win * wrap * cone / (de * (0.7 + 0.03 * de));
    }
    return sum;
}

float4 Tint(float2 idx) { return look.x > 0.5 ? tex2Dlod(lookTex, float4((idx + 0.5) * grid.w, 0, 0)) : 0; }


float Cheb(float2 idx) { float2 a = abs(idx - lod.xy); return max(a.x, a.y); }
float Fade(float2 idx) { return fade.z > 0.5 ? saturate((fade.y - Cheb(idx)) / (fade.y - fade.x)) : 1; }

// The cover texture holds the depth, negative where the cover sits below the terrain (ZOffset < 0):
// such a cover is lowered far away instead of lifted.
// x = base height, y = cover depth
float2 Sample(float2 idx)
{
    float2 uv = (idx + 0.5) * grid.w;
    return float2(tex2Dlod(baseTex, float4(uv, 0, 0)).r, abs(tex2Dlod(coverTex, float4(uv, 0, 0)).r) * Fade(idx));
}

float B(float2 idx) { return tex2Dlod(baseTex, float4((idx + 0.5) * grid.w, 0, 0)).r; }
float C(float2 idx) { return abs(tex2Dlod(coverTex, float4((idx + 0.5) * grid.w, 0, 0)).r) * Fade(idx); }
float LiftDir(float2 idx) { return tex2Dlod(coverTex, float4((idx + 0.5) * grid.w, 0, 0)).r < 0 ? -1 : 1; }
// Inside the level's filled area (its grid plus the 2-cell apron): beyond it the slots hold stale heights.
float2 Inside(float2 idx) { return clamp(idx, lod.xy - lod.z - 2, lod.xy + lod.z + 2); }

// step = neighbour distance for the normal in cells (2 = the coarser level's spacing).
// The slope has two parts. The terrain's: the terrain is flat triangles (4 per 4.17 yd cell) and
// shades itself with smooth vertex normals, so measured over 0.25 yd its slope jumps at every crease
// and the lit cover shows the triangles -- it's measured over ~2.5 yd instead (the short step near
// holes). The cover's own (drifts, trenches, rims): the fine step.
void Vertex(float2 idx, float step, out float3 p, out float3 n, out float2 s)
{
    s = Sample(idx);
    p = float3(idx * grid.z, s.x + s.y);
    float wide = max(step, floor(2.5 / grid.z));
    float2 xa = Inside(idx - float2(wide, 0)), xb = Inside(idx + float2(wide, 0));
    float2 ya = Inside(idx - float2(0, wide)), yb = Inside(idx + float2(0, wide));
    float bxa = B(xa), bxb = B(xb), bya = B(ya), byb = B(yb);
    float2 terrain;
    [branch] if (min(min(bxa, bxb), min(bya, byb)) < -10000)
        terrain = float2(B(idx - float2(step, 0)) - B(idx + float2(step, 0)), B(idx - float2(0, step)) - B(idx + float2(0, step))) / (2 * step * grid.z);
    else
        terrain = float2((bxa - bxb) / max(xb.x - xa.x, 1), (bya - byb) / max(yb.y - ya.y, 1)) / grid.z;
    float2 cover = float2(C(idx - float2(step, 0)) - C(idx + float2(step, 0)), C(idx - float2(0, step)) - C(idx + float2(0, step))) / (2 * step * grid.z);
    n = float3(terrain + cover, 1);
}

VOut main(float2 ij : POSITION)
{
    VOut o;
    float2 idx = grid.xy + ij;
    float3 p, n; float2 s;
    Vertex(idx, 1, p, n, s);
    o.tint = Tint(idx);
    o.wuv = (idx + 0.5) * grid.w;
    float dir = LiftDir(idx);

    // Border band: odd vertices slide onto their even neighbours, so the edge becomes the coarser
    // level's triangles.
    float m = lod.w > 1 ? saturate((Cheb(idx) - (lod.z - lod.w)) / (lod.w - 1)) : 0;
    [branch] if (m > 0)
    {
        float2 odd = frac(idx * 0.5) * 2;
        float3 p1, n1; float2 s1;
        Vertex(idx - odd, 2, p1, n1, s1);
        // Fully morphed takes the coarser vertex exactly: lerp(p, p1, 1) = p + (p1 - p) rounds off p1
        // at world-sized coordinates, and the two levels' edges no longer meet (hairline cracks).
        float4 t1 = Tint(idx - odd);
        float2 w1 = (idx - odd + 0.5) * grid.w;
        if (m >= 1) { p = p1; n = n1; s = s1; o.tint = t1; o.wuv = w1; dir = LiftDir(idx - odd); }
        else        { p = lerp(p, p1, m); n = lerp(normalize(n), normalize(n1), m); s = lerp(s, s1, m); o.tint = lerp(o.tint, t1, m); o.wuv = lerp(o.wuv, w1, m); }
    }

    o.far = FarAt(p.xy);

    // Far away the client draws simpler terrain than its heights, which pokes through a thin
    // cover; a lift that only depends on the distance stays seamless across the levels. A cover
    // below the terrain (mud whose rims show through) is lowered instead, so the simpler far terrain
    // hides it rather than the lift pushing it into view.
    float dist = length(float3(p.xy - eye.xy, p.z - eye.z));
    p.z += dir * lift.z * saturate((dist - lift.x) / (lift.y - lift.x)) * saturate(s.y * 10);

    // Normal and position into view space, where the terrain shader lights and fogs (its sun
    // direction is view space; its fog runs on the view depth).
    float3 nw = normalize(n);
    o.n = nw.x * vr0.xyz + nw.y * vr1.xyz + nw.z * vr2.xyz;
    o.rel = p.xy - eye.xy;
    float4 q = float4(o.rel, p.z - eye.z, 1);
    o.pos = float4(dot(q, vp0), dot(q, vp1), dot(q, vp2), dot(q, vp3));
    o.vpos = q.x * vr0.xyz + q.y * vr1.xyz + q.z * vr2.xyz;
    float viewZ = o.vpos.z;
    o.fog = min(pow(max(viewZ * fogp.x + fogp.y, 0), fogp.z), 1);
    o.lamps = pointInfo.x > 0 ? Lamps(o.vpos, normalize(o.n)) : 0;
    o.d = float2(s.y, s.x < -10000 ? 1 : 0);
    return o;
}
)";

    // PS: c0 = light direction, c1 = colour, c2.x = smallest drawn depth, c3 = the next finer
    // level's box (camera-relative min x, min y, max x, max y): this level isn't drawn there;
    // c4 = ambient, c5 = diffuse, c6 = fog colour; c8/c9 = per cover texture slot 1 when its
    // alpha is a specular mask; c10 = specular colour (rgb) and exponent (w). The tint (rgb, a =
    // strength) comes from the VS, wetness and vertex colour from the props texture.
    // Lit the way the terrain shader lights: min(ambient + diffuse * saturate(N.L), 1), plus
    // Blinn-Phong specular in view space (terrain VS: pow(N.normalize(L + normalize(-pos)), c27.w)
    // x c27.rgb, times the layer texture's alpha in its PS), then fogged towards the fog colour.
    const char* kPsHlsl = R"(
float4 light : register(c0);
float4 albedo : register(c1);
float4 opts : register(c2);
float4 inner : register(c3);
float4 ambient : register(c4);
float4 diffuse : register(c5);
float4 fogColor : register(c6);
float4 texMap : register(c7);   // 1 / tile size (yd), uv offset (x, y): uv = rel * x + yz
float4 specOn0 : register(c8);
float4 specOn1 : register(c9);
float4 specular : register(c10);
float4 wetMap : register(c11);  // 1 / grid extent (yd), uv offset (x, y), on
float4 wetBox : register(c12);  // the grid's valid box, camera-relative (min x, min y, max x, max y)
float4 debugView : register(c14); // x = 0 off, 1 cover texture share, 2 specular only, 3 lighting only
float4 misc : register(c13);     // x = 1: the terrain's dynamic shadows apply; y = (unused), z = 1 / the near grid's edge fade (yd),
                                 // w = how much darker soaked ground gets (the wet terrain's strength, so both match)
float4 pcf[8] : register(c20);   // the terrain's PCF offsets (its PS c3..c10)
float4 shadowM[12] : register(c30); // the terrain's shadow matrices (its VS c37..c48), from view space
sampler2D cover0 : register(s0);
sampler2D cover1 : register(s1);
sampler2D cover2 : register(s2);
sampler2D cover3 : register(s3);
sampler2D cover4 : register(s4);
sampler2D cover5 : register(s5);
sampler2D cover6 : register(s6);
sampler2D cover7 : register(s7);
sampler2D weights0 : register(s8); // slots 1..4 (r..a)
sampler2D weights1 : register(s9); // slots 5..8
sampler2D props : register(s10);   // a = wetness, rgb = vertex colour (0.5 = neutral)
sampler2D wetGrid : register(s11); // a = ground moisture above the material's rest (env/Fields)
sampler2D shadow0 : register(s12); // the terrain's shadow maps (its s5..s8): three cascades, then far
sampler2D shadow1 : register(s13);
sampler2D shadow2 : register(s14);
sampler2D shadow3 : register(s15);

float Tap(sampler2D s, float2 uv, float z) { return tex2Dlod(s, float4(uv, z, 0)).x; }

// 1 = lit, 0 = in shadow. The same cascades, tests and PCF taps as the terrain's Terrain3 shader
// (dumped 2026-10-02): 9 taps in the nearest cascade, 5 in the next two, then the far map fading out.
float Shadow(float3 vp)
{
    float4 p = float4(vp, 1);
    float4 a = float4(dot(p, shadowM[0]), dot(p, shadowM[1]), dot(p, shadowM[2]), dot(p, shadowM[9]));
    float4 b = float4(dot(p, shadowM[3]), dot(p, shadowM[4]), dot(p, shadowM[5]), dot(p, shadowM[10]));
    float4 c = float4(dot(p, shadowM[6]), dot(p, shadowM[7]), dot(p, shadowM[8]), dot(p, shadowM[11]));
    [branch] if (saturate(max(abs(a.x), abs(a.y)) * -3.4482758 + 3.41379309) > 0)
    {
        float2 uv = a.xy * 0.5 + 0.5;
        float s = Tap(shadow0, uv, a.z);
        [unroll] for (int k = 0; k < 8; ++k) s += Tap(shadow0, uv + pcf[k].xy, a.z);
        return s / 9;
    }
    [branch] if (max(abs(b.x), abs(b.y)) < 1)
    {
        float2 uv = b.xy * 0.5 + 0.5;
        float s = Tap(shadow1, uv, b.z);
        [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow1, uv + pcf[k].xy, b.z);
        return s * 0.2;
    }
    [branch] if (max(abs(c.x), abs(c.y)) < 1)
    {
        float2 uv = c.xy * 0.5 + 0.5;
        float s = Tap(shadow2, uv, c.z);
        [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow2, uv + pcf[k].xy, c.z);
        return s * 0.2;
    }
    float2 uv = float2(a.w, b.w) * 0.5 + 0.5;
    float s = Tap(shadow3, uv, c.w);
    [unroll] for (int k = 0; k < 8; k += 2) s += Tap(shadow3, uv + pcf[k].xy, c.w);
    float edge = saturate(max(abs(a.w), abs(b.w)) * -11.1111107 + 11);
    return edge * (s * 0.2 - 1) + 1;
}

float4 main(float3 n : TEXCOORD0, float2 d : TEXCOORD1, float2 rel : TEXCOORD2, float fog : TEXCOORD3, float4 tint : TEXCOORD4, float2 wuv : TEXCOORD5, float3 vpos : TEXCOORD6, float far : TEXCOORD7, float3 lamps : TEXCOORD8) : COLOR
{
    clip(d.x - opts.x);
    clip(0.5 - d.y);
    float2 a = step(inner.xy, rel) * step(rel, inner.zw);
    clip(0.5 - a.x * a.y);
    float3 nn = normalize(n), l = normalize(light.xyz);
    float ndl = saturate(dot(nn, l));
    float3 lit = min(ambient.rgb + diffuse.rgb * ndl, 1);
    // Cover textures, tiled in world space like the terrain's layers, mixed by their share here
    // (two filtered weight textures, slots 1..8); the plain cover colour fills the rest. Their
    // alpha is the specular mask (as on the terrain's layers, or the _s texture's).
    float2 uv = rel * texMap.x + texMap.yz;
    float4 w = tex2D(weights0, wuv), v = tex2D(weights1, wuv);
    float4 t0 = tex2D(cover0, uv), t1 = tex2D(cover1, uv), t2 = tex2D(cover2, uv), t3 = tex2D(cover3, uv);
    float4 t4 = tex2D(cover4, uv), t5 = tex2D(cover5, uv), t6 = tex2D(cover6, uv), t7 = tex2D(cover7, uv);
    float3 textured = t0.rgb * w.r + t1.rgb * w.g + t2.rgb * w.b + t3.rgb * w.a + t4.rgb * v.r + t5.rgb * v.g + t6.rgb * v.b + t7.rgb * v.a;
    float mask = dot(float4(t0.a, t1.a, t2.a, t3.a) * w, specOn0) + dot(float4(t4.a, t5.a, t6.a, t7.a) * v, specOn1);
    float share = saturate(dot(w, 1) + dot(v, 1));
    float3 base = albedo.rgb * (1 - share) + textured * albedo.w;
    float3 colour = lerp(base, tint.rgb, saturate(tint.a));
    // Vertex colour (the terrain multiplies by 2 x it) and wetness: darker, and a stronger, tighter shine.
    float4 pr = tex2D(props, wuv);
    // The row's own wet look, plus how much wetter than its rest the ground is now (rain, water nearby).
    // Inside its last yards the grid fades into the one regional value used beyond it.
    float2 fromMin = rel - wetBox.xy, toMax = wetBox.zw - rel;
    float inner = saturate(min(min(fromMin.x, fromMin.y), min(toMax.x, toMax.y)) * misc.z);
    float wet = saturate(pr.a + wetMap.w * lerp(far, tex2D(wetGrid, rel * wetMap.x + wetMap.yz).a, inner));
    colour *= pr.rgb * 2 * (1 - misc.w * wet);
    float3 h = normalize(l + normalize(-vpos));
    float sp = pow(saturate(dot(nn, h)), max(specular.w, 1) * (1 + 3 * wet)) * saturate(mask * (1 + 2 * wet));
    // Dynamic shadows like the terrain's: down to 70% in full shadow, no shine there.
    float shade = misc.x > 0.5 ? Shadow(vpos) : 1;
    // Point lights (torches, from the VS): not shadowed by the sun's shadow maps. Added to the sun's
    // light like the client's own (the same amount whichever way the ground faces the sun), then
    // compressed smoothly above a knee instead of clamped (a hard clamp left a flat white plateau with
    // a ring), on the brightest channel with all three scaled alike so the flame's hue stays. The knee
    // starts at the sun's own level (at least 0.8): without lamps this is the sun's lighting as before.
    float3 sunLit = lit * (0.7 + 0.3 * shade);
    float3 total = sunLit + lamps;
    float peak = max(total.r, max(total.g, total.b));
    float knee = max(0.8, max(sunLit.r, max(sunLit.g, sunLit.b)));
    float over = max(peak - knee, 0);
    float room = max(1 - knee, 0.001);
    float shaped = peak - over + room * (1 - exp(-over / room));
    total *= shaped / max(peak, 0.001);
    float3 c = colour * total + specular.rgb * sp * shade;
    // Debug views (panel): which part makes a colour.
    [branch] if (debugView.x > 0.5)
    {
        if (debugView.x < 1.5) return float4(1 - share, share, 0, 1);       // cover texture share: red = plain colour
        if (debugView.x < 2.5) return float4(specular.rgb * sp * shade, 1); // the shine alone
        return float4(total * 0.5, 1);                                     // the light alone (half, so bright stays readable)
    }
    return float4(lerp(fogColor.rgb, c, fog), 1);
}
)";
}
