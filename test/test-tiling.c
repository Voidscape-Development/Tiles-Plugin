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
 * Geometry checks for the tiling transition.
 *
 * The lattice and coverage math below mirrors data/effects/tiles_transition.effect
 * one for one. It exists so the two claims the effect rests on can be checked
 * without a GPU:
 *
 *   1. every shape covers the canvas edge to edge at full scale - no pixel is
 *      ever left uncovered when the sweep finishes
 *   2. every tile centre lands inside the sweep, so an offset origin cannot cut
 *      the transition short
 */

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define SHAPE_SQUARE 0
#define SHAPE_CIRCLE 1
#define SHAPE_HEXAGON 2
#define SHAPE_TRIANGLE 3

#define DIR_INSIDE_OUT 0
#define DIR_OUTSIDE_IN 1
#define DIR_MIRRORED_IN 2
#define DIR_MIRRORED_OUT 3
#define DIR_ANGLE 4

#define EASE_LINEAR 0
#define EASE_IN_OUT 1
#define EASE_OUT 2

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

struct vec2 {
	float x, y;
};

struct cell {
	struct vec2 centre;
	float metric;
};

struct grid {
	int shape;
	float tile_px;
	struct vec2 origin;
	struct vec2 rot; /* cos, sin */
};

static struct vec2 to_lattice(const struct grid *g, struct vec2 px)
{
	struct vec2 d = {px.x - g->origin.x, px.y - g->origin.y};
	struct vec2 out;

	out.x = (d.x * g->rot.x + d.y * g->rot.y) / g->tile_px;
	out.y = (d.y * g->rot.x - d.x * g->rot.y) / g->tile_px;
	return out;
}

static struct vec2 from_lattice(const struct grid *g, struct vec2 q)
{
	struct vec2 r = {q.x * g->tile_px, q.y * g->tile_px};
	struct vec2 out;

	out.x = r.x * g->rot.x - r.y * g->rot.y + g->origin.x;
	out.y = r.x * g->rot.y + r.y * g->rot.x + g->origin.y;
	return out;
}

static struct vec2 hex_centre(struct vec2 q)
{
	const float sx = 1.0f, sy = 1.7320508f;
	const float ox = 0.5f, oy = 0.8660254f;
	struct vec2 c1, c2, d1, d2;

	c1.x = (floorf(q.x / sx) + 0.5f) * sx;
	c1.y = (floorf(q.y / sy) + 0.5f) * sy;
	c2.x = (floorf((q.x - ox) / sx) + 0.5f) * sx + ox;
	c2.y = (floorf((q.y - oy) / sy) + 0.5f) * sy + oy;

	d1.x = q.x - c1.x;
	d1.y = q.y - c1.y;
	d2.x = q.x - c2.x;
	d2.y = q.y - c2.y;

	if (d1.x * d1.x + d1.y * d1.y < d2.x * d2.x + d2.y * d2.y)
		return c1;
	return c2;
}

static float shape_metric(int shape, struct vec2 local)
{
	float ax = fabsf(local.x);
	float ay = fabsf(local.y);

	if (shape == SHAPE_SQUARE)
		return (ax > ay ? ax : ay) * 2.0f;
	if (shape == SHAPE_CIRCLE)
		return sqrtf(local.x * local.x + local.y * local.y) * 1.7320508f;

	return (ax > ax * 0.5f + ay * 0.8660254f ? ax : ax * 0.5f + ay * 0.8660254f) * 2.0f;
}

static struct cell resolve_tile(int shape, struct vec2 q)
{
	struct cell cell;
	struct vec2 local;

	if (shape == SHAPE_SQUARE) {
		cell.centre.x = floorf(q.x) + 0.5f;
		cell.centre.y = floorf(q.y) + 0.5f;
		local.x = q.x - cell.centre.x;
		local.y = q.y - cell.centre.y;
		cell.metric = shape_metric(shape, local);
	} else if (shape == SHAPE_TRIANGLE) {
		float sy = q.y / 0.8660254f;
		float sx = q.x - 0.5f * sy;
		float ix = floorf(sx), iy = floorf(sy);
		float fx = sx - ix, fy = sy - iy;
		float b0, b1, b2, mb, cx, cy;

		if (fx + fy < 1.0f) {
			b0 = 1.0f - fx - fy;
			b1 = fx;
			b2 = fy;
			cx = ix + 0.3333333f;
			cy = iy + 0.3333333f;
		} else {
			b0 = 1.0f - fy;
			b1 = 1.0f - fx;
			b2 = fx + fy - 1.0f;
			cx = ix + 0.6666667f;
			cy = iy + 0.6666667f;
		}

		mb = b0 < b1 ? b0 : b1;
		mb = mb < b2 ? mb : b2;

		cell.metric = 1.0f - 3.0f * mb;
		cell.centre.x = cx + 0.5f * cy;
		cell.centre.y = 0.8660254f * cy;
	} else {
		cell.centre = hex_centre(q);
		local.x = q.x - cell.centre.x;
		local.y = q.y - cell.centre.y;
		cell.metric = shape_metric(shape, local);
	}

	return cell;
}

static float saturatef(float v)
{
	if (v < 0.0f)
		return 0.0f;
	if (v > 1.0f)
		return 1.0f;
	return v;
}

/* mirrors ease_order() in the effect */
static float ease_order(int easing, float d)
{
	if (easing == EASE_IN_OUT)
		return d * d * (3.0f - 2.0f * d);
	if (easing == EASE_OUT)
		return 1.0f - (1.0f - d) * (1.0f - d);
	return d;
}

/* How much of the transition a tile spends growing, found by walking the clock
 * rather than by restating the formula. */
static float growth_window(int easing, float td, float d, bool legacy)
{
	const int steps = 20000;
	float first = -1.0f, last = -1.0f;
	int i;

	for (i = 0; i <= steps; i++) {
		float t = (float)i / (float)steps;
		float p;

		if (legacy) {
			/* the old scheme: ease the clock, launch on the raw order */
			p = saturatef((ease_order(easing, t) - d * (1.0f - td)) / td);
		} else {
			p = saturatef((t - ease_order(easing, d) * (1.0f - td)) / td);
		}

		if (p > 0.0f && first < 0.0f)
			first = t;
		/* the last tile lands on p = 1 at t = 1, where the division
		 * leaves it a rounding step short in float */
		if (p >= 1.0f - 1e-6f && last < 0.0f)
			last = t;
	}

	if (first < 0.0f || last < 0.0f)
		return -1.0f;
	return last - first;
}

static float sweep_order(int direction, struct vec2 origin, struct vec2 dir, struct vec2 range, struct vec2 centre)
{
	struct vec2 d = {centre.x - origin.x, centre.y - origin.y};
	float len = sqrtf(d.x * d.x + d.y * d.y);
	float proj, m;

	if (direction == DIR_INSIDE_OUT)
		return len / range.y;
	if (direction == DIR_OUTSIDE_IN)
		return 1.0f - len / range.y;

	proj = d.x * dir.x + d.y * dir.y;

	if (direction == DIR_ANGLE)
		return (proj - range.x) / range.y;

	m = fabsf(proj) / range.y;

	if (direction == DIR_MIRRORED_OUT)
		return m;
	return 1.0f - m;
}

/* mirror of tiles_sweep_extent() in src/tiles-transition.c */
static void sweep_extent(int direction, float angle, struct vec2 origin, float cx, float cy, float pad,
			 struct vec2 *dir, struct vec2 *range)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float max_radius = 0.0f, proj_min = 0.0f, proj_max = 0.0f;
	int i;

	dir->x = cosf(angle);
	dir->y = sinf(angle);

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - origin.x;
		float dy = corner_y[i] - origin.y;
		float radius = sqrtf(dx * dx + dy * dy);
		float proj = dx * dir->x + dy * dir->y;

		if (radius > max_radius)
			max_radius = radius;
		if (i == 0 || proj < proj_min)
			proj_min = proj;
		if (i == 0 || proj > proj_max)
			proj_max = proj;
	}

	if (direction == DIR_ANGLE) {
		range->x = proj_min - pad;
		range->y = fmaxf(proj_max - proj_min + 2.0f * pad, 1.0f);
	} else if (direction == DIR_MIRRORED_IN || direction == DIR_MIRRORED_OUT) {
		range->x = 0.0f;
		range->y = fmaxf(fmaxf(fabsf(proj_min), fabsf(proj_max)) + pad, 1.0f);
	} else {
		range->x = 0.0f;
		range->y = fmaxf(max_radius + pad, 1.0f);
	}
}

/* mirror of tiles_circumradius() in src/tiles-transition.c */
static float circumradius(int shape, float tile_px)
{
	if (shape == SHAPE_SQUARE)
		return tile_px * 0.7071068f;
	return tile_px * 0.5773503f;
}

/* ------------------------------------------------------------------ */

static const char *shape_name(int shape)
{
	switch (shape) {
	case SHAPE_SQUARE:
		return "square";
	case SHAPE_CIRCLE:
		return "circle";
	case SHAPE_HEXAGON:
		return "hexagon";
	default:
		return "triangle";
	}
}

static const char *direction_name(int direction)
{
	switch (direction) {
	case DIR_INSIDE_OUT:
		return "inside out";
	case DIR_OUTSIDE_IN:
		return "outside in";
	case DIR_MIRRORED_IN:
		return "mirrored in";
	case DIR_MIRRORED_OUT:
		return "mirrored out";
	default:
		return "directional";
	}
}

/*
 * Fraction of the canvas the tiles cover at scale s. Circles reach past their
 * own cell, so their neighbours are taken into account the same way the effect
 * does it.
 */
static double covered_fraction(const struct grid *g, float s, float cw, float ch, float *worst_excess)
{
	const float step = 1.31f; /* deliberately not a whole pixel */
	long inside = 0, total = 0;
	float px, py;

	if (worst_excess)
		*worst_excess = 0.0f;

	for (py = 0.13f; py < ch; py += step) {
		for (px = 0.11f; px < cw; px += step) {
			struct vec2 p = {px, py};
			struct vec2 q = to_lattice(g, p);
			struct cell cell = resolve_tile(g->shape, q);
			/* A cell boundary is a float32 tie. The round-off grows with
			 * the lattice coordinate, so the slack has to as well: a tile
			 * 150 units out from the origin resolves to about 1e-5. */
			float tolerance = 4.0f * FLT_EPSILON * (1.0f + fabsf(q.x) + fabsf(q.y));
			bool covered = cell.metric <= s + tolerance;

			if (worst_excess && cell.metric - tolerance > *worst_excess)
				*worst_excess = cell.metric - tolerance;

			if (!covered && g->shape == SHAPE_CIRCLE) {
				const float ox[6] = {1.0f, -1.0f, 0.5f, -0.5f, 0.5f, -0.5f};
				const float oy[6] = {0.0f, 0.0f, 0.8660254f, 0.8660254f, -0.8660254f, -0.8660254f};
				int i;

				for (i = 0; i < 6 && !covered; i++) {
					struct vec2 local = {q.x - (cell.centre.x + ox[i]),
							     q.y - (cell.centre.y + oy[i])};

					covered = shape_metric(SHAPE_CIRCLE, local) <= s + tolerance;
				}
			}

			total++;
			if (covered)
				inside++;
		}
	}

	return (double)inside / (double)total;
}

/* Claim 1: at full scale the tiles leave no gap, whatever the tile size, grid
 * rotation or origin offset. */
static void test_full_coverage(void)
{
	const float sizes[] = {13.0f, 64.0f, 96.0f, 257.0f};
	const float rotations[] = {0.0f, 7.0f, 30.0f, 45.0f, 137.5f};
	const float origins[][2] = {{960.0f, 540.0f}, {0.0f, 0.0f}, {1920.0f, 1080.0f}, {-480.0f, 1600.0f}};
	int shape, i, j, k;

	for (shape = 0; shape < 4; shape++) {
		for (i = 0; i < 4; i++) {
			for (j = 0; j < 5; j++) {
				for (k = 0; k < 4; k++) {
					struct grid g;
					double frac;
					float excess = 0.0f;

					g.shape = shape;
					g.tile_px = sizes[i];
					g.origin.x = origins[k][0];
					g.origin.y = origins[k][1];
					g.rot.x = cosf(rotations[j] * (float)M_PI / 180.0f);
					g.rot.y = sinf(rotations[j] * (float)M_PI / 180.0f);

					frac = covered_fraction(&g, 1.0f, 1920.0f, 1080.0f, &excess);
					check(frac >= 1.0,
					      "%s tiles leave %.4f%% of the canvas uncovered at full scale "
					      "(size %.0f, rotation %.1f deg, origin %.0f/%.0f)",
					      shape_name(shape), (1.0 - frac) * 100.0, sizes[i], rotations[j],
					      origins[k][0], origins[k][1]);

					/* No pixel sits outside the tile that owns it: the lattice
					 * really is gap free, rather than merely close. */
					check(excess <= 1.0f,
					      "%s pixel sits %.6f outside its own tile at full scale "
					      "(size %.0f, rotation %.1f deg, origin %.0f/%.0f)",
					      shape_name(shape), excess - 1.0f, sizes[i], rotations[j], origins[k][0],
					      origins[k][1]);
				}
			}
		}
	}
}

/* Tiles grow from nothing, area tracks scale squared, and they only ever
 * reach full coverage at scale 1. */
static void test_growth_profile(void)
{
	const float scales[] = {0.0f, 0.25f, 0.5f, 0.75f, 0.999f};
	int shape, i;

	for (shape = 0; shape < 4; shape++) {
		struct grid g;
		double previous = -1.0;

		g.shape = shape;
		g.tile_px = 96.0f;
		g.origin.x = 960.0f;
		g.origin.y = 540.0f;
		g.rot.x = 1.0f;
		g.rot.y = 0.0f;

		for (i = 0; i < 5; i++) {
			double frac = covered_fraction(&g, scales[i], 1920.0f, 1080.0f, NULL);

			check(frac >= previous, "%s coverage went backwards at scale %.3f", shape_name(shape),
			      scales[i]);
			previous = frac;

			if (scales[i] == 0.0f)
				check(frac == 0.0, "%s covers %.4f of the canvas at scale 0", shape_name(shape), frac);

			if (shape != SHAPE_CIRCLE && scales[i] > 0.0f) {
				double expected = (double)scales[i] * (double)scales[i];

				check(fabs(frac - expected) < 0.02, "%s covers %.4f at scale %.3f, expected about %.4f",
				      shape_name(shape), frac, scales[i], expected);
			}

			if (shape == SHAPE_CIRCLE && scales[i] > 0.0f) {
				double expected = (double)scales[i] * (double)scales[i];

				check(frac >= expected,
				      "overlapping circles cover %.4f at scale %.3f, less than the %.4f a "
				      "non-overlapping shape would",
				      frac, scales[i], expected);
			}
		}
	}
}

/* Claim: the shapes tile, they do not stack. A pixel belongs to exactly one
 * tile for the three gap-free shapes. */
static void test_tiles_do_not_overlap(void)
{
	const int shapes[] = {SHAPE_SQUARE, SHAPE_HEXAGON};
	const float square_x[4] = {1.0f, -1.0f, 0.0f, 0.0f};
	const float square_y[4] = {0.0f, 0.0f, 1.0f, -1.0f};
	const float hex_x[6] = {1.0f, -1.0f, 0.5f, -0.5f, 0.5f, -0.5f};
	const float hex_y[6] = {0.0f, 0.0f, 0.8660254f, 0.8660254f, -0.8660254f, -0.8660254f};
	int s, i;

	for (s = 0; s < 2; s++) {
		struct grid g;
		int shape = shapes[s];
		int neighbours = shape == SHAPE_SQUARE ? 4 : 6;

		g.shape = shape;
		g.tile_px = 71.0f;
		g.origin.x = 640.0f;
		g.origin.y = 360.0f;
		g.rot.x = cosf(0.4f);
		g.rot.y = sinf(0.4f);

		for (i = 0; i < 20000; i++) {
			struct vec2 p = {(float)(rand() % 192000) / 100.0f, (float)(rand() % 108000) / 100.0f};
			struct vec2 q = to_lattice(&g, p);
			struct cell cell = resolve_tile(shape, q);
			int n;

			for (n = 0; n < neighbours; n++) {
				struct vec2 local;

				local.x = q.x - (cell.centre.x + (shape == SHAPE_SQUARE ? square_x[n] : hex_x[n]));
				local.y = q.y - (cell.centre.y + (shape == SHAPE_SQUARE ? square_y[n] : hex_y[n]));

				check(shape_metric(shape, local) >= 1.0f - 1e-4f,
				      "%s tiles overlap: pixel %.2f/%.2f sits inside two tiles at full scale",
				      shape_name(shape), p.x, p.y);
			}
		}
	}
}

/* Claim 2: no early cutoff. Every tile centre on the canvas gets a sweep
 * position inside 0..1, so its animation both starts and ends within the
 * transition - including when the origin sits outside the frame. */
static void test_sweep_completes(void)
{
	const float origins[][2] = {{960.0f, 540.0f},  {0.0f, 0.0f},       {1920.0f, 1080.0f},
				    {-600.0f, 300.0f}, {2400.0f, -200.0f}, {480.0f, 1080.0f}};
	const float angles[] = {0.0f, 33.0f, 90.0f, 214.0f, 359.0f};
	const float tile_dur = 0.35f;
	const int easings[] = {EASE_LINEAR, EASE_IN_OUT, EASE_OUT};
	int direction, o, a, shape, e;

	for (shape = 0; shape < 4; shape++)
		for (direction = 0; direction < 5; direction++) {
			for (o = 0; o < 6; o++) {
				for (a = 0; a < 5; a++) {
					struct grid g;
					struct vec2 origin = {origins[o][0], origins[o][1]};
					struct vec2 dir, range;
					float px, py;
					float lowest = 1.0f, highest = 0.0f;

					g.shape = shape;
					g.tile_px = 96.0f;
					g.origin = origin;
					g.rot.x = 1.0f;
					g.rot.y = 0.0f;

					sweep_extent(direction, angles[a] * (float)M_PI / 180.0f, origin, 1920.0f,
						     1080.0f, circumradius(shape, g.tile_px), &dir, &range);

					for (py = 0.0f; py <= 1080.0f; py += 9.0f) {
						for (px = 0.0f; px <= 1920.0f; px += 9.0f) {
							struct vec2 p = {px, py};
							struct cell cell = resolve_tile(g.shape, to_lattice(&g, p));
							struct vec2 centre = from_lattice(&g, cell.centre);
							float d = sweep_order(direction, origin, dir, range, centre);

							if (d < lowest)
								lowest = d;
							if (d > highest)
								highest = d;
						}
					}

					check(lowest >= -1e-4f && highest <= 1.0f + 1e-4f,
					      "%s sweep at %.0f deg from origin %.0f/%.0f puts a tile at %.4f..%.4f, "
					      "outside 0..1",
					      direction_name(direction), angles[a], origins[o][0], origins[o][1],
					      lowest, highest);

					/* the last tile still finishes its own animation by t = 1,
					 * whichever curve the launch order is shaped by - the
					 * easing is monotonic, so the latest tile is still the
					 * one furthest along the sweep */
					for (e = 0; e < 3; e++) {
						float worst_start = ease_order(easings[e], highest) * (1.0f - tile_dur);

						check(worst_start + tile_dur <= 1.0f + 1e-4f,
						      "%s sweep at %.0f deg from origin %.0f/%.0f leaves the last "
						      "tile unfinished at t=1 with easing %d (starts at %.4f)",
						      direction_name(direction), angles[a], origins[o][0],
						      origins[o][1], easings[e], worst_start);
					}
				}
			}
		}
}

/* Cover has to be completely opaque at the halfway point, otherwise the scene
 * swap underneath it is visible. */
static void test_cover_is_opaque_at_the_switch(void)
{
	const float tile_durs[] = {0.01f, 0.35f, 1.0f};
	const int easings[] = {EASE_LINEAR, EASE_IN_OUT, EASE_OUT};
	int i, e;

	for (e = 0; e < 3; e++) {
		for (i = 0; i < 3; i++) {
			float tile_dur = tile_durs[i];
			float half_t = 1.0f; /* progress 0.5, doubled */
			float d = 1.0f;      /* the very last tile */
			float start = ease_order(easings[e], d) * (1.0f - tile_dur);
			float p = saturatef((half_t - start) / tile_dur);

			check(p >= 1.0f - 1e-4f,
			      "cover is only %.4f grown at the scene switch with %.0f%% per-tile duration, "
			      "easing %d",
			      p, tile_dur * 100.0f, easings[e]);
		}
	}
}

/* The wave has to grow, hold and shrink inside one sweep however thick the
 * band is. */
static void test_wave_fits_the_sweep(void)
{
	const float bands[] = {0.0f, 0.2f, 0.6f, 4.0f};
	const float tile_durs[] = {0.01f, 0.35f, 1.0f};
	int i, j;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 3; j++) {
			float band = bands[i];
			float tile_dur = tile_durs[j];
			float total = 2.0f * tile_dur + band;
			float start, finish;

			if (total > 0.95f) {
				tile_dur *= 0.95f / total;
				band *= 0.95f / total;
			}

			start = 1.0f * (1.0f - 2.0f * tile_dur - band);
			finish = start + 2.0f * tile_dur + band;

			check(start >= -1e-4f, "wave band %.2f/duration %.2f starts the last tile at %.4f", bands[i],
			      tile_durs[j], start);
			check(finish <= 1.0f + 1e-4f, "wave band %.2f/duration %.2f finishes the last tile at %.4f",
			      bands[i], tile_durs[j], finish);
		}
	}
}

/* The easing shapes the launch order, not the clock. That is what keeps every
 * tile growing at the same rate: whatever the easing, a tile's animation has to
 * occupy the same slice of the transition wherever it sits in the sweep. */
static void test_growth_is_evenly_paced(void)
{
	const int easings[] = {EASE_LINEAR, EASE_IN_OUT, EASE_OUT};
	const float tile_durs[] = {0.15f, 0.35f, 0.6f};
	int e, i, k;

	for (e = 0; e < 3; e++) {
		for (i = 0; i < 3; i++) {
			float td = tile_durs[i];
			float lo = 2.0f, hi = 0.0f;
			float old_lo = 2.0f, old_hi = 0.0f;

			for (k = 0; k <= 20; k++) {
				float d = (float)k / 20.0f;
				float w = growth_window(easings[e], td, d, false);
				float old = growth_window(easings[e], td, d, true);

				check(fabsf(w - td) < 0.01f,
				      "easing %d gives the tile at %.2f %.4f of the transition to grow, not %.4f",
				      easings[e], d, w, td);

				if (w < lo)
					lo = w;
				if (w > hi)
					hi = w;
				if (old < old_lo)
					old_lo = old;
				if (old > old_hi)
					old_hi = old;
			}

			/* the regression this guards: easing the clock stretched some
			 * tiles and compressed others, and the compressed ones snapped */
			check(hi - lo <= old_hi - old_lo + 1e-4f,
			      "easing %d at %.0f%% spreads growth over %.4f..%.4f, no better than the %.4f..%.4f "
			      "it replaced",
			      easings[e], td * 100.0f, lo, hi, old_lo, old_hi);

			if (easings[e] != EASE_LINEAR)
				check(old_hi - old_lo > 0.01f,
				      "easing %d at %.0f%% was already even, so this test proves nothing", easings[e],
				      td * 100.0f);
		}
	}
}

/* Whichever curve the launch order is shaped by, the first tile still has to
 * set off at the start and the last one still has to land on the finish. */
static void test_easing_keeps_the_endpoints(void)
{
	const int easings[] = {EASE_LINEAR, EASE_IN_OUT, EASE_OUT};
	int e, k;

	for (e = 0; e < 3; e++) {
		float previous = -1.0f;

		check(ease_order(easings[e], 0.0f) == 0.0f, "easing %d starts the sweep at %.6f, not 0", easings[e],
		      ease_order(easings[e], 0.0f));
		check(ease_order(easings[e], 1.0f) == 1.0f, "easing %d finishes the sweep at %.6f, not 1", easings[e],
		      ease_order(easings[e], 1.0f));

		for (k = 0; k <= 200; k++) {
			float d = (float)k / 200.0f;
			float o = ease_order(easings[e], d);

			check(o >= previous, "easing %d reorders the sweep at %.3f", easings[e], d);
			check(o >= 0.0f && o <= 1.0f, "easing %d puts the tile at %.3f outside 0..1 (%.6f)", easings[e],
			      d, o);
			previous = o;
		}
	}
}

int main(void)
{
	srand(20260812);

	test_full_coverage();
	test_growth_profile();
	test_tiles_do_not_overlap();
	test_sweep_completes();
	test_cover_is_opaque_at_the_switch();
	test_wave_fits_the_sweep();
	test_growth_is_evenly_paced();
	test_easing_keeps_the_endpoints();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
