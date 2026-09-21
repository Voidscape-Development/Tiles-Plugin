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
 * The glass transition's geometry and motion, checked without a graphics
 * device.
 *
 * Unlike test-tiling.c and test-vortex.c, this does not mirror a shader: the
 * shards are built in C by src/glass-mesh.c, so the test links the real thing.
 * What it holds that code to is the pair of properties the transition rests on:
 *
 *   1. the shards are an exact partition of the canvas - every pixel belongs to
 *      one shard and no pixel belongs to two - for every pattern, shard size and
 *      impact point, including impacts pushed outside the frame. A gap would
 *      show the wrong scene through an unbroken pane; an overlap would blend two
 *      shards over each other once they started moving.
 *
 *   2. every shard's flight fits inside the transition. It is seated and opaque
 *      on the first frame, and it has finished and faded on the last one,
 *      whatever the settings. That is what makes the transition land on a clean
 *      incoming scene rather than on a frame with glass still in the air.
 *
 * The seam bleed is checked separately: it may only ever grow a shard, because
 * it trades an exact partition for a slight overlap on purpose.
 */

#include "../src/glass-mesh.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(bool ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;

	failures++;
	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static const char *pattern_name(int pattern)
{
	switch (pattern) {
	case GLASS_PATTERN_WEB:
		return "web";
	case GLASS_PATTERN_VORONOI:
		return "voronoi";
	default:
		return "ice";
	}
}

static const char *style_name(int style)
{
	switch (style) {
	case GLASS_STYLE_WINDOW:
		return "window";
	case GLASS_STYLE_BLOW:
		return "blow";
	case GLASS_STYLE_WIPE:
		return "wipe";
	default:
		return "assemble";
	}
}

/*
 * Signed distance from a point to a convex polygon's boundary, negative inside.
 * The cells are wound counter-clockwise, so a point inside is on the left of
 * every edge; the worst edge is the boundary the point is nearest.
 */
static float poly_sdf(const struct glass_poly *p, float x, float y)
{
	float worst = -1e30f;
	int i;

	for (i = 0; i < p->n; i++) {
		int j = (i + 1) % p->n;
		float ex = p->x[j] - p->x[i];
		float ey = p->y[j] - p->y[i];
		float len = sqrtf(ex * ex + ey * ey);
		float d;

		if (len < 1e-6f)
			continue;

		/* positive outside: the point is to the right of the edge */
		d = ((x - p->x[i]) * ey - (y - p->y[i]) * ex) / len;
		if (d > worst)
			worst = d;
	}

	return worst;
}

/*
 * The cell's area in double precision.
 *
 * glass_shard::area is a float, and summing a few thousand of those over a
 * canvas of a couple of million square pixels loses enough in the last place to
 * blur the difference between rounding and a shard genuinely counted twice.
 * Re-adding them here keeps the tolerance below tight enough to mean something.
 */
static double poly_area_exact(const struct glass_poly *p)
{
	double sum = 0.0;
	int i;

	for (i = 0; i < p->n; i++) {
		int j = (i + 1) % p->n;

		sum += (double)p->x[i] * (double)p->y[j] - (double)p->x[j] * (double)p->y[i];
	}

	return sum * 0.5;
}

/* ------------------------------------------------------------------ */
/* the partition                                                      */
/* ------------------------------------------------------------------ */

/*
 * Two independent readings of the same property.
 *
 * The areas have to add up to the canvas, which catches a pattern that loses or
 * double counts whole cells. The sample points then catch the case the areas
 * cannot see: a cell in the wrong place, where one region is covered twice and
 * another not at all by exactly the same amount.
 */
static void check_partition(int pattern, float cx, float cy, float shard_px, float ix, float iy, uint32_t seed)
{
	struct glass_build build;
	struct glass_field field;
	char ctx[192];
	double area = 0.0;
	double canvas = (double)cx * (double)cy;
	int gaps = 0;
	int overlaps = 0;
	int worst_overlap = 0;
	int concave = 0;
	int off_centre = 0;
	int i;
	int sx;
	int sy;

	build.canvas_cx = cx;
	build.canvas_cy = cy;
	build.impact_x = ix;
	build.impact_y = iy;
	build.shard_px = shard_px;
	build.pattern = pattern;
	build.seed = seed;

	snprintf(ctx, sizeof(ctx), "%s %.0fx%.0f shard %.0f impact %.0f,%.0f", pattern_name(pattern), (double)cx,
		 (double)cy, (double)shard_px, (double)ix, (double)iy);

	if (!glass_field_build(&field, &build)) {
		check(false, "[%s] the field would not build", ctx);
		return;
	}

	check(field.count > 0 && field.count <= GLASS_MAX_SHARDS, "[%s] %d shards is outside 1..%d", ctx, field.count,
	      GLASS_MAX_SHARDS);

	for (i = 0; i < field.count; i++) {
		const struct glass_poly *p = &field.shard[i].poly;
		int k;

		area += poly_area_exact(p);

		/*
		 * Every cell has to be convex with its centroid inside it.
		 * Shards are drawn as a triangle fan from the centroid, so a
		 * cell that folded over - or one whose centroid escaped it -
		 * would be rendered as a different shape than the one measured
		 * here, and the two readings above would both still pass.
		 */
		for (k = 0; k < p->n; k++) {
			int j = (k + 1) % p->n;
			int l = (k + 2) % p->n;
			float ax = p->x[j] - p->x[k];
			float ay = p->y[j] - p->y[k];
			float bx = p->x[l] - p->x[j];
			float by = p->y[l] - p->y[j];

			/* wound counter-clockwise, so every turn is one way */
			if (ax * by - ay * bx < -0.01f)
				concave++;
		}

		/*
		 * Outside, not merely close to an edge. Clipping leaves the odd
		 * hair-thin sliver where a cell barely catches the canvas, and
		 * the centroid of one of those is legitimately a hundredth of a
		 * pixel from its own boundary. Its fan is degenerate rather
		 * than wrong, and it covers a fraction of a pixel either way.
		 */
		if (poly_sdf(p, field.shard[i].cx, field.shard[i].cy) > 0.01f)
			off_centre++;
	}

	check(concave == 0, "[%s] %d corners of the break turn the wrong way - a cell is not convex", ctx, concave);
	check(off_centre == 0, "[%s] %d shards have their fan centre outside their own cell", ctx, off_centre);

	/*
	 * Cells below GLASS_MIN_AREA are dropped, so the total may come up a
	 * shade short - but only by what those slivers could have been worth.
	 * It may never come out over: that would be double coverage.
	 *
	 * The slack either way is a fraction of one average shard. The cuts are
	 * computed in single precision and two cells meeting along one arrive
	 * at it by different routes, so the shared corner lands a rounding step
	 * apart and the areas do not add up to the last decimal. What that is
	 * worth over a whole canvas is thousands of times smaller than one
	 * shard, which is the smallest thing that can actually go missing.
	 */
	check(area <= canvas + canvas / (double)field.count * 0.25 + 1.0,
	      "[%s] the shards cover %.2f of a %.2f canvas - more than all of it", ctx, area, canvas);
	check(area >= canvas - (double)field.count * 0.35 - 1.0, "[%s] the shards cover only %.2f of a %.2f canvas",
	      ctx, area, canvas);

	/*
	 * A lattice of sample points, offset by an irrational-ish fraction so
	 * the points do not land on the cuts of any of the three patterns.
	 */
	for (sy = 0; sy < 53; sy++) {
		for (sx = 0; sx < 71; sx++) {
			float px = ((float)sx + 0.31830989f) / 71.0f * cx;
			float py = ((float)sy + 0.61803399f) / 53.0f * cy;
			int strict = 0;
			int loose = 0;

			for (i = 0; i < field.count; i++) {
				float d = poly_sdf(&field.shard[i].poly, px, py);

				if (d < -0.01f)
					strict++;
				if (d < 0.01f)
					loose++;
			}

			if (loose == 0)
				gaps++;
			if (strict > 1) {
				overlaps++;
				if (strict > worst_overlap)
					worst_overlap = strict;
			}
		}
	}

	check(gaps == 0, "[%s] %d sample points are in no shard at all", ctx, gaps);
	check(overlaps == 0, "[%s] %d sample points are strictly inside up to %d shards at once", ctx, overlaps,
	      worst_overlap);

	glass_field_free(&field);
}

/*
 * The seam bleed only grows a shard.
 *
 * It is what stops a hairline of the incoming scene showing between two shards
 * that have not moved yet, and it works by letting neighbours overlap slightly.
 * A bleed that moved a corner inwards anywhere would open the very gap it is
 * there to close, so every emitted corner has to sit at least as far from the
 * centroid as the corner it came from.
 */
static void check_seam_bleed(int pattern, uint32_t seed)
{
	struct glass_build build = {1280.0f, 720.0f, 640.0f, 360.0f, 96.0f, pattern, seed};
	struct glass_field field;
	struct glass_vertex *verts;
	uint32_t *indices;
	uint32_t at = 0;
	int shrunk = 0;
	int off_centre = 0;
	int i;

	if (!glass_field_build(&field, &build)) {
		check(false, "[%s bleed] the field would not build", pattern_name(pattern));
		return;
	}

	verts = malloc(glass_vertex_count(&field) * sizeof(*verts));
	indices = malloc(glass_index_count(&field) * sizeof(*indices));
	if (!verts || !indices) {
		check(false, "out of memory");
		free(verts);
		free(indices);
		glass_field_free(&field);
		return;
	}

	glass_emit(&field, verts, indices, NULL);

	for (i = 0; i < field.count; i++) {
		const struct glass_shard *s = &field.shard[i];
		int k;

		/* the fan centre comes first, and has to be the pivot the
		 * shader turns the shard about */
		if (fabsf(verts[at].x - s->cx) > 1e-3f || fabsf(verts[at].y - s->cy) > 1e-3f)
			off_centre++;
		if (verts[at].core != 1.0f)
			off_centre++;
		at++;

		for (k = 0; k < s->poly.n; k++) {
			float was = hypotf(s->poly.x[k] - s->cx, s->poly.y[k] - s->cy);
			float now = hypotf(verts[at].x - s->cx, verts[at].y - s->cy);

			if (now < was - 1e-3f)
				shrunk++;
			if (verts[at].core != 0.0f)
				off_centre++;

			/* the scene coordinate has to follow the grown corner,
			 * or the overlap would sample the wrong texel and show
			 * as a seam of its own */
			if (fabsf(verts[at].u * build.canvas_cx - verts[at].x) > 1e-2f ||
			    fabsf(verts[at].v * build.canvas_cy - verts[at].y) > 1e-2f)
				off_centre++;
			at++;
		}
	}

	check(shrunk == 0, "[%s bleed] %d corners were pulled inwards rather than pushed out", pattern_name(pattern),
	      shrunk);
	check(off_centre == 0, "[%s bleed] %d emitted vertices carry the wrong pivot, ramp or coordinate",
	      pattern_name(pattern), off_centre);
	check(at == (uint32_t)glass_vertex_count(&field), "[%s bleed] emitted %u vertices, counted %zu",
	      pattern_name(pattern), at, glass_vertex_count(&field));

	free(verts);
	free(indices);
	glass_field_free(&field);
}

/* ------------------------------------------------------------------ */
/* the flight                                                         */
/* ------------------------------------------------------------------ */

/* The crack phase the checks below are written around. */
#define TEST_CRACK_END 0.30f

/* Moves the transition clock, and with it the crack front - which the source
 * works out once a frame rather than per shard, so the test has to as well. */
static void motion_at(struct glass_motion *m, float progress, int easing)
{
	m->progress = progress;
	m->crack_front = glass_crack_front(progress, TEST_CRACK_END, easing);
}

static struct glass_motion base_motion(int style, float reach, float ix, float iy)
{
	struct glass_motion m;

	memset(&m, 0, sizeof(m));
	m.style = style;
	m.hold_end = 0.45f;
	m.gravity_x = 0.0f;
	m.gravity_y = 1.0f;
	m.gravity = 0.8f;
	m.throw_px = 0.35f;
	m.spin = 0.7f;
	m.scatter = 0.5f;
	m.reach = reach;
	m.impact_x = ix;
	m.impact_y = iy;
	m.flat = false;
	m.arriving = false;

	return m;
}

/*
 * The two frames that are not allowed to be interesting.
 *
 * OBS shows the transition's first and last frame as the boundary between two
 * scenes, so anything moving, lit or faded on either one is a visible glitch at
 * the join. Every setting has to arrive at the same two frames.
 */
static void check_endpoints(int pattern, int style, bool flat, bool arriving, float scatter, float spin)
{
	struct glass_build build = {1920.0f, 1080.0f, 300.0f, 820.0f, 80.0f, pattern, 7u};
	struct glass_field field;
	struct glass_motion motion;
	char ctx[192];
	int at_rest = 0;
	int unfinished = 0;
	int uncracked = 0;
	int still_lit = 0;
	int i;

	if (!glass_field_build(&field, &build)) {
		check(false, "the field would not build");
		return;
	}

	snprintf(ctx, sizeof(ctx), "%s/%s%s%s scatter %.1f spin %.1f", pattern_name(pattern), style_name(style),
		 flat ? "/flat" : "", arriving ? "/arriving" : "", (double)scatter, (double)spin);

	motion = base_motion(style, field.reach, build.impact_x, build.impact_y);
	motion.flat = flat;
	motion.arriving = arriving;
	motion.scatter = scatter;
	motion.spin = spin;

	/* the first frame: the pane is whole */
	motion_at(&motion, 0.0f, GLASS_EASE_LINEAR);
	for (i = 0; i < field.count; i++) {
		struct glass_xform x;

		glass_shard_xform(&motion, &field.shard[i], &x);

		if (arriving) {
			/* an arriving shard has not turned up yet, so what it
			 * owes the first frame is to be invisible */
			if (x.alpha > 0.001f)
				at_rest++;
			continue;
		}

		if (fabsf(x.ox) > 1e-3f || fabsf(x.oy) > 1e-3f || fabsf(x.oz) > 1e-3f || fabsf(x.angle) > 1e-5f ||
		    x.alpha < 0.999f || x.crack > 1e-5f || x.reveal > 1e-5f)
			at_rest++;
	}
	check(at_rest == 0, "[%s] %d shards are not seated and clean on the first frame", ctx, at_rest);

	/* the last frame: the pane has gone, or has landed */
	motion_at(&motion, 1.0f, GLASS_EASE_LINEAR);
	for (i = 0; i < field.count; i++) {
		struct glass_xform x;

		glass_shard_xform(&motion, &field.shard[i], &x);

		if (x.motion < 0.999f)
			unfinished++;

		/*
		 * No lit glass on the last frame, whatever is still on screen.
		 * A shard that has left has faded out; one that stayed - the
		 * wipe never moves, and the assemble lands its shards - has to
		 * have put its cracks and its bevel away instead, or the
		 * incoming scene would arrive with the break still drawn on it.
		 */
		if (x.crack * x.settle * x.alpha > 1e-4f)
			still_lit++;

		if (style == GLASS_STYLE_WIPE) {
			if (x.reveal < 0.999f)
				at_rest++;
			continue;
		}

		if (arriving) {
			/* landed: seated exactly where it started, and opaque */
			if (fabsf(x.ox) > 1e-3f || fabsf(x.oy) > 1e-3f || fabsf(x.oz) > 1e-3f ||
			    fabsf(x.angle) > 1e-5f || x.alpha < 0.999f)
				at_rest++;
		} else if (x.alpha > 0.001f) {
			at_rest++;
		}
	}
	check(unfinished == 0, "[%s] %d shards had not finished their flight when the transition ended", ctx,
	      unfinished);
	check(still_lit == 0, "[%s] %d shards were still lit as broken glass on the last frame", ctx, still_lit);
	check(at_rest == 0, "[%s] %d shards are wrong on the last frame", ctx, at_rest);

	/* and the crack phase really does finish cracking the pane */
	motion_at(&motion, TEST_CRACK_END, GLASS_EASE_LINEAR);
	for (i = 0; i < field.count; i++) {
		struct glass_xform x;

		glass_shard_xform(&motion, &field.shard[i], &x);
		if (x.crack < 0.999f)
			uncracked++;
	}
	check(uncracked == 0, "[%s] %d shards were still uncracked when the crack phase ended", ctx, uncracked);

	glass_field_free(&field);
}

/*
 * A shard may not jump. The transform is sampled along the whole transition and
 * the step between frames is held to a bound: a discontinuity here would read
 * as a shard teleporting, which is the failure the phase splits and the per
 * shard launch delay are most likely to produce.
 */
static void check_continuity(int pattern, int style)
{
	struct glass_build build = {1280.0f, 720.0f, 640.0f, 360.0f, 110.0f, pattern, 3u};
	struct glass_field field;
	struct glass_motion motion;
	char ctx[128];
	float worst = 0.0f;
	int i;
	int f;

	if (!glass_field_build(&field, &build)) {
		check(false, "the field would not build");
		return;
	}

	snprintf(ctx, sizeof(ctx), "%s/%s", pattern_name(pattern), style_name(style));
	motion = base_motion(style, field.reach, build.impact_x, build.impact_y);

	for (i = 0; i < field.count; i++) {
		struct glass_xform prev;

		motion_at(&motion, 0.0f, GLASS_EASE_LINEAR);
		glass_shard_xform(&motion, &field.shard[i], &prev);

		for (f = 1; f <= 240; f++) {
			struct glass_xform now;
			float step;

			motion_at(&motion, (float)f / 240.0f, GLASS_EASE_LINEAR);
			glass_shard_xform(&motion, &field.shard[i], &now);

			step = hypotf(now.ox - prev.ox, now.oy - prev.oy) / field.reach;
			if (step > worst)
				worst = step;

			prev = now;
		}
	}

	/* a quarter of the reach in one frame of a 240 frame transition is far
	 * more than the motion ever asks for, and far less than a jump */
	check(worst < 0.25f, "[%s] a shard moved %.3f of the reach between two frames", ctx, (double)worst);

	glass_field_free(&field);
}

/* ------------------------------------------------------------------ */
/* housekeeping                                                       */
/* ------------------------------------------------------------------ */

/*
 * The crack front, over all three easings.
 *
 * Easing shapes the order the shards crack in, not the transition clock, so
 * whichever curve is picked the front still starts at nothing and still has the
 * whole pane broken by the time the crack phase ends. Only the way it gets
 * there changes.
 */
static void check_crack_front(void)
{
	int easing;

	for (easing = GLASS_EASE_LINEAR; easing <= GLASS_EASE_OUT; easing++) {
		float prev = -1.0f;
		int backwards = 0;
		int f;

		check(glass_crack_front(0.0f, TEST_CRACK_END, easing) <= 1e-6f,
		      "[easing %d] the pane is already cracking on the first frame", easing);

		/* past 1 by the width of the front, so the last shard has
		 * finished cracking rather than just started */
		check(glass_crack_front(TEST_CRACK_END, TEST_CRACK_END, easing) >= 1.0f + GLASS_CRACK_EDGE - 1e-5f,
		      "[easing %d] the crack phase ended with the pane not fully broken", easing);

		for (f = 0; f <= 200; f++) {
			float now = glass_crack_front((float)f / 200.0f, TEST_CRACK_END, easing);

			if (now < prev - 1e-6f)
				backwards++;
			prev = now;
		}

		check(backwards == 0, "[easing %d] the crack front went backwards %d times", easing, backwards);
	}
}

/* The mesh is rebuilt on the graphics thread, so the source has to be able to
 * tell cheaply whether a settings change needs one. */
static void check_staleness(void)
{
	struct glass_build build = {1280.0f, 720.0f, 100.0f, 200.0f, 64.0f, GLASS_PATTERN_WEB, 11u};
	struct glass_build other;
	struct glass_field field;

	if (!glass_field_build(&field, &build)) {
		check(false, "the field would not build");
		return;
	}

	check(!glass_field_stale(&field, &build), "an unchanged build was called stale");

	other = build;
	other.shard_px = 65.0f;
	check(glass_field_stale(&field, &other), "a changed shard size was not called stale");

	other = build;
	other.impact_x = 101.0f;
	check(glass_field_stale(&field, &other), "a moved impact was not called stale");

	other = build;
	other.pattern = GLASS_PATTERN_ICE;
	check(glass_field_stale(&field, &other), "a changed pattern was not called stale");

	other = build;
	other.canvas_cy = 1080.0f;
	check(glass_field_stale(&field, &other), "a resized canvas was not called stale");

	glass_field_free(&field);
}

/* The same settings have to give the same break, or a transition would look
 * different every time it played. */
static void check_determinism(int pattern)
{
	struct glass_build build = {1920.0f, 1080.0f, 960.0f, 540.0f, 70.0f, pattern, 5u};
	struct glass_field a;
	struct glass_field b;
	int differ = 0;
	int i;

	if (!glass_field_build(&a, &build) || !glass_field_build(&b, &build)) {
		check(false, "the field would not build");
		return;
	}

	if (a.count != b.count) {
		check(false, "[%s] two builds of one setting gave %d and %d shards", pattern_name(pattern), a.count,
		      b.count);
	} else {
		for (i = 0; i < a.count; i++) {
			if (a.shard[i].poly.n != b.shard[i].poly.n || a.shard[i].cx != b.shard[i].cx ||
			    a.shard[i].cy != b.shard[i].cy || a.shard[i].rnd[0] != b.shard[i].rnd[0])
				differ++;
		}
		check(differ == 0, "[%s] %d shards differed between two builds of one setting", pattern_name(pattern),
		      differ);
	}

	glass_field_free(&a);
	glass_field_free(&b);
}

/*
 * The shard cap is a ceiling on the mesh, not on the break: asking for shards
 * far too small to budget for has to give fewer, larger shards that still cover
 * the canvas, never a partial break.
 */
static void check_shard_cap(int pattern)
{
	struct glass_build build = {3840.0f, 2160.0f, 1920.0f, 1080.0f, 4.0f, pattern, 2u};
	struct glass_field field;
	double area = 0.0;
	int i;

	if (!glass_field_build(&field, &build)) {
		check(false, "[%s cap] the field would not build", pattern_name(pattern));
		return;
	}

	check(field.count <= GLASS_MAX_SHARDS, "[%s cap] %d shards is over the %d ceiling", pattern_name(pattern),
	      field.count, GLASS_MAX_SHARDS);
	check(field.shard_px > build.shard_px, "[%s cap] the shard size was not grown to fit the ceiling",
	      pattern_name(pattern));

	for (i = 0; i < field.count; i++)
		area += poly_area_exact(&field.shard[i].poly);

	check(area >= 3840.0 * 2160.0 - (double)field.count * 0.35 - 1.0,
	      "[%s cap] a capped break covers only %.0f of %.0f", pattern_name(pattern), area, 3840.0 * 2160.0);

	glass_field_free(&field);
}

int main(void)
{
	const float sizes[] = {28.0f, 64.0f, 150.0f};
	const float impacts[][2] = {
		{0.5f, 0.5f},  /* the middle */
		{0.0f, 0.0f},  /* a corner */
		{0.17f, 0.9f}, /* off to one side */
		{-0.4f, 0.3f}, /* outside the frame, which the sliders allow */
		{1.35f, 1.2f},
	};
	const float canvases[][2] = {{1920.0f, 1080.0f}, {640.0f, 480.0f}, {1080.0f, 1920.0f}};
	int pattern;
	int style;
	size_t s;
	size_t c;
	size_t p;

	for (pattern = 0; pattern <= GLASS_PATTERN_ICE; pattern++) {
		for (c = 0; c < sizeof(canvases) / sizeof(canvases[0]); c++) {
			for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
				for (p = 0; p < sizeof(impacts) / sizeof(impacts[0]); p++) {
					float cx = canvases[c][0];
					float cy = canvases[c][1];

					check_partition(pattern, cx, cy, sizes[s], impacts[p][0] * cx,
							impacts[p][1] * cy, 1u + (uint32_t)(s * 7 + p));
				}
			}
		}

		check_seam_bleed(pattern, 13u);
		check_determinism(pattern);
		check_shard_cap(pattern);

		for (style = 0; style <= GLASS_STYLE_ASSEMBLE; style++) {
			check_endpoints(pattern, style, false, false, 0.0f, 0.0f);
			check_endpoints(pattern, style, false, false, 1.0f, 2.0f);
			check_endpoints(pattern, style, true, false, 0.5f, 1.0f);

			/* Arriving runs the flight backwards so the shard lands
			 * on the last frame. Nothing flies in the style where
			 * nothing moves, so there is no backwards to run. */
			if (style != GLASS_STYLE_WIPE)
				check_endpoints(pattern, style, false, true, 0.5f, 1.0f);

			check_continuity(pattern, style);
		}
	}

	check_crack_front();
	check_staleness();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
