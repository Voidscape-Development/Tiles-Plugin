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

#include "matte-math.h"

#include <math.h>

#define TAU_F 6.2831853f
#define PI_F 3.14159265f

/* Samples along each canvas edge when looking for the field's range. */
#define RANGE_EDGE_SAMPLES 1024

/* Rotations sampled across a spin. */
#define RANGE_ANGLES 64

static inline float fracf_(float v)
{
	return v - floorf(v);
}

static inline float saturatef_(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * The curling crest. Each wave rises gently over CURL_PEAK of its length and
 * drops away steeply over the rest, the way a wave stands up before it breaks,
 * and a lip of radius LIP_R hangs LIP_O ahead of the trough line, pushed LIP_U
 * past the crest so it spills over the steep face. All are shares of the wave
 * height but the peak, which is a share of the wavelength.
 */
#define CURL_PEAK 0.7f
#define CURL_LIP_R 0.5f
#define CURL_LIP_O 0.85f
#define CURL_LIP_U 0.35f

/* The heart is a square standing on its point with a disc on each of its two
 * upper edges. Measured from the heart's centre, which sits a quarter of the
 * square's half diagonal above the square's own, so the origin is inside both
 * discs as well as the square - every piece of a union has to contain the
 * origin for its gauge to be the smallest of theirs. */
#define HEART_DISC_X 0.5f
#define HEART_DISC_Y -0.25f
#define HEART_DISC_R2 0.5f

bool matte_is_sweep(int shape)
{
	return shape == MATTE_SHAPE_WAVE || shape == MATTE_SHAPE_CURL || shape == MATTE_SHAPE_ZIGZAG;
}

bool matte_has_phase(int shape)
{
	return matte_is_sweep(shape) || shape == MATTE_SHAPE_RIPPLE;
}

float matte_star_slope(float points, float depth)
{
	float half = PI_F / points;

	return (1.0f - depth * cosf(half)) / (depth * sinf(half));
}

/* The gauge of a disc that contains the origin: the scale s at which the
 * origin-scaled disc's outline passes p. Solves |p - s c|^2 = s^2 r^2. */
static float disc_gauge(float px, float py, float cx, float cy, float r2)
{
	float a = r2 - (cx * cx + cy * cy);
	float pc = px * cx + py * cy;
	float pp = px * px + py * py;

	return (-pc + sqrtf(pc * pc + a * pp)) / a;
}

static float profile_radius(const float *profile, float ang)
{
	float f = fracf_(ang / TAU_F) * (float)MATTE_PROFILE_SIZE - 0.5f;
	float fl = floorf(f);
	float w = f - fl;
	int i0 = (int)fl;
	int i1 = i0 + 1;

	i0 = (i0 % MATTE_PROFILE_SIZE + MATTE_PROFILE_SIZE) % MATTE_PROFILE_SIZE;
	i1 = i1 % MATTE_PROFILE_SIZE;

	return profile[i0] + (profile[i1] - profile[i0]) * w;
}

/* MIRRORED in data/effects/matte_transition.effect: matte_field() */
float matte_field(const struct matte_shape_params *sp, float qx, float qy, float phase)
{
	float ax = fabsf(qx);
	float ay = fabsf(qy);
	float r = sqrtf(qx * qx + qy * qy);

	switch (sp->shape) {
	case MATTE_SHAPE_SQUARE:
		return fmaxf(ax, ay);

	case MATTE_SHAPE_TRIANGLE:
		/* inradius 1, point up: y grows down the canvas */
		return fmaxf(qy, -0.5f * qy + 0.8660254f * ax);

	case MATTE_SHAPE_DIAMOND:
		return (MATTE_DIAMOND_TALL * ax + ay) / 1.7204651f;

	case MATTE_SHAPE_HEXAGON:
		return fmaxf(ax, 0.5f * ax + 0.8660254f * ay);

	case MATTE_SHAPE_OCTAGON:
		return fmaxf(fmaxf(ax, ay), (ax + ay) * 0.7071068f);

	case MATTE_SHAPE_STAR: {
		/* angle from straight up, clockwise, folded into one point's half
		 * sector: the edge there is fx + K * fy = 1 */
		float sector = TAU_F / sp->star_n;
		float a = atan2f(qx, -qy);

		a -= sector * floorf(a / sector + 0.5f);
		return r * cosf(a) + sp->star_k * r * fabsf(sinf(a));
	}

	case MATTE_SHAPE_HEART: {
		float sq = fmaxf((ax + qy) / 1.25f, (ax - qy) / 0.75f);
		float disc = disc_gauge(ax, qy, HEART_DISC_X, HEART_DISC_Y, HEART_DISC_R2);

		return fminf(sq, disc);
	}

	case MATTE_SHAPE_CROSS: {
		float u = fabsf(qx + qy) * 0.7071068f;
		float v = fabsf(qx - qy) * 0.7071068f;

		return fminf(fmaxf(u, v / MATTE_CROSS_THICKNESS), fmaxf(v, u / MATTE_CROSS_THICKNESS));
	}

	case MATTE_SHAPE_WAVE:
		return qx - sp->amp * sinf(TAU_F * (qy / sp->wavelen + phase));

	case MATTE_SHAPE_CURL: {
		float x = fracf_(qy / sp->wavelen + phase);
		float s = x < CURL_PEAK ? x / CURL_PEAK : (1.0f - x) / (1.0f - CURL_PEAK);
		float g = qx - sp->amp * s * s * (3.0f - 2.0f * s);
		float lip_r = CURL_LIP_R * sp->amp;
		float du = (x - CURL_PEAK) * sp->wavelen - CURL_LIP_U * sp->amp;

		if (fabsf(du) < lip_r)
			g = fminf(g, qx - CURL_LIP_O * sp->amp - sqrtf(lip_r * lip_r - du * du));
		return g;
	}

	case MATTE_SHAPE_RIPPLE: {
		float ramp = saturatef_(r / fmaxf(2.0f * sp->amp, 0.001f));

		return r + sp->amp * ramp * sinf(sp->lobes * atan2f(qy, qx) + TAU_F * phase);
	}

	case MATTE_SHAPE_CLOCK:
	case MATTE_SHAPE_SPIRAL: {
		/* from straight up, clockwise */
		float turn = fracf_((atan2f(qy, qx) / TAU_F + 0.25f) * sp->arms);

		return (sp->shape == MATTE_SHAPE_SPIRAL ? r : 0.0f) + turn * sp->pitch;
	}

	case MATTE_SHAPE_BLINDS:
		return fabsf(fracf_(qy / sp->bar_px + 0.5f) - 0.5f) * sp->bar_px;

	case MATTE_SHAPE_ZIGZAG:
		return qx - sp->amp * (1.0f - 4.0f * fabsf(fracf_(qy / sp->wavelen + phase) - 0.5f));

	case MATTE_SHAPE_IMAGE:
		if (sp->profile)
			return r / fmaxf(profile_radius(sp->profile, atan2f(qy, qx)), 0.001f);
		return r;

	case MATTE_SHAPE_LUMA:
		return 0.0f; /* a texture lookup, not a function of q */

	case MATTE_SHAPE_CIRCLE:
	default:
		return r;
	}
}

/* The field's extremes at q over every phase. A moving edge is bounded by its
 * height either side of the line it moves along, so that is used outright
 * rather than hunting for the worst phase. */
static void field_bounds(const struct matte_shape_params *sp, float qx, float qy, float *lo, float *hi)
{
	float r;
	float ramp;

	switch (sp->shape) {
	case MATTE_SHAPE_WAVE:
	case MATTE_SHAPE_ZIGZAG:
		*lo = qx - sp->amp;
		*hi = qx + sp->amp;
		return;
	case MATTE_SHAPE_CURL:
		*lo = qx - (CURL_LIP_O + CURL_LIP_R) * sp->amp;
		*hi = qx;
		return;
	case MATTE_SHAPE_RIPPLE:
		r = sqrtf(qx * qx + qy * qy);
		ramp = saturatef_(r / fmaxf(2.0f * sp->amp, 0.001f));
		*lo = r - sp->amp * ramp;
		*hi = r + sp->amp * ramp;
		return;
	case MATTE_SHAPE_CLOCK:
	case MATTE_SHAPE_SPIRAL:
		/* Every angle meets at the origin, and the field takes its turn
		 * from the angle, so the one point the boundary cannot speak for
		 * has to be taken as every turn at once: the pixels around it
		 * run the whole segment. */
		if (qx == 0.0f && qy == 0.0f) {
			*lo = 0.0f;
			*hi = sp->pitch;
			return;
		}
		*lo = *hi = matte_field(sp, qx, qy, 0.0f);
		return;
	default:
		*lo = *hi = matte_field(sp, qx, qy, 0.0f);
		return;
	}
}

static void range_at(const struct matte_shape_params *sp, const struct matte_frame *f, float angle, float cx, float cy,
		     float *lo, float *hi)
{
	float ex[4] = {0.0f, cx, cx, 0.0f};
	float ey[4] = {0.0f, 0.0f, cy, cy};
	int e;
	int i;

	for (e = 0; e < 4; e++) {
		float x0 = ex[e], y0 = ey[e];
		float x1 = ex[(e + 1) % 4], y1 = ey[(e + 1) % 4];

		for (i = 0; i <= RANGE_EDGE_SAMPLES; i++) {
			float k = (float)i / (float)RANGE_EDGE_SAMPLES;
			float qx, qy, l, h;

			matte_to_shape(f, angle, x0 + (x1 - x0) * k, y0 + (y1 - y0) * k, &qx, &qy);
			field_bounds(sp, qx, qy, &l, &h);
			*lo = fminf(*lo, l);
			*hi = fmaxf(*hi, h);
		}
	}

	/* the origin, if it is on the canvas: a scaled shape is at its lowest
	 * there, and that is not on the boundary */
	if (f->origin_x >= 0.0f && f->origin_x <= cx && f->origin_y >= 0.0f && f->origin_y <= cy) {
		float l, h;

		field_bounds(sp, 0.0f, 0.0f, &l, &h);
		*lo = fminf(*lo, l);
		*hi = fmaxf(*hi, h);
	}
}

void matte_field_range(const struct matte_shape_params *sp, const struct matte_frame *f, float cx, float cy, float *lo,
		       float *hi)
{
	float margin;
	int i;

	*lo = INFINITY;
	*hi = -INFINITY;

	if (sp->shape == MATTE_SHAPE_BLINDS) {
		/* periodic across the canvas, so the boundary can miss a bar's
		 * edge; the range is the bar itself */
		*lo = 0.0f;
		*hi = 0.5f * sp->bar_px;
		return;
	}

	range_at(sp, f, f->angle0, cx, cy, lo, hi);

	if (f->angle1 != f->angle0) {
		range_at(sp, f, f->angle1, cx, cy, lo, hi);
		for (i = 1; i < RANGE_ANGLES; i++)
			range_at(sp, f, f->angle0 + (f->angle1 - f->angle0) * (float)i / (float)RANGE_ANGLES, cx, cy,
				 lo, hi);
	}

	/* the gaps between samples: a star's tip can fall between two of them */
	margin = 0.01f * (*hi - *lo) + 1.0f;
	*lo -= margin;
	*hi += margin;
}

/* ------------------------------------------------------------------ */
/* the clock                                                          */
/* ------------------------------------------------------------------ */

float matte_ease(int easing, float overshoot, float x)
{
	float y;
	float c1;

	x = saturatef_(x);

	switch (easing) {
	case MATTE_EASE_IN_OUT:
		return x * x * (3.0f - 2.0f * x);
	case MATTE_EASE_OUT:
		y = 1.0f - x;
		return 1.0f - y * y * y;
	case MATTE_EASE_IN:
		return x * x * x;
	case MATTE_EASE_OVERSHOOT:
		/* back-out: runs past 1 and settles back onto it */
		c1 = 2.5f * saturatef_(overshoot);
		y = x - 1.0f;
		return 1.0f + (c1 + 1.0f) * y * y * y + c1 * y * y;
	case MATTE_EASE_LINEAR:
	default:
		return x;
	}
}

float matte_travel(const struct matte_timing *tm, float t)
{
	float len = tm->pause_len;
	float at = saturatef_(tm->pause_at);
	float a, b;

	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	if (len <= 0.0f)
		return matte_ease(tm->easing, tm->overshoot, t);

	/* the two moving stages share the time left over in proportion to the
	 * distance each covers, so the front keeps the same average pace */
	a = (1.0f - len) * at;
	b = a + len;

	if (t < a)
		return at * matte_ease(tm->easing, tm->overshoot, t / a);
	if (t < b)
		return at;
	return at + (1.0f - at) * matte_ease(tm->easing, tm->overshoot, (t - b) / (1.0f - b));
}

float matte_front(int direction, float lo, float hi, const struct matte_bands *b, float u)
{
	float w = matte_band_width(b);
	float pad = matte_band_pad(b);

	if (direction == MATTE_DIR_IN)
		return (hi + pad) + ((lo - w - pad) - (hi + pad)) * u;

	return (lo - pad) + ((hi + w + pad) - (lo - pad)) * u;
}

/* ------------------------------------------------------------------ */
/* image silhouettes                                                  */
/* ------------------------------------------------------------------ */

bool matte_build_profile(const uint8_t *rgba, uint32_t w, uint32_t h, float *profile)
{
	float radii[MATTE_PROFILE_SIZE];
	bool has_alpha = false;
	float cx = 0.5f * (float)w;
	float cy = 0.5f * (float)h;
	float reach = sqrtf(cx * cx + cy * cy);
	float sum = 0.0f;
	float peak = 0.0f;
	size_t n = (size_t)w * (size_t)h;
	size_t i;
	int a;

	if (!rgba || w == 0 || h == 0)
		return false;

	for (i = 0; i < n; i++) {
		if (rgba[i * 4 + 3] < 250) {
			has_alpha = true;
			break;
		}
	}

	for (a = 0; a < MATTE_PROFILE_SIZE; a++) {
		/* texel centres, matching how the shader samples the profile */
		float ang = ((float)a + 0.5f) / (float)MATTE_PROFILE_SIZE * TAU_F;
		float dx = cosf(ang);
		float dy = sinf(ang);
		float best = 0.0f;
		float r;

		for (r = 0.0f; r <= reach; r += 0.5f) {
			int x = (int)floorf(cx + dx * r);
			int y = (int)floorf(cy + dy * r);
			const uint8_t *p;
			bool inside;

			if (x < 0 || y < 0 || x >= (int)w || y >= (int)h)
				break;

			p = rgba + ((size_t)y * w + (size_t)x) * 4;
			if (has_alpha)
				inside = p[3] >= 128;
			else
				inside = (p[0] * 54 + p[1] * 183 + p[2] * 19) >= 128 * 256;

			if (inside)
				best = r + 0.5f;
		}

		radii[a] = best;
		peak = fmaxf(peak, best);
	}

	if (peak <= 0.0f)
		return false;

	/* A ray that found nothing would make the field run away along it; floor
	 * it, so the silhouette pinches there rather than taking forever to
	 * cover the canvas in that direction. */
	for (a = 0; a < MATTE_PROFILE_SIZE; a++) {
		radii[a] = fmaxf(radii[a], 0.05f * peak);
		sum += radii[a];
	}

	/* The march finds each radius to the pixel, which grown to the size of
	 * the canvas is a staircase along the outline. Two passes of a small
	 * blur round the circle take the steps out without rounding off any
	 * corner worth keeping. */
	for (i = 0; i < 2; i++) {
		float prev = radii[MATTE_PROFILE_SIZE - 1];
		float first = radii[0];

		for (a = 0; a < MATTE_PROFILE_SIZE; a++) {
			float next = a + 1 < MATTE_PROFILE_SIZE ? radii[a + 1] : first;
			float here = radii[a];

			radii[a] = 0.25f * prev + 0.5f * here + 0.25f * next;
			prev = here;
		}
	}

	for (a = 0; a < MATTE_PROFILE_SIZE; a++)
		profile[a] = radii[a] * (float)MATTE_PROFILE_SIZE / sum;

	return true;
}
