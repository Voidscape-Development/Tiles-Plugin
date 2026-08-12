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

#include <obs-module.h>
#include <plugin-support.h>

#include <math.h>

#include "tiles-transition.h"

/* clang-format off */

#define S_MODE          "mode"
#define S_SHAPE         "shape"
#define S_TILE_SIZE     "tile_size"
#define S_GRID_ROTATION "grid_rotation"
#define S_DIRECTION     "direction"
#define S_ANGLE         "angle"
#define S_ORIGIN_X      "origin_x"
#define S_ORIGIN_Y      "origin_y"
#define S_COLOR         "color"
#define S_COLOR_END     "color_end"
#define S_VARIATION     "variation"
#define S_STAGGER       "stagger"
#define S_RANDOMNESS    "randomness"
#define S_BAND          "band"
#define S_EASING        "easing"
#define S_INVERT        "invert"
#define S_REVERSE_EXIT  "reverse_exit"
#define S_B_TILES       "b_tiles"

#define T_(x)           obs_module_text(x)

/* clang-format on */

enum tiles_mode {
	TILES_MODE_COVER = 0,
	TILES_MODE_KEYHOLE = 1,
	TILES_MODE_WAVE = 2,
	TILES_MODE_CROP_SHRINK = 3,
	TILES_MODE_SCALE_SHRINK = 4,
};

enum tiles_shape {
	TILES_SHAPE_SQUARE = 0,
	TILES_SHAPE_CIRCLE = 1,
	TILES_SHAPE_HEXAGON = 2,
	TILES_SHAPE_TRIANGLE = 3,
};

enum tiles_direction {
	TILES_DIR_INSIDE_OUT = 0,
	TILES_DIR_OUTSIDE_IN = 1,
	TILES_DIR_MIRRORED_IN = 2,
	TILES_DIR_MIRRORED_OUT = 3,
	TILES_DIR_ANGLE = 4,
};

enum tiles_easing {
	TILES_EASE_LINEAR = 0,
	TILES_EASE_IN_OUT = 1,
	TILES_EASE_OUT = 2,
};

struct tiles_info {
	obs_source_t *source;
	gs_effect_t *effect;

	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_canvas;
	gs_eparam_t *ep_origin_px;
	gs_eparam_t *ep_grid_rot;
	gs_eparam_t *ep_dir_vec;
	gs_eparam_t *ep_dir_range;
	gs_eparam_t *ep_color_a;
	gs_eparam_t *ep_color_b;
	gs_eparam_t *ep_progress;
	gs_eparam_t *ep_tile_px;
	gs_eparam_t *ep_edge;
	gs_eparam_t *ep_stagger;
	gs_eparam_t *ep_band;
	gs_eparam_t *ep_randomness;
	gs_eparam_t *ep_variation;
	gs_eparam_t *ep_mode;
	gs_eparam_t *ep_shape;
	gs_eparam_t *ep_direction;
	gs_eparam_t *ep_invert;
	gs_eparam_t *ep_reverse_exit;
	gs_eparam_t *ep_b_tiles;

	int mode;
	int shape;
	int direction;
	int easing;

	float tile_px;
	float grid_rotation; /* radians */
	float angle;         /* radians */
	float origin_x;      /* 0..1 of canvas width */
	float origin_y;      /* 0..1 of canvas height */
	float band_px;
	float stagger;
	float randomness;
	float variation;

	struct vec4 color_a;
	struct vec4 color_a_srgb;
	struct vec4 color_b;
	struct vec4 color_b_srgb;

	bool invert;
	bool reverse_exit;
	bool b_tiles;
};

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static const char *tiles_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Tiles.Transition");
}

static void tiles_update(void *data, obs_data_t *settings)
{
	struct tiles_info *tiles = data;
	uint32_t color;

	tiles->mode = (int)obs_data_get_int(settings, S_MODE);
	tiles->shape = (int)obs_data_get_int(settings, S_SHAPE);
	tiles->direction = (int)obs_data_get_int(settings, S_DIRECTION);
	tiles->easing = (int)obs_data_get_int(settings, S_EASING);

	tiles->tile_px = (float)obs_data_get_int(settings, S_TILE_SIZE);
	tiles->grid_rotation = (float)obs_data_get_double(settings, S_GRID_ROTATION) * (float)M_PI / 180.0f;
	tiles->angle = (float)obs_data_get_double(settings, S_ANGLE) * (float)M_PI / 180.0f;
	tiles->origin_x = (float)obs_data_get_double(settings, S_ORIGIN_X) / 100.0f;
	tiles->origin_y = (float)obs_data_get_double(settings, S_ORIGIN_Y) / 100.0f;
	tiles->band_px = (float)obs_data_get_int(settings, S_BAND);
	tiles->stagger = (float)obs_data_get_double(settings, S_STAGGER) / 100.0f;
	tiles->randomness = (float)obs_data_get_double(settings, S_RANDOMNESS) / 100.0f;
	tiles->variation = (float)obs_data_get_double(settings, S_VARIATION) / 100.0f;

	tiles->invert = obs_data_get_bool(settings, S_INVERT);
	tiles->reverse_exit = obs_data_get_bool(settings, S_REVERSE_EXIT);
	tiles->b_tiles = obs_data_get_bool(settings, S_B_TILES);

	color = (uint32_t)obs_data_get_int(settings, S_COLOR) | 0xFF000000;
	vec4_from_rgba(&tiles->color_a, color);
	vec4_from_rgba_srgb(&tiles->color_a_srgb, color);

	color = (uint32_t)obs_data_get_int(settings, S_COLOR_END) | 0xFF000000;
	vec4_from_rgba(&tiles->color_b, color);
	vec4_from_rgba_srgb(&tiles->color_b_srgb, color);

	tiles->tile_px = clampf(tiles->tile_px, 4.0f, 4096.0f);
	tiles->stagger = clampf(tiles->stagger, 0.01f, 1.0f);
}

static void *tiles_create(obs_data_t *settings, obs_source_t *source)
{
	struct tiles_info *tiles;
	gs_effect_t *effect;
	char *file = obs_module_file("effects/tiles_transition.effect");

	if (!file) {
		obs_log(LOG_ERROR, "effects/tiles_transition.effect is missing");
		return NULL;
	}

	obs_enter_graphics();
	effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();

	bfree(file);

	if (!effect) {
		obs_log(LOG_ERROR, "failed to compile effects/tiles_transition.effect");
		return NULL;
	}

	tiles = bzalloc(sizeof(struct tiles_info));
	tiles->source = source;
	tiles->effect = effect;

	tiles->ep_a_tex = gs_effect_get_param_by_name(effect, "a_tex");
	tiles->ep_b_tex = gs_effect_get_param_by_name(effect, "b_tex");
	tiles->ep_canvas = gs_effect_get_param_by_name(effect, "canvas");
	tiles->ep_origin_px = gs_effect_get_param_by_name(effect, "origin_px");
	tiles->ep_grid_rot = gs_effect_get_param_by_name(effect, "grid_rot");
	tiles->ep_dir_vec = gs_effect_get_param_by_name(effect, "dir_vec");
	tiles->ep_dir_range = gs_effect_get_param_by_name(effect, "dir_range");
	tiles->ep_color_a = gs_effect_get_param_by_name(effect, "color_a");
	tiles->ep_color_b = gs_effect_get_param_by_name(effect, "color_b");
	tiles->ep_progress = gs_effect_get_param_by_name(effect, "progress");
	tiles->ep_tile_px = gs_effect_get_param_by_name(effect, "tile_px");
	tiles->ep_edge = gs_effect_get_param_by_name(effect, "edge");
	tiles->ep_stagger = gs_effect_get_param_by_name(effect, "stagger");
	tiles->ep_band = gs_effect_get_param_by_name(effect, "band");
	tiles->ep_randomness = gs_effect_get_param_by_name(effect, "randomness");
	tiles->ep_variation = gs_effect_get_param_by_name(effect, "variation");
	tiles->ep_mode = gs_effect_get_param_by_name(effect, "mode");
	tiles->ep_shape = gs_effect_get_param_by_name(effect, "shape");
	tiles->ep_direction = gs_effect_get_param_by_name(effect, "direction");
	tiles->ep_invert = gs_effect_get_param_by_name(effect, "invert");
	tiles->ep_reverse_exit = gs_effect_get_param_by_name(effect, "reverse_exit");
	tiles->ep_b_tiles = gs_effect_get_param_by_name(effect, "b_tiles");

	/* Apply the settings directly rather than through obs_source_update: the
	 * source's data pointer is only assigned once create returns, so an
	 * update queued here would not reach us until a later tick and the first
	 * frames would render with a zeroed tile size. */
	tiles_update(tiles, settings);

	return tiles;
}

static void tiles_destroy(void *data)
{
	struct tiles_info *tiles = data;

	obs_enter_graphics();
	gs_effect_destroy(tiles->effect);
	obs_leave_graphics();

	bfree(tiles);
}

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static bool tiles_layout_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int mode = (int)obs_data_get_int(settings, S_MODE);
	int direction = (int)obs_data_get_int(settings, S_DIRECTION);
	bool b_tiles = obs_data_get_bool(settings, S_B_TILES);
	bool dissolve = mode == TILES_MODE_CROP_SHRINK || mode == TILES_MODE_SCALE_SHRINK;
	bool coloured = mode == TILES_MODE_COVER || mode == TILES_MODE_WAVE || (dissolve && b_tiles);

	UNUSED_PARAMETER(prop);

	obs_property_set_visible(obs_properties_get(props, S_ANGLE),
				 direction != TILES_DIR_INSIDE_OUT && direction != TILES_DIR_OUTSIDE_IN);
	obs_property_set_visible(obs_properties_get(props, S_BAND), mode == TILES_MODE_WAVE);
	obs_property_set_visible(obs_properties_get(props, S_INVERT), mode == TILES_MODE_KEYHOLE);
	obs_property_set_visible(obs_properties_get(props, S_REVERSE_EXIT), mode == TILES_MODE_COVER);
	obs_property_set_visible(obs_properties_get(props, S_B_TILES), dissolve);
	obs_property_set_visible(obs_properties_get(props, S_COLOR), coloured);
	obs_property_set_visible(obs_properties_get(props, S_COLOR_END), coloured);
	obs_property_set_visible(obs_properties_get(props, S_VARIATION), coloured);

	return true;
}

static obs_properties_t *tiles_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_list(props, S_MODE, T_("Tiles.Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Tiles.Mode.Cover"), TILES_MODE_COVER);
	obs_property_list_add_int(p, T_("Tiles.Mode.Keyhole"), TILES_MODE_KEYHOLE);
	obs_property_list_add_int(p, T_("Tiles.Mode.Wave"), TILES_MODE_WAVE);
	obs_property_list_add_int(p, T_("Tiles.Mode.CropShrink"), TILES_MODE_CROP_SHRINK);
	obs_property_list_add_int(p, T_("Tiles.Mode.ScaleShrink"), TILES_MODE_SCALE_SHRINK);
	obs_property_set_modified_callback(p, tiles_layout_modified);

	p = obs_properties_add_list(props, S_SHAPE, T_("Tiles.Shape"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Tiles.Shape.Square"), TILES_SHAPE_SQUARE);
	obs_property_list_add_int(p, T_("Tiles.Shape.Circle"), TILES_SHAPE_CIRCLE);
	obs_property_list_add_int(p, T_("Tiles.Shape.Hexagon"), TILES_SHAPE_HEXAGON);
	obs_property_list_add_int(p, T_("Tiles.Shape.Triangle"), TILES_SHAPE_TRIANGLE);

	p = obs_properties_add_int_slider(props, S_TILE_SIZE, T_("Tiles.TileSize"), 8, 1024, 1);
	obs_property_int_set_suffix(p, " px");

	p = obs_properties_add_float_slider(props, S_GRID_ROTATION, T_("Tiles.GridRotation"), -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(p, "°");

	p = obs_properties_add_list(props, S_DIRECTION, T_("Tiles.Direction"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Tiles.Direction.InsideOut"), TILES_DIR_INSIDE_OUT);
	obs_property_list_add_int(p, T_("Tiles.Direction.OutsideIn"), TILES_DIR_OUTSIDE_IN);
	obs_property_list_add_int(p, T_("Tiles.Direction.MirroredIn"), TILES_DIR_MIRRORED_IN);
	obs_property_list_add_int(p, T_("Tiles.Direction.MirroredOut"), TILES_DIR_MIRRORED_OUT);
	obs_property_list_add_int(p, T_("Tiles.Direction.Angle"), TILES_DIR_ANGLE);
	obs_property_set_modified_callback(p, tiles_layout_modified);

	p = obs_properties_add_float_slider(props, S_ANGLE, T_("Tiles.Angle"), 0.0, 360.0, 1.0);
	obs_property_float_set_suffix(p, "°");
	obs_property_set_long_description(p, T_("Tiles.Angle.Description"));

	p = obs_properties_add_float_slider(props, S_ORIGIN_X, T_("Tiles.OriginX"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, S_ORIGIN_Y, T_("Tiles.OriginY"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_int_slider(props, S_BAND, T_("Tiles.Band"), 1, 4096, 1);
	obs_property_int_set_suffix(p, " px");

	obs_properties_add_color(props, S_COLOR, T_("Tiles.Color"));
	obs_properties_add_color(props, S_COLOR_END, T_("Tiles.ColorEnd"));

	p = obs_properties_add_float_slider(props, S_VARIATION, T_("Tiles.Variation"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_float_slider(props, S_STAGGER, T_("Tiles.Stagger"), 1.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Tiles.Stagger.Description"));

	p = obs_properties_add_float_slider(props, S_RANDOMNESS, T_("Tiles.Randomness"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_list(props, S_EASING, T_("Tiles.Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Tiles.Easing.Linear"), TILES_EASE_LINEAR);
	obs_property_list_add_int(p, T_("Tiles.Easing.InOut"), TILES_EASE_IN_OUT);
	obs_property_list_add_int(p, T_("Tiles.Easing.Out"), TILES_EASE_OUT);

	obs_properties_add_bool(props, S_INVERT, T_("Tiles.Invert"));
	obs_properties_add_bool(props, S_REVERSE_EXIT, T_("Tiles.ReverseExit"));

	p = obs_properties_add_bool(props, S_B_TILES, T_("Tiles.BTiles"));
	obs_property_set_long_description(p, T_("Tiles.BTiles.Description"));
	obs_property_set_modified_callback(p, tiles_layout_modified);

	return props;
}

static void tiles_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MODE, TILES_MODE_COVER);
	obs_data_set_default_int(settings, S_SHAPE, TILES_SHAPE_HEXAGON);
	obs_data_set_default_int(settings, S_TILE_SIZE, 96);
	obs_data_set_default_double(settings, S_GRID_ROTATION, 0.0);
	obs_data_set_default_int(settings, S_DIRECTION, TILES_DIR_INSIDE_OUT);
	obs_data_set_default_double(settings, S_ANGLE, 0.0);
	obs_data_set_default_double(settings, S_ORIGIN_X, 50.0);
	obs_data_set_default_double(settings, S_ORIGIN_Y, 50.0);
	obs_data_set_default_int(settings, S_COLOR, 0xFFFFFFFF);
	obs_data_set_default_int(settings, S_COLOR_END, 0xFFFFFFFF);
	obs_data_set_default_double(settings, S_VARIATION, 0.0);
	obs_data_set_default_double(settings, S_STAGGER, 35.0);
	obs_data_set_default_double(settings, S_RANDOMNESS, 0.0);
	obs_data_set_default_int(settings, S_BAND, 240);
	obs_data_set_default_int(settings, S_EASING, TILES_EASE_IN_OUT);
	obs_data_set_default_bool(settings, S_INVERT, false);
	obs_data_set_default_bool(settings, S_REVERSE_EXIT, false);
	obs_data_set_default_bool(settings, S_B_TILES, false);
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */

static inline float tiles_ease(int easing, float t)
{
	if (easing == TILES_EASE_IN_OUT)
		return t * t * (3.0f - 2.0f * t);
	if (easing == TILES_EASE_OUT)
		return 1.0f - (1.0f - t) * (1.0f - t);
	return t;
}

/* Distance from a tile centre to the furthest pixel it owns, in pixels. */
static inline float tiles_circumradius(const struct tiles_info *tiles)
{
	if (tiles->shape == TILES_SHAPE_SQUARE)
		return tiles->tile_px * 0.7071068f;
	return tiles->tile_px * 0.5773503f;
}

/*
 * The sweep extent is measured to the far canvas corner as seen from the
 * origin, so every tile still starts and finishes inside the transition when
 * the origin is offset or pushed outside the frame. It is padded by one tile
 * circumradius because a tile straddling the canvas edge has its centre
 * outside the frame, and it still has to take its turn in the sweep.
 */
static void tiles_sweep_extent(const struct tiles_info *tiles, float ox, float oy, float cx, float cy, struct vec2 *dir,
			       struct vec2 *range)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float pad = tiles_circumradius(tiles);
	float max_radius = 0.0f;
	float proj_min = 0.0f;
	float proj_max = 0.0f;
	size_t i;

	vec2_set(dir, cosf(tiles->angle), sinf(tiles->angle));

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - ox;
		float dy = corner_y[i] - oy;
		float radius = sqrtf(dx * dx + dy * dy);
		float proj = dx * dir->x + dy * dir->y;

		if (radius > max_radius)
			max_radius = radius;
		if (i == 0 || proj < proj_min)
			proj_min = proj;
		if (i == 0 || proj > proj_max)
			proj_max = proj;
	}

	switch (tiles->direction) {
	case TILES_DIR_ANGLE:
		vec2_set(range, proj_min - pad, fmaxf(proj_max - proj_min + 2.0f * pad, 1.0f));
		break;
	case TILES_DIR_MIRRORED_IN:
	case TILES_DIR_MIRRORED_OUT:
		vec2_set(range, 0.0f, fmaxf(fmaxf(fabsf(proj_min), fabsf(proj_max)) + pad, 1.0f));
		break;
	default:
		vec2_set(range, 0.0f, fmaxf(max_radius + pad, 1.0f));
		break;
	}
}

/* Metric units per pixel, so the coverage ramp stays about one pixel wide. */
static inline float tiles_edge_width(const struct tiles_info *tiles)
{
	switch (tiles->shape) {
	case TILES_SHAPE_CIRCLE:
		return 1.7320508f / tiles->tile_px;
	case TILES_SHAPE_TRIANGLE:
		return 3.4641016f / tiles->tile_px;
	default:
		return 2.0f / tiles->tile_px;
	}
}

static void tiles_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct tiles_info *tiles = data;
	struct vec2 canvas;
	struct vec2 origin;
	struct vec2 grid_rot;
	struct vec2 dir;
	struct vec2 range;
	float band;
	float stagger = tiles->stagger;
	bool nonlinear = gs_get_color_space() == GS_CS_SRGB;
	bool previous_srgb = gs_framebuffer_srgb_enabled();

	vec2_set(&canvas, (float)cx, (float)cy);
	vec2_set(&origin, tiles->origin_x * (float)cx, tiles->origin_y * (float)cy);
	vec2_set(&grid_rot, cosf(tiles->grid_rotation), sinf(tiles->grid_rotation));

	tiles_sweep_extent(tiles, origin.x, origin.y, (float)cx, (float)cy, &dir, &range);

	band = tiles->band_px / range.y;

	/* The wave has to grow, hold and shrink inside the same sweep. */
	if (tiles->mode == TILES_MODE_WAVE) {
		float total = 2.0f * stagger + band;

		if (total > 0.95f) {
			stagger *= 0.95f / total;
			band *= 0.95f / total;
		}
	}

	gs_enable_framebuffer_srgb(!nonlinear);

	if (nonlinear) {
		gs_effect_set_texture(tiles->ep_a_tex, a);
		gs_effect_set_texture(tiles->ep_b_tex, b);
		gs_effect_set_vec4(tiles->ep_color_a, &tiles->color_a);
		gs_effect_set_vec4(tiles->ep_color_b, &tiles->color_b);
	} else {
		gs_effect_set_texture_srgb(tiles->ep_a_tex, a);
		gs_effect_set_texture_srgb(tiles->ep_b_tex, b);
		gs_effect_set_vec4(tiles->ep_color_a, &tiles->color_a_srgb);
		gs_effect_set_vec4(tiles->ep_color_b, &tiles->color_b_srgb);
	}

	gs_effect_set_vec2(tiles->ep_canvas, &canvas);
	gs_effect_set_vec2(tiles->ep_origin_px, &origin);
	gs_effect_set_vec2(tiles->ep_grid_rot, &grid_rot);
	gs_effect_set_vec2(tiles->ep_dir_vec, &dir);
	gs_effect_set_vec2(tiles->ep_dir_range, &range);

	gs_effect_set_float(tiles->ep_progress, tiles_ease(tiles->easing, t));
	gs_effect_set_float(tiles->ep_tile_px, tiles->tile_px);
	gs_effect_set_float(tiles->ep_edge, tiles_edge_width(tiles));
	gs_effect_set_float(tiles->ep_stagger, stagger);
	gs_effect_set_float(tiles->ep_band, band);
	gs_effect_set_float(tiles->ep_randomness, tiles->randomness);
	gs_effect_set_float(tiles->ep_variation, tiles->variation);

	gs_effect_set_int(tiles->ep_mode, tiles->mode);
	gs_effect_set_int(tiles->ep_shape, tiles->shape);
	gs_effect_set_int(tiles->ep_direction, tiles->direction);

	gs_effect_set_bool(tiles->ep_invert, tiles->invert);
	gs_effect_set_bool(tiles->ep_reverse_exit, tiles->reverse_exit);
	gs_effect_set_bool(tiles->ep_b_tiles, tiles->b_tiles);

	while (gs_effect_loop(tiles->effect, "Tiles"))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous_srgb);
}

static void tiles_video_render(void *data, gs_effect_t *effect)
{
	struct tiles_info *tiles = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(tiles->source, tiles_callback);
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

static bool tiles_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
			       size_t channels, size_t sample_rate)
{
	struct tiles_info *tiles = data;

	return obs_transition_audio_render(tiles->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static enum gs_color_space tiles_video_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	struct tiles_info *tiles = data;

	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return obs_transition_video_get_color_space(tiles->source);
}

struct obs_source_info tiles_transition = {
	.id = "voidscape_tiles_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = tiles_get_name,
	.create = tiles_create,
	.destroy = tiles_destroy,
	.update = tiles_update,
	.video_render = tiles_video_render,
	.audio_render = tiles_audio_render,
	.get_properties = tiles_properties,
	.get_defaults = tiles_defaults,
	.video_get_color_space = tiles_video_get_color_space,
};
