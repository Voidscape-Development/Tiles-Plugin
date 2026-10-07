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
 * The geometry and the clock behind the matte cutout transition, with no
 * dependency on libobs or on a graphics device.
 *
 * Every cutout is one scalar field over the canvas: matte_field() says, for a
 * point, how far the front has to have travelled before that point changes
 * scene. A shape that grows about the origin is the field of its gauge - the
 * scale at which its outline passes the point - and a sweep is the distance
 * along the sweep with the edge's own profile taken off it. The front then
 * only has to run from the lowest value on the canvas to the highest, and the
 * bands that trail it are fixed offsets behind it, so every shape gets the
 * border, the echoes, the feather and the glow from the same few lines of
 * shader.
 *
 * The shader evaluates the field per pixel in data/effects/matte_transition.effect,
 * and matte_field() below is its mirror, marked at both ends. The plugin uses
 * the mirror to find the range the front has to cover, and the tests use it to
 * check that range really does cover the canvas.
 */

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The shader carries these as integers, so the numbering is part of the
 * contract with the effect file. */
enum matte_shape {
	MATTE_SHAPE_CIRCLE = 0,
	MATTE_SHAPE_SQUARE = 1,
	MATTE_SHAPE_TRIANGLE = 2,
	MATTE_SHAPE_DIAMOND = 3,
	MATTE_SHAPE_HEXAGON = 4,
	MATTE_SHAPE_OCTAGON = 5,
	MATTE_SHAPE_STAR = 6,
	MATTE_SHAPE_HEART = 7,
	MATTE_SHAPE_CROSS = 8,
	MATTE_SHAPE_WAVE = 9,    /* a sine edge sweeping across */
	MATTE_SHAPE_CURL = 10,   /* a breaking crest sweeping across */
	MATTE_SHAPE_RIPPLE = 11, /* a circle with a rippling outline */
	MATTE_SHAPE_CLOCK = 12,
	MATTE_SHAPE_SPIRAL = 13,
	MATTE_SHAPE_BLINDS = 14,
	MATTE_SHAPE_ZIGZAG = 15,
	MATTE_SHAPE_IMAGE = 16, /* an image's silhouette, grown like the shapes */
	MATTE_SHAPE_LUMA = 17,  /* an image's brightness as the reveal order */
};

#define MATTE_SHAPE_COUNT 18

enum matte_direction {
	MATTE_DIR_OUT = 0, /* the incoming scene grows out of the origin */
	MATTE_DIR_IN = 1,  /* the outgoing scene shrinks into it */
};

enum matte_easing {
	MATTE_EASE_LINEAR = 0,
	MATTE_EASE_IN_OUT = 1,
	MATTE_EASE_OUT = 2,
	MATTE_EASE_IN = 3,
	MATTE_EASE_OVERSHOOT = 4,
};

/* Samples in the radial profile an image silhouette is reduced to, one per
 * angle around its centre. */
#define MATTE_PROFILE_SIZE 512

/* How many echo bands can trail the edge. The shader blends them with a fixed,
 * unrolled chain, so raising this means adding a link there too. */
#define MATTE_ECHOES 4

/* Cross arm thickness as a share of its length. */
#define MATTE_CROSS_THICKNESS 0.28f

/* The shortest curl wavelength, in wave heights, that still holds the whole
 * lip between one crest and the next. */
#define MATTE_CURL_MIN_LENGTH 3.0f

/* Diamond height over width. */
#define MATTE_DIAMOND_TALL 1.4f

/*
 * Everything matte_field() reads. Distances are canvas pixels in the shape's
 * own frame, which is the canvas frame turned by the rotation and divided by
 * the stretch.
 */
struct matte_shape_params {
	int shape;
	float star_k;         /* star: slope folded out of points and depth */
	float star_n;         /* star: point count */
	float arms;           /* clock and spiral: segments around the circle */
	float pitch;          /* clock and spiral: field gained over one segment */
	float amp;            /* waves, zigzag, ripple: edge height, px */
	float wavelen;        /* waves and zigzag: px between crests */
	float lobes;          /* ripple: ripples around the circle */
	float bar_px;         /* blinds: px across one bar */
	const float *profile; /* image: MATTE_PROFILE_SIZE radii, mean 1 */
};

/* The field at q (shape frame, pixels from the origin) with the edge pattern
 * slid along by phase, in cycles. MIRRORED by matte_field() in the effect. */
float matte_field(const struct matte_shape_params *sp, float qx, float qy, float phase);

/* Whether a shape is a sweep: its field is a distance along one axis rather
 * than a size about the origin. */
bool matte_is_sweep(int shape);

/* Whether the edge pattern moves with the phase, so that the range has to be
 * found over every phase rather than one. */
bool matte_has_phase(int shape);

/*
 * The star folds its point count and depth into a single slope, the field
 * being x + K*|y| once the angle has been folded into one point's half sector.
 * depth is the inner radius as a share of the outer one.
 */
float matte_star_slope(float points, float depth);

/*
 * What the transform from canvas to shape frame needs: the origin, and the
 * rotation and stretch at both ends of the transition. Spin turns the shape
 * from angle0 to angle1; the range is found across the turn as well as at its
 * ends, so a spinning shape is never caught short mid-turn either.
 */
struct matte_frame {
	float origin_x, origin_y; /* px */
	float angle0, angle1;     /* radians */
	float stretch_x, stretch_y;
};

static inline void matte_to_shape(const struct matte_frame *f, float angle, float px, float py, float *qx, float *qy)
{
	float dx = px - f->origin_x;
	float dy = py - f->origin_y;
	float c = cosf(angle);
	float s = sinf(angle);

	*qx = (dx * c + dy * s) / f->stretch_x;
	*qy = (-dx * s + dy * c) / f->stretch_y;
}

/*
 * The lowest and highest field value anywhere on a cx by cy canvas, over the
 * whole of the rotation and, for a moving edge, every phase. The front runs
 * from the one to the other, so this is what keeps the first frame wholly the
 * outgoing scene and the last wholly the incoming one.
 *
 * Every field is monotone along rays from the origin (a scaled shape) or linear
 * across the canvas (a sweep), so the extremes lie on the canvas boundary, plus
 * the origin itself when it is inside. Those are what get sampled. A small
 * margin covers the gaps between samples.
 */
void matte_field_range(const struct matte_shape_params *sp, const struct matte_frame *f, float cx, float cy, float *lo,
		       float *hi);

/* ------------------------------------------------------------------ */
/* the clock                                                          */
/* ------------------------------------------------------------------ */

struct matte_timing {
	int easing;
	float overshoot; /* 0..1, scales the back-out pull for MATTE_EASE_OVERSHOOT */
	float pause_at;  /* 0..1 of the travel, where the pause holds */
	float pause_len; /* 0..1 of the transition; 0 is no pause */
};

float matte_ease(int easing, float overshoot, float x);

/* How far along its travel the front is at transition time t, 0 at t = 0 and
 * 1 at t = 1 exactly. Can run past 1 in between with overshoot. */
float matte_travel(const struct matte_timing *tm, float t);

/*
 * The band layout behind the front, all in field units (which are pixels for
 * every shape but the luma wipe): a border, the echoes, and then the incoming
 * scene. width is where the incoming scene starts. pad is how far the front is
 * run beyond the field range at each end, so that nothing - not the feather,
 * not the glow - is left on the first or last frame.
 */
struct matte_bands {
	float border;
	float echo;
	int echo_count;
	float feather; /* at least 1, it is also the anti-aliasing */
	float glow;    /* glow reach either side of the edge; 0 is off */
};

static inline float matte_band_width(const struct matte_bands *b)
{
	return b->border + b->echo * (float)b->echo_count;
}

static inline float matte_band_pad(const struct matte_bands *b)
{
	return b->feather + b->glow;
}

/*
 * The front for travel u. The shader takes d = sign * (front - field) as the
 * distance the front has passed a pixel: below zero the outgoing scene, then
 * the bands, then the incoming scene from matte_band_width() on.
 *
 * Inside out, the front climbs from below the lowest field value to above the
 * highest plus the bands. Outside in, it falls from above the highest to below
 * the lowest minus the bands, with the sign flipped so the bands still trail it.
 */
float matte_front(int direction, float lo, float hi, const struct matte_bands *b, float u);

static inline float matte_sign(int direction)
{
	return direction == MATTE_DIR_IN ? -1.0f : 1.0f;
}

/* ------------------------------------------------------------------ */
/* image silhouettes                                                  */
/* ------------------------------------------------------------------ */

/*
 * Reduces an image to the radial profile its silhouette is grown from: for each
 * angle around the image centre, the furthest point along that ray that is
 * inside the shape. A pixel is inside if its alpha is at least half, or, for an
 * image with no transparency at all, if its brightness is.
 *
 * That is the outline as seen from the centre, which is what lets the
 * silhouette grow until it covers the canvas: a shape with a hole grown about
 * its centre would grow the hole too, and never close. Holes and overhangs are
 * filled in. Radii are normalised to a mean of 1 so the band widths stay near
 * pixels, and the image's own aspect ratio is kept.
 *
 * rgba is 8 bits per channel, tightly packed. Returns false if nothing in the
 * image is inside, leaving profile untouched.
 */
bool matte_build_profile(const uint8_t *rgba, uint32_t w, uint32_t h, float *profile);
