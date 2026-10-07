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
 * Checks for the matte cutout transition, linked against src/matte-math.c.
 *
 *   1. the first frame is the outgoing scene and the last the incoming one,
 *      for every pixel, every shape, both directions, any centre (on the
 *      canvas or off it), turn, spin, stretch and band layout: the range
 *      matte_field_range() finds really does bound the field, and the front
 *      is run far enough past it at both ends
 *   2. the shapes are what they say: the polygons are 1 on their own outline,
 *      the star's tips and inner corners are both on it, the heart and cross
 *      are 0 at the centre and nowhere negative
 *   3. every grown shape scales about its centre - the field at s * q is s
 *      times the field at q - which is what makes the bands concentric copies
 *      of the shape and the gauge a pixel distance
 *   4. every field is monotone along rays from the centre, which is what lets
 *      the range be found from the canvas boundary alone
 *   5. the star's fold has no seam where one point's sector meets the next
 *   6. the clock runs from 0 to 1 exactly, the pause holds, and only the
 *      overshoot ever runs past the end
 *   7. an image's silhouette becomes a sensible profile: a disc is round, a
 *      square has its corners, a hole is filled in, an empty image is refused
 *
 * The GPU check in test/matte-check.c holds the effect's field to the same
 * function this one exercises.
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matte-math.h"

#define TAU 6.2831853f

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

static const char *const shape_name[MATTE_SHAPE_COUNT] = {
	"circle", "square", "triangle", "diamond", "hexagon", "octagon", "star",   "heart", "cross",
	"wave",   "curl",   "ripple",   "clock",   "spiral",  "blinds",  "zigzag", "image", "luma",
};

static float profile[MATTE_PROFILE_SIZE];

static bool is_grown(int shape)
{
	return shape <= MATTE_SHAPE_CROSS || shape == MATTE_SHAPE_IMAGE;
}

static void shape_params(int shape, struct matte_shape_params *sp)
{
	memset(sp, 0, sizeof(*sp));
	sp->shape = shape;
	sp->star_n = 5.0f;
	sp->star_k = matte_star_slope(5.0f, 0.45f);
	sp->arms = 3.0f;
	sp->pitch = 300.0f;
	sp->amp = 50.0f;
	sp->wavelen = 220.0f;
	sp->lobes = 7.0f;
	sp->bar_px = 90.0f;
	sp->profile = profile;
}

/* ------------------------------------------------------------------ */
/* 1. both ends of the transition are exact                           */
/* ------------------------------------------------------------------ */

static void check_ends(void)
{
	const float cx = 640.0f, cy = 360.0f;
	const float origins[][2] = {{0.5f, 0.5f}, {0.1f, 0.85f}, {-0.4f, 1.3f}, {1.5f, -0.5f}, {1.0f, 0.0f}};
	const float angles[] = {0.0f, 2.4f};
	const float spins[] = {0.0f, -0.75f};
	const float stretches[] = {1.0f, 0.4f};
	const float drift = 1.7f;
	const struct matte_bands layouts[] = {
		{0.0f, 0.0f, 0, 1.0f, 0.0f},
		{12.0f, 40.0f, 3, 6.0f, 25.0f},
	};
	int shape, dir;

	for (shape = 0; shape < MATTE_SHAPE_COUNT; shape++) {
		struct matte_shape_params sp;

		if (shape == MATTE_SHAPE_LUMA) /* a texture: its range is the image's */
			continue;

		shape_params(shape, &sp);

		for (dir = 0; dir < 2; dir++)
			for (size_t o = 0; o < sizeof(origins) / sizeof(origins[0]); o++)
				for (size_t a = 0; a < 2; a++)
					for (size_t s = 0; s < 2; s++)
						for (size_t k = 0; k < 2; k++)
							for (size_t b = 0; b < 2; b++) {
								struct matte_frame f;
								const struct matte_bands *bands = &layouts[b];
								float lo, hi, front0, front1;
								float w = matte_band_width(bands);
								float pad = matte_band_pad(bands);
								float sign = matte_sign(dir);
								float worst0 = -INFINITY, worst1 = INFINITY;
								int x, y;

								f.origin_x = origins[o][0] * cx;
								f.origin_y = origins[o][1] * cy;
								f.angle0 = angles[a];
								f.angle1 = angles[a] + spins[s] * TAU;
								f.stretch_x = sqrtf(stretches[k]);
								f.stretch_y = 1.0f / f.stretch_x;

								matte_field_range(&sp, &f, cx, cy, &lo, &hi);
								front0 = matte_front(dir, lo, hi, bands, 0.0f);
								front1 = matte_front(dir, lo, hi, bands, 1.0f);

								/* every pixel centre on a coarse grid, and
								 * every one along the edges, where the
								 * extremes live */
								for (y = 0; y < (int)cy; y += 1) {
									int step = (y == 0 || y == (int)cy - 1) ? 1 : 9;

									for (x = (y * 3) % step; x < (int)cx;
									     x += step) {
										float qx, qy, d;

										matte_to_shape(&f, f.angle0, x + 0.5f,
											       y + 0.5f, &qx, &qy);
										d = sign *
										    (front0 -
										     matte_field(&sp, qx, qy, 0.0f));
										worst0 = fmaxf(worst0, d);

										matte_to_shape(&f, f.angle1, x + 0.5f,
											       y + 0.5f, &qx, &qy);
										d = sign *
										    (front1 -
										     matte_field(&sp, qx, qy, drift));
										worst1 = fminf(worst1, d);
									}

									/* the last column too */
									{
										float qx, qy;

										matte_to_shape(&f, f.angle0, cx - 0.5f,
											       y + 0.5f, &qx, &qy);
										worst0 = fmaxf(
											worst0,
											sign * (front0 -
												matte_field(&sp, qx, qy,
													    0.0f)));
										matte_to_shape(&f, f.angle1, cx - 0.5f,
											       y + 0.5f, &qx, &qy);
										worst1 = fminf(
											worst1,
											sign * (front1 -
												matte_field(&sp, qx, qy,
													    drift)));
									}
								}

								check(worst0 <= -pad + 0.01f,
								      "%s %s origin %zu angle %zu spin %zu stretch %zu bands %zu: "
								      "a pixel is %.2f past the front on the first frame",
								      shape_name[shape], dir ? "in" : "out", o, a, s, k,
								      b, worst0);
								check(worst1 >= w + pad - 0.01f,
								      "%s %s origin %zu angle %zu spin %zu stretch %zu bands %zu: "
								      "a pixel is only %.2f past the front on the last frame "
								      "(needs %.2f)",
								      shape_name[shape], dir ? "in" : "out", o, a, s, k,
								      b, worst1, w + pad);
							}
	}
}

/* ------------------------------------------------------------------ */
/* 2. the shapes are what they say                                    */
/* ------------------------------------------------------------------ */

static float field_at(int shape, float x, float y)
{
	struct matte_shape_params sp;

	shape_params(shape, &sp);
	return matte_field(&sp, x, y, 0.0f);
}

static void check_outlines(void)
{
	const float eps = 1e-4f;
	int i;

	/* inradius 1: the middle of every edge is at 1 */
	check(fabsf(field_at(MATTE_SHAPE_SQUARE, 1.0f, 0.0f) - 1.0f) < eps, "square edge");
	check(fabsf(field_at(MATTE_SHAPE_SQUARE, 1.0f, 1.0f) - 1.0f) < eps, "square corner");
	check(fabsf(field_at(MATTE_SHAPE_TRIANGLE, 0.0f, 1.0f) - 1.0f) < eps, "triangle base");
	check(fabsf(field_at(MATTE_SHAPE_TRIANGLE, 0.0f, -2.0f) - 1.0f) < eps, "triangle apex points up");
	check(fabsf(field_at(MATTE_SHAPE_HEXAGON, 1.0f, 0.0f) - 1.0f) < eps, "hexagon flat side");
	check(fabsf(field_at(MATTE_SHAPE_HEXAGON, 0.0f, 1.1547005f) - 1.0f) < eps, "hexagon point is up");
	check(fabsf(field_at(MATTE_SHAPE_OCTAGON, 1.0f, 0.0f) - 1.0f) < eps, "octagon side");
	check(fabsf(field_at(MATTE_SHAPE_OCTAGON, 0.7071068f, 0.7071068f) - 1.0f) < eps, "octagon diagonal side");

	/* the diamond is taller than it is wide by its ratio */
	check(fabsf(field_at(MATTE_SHAPE_DIAMOND, 0.0f, 1.7204651f) - 1.0f) < eps, "diamond tip");
	check(fabsf(field_at(MATTE_SHAPE_DIAMOND, 1.7204651f / MATTE_DIAMOND_TALL, 0.0f) - 1.0f) < eps,
	      "diamond side corner");

	/* star: every tip at radius 1, every inner corner at the depth */
	for (i = 0; i < 5; i++) {
		float tip = TAU * (float)i / 5.0f;
		float inner = tip + TAU / 10.0f;

		check(fabsf(field_at(MATTE_SHAPE_STAR, sinf(tip), -cosf(tip)) - 1.0f) < 1e-3f, "star tip %d", i);
		check(fabsf(field_at(MATTE_SHAPE_STAR, 0.45f * sinf(inner), -0.45f * cosf(inner)) - 1.0f) < 1e-3f,
		      "star inner corner %d", i);
	}

	/* the regular-polygon limit of the star is the polygon */
	{
		struct matte_shape_params sp;

		shape_params(MATTE_SHAPE_STAR, &sp);
		sp.star_n = 6.0f;
		sp.star_k = matte_star_slope(6.0f, cosf(TAU / 12.0f));
		for (i = 0; i < 64; i++) {
			float a = TAU * (float)i / 64.0f;
			float x = cosf(a) * 3.0f, y = sinf(a) * 3.0f;
			/* the repository's hexagon has an inradius of 1; the star's
			 * tips are at 1, so it is the hexagon with its corners there */
			float hex = fmaxf(fabsf(x), 0.5f * fabsf(x) + 0.8660254f * fabsf(y)) / 0.8660254f;

			check(fabsf(matte_field(&sp, x, y, 0.0f) - hex) < 1e-3f,
			      "a six point star at full depth is a hexagon (angle %d)", i);
		}
	}

	/* the heart and the cross have their centre inside and are nowhere
	 * negative; the heart's dip is at the top and its point at the bottom */
	check(field_at(MATTE_SHAPE_HEART, 0.0f, 0.0f) == 0.0f, "heart centre");
	check(field_at(MATTE_SHAPE_CROSS, 0.0f, 0.0f) == 0.0f, "cross centre");
	check(field_at(MATTE_SHAPE_HEART, 0.0f, -0.5f) > field_at(MATTE_SHAPE_HEART, 0.5f, -0.5f),
	      "the heart dips between its lobes");
	check(fabsf(field_at(MATTE_SHAPE_HEART, 0.0f, 1.25f) - 1.0f) < eps, "the heart's point");
	check(fabsf(field_at(MATTE_SHAPE_CROSS, 0.7071068f, 0.7071068f) - 1.0f) < eps, "cross arm tip");
	for (i = 0; i < 360; i++) {
		float a = TAU * (float)i / 360.0f;

		check(field_at(MATTE_SHAPE_HEART, cosf(a), sinf(a)) > 0.0f, "heart positive at angle %d", i);
		check(field_at(MATTE_SHAPE_CROSS, cosf(a), sinf(a)) > 0.0f, "cross positive at angle %d", i);
	}
}

/* ------------------------------------------------------------------ */
/* 3, 4, 5. scaling, rays and seams                                   */
/* ------------------------------------------------------------------ */

static void check_scaling_and_rays(void)
{
	int shape, i;

	srand(7);

	for (shape = 0; shape < MATTE_SHAPE_COUNT; shape++) {
		struct matte_shape_params sp;
		bool sweep = matte_is_sweep(shape) || shape == MATTE_SHAPE_BLINDS;

		if (shape == MATTE_SHAPE_LUMA)
			continue;

		shape_params(shape, &sp);

		for (i = 0; i < 2000; i++) {
			float a = TAU * (float)rand() / (float)RAND_MAX;
			float r = 1.0f + 900.0f * (float)rand() / (float)RAND_MAX;
			float s = 0.1f + 4.0f * (float)rand() / (float)RAND_MAX;
			float qx = cosf(a) * r, qy = sinf(a) * r;
			float g = matte_field(&sp, qx, qy, 0.0f);

			if (is_grown(shape)) {
				float gs = matte_field(&sp, qx * s, qy * s, 0.0f);

				check(fabsf(gs - s * g) <= 1e-3f * fmaxf(1.0f, s * g),
				      "%s does not scale about its centre: f(%.3f q) = %.4f, %.3f f(q) = %.4f",
				      shape_name[shape], s, gs, s, s * g);
			}

			/* monotone outwards along the ray, for everything whose
			 * range is found from the boundary */
			if (!sweep) {
				float g2 = matte_field(&sp, qx * 1.05f, qy * 1.05f, 0.0f);

				check(g2 >= g - 1e-3f, "%s falls along a ray at angle %.3f radius %.1f: %.4f -> %.4f",
				      shape_name[shape], a, r, g, g2);
			}
		}
	}

	/* the star's fold: points either side of every sector boundary agree */
	{
		struct matte_shape_params sp;

		shape_params(MATTE_SHAPE_STAR, &sp);
		for (i = 0; i < 10; i++) {
			float a = TAU * ((float)i + 0.5f) / 10.0f; /* the folds sit between points */
			float l = matte_field(&sp, 100.0f * sinf(a - 1e-4f), -100.0f * cosf(a - 1e-4f), 0.0f);
			float h = matte_field(&sp, 100.0f * sinf(a + 1e-4f), -100.0f * cosf(a + 1e-4f), 0.0f);

			check(fabsf(l - h) < 0.05f, "star seam at fold %d: %.4f against %.4f", i, l, h);
		}
	}
}

/* ------------------------------------------------------------------ */
/* 6. the clock                                                       */
/* ------------------------------------------------------------------ */

static void check_clock(void)
{
	int easing, i;

	for (easing = 0; easing <= MATTE_EASE_OVERSHOOT; easing++) {
		const float pauses[][2] = {{0.0f, 0.0f}, {0.3f, 0.25f}, {0.0f, 0.4f}, {1.0f, 0.4f}, {0.7f, 0.8f}};

		for (size_t p = 0; p < sizeof(pauses) / sizeof(pauses[0]); p++) {
			struct matte_timing tm = {easing, 0.8f, pauses[p][0], pauses[p][1]};
			float prev = 0.0f;
			float peak = 0.0f;

			check(matte_travel(&tm, 0.0f) == 0.0f, "easing %d pause %zu: travel at t=0 is %.6f", easing, p,
			      matte_travel(&tm, 0.0f));
			check(matte_travel(&tm, 1.0f) == 1.0f, "easing %d pause %zu: travel at t=1 is %.6f", easing, p,
			      matte_travel(&tm, 1.0f));

			for (i = 1; i <= 1000; i++) {
				float t = (float)i / 1000.0f;
				float u = matte_travel(&tm, t);

				peak = fmaxf(peak, u);
				check(isfinite(u), "easing %d pause %zu: travel at %.3f is not finite", easing, p, t);
				if (easing != MATTE_EASE_OVERSHOOT)
					check(u >= prev - 1e-6f && u <= 1.0f + 1e-6f,
					      "easing %d pause %zu: travel goes backwards or past the end at %.3f",
					      easing, p, t);
				prev = u;
			}

			if (easing == MATTE_EASE_OVERSHOOT && pauses[p][1] > 0.0f && pauses[p][0] > 0.0f &&
			    pauses[p][0] < 1.0f)
				check(peak > pauses[p][0] + 0.01f, "overshoot never runs past the pause (pause %zu)",
				      p);

			/* the pause really holds */
			if (pauses[p][1] > 0.0f) {
				float a = (1.0f - pauses[p][1]) * pauses[p][0];
				float b = a + pauses[p][1];
				float mid = 0.5f * (a + b);

				check(fabsf(matte_travel(&tm, mid) - pauses[p][0]) < 1e-6f,
				      "easing %d pause %zu: the pause does not hold at its size", easing, p);
			}
		}
	}

	/* overshoot does overshoot */
	{
		float peak = 0.0f;

		for (i = 0; i <= 1000; i++)
			peak = fmaxf(peak, matte_ease(MATTE_EASE_OVERSHOOT, 1.0f, (float)i / 1000.0f));
		check(peak > 1.05f, "overshoot at full strength peaks at only %.3f", peak);
		check(fabsf(matte_ease(MATTE_EASE_OVERSHOOT, 0.0f, 0.5f) - (1.0f - 0.125f)) < 1e-5f,
		      "overshoot at zero strength is a cubic ease out");
	}
}

/* ------------------------------------------------------------------ */
/* 7. image silhouettes                                               */
/* ------------------------------------------------------------------ */

static uint8_t img[200 * 160 * 4];

static void paint(int w, int h, bool (*inside)(float x, float y), bool use_alpha)
{
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint8_t *p = img + (y * w + x) * 4;
			bool in = inside((x + 0.5f - w * 0.5f), (y + 0.5f - h * 0.5f));

			if (use_alpha) {
				p[0] = p[1] = p[2] = 200;
				p[3] = in ? 255 : 0;
			} else {
				p[0] = p[1] = p[2] = in ? 255 : 0;
				p[3] = 255;
			}
		}
}

static bool in_disc(float x, float y)
{
	return x * x + y * y < 60.0f * 60.0f;
}

static bool in_square(float x, float y)
{
	return fabsf(x) < 50.0f && fabsf(y) < 50.0f;
}

static bool in_ring(float x, float y)
{
	float r2 = x * x + y * y;
	return r2 < 70.0f * 70.0f && r2 > 40.0f * 40.0f;
}

static bool in_nothing(float x, float y)
{
	(void)x;
	(void)y;
	return false;
}

static void profile_spread(float *lo, float *hi)
{
	int i;

	*lo = INFINITY;
	*hi = 0.0f;
	for (i = 0; i < MATTE_PROFILE_SIZE; i++) {
		*lo = fminf(*lo, profile[i]);
		*hi = fmaxf(*hi, profile[i]);
	}
}

static void check_profiles(void)
{
	float lo, hi, sum = 0.0f;
	int i;

	paint(200, 160, in_disc, true);
	check(matte_build_profile(img, 200, 160, profile), "a disc builds a profile");
	profile_spread(&lo, &hi);
	check(hi / lo < 1.04f, "a disc's profile is round: %.3f to %.3f", lo, hi);
	for (i = 0; i < MATTE_PROFILE_SIZE; i++)
		sum += profile[i];
	check(fabsf(sum / MATTE_PROFILE_SIZE - 1.0f) < 1e-3f, "the profile is normalised to a mean of 1");

	/* the brightness is used when there is no transparency */
	paint(200, 160, in_square, false);
	check(matte_build_profile(img, 200, 160, profile), "an opaque square builds a profile");
	profile_spread(&lo, &hi);
	check(fabsf(hi / lo - 1.4142f) < 0.06f, "a square's corners are root two out: %.3f", hi / lo);

	/* a ring is traced as its outline, hole filled */
	paint(200, 160, in_ring, true);
	check(matte_build_profile(img, 200, 160, profile), "a ring builds a profile");
	profile_spread(&lo, &hi);
	check(hi / lo < 1.04f, "a ring is traced as a disc: %.3f to %.3f", lo, hi);

	paint(200, 160, in_nothing, true);
	check(!matte_build_profile(img, 200, 160, profile), "an empty image is refused");
}

int main(void)
{
	/* the silhouette the image shape is checked with: a lopsided square */
	paint(200, 160, in_square, true);
	matte_build_profile(img, 200, 160, profile);

	check_outlines();
	check_scaling_and_rays();
	check_clock();
	check_ends();
	check_profiles();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
