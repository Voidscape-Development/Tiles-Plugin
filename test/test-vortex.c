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
 * The noise and phase math below mirrors data/effects/vortex_transition.effect
 * one for one, and the clamps mirror vortex_update() in src/vortex-transition.c.
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
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define TAU 6.2831853f
#define FIBRES 96.0f
#define PHASE_GAP 0.05f

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

/* ------------------------------------------------------------------ */
/* mirror of vortex-transition.c                                      */
/* ------------------------------------------------------------------ */

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

int main(void)
{
	check_start_is_clean();
	check_portal_closes();
	check_reveal_ends();
	check_noise_wraps();
	check_phase_schedule();
	check_reach_covers_canvas();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
