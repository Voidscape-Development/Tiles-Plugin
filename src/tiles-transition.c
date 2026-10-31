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

#include "tiles-math.h"
#include "tiles-transition.h"
#include "transition-common.h"

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
#define S_TILE_DURATION "tile_duration"
#define S_STAGGER_OLD   "stagger" /* pre-rename name of S_TILE_DURATION */
#define S_RANDOMNESS    "randomness"
#define S_BAND          "band"
#define S_EASING        "easing"
#define S_INVERT        "invert"
#define S_REVERSE_EXIT  "reverse_exit"
#define S_B_TILES       "b_tiles"

#define T_(x)           obs_module_text(x)

/* clang-format on */

struct tiles_info {
	struct transition_base base;

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
	gs_eparam_t *ep_tile_dur;
	gs_eparam_t *ep_band;
	gs_eparam_t *ep_randomness;
	gs_eparam_t *ep_variation;
	gs_eparam_t *ep_easing;
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
	float tile_duration;
	float randomness;
	float variation;

	struct tc_color color_a;
	struct tc_color color_b;

	bool invert;
	bool reverse_exit;
	bool b_tiles;
};

static const char *tiles_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Tiles.Transition");
}

static void tiles_update(void *data, obs_data_t *settings)
{
	struct tiles_info *tiles = data;

	/* S_TILE_DURATION used to be called "stagger". Carry a saved value over
	 * so scenes tuned before the rename keep their timing. */
	if (obs_data_has_user_value(settings, S_STAGGER_OLD)) {
		if (!obs_data_has_user_value(settings, S_TILE_DURATION))
			obs_data_set_double(settings, S_TILE_DURATION, obs_data_get_double(settings, S_STAGGER_OLD));
		obs_data_unset_user_value(settings, S_STAGGER_OLD);
	}

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
	tiles->tile_duration = (float)obs_data_get_double(settings, S_TILE_DURATION) / 100.0f;
	tiles->randomness = (float)obs_data_get_double(settings, S_RANDOMNESS) / 100.0f;
	tiles->variation = (float)obs_data_get_double(settings, S_VARIATION) / 100.0f;

	tiles->invert = obs_data_get_bool(settings, S_INVERT);
	tiles->reverse_exit = obs_data_get_bool(settings, S_REVERSE_EXIT);
	tiles->b_tiles = obs_data_get_bool(settings, S_B_TILES);

	tc_color_from_setting(&tiles->color_a, settings, S_COLOR);
	tc_color_from_setting(&tiles->color_b, settings, S_COLOR_END);

	tiles->tile_px = clampf(tiles->tile_px, 4.0f, 4096.0f);
	tiles->tile_duration = clampf(tiles->tile_duration, 0.01f, 1.0f);
}

/* clang-format off */

static const struct tc_param tiles_params[] = {
	TC_PARAM(struct tiles_info, ep_a_tex, "a_tex"),
	TC_PARAM(struct tiles_info, ep_b_tex, "b_tex"),
	TC_PARAM(struct tiles_info, ep_canvas, "canvas"),
	TC_PARAM(struct tiles_info, ep_origin_px, "origin_px"),
	TC_PARAM(struct tiles_info, ep_grid_rot, "grid_rot"),
	TC_PARAM(struct tiles_info, ep_dir_vec, "dir_vec"),
	TC_PARAM(struct tiles_info, ep_dir_range, "dir_range"),
	TC_PARAM(struct tiles_info, ep_color_a, "color_a"),
	TC_PARAM(struct tiles_info, ep_color_b, "color_b"),
	TC_PARAM(struct tiles_info, ep_progress, "progress"),
	TC_PARAM(struct tiles_info, ep_tile_px, "tile_px"),
	TC_PARAM(struct tiles_info, ep_edge, "edge"),
	TC_PARAM(struct tiles_info, ep_tile_dur, "tile_dur"),
	TC_PARAM(struct tiles_info, ep_band, "band"),
	TC_PARAM(struct tiles_info, ep_randomness, "randomness"),
	TC_PARAM(struct tiles_info, ep_variation, "variation"),
	TC_PARAM(struct tiles_info, ep_easing, "easing"),
	TC_PARAM(struct tiles_info, ep_mode, "mode"),
	TC_PARAM(struct tiles_info, ep_shape, "shape"),
	TC_PARAM(struct tiles_info, ep_direction, "direction"),
	TC_PARAM(struct tiles_info, ep_invert, "invert"),
	TC_PARAM(struct tiles_info, ep_reverse_exit, "reverse_exit"),
	TC_PARAM(struct tiles_info, ep_b_tiles, "b_tiles"),
};

/* clang-format on */

static void *tiles_create(obs_data_t *settings, obs_source_t *source)
{
	struct tiles_info *tiles = bzalloc(sizeof(struct tiles_info));

	if (!tc_base_init(&tiles->base, source, "tiles_transition.effect", tiles_params, TC_COUNT(tiles_params))) {
		bfree(tiles);
		return NULL;
	}

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

	tc_base_free(&tiles->base);
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
	obs_property_list_add_int(p, T_("Tiles.Shape.Diamond"), TILES_SHAPE_DIAMOND);
	obs_property_list_add_int(p, T_("Tiles.Shape.Brick"), TILES_SHAPE_BRICK);
	obs_property_list_add_int(p, T_("Tiles.Shape.Octagon"), TILES_SHAPE_OCTAGON);
	obs_property_list_add_int(p, T_("Tiles.Shape.Cairo"), TILES_SHAPE_CAIRO);
	obs_property_list_add_int(p, T_("Tiles.Shape.Mosaic"), TILES_SHAPE_MOSAIC);
	obs_property_list_add_int(p, T_("Tiles.Shape.Shatter"), TILES_SHAPE_SHATTER);

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

	p = obs_properties_add_float_slider(props, S_TILE_DURATION, T_("Tiles.TileDuration"), 1.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Tiles.TileDuration.Description"));

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
	obs_data_set_default_double(settings, S_TILE_DURATION, 35.0);
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

static void tiles_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct tiles_info *tiles = data;
	struct vec2 canvas;
	struct vec2 origin;
	struct vec2 grid_rot;
	struct vec2 dir;
	struct vec2 range;
	float dir_v[2];
	float range_v[2];
	float band;
	float tile_dur = tiles->tile_duration;
	struct tc_frame frame;

	vec2_set(&canvas, (float)cx, (float)cy);
	vec2_set(&origin, tiles->origin_x * (float)cx, tiles->origin_y * (float)cy);
	vec2_set(&grid_rot, cosf(tiles->grid_rotation), sinf(tiles->grid_rotation));

	tiles_sweep_extent(tiles->direction, tiles->angle, origin.x, origin.y, (float)cx, (float)cy,
			   tiles_circumradius(tiles->shape, tiles->tile_px), dir_v, range_v);
	vec2_set(&dir, dir_v[0], dir_v[1]);
	vec2_set(&range, range_v[0], range_v[1]);

	band = tiles->band_px / range.y;
	if (tiles->mode == TILES_MODE_WAVE)
		tiles_wave_fit(&tile_dur, &band);

	tc_frame_begin(&frame);
	tc_set_texture(&frame, tiles->ep_a_tex, a);
	tc_set_texture(&frame, tiles->ep_b_tex, b);
	tc_set_color(&frame, tiles->ep_color_a, &tiles->color_a);
	tc_set_color(&frame, tiles->ep_color_b, &tiles->color_b);

	gs_effect_set_vec2(tiles->ep_canvas, &canvas);
	gs_effect_set_vec2(tiles->ep_origin_px, &origin);
	gs_effect_set_vec2(tiles->ep_grid_rot, &grid_rot);
	gs_effect_set_vec2(tiles->ep_dir_vec, &dir);
	gs_effect_set_vec2(tiles->ep_dir_range, &range);

	/* linear: the easing shapes the launch order inside the effect instead,
	 * so every tile grows at the same rate wherever it sits in the sweep */
	gs_effect_set_float(tiles->ep_progress, t);
	gs_effect_set_float(tiles->ep_tile_px, tiles->tile_px);
	gs_effect_set_float(tiles->ep_edge, tiles_edge_width(tiles->shape, tiles->tile_px));
	gs_effect_set_float(tiles->ep_tile_dur, tile_dur);
	gs_effect_set_float(tiles->ep_band, band);
	gs_effect_set_float(tiles->ep_randomness, tiles->randomness);
	gs_effect_set_float(tiles->ep_variation, tiles->variation);

	gs_effect_set_int(tiles->ep_easing, tiles->easing);
	gs_effect_set_int(tiles->ep_mode, tiles->mode);
	gs_effect_set_int(tiles->ep_shape, tiles->shape);
	gs_effect_set_int(tiles->ep_direction, tiles->direction);

	gs_effect_set_bool(tiles->ep_invert, tiles->invert);
	gs_effect_set_bool(tiles->ep_reverse_exit, tiles->reverse_exit);
	gs_effect_set_bool(tiles->ep_b_tiles, tiles->b_tiles);

	while (gs_effect_loop(tiles->base.effect, "Tiles"))
		gs_draw_sprite(NULL, 0, cx, cy);

	tc_frame_end(&frame);
}

static void tiles_video_render(void *data, gs_effect_t *effect)
{
	struct tiles_info *tiles = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(tiles->base.source, tiles_callback);
}

struct obs_source_info tiles_transition = {
	.id = "voidscape_tiles_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = tiles_get_name,
	.create = tiles_create,
	.destroy = tiles_destroy,
	.update = tiles_update,
	.video_render = tiles_video_render,
	.audio_render = tc_audio_render,
	.get_properties = tiles_properties,
	.get_defaults = tiles_defaults,
	.video_get_color_space = tc_video_get_color_space,
};
