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
 * The lattice, the sweep order and the tile clock are not restated here: this
 * file compiles data/effects/tiles_math.effect itself, through test/hlsl.hpp,
 * and the sweep extent and ramp widths come from src/tiles-math.c, the same as
 * the plugin. What is checked is the two claims the effect rests on:
 *
 *   1. every shape covers the canvas edge to edge at full scale - no pixel is
 *      ever left uncovered when the sweep finishes
 *   2. every tile centre lands inside the sweep, so an offset origin cannot cut
 *      the transition short
 */

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "hlsl.hpp"
#include "tiles-math.h"

/* ------------------------------------------------------------------ */
/* the effect                                                         */
/* ------------------------------------------------------------------ */

namespace effect {
using namespace hlsl;

/* The uniforms tiles_math.effect reads, set below the way the plugin sets
 * them. */
float2 canvas;
float2 origin_px;
float2 grid_rot;
float2 dir_vec;
float2 dir_range;
float4 color_a;
float4 color_b;
float progress;
float tile_px;
float edge;
float tile_dur;
float band;
float randomness;
float variation;
int easing;
int mode;
int shape;
int direction;
bool invert;
bool reverse_exit;
bool b_tiles;

#include "tiles_math.effect"
} // namespace effect

using effect::Cell;
using hlsl::float2;

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
/* driving it                                                         */
/* ------------------------------------------------------------------ */

struct grid {
	int shape;
	float tile_px;
	float2 origin;
	float2 rot; /* cos, sin */
};

static struct grid make_grid(int shape, float tile_px, float ox, float oy, float rotation)
{
	struct grid g;

	g.shape = shape;
	g.tile_px = tile_px;
	g.origin = float2(ox, oy);
	g.rot = float2(cosf(rotation), sinf(rotation));
	return g;
}

/* Sets the lattice uniforms for this grid. */
static void use(const struct grid &g)
{
	effect::shape = g.shape;
	effect::tile_px = g.tile_px;
	effect::origin_px = g.origin;
	effect::grid_rot = g.rot;
}

/* Sets the sweep uniforms, through the plugin's own function. */
static void use_sweep(int direction, float angle, float cx, float cy)
{
	float dir[2], range[2];

	tiles_sweep_extent(direction, angle, effect::origin_px.x, effect::origin_px.y, cx, cy,
			   tiles_circumradius(effect::shape, effect::tile_px), dir, range);
	effect::direction = direction;
	effect::dir_vec = float2(dir[0], dir[1]);
	effect::dir_range = float2(range[0], range[1]);
}

/* The orientation a Cairo pentagon's gauge is read in. The shader reads each of
 * the four candidates this way when it picks the one a pixel belongs to. */
static float2 cairo_frame(float2 d, int orientation)
{
	if (orientation == 1)
		return float2(-d.x, -d.y);
	if (orientation == 2)
		return float2(d.y, -d.x);
	if (orientation == 3)
		return float2(-d.y, d.x);
	return d;
}

/*
 * What a given cell makes of an arbitrary pixel. resolve_tile() only answers
 * for the cell that owns a pixel; this asks a neighbour, which is what the
 * overlap check needs and the shader never does. It is built from the shader's
 * own gauges and the kind and size the shader carries on the cell. Triangles
 * are the one shape left out: their metric is barycentric, and the shader never
 * asks a triangle about a pixel it does not own.
 */
static float metric_at(const Cell &c, float2 q)
{
	float2 local = q - c.centre;

	if (effect::shape == TILES_SHAPE_OCTAGON)
		return c.kind < 0.5f ? effect::octagon_metric(local) : (fabsf(local.x) + fabsf(local.y)) * 3.4142136f;
	if (effect::shape == TILES_SHAPE_CAIRO)
		return effect::cairo_gauge(cairo_frame(local, (int)c.kind));
	if (effect::shape == TILES_SHAPE_MOSAIC)
		return fmaxf(fabsf(local.x), fabsf(local.y)) * 2.0f / c.size;
	if (effect::shape == TILES_SHAPE_SHATTER) {
		float2 oc = hlsl::floor(c.centre);
		float2 m(0.0f, 0.0f);

		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++)
				m = hlsl::max(m, effect::shatter_cut(local, c.centre, oc + float2((float)i, (float)j)));
		return m.x;
	}

	return effect::shape_metric(local);
}

/* How much of the transition a tile spends growing, found by walking the clock
 * rather than by restating the formula. Keyhole with a linear clock is the
 * tile's plain growth; the legacy scheme is the one the easing replaced, kept
 * here only to show this one is better. */
static float growth_window(int easing, float td, float d, bool legacy)
{
	const int steps = 20000;
	float first = -1.0f, last = -1.0f;

	effect::mode = TILES_MODE_KEYHOLE;
	effect::invert = false;
	effect::easing = easing;
	effect::tile_dur = td;

	for (int i = 0; i <= steps; i++) {
		float t = (float)i / (float)steps;
		float p;

		if (legacy) {
			/* the old scheme: ease the clock, launch on the raw order */
			p = hlsl::saturate((effect::ease_order(t) - d * (1.0f - td)) / td);
		} else {
			effect::progress = t;
			p = effect::tile_scale(d).x;
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

/* A pixel somewhere on a 1920x1080 canvas. Drawn one coordinate at a time:
 * the order two calls in one argument list run in is not fixed. */
static float2 random_pixel(void)
{
	float x = (float)(rand() % 192000) / 100.0f;
	float y = (float)(rand() % 108000) / 100.0f;

	return float2(x, y);
}

/* ------------------------------------------------------------------ */

static const char *shape_name(int shape)
{
	static const char *const names[TILES_SHAPE_COUNT] = {
		"square", "circle", "hexagon", "triangle", "diamond", "brick", "octagon", "cairo", "mosaic", "shatter",
	};

	return names[shape];
}

static const char *direction_name(int direction)
{
	static const char *const names[TILES_DIRECTION_COUNT] = {
		"inside out", "outside in", "mirrored in", "mirrored out", "directional",
	};

	return names[direction];
}

/*
 * Fraction of the canvas the tiles cover at scale s. Circles reach past their
 * own cell, so their six neighbours are asked as well, the ones the shader's
 * best_tile() asks.
 */
static double covered_fraction(const struct grid &g, float s, float cw, float ch, float *worst_excess)
{
	const float step = 1.31f; /* deliberately not a whole pixel */
	long inside = 0, total = 0;

	use(g);

	if (worst_excess)
		*worst_excess = 0.0f;

	for (float py = 0.13f; py < ch; py += step) {
		for (float px = 0.11f; px < cw; px += step) {
			float2 q = effect::to_lattice(float2(px, py));
			Cell cell = effect::resolve_tile(q);
			/* A cell boundary is a float32 tie. The round-off grows with
			 * the lattice coordinate, so the slack has to as well: a tile
			 * 150 units out from the origin resolves to about 1e-5. */
			float tolerance = 4.0f * FLT_EPSILON * (1.0f + fabsf(q.x) + fabsf(q.y));
			bool covered = cell.metric <= s + tolerance;

			if (worst_excess && cell.metric - tolerance > *worst_excess)
				*worst_excess = cell.metric - tolerance;

			if (!covered && g.shape == TILES_SHAPE_CIRCLE) {
				const float ox[6] = {1.0f, -1.0f, 0.5f, -0.5f, 0.5f, -0.5f};
				const float oy[6] = {0.0f, 0.0f, 0.8660254f, 0.8660254f, -0.8660254f, -0.8660254f};

				for (int i = 0; i < 6 && !covered; i++) {
					float2 c = cell.centre + float2(ox[i], oy[i]);

					covered = effect::shape_metric(q - c) <= s + tolerance;
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

	for (int shape = 0; shape < TILES_SHAPE_COUNT; shape++) {
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 5; j++) {
				for (int k = 0; k < 4; k++) {
					struct grid g = make_grid(shape, sizes[i], origins[k][0], origins[k][1],
								  rotations[j] * (float)M_PI / 180.0f);
					float excess = 0.0f;
					double frac = covered_fraction(g, 1.0f, 1920.0f, 1080.0f, &excess);

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

	for (int shape = 0; shape < TILES_SHAPE_COUNT; shape++) {
		struct grid g = make_grid(shape, 96.0f, 960.0f, 540.0f, 0.0f);
		double previous = -1.0;

		for (int i = 0; i < 5; i++) {
			double frac = covered_fraction(g, scales[i], 1920.0f, 1080.0f, NULL);

			check(frac >= previous, "%s coverage went backwards at scale %.3f", shape_name(shape),
			      scales[i]);
			previous = frac;

			if (scales[i] == 0.0f)
				check(frac == 0.0, "%s covers %.4f of the canvas at scale 0", shape_name(shape), frac);

			if (shape != TILES_SHAPE_CIRCLE && scales[i] > 0.0f) {
				double expected = (double)scales[i] * (double)scales[i];

				check(fabs(frac - expected) < 0.02, "%s covers %.4f at scale %.3f, expected about %.4f",
				      shape_name(shape), frac, scales[i], expected);
			}

			if (shape == TILES_SHAPE_CIRCLE && scales[i] > 0.0f) {
				double expected = (double)scales[i] * (double)scales[i];

				check(frac >= expected,
				      "overlapping circles cover %.4f at scale %.3f, less than the %.4f a "
				      "non-overlapping shape would",
				      frac, scales[i], expected);
			}
		}
	}
}

/*
 * Claim: the shapes tile, they do not stack. A pixel belongs to exactly one
 * tile, so no other tile anywhere near it claims the pixel at full scale.
 *
 * The neighbours are found rather than listed: the pixels in a ring around the
 * sample are resolved, and every distinct tile that turns up is asked what it
 * makes of the sample itself. That covers the shapes whose neighbours are not
 * a fixed set - mosaic cells differ in size, shatter cells differ in
 * everything - without a table per shape.
 */
static void test_tiles_do_not_overlap(void)
{
	const int shapes[] = {TILES_SHAPE_SQUARE,  TILES_SHAPE_HEXAGON, TILES_SHAPE_DIAMOND, TILES_SHAPE_BRICK,
			      TILES_SHAPE_OCTAGON, TILES_SHAPE_CAIRO,   TILES_SHAPE_MOSAIC,  TILES_SHAPE_SHATTER};
	const float radii[] = {0.3f, 0.6f, 0.9f, 1.2f, 1.5f};

	for (int shape : shapes) {
		use(make_grid(shape, 71.0f, 640.0f, 360.0f, 0.4f));

		for (int i = 0; i < 3000; i++) {
			float2 p = random_pixel();
			float2 q = effect::to_lattice(p);
			Cell own = effect::resolve_tile(q);

			/* the cell has to agree with itself first, or the check
			 * below is measuring the wrong thing */
			check(fabsf(metric_at(own, q) - own.metric) < 1e-3f,
			      "%s cell at %.3f/%.3f reads its own pixel as %.6f, not the %.6f it resolved to",
			      shape_name(shape), own.centre.x, own.centre.y, metric_at(own, q), own.metric);

			for (float radius : radii) {
				for (int a = 0; a < 12; a++) {
					float ang = (float)a * (float)M_PI / 6.0f;
					float2 probe = q + float2(radius * cosf(ang), radius * sinf(ang));
					Cell other = effect::resolve_tile(probe);
					float2 d = other.centre - own.centre;

					if (hlsl::dot(d, d) < 1e-8f)
						continue;

					check(metric_at(other, q) >= 1.0f - 1e-4f,
					      "%s tiles overlap: pixel %.2f/%.2f sits inside the tile at "
					      "%.3f/%.3f as well as its own at %.3f/%.3f",
					      shape_name(shape), p.x, p.y, other.centre.x, other.centre.y, own.centre.x,
					      own.centre.y);
				}
			}
		}
	}
}

/*
 * The coverage ramp is a fixed width in metric units, chosen per shape so it
 * comes out about a pixel wide on screen. That only works if the metric really
 * does climb at the rate the constant assumes: too slow and the edges blur,
 * too fast and they alias. Mixed size tilings scale the ramp per cell, so the
 * claim is that the metric never climbs faster than the shape's own rate times
 * the cell's own scale, and that it gets close enough somewhere to be worth
 * the width.
 *
 * Triangles are checked by hand instead: their metric is 1 - 3 * min(bary),
 * each barycentric climbs at 1 / height = 1.1547 per lattice unit, so the
 * metric climbs at 3.4641 - exactly the constant tiles_edge_width() gives.
 */
static void test_ramp_matches_the_metric(void)
{
	const float step = 1e-3f;

	for (int shape = 0; shape < TILES_SHAPE_COUNT; shape++) {
		/* metric units per lattice unit, rather than per pixel */
		float gradient = tiles_edge_width(shape, 1.0f);
		float steepest = 0.0f;

		if (shape == TILES_SHAPE_TRIANGLE)
			continue;

		use(make_grid(shape, 96.0f, 0.0f, 0.0f, 0.0f));

		for (int i = 0; i < 3000; i++) {
			float2 p = random_pixel();
			float2 q = effect::to_lattice(p);
			Cell cell = effect::resolve_tile(q);
			float allowed = gradient * cell.aa;
			float grad = 0.0f;

			/*
			 * The metric is a max of linear pieces, so a difference
			 * taken across two axes can straddle a corner between
			 * two of them and read steeper than either. Sweeping the
			 * direction instead and keeping the steepest one-sided
			 * rate gives the gradient of whichever face is crossed,
			 * which is what the ramp is set against.
			 */
			for (int a = 0; a < 24; a++) {
				float ang = (float)a * (float)M_PI / 12.0f;
				float2 probe = q + float2(step * cosf(ang), step * sinf(ang));
				float rate = (metric_at(cell, probe) - cell.metric) / step;

				if (rate > grad)
					grad = rate;
			}

			check(grad <= allowed * 1.001f + 1e-3f,
			      "%s metric climbs at %.4f per lattice unit where the ramp is set for %.4f, so its "
			      "edges would alias",
			      shape_name(shape), grad, allowed);

			if (grad / cell.aa > steepest)
				steepest = grad / cell.aa;
		}

		check(steepest >= gradient * 0.5f,
		      "%s never climbs faster than %.4f per lattice unit but its ramp is set for %.4f, so its "
		      "edges are softer than they need to be",
		      shape_name(shape), steepest, gradient);
	}
}

/*
 * The sweep extent is padded by one circumradius so a tile whose centre sits
 * off canvas still takes its turn. That only holds if the constant really is
 * the furthest a pixel can be from the centre of the tile that owns it.
 */
static void test_circumradius_covers_the_tile(void)
{
	for (int shape = 0; shape < TILES_SHAPE_COUNT; shape++) {
		float worst = 0.0f;
		float limit = tiles_circumradius(shape, 64.0f);

		use(make_grid(shape, 64.0f, 640.0f, 360.0f, 0.3f));

		for (int i = 0; i < 40000; i++) {
			float2 p = random_pixel();
			Cell cell = effect::resolve_tile(effect::to_lattice(p));
			float r = hlsl::length(p - effect::from_lattice(cell.centre));

			if (r > worst)
				worst = r;
		}

		check(worst <= limit,
		      "%s pixels reach %.3f px from their own tile centre, past the %.3f px the "
		      "sweep is padded by",
		      shape_name(shape), worst, limit);
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
	const int easings[] = {TILES_EASE_LINEAR, TILES_EASE_IN_OUT, TILES_EASE_OUT};

	for (int shape = 0; shape < TILES_SHAPE_COUNT; shape++) {
		for (int direction = 0; direction < TILES_DIRECTION_COUNT; direction++) {
			for (int o = 0; o < 6; o++) {
				for (int a = 0; a < 5; a++) {
					float lowest = 1.0f, highest = 0.0f;

					use(make_grid(shape, 96.0f, origins[o][0], origins[o][1], 0.0f));
					use_sweep(direction, angles[a] * (float)M_PI / 180.0f, 1920.0f, 1080.0f);

					for (float py = 0.0f; py <= 1080.0f; py += 9.0f) {
						for (float px = 0.0f; px <= 1920.0f; px += 9.0f) {
							Cell cell = effect::resolve_tile(
								effect::to_lattice(float2(px, py)));
							float d = effect::sweep_position(
								effect::from_lattice(cell.centre));

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
					for (int easing : easings) {
						float worst_start;

						effect::easing = easing;
						worst_start = effect::ease_order(highest) * (1.0f - tile_dur);

						check(worst_start + tile_dur <= 1.0f + 1e-4f,
						      "%s sweep at %.0f deg from origin %.0f/%.0f leaves the last "
						      "tile unfinished at t=1 with easing %d (starts at %.4f)",
						      direction_name(direction), angles[a], origins[o][0],
						      origins[o][1], easing, worst_start);
					}
				}
			}
		}
	}
}

/* Cover has to be completely opaque either side of the halfway point, where
 * the scenes are swapped underneath it - for every tile, both ways out. */
static void test_cover_is_opaque_at_the_switch(void)
{
	const float tile_durs[] = {0.01f, 0.35f, 1.0f};
	const int easings[] = {TILES_EASE_LINEAR, TILES_EASE_IN_OUT, TILES_EASE_OUT};
	const float either_side[] = {nextafterf(0.5f, 0.0f), 0.5f};

	effect::mode = TILES_MODE_COVER;

	for (int easing : easings) {
		for (float tile_dur : tile_durs) {
			for (int reverse = 0; reverse < 2; reverse++) {
				for (float t : either_side) {
					float least = 1.0f;

					effect::easing = easing;
					effect::tile_dur = tile_dur;
					effect::reverse_exit = reverse != 0;
					effect::progress = t;

					for (int k = 0; k <= 100; k++)
						least = fminf(least, effect::tile_scale((float)k / 100.0f).x);

					check(least >= 1.0f - 1e-4f,
					      "cover is only %.4f grown at t=%.8f with %.0f%% per-tile duration, "
					      "easing %d%s",
					      least, t, tile_dur * 100.0f, easing, reverse ? ", reverse exit" : "");
				}
			}
		}
	}
}

/* The wave has to grow, hold and shrink inside one sweep however thick the
 * band is: nothing showing on the first frame, every tile gone by the last. */
static void test_wave_fits_the_sweep(void)
{
	const float bands[] = {0.0f, 0.2f, 0.6f, 4.0f};
	const float tile_durs[] = {0.01f, 0.35f, 1.0f};
	const int easings[] = {TILES_EASE_LINEAR, TILES_EASE_IN_OUT, TILES_EASE_OUT};

	effect::mode = TILES_MODE_WAVE;

	for (float band : bands) {
		for (float tile_dur : tile_durs) {
			for (int easing : easings) {
				float first = 0.0f, last = 0.0f;
				bool all_past = true;

				effect::tile_dur = tile_dur;
				effect::band = band;
				effect::easing = easing;
				tiles_wave_fit(&effect::tile_dur, &effect::band);

				for (int k = 0; k <= 100; k++) {
					float d = (float)k / 100.0f;
					float2 s;

					effect::progress = 0.0f;
					first = fmaxf(first, effect::tile_scale(d).x);

					effect::progress = 1.0f;
					s = effect::tile_scale(d);
					last = fmaxf(last, s.x);
					all_past = all_past && s.y > 0.5f;
				}

				check(first <= 0.0f,
				      "wave band %.2f/duration %.2f/easing %d shows a tile at %.4f on the "
				      "first frame",
				      band, tile_dur, easing, first);
				/* the last tile's shrink lands on its end a rounding
				 * step either side of exactly 1 */
				check(last <= 1e-4f && all_past,
				      "wave band %.2f/duration %.2f/easing %d leaves a tile at %.4f on the last frame",
				      band, tile_dur, easing, last);
			}
		}
	}
}

/* The easing shapes the launch order, not the clock. That is what keeps every
 * tile growing at the same rate: whatever the easing, a tile's animation has to
 * occupy the same slice of the transition wherever it sits in the sweep. */
static void test_growth_is_evenly_paced(void)
{
	const int easings[] = {TILES_EASE_LINEAR, TILES_EASE_IN_OUT, TILES_EASE_OUT};
	const float tile_durs[] = {0.15f, 0.35f, 0.6f};

	for (int easing : easings) {
		for (float td : tile_durs) {
			float lo = 2.0f, hi = 0.0f;
			float old_lo = 2.0f, old_hi = 0.0f;

			for (int k = 0; k <= 20; k++) {
				float d = (float)k / 20.0f;
				float w = growth_window(easing, td, d, false);
				float old = growth_window(easing, td, d, true);

				check(fabsf(w - td) < 0.01f,
				      "easing %d gives the tile at %.2f %.4f of the transition to grow, not %.4f",
				      easing, d, w, td);

				lo = fminf(lo, w);
				hi = fmaxf(hi, w);
				old_lo = fminf(old_lo, old);
				old_hi = fmaxf(old_hi, old);
			}

			/* the regression this guards: easing the clock stretched some
			 * tiles and compressed others, and the compressed ones snapped */
			check(hi - lo <= old_hi - old_lo + 1e-4f,
			      "easing %d at %.0f%% spreads growth over %.4f..%.4f, no better than the %.4f..%.4f "
			      "it replaced",
			      easing, td * 100.0f, lo, hi, old_lo, old_hi);

			if (easing != TILES_EASE_LINEAR)
				check(old_hi - old_lo > 0.01f,
				      "easing %d at %.0f%% was already even, so this test proves nothing", easing,
				      td * 100.0f);
		}
	}
}

/* Whichever curve the launch order is shaped by, the first tile still has to
 * set off at the start and the last one still has to land on the finish. */
static void test_easing_keeps_the_endpoints(void)
{
	const int easings[] = {TILES_EASE_LINEAR, TILES_EASE_IN_OUT, TILES_EASE_OUT};

	for (int easing : easings) {
		float previous = -1.0f;

		effect::easing = easing;

		check(effect::ease_order(0.0f) == 0.0f, "easing %d starts the sweep at %.6f, not 0", easing,
		      effect::ease_order(0.0f));
		check(effect::ease_order(1.0f) == 1.0f, "easing %d finishes the sweep at %.6f, not 1", easing,
		      effect::ease_order(1.0f));

		for (int k = 0; k <= 200; k++) {
			float d = (float)k / 200.0f;
			float o = effect::ease_order(d);

			check(o >= previous, "easing %d reorders the sweep at %.3f", easing, d);
			check(o >= 0.0f && o <= 1.0f, "easing %d puts the tile at %.3f outside 0..1 (%.6f)", easing, d,
			      o);
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
	test_circumradius_covers_the_tile();
	test_ramp_matches_the_metric();
	test_sweep_completes();
	test_cover_is_opaque_at_the_switch();
	test_wave_fits_the_sweep();
	test_growth_is_evenly_paced();
	test_easing_keeps_the_endpoints();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
