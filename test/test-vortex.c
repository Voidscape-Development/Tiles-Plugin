/*
Tiles - tiling scene transitions for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Checks for the vortex transition.
 *
 * The noise, phase and colour math below mirrors
 * data/effects/vortex_transition.effect one for one, and the clamps and curves
 * mirror src/vortex-transition.c.
 * It exists so the claims the effect rests on can be checked without a GPU:
 *
 *   1. the transition starts on an untouched outgoing scene and finishes on an
 *      untouched incoming one, for every pixel and every setting
 *   2. the portal is opaque edge to edge for the whole hold, so the scene swap
 *      in the middle of it can never show
 *   3. the angular noise wraps, so the spiral and the portal rim have no seam
 *      where the circle closes
 *   4. the reach covers the far corner, so an offset centre does not leave a
 *      corner of the outgoing scene showing when the scenes swap
 *   5. the bounds the shader skips work on are sound: the portal boundary never
 *      leaves the band the radius test assumes, and the reveal really is
 *      finished below its low bound and untouched above its high one, so the
 *      pixels those tests throw away had nothing to contribute
 *   6. the core ramp still lands on the values the look was tuned at when both
 *      of its sliders are left at the default
 *   7. the body palette returns a colour the user picked - exactly the first one
 *      when the mix is off - and never invents one in between
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define TAU 6.2831853f
#define FIBRES 96.0f
#define PHASE_GAP 0.05f

/* mirrors of the palette constants in the effect */
#define MIX_GAIN 1.7f
#define BAND_EDGE 0.12f
#define BODY_COLORS 4

/* mirrors of the core ramp constants in src/vortex-transition.c */
#define CORE_LEVEL 1.1f
#define CORE_WIDTH 0.9f
#define CORE_LEVEL_RANGE 3.6f
#define CORE_WIDTH_RANGE 4.0f

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *fmt, ...)
{
	va_list args;

	checks++;
	if (ok)
		return;

	failures++;
	fputs("FAIL: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* mirror of the effect                                               */
/* ------------------------------------------------------------------ */

static inline float fracf(float v)
{
	return v - floorf(v);
}

static inline float saturatef(float v)
{
	if (v < 0.0f)
		return 0.0f;
	if (v > 1.0f)
		return 1.0f;
	return v;
}

static inline float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static inline float smoothstepf(float lo, float hi, float v)
{
	float t = saturatef((v - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

static float hash1(float px, float py)
{
	float qx = fracf(px * 0.1031f + 7.13f);
	float qy = fracf(py * 0.1030f + 7.13f);
	float qz = fracf(px * 0.0973f + 7.13f);
	float d = qx * (qy + 33.33f) + qy * (qz + 33.33f) + qz * (qx + 33.33f);

	qx += d;
	qy += d;
	qz += d;

	return fracf((qx + qy) * qz);
}

static float vnoise_p(float px, float py, float per)
{
	float ix = floorf(px);
	float iy = floorf(py);
	float fx = px - ix;
	float fy = py - iy;
	float x0 = ix - per * floorf(ix / per);
	float x1 = ix + 1.0f - per * floorf((ix + 1.0f) / per);
	float a, b, c, d;

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);

	a = hash1(x0, iy);
	b = hash1(x1, iy);
	c = hash1(x0, iy + 1.0f);
	d = hash1(x1, iy + 1.0f);

	return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}

static float fbm_p(float px, float py, float per)
{
	float v = 0.5f * vnoise_p(px, py, per);
	int i;

	for (i = 0; i < 3; i++) {
		static const float amp[3] = {0.25f, 0.125f, 0.0625f};

		px *= 2.0f;
		py *= 2.0f;
		per *= 2.0f;
		v += amp[i] * vnoise_p(px, py, per);
	}

	return v / 0.9375f;
}

static float vnoise(float px, float py)
{
	float ix = floorf(px);
	float iy = floorf(py);
	float fx = px - ix;
	float fy = py - iy;
	float a, b, c, d;

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);

	a = hash1(ix, iy);
	b = hash1(ix + 1.0f, iy);
	c = hash1(ix, iy + 1.0f);
	d = hash1(ix + 1.0f, iy + 1.0f);

	return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}

static float fbm(float px, float py)
{
	float v = 0.5f * vnoise(px, py);
	int i;

	for (i = 0; i < 3; i++) {
		static const float amp[3] = {0.25f, 0.125f, 0.0625f};

		px *= 2.0f;
		py *= 2.0f;
		v += amp[i] * vnoise(px, py);
	}

	return v / 0.9375f;
}

static float iris_edge(float ang)
{
	float lobes = fbm_p(ang * FIBRES / TAU, 3.7f, FIBRES);
	float chatter = fbm_p(ang * FIBRES * 4.0f / TAU, 9.1f, FIBRES * 4.0f);

	return saturatef(lobes * 0.62f + chatter * 0.38f);
}

static float iris_radius(float ang, float k, float reach, float fringe)
{
	return k * reach * (1.0f + fringe) - k * fringe * reach * iris_edge(ang);
}

static float reveal_amount(float px, float py, float r, float k, float reach, float scale, float soft)
{
	float qx = px * scale;
	float qy = py * scale;
	float wx = fbm(qx + 11.3f, qy + 11.3f) - 0.5f;
	float wy = fbm(qx - 7.1f, qy - 7.1f) - 0.5f;
	float n = fbm(qx + wx * 1.8f, qy + wy * 1.8f);
	float field = saturatef(n * 0.55f + saturatef(r / reach) * 0.45f);

	if (soft < 0.02f)
		soft = 0.02f;

	return saturatef((k * (1.0f + 2.0f * soft) - soft - field) / soft);
}

/*
 * body_colour(): three lerps that each start where the one before it ran out,
 * so a whole t lands on that palette entry and a fractional one blends its two
 * neighbours. With a single colour t is pinned at 0 and this is the first
 * colour untouched.
 */
static void palette_colour(float t, const float pal[BODY_COLORS][3], float out[3])
{
	int i;

	for (i = 0; i < 3; i++) {
		float c = lerpf(pal[0][i], pal[1][i], saturatef(t));

		c = lerpf(c, pal[2][i], saturatef(t - 1.0f));
		c = lerpf(c, pal[3][i], saturatef(t - 2.0f));
		out[i] = c;
	}
}

/* the widening the arms and radial mixes put the raw selector through */
static float mix_gain(float m)
{
	return saturatef((m - 0.5f) * MIX_GAIN + 0.5f);
}

/* the snap the banded mix applies to the palette position */
static float band_snap(float t)
{
	float base = floorf(t);

	return base + smoothstepf(1.0f - BAND_EDGE, 1.0f, t - base);
}

/* ------------------------------------------------------------------ */
/* mirror of vortex-transition.c                                      */
/* ------------------------------------------------------------------ */

/* Core Bleed and Core Blend, geometric about the values the look was tuned at */
static float core_level_for(float bleed)
{
	return CORE_LEVEL * powf(CORE_LEVEL_RANGE, 1.0f - 2.0f * bleed);
}

static float core_width_for(float bleed, float blend)
{
	float width = core_level_for(bleed) * (CORE_WIDTH / CORE_LEVEL) * powf(CORE_WIDTH_RANGE, 2.0f * blend - 1.0f);

	return fmaxf(width, 0.0001f);
}

/* the light envelope, which the host now evaluates once a frame */
static float glow_for(float progress, float open_end)
{
	float rise = saturatef(progress / fmaxf(open_end, 0.0001f));
	float fall = 1.0f - saturatef((progress - open_end) / fmaxf(1.0f - open_end, 0.0001f));

	return rise * rise * (0.06f + (1.0f - 0.06f) * powf(fall, 1.8f));
}

static float reach_for(float ox, float oy, float cx, float cy, float unit_px)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float max_radius = 0.0f;
	int i;

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - ox;
		float dy = corner_y[i] - oy;
		float radius = sqrtf(dx * dx + dy * dy);

		if (radius > max_radius)
			max_radius = radius;
	}

	return fmaxf(max_radius / unit_px, 0.0001f);
}

static void clamp_phases(float *open_end, float *reveal_start)
{
	float lo = 0.02f;
	float hi = 1.0f - 2.0f * PHASE_GAP;

	*open_end = fminf(fmaxf(*open_end, lo), hi);
	*reveal_start = fminf(fmaxf(*reveal_start, *open_end + PHASE_GAP), 1.0f - PHASE_GAP);
}

/* ------------------------------------------------------------------ */
/* checks                                                             */
/* ------------------------------------------------------------------ */

static const float roughness[] = {0.0f, 0.15f, 0.35f, 0.6f, 1.0f};
static const float reaches[] = {1.0f, 1.803f, 2.5f, 4.0f};

/*
 * 1a. Nothing is covered at progress 0. The fringe is subtracted from an
 * oversized radius, so a rim spike must never push the boundary out past the
 * centre before the portal has started to open.
 */
static void check_start_is_clean(void)
{
	size_t ri, fi;
	int a;

	for (ri = 0; ri < sizeof(reaches) / sizeof(*reaches); ri++) {
		for (fi = 0; fi < sizeof(roughness) / sizeof(*roughness); fi++) {
			float worst = -1e30f;

			for (a = 0; a < 2048; a++) {
				float ang = -3.14159265f + 2.0f * 3.14159265f * (float)a / 2048.0f;
				float radius = iris_radius(ang, 0.0f, reaches[ri], roughness[fi]);

				if (radius > worst)
					worst = radius;
			}

			check(worst <= 0.0f, "portal already open at progress 0: reach %g roughness %g radius %g",
			      (double)reaches[ri], (double)roughness[fi], (double)worst);
		}
	}
}

/*
 * 1b. Everything is covered by the time the portal has finished opening, which
 * is what lets the scenes be swapped without the cut showing. The check is run
 * against the reach, which is measured to the far corner, so it also covers an
 * offset centre.
 */
static void check_portal_closes(void)
{
	size_t ri, fi;
	int a;

	for (ri = 0; ri < sizeof(reaches) / sizeof(*reaches); ri++) {
		for (fi = 0; fi < sizeof(roughness) / sizeof(*roughness); fi++) {
			float worst = 1e30f;

			for (a = 0; a < 2048; a++) {
				float ang = -3.14159265f + 2.0f * 3.14159265f * (float)a / 2048.0f;
				float radius = iris_radius(ang, 1.0f, reaches[ri], roughness[fi]);

				if (radius < worst)
					worst = radius;
			}

			check(worst >= reaches[ri], "portal leaves a gap when open: reach %g roughness %g radius %g",
			      (double)reaches[ri], (double)roughness[fi], (double)worst);
		}
	}
}

/*
 * 2. The reveal runs from nothing to everything. At k = 0 the incoming scene is
 * entirely hidden, so the hold stays opaque right up to the moment the reveal
 * begins; at k = 1 none of the outgoing scene is left anywhere.
 */
static void check_reveal_ends(void)
{
	static const float softs[] = {0.05f, 0.3f, 0.8f};
	static const float scales[] = {0.5f, 2.5f, 8.0f};
	size_t si, ci;
	int x, y;

	for (si = 0; si < sizeof(softs) / sizeof(*softs); si++) {
		for (ci = 0; ci < sizeof(scales) / sizeof(*scales); ci++) {
			float lo_worst = 0.0f;
			float hi_worst = 1.0f;

			for (y = 0; y < 96; y++) {
				for (x = 0; x < 96; x++) {
					float px = -2.0f + 4.0f * (float)x / 95.0f;
					float py = -2.0f + 4.0f * (float)y / 95.0f;
					float r = sqrtf(px * px + py * py);
					float lo = reveal_amount(px, py, r, 0.0f, 2.5f, scales[ci], softs[si]);
					float hi = reveal_amount(px, py, r, 1.0f, 2.5f, scales[ci], softs[si]);

					if (lo > lo_worst)
						lo_worst = lo;
					if (hi < hi_worst)
						hi_worst = hi;
				}
			}

			check(lo_worst == 0.0f, "reveal shows through before it starts: scale %g soft %g got %g",
			      (double)scales[ci], (double)softs[si], (double)lo_worst);
			check(hi_worst == 1.0f, "reveal never finishes: scale %g soft %g got %g", (double)scales[ci],
			      (double)softs[si], (double)hi_worst);
		}
	}
}

/*
 * 3. The angular noise wraps. Both the spiral arms and the portal rim sample it
 * on a coordinate that comes from an angle, so a lattice that did not close at
 * the period would draw a hard seam along the +x axis.
 */
static void check_noise_wraps(void)
{
	static const float periods[] = {1.0f, 3.0f, 5.0f, 16.0f, FIBRES};
	size_t pi;
	int i;

	for (pi = 0; pi < sizeof(periods) / sizeof(*periods); pi++) {
		float per = periods[pi];
		float worst = 0.0f;

		for (i = 0; i < 512; i++) {
			float x = per * (float)i / 512.0f;
			float y = 3.7f;
			float d = fabsf(fbm_p(x, y, per) - fbm_p(x + per, y, per));

			if (d > worst)
				worst = d;
		}

		check(worst < 1e-4f, "angular noise does not wrap at period %g: worst gap %g", (double)per,
		      (double)worst);
	}
}

/*
 * 4. The phase clamps keep the hold non-empty however the two sliders are set,
 * including when the user drags the reveal below the portal. The scene swap
 * sits in the middle of the hold, so an empty hold would put the cut on a frame
 * where the vortex was not yet opaque.
 */
static void check_phase_schedule(void)
{
	int i, j;

	for (i = 0; i <= 20; i++) {
		for (j = 0; j <= 20; j++) {
			float open_end = (float)i / 20.0f;
			float reveal_start = (float)j / 20.0f;
			float cut;

			clamp_phases(&open_end, &reveal_start);
			cut = (open_end + reveal_start) * 0.5f;

			check(reveal_start - open_end >= PHASE_GAP - 1e-6f, "hold collapsed: open %g reveal %g",
			      (double)open_end, (double)reveal_start);
			check(cut > open_end && cut < reveal_start, "scene swap outside the hold: cut %g in (%g, %g)",
			      (double)cut, (double)open_end, (double)reveal_start);
			check(open_end > 0.0f && reveal_start < 1.0f,
			      "phases reach the ends of the transition: open %g reveal %g", (double)open_end,
			      (double)reveal_start);
		}
	}
}

/*
 * 5. The reach covers the far corner from wherever the centre has been pushed,
 * including well outside the frame, so the portal never finishes opening while
 * a corner of the outgoing scene is still visible.
 */
static void check_reach_covers_canvas(void)
{
	static const float offsets[] = {-0.5f, 0.0f, 0.25f, 0.5f, 1.0f, 1.5f};
	const float cx = 1920.0f;
	const float cy = 1080.0f;
	const float unit_px = cy * 0.5f;
	size_t ox, oy;
	int x, y;

	for (oy = 0; oy < sizeof(offsets) / sizeof(*offsets); oy++) {
		for (ox = 0; ox < sizeof(offsets) / sizeof(*offsets); ox++) {
			float origin_x = offsets[ox] * cx;
			float origin_y = offsets[oy] * cy;
			float reach = reach_for(origin_x, origin_y, cx, cy, unit_px);
			float worst = 0.0f;

			for (y = 0; y <= 32; y++) {
				for (x = 0; x <= 32; x++) {
					float sx = cx * (float)x / 32.0f;
					float sy = cy * (float)y / 32.0f;
					float dx = (sx - origin_x) / unit_px;
					float dy = (sy - origin_y) / unit_px;
					float r = sqrtf(dx * dx + dy * dy);

					if (r > worst)
						worst = r;
				}
			}

			check(worst <= reach + 1e-4f, "canvas reaches past the portal: origin %g,%g r %g reach %g",
			      (double)offsets[ox], (double)offsets[oy], (double)worst, (double)reach);
		}
	}
}

/*
 * 6. The portal boundary stays inside the band the shader's radius test
 * assumes. iris_edge() is a saturate(), so the noise can only ever eat into an
 * oversized radius, and the boundary is therefore between k * reach and
 * k * reach * (1 + fringe) at every angle. PSVortex() throws away everything
 * past the upper bound without evaluating the rim noise at all, which is only
 * sound while this holds.
 */
static void check_iris_bounds(void)
{
	static const float ks[] = {0.05f, 0.25f, 0.5f, 0.8f, 1.0f};
	size_t ri, fi, ki;
	int a;

	for (ri = 0; ri < sizeof(reaches) / sizeof(*reaches); ri++) {
		for (fi = 0; fi < sizeof(roughness) / sizeof(*roughness); fi++) {
			for (ki = 0; ki < sizeof(ks) / sizeof(*ks); ki++) {
				float k = ks[ki];
				float lo = k * reaches[ri];
				float hi = lo * (1.0f + roughness[fi]);
				float under = 1e30f;
				float over = -1e30f;

				for (a = 0; a < 2048; a++) {
					float ang = -3.14159265f + 2.0f * 3.14159265f * (float)a / 2048.0f;
					float radius = iris_radius(ang, k, reaches[ri], roughness[fi]);

					if (radius < under)
						under = radius;
					if (radius > over)
						over = radius;
				}

				check(under >= lo - 1e-4f,
				      "portal boundary below its bound: k %g reach %g "
				      "roughness %g got %g want >= %g",
				      (double)k, (double)reaches[ri], (double)roughness[fi], (double)under, (double)lo);
				check(over <= hi + 1e-4f,
				      "portal boundary past its bound: k %g reach %g "
				      "roughness %g got %g want <= %g",
				      (double)k, (double)reaches[ri], (double)roughness[fi], (double)over, (double)hi);
			}
		}
	}
}

/*
 * 7. The two radius tests the reveal skips work on are sound. The noise term is
 * bounded to 0..1, so the field lies between 0.45 * rr and 0.55 + 0.45 * rr;
 * below the low bound the pixel is fully back and above the high one it has not
 * started, and in both cases the smoke does not need evaluating. Both are
 * checked against the real noise, and both have to fire on some pixels or the
 * check would be passing on an empty set.
 */
static void check_reveal_bounds(void)
{
	static const float softs[] = {0.05f, 0.3f, 0.8f};
	static const float scales[] = {0.5f, 2.5f, 8.0f};
	static const float ks[] = {0.0f, 0.15f, 0.35f, 0.5f, 0.7f, 0.9f, 1.0f};
	const float reach = 2.5f;
	long spent_hits = 0;
	long held_hits = 0;
	size_t si, ci, ki;
	int x, y;

	for (si = 0; si < sizeof(softs) / sizeof(*softs); si++) {
		for (ci = 0; ci < sizeof(scales) / sizeof(*scales); ci++) {
			for (ki = 0; ki < sizeof(ks) / sizeof(*ks); ki++) {
				float soft = softs[si];
				float k = ks[ki];
				float spent = k * (1.0f + 2.0f * soft) - 2.0f * soft;

				for (y = 0; y < 64; y++) {
					for (x = 0; x < 64; x++) {
						float px = -2.5f + 5.0f * (float)x / 63.0f;
						float py = -2.5f + 5.0f * (float)y / 63.0f;
						float r = sqrtf(px * px + py * py);
						float rr = saturatef(r / reach);
						float got;

						if (0.55f + 0.45f * rr <= spent) {
							got = reveal_amount(px, py, r, k, reach, scales[ci], soft);
							spent_hits++;
							check(got == 1.0f,
							      "reveal not finished below its bound: k %g soft "
							      "%g r %g got %g",
							      (double)k, (double)soft, (double)r, (double)got);
						} else if (0.45f * rr >= spent + soft) {
							got = reveal_amount(px, py, r, k, reach, scales[ci], soft);
							held_hits++;
							check(got == 0.0f,
							      "reveal started above its bound: k %g soft %g r "
							      "%g got %g",
							      (double)k, (double)soft, (double)r, (double)got);
						}
					}
				}
			}
		}
	}

	check(spent_hits > 0, "no pixel ever reached the fully revealed bound");
	check(held_hits > 0, "no pixel ever reached the still hidden bound");
}

/*
 * 8. The light envelope, which the host now works out once a frame instead of
 * the shader working it out per pixel. Nothing may light up before the portal
 * has opened at all, the peak has to sit where the portal fills the canvas, and
 * the whole curve has to stay inside 0..1 so intensity means what it says.
 */
static void check_glow_envelope(void)
{
	static const float opens[] = {0.02f, 0.22f, 0.5f, 0.9f};
	size_t oi;
	int i;

	for (oi = 0; oi < sizeof(opens) / sizeof(*opens); oi++) {
		float open_end = opens[oi];
		float peak = glow_for(open_end, open_end);

		check(glow_for(0.0f, open_end) == 0.0f, "vortex lit at progress 0: open %g got %g", (double)open_end,
		      (double)glow_for(0.0f, open_end));

		for (i = 0; i <= 200; i++) {
			float progress = (float)i / 200.0f;
			float g = glow_for(progress, open_end);

			check(g >= 0.0f && g <= 1.0f + 1e-6f, "glow left 0..1: open %g progress %g got %g",
			      (double)open_end, (double)progress, (double)g);
			check(g <= peak + 1e-6f,
			      "glow peaks away from the open: open %g progress %g got %g "
			      "over %g",
			      (double)open_end, (double)progress, (double)g, (double)peak);
		}
	}
}

/*
 * 9. The core ramp. Both sliders are geometric about the values the effect was
 * originally tuned at, so the middle of each has to reproduce them exactly or
 * every existing scene collection would come back looking different. More bleed
 * has to mean a lower threshold, and the width can never reach zero, which the
 * shader divides by.
 */
static void check_core_ramp(void)
{
	float previous = 1e30f;
	int i, j;

	check(fabsf(core_level_for(0.5f) - CORE_LEVEL) < 1e-5f, "core level default moved: got %g want %g",
	      (double)core_level_for(0.5f), (double)CORE_LEVEL);
	check(fabsf(core_width_for(0.5f, 0.5f) - CORE_WIDTH) < 1e-5f, "core width default moved: got %g want %g",
	      (double)core_width_for(0.5f, 0.5f), (double)CORE_WIDTH);

	for (i = 0; i <= 100; i++) {
		float bleed = (float)i / 100.0f;
		float level = core_level_for(bleed);

		check(level > 0.0f, "core level not positive: bleed %g got %g", (double)bleed, (double)level);
		check(level < previous, "core level not falling with bleed: bleed %g got %g after %g", (double)bleed,
		      (double)level, (double)previous);
		previous = level;

		for (j = 0; j <= 10; j++) {
			float blend = (float)j / 10.0f;
			float width = core_width_for(bleed, blend);

			check(width >= 0.0001f, "core width collapsed: bleed %g blend %g got %g", (double)bleed,
			      (double)blend, (double)width);
		}
	}
}

/*
 * 10. The body palette. A single colour has to come back bit for bit unchanged,
 * because that is the whole of the previous behaviour; a whole palette position
 * has to be the colour at that slot rather than something near it; and anything
 * in between has to stay inside the two entries it sits between, so a mix can
 * never invent a colour the user did not pick.
 */
static void check_palette(void)
{
	static const float pal[BODY_COLORS][3] = {
		{0.48f, 0.25f, 0.82f},
		{0.25f, 0.71f, 0.82f},
		{0.82f, 0.25f, 0.62f},
		{0.25f, 0.35f, 0.82f},
	};
	int i, j, c;

	for (i = 0; i < 3; i++) {
		float out[3];

		palette_colour(0.0f, pal, out);
		check(out[i] == pal[0][i], "single colour body drifted: channel %d got %g want %g", i, (double)out[i],
		      (double)pal[0][i]);
	}

	for (i = 0; i < BODY_COLORS; i++) {
		float out[3];

		palette_colour((float)i, pal, out);
		for (c = 0; c < 3; c++)
			check(out[c] == pal[i][c], "palette entry %d is not itself: channel %d got %g want %g", i, c,
			      (double)out[c], (double)pal[i][c]);
	}

	for (i = 0; i + 1 < BODY_COLORS; i++) {
		for (j = 0; j <= 32; j++) {
			float f = (float)j / 32.0f;
			float out[3];

			palette_colour((float)i + f, pal, out);

			for (c = 0; c < 3; c++) {
				float lo = fminf(pal[i][c], pal[i + 1][c]);
				float hi = fmaxf(pal[i][c], pal[i + 1][c]);

				check(out[c] >= lo - 1e-6f && out[c] <= hi + 1e-6f,
				      "palette left its segment: t %g channel %d got %g not in [%g, %g]",
				      (double)((float)i + f), c, (double)out[c], (double)lo, (double)hi);
			}
		}
	}
}

/*
 * 11. The mix selectors. The gain has to stay inside 0..1 so it cannot index
 * past the palette, and the banded snap has to land exactly on whole entries,
 * stay inside the span, and never run backwards - a snap that overshot would
 * put a colour the band does not belong to along its edge.
 */
static void check_mix_selectors(void)
{
	int i;

	for (i = 0; i <= 200; i++) {
		float m = -0.5f + 2.0f * (float)i / 200.0f;
		float g = mix_gain(m);

		check(g >= 0.0f && g <= 1.0f, "mix selector left 0..1: m %g got %g", (double)m, (double)g);
	}

	for (i = 0; i <= 3; i++)
		check(band_snap((float)i) == (float)i, "banded mix does not land on entry %d: got %g", i,
		      (double)band_snap((float)i));

	for (i = 0; i <= 600; i++) {
		float span = 3.0f;
		float t = span * (float)i / 600.0f;
		float snapped = band_snap(t);
		float previous = band_snap(span * (float)(i > 0 ? i - 1 : 0) / 600.0f);

		check(snapped >= 0.0f && snapped <= span, "banded mix left the palette: t %g got %g", (double)t,
		      (double)snapped);
		check(snapped >= previous - 1e-6f, "banded mix ran backwards: t %g got %g after %g", (double)t,
		      (double)snapped, (double)previous);
	}
}

int main(void)
{
	check_start_is_clean();
	check_portal_closes();
	check_reveal_ends();
	check_noise_wraps();
	check_phase_schedule();
	check_reach_covers_canvas();
	check_iris_bounds();
	check_reveal_bounds();
	check_glow_envelope();
	check_core_ramp();
	check_palette();
	check_mix_selectors();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
