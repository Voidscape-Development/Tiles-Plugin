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

#include <math.h>
#include <string.h>

#include "matte-math.h"
#include "matte-transition.h"

/* clang-format off */

#define S_PRESET       "preset"
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

static const char *const echo_param[MATTE_ECHOES] = {
	"color_echo1",
	"color_echo2",
	"color_echo3",
	"color_echo4",
};

enum matte_preset_id {
	MATTE_PRESET_CUSTOM = 0,
};

/*
 * A whole look in one row, the way the glass transition does it: picking a
 * preset writes every one of these, and touching any of them afterwards puts
 * the list back to Custom. The centre and the image are left alone - they are
 * where and what, not how.
 */
struct matte_preset {
	const char *label;
	int shape;
	int direction;
	double star_points;
	double star_depth;
	double arms;
	double turns;
	double bars;
	double wave_height;
	double wave_length;
	double wave_drift;
	double lobes;
	double angle;
	double spin;
	double stretch;
	int easing;
	double overshoot;
	double pause_size;
	double pause_len;
	double feather;
	double border;
	uint32_t border_color;
	int echo_count;
	double echo_width;
	uint32_t echo_color[MATTE_ECHOES];
	double glow;
	double glow_width;
	uint32_t glow_color;
};

#define WHITE RGB_(0xFF, 0xFF, 0xFF)
#define BLACK RGB_(0x00, 0x00, 0x00)
#define CYAN RGB_(0x00, 0xB4, 0xFF)
#define PURPLE RGB_(0x7A, 0x3F, 0xD0)
#define PINK RGB_(0xFF, 0x3D, 0x7F)
#define AMBER RGB_(0xFF, 0xB1, 0x3D)

/* clang-format off */

static const struct matte_preset matte_presets[] = {
	{
		.label = "Matte.Preset.IrisOut", .shape = MATTE_SHAPE_CIRCLE, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_IN_OUT, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 1,
		.border = 0, .border_color = WHITE,
		.echo_count = 0, .echo_width = 60, .echo_color = {CYAN, PURPLE, PINK, AMBER},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
	{
		.label = "Matte.Preset.Cartoon", .shape = MATTE_SHAPE_CIRCLE, .direction = MATTE_DIR_IN,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_OVERSHOOT, .overshoot = 60,
		.pause_size = 22, .pause_len = 30, .feather = 1,
		.border = 8, .border_color = BLACK,
		.echo_count = 0, .echo_width = 60, .echo_color = {CYAN, PURPLE, PINK, AMBER},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
	{
		.label = "Matte.Preset.Star", .shape = MATTE_SHAPE_STAR, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0.35, .stretch = 100, .easing = MATTE_EASE_OUT, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 2,
		.border = 14, .border_color = WHITE,
		.echo_count = 2, .echo_width = 70, .echo_color = {CYAN, PURPLE, PINK, AMBER},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
	{
		.label = "Matte.Preset.Hexagon", .shape = MATTE_SHAPE_HEXAGON, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 1.0, .stretch = 100, .easing = MATTE_EASE_IN_OUT, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 2,
		.border = 0, .border_color = WHITE,
		.echo_count = 3, .echo_width = 50, .echo_color = {PINK, AMBER, CYAN, PURPLE},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
	{
		.label = "Matte.Preset.Heart", .shape = MATTE_SHAPE_HEART, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_OVERSHOOT, .overshoot = 80,
		.pause_size = 30, .pause_len = 25, .feather = 2,
		.border = 10, .border_color = WHITE,
		.echo_count = 1, .echo_width = 30, .echo_color = {RGB_(0xFF, 0x4D, 0x88), PURPLE, PINK, AMBER},
		.glow = 60, .glow_width = 50, .glow_color = RGB_(0xFF, 0x9E, 0xC4),
	},
	{
		.label = "Matte.Preset.Wave", .shape = MATTE_SHAPE_WAVE, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.5, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_IN_OUT, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 2,
		.border = 18, .border_color = RGB_(0xE8, 0xFB, 0xFF),
		.echo_count = 2, .echo_width = 45, .echo_color = {RGB_(0x29, 0xB6, 0xF6), RGB_(0x0D, 0x47, 0xA1), PINK, AMBER},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
	{
		.label = "Matte.Preset.Clock", .shape = MATTE_SHAPE_CLOCK, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_LINEAR, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 1,
		.border = 8, .border_color = WHITE,
		.echo_count = 0, .echo_width = 60, .echo_color = {CYAN, PURPLE, PINK, AMBER},
		.glow = 80, .glow_width = 30, .glow_color = CYAN,
	},
	{
		.label = "Matte.Preset.Blinds", .shape = MATTE_SHAPE_BLINDS, .direction = MATTE_DIR_OUT,
		.star_points = 5, .star_depth = 45, .arms = 1, .turns = 3, .bars = 8,
		.wave_height = 60, .wave_length = 420, .wave_drift = 1.0, .lobes = 8,
		.angle = 0, .spin = 0, .stretch = 100, .easing = MATTE_EASE_IN_OUT, .overshoot = 50,
		.pause_size = 30, .pause_len = 0, .feather = 1,
		.border = 6, .border_color = WHITE,
		.echo_count = 0, .echo_width = 60, .echo_color = {CYAN, PURPLE, PINK, AMBER},
		.glow = 0, .glow_width = 40, .glow_color = WHITE,
	},
};

/* clang-format on */

#define MATTE_PRESET_COUNT ((int)(sizeof(matte_presets) / sizeof(matte_presets[0])))

struct matte_info {
	obs_source_t *source;
	gs_effect_t *effect;

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

	struct vec4 border_color, border_color_srgb;
	struct vec4 echo_color[MATTE_ECHOES], echo_color_srgb[MATTE_ECHOES];
	struct vec4 glow_color, glow_color_srgb;

	/* mask image - owned by the graphics thread once loaded */
	struct dstr image_path;
	gs_image_file_t image;
	bool have_image;
	bool have_profile;
	gs_texture_t *profile_tex;
	float profile[MATTE_PROFILE_SIZE];
	float luma_lo, luma_hi;

	/* the field's range, cached against the canvas and the settings */
	bool range_valid;
	uint32_t range_cx, range_cy;
	float range_lo, range_hi;
};

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

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
static uint8_t *image_rgba(const gs_image_file_t *img)
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

static void matte_load_image(struct matte_info *matte, const char *path)
{
	gs_image_file_t img;
	float profile[MATTE_PROFILE_SIZE];
	bool have_profile = false;
	float lo = 0.0f, hi = 1.0f;
	gs_texture_t *profile_tex = NULL;
	uint8_t *rgba;

	memset(&img, 0, sizeof(img));

	if (path && *path) {
		gs_image_file_init(&img, path);

		if (!img.loaded)
			obs_log(LOG_WARNING, "matte: could not load mask image '%s'", path);
	}

	rgba = img.loaded ? image_rgba(&img) : NULL;
	if (rgba) {
		have_profile = matte_build_profile(rgba, img.cx, img.cy, profile);
		luma_range(rgba, (size_t)img.cx * (size_t)img.cy, &lo, &hi);
		bfree(rgba);

		if (!have_profile)
			obs_log(LOG_WARNING, "matte: mask image '%s' has nothing in it to cut a shape from", path);
	}

	/* Everything the render callback reads is swapped under the graphics
	 * lock, so a frame never sees half of an old image and half of a new
	 * one, and the old textures are never freed out from under a draw. */
	obs_enter_graphics();

	if (img.loaded)
		gs_image_file_init_texture(&img);

	if (have_profile) {
		const uint8_t *data = (const uint8_t *)profile;

		profile_tex = gs_texture_create(MATTE_PROFILE_SIZE, 1, GS_R32F, 1, &data, 0);
	}

	gs_image_file_free(&matte->image);
	gs_texture_destroy(matte->profile_tex);

	matte->image = img;
	matte->have_image = img.loaded && img.texture != NULL;
	matte->profile_tex = profile_tex;
	matte->have_profile = have_profile && profile_tex != NULL;
	if (have_profile)
		memcpy(matte->profile, profile, sizeof(profile));
	matte->luma_lo = lo;
	matte->luma_hi = fmaxf(hi, lo + 0.001f);
	matte->range_valid = false;

	obs_leave_graphics();
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */

static void color_pair(obs_data_t *settings, const char *name, struct vec4 *plain, struct vec4 *srgb)
{
	uint32_t color = (uint32_t)obs_data_get_int(settings, name) | 0xFF000000;

	vec4_from_rgba(plain, color);
	vec4_from_rgba_srgb(srgb, color);
}

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

	color_pair(settings, S_BORDER_COLOR, &matte->border_color, &matte->border_color_srgb);
	color_pair(settings, S_GLOW_COLOR, &matte->glow_color, &matte->glow_color_srgb);
	for (i = 0; i < MATTE_ECHOES; i++)
		color_pair(settings, echo_setting[i], &matte->echo_color[i], &matte->echo_color_srgb[i]);

	matte->range_valid = false;

	/* Only reload when the path itself changed: every slider move comes
	 * through here, and decoding the image again for each would stall it. */
	if (!path)
		path = "";

	/* an emptied dstr holds NULL rather than "", and that is still no image */
	if (strcmp(matte->image_path.array ? matte->image_path.array : "", path) != 0) {
		dstr_copy(&matte->image_path, path);
		matte_load_image(matte, path);
	}
}

static void *matte_create(obs_data_t *settings, obs_source_t *source)
{
	struct matte_info *matte;
	gs_effect_t *effect;
	char *file = obs_module_file("effects/matte_transition.effect");
	int i;

	if (!file) {
		obs_log(LOG_ERROR, "effects/matte_transition.effect is missing");
		return NULL;
	}

	obs_enter_graphics();
	effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();

	bfree(file);

	if (!effect) {
		obs_log(LOG_ERROR, "failed to compile effects/matte_transition.effect");
		return NULL;
	}

	matte = bzalloc(sizeof(struct matte_info));
	matte->source = source;
	matte->effect = effect;

	matte->ep_a_tex = gs_effect_get_param_by_name(effect, "a_tex");
	matte->ep_b_tex = gs_effect_get_param_by_name(effect, "b_tex");
	matte->ep_luma_tex = gs_effect_get_param_by_name(effect, "luma_tex");
	matte->ep_profile_tex = gs_effect_get_param_by_name(effect, "profile_tex");
	matte->ep_canvas = gs_effect_get_param_by_name(effect, "canvas");
	matte->ep_origin_px = gs_effect_get_param_by_name(effect, "origin_px");
	matte->ep_rot = gs_effect_get_param_by_name(effect, "rot");
	matte->ep_inv_stretch = gs_effect_get_param_by_name(effect, "inv_stretch");
	matte->ep_shape = gs_effect_get_param_by_name(effect, "shape");
	matte->ep_star_n = gs_effect_get_param_by_name(effect, "star_n");
	matte->ep_star_k = gs_effect_get_param_by_name(effect, "star_k");
	matte->ep_arms = gs_effect_get_param_by_name(effect, "arms");
	matte->ep_pitch = gs_effect_get_param_by_name(effect, "pitch");
	matte->ep_amp = gs_effect_get_param_by_name(effect, "amp");
	matte->ep_wavelen = gs_effect_get_param_by_name(effect, "wavelen");
	matte->ep_lobes = gs_effect_get_param_by_name(effect, "lobes");
	matte->ep_bar_px = gs_effect_get_param_by_name(effect, "bar_px");
	matte->ep_phase = gs_effect_get_param_by_name(effect, "phase");
	matte->ep_luma_scale = gs_effect_get_param_by_name(effect, "luma_scale");
	matte->ep_front = gs_effect_get_param_by_name(effect, "front");
	matte->ep_dir_sign = gs_effect_get_param_by_name(effect, "dir_sign");
	matte->ep_feather = gs_effect_get_param_by_name(effect, "feather");
	matte->ep_border = gs_effect_get_param_by_name(effect, "border");
	matte->ep_echo = gs_effect_get_param_by_name(effect, "echo");
	matte->ep_echo_count = gs_effect_get_param_by_name(effect, "echo_count");
	matte->ep_band_end = gs_effect_get_param_by_name(effect, "band_end");
	matte->ep_glow = gs_effect_get_param_by_name(effect, "glow");
	matte->ep_glow_amt = gs_effect_get_param_by_name(effect, "glow_amt");
	matte->ep_color_border = gs_effect_get_param_by_name(effect, "color_border");
	matte->ep_color_glow = gs_effect_get_param_by_name(effect, "color_glow");

	for (i = 0; i < MATTE_ECHOES; i++)
		matte->ep_color_echo[i] = gs_effect_get_param_by_name(effect, echo_param[i]);

	dstr_init(&matte->image_path);

	/* Applied directly, as the other transitions do: the source's data
	 * pointer is only assigned once create returns. */
	matte_update(matte, settings);

	return matte;
}

static void matte_destroy(void *data)
{
	struct matte_info *matte = data;

	obs_enter_graphics();
	gs_effect_destroy(matte->effect);
	gs_image_file_free(&matte->image);
	gs_texture_destroy(matte->profile_tex);
	obs_leave_graphics();

	dstr_free(&matte->image_path);
	bfree(matte);
}

/* ------------------------------------------------------------------ */
/* presets                                                            */
/* ------------------------------------------------------------------ */

static void matte_defaults(obs_data_t *settings)
{
	const struct matte_preset *p = &matte_presets[0];
	int i;

	obs_data_set_default_int(settings, S_PRESET, 1);
	obs_data_set_default_int(settings, S_SHAPE, p->shape);
	obs_data_set_default_int(settings, S_DIRECTION, p->direction);
	obs_data_set_default_int(settings, S_STAR_POINTS, (long long)p->star_points);
	obs_data_set_default_double(settings, S_STAR_DEPTH, p->star_depth);
	obs_data_set_default_int(settings, S_ARMS, (long long)p->arms);
	obs_data_set_default_double(settings, S_TURNS, p->turns);
	obs_data_set_default_int(settings, S_BARS, (long long)p->bars);
	obs_data_set_default_double(settings, S_WAVE_HEIGHT, p->wave_height);
	obs_data_set_default_double(settings, S_WAVE_LENGTH, p->wave_length);
	obs_data_set_default_double(settings, S_WAVE_DRIFT, p->wave_drift);
	obs_data_set_default_int(settings, S_LOBES, (long long)p->lobes);
	obs_data_set_default_string(settings, S_IMAGE, "");
	obs_data_set_default_double(settings, S_ANGLE, p->angle);
	obs_data_set_default_double(settings, S_SPIN, p->spin);
	obs_data_set_default_double(settings, S_STRETCH, p->stretch);
	obs_data_set_default_double(settings, S_ORIGIN_X, 50.0);
	obs_data_set_default_double(settings, S_ORIGIN_Y, 50.0);
	obs_data_set_default_int(settings, S_EASING, p->easing);
	obs_data_set_default_double(settings, S_OVERSHOOT, p->overshoot);
	obs_data_set_default_double(settings, S_PAUSE_SIZE, p->pause_size);
	obs_data_set_default_double(settings, S_PAUSE_LEN, p->pause_len);
	obs_data_set_default_double(settings, S_FEATHER, p->feather);
	obs_data_set_default_double(settings, S_BORDER, p->border);
	obs_data_set_default_int(settings, S_BORDER_COLOR, p->border_color);
	obs_data_set_default_int(settings, S_ECHO_COUNT, p->echo_count);
	obs_data_set_default_double(settings, S_ECHO_WIDTH, p->echo_width);
	for (i = 0; i < MATTE_ECHOES; i++)
		obs_data_set_default_int(settings, echo_setting[i], p->echo_color[i]);
	obs_data_set_default_double(settings, S_GLOW, p->glow);
	obs_data_set_default_double(settings, S_GLOW_WIDTH, p->glow_width);
	obs_data_set_default_int(settings, S_GLOW_COLOR, p->glow_color);
}

static bool near_enough(double a, double b)
{
	return fabs(a - b) < 0.001;
}

static bool same_color(obs_data_t *s, const char *name, uint32_t c)
{
	return ((uint32_t)obs_data_get_int(s, name) & 0xFFFFFF) == (c & 0xFFFFFF);
}

static bool preset_matches(const struct matte_preset *p, obs_data_t *s)
{
	int i;

	if ((int)obs_data_get_int(s, S_SHAPE) != p->shape || (int)obs_data_get_int(s, S_DIRECTION) != p->direction ||
	    (int)obs_data_get_int(s, S_EASING) != p->easing || (int)obs_data_get_int(s, S_ECHO_COUNT) != p->echo_count)
		return false;

	if (!near_enough((double)obs_data_get_int(s, S_STAR_POINTS), p->star_points) ||
	    !near_enough(obs_data_get_double(s, S_STAR_DEPTH), p->star_depth) ||
	    !near_enough((double)obs_data_get_int(s, S_ARMS), p->arms) ||
	    !near_enough(obs_data_get_double(s, S_TURNS), p->turns) ||
	    !near_enough((double)obs_data_get_int(s, S_BARS), p->bars) ||
	    !near_enough(obs_data_get_double(s, S_WAVE_HEIGHT), p->wave_height) ||
	    !near_enough(obs_data_get_double(s, S_WAVE_LENGTH), p->wave_length) ||
	    !near_enough(obs_data_get_double(s, S_WAVE_DRIFT), p->wave_drift) ||
	    !near_enough((double)obs_data_get_int(s, S_LOBES), p->lobes) ||
	    !near_enough(obs_data_get_double(s, S_ANGLE), p->angle) ||
	    !near_enough(obs_data_get_double(s, S_SPIN), p->spin) ||
	    !near_enough(obs_data_get_double(s, S_STRETCH), p->stretch) ||
	    !near_enough(obs_data_get_double(s, S_OVERSHOOT), p->overshoot) ||
	    !near_enough(obs_data_get_double(s, S_PAUSE_SIZE), p->pause_size) ||
	    !near_enough(obs_data_get_double(s, S_PAUSE_LEN), p->pause_len) ||
	    !near_enough(obs_data_get_double(s, S_FEATHER), p->feather) ||
	    !near_enough(obs_data_get_double(s, S_BORDER), p->border) ||
	    !near_enough(obs_data_get_double(s, S_ECHO_WIDTH), p->echo_width) ||
	    !near_enough(obs_data_get_double(s, S_GLOW), p->glow) ||
	    !near_enough(obs_data_get_double(s, S_GLOW_WIDTH), p->glow_width))
		return false;

	if (!same_color(s, S_BORDER_COLOR, p->border_color) || !same_color(s, S_GLOW_COLOR, p->glow_color))
		return false;

	for (i = 0; i < MATTE_ECHOES; i++)
		if (!same_color(s, echo_setting[i], p->echo_color[i]))
			return false;

	return true;
}

static void preset_apply(const struct matte_preset *p, obs_data_t *s)
{
	int i;

	obs_data_set_int(s, S_SHAPE, p->shape);
	obs_data_set_int(s, S_DIRECTION, p->direction);
	obs_data_set_int(s, S_STAR_POINTS, (long long)p->star_points);
	obs_data_set_double(s, S_STAR_DEPTH, p->star_depth);
	obs_data_set_int(s, S_ARMS, (long long)p->arms);
	obs_data_set_double(s, S_TURNS, p->turns);
	obs_data_set_int(s, S_BARS, (long long)p->bars);
	obs_data_set_double(s, S_WAVE_HEIGHT, p->wave_height);
	obs_data_set_double(s, S_WAVE_LENGTH, p->wave_length);
	obs_data_set_double(s, S_WAVE_DRIFT, p->wave_drift);
	obs_data_set_int(s, S_LOBES, (long long)p->lobes);
	obs_data_set_double(s, S_ANGLE, p->angle);
	obs_data_set_double(s, S_SPIN, p->spin);
	obs_data_set_double(s, S_STRETCH, p->stretch);
	obs_data_set_int(s, S_EASING, p->easing);
	obs_data_set_double(s, S_OVERSHOOT, p->overshoot);
	obs_data_set_double(s, S_PAUSE_SIZE, p->pause_size);
	obs_data_set_double(s, S_PAUSE_LEN, p->pause_len);
	obs_data_set_double(s, S_FEATHER, p->feather);
	obs_data_set_double(s, S_BORDER, p->border);
	obs_data_set_int(s, S_BORDER_COLOR, p->border_color);
	obs_data_set_int(s, S_ECHO_COUNT, p->echo_count);
	obs_data_set_double(s, S_ECHO_WIDTH, p->echo_width);
	for (i = 0; i < MATTE_ECHOES; i++)
		obs_data_set_int(s, echo_setting[i], p->echo_color[i]);
	obs_data_set_double(s, S_GLOW, p->glow);
	obs_data_set_double(s, S_GLOW_WIDTH, p->glow_width);
	obs_data_set_int(s, S_GLOW_COLOR, p->glow_color);
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
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= 1 && id <= MATTE_PRESET_COUNT)
		preset_apply(&matte_presets[id - 1], settings);

	matte_layout(props, settings);
	return true;
}

/* Every control but the preset list and the where-and-what settings runs
 * through here, and drops the preset back to Custom once the settings have
 * left it. */
static bool matte_touched(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= 1 && id <= MATTE_PRESET_COUNT && !preset_matches(&matte_presets[id - 1], settings))
		obs_data_set_int(settings, S_PRESET, MATTE_PRESET_CUSTOM);

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
	obs_property_list_add_int(p, T_("Matte.Preset.Custom"), MATTE_PRESET_CUSTOM);
	for (i = 0; i < MATTE_PRESET_COUNT; i++)
		obs_property_list_add_int(p, T_(matte_presets[i].label), i + 1);
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
	int shape = matte_effective_shape(matte);
	struct matte_shape_params sp;
	struct matte_frame frame;
	struct vec2 canvas, origin, rot, inv_stretch;
	float fcx = (float)cx, fcy = (float)cy;
	float luma_scale = MATTE_LUMA_SPAN * fcy;
	float u, front, angle;

	bool nonlinear = gs_get_color_space() == GS_CS_SRGB;
	bool previous_srgb = gs_framebuffer_srgb_enabled();
	int i;

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

	gs_enable_framebuffer_srgb(!nonlinear);

	if (nonlinear) {
		gs_effect_set_texture(matte->ep_a_tex, a);
		gs_effect_set_texture(matte->ep_b_tex, b);
		gs_effect_set_vec4(matte->ep_color_border, &matte->border_color);
		gs_effect_set_vec4(matte->ep_color_glow, &matte->glow_color);
		for (i = 0; i < MATTE_ECHOES; i++)
			gs_effect_set_vec4(matte->ep_color_echo[i], &matte->echo_color[i]);
	} else {
		gs_effect_set_texture_srgb(matte->ep_a_tex, a);
		gs_effect_set_texture_srgb(matte->ep_b_tex, b);
		gs_effect_set_vec4(matte->ep_color_border, &matte->border_color_srgb);
		gs_effect_set_vec4(matte->ep_color_glow, &matte->glow_color_srgb);
		for (i = 0; i < MATTE_ECHOES; i++)
			gs_effect_set_vec4(matte->ep_color_echo[i], &matte->echo_color_srgb[i]);
	}

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

	while (gs_effect_loop(matte->effect, "Matte"))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous_srgb);
}

static void matte_video_render(void *data, gs_effect_t *effect)
{
	struct matte_info *matte = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(matte->source, matte_callback);
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool matte_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
			       size_t channels, size_t sample_rate)
{
	struct matte_info *matte = data;

	return obs_transition_audio_render(matte->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static enum gs_color_space matte_video_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	struct matte_info *matte = data;

	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return obs_transition_video_get_color_space(matte->source);
}

struct obs_source_info matte_transition = {
	.id = "voidscape_matte_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = matte_get_name,
	.create = matte_create,
	.destroy = matte_destroy,
	.update = matte_update,
	.video_render = matte_video_render,
	.audio_render = matte_audio_render,
	.get_properties = matte_properties,
	.get_defaults = matte_defaults,
	.video_get_color_space = matte_video_get_color_space,
};
