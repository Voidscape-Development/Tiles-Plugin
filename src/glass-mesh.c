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
 * Breaking the canvas into shards, and flying them.
 *
 * All three patterns are built the same way: start from a convex region and cut
 * it with half planes. A convex polygon cut by a half plane is a convex polygon,
 * and two cells that share a cut share the line it was computed from, so the
 * result is an exact partition of the canvas - no gap for the incoming scene to
 * show through before the shards have moved, and no double coverage to blend
 * wrongly once they have. That property is what test/test-glass.c checks first,
 * because everything else rests on it.
 *
 * It also means a shard is always convex, so it fans into triangles from its own
 * centroid with no triangulation to speak of.
 */

#include "glass-mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GLASS_TAU 6.2831853071795864769f

/* Cells smaller than this are dropped rather than emitted: they are the slivers
 * a clip leaves when a cell barely touches the canvas, and at this size they
 * cannot show a pixel that its neighbours do not already cover. */
#define GLASS_MIN_AREA 0.35f

/* The web's rings step out by this ratio, and each one is jittered by this much
 * of its own radius. The jitter is held under the gap the growth leaves, so
 * radii stay in order along a spoke and cells never fold over. */
#define WEB_RING_GROWTH 1.32f
#define WEB_RING_JITTER 0.12f

/* How far a spoke may be knocked off even spacing, as a share of the spacing.
 * Two neighbouring spokes can lean apart by twice this, which is what widens
 * the worst wedge web_outer() has to reach across. */
#define WEB_SPOKE_JITTER 0.18f

/*
 * Cutting a rectangle down to a target area does not land on area/target
 * cells: the cuts are deliberately off centre, so pieces come out uneven and
 * the small ones stop splitting early. This is what that overshoot measures,
 * and it only has to be close - the split loop below stops on the shard budget
 * regardless, and the size search only needs to get near enough that it does
 * not have to.
 */
#define ICE_SPLIT_OVERSHOOT 1.45f

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline float saturatef(float v)
{
	return clampf(v, 0.0f, 1.0f);
}

static inline float smoothstepf(float lo, float hi, float v)
{
	float k = saturatef((v - lo) / (hi - lo > 1e-6f ? hi - lo : 1e-6f));

	return k * k * (3.0f - 2.0f * k);
}

/* ------------------------------------------------------------------ */
/* hashes                                                             */
/* ------------------------------------------------------------------ */

/*
 * Integer hashing rather than the usual sin()/frac() trick. The shards are
 * built once on the CPU and never have to agree with a hash in the shader, and
 * an integer mix has no platform-dependent transcendental in it, so a given
 * seed gives the same break everywhere - which is what lets the tests pin
 * specific breaks.
 */
static inline uint32_t hash_mix(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
	return hash_mix(hash_mix(hash_mix(a) ^ (b * 0x9e3779b9U)) ^ (c * 0x85ebca6bU));
}

/* 0..1 */
static inline float hashf(uint32_t a, uint32_t b, uint32_t c)
{
	return (float)(hash3(a, b, c) >> 8) * (1.0f / 16777216.0f);
}

/* -1..1 */
static inline float hashs(uint32_t a, uint32_t b, uint32_t c)
{
	return hashf(a, b, c) * 2.0f - 1.0f;
}

/* ------------------------------------------------------------------ */
/* polygons                                                           */
/* ------------------------------------------------------------------ */

/*
 * Area and centroid, both measured from the cell's own first corner rather than
 * from the canvas origin.
 *
 * The usual shoelace sum over absolute coordinates multiplies pixel positions
 * together, so on a 4K canvas the terms run to eight digits while the answer
 * for a small cell is two, and single precision has nothing left over. A cell
 * clipped down to a hair against the edge of the frame is the worst case: the
 * cancellation there was enough to put the computed centroid over a pixel
 * outside its own cell, which the triangle fan would then have turned inside
 * out. Shifting to the first corner drops the magnitudes to the size of the
 * cell and the problem with them.
 */
static float poly_area(const struct glass_poly *p)
{
	float sum = 0.0f;
	int i;

	if (p->n < 3)
		return 0.0f;

	for (i = 1; i + 1 < p->n; i++) {
		float ax = p->x[i] - p->x[0];
		float ay = p->y[i] - p->y[0];
		float bx = p->x[i + 1] - p->x[0];
		float by = p->y[i + 1] - p->y[0];

		sum += ax * by - ay * bx;
	}

	return sum * 0.5f;
}

/* Falls back to the vertex average for a polygon with no area left, which only
 * happens for the slivers that are about to be dropped. */
static void poly_centroid(const struct glass_poly *p, float *cx, float *cy)
{
	float ox;
	float oy;
	float area = 0.0f;
	float ax = 0.0f;
	float ay = 0.0f;
	int i;

	if (p->n < 1) {
		*cx = 0.0f;
		*cy = 0.0f;
		return;
	}

	ox = p->x[0];
	oy = p->y[0];

	for (i = 0; i < p->n; i++) {
		int j = (i + 1) % p->n;
		float xi = p->x[i] - ox;
		float yi = p->y[i] - oy;
		float xj = p->x[j] - ox;
		float yj = p->y[j] - oy;
		float cross = xi * yj - xj * yi;

		area += cross;
		ax += (xi + xj) * cross;
		ay += (yi + yj) * cross;
	}

	if (fabsf(area) < 1e-9f) {
		float sx = 0.0f;
		float sy = 0.0f;

		for (i = 0; i < p->n; i++) {
			sx += p->x[i] - ox;
			sy += p->y[i] - oy;
		}

		*cx = ox + sx / (float)p->n;
		*cy = oy + sy / (float)p->n;
		return;
	}

	*cx = ox + ax / (3.0f * area);
	*cy = oy + ay / (3.0f * area);
}

/* Distance from a point inside a convex polygon to its nearest edge. This is
 * how far the rim light has to travel to reach the middle of the shard. */
static float poly_inradius(const struct glass_poly *p, float cx, float cy)
{
	float best = 1e30f;
	int i;

	for (i = 0; i < p->n; i++) {
		int j = (i + 1) % p->n;
		float ex = p->x[j] - p->x[i];
		float ey = p->y[j] - p->y[i];
		float len = sqrtf(ex * ex + ey * ey);
		float d;

		if (len < 1e-6f)
			continue;

		/* perpendicular distance from the centroid to the edge line */
		d = fabsf((cx - p->x[i]) * ey - (cy - p->y[i]) * ex) / len;
		if (d < best)
			best = d;
	}

	return best < 1e30f ? best : 0.0f;
}

static void poly_wind_ccw(struct glass_poly *p)
{
	int i;

	if (poly_area(p) >= 0.0f)
		return;

	for (i = 0; i < p->n / 2; i++) {
		int j = p->n - 1 - i;
		float tx = p->x[i];
		float ty = p->y[i];

		p->x[i] = p->x[j];
		p->y[i] = p->y[j];
		p->x[j] = tx;
		p->y[j] = ty;
	}
}

/*
 * Sutherland-Hodgman against one half plane: keeps a*x + b*y <= c.
 *
 * Every cut a cell takes goes through here, whichever pattern asked for it, so
 * two cells that share a boundary are trimmed by the same line with the same
 * arithmetic and meet exactly.
 */
static void poly_clip(struct glass_poly *p, float a, float b, float c)
{
	struct glass_poly out;
	int i;

	out.n = 0;

	for (i = 0; i < p->n; i++) {
		int j = (i + 1) % p->n;
		float di = a * p->x[i] + b * p->y[i] - c;
		float dj = a * p->x[j] + b * p->y[j] - c;

		if (di <= 0.0f && out.n < GLASS_MAX_POLY) {
			out.x[out.n] = p->x[i];
			out.y[out.n] = p->y[i];
			out.n++;
		}

		/* the edge crosses the plane: add the crossing point */
		if ((di < 0.0f && dj > 0.0f) || (di > 0.0f && dj < 0.0f)) {
			float k = di / (di - dj);

			if (out.n < GLASS_MAX_POLY) {
				out.x[out.n] = p->x[i] + (p->x[j] - p->x[i]) * k;
				out.y[out.n] = p->y[i] + (p->y[j] - p->y[i]) * k;
				out.n++;
			}
		}
	}

	*p = out;
}

/* Keeps whichever side of the bisector of `own` and `other` is nearer `own`.
 * |p - own|^2 <= |p - other|^2 rearranges to a plane, so the Voronoi cell is
 * just a run of these. */
static void poly_clip_bisector(struct glass_poly *p, float ownx, float owny, float ox, float oy)
{
	float a = 2.0f * (ox - ownx);
	float b = 2.0f * (oy - owny);
	float c = ox * ox + oy * oy - ownx * ownx - owny * owny;

	poly_clip(p, a, b, c);
}

static void poly_rect(struct glass_poly *p, float x0, float y0, float x1, float y1)
{
	p->n = 4;
	p->x[0] = x0;
	p->y[0] = y0;
	p->x[1] = x1;
	p->y[1] = y0;
	p->x[2] = x1;
	p->y[2] = y1;
	p->x[3] = x0;
	p->y[3] = y1;
}

/* ------------------------------------------------------------------ */
/* building the field                                                 */
/* ------------------------------------------------------------------ */

struct glass_builder {
	struct glass_field *field;
	int cap;
	float canvas_cx;
	float canvas_cy;
	uint32_t seed;
};

/*
 * Takes a finished cell. Everything the patterns produce arrives here, so this
 * is the single place a cell is measured, wound, given its randoms and dropped
 * if it is a sliver.
 */
static void builder_add(struct glass_builder *bld, struct glass_poly *poly)
{
	struct glass_shard *shard;
	float area;
	uint32_t h;

	if (bld->field->count >= bld->cap || poly->n < 3)
		return;

	poly_wind_ccw(poly);
	area = poly_area(poly);
	if (area < GLASS_MIN_AREA)
		return;

	shard = &bld->field->shard[bld->field->count++];
	shard->poly = *poly;
	shard->area = area;
	poly_centroid(poly, &shard->cx, &shard->cy);
	shard->inner = poly_inradius(poly, shard->cx, shard->cy);

	/* Keyed off the centroid rather than the shard index: the randoms then
	 * belong to the place on the canvas, so a shard keeps its tumble and its
	 * tint when a neighbouring cell changes and shifts every index after it. */
	h = hash3((uint32_t)(int32_t)(shard->cx * 16.0f), (uint32_t)(int32_t)(shard->cy * 16.0f), bld->seed);
	shard->rnd[0] = (float)((h >> 8) & 0xFFFF) * (1.0f / 65535.0f);
	shard->rnd[1] = (float)(hash_mix(h ^ 0x51ed270bU) >> 16) * (1.0f / 65535.0f);
	shard->rnd[2] = (float)(hash_mix(h ^ 0x27d4eb2dU) >> 16) * (1.0f / 65535.0f);

	shard->launch = 0.0f; /* filled in once the whole field is known */
}

/*
 * How far out the web's outermost ring has to sit to carry the pattern past the
 * furthest corner.
 *
 * Two things eat into the radius and both have to be paid for, or the corners
 * of the canvas fall outside the last ring and are covered by nothing at all:
 *
 *   - the rings are polylines, not circles, so a ring of nominal radius r only
 *     reaches r * cos(half the widest wedge) where it is thinnest, midway
 *     between two spokes - and the spoke jitter can lean two neighbours apart,
 *     widening that wedge by twice WEB_SPOKE_JITTER;
 *   - the radial jitter can pull both ends of that chord in by
 *     WEB_RING_JITTER.
 */
static float web_outer(float reach, int spokes)
{
	float widest = (GLASS_TAU * 0.5f / (float)spokes) * (1.0f + 2.0f * WEB_SPOKE_JITTER);
	float thin = cosf(fminf(widest, GLASS_TAU * 0.24f));

	return reach * 1.02f / fmaxf(thin * (1.0f - WEB_RING_JITTER), 0.05f);
}

static int web_spokes(float reach, float shard_px)
{
	int spokes = (int)(GLASS_TAU * reach * 0.40f / fmaxf(shard_px, 1.0f));

	if (spokes < 8)
		spokes = 8;
	if (spokes > 240)
		spokes = 240;

	return spokes;
}

/*
 * Radial spokes crossed by concentric rings, which is what a struck pane
 * actually does.
 *
 * The rings are spaced geometrically, so the cells grow as they get further
 * from the impact - the fine splintering at the point of impact and the big
 * slabs out at the edges both fall out of that one ratio rather than being
 * special cased.
 *
 * Every cell is a quad between two neighbouring spokes and two neighbouring
 * rings, and its four corners are read out of a shared table. Two cells that
 * touch therefore use the same two corner points, so the jitter on the table
 * breaks the regularity up without opening a single gap: the radial cracks
 * wander, the rings are polylines rather than arcs, and the partition is still
 * exact.
 */
static void build_web(struct glass_builder *bld, const struct glass_build *b, float reach, float shard_px)
{
	float *ring_x;
	float *ring_y;
	float inner = fmaxf(shard_px * 0.30f, 1.0f);
	int spokes = web_spokes(reach, shard_px);
	float outer = web_outer(reach, spokes);
	int rings = 0;
	float r = inner;
	int k;
	int s;

	while (r < outer && rings < 96) {
		r *= WEB_RING_GROWTH;
		rings++;
	}
	rings++; /* the ring table has one more entry than it has gaps */

	ring_x = malloc((size_t)rings * (size_t)spokes * sizeof(float));
	ring_y = malloc((size_t)rings * (size_t)spokes * sizeof(float));
	if (!ring_x || !ring_y) {
		free(ring_x);
		free(ring_y);
		return;
	}

	for (k = 0; k < rings; k++) {
		float base = inner * powf(WEB_RING_GROWTH, (float)k);

		for (s = 0; s < spokes; s++) {
			/*
			 * The angle belongs to the spoke and the radius to the
			 * crossing, which is what keeps every cell convex.
			 *
			 * A cell is the piece of the wedge between two spokes
			 * that lies between two ring chords. With both spokes
			 * dead straight, that is the intersection of four half
			 * planes - two rays and two chords - so it is convex by
			 * construction however the radii are jittered, as long
			 * as the outer ring stays outside the inner one on both
			 * spokes. WEB_RING_JITTER is held under the gap the
			 * growth ratio leaves, so it does.
			 *
			 * Convexity is not cosmetic: each shard is drawn as a
			 * fan from its own centroid, and a centroid that fell
			 * outside its cell would fan over the wrong triangles
			 * and tear a hole in the pane. Straight radial cracks
			 * are also what a struck pane actually does, so nothing
			 * is lost by it - the break is broken up by the spoke
			 * spacing and the wandering rings instead.
			 */
			float ang = (GLASS_TAU / (float)spokes) *
				    ((float)s + WEB_SPOKE_JITTER * hashs(0u, (uint32_t)s, bld->seed));
			float rad = base * (1.0f + WEB_RING_JITTER * hashs((uint32_t)k, (uint32_t)s + 977u, bld->seed));
			size_t at = (size_t)k * (size_t)spokes + (size_t)s;

			ring_x[at] = b->impact_x + cosf(ang) * rad;
			ring_y[at] = b->impact_y + sinf(ang) * rad;
		}
	}

	/* The impact itself: a ring of slivers round the point of the strike,
	 * where real glass is pulverised rather than cracked. */
	for (s = 0; s < spokes; s++) {
		int s2 = (s + 1) % spokes;
		struct glass_poly poly;

		poly.n = 3;
		poly.x[0] = b->impact_x;
		poly.y[0] = b->impact_y;
		poly.x[1] = ring_x[s];
		poly.y[1] = ring_y[s];
		poly.x[2] = ring_x[s2];
		poly.y[2] = ring_y[s2];

		poly_clip(&poly, -1.0f, 0.0f, 0.0f);
		poly_clip(&poly, 1.0f, 0.0f, bld->canvas_cx);
		poly_clip(&poly, 0.0f, -1.0f, 0.0f);
		poly_clip(&poly, 0.0f, 1.0f, bld->canvas_cy);
		builder_add(bld, &poly);
	}

	for (k = 0; k + 1 < rings; k++) {
		for (s = 0; s < spokes; s++) {
			int s2 = (s + 1) % spokes;
			size_t a = (size_t)k * (size_t)spokes;
			size_t c = (size_t)(k + 1) * (size_t)spokes;
			struct glass_poly poly;

			poly.n = 4;
			poly.x[0] = ring_x[a + (size_t)s];
			poly.y[0] = ring_y[a + (size_t)s];
			poly.x[1] = ring_x[a + (size_t)s2];
			poly.y[1] = ring_y[a + (size_t)s2];
			poly.x[2] = ring_x[c + (size_t)s2];
			poly.y[2] = ring_y[c + (size_t)s2];
			poly.x[3] = ring_x[c + (size_t)s];
			poly.y[3] = ring_y[c + (size_t)s];

			poly_clip(&poly, -1.0f, 0.0f, 0.0f);
			poly_clip(&poly, 1.0f, 0.0f, bld->canvas_cx);
			poly_clip(&poly, 0.0f, -1.0f, 0.0f);
			poly_clip(&poly, 0.0f, 1.0f, bld->canvas_cy);
			builder_add(bld, &poly);
		}
	}

	free(ring_x);
	free(ring_y);
}

/* The seed of one lattice cell, jittered inside its own cell so no seed ever
 * crosses into a neighbour's - which is what bounds how far a cell can reach
 * and lets the neighbour search below stop at two rings. */
static void voronoi_seed(uint32_t seed, int i, int j, float spacing, float *sx, float *sy)
{
	*sx = ((float)i + 0.5f + 0.42f * hashs((uint32_t)(int32_t)i, (uint32_t)(int32_t)j, seed)) * spacing;
	*sy = ((float)j + 0.5f + 0.42f * hashs((uint32_t)(int32_t)i, (uint32_t)(int32_t)j + 4177u, seed)) * spacing;
}

/*
 * Voronoi cells of a jittered lattice: irregular angular shards with no focal
 * point, the same construction the Shatter tile shape uses, but resolved into
 * real polygons here rather than sampled per pixel.
 */
static void build_voronoi(struct glass_builder *bld, float shard_px)
{
	float spacing = fmaxf(shard_px, 1.0f);
	int nx = (int)(bld->canvas_cx / spacing) + 3;
	int ny = (int)(bld->canvas_cy / spacing) + 3;
	int i;
	int j;

	for (j = -2; j < ny; j++) {
		for (i = -2; i < nx; i++) {
			struct glass_poly poly;
			float sx;
			float sy;
			int dj;

			voronoi_seed(bld->seed, i, j, spacing, &sx, &sy);
			poly_rect(&poly, 0.0f, 0.0f, bld->canvas_cx, bld->canvas_cy);

			for (dj = -2; dj <= 2; dj++) {
				int di;

				for (di = -2; di <= 2; di++) {
					float ox;
					float oy;

					if (!di && !dj)
						continue;

					voronoi_seed(bld->seed, i + di, j + dj, spacing, &ox, &oy);
					poly_clip_bisector(&poly, sx, sy, ox, oy);
					if (poly.n < 3)
						break;
				}

				if (poly.n < 3)
					break;
			}

			builder_add(bld, &poly);
		}
	}
}

/*
 * Straight cracks, cut recursively.
 *
 * The canvas is split by a line, each half is split again, and so on until the
 * pieces are down to the wanted size. Nothing here is a lattice, so the shards
 * come out in a range of sizes and long splinters appear naturally.
 *
 * The cuts are steered by the impact: each one runs either away from it or
 * across it, which is enough to make the break read as having a source without
 * the pattern being radial the way the web is.
 */
static void build_ice(struct glass_builder *bld, const struct glass_build *b, float shard_px)
{
	struct glass_poly *queue;
	int cap = bld->cap * 2 + 8;
	int head = 0;
	int tail = 0;
	float target = shard_px * shard_px;
	uint32_t step = 0;

	queue = malloc((size_t)cap * sizeof(*queue));
	if (!queue)
		return;

	poly_rect(&queue[tail++], 0.0f, 0.0f, bld->canvas_cx, bld->canvas_cy);

	while (head < tail) {
		struct glass_poly poly = queue[head++];
		struct glass_poly left;
		struct glass_poly right;
		float cx;
		float cy;
		float px;
		float py;
		float ang;
		float radial;
		float na;
		float nb;
		float nc;
		float extent;

		/*
		 * Small enough, no room left to queue the halves, or splitting
		 * again would take the break past the shard budget. Either way
		 * it is a shard now.
		 *
		 * Stopping early has to cost detail and never coverage, which
		 * is why the budget is checked against what is still in the
		 * queue rather than against the shards already emitted: every
		 * piece waiting here is a piece of the canvas, and dropping one
		 * would leave a hole. Splitting adds exactly one cell, so
		 * knowing there is room for the queue plus one is enough.
		 */
		if (fabsf(poly_area(&poly)) <= target || tail + 2 > cap ||
		    bld->field->count + (tail - head) + 2 > bld->cap) {
			builder_add(bld, &poly);
			continue;
		}

		poly_centroid(&poly, &cx, &cy);
		extent = sqrtf(fabsf(poly_area(&poly)));
		step++;

		/* Cut near the middle, but not through it: splitting a piece
		 * exactly in half every time gives a suspiciously even break. */
		px = cx + extent * 0.22f * hashs(step, 1u, bld->seed);
		py = cy + extent * 0.22f * hashs(step, 2u, bld->seed);

		radial = atan2f(cy - b->impact_y, cx - b->impact_x);
		ang = radial + (hashf(step, 3u, bld->seed) < 0.55f ? 0.0f : GLASS_TAU * 0.25f);
		ang += 0.55f * hashs(step, 4u, bld->seed);

		/* the cut runs along `ang`, so the plane's normal is across it */
		na = -sinf(ang);
		nb = cosf(ang);
		nc = na * px + nb * py;

		left = poly;
		right = poly;
		poly_clip(&left, na, nb, nc);
		poly_clip(&right, -na, -nb, -nc);

		if (left.n < 3 || right.n < 3) {
			builder_add(bld, &poly);
			continue;
		}

		queue[tail++] = left;
		queue[tail++] = right;
	}

	free(queue);
}

/* Pixels from the impact to the furthest corner of the canvas. Every distance
 * in the transition is measured against this, so an impact pushed off the side
 * of the frame still has the break reach the far corner by the time it is
 * due. */
static float glass_reach(float ix, float iy, float cx, float cy)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float best = 0.0f;
	int i;

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - ix;
		float dy = corner_y[i] - iy;
		float r = sqrtf(dx * dx + dy * dy);

		if (r > best)
			best = r;
	}

	return fmaxf(best, 1.0f);
}

/* Roughly how many cells a pattern would produce at this shard size. Only used
 * to pick the size, so it does not have to be exact - the builder stops at the
 * cap regardless. */
static float estimate_cells(int pattern, float canvas_cx, float canvas_cy, float reach, float shard_px)
{
	float s = fmaxf(shard_px, 1.0f);

	if (pattern == GLASS_PATTERN_WEB) {
		int spokes = web_spokes(reach, s);
		float inner = fmaxf(s * 0.30f, 1.0f);
		float rings = logf(fmaxf(web_outer(reach, spokes) / inner, 1.0f)) / logf(WEB_RING_GROWTH) + 2.0f;

		return (float)spokes * rings;
	}

	if (pattern == GLASS_PATTERN_ICE)
		return canvas_cx * canvas_cy / (s * s) * ICE_SPLIT_OVERSHOOT;

	/* voronoi: one cell per lattice point, margin included */
	return (canvas_cx / s + 3.0f) * (canvas_cy / s + 3.0f);
}

bool glass_field_build(struct glass_field *field, const struct glass_build *build)
{
	struct glass_builder bld;
	float shard_px;
	float reach;
	int i;
	int guard;

	memset(field, 0, sizeof(*field));
	field->build = *build;

	if (build->canvas_cx < 1.0f || build->canvas_cy < 1.0f)
		return false;

	reach = glass_reach(build->impact_x, build->impact_y, build->canvas_cx, build->canvas_cy);
	shard_px = clampf(build->shard_px, 4.0f, 4096.0f);

	/*
	 * The cap, enforced by growing the shards rather than by dropping them.
	 * Dropping would leave a hole; growing just means a small size setting
	 * stops getting smaller once the canvas is as broken up as the mesh
	 * budget allows.
	 */
	for (guard = 0; guard < 256; guard++) {
		if (estimate_cells(build->pattern, build->canvas_cx, build->canvas_cy, reach, shard_px) <=
		    (float)GLASS_MAX_SHARDS)
			break;
		shard_px *= 1.06f;
	}

	field->shard = calloc(GLASS_MAX_SHARDS, sizeof(*field->shard));
	if (!field->shard)
		return false;

	field->reach = reach;
	field->shard_px = shard_px;

	bld.field = field;
	bld.cap = GLASS_MAX_SHARDS;
	bld.canvas_cx = build->canvas_cx;
	bld.canvas_cy = build->canvas_cy;
	bld.seed = build->seed ? build->seed : 1u;

	switch (build->pattern) {
	case GLASS_PATTERN_VORONOI:
		build_voronoi(&bld, shard_px);
		break;
	case GLASS_PATTERN_ICE:
		build_ice(&bld, build, shard_px);
		break;
	default:
		build_web(&bld, build, reach, shard_px);
		break;
	}

	/*
	 * When the crack front gets to each shard. Distance from the impact
	 * over the reach, so the last shard to crack is the one in the furthest
	 * corner and it cracks exactly as the crack phase ends, wherever the
	 * impact was put.
	 */
	for (i = 0; i < field->count; i++) {
		struct glass_shard *s = &field->shard[i];
		float dx = s->cx - build->impact_x;
		float dy = s->cy - build->impact_y;

		s->launch = saturatef(sqrtf(dx * dx + dy * dy) / reach);
	}

	if (field->count == 0) {
		glass_field_free(field);
		return false;
	}

	/*
	 * The shard array is sized for the ceiling because the patterns cannot
	 * say in advance how many cells they will produce, but a break is
	 * usually a fraction of that and the field is kept for as long as the
	 * transition exists. Hand the rest back.
	 */
	{
		struct glass_shard *fit = realloc(field->shard, (size_t)field->count * sizeof(*field->shard));

		if (fit)
			field->shard = fit;
	}

	return true;
}

void glass_field_free(struct glass_field *field)
{
	if (!field)
		return;

	free(field->shard);
	field->shard = NULL;
	field->count = 0;
}

bool glass_field_stale(const struct glass_field *field, const struct glass_build *build)
{
	const struct glass_build *had = &field->build;

	if (!field->shard || field->count == 0)
		return true;

	return had->canvas_cx != build->canvas_cx || had->canvas_cy != build->canvas_cy ||
	       had->impact_x != build->impact_x || had->impact_y != build->impact_y ||
	       had->shard_px != build->shard_px || had->pattern != build->pattern || had->seed != build->seed;
}

/* ------------------------------------------------------------------ */
/* triangles                                                          */
/* ------------------------------------------------------------------ */

size_t glass_vertex_count(const struct glass_field *field)
{
	size_t total = 0;
	int i;

	for (i = 0; i < field->count; i++)
		total += (size_t)field->shard[i].poly.n + 1; /* the rim, plus the centroid */

	return total;
}

size_t glass_index_count(const struct glass_field *field)
{
	size_t total = 0;
	int i;

	for (i = 0; i < field->count; i++)
		total += (size_t)field->shard[i].poly.n * 3;

	return total;
}

void glass_emit(const struct glass_field *field, struct glass_vertex *verts, uint32_t *indices,
		struct glass_span *spans)
{
	uint32_t vat = 0;
	uint32_t iat = 0;
	int i;

	for (i = 0; i < field->count; i++) {
		const struct glass_shard *s = &field->shard[i];
		uint32_t centre = vat;
		int n = s->poly.n;
		int k;

		verts[vat].x = s->cx;
		verts[vat].y = s->cy;
		verts[vat].u = s->cx / field->build.canvas_cx;
		verts[vat].v = s->cy / field->build.canvas_cy;
		verts[vat].cx = s->cx;
		verts[vat].cy = s->cy;
		verts[vat].core = 1.0f;
		verts[vat].launch = s->launch;
		verts[vat].rnd[0] = s->rnd[0];
		verts[vat].rnd[1] = s->rnd[1];
		verts[vat].rnd[2] = s->rnd[2];
		verts[vat].inner = s->inner;
		vat++;

		for (k = 0; k < n; k++) {
			float dx = s->poly.x[k] - s->cx;
			float dy = s->poly.y[k] - s->cy;
			float len = sqrtf(dx * dx + dy * dy);
			float grow = len > 1e-4f ? 1.0f + GLASS_SEAM_BLEED / len : 1.0f;
			float px = s->cx + dx * grow;
			float py = s->cy + dy * grow;

			verts[vat].x = px;
			verts[vat].y = py;

			/*
			 * The coordinate follows the grown corner rather than
			 * the original one, so every pixel a shard covers still
			 * samples the scene at its own position. That is what
			 * makes the overlap invisible at rest: where two shards
			 * both cover a pixel they fetch the same texel.
			 */
			verts[vat].u = px / field->build.canvas_cx;
			verts[vat].v = py / field->build.canvas_cy;
			verts[vat].cx = s->cx;
			verts[vat].cy = s->cy;
			verts[vat].core = 0.0f;
			verts[vat].launch = s->launch;
			verts[vat].rnd[0] = s->rnd[0];
			verts[vat].rnd[1] = s->rnd[1];
			verts[vat].rnd[2] = s->rnd[2];
			verts[vat].inner = s->inner;
			vat++;
		}

		if (spans) {
			spans[i].first = iat;
			spans[i].count = (uint32_t)n * 3;
		}

		for (k = 0; k < n; k++) {
			indices[iat++] = centre;
			indices[iat++] = centre + 1 + (uint32_t)k;
			indices[iat++] = centre + 1 + (uint32_t)((k + 1) % n);
		}
	}
}

/* ------------------------------------------------------------------ */
/* motion                                                             */
/* ------------------------------------------------------------------ */

float glass_crack_front(float progress, float crack_end, int easing)
{
	float d = saturatef(progress / fmaxf(crack_end, 1e-4f));

	if (easing == GLASS_EASE_IN_OUT)
		d = d * d * (3.0f - 2.0f * d);
	else if (easing == GLASS_EASE_OUT)
		d = 1.0f - (1.0f - d) * (1.0f - d);

	/*
	 * The front is run GLASS_CRACK_EDGE past the last shard, because a
	 * shard takes that much of the sweep to finish cracking. Without it the
	 * furthest corner would only be starting to break as the crack phase
	 * ended, and the shards would begin to fall through an unbroken pane.
	 */
	return d * (1.0f + GLASS_CRACK_EDGE);
}

/*
 * NOTE: mirrored by shard_xform() in data/effects/glass_transition.effect.
 * Keep the two in sync - the shader is what the viewer sees, and this is what
 * the depth sort and the tests believe.
 */
void glass_shard_xform(const struct glass_motion *motion, const struct glass_shard *shard, struct glass_xform *out)
{
	float order;
	float start;
	float m;
	float k;
	float dx;
	float dy;
	float len;
	float ang;
	float throw_dir_x;
	float throw_dir_y;
	float spin_turns;

	memset(out, 0, sizeof(*out));
	out->axis[2] = 1.0f;
	out->alpha = 1.0f;
	out->settle = 1.0f;

	/* Scatter blends the ordered break towards a shuffle, the same way the
	 * tiling transition's Order Randomness does. */
	order = saturatef(shard->launch + (shard->rnd[0] - shard->launch) * saturatef(motion->scatter) * 0.7f);

	out->crack = saturatef((motion->crack_front - order) / GLASS_CRACK_EDGE);

	/*
	 * Every shard's flight has to finish by the end of the transition, so
	 * the window it is given runs from wherever it starts to 1 rather than
	 * being a fixed length. The shards nearest the impact let go first;
	 * the last one to start still lands exactly on the final frame.
	 */
	start = motion->hold_end + (1.0f - motion->hold_end) * order * 0.30f;
	m = saturatef((motion->progress - start) / fmaxf(1.0f - start, 1e-4f));
	out->motion = m;

	if (motion->style == GLASS_STYLE_WIPE) {
		/* nothing moves: the shard changes scene where it stands, and
		 * the break goes with the scene it belonged to */
		out->reveal = smoothstepf(0.0f, 1.0f, m);
		out->settle = 1.0f - out->reveal;
		return;
	}

	/* Flight parameter: 0 is seated in the pane. An arriving shard runs it
	 * backwards, so it starts scattered and lands on the last frame. */
	k = motion->arriving ? 1.0f - m : m;

	out->alpha = 1.0f - smoothstepf(0.72f, 1.0f, k);

	/* An arriving shard is glass until it seats itself, and the scene
	 * afterwards. A leaving one is gone by then, so its alpha has already
	 * taken care of it. */
	if (motion->arriving)
		out->settle = smoothstepf(0.0f, 0.25f, k);

	dx = shard->cx - motion->impact_x;
	dy = shard->cy - motion->impact_y;
	len = sqrtf(dx * dx + dy * dy);

	if (len > 1e-3f) {
		throw_dir_x = dx / len;
		throw_dir_y = dy / len;
	} else {
		throw_dir_x = 0.0f;
		throw_dir_y = -1.0f;
	}

	/* scatter also bends each shard off its own radius, so the spray is not
	 * perfectly radial */
	ang = (shard->rnd[1] - 0.5f) * saturatef(motion->scatter) * 1.6f;
	{
		float cs = cosf(ang);
		float sn = sinf(ang);
		float tx = throw_dir_x * cs - throw_dir_y * sn;
		float ty = throw_dir_x * sn + throw_dir_y * cs;

		throw_dir_x = tx;
		throw_dir_y = ty;
	}

	/*
	 * Throw is an impulse and gravity is an acceleration, so one is linear
	 * in the flight and the other goes with its square. That is the whole
	 * of the ballistics, and it is enough: a shard leaves along its own
	 * radius and the fall takes over.
	 */
	{
		float spread = 0.55f + shard->rnd[2] * 0.9f;
		float fling = motion->throw_px * k * motion->reach * 0.85f * spread;
		float fall = motion->gravity * k * k * motion->reach * 1.25f;

		out->ox = throw_dir_x * fling + motion->gravity_x * fall;
		out->oy = throw_dir_y * fling + motion->gravity_y * fall;
	}

	if (motion->style == GLASS_STYLE_BLOW) {
		/* towards the camera, so the break opens out of the screen */
		out->oz = motion->throw_px * k * motion->reach * 0.55f * (0.35f + shard->rnd[2] * 0.9f);
	} else {
		/* a little depth either way, so the shards do not read as one
		 * flat sheet sliding off */
		out->oz = k * k * motion->reach * 0.10f * (shard->rnd[2] - 0.45f);
	}

	if (motion->flat) {
		out->axis[0] = 0.0f;
		out->axis[1] = 0.0f;
		out->axis[2] = 1.0f;
		out->oz = 0.0f;
	} else {
		float a = shard->rnd[1] * GLASS_TAU;
		float az = (shard->rnd[2] - 0.5f) * 1.2f;
		float inv = 1.0f / sqrtf(1.0f + az * az);

		out->axis[0] = cosf(a) * inv;
		out->axis[1] = sinf(a) * inv;
		out->axis[2] = az * inv;
	}

	spin_turns = motion->spin * (0.35f + shard->rnd[0] * 1.3f) * (shard->rnd[1] < 0.5f ? -1.0f : 1.0f);
	out->angle = spin_turns * GLASS_TAU * k;
}

float glass_shard_depth(const struct glass_motion *motion, const struct glass_shard *shard)
{
	struct glass_xform x;

	glass_shard_xform(motion, shard, &x);
	return x.oz;
}

static int sortkey_cmp(const void *a, const void *b)
{
	const struct glass_sortkey *ka = a;
	const struct glass_sortkey *kb = b;

	/* back to front, so the nearest shard is drawn last */
	if (ka->depth < kb->depth)
		return -1;
	if (ka->depth > kb->depth)
		return 1;

	/* a stable order for the shards that share a depth, which with the
	 * tumble switched off is all of them */
	return ka->index < kb->index ? -1 : (ka->index > kb->index ? 1 : 0);
}

void glass_sort_order(const struct glass_field *field, const struct glass_motion *motion, uint32_t *order,
		      struct glass_sortkey *scratch)
{
	int i;

	for (i = 0; i < field->count; i++) {
		scratch[i].depth = glass_shard_depth(motion, &field->shard[i]);
		scratch[i].index = (uint32_t)i;
	}

	qsort(scratch, (size_t)field->count, sizeof(*scratch), sortkey_cmp);

	for (i = 0; i < field->count; i++)
		order[i] = scratch[i].index;
}
