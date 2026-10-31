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
 * A matte cutout transition: the outgoing scene is cut away by one big shape,
 * growing out of a point or closing in on it, with the incoming scene showing
 * through - an iris wipe, and with the border, echo bands and glow turned up,
 * a stinger.
 *
 * The tiling transition's Keyhole mode does something related with a whole
 * lattice of small shapes. This is the single shape, which is a different
 * problem: no lattice, but shapes that cannot tile (a star, a heart, a
 * silhouette from an image), sweeps, and bands trailing the edge.
 *
 * The geometry and the clock are in src/matte-math.c, shared with the tests;
 * the look is in data/effects/matte_transition.effect. This file turns
 * properties into uniforms, loads the mask image, and works out the range the
 * front has to cover for the current canvas.
 */

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/image-file.h>
#include <util/dstr.h>
#include <util/task.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "matte-math.h"
#include "matte-transition.h"
#include "presets.h"
#include "transition-common.h"

/* clang-format off */

#define S_PRESET       PRESET_KEY
#define S_SHAPE        "shape"
#define S_DIRECTION    "direction"
#define S_STAR_POINTS  "star_points"
#define S_STAR_DEPTH   "star_depth"
#define S_ARMS         "segments"
#define S_TURNS        "spiral_turns"
#define S_BARS         "bars"
#define S_WAVE_HEIGHT  "wave_height"
#define S_WAVE_LENGTH  "wave_length"
#define S_WAVE_DRIFT   "wave_drift"
#define S_LOBES        "ripples"
#define S_IMAGE        "image"
#define S_ANGLE        "angle"
#define S_SPIN         "spin"
#define S_STRETCH      "stretch"
#define S_ORIGIN_X     "origin_x"
#define S_ORIGIN_Y     "origin_y"
#define S_EASING       "easing"
#define S_OVERSHOOT    "overshoot"
#define S_PAUSE_SIZE   "pause_size"
#define S_PAUSE_LEN    "pause_length"
#define S_FEATHER      "feather"
#define S_BORDER       "border"
#define S_BORDER_COLOR "border_color"
#define S_ECHO_COUNT   "echo_count"
#define S_ECHO_WIDTH   "echo_width"
#define S_ECHO_COLOR1  "echo_color1"
#define S_ECHO_COLOR2  "echo_color2"
#define S_ECHO_COLOR3  "echo_color3"
#define S_ECHO_COLOR4  "echo_color4"
#define S_GLOW         "glow"
#define S_GLOW_WIDTH   "glow_width"
#define S_GLOW_COLOR   "glow_color"

#define T_(x)          obs_module_text(x)

/* obs colours are 0xAABBGGRR */
#define RGB_(r, g, b)  (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* clang-format on */

/* The luma wipe's field is brightness times this much of the canvas height, so
 * its bands and feather come out near enough to pixels across a full range. */
#define MATTE_LUMA_SPAN 0.5f

/* The longest pause allowed, so the front always has time to move. */
#define MATTE_PAUSE_MAX 0.8f

static const char *const echo_setting[MATTE_ECHOES] = {
	S_ECHO_COLOR1,
	S_ECHO_COLOR2,
	S_ECHO_COLOR3,
	S_ECHO_COLOR4,
};

static const char *const echo_label[MATTE_ECHOES] = {
	"Matte.EchoColor1",
	"Matte.EchoColor2",
	"Matte.EchoColor3",
	"Matte.EchoColor4",
};

/* Preset ids are saved in scene collections, so they are permanent. */
enum matte_preset_id {
	MATTE_PRESET_IRIS_OUT = 1,
	MATTE_PRESET_CARTOON = 2,
	MATTE_PRESET_STAR = 3,
	MATTE_PRESET_HEXAGON = 4,
	MATTE_PRESET_HEART = 5,
	MATTE_PRESET_WAVE = 6,
	MATTE_PRESET_CLOCK = 7,
	MATTE_PRESET_BLINDS = 8,
};

/*
 * A whole look in one row, the way the glass transition does it: picking a
 * preset writes every column, and touching any of them afterwards puts the list
 * back to Custom. The centre and the image are left alone - they are where and
 * what, not how.
 *
 * The columns are named, and the rows below set them by name, because there
 * are too many for a row of bare numbers to stay readable.
 */
enum matte_column {
	MC_SHAPE,
	MC_DIRECTION,
	MC_STAR_POINTS,
	MC_STAR_DEPTH,
	MC_ARMS,
	MC_TURNS,
	MC_BARS,
	MC_WAVE_HEIGHT,
	MC_WAVE_LENGTH,
	MC_WAVE_DRIFT,
	MC_LOBES,
	MC_ANGLE,
	MC_SPIN,
	MC_STRETCH,
	MC_EASING,
	MC_OVERSHOOT,
	MC_PAUSE_SIZE,
	MC_PAUSE_LEN,
	MC_FEATHER,
	MC_BORDER,
	MC_BORDER_COLOR,
	MC_ECHO_COUNT,
	MC_ECHO_WIDTH,
	MC_ECHO_COLOR1,
	MC_ECHO_COLOR2,
	MC_ECHO_COLOR3,
	MC_ECHO_COLOR4,
	MC_GLOW,
	MC_GLOW_WIDTH,
	MC_GLOW_COLOR,
};

static const struct preset_column matte_columns[] = {
	[MC_SHAPE] = {S_SHAPE, PRESET_INT},
	[MC_DIRECTION] = {S_DIRECTION, PRESET_INT},
	[MC_STAR_POINTS] = {S_STAR_POINTS, PRESET_INT},
	[MC_STAR_DEPTH] = {S_STAR_DEPTH, PRESET_DOUBLE},
	[MC_ARMS] = {S_ARMS, PRESET_INT},
	[MC_TURNS] = {S_TURNS, PRESET_DOUBLE},
	[MC_BARS] = {S_BARS, PRESET_INT},
	[MC_WAVE_HEIGHT] = {S_WAVE_HEIGHT, PRESET_DOUBLE},
	[MC_WAVE_LENGTH] = {S_WAVE_LENGTH, PRESET_DOUBLE},
	[MC_WAVE_DRIFT] = {S_WAVE_DRIFT, PRESET_DOUBLE},
	[MC_LOBES] = {S_LOBES, PRESET_INT},
	[MC_ANGLE] = {S_ANGLE, PRESET_DOUBLE},
	[MC_SPIN] = {S_SPIN, PRESET_DOUBLE},
	[MC_STRETCH] = {S_STRETCH, PRESET_DOUBLE},
	[MC_EASING] = {S_EASING, PRESET_INT},
	[MC_OVERSHOOT] = {S_OVERSHOOT, PRESET_DOUBLE},
	[MC_PAUSE_SIZE] = {S_PAUSE_SIZE, PRESET_DOUBLE},
	[MC_PAUSE_LEN] = {S_PAUSE_LEN, PRESET_DOUBLE},
	[MC_FEATHER] = {S_FEATHER, PRESET_DOUBLE},
	[MC_BORDER] = {S_BORDER, PRESET_DOUBLE},
	[MC_BORDER_COLOR] = {S_BORDER_COLOR, PRESET_COLOR},
	[MC_ECHO_COUNT] = {S_ECHO_COUNT, PRESET_INT},
	[MC_ECHO_WIDTH] = {S_ECHO_WIDTH, PRESET_DOUBLE},
	[MC_ECHO_COLOR1] = {S_ECHO_COLOR1, PRESET_COLOR},
	[MC_ECHO_COLOR2] = {S_ECHO_COLOR2, PRESET_COLOR},
	[MC_ECHO_COLOR3] = {S_ECHO_COLOR3, PRESET_COLOR},
	[MC_ECHO_COLOR4] = {S_ECHO_COLOR4, PRESET_COLOR},
	[MC_GLOW] = {S_GLOW, PRESET_DOUBLE},
	[MC_GLOW_WIDTH] = {S_GLOW_WIDTH, PRESET_DOUBLE},
	[MC_GLOW_COLOR] = {S_GLOW_COLOR, PRESET_COLOR},
};

#define WHITE RGB_(0xFF, 0xFF, 0xFF)
#define BLACK RGB_(0x00, 0x00, 0x00)
#define CYAN RGB_(0x00, 0xB4, 0xFF)
#define PURPLE RGB_(0x7A, 0x3F, 0xD0)
#define PINK RGB_(0xFF, 0x3D, 0x7F)
#define AMBER RGB_(0xFF, 0xB1, 0x3D)

/* clang-format off */

static const struct preset matte_presets[] = {
	{
		.id = MATTE_PRESET_IRIS_OUT,
		.label = "Matte.Preset.IrisOut",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_CIRCLE, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_IN_OUT, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 1,
			[MC_BORDER] = 0, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 0, [MC_ECHO_WIDTH] = 60,
			[MC_ECHO_COLOR1] = CYAN, [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
	{
		.id = MATTE_PRESET_CARTOON,
		.label = "Matte.Preset.Cartoon",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_CIRCLE, [MC_DIRECTION] = MATTE_DIR_IN,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_OVERSHOOT, [MC_OVERSHOOT] = 60,
			[MC_PAUSE_SIZE] = 22, [MC_PAUSE_LEN] = 30, [MC_FEATHER] = 1,
			[MC_BORDER] = 8, [MC_BORDER_COLOR] = BLACK,
			[MC_ECHO_COUNT] = 0, [MC_ECHO_WIDTH] = 60,
			[MC_ECHO_COLOR1] = CYAN, [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
	{
		.id = MATTE_PRESET_STAR,
		.label = "Matte.Preset.Star",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_STAR, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0.35, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_OUT, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 2,
			[MC_BORDER] = 14, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 2, [MC_ECHO_WIDTH] = 70,
			[MC_ECHO_COLOR1] = CYAN, [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
	{
		.id = MATTE_PRESET_HEXAGON,
		.label = "Matte.Preset.Hexagon",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_HEXAGON, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 1.0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_IN_OUT, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 2,
			[MC_BORDER] = 0, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 3, [MC_ECHO_WIDTH] = 50,
			[MC_ECHO_COLOR1] = PINK, [MC_ECHO_COLOR2] = AMBER, [MC_ECHO_COLOR3] = CYAN, [MC_ECHO_COLOR4] = PURPLE,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
	{
		.id = MATTE_PRESET_HEART,
		.label = "Matte.Preset.Heart",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_HEART, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_OVERSHOOT, [MC_OVERSHOOT] = 80,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 25, [MC_FEATHER] = 2,
			[MC_BORDER] = 10, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 1, [MC_ECHO_WIDTH] = 30,
			[MC_ECHO_COLOR1] = RGB_(0xFF, 0x4D, 0x88), [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 60, [MC_GLOW_WIDTH] = 50, [MC_GLOW_COLOR] = RGB_(0xFF, 0x9E, 0xC4),
		},
	},
	{
		.id = MATTE_PRESET_WAVE,
		.label = "Matte.Preset.Wave",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_WAVE, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.5, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_IN_OUT, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 2,
			[MC_BORDER] = 18, [MC_BORDER_COLOR] = RGB_(0xE8, 0xFB, 0xFF),
			[MC_ECHO_COUNT] = 2, [MC_ECHO_WIDTH] = 45,
			[MC_ECHO_COLOR1] = RGB_(0x29, 0xB6, 0xF6), [MC_ECHO_COLOR2] = RGB_(0x0D, 0x47, 0xA1), [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
	{
		.id = MATTE_PRESET_CLOCK,
		.label = "Matte.Preset.Clock",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_CLOCK, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_LINEAR, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 1,
			[MC_BORDER] = 8, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 0, [MC_ECHO_WIDTH] = 60,
			[MC_ECHO_COLOR1] = CYAN, [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 80, [MC_GLOW_WIDTH] = 30, [MC_GLOW_COLOR] = CYAN,
		},
	},
	{
		.id = MATTE_PRESET_BLINDS,
		.label = "Matte.Preset.Blinds",
		.value = {
			[MC_SHAPE] = MATTE_SHAPE_BLINDS, [MC_DIRECTION] = MATTE_DIR_OUT,
			[MC_STAR_POINTS] = 5, [MC_STAR_DEPTH] = 45, [MC_ARMS] = 1, [MC_TURNS] = 3, [MC_BARS] = 8,
			[MC_WAVE_HEIGHT] = 60, [MC_WAVE_LENGTH] = 420, [MC_WAVE_DRIFT] = 1.0, [MC_LOBES] = 8,
			[MC_ANGLE] = 0, [MC_SPIN] = 0, [MC_STRETCH] = 100,
			[MC_EASING] = MATTE_EASE_IN_OUT, [MC_OVERSHOOT] = 50,
			[MC_PAUSE_SIZE] = 30, [MC_PAUSE_LEN] = 0, [MC_FEATHER] = 1,
			[MC_BORDER] = 6, [MC_BORDER_COLOR] = WHITE,
			[MC_ECHO_COUNT] = 0, [MC_ECHO_WIDTH] = 60,
			[MC_ECHO_COLOR1] = CYAN, [MC_ECHO_COLOR2] = PURPLE, [MC_ECHO_COLOR3] = PINK, [MC_ECHO_COLOR4] = AMBER,
			[MC_GLOW] = 0, [MC_GLOW_WIDTH] = 40, [MC_GLOW_COLOR] = WHITE,
		},
	},
};

/* clang-format on */

static const struct preset_set matte_preset_set = {
	.columns = matte_columns,
	.column_count = TC_COUNT(matte_columns),
	.presets = matte_presets,
	.count = TC_COUNT(matte_presets),
	.custom_label = "Matte.Preset.Custom",
};

struct matte_info {
	struct transition_base base;

	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_luma_tex;
	gs_eparam_t *ep_profile_tex;
	gs_eparam_t *ep_canvas;
	gs_eparam_t *ep_origin_px;
	gs_eparam_t *ep_rot;
	gs_eparam_t *ep_inv_stretch;
	gs_eparam_t *ep_shape;
	gs_eparam_t *ep_star_n;
	gs_eparam_t *ep_star_k;
	gs_eparam_t *ep_arms;
	gs_eparam_t *ep_pitch;
	gs_eparam_t *ep_amp;
	gs_eparam_t *ep_wavelen;
	gs_eparam_t *ep_lobes;
	gs_eparam_t *ep_bar_px;
	gs_eparam_t *ep_phase;
	gs_eparam_t *ep_luma_scale;
	gs_eparam_t *ep_front;
	gs_eparam_t *ep_dir_sign;
	gs_eparam_t *ep_feather;
	gs_eparam_t *ep_border;
	gs_eparam_t *ep_echo;
	gs_eparam_t *ep_echo_count;
	gs_eparam_t *ep_band_end;
	gs_eparam_t *ep_glow;
	gs_eparam_t *ep_glow_amt;
	gs_eparam_t *ep_color_border;
	gs_eparam_t *ep_color_echo[MATTE_ECHOES];
	gs_eparam_t *ep_color_glow;

	/* settings */
	int shape;
	int direction;
	float star_points;
	float star_depth;
	float arms;
	float turns;
	float bars;
	float wave_height;
	float wave_length;
	float wave_drift;
	float lobes;
	float angle;    /* radians */
	float spin;     /* turns over the transition */
	float stretch;  /* width over height */
	float origin_x; /* 0..1 of the canvas */
	float origin_y;
	struct matte_timing timing;
	struct matte_bands bands;
	float glow_amt;

	struct tc_color border_color;
	struct tc_color echo_color[MATTE_ECHOES];
	struct tc_color glow_color;

	/* mask image - owned by the graphics thread once adopted */
	struct dstr image_path;
	gs_image_file_ex_t image;
	bool have_image;
	bool have_profile;
	gs_texture_t *profile_tex;
	float profile[MATTE_PROFILE_SIZE];
	float luma_lo, luma_hi;

	/*
	 * Mask images are decoded on a thread of their own, because settings
	 * changes reach a transition on the graphics thread, and decoding a large
	 * image there would drop frames. Each request bumps the generation; a
	 * decode that finishes for an older one is thrown away. A finished one
	 * waits in mask_ready until the next frame adopts it.
	 */
	os_task_queue_t *loader;
	volatile long load_generation;
	pthread_mutex_t mask_mutex;
	struct matte_mask *mask_ready;

	/* the field's range, cached against the canvas and the settings */
	bool range_valid;
	uint32_t range_cx, range_cy;
	float range_lo, range_hi;
};

static const char *matte_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Matte.Transition");
}

/* ------------------------------------------------------------------ */
/* mask image                                                         */
/* ------------------------------------------------------------------ */

/* Takes a decoded image's pixels to tightly packed RGBA, whichever of the two
 * byte orders it came in. Returns NULL for anything else. */
static uint8_t *image_rgba(const gs_image_file_ex_t *img)
{
	size_t n = (size_t)img->cx * (size_t)img->cy;
	uint8_t *out;
	size_t i;

	if (!img->texture_data || n == 0)
		return NULL;
	if (img->format != GS_RGBA && img->format != GS_BGRA && img->format != GS_BGRX)
		return NULL;

	out = bmalloc(n * 4);
	memcpy(out, img->texture_data, n * 4);

	if (img->format != GS_RGBA) {
		for (i = 0; i < n; i++) {
			uint8_t t = out[i * 4];

			out[i * 4] = out[i * 4 + 2];
			out[i * 4 + 2] = t;
			if (img->format == GS_BGRX)
				out[i * 4 + 3] = 255;
		}
	}

	return out;
}

/* The brightness range the luma wipe has to run over, weighted the way the
 * shader weights it, alpha included. */
static void luma_range(const uint8_t *rgba, size_t n, float *lo, float *hi)
{
	size_t i;

	*lo = 1.0f;
	*hi = 0.0f;

	for (i = 0; i < n; i++) {
		const uint8_t *p = rgba + i * 4;
		float l = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f * (p[3] / 255.0f);

		*lo = fminf(*lo, l);
		*hi = fmaxf(*hi, l);
	}
}

/* A decoded mask image and everything worked out from it: all the render
 * needs from the file except its textures, which only the graphics thread can
 * make. */
struct matte_mask {
	gs_image_file_ex_t image;
	bool have_profile;
	float profile[MATTE_PROFILE_SIZE];
	float luma_lo, luma_hi;
};

/* Decodes and measures an image. Touches no graphics state, so it can run on
 * any thread. An empty path gives an empty mask, which is no image. */
static struct matte_mask *matte_decode_mask(const char *path)
{
	struct matte_mask *mask = bzalloc(sizeof(*mask));
	uint8_t *rgba;

	mask->luma_lo = 0.0f;
	mask->luma_hi = 1.0f;

	if (path && *path) {
		gs_image_file_ex_init(&mask->image, path, GS_IMAGE_ALPHA_STRAIGHT);

		if (!mask->image.loaded)
			obs_log(LOG_WARNING, "matte: could not load mask image '%s'", path);
	}

	rgba = mask->image.loaded ? image_rgba(&mask->image) : NULL;
	if (rgba) {
		mask->have_profile = matte_build_profile(rgba, mask->image.cx, mask->image.cy, mask->profile);
		luma_range(rgba, (size_t)mask->image.cx * (size_t)mask->image.cy, &mask->luma_lo, &mask->luma_hi);
		bfree(rgba);

		if (!mask->have_profile)
			obs_log(LOG_WARNING, "matte: mask image '%s' has nothing in it to cut a shape from", path);
	}

	return mask;
}

/* Frees a mask that was never adopted. Takes the graphics context itself, as
 * freeing a decoded image goes through the texture it may have. */
static void matte_mask_free(struct matte_mask *mask)
{
	if (!mask)
		return;

	obs_enter_graphics();
	gs_image_file_ex_free(&mask->image);
	obs_leave_graphics();
	bfree(mask);
}

/*
 * Makes a decoded mask the one the render reads, creating its textures and
 * freeing the old ones. Graphics context required, which is also what keeps a
 * frame from seeing half of an old image and half of a new one.
 */
static void matte_adopt_mask(struct matte_info *matte, struct matte_mask *mask)
{
	gs_texture_t *profile_tex = NULL;

	if (mask->image.loaded)
		gs_image_file_ex_init_texture(&mask->image);

	if (mask->have_profile) {
		const uint8_t *data = (const uint8_t *)mask->profile;

		profile_tex = gs_texture_create(MATTE_PROFILE_SIZE, 1, GS_R32F, 1, &data, 0);
	}

	gs_image_file_ex_free(&matte->image);
	gs_texture_destroy(matte->profile_tex);

	matte->image = mask->image;
	matte->have_image = mask->image.loaded && mask->image.texture != NULL;
	matte->profile_tex = profile_tex;
	matte->have_profile = mask->have_profile && profile_tex != NULL;
	if (mask->have_profile)
		memcpy(matte->profile, mask->profile, sizeof(matte->profile));
	matte->luma_lo = mask->luma_lo;
	matte->luma_hi = fmaxf(mask->luma_hi, mask->luma_lo + 0.001f);
	matte->range_valid = false;

	/* the image itself has moved into matte->image */
	bfree(mask);
}

/* Adopts a mask the loader has finished, if there is one. Graphics context
 * required. */
static void matte_take_mask(struct matte_info *matte)
{
	struct matte_mask *mask;

	pthread_mutex_lock(&matte->mask_mutex);
	mask = matte->mask_ready;
	matte->mask_ready = NULL;
	pthread_mutex_unlock(&matte->mask_mutex);

	if (mask)
		matte_adopt_mask(matte, mask);
}

struct matte_load_job {
	struct matte_info *matte;
	char *path;
	long generation;
};

static void matte_load_task(void *param)
{
	struct matte_load_job *job = param;
	struct matte_info *matte = job->matte;
	struct matte_mask *mask = NULL;
	struct matte_mask *stale = NULL;

	/* superseded before it started, so not worth decoding */
	if (os_atomic_load_long(&matte->load_generation) == job->generation)
		mask = matte_decode_mask(job->path);

	if (mask) {
		pthread_mutex_lock(&matte->mask_mutex);
		if (os_atomic_load_long(&matte->load_generation) == job->generation) {
			stale = matte->mask_ready;
			matte->mask_ready = mask;
			mask = NULL;
		}
		pthread_mutex_unlock(&matte->mask_mutex);
	}

	matte_mask_free(mask);
	matte_mask_free(stale);
	bfree(job->path);
	bfree(job);
}

/*
 * Starts loading the image at path, replacing whatever is loading already.
 *
 * Before the loader exists - inside create, which is not on the graphics
 * thread - the image is loaded there and then instead, so a transition is
 * never first shown with its image missing.
 */
static void matte_request_mask(struct matte_info *matte, const char *path)
{
	struct matte_load_job *job;

	if (!matte->loader) {
		struct matte_mask *mask = matte_decode_mask(path);

		obs_enter_graphics();
		matte_adopt_mask(matte, mask);
		obs_leave_graphics();
		return;
	}

	job = bzalloc(sizeof(*job));
	job->matte = matte;
	job->path = bstrdup(path);
	job->generation = os_atomic_inc_long(&matte->load_generation);

	if (!os_task_queue_queue_task(matte->loader, matte_load_task, job)) {
		bfree(job->path);
		bfree(job);
	}
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */

static void matte_update(void *data, obs_data_t *settings)
{
	struct matte_info *matte = data;
	const char *path = obs_data_get_string(settings, S_IMAGE);
	float pause_size;
	int i;

	matte->shape = (int)obs_data_get_int(settings, S_SHAPE);
	if (matte->shape < 0 || matte->shape >= MATTE_SHAPE_COUNT)
		matte->shape = MATTE_SHAPE_CIRCLE;

	matte->direction = (int)obs_data_get_int(settings, S_DIRECTION);
	matte->star_points = clampf((float)obs_data_get_int(settings, S_STAR_POINTS), 3.0f, 12.0f);
	matte->star_depth = clampf((float)obs_data_get_double(settings, S_STAR_DEPTH) / 100.0f, 0.1f, 0.95f);
	matte->arms = clampf((float)obs_data_get_int(settings, S_ARMS), 1.0f, 12.0f);
	matte->turns = clampf((float)obs_data_get_double(settings, S_TURNS), 0.5f, 12.0f);
	matte->bars = clampf((float)obs_data_get_int(settings, S_BARS), 1.0f, 64.0f);
	matte->wave_height = fmaxf((float)obs_data_get_double(settings, S_WAVE_HEIGHT), 0.0f);
	matte->wave_length = fmaxf((float)obs_data_get_double(settings, S_WAVE_LENGTH), 8.0f);
	matte->wave_drift = (float)obs_data_get_double(settings, S_WAVE_DRIFT);
	matte->lobes = clampf((float)obs_data_get_int(settings, S_LOBES), 1.0f, 32.0f);

	/* The curl's lip hangs over the steep face of the crest. A wavelength too
	 * short to hold it would cut it off at the next wave along, so the waves
	 * are spread out to fit rather than letting the lip be clipped flat. */
	if (matte->shape == MATTE_SHAPE_CURL)
		matte->wave_length = fmaxf(matte->wave_length, MATTE_CURL_MIN_LENGTH * matte->wave_height);

	matte->angle = (float)obs_data_get_double(settings, S_ANGLE) * (float)M_PI / 180.0f;
	matte->spin = (float)obs_data_get_double(settings, S_SPIN);
	matte->stretch = clampf((float)obs_data_get_double(settings, S_STRETCH) / 100.0f, 0.1f, 10.0f);
	matte->origin_x = (float)obs_data_get_double(settings, S_ORIGIN_X) / 100.0f;
	matte->origin_y = (float)obs_data_get_double(settings, S_ORIGIN_Y) / 100.0f;

	matte->timing.easing = (int)obs_data_get_int(settings, S_EASING);
	matte->timing.overshoot = (float)obs_data_get_double(settings, S_OVERSHOOT) / 100.0f;
	matte->timing.pause_len =
		clampf((float)obs_data_get_double(settings, S_PAUSE_LEN) / 100.0f, 0.0f, MATTE_PAUSE_MAX);

	/* The pause is set as the size the shape holds at. Outside in, the shape
	 * is the outgoing scene shrinking, so its size is what is left of the
	 * travel rather than what has been covered. */
	pause_size = clampf((float)obs_data_get_double(settings, S_PAUSE_SIZE) / 100.0f, 0.0f, 1.0f);
	matte->timing.pause_at = matte->direction == MATTE_DIR_IN ? 1.0f - pause_size : pause_size;

	matte->bands.feather = fmaxf((float)obs_data_get_double(settings, S_FEATHER), 1.0f);
	matte->bands.border = fmaxf((float)obs_data_get_double(settings, S_BORDER), 0.0f);
	matte->bands.echo_count = (int)obs_data_get_int(settings, S_ECHO_COUNT);
	if (matte->bands.echo_count < 0)
		matte->bands.echo_count = 0;
	if (matte->bands.echo_count > MATTE_ECHOES)
		matte->bands.echo_count = MATTE_ECHOES;
	matte->bands.echo = fmaxf((float)obs_data_get_double(settings, S_ECHO_WIDTH), 0.0f);
	if (matte->bands.echo <= 0.0f)
		matte->bands.echo_count = 0;

	matte->glow_amt = fmaxf((float)obs_data_get_double(settings, S_GLOW) / 100.0f, 0.0f);
	matte->bands.glow = matte->glow_amt > 0.0f ? fmaxf((float)obs_data_get_double(settings, S_GLOW_WIDTH), 1.0f)
						   : 0.0f;

	tc_color_from_setting(&matte->border_color, settings, S_BORDER_COLOR);
	tc_color_from_setting(&matte->glow_color, settings, S_GLOW_COLOR);
	for (i = 0; i < MATTE_ECHOES; i++)
		tc_color_from_setting(&matte->echo_color[i], settings, echo_setting[i]);

	matte->range_valid = false;

	/* Only reload when the path itself changed: every slider move comes
	 * through here, and decoding the image again for each would stall it. */
	if (!path)
		path = "";

	/* an emptied dstr holds NULL rather than "", and that is still no image */
	if (strcmp(matte->image_path.array ? matte->image_path.array : "", path) != 0) {
		dstr_copy(&matte->image_path, path);
		matte_request_mask(matte, path);
	}
}

/* clang-format off */

static const struct tc_param matte_params[] = {
	TC_PARAM(struct matte_info, ep_a_tex, "a_tex"),
	TC_PARAM(struct matte_info, ep_b_tex, "b_tex"),
	TC_PARAM(struct matte_info, ep_luma_tex, "luma_tex"),
	TC_PARAM(struct matte_info, ep_profile_tex, "profile_tex"),
	TC_PARAM(struct matte_info, ep_canvas, "canvas"),
	TC_PARAM(struct matte_info, ep_origin_px, "origin_px"),
	TC_PARAM(struct matte_info, ep_rot, "rot"),
	TC_PARAM(struct matte_info, ep_inv_stretch, "inv_stretch"),
	TC_PARAM(struct matte_info, ep_shape, "shape"),
	TC_PARAM(struct matte_info, ep_star_n, "star_n"),
	TC_PARAM(struct matte_info, ep_star_k, "star_k"),
	TC_PARAM(struct matte_info, ep_arms, "arms"),
	TC_PARAM(struct matte_info, ep_pitch, "pitch"),
	TC_PARAM(struct matte_info, ep_amp, "amp"),
	TC_PARAM(struct matte_info, ep_wavelen, "wavelen"),
	TC_PARAM(struct matte_info, ep_lobes, "lobes"),
	TC_PARAM(struct matte_info, ep_bar_px, "bar_px"),
	TC_PARAM(struct matte_info, ep_phase, "phase"),
	TC_PARAM(struct matte_info, ep_luma_scale, "luma_scale"),
	TC_PARAM(struct matte_info, ep_front, "front"),
	TC_PARAM(struct matte_info, ep_dir_sign, "dir_sign"),
	TC_PARAM(struct matte_info, ep_feather, "feather"),
	TC_PARAM(struct matte_info, ep_border, "border"),
	TC_PARAM(struct matte_info, ep_echo, "echo"),
	TC_PARAM(struct matte_info, ep_echo_count, "echo_count"),
	TC_PARAM(struct matte_info, ep_band_end, "band_end"),
	TC_PARAM(struct matte_info, ep_glow, "glow"),
	TC_PARAM(struct matte_info, ep_glow_amt, "glow_amt"),
	TC_PARAM(struct matte_info, ep_color_border, "color_border"),
	TC_PARAM(struct matte_info, ep_color_glow, "color_glow"),
	TC_PARAM(struct matte_info, ep_color_echo[0], "color_echo1"),
	TC_PARAM(struct matte_info, ep_color_echo[1], "color_echo2"),
	TC_PARAM(struct matte_info, ep_color_echo[2], "color_echo3"),
	TC_PARAM(struct matte_info, ep_color_echo[3], "color_echo4"),
};

/* clang-format on */

static void *matte_create(obs_data_t *settings, obs_source_t *source)
{
	struct matte_info *matte = bzalloc(sizeof(struct matte_info));

	if (!tc_base_init(&matte->base, source, "matte_transition.effect", matte_params, TC_COUNT(matte_params))) {
		bfree(matte);
		return NULL;
	}

	dstr_init(&matte->image_path);
	pthread_mutex_init_value(&matte->mask_mutex);
	if (pthread_mutex_init(&matte->mask_mutex, NULL) != 0) {
		tc_base_free(&matte->base);
		bfree(matte);
		return NULL;
	}

	/* Applied directly, as the other transitions do: the source's data
	 * pointer is only assigned once create returns. The loader is started
	 * after, so the first image is loaded here rather than handed off. */
	matte_update(matte, settings);
	matte->loader = os_task_queue_create();

	return matte;
}

static void matte_destroy(void *data)
{
	struct matte_info *matte = data;

	/* Runs any load still queued to completion first, so nothing is left
	 * to write into the struct once it is gone. */
	os_task_queue_destroy(matte->loader);
	matte_mask_free(matte->mask_ready);

	obs_enter_graphics();
	gs_image_file_ex_free(&matte->image);
	gs_texture_destroy(matte->profile_tex);
	obs_leave_graphics();

	pthread_mutex_destroy(&matte->mask_mutex);
	tc_base_free(&matte->base);
	dstr_free(&matte->image_path);
	bfree(matte);
}

/* ------------------------------------------------------------------ */
/* presets                                                            */
/* ------------------------------------------------------------------ */

static void matte_defaults(obs_data_t *settings)
{
	/* the first preset is the default look */
	preset_set_defaults(&matte_preset_set, &matte_presets[0], settings);

	obs_data_set_default_string(settings, S_IMAGE, "");
	obs_data_set_default_double(settings, S_ORIGIN_X, 50.0);
	obs_data_set_default_double(settings, S_ORIGIN_Y, 50.0);
}

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static void set_visible(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, name), visible);
}

static void matte_layout(obs_properties_t *props, obs_data_t *settings)
{
	int shape = (int)obs_data_get_int(settings, S_SHAPE);
	int echoes = (int)obs_data_get_int(settings, S_ECHO_COUNT);
	bool sweep = matte_is_sweep(shape);
	bool luma = shape == MATTE_SHAPE_LUMA;
	bool wave = sweep || shape == MATTE_SHAPE_RIPPLE;
	int i;

	set_visible(props, S_STAR_POINTS, shape == MATTE_SHAPE_STAR);
	set_visible(props, S_STAR_DEPTH, shape == MATTE_SHAPE_STAR);
	set_visible(props, S_ARMS, shape == MATTE_SHAPE_CLOCK || shape == MATTE_SHAPE_SPIRAL);
	set_visible(props, S_TURNS, shape == MATTE_SHAPE_SPIRAL);
	set_visible(props, S_BARS, shape == MATTE_SHAPE_BLINDS);
	set_visible(props, S_WAVE_HEIGHT, wave);
	set_visible(props, S_WAVE_LENGTH, sweep);
	set_visible(props, S_WAVE_DRIFT, wave);
	set_visible(props, S_LOBES, shape == MATTE_SHAPE_RIPPLE);
	set_visible(props, S_IMAGE, shape == MATTE_SHAPE_IMAGE || luma);

	/* A sweep has no centre to grow from and the luma wipe is laid over the
	 * canvas as it is, so neither has a use for these. */
	set_visible(props, S_ORIGIN_X, !sweep && !luma && shape != MATTE_SHAPE_BLINDS);
	set_visible(props, S_ORIGIN_Y, !sweep && !luma && shape != MATTE_SHAPE_BLINDS);
	set_visible(props, S_ANGLE, !luma);
	set_visible(props, S_SPIN, !luma);
	set_visible(props, S_STRETCH, !luma && !sweep && shape != MATTE_SHAPE_BLINDS);

	set_visible(props, S_OVERSHOOT, (int)obs_data_get_int(settings, S_EASING) == MATTE_EASE_OVERSHOOT);
	set_visible(props, S_PAUSE_SIZE, obs_data_get_double(settings, S_PAUSE_LEN) > 0.0);
	set_visible(props, S_BORDER_COLOR, obs_data_get_double(settings, S_BORDER) > 0.0);
	set_visible(props, S_ECHO_WIDTH, echoes > 0);
	for (i = 0; i < MATTE_ECHOES; i++)
		set_visible(props, echo_setting[i], echoes > i);
	set_visible(props, S_GLOW_WIDTH, obs_data_get_double(settings, S_GLOW) > 0.0);
	set_visible(props, S_GLOW_COLOR, obs_data_get_double(settings, S_GLOW) > 0.0);
}

static bool matte_preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	preset_selected(&matte_preset_set, settings);

	matte_layout(props, settings);
	return true;
}

/* Every control but the preset list and the where-and-what settings runs
 * through here, and drops the preset back to Custom once the settings have
 * left it. */
static bool matte_touched(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	preset_touched(&matte_preset_set, settings);

	matte_layout(props, settings);
	return true;
}

static obs_property_t *watched(obs_property_t *prop)
{
	obs_property_set_modified_callback(prop, matte_touched);
	return prop;
}

static obs_property_t *percent(obs_property_t *prop)
{
	obs_property_float_set_suffix(prop, " %");
	return prop;
}

static obs_property_t *pixels(obs_property_t *prop)
{
	obs_property_float_set_suffix(prop, " px");
	return prop;
}

static obs_properties_t *matte_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	int i;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_list(props, S_PRESET, T_("Matte.Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	preset_fill_list(&matte_preset_set, p);
	obs_property_set_long_description(p, T_("Matte.Preset.Description"));
	obs_property_set_modified_callback(p, matte_preset_modified);

	p = watched(
		obs_properties_add_list(props, S_SHAPE, T_("Matte.Shape"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
	obs_property_list_add_int(p, T_("Matte.Shape.Circle"), MATTE_SHAPE_CIRCLE);
	obs_property_list_add_int(p, T_("Matte.Shape.Square"), MATTE_SHAPE_SQUARE);
	obs_property_list_add_int(p, T_("Matte.Shape.Triangle"), MATTE_SHAPE_TRIANGLE);
	obs_property_list_add_int(p, T_("Matte.Shape.Diamond"), MATTE_SHAPE_DIAMOND);
	obs_property_list_add_int(p, T_("Matte.Shape.Hexagon"), MATTE_SHAPE_HEXAGON);
	obs_property_list_add_int(p, T_("Matte.Shape.Octagon"), MATTE_SHAPE_OCTAGON);
	obs_property_list_add_int(p, T_("Matte.Shape.Star"), MATTE_SHAPE_STAR);
	obs_property_list_add_int(p, T_("Matte.Shape.Heart"), MATTE_SHAPE_HEART);
	obs_property_list_add_int(p, T_("Matte.Shape.Cross"), MATTE_SHAPE_CROSS);
	obs_property_list_add_int(p, T_("Matte.Shape.Wave"), MATTE_SHAPE_WAVE);
	obs_property_list_add_int(p, T_("Matte.Shape.Curl"), MATTE_SHAPE_CURL);
	obs_property_list_add_int(p, T_("Matte.Shape.Ripple"), MATTE_SHAPE_RIPPLE);
	obs_property_list_add_int(p, T_("Matte.Shape.Clock"), MATTE_SHAPE_CLOCK);
	obs_property_list_add_int(p, T_("Matte.Shape.Spiral"), MATTE_SHAPE_SPIRAL);
	obs_property_list_add_int(p, T_("Matte.Shape.Blinds"), MATTE_SHAPE_BLINDS);
	obs_property_list_add_int(p, T_("Matte.Shape.Zigzag"), MATTE_SHAPE_ZIGZAG);
	obs_property_list_add_int(p, T_("Matte.Shape.Image"), MATTE_SHAPE_IMAGE);
	obs_property_list_add_int(p, T_("Matte.Shape.Luma"), MATTE_SHAPE_LUMA);

	p = watched(obs_properties_add_list(props, S_DIRECTION, T_("Matte.Direction"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT));
	obs_property_list_add_int(p, T_("Matte.Direction.Out"), MATTE_DIR_OUT);
	obs_property_list_add_int(p, T_("Matte.Direction.In"), MATTE_DIR_IN);
	obs_property_set_long_description(p, T_("Matte.Direction.Description"));

	p = obs_properties_add_path(props, S_IMAGE, T_("Matte.Image"), OBS_PATH_FILE,
				    "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;All files (*.*)", NULL);
	obs_property_set_long_description(p, T_("Matte.Image.Description"));

	watched(obs_properties_add_int_slider(props, S_STAR_POINTS, T_("Matte.StarPoints"), 3, 12, 1));
	p = percent(
		watched(obs_properties_add_float_slider(props, S_STAR_DEPTH, T_("Matte.StarDepth"), 15.0, 90.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.StarDepth.Description"));

	watched(obs_properties_add_int_slider(props, S_ARMS, T_("Matte.Segments"), 1, 12, 1));
	watched(obs_properties_add_float_slider(props, S_TURNS, T_("Matte.SpiralTurns"), 0.5, 12.0, 0.5));
	watched(obs_properties_add_int_slider(props, S_BARS, T_("Matte.Bars"), 2, 40, 1));

	pixels(watched(obs_properties_add_float_slider(props, S_WAVE_HEIGHT, T_("Matte.WaveHeight"), 0.0, 400.0, 1.0)));
	pixels(watched(
		obs_properties_add_float_slider(props, S_WAVE_LENGTH, T_("Matte.WaveLength"), 20.0, 2000.0, 1.0)));
	p = watched(obs_properties_add_float_slider(props, S_WAVE_DRIFT, T_("Matte.WaveDrift"), -4.0, 4.0, 0.1));
	obs_property_set_long_description(p, T_("Matte.WaveDrift.Description"));
	watched(obs_properties_add_int_slider(props, S_LOBES, T_("Matte.Ripples"), 2, 24, 1));

	p = watched(obs_properties_add_float_slider(props, S_ANGLE, T_("Matte.Angle"), 0.0, 359.0, 1.0));
	obs_property_float_set_suffix(p, "°");
	obs_property_set_long_description(p, T_("Matte.Angle.Description"));

	p = watched(obs_properties_add_float_slider(props, S_SPIN, T_("Matte.Spin"), -4.0, 4.0, 0.05));
	obs_property_set_long_description(p, T_("Matte.Spin.Description"));

	p = percent(watched(obs_properties_add_float_slider(props, S_STRETCH, T_("Matte.Stretch"), 25.0, 400.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.Stretch.Description"));

	percent(obs_properties_add_float_slider(props, S_ORIGIN_X, T_("Matte.OriginX"), -50.0, 150.0, 0.5));
	percent(obs_properties_add_float_slider(props, S_ORIGIN_Y, T_("Matte.OriginY"), -50.0, 150.0, 0.5));

	p = watched(obs_properties_add_list(props, S_EASING, T_("Matte.Easing"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT));
	obs_property_list_add_int(p, T_("Matte.Easing.Linear"), MATTE_EASE_LINEAR);
	obs_property_list_add_int(p, T_("Matte.Easing.InOut"), MATTE_EASE_IN_OUT);
	obs_property_list_add_int(p, T_("Matte.Easing.Out"), MATTE_EASE_OUT);
	obs_property_list_add_int(p, T_("Matte.Easing.In"), MATTE_EASE_IN);
	obs_property_list_add_int(p, T_("Matte.Easing.Overshoot"), MATTE_EASE_OVERSHOOT);

	percent(watched(obs_properties_add_float_slider(props, S_OVERSHOOT, T_("Matte.Overshoot"), 0.0, 100.0, 1.0)));

	p = percent(
		watched(obs_properties_add_float_slider(props, S_PAUSE_LEN, T_("Matte.PauseLength"), 0.0, 80.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.PauseLength.Description"));

	p = percent(
		watched(obs_properties_add_float_slider(props, S_PAUSE_SIZE, T_("Matte.PauseSize"), 0.0, 100.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.PauseSize.Description"));

	p = pixels(watched(obs_properties_add_float_slider(props, S_FEATHER, T_("Matte.Feather"), 0.0, 200.0, 0.5)));
	obs_property_set_long_description(p, T_("Matte.Feather.Description"));

	p = pixels(watched(obs_properties_add_float_slider(props, S_BORDER, T_("Matte.Border"), 0.0, 400.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.Border.Description"));
	watched(obs_properties_add_color(props, S_BORDER_COLOR, T_("Matte.BorderColor")));

	p = watched(obs_properties_add_int_slider(props, S_ECHO_COUNT, T_("Matte.EchoCount"), 0, MATTE_ECHOES, 1));
	obs_property_set_long_description(p, T_("Matte.EchoCount.Description"));
	pixels(watched(obs_properties_add_float_slider(props, S_ECHO_WIDTH, T_("Matte.EchoWidth"), 1.0, 600.0, 1.0)));
	for (i = 0; i < MATTE_ECHOES; i++)
		watched(obs_properties_add_color(props, echo_setting[i], T_(echo_label[i])));

	p = percent(watched(obs_properties_add_float_slider(props, S_GLOW, T_("Matte.Glow"), 0.0, 300.0, 1.0)));
	obs_property_set_long_description(p, T_("Matte.Glow.Description"));
	pixels(watched(obs_properties_add_float_slider(props, S_GLOW_WIDTH, T_("Matte.GlowWidth"), 1.0, 300.0, 1.0)));
	watched(obs_properties_add_color(props, S_GLOW_COLOR, T_("Matte.GlowColor")));

	return props;
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */

/* The shape the shader is asked to draw: an image shape with no image to cut
 * it from falls back to a circle rather than to nothing. */
static int matte_effective_shape(const struct matte_info *matte)
{
	if (matte->shape == MATTE_SHAPE_IMAGE && !matte->have_profile)
		return MATTE_SHAPE_CIRCLE;
	if (matte->shape == MATTE_SHAPE_LUMA && !matte->have_image)
		return MATTE_SHAPE_CIRCLE;
	return matte->shape;
}

/* Furthest canvas corner from the origin, the radius the clock and spiral are
 * paced against. */
static float corner_reach(float ox, float oy, float cx, float cy)
{
	float far_x = fmaxf(fabsf(ox), fabsf(cx - ox));
	float far_y = fmaxf(fabsf(oy), fabsf(cy - oy));

	return fmaxf(sqrtf(far_x * far_x + far_y * far_y), 1.0f);
}

static void matte_shape_params(const struct matte_info *matte, int shape, const struct matte_frame *f, float cx,
			       float cy, struct matte_shape_params *sp)
{
	float reach = corner_reach(f->origin_x, f->origin_y, cx, cy);

	memset(sp, 0, sizeof(*sp));
	sp->shape = shape;
	sp->star_n = matte->star_points;
	sp->star_k = matte_star_slope(matte->star_points, matte->star_depth);
	sp->arms = matte->arms;
	sp->amp = matte->wave_height;
	sp->wavelen = matte->wave_length;
	sp->lobes = matte->lobes;
	sp->profile = matte->have_profile ? matte->profile : NULL;

	/* A clock's segment is paced as the arc at half the reach, so its bands
	 * come out near pixels around the middle of the canvas. A spiral gains
	 * one segment's worth for every turn's spacing, so Turns is how many
	 * times an arm winds round between the centre and the far corner. */
	if (shape == MATTE_SHAPE_CLOCK)
		sp->pitch = (float)M_PI * reach / matte->arms;
	else
		sp->pitch = reach / matte->turns;

	/* bars across the canvas as the shape frame sees it at the start */
	if (shape == MATTE_SHAPE_BLINDS) {
		float s = fabsf(sinf(f->angle0));
		float c = fabsf(cosf(f->angle0));
		float span = (cx * s + cy * c) / f->stretch_y;

		sp->bar_px = fmaxf(span / matte->bars, 2.0f);
	}
}

static void matte_frame_for(const struct matte_info *matte, int shape, float cx, float cy, struct matte_frame *f)
{
	f->origin_x = matte->origin_x * cx;
	f->origin_y = matte->origin_y * cy;
	f->angle0 = matte->angle;
	f->angle1 = matte->angle + matte->spin * 2.0f * (float)M_PI;

	/* Stretch is width over height, split evenly so the shape keeps its
	 * area. Sweeps run edge to edge, and stretching one would only change
	 * its speed, so they are left square. */
	if (matte_is_sweep(shape) || shape == MATTE_SHAPE_BLINDS) {
		f->stretch_x = f->stretch_y = 1.0f;
	} else {
		f->stretch_x = sqrtf(matte->stretch);
		f->stretch_y = 1.0f / f->stretch_x;
	}
}

static void matte_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct matte_info *matte = data;
	int shape;
	struct matte_shape_params sp;
	struct matte_frame frame;
	struct vec2 canvas, origin, rot, inv_stretch;
	float fcx = (float)cx, fcy = (float)cy;
	float luma_scale = MATTE_LUMA_SPAN * fcy;
	float u, front, angle;

	struct tc_frame blend;
	int i;

	/* a mask image the loader has finished since the last frame */
	matte_take_mask(matte);
	shape = matte_effective_shape(matte);

	matte_frame_for(matte, shape, fcx, fcy, &frame);
	matte_shape_params(matte, shape, &frame, fcx, fcy, &sp);

	/* Sampling the canvas edge is cheap but not free, so it is only redone
	 * when the canvas or a setting has moved. */
	if (!matte->range_valid || matte->range_cx != cx || matte->range_cy != cy) {
		if (shape == MATTE_SHAPE_LUMA) {
			matte->range_lo = matte->luma_lo * luma_scale;
			matte->range_hi = matte->luma_hi * luma_scale;
		} else {
			matte_field_range(&sp, &frame, fcx, fcy, &matte->range_lo, &matte->range_hi);
		}
		matte->range_cx = cx;
		matte->range_cy = cy;
		matte->range_valid = true;
	}

	u = matte_travel(&matte->timing, t);
	front = matte_front(matte->direction, matte->range_lo, matte->range_hi, &matte->bands, u);
	angle = frame.angle0 + (frame.angle1 - frame.angle0) * t;

	vec2_set(&canvas, fcx, fcy);
	vec2_set(&origin, frame.origin_x, frame.origin_y);
	vec2_set(&rot, cosf(angle), sinf(angle));
	vec2_set(&inv_stretch, 1.0f / frame.stretch_x, 1.0f / frame.stretch_y);

	tc_frame_begin(&blend);
	tc_set_texture(&blend, matte->ep_a_tex, a);
	tc_set_texture(&blend, matte->ep_b_tex, b);
	tc_set_color(&blend, matte->ep_color_border, &matte->border_color);
	tc_set_color(&blend, matte->ep_color_glow, &matte->glow_color);
	for (i = 0; i < MATTE_ECHOES; i++)
		tc_set_color(&blend, matte->ep_color_echo[i], &matte->echo_color[i]);

	/* the mask is an ordering, not a picture, so it is read as stored */
	gs_effect_set_texture(matte->ep_luma_tex, matte->have_image ? matte->image.texture : NULL);
	gs_effect_set_texture(matte->ep_profile_tex, matte->profile_tex);

	gs_effect_set_vec2(matte->ep_canvas, &canvas);
	gs_effect_set_vec2(matte->ep_origin_px, &origin);
	gs_effect_set_vec2(matte->ep_rot, &rot);
	gs_effect_set_vec2(matte->ep_inv_stretch, &inv_stretch);

	gs_effect_set_int(matte->ep_shape, shape);
	gs_effect_set_float(matte->ep_star_n, sp.star_n);
	gs_effect_set_float(matte->ep_star_k, sp.star_k);
	gs_effect_set_float(matte->ep_arms, sp.arms);
	gs_effect_set_float(matte->ep_pitch, sp.pitch);
	gs_effect_set_float(matte->ep_amp, sp.amp);
	gs_effect_set_float(matte->ep_wavelen, sp.wavelen);
	gs_effect_set_float(matte->ep_lobes, sp.lobes);
	gs_effect_set_float(matte->ep_bar_px, fmaxf(sp.bar_px, 1.0f));
	gs_effect_set_float(matte->ep_phase, matte->wave_drift * t);
	gs_effect_set_float(matte->ep_luma_scale, luma_scale);

	gs_effect_set_float(matte->ep_front, front);
	gs_effect_set_float(matte->ep_dir_sign, matte_sign(matte->direction));
	gs_effect_set_float(matte->ep_feather, matte->bands.feather);
	gs_effect_set_float(matte->ep_border, matte->bands.border);
	gs_effect_set_float(matte->ep_echo, matte->bands.echo);
	gs_effect_set_int(matte->ep_echo_count, matte->bands.echo_count);
	gs_effect_set_float(matte->ep_band_end, matte_band_width(&matte->bands));
	gs_effect_set_float(matte->ep_glow, matte->bands.glow);
	gs_effect_set_float(matte->ep_glow_amt, matte->glow_amt);

	while (gs_effect_loop(matte->base.effect, "Matte"))
		gs_draw_sprite(NULL, 0, cx, cy);

	tc_frame_end(&blend);
}

static void matte_video_render(void *data, gs_effect_t *effect)
{
	struct matte_info *matte = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(matte->base.source, matte_callback);
}

struct obs_source_info matte_transition = {
	.id = "voidscape_matte_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = matte_get_name,
	.create = matte_create,
	.destroy = matte_destroy,
	.update = matte_update,
	.video_render = matte_video_render,
	.audio_render = tc_audio_render,
	.get_properties = matte_properties,
	.get_defaults = matte_defaults,
	.video_get_color_space = tc_video_get_color_space,
};
