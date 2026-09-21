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
 * The geometry and the motion behind the glass transition, with no dependency
 * on libobs or on a graphics device.
 *
 * The other two transitions in this module are full screen shaders, so their
 * math lives in the effect file and test/ mirrors it in C. This one is a mesh:
 * the shards are real triangles built on the CPU, so the code here *is* the
 * implementation rather than a copy of it, and the tests link it directly.
 * Only glass_shard_xform() has a mirror - the vertex shader has to evaluate it
 * per vertex - and that one is marked at both ends.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Ceiling on the shard count, whatever shard size is asked for. A 4K canvas
 * with the size slider at its minimum would otherwise ask for a few hundred
 * thousand shards, and the cost of a break is linear in them twice over: the
 * mesh is rebuilt on the graphics thread, and the draw order is re-sorted every
 * frame. glass_field_build() grows the shard size until the count fits rather
 * than truncating the break, so the canvas stays covered.
 */
#define GLASS_MAX_SHARDS 3000

/* Vertices one cell may have. Cells start as quads or as the canvas rectangle
 * and only gain vertices by being clipped, one per clipping plane that actually
 * cuts them, so this is slack rather than a limit that is reached. */
#define GLASS_MAX_POLY 24

/*
 * Shards are grown by this many pixels about their own centre before they are
 * emitted.
 *
 * The patterns below partition the canvas exactly, but two triangles meeting
 * along a shared edge are rasterised from vertices that were computed by
 * different routes, and float rounding can leave the odd pixel on the edge
 * owned by neither. A shard that overlaps its neighbour slightly cannot. The
 * overlap costs nothing at rest because every shard samples the outgoing scene
 * at the pixel's own position: two shards covering one pixel agree on its
 * colour exactly.
 */
#define GLASS_SEAM_BLEED 0.75f

/* How much of the crack sweep it takes one shard's edges to light up. The
 * front is run this far past the furthest shard so that the crack phase ends
 * with the whole pane broken rather than with the far corner just starting. */
#define GLASS_CRACK_EDGE 0.12f

enum glass_pattern {
	GLASS_PATTERN_WEB = 0,     /* radial spokes and concentric rings */
	GLASS_PATTERN_VORONOI = 1, /* cells of a jittered lattice of seeds */
	GLASS_PATTERN_ICE = 2,     /* a plane cut by straight cracks, recursively */
};

/* Shapes how the crack front crosses the pane. The same three curves the
 * tiling transition offers, and like that one it eases the order the shards
 * crack in rather than the transition clock. */
enum glass_easing {
	GLASS_EASE_LINEAR = 0,
	GLASS_EASE_IN_OUT = 1,
	GLASS_EASE_OUT = 2,
};

enum glass_style {
	GLASS_STYLE_WINDOW = 0,   /* crack, hold, then the shards fall away */
	GLASS_STYLE_BLOW = 1,     /* the shards blast outwards and past the camera */
	GLASS_STYLE_WIPE = 2,     /* nothing moves; the scene changes shard by shard */
	GLASS_STYLE_ASSEMBLE = 3, /* the outgoing scene leaves, the incoming one lands */
};

/* A convex cell, in canvas pixels, wound counter-clockwise. */
struct glass_poly {
	float x[GLASS_MAX_POLY];
	float y[GLASS_MAX_POLY];
	int n;
};

struct glass_shard {
	struct glass_poly poly; /* as generated: the exact partition, no seam bleed */

	float cx; /* centroid, canvas pixels - the pivot the shard turns about */
	float cy;
	float area;   /* signed area, positive for the winding above */
	float inner;  /* distance from the centroid to the nearest cut edge */
	float launch; /* 0..1, when the crack front reaches this shard */

	/* Three uncorrelated values in 0..1, so one shard's throw, tumble axis,
	 * spin and tint do not move together with its neighbour's. */
	float rnd[3];
};

struct glass_build {
	float canvas_cx; /* canvas size, pixels */
	float canvas_cy;
	float impact_x; /* impact point, pixels; may sit outside the canvas */
	float impact_y;
	float shard_px; /* wanted shard size, pixels */
	int pattern;
	uint32_t seed;
};

struct glass_field {
	struct glass_shard *shard;
	int count;

	float reach;    /* pixels from the impact to the furthest canvas corner */
	float shard_px; /* the size actually used, after the shard cap */

	/* what it was built from, so the caller can tell whether a settings
	 * change needs a rebuild */
	struct glass_build build;
};

/* One shard's triangles inside the emitted index buffer, so the draw order can
 * be re-sorted per frame without touching the vertices. */
struct glass_span {
	uint32_t first;
	uint32_t count;
};

/*
 * A mesh vertex. The layout is split across the texture coordinate sets the
 * vertex shader declares:
 *
 *   position  -> x, y (canvas pixels), z unused
 *   texcoord0 -> u, v
 *   texcoord1 -> centroid x, centroid y, core, launch
 *   texcoord2 -> rnd0, rnd1, rnd2, inner
 */
struct glass_vertex {
	float x, y;
	float u, v;
	float cx, cy;
	float core;   /* 0 on the cut edge, 1 at the centroid - the rim light ramp */
	float launch; /* copied from the shard, so the shader needs no lookup */
	float rnd[3];

	/* The shard's inradius, so the shader can turn `core` back into a
	 * distance in pixels. Without it a crack line drawn at a fixed value of
	 * `core` would be as many pixels wide as the shard is big, and the
	 * cracks across the small shards at the impact would be hairlines while
	 * the ones out at the edge were bands. */
	float inner;
};

/* How a shard is sitting at one instant. */
struct glass_xform {
	float ox, oy, oz; /* displacement in canvas pixels; +z is towards the camera */
	float axis[3];    /* tumble axis, unit length */
	float angle;      /* tumble angle, radians */
	float alpha;      /* 1 until the shard fades out at the end of its flight */
	float motion;     /* 0..1, the shard's own flight clock */
	float crack;      /* 0..1, how far the crack front is past this shard */
	float reveal;     /* 0..1, outgoing to incoming, for GLASS_STYLE_WIPE */

	/*
	 * Fades the glass out as the shard finishes: 1 while it is broken
	 * glass, 0 once it has become the incoming scene.
	 *
	 * A shard that leaves the frame takes its cracks with it, so for those
	 * styles this stays at 1 and the alpha above does the work. The two
	 * styles that end with shards still on screen - the wipe, where nothing
	 * ever moves, and the shards that land in the assemble - need this
	 * instead, or the transition would hand over an incoming scene with
	 * every shard still outlined on it.
	 */
	float settle;
};

/* Everything the motion depends on that is the same for every shard. */
struct glass_motion {
	int style;
	float progress; /* transition time, 0..1 */
	float hold_end; /* progress at which the shards let go */

	/*
	 * How far the crack front has swept, already eased. It is the same for
	 * every shard in a frame, so it is worked out once by glass_crack_front()
	 * rather than per vertex - which is also what keeps the easing curve in
	 * one place instead of in both the shader and this file.
	 */
	float crack_front;
	float gravity_x; /* gravity direction, unit length */
	float gravity_y;
	float gravity;  /* gravity strength, 0..1 */
	float throw_px; /* outward push, 0..1 */
	float spin;     /* tumble turns over a whole flight */
	float scatter;  /* 0 uniform, 1 every shard on its own timing and heading */
	float reach;    /* pixels from the impact to the furthest corner */
	float impact_x; /* impact point, pixels */
	float impact_y;
	bool flat; /* keep the shards in the screen plane */

	/* Runs the flight backwards: the shard starts scattered and lands on
	 * the last frame instead of leaving on it. This is what the incoming
	 * scene's pass uses in GLASS_STYLE_ASSEMBLE. */
	bool arriving;
};

/* Scratch for the per frame draw order sort, owned by the caller so that
 * sorting a few thousand shards every frame allocates nothing. */
struct glass_sortkey {
	float depth;
	uint32_t index;
};

/*
 * Builds the break. Returns false only if the allocation fails; every other
 * input is clamped into range, including an impact point outside the canvas and
 * a shard size that would blow through GLASS_MAX_SHARDS.
 */
bool glass_field_build(struct glass_field *field, const struct glass_build *build);
void glass_field_free(struct glass_field *field);

/* True when a field built for `build` would differ from `field`, i.e. when the
 * mesh has to be rebuilt rather than just redrawn with new uniforms. */
bool glass_field_stale(const struct glass_field *field, const struct glass_build *build);

/*
 * Triangles. Each shard is fanned from its own centroid rather than from one of
 * its corners: every triangle then has two vertices on the cut edge and one in
 * the middle, which is what gives the rim light a linear ramp to interpolate
 * across the shard instead of a value that is zero at every corner.
 */
size_t glass_vertex_count(const struct glass_field *field);
size_t glass_index_count(const struct glass_field *field);

/* `spans` may be NULL; it is only needed by callers that re-sort the draw
 * order. `verts` and `indices` must have room for the counts above. */
void glass_emit(const struct glass_field *field, struct glass_vertex *verts, uint32_t *indices,
		struct glass_span *spans);

/*
 * How far the crack front has swept at `progress`, ready for
 * glass_motion::crack_front.
 *
 * It runs GLASS_CRACK_EDGE past 1 so that the shard in the furthest corner has
 * finished cracking as the crack phase ends rather than just starting to.
 */
float glass_crack_front(float progress, float crack_end, int easing);

/*
 * Where one shard is at glass_motion::progress.
 *
 * NOTE: mirrored by shard_xform() in data/effects/glass_transition.effect,
 * which is what actually moves the vertices. This copy places the shard for the
 * depth sort and is what the tests hold the motion to. Keep the two in sync.
 */
void glass_shard_xform(const struct glass_motion *motion, const struct glass_shard *shard, struct glass_xform *out);

/* The shard's distance from the camera, for back to front drawing. Larger is
 * nearer. Cheaper than the full transform, which is worth it once a frame per
 * shard. */
float glass_shard_depth(const struct glass_motion *motion, const struct glass_shard *shard);

/* Sorts `order` (shard indices) back to front. `order` and `scratch` must each
 * hold field->count entries; neither needs to be initialised. */
void glass_sort_order(const struct glass_field *field, const struct glass_motion *motion, uint32_t *order,
		      struct glass_sortkey *scratch);
