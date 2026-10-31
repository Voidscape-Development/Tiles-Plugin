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
 * A spiral portal transition: a ragged portal opens over the outgoing scene,
 * swallows the canvas as a turning vortex, and lets the incoming scene erode
 * back in through the smoke.
 *
 * It shares nothing with the tiling transition but the module it ships in, so
 * it is a source of its own rather than another mode. There is no lattice here
 * and none of the tile properties would mean anything.
 *
 * The look is generated in data/effects/vortex_transition.effect; this file
 * only turns properties into uniforms, works out the geometry that depends on
 * the canvas size, and evaluates the handful of curves that are the same for
 * every pixel in a frame so the shader does not have to.
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <math.h>

#include "transition-common.h"
#include "vortex-math.h"
#include "vortex-transition.h"

/* clang-format off */

#define S_ORIGIN_X     "origin_x"
#define S_ORIGIN_Y     "origin_y"
#define S_ARMS         "arms"
#define S_TWIST        "twist"
#define S_SPIN         "spin"
#define S_DETAIL       "detail"
#define S_ROUGHNESS    "roughness"
#define S_INTENSITY    "intensity"
#define S_COLOR_CORE   "color_core"
#define S_CORE_BLEED   "core_bleed"
#define S_CORE_BLEND   "core_blend"
#define S_COLOR_BODY   "color_body"
#define S_COLOR_BODY2  "color_body2"
#define S_COLOR_BODY3  "color_body3"
#define S_COLOR_BODY4  "color_body4"
#define S_COLOR_COUNT  "color_count"
#define S_COLOR_MIX    "color_mix"
#define S_OPEN_END     "open_end"
#define S_REVEAL_START "reveal_start"
#define S_SMOKE_SCALE  "smoke_scale"
#define S_SMOKE_SOFT   "smoke_soft"
#define S_SWIRL        "swirl"
#define S_SWIRL_AMOUNT "swirl_amount"

#define T_(x)          obs_module_text(x)

static const char *const body_setting[VORTEX_BODY_COLORS] = {
	S_COLOR_BODY, S_COLOR_BODY2, S_COLOR_BODY3, S_COLOR_BODY4,
};

static const char *const body_label[VORTEX_BODY_COLORS] = {
	"Vortex.ColorBody", "Vortex.ColorBody2", "Vortex.ColorBody3", "Vortex.ColorBody4",
};

/* obs colours are 0xAABBGGRR: #7A3FD0 purple body, then a teal, a magenta and a
 * blue that mix with it without any pair of them turning muddy. */
static const uint32_t body_default[VORTEX_BODY_COLORS] = {
	0xFFD03F7A, 0xFFD0B53F, 0xFF9E3FD0, 0xFFD05A3F,
};

/* clang-format on */

struct vortex_info {
	struct transition_base base;

	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_canvas;
	gs_eparam_t *ep_origin_px;
	gs_eparam_t *ep_unit_px;
	gs_eparam_t *ep_reach;
	gs_eparam_t *ep_progress;
	gs_eparam_t *ep_open_end;
	gs_eparam_t *ep_reveal_start;
	gs_eparam_t *ep_cut;
	gs_eparam_t *ep_iris_k;
	gs_eparam_t *ep_reveal_k;
	gs_eparam_t *ep_glow;
	gs_eparam_t *ep_arms;
	gs_eparam_t *ep_twist;
	gs_eparam_t *ep_spin;
	gs_eparam_t *ep_detail;
	gs_eparam_t *ep_fringe;
	gs_eparam_t *ep_intensity;
	gs_eparam_t *ep_smoke_scale;
	gs_eparam_t *ep_smoke_soft;
	gs_eparam_t *ep_swirl_angle;
	gs_eparam_t *ep_swirl_pull;
	gs_eparam_t *ep_color_bright;
	gs_eparam_t *ep_body[VORTEX_BODY_COLORS];
	gs_eparam_t *ep_body_span;
	gs_eparam_t *ep_color_mix;
	gs_eparam_t *ep_core_level;
	gs_eparam_t *ep_core_width;

	float origin_x; /* 0..1 of canvas width */
	float origin_y; /* 0..1 of canvas height */
	float arms;
	float twist;
	float spin;
	float detail;
	float fringe;
	float intensity;
	float open_end;
	float reveal_start;
	float smoke_scale;
	float smoke_soft;
	float warp;      /* radians, 0 when the swirl is off */
	float warp_pull; /* zoom that travels with the swirl */

	float body_span; /* body colour count minus one */
	int color_mix;
	float core_level;
	float core_width;

	struct tc_color color_bright;
	struct tc_color body[VORTEX_BODY_COLORS];
};

static const char *vortex_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Vortex.Transition");
}

static void vortex_update(void *data, obs_data_t *settings)
{
	struct vortex_info *vortex = data;
	float bleed;
	float blend;
	int count;
	int i;

	vortex->origin_x = (float)obs_data_get_double(settings, S_ORIGIN_X) / 100.0f;
	vortex->origin_y = (float)obs_data_get_double(settings, S_ORIGIN_Y) / 100.0f;

	/* integral: the log-polar noise wraps on a lattice of this period, and a
	 * fractional period would leave a seam down the +x axis */
	vortex->arms = (float)obs_data_get_int(settings, S_ARMS);

	vortex->twist = (float)obs_data_get_double(settings, S_TWIST);
	vortex->spin = (float)obs_data_get_double(settings, S_SPIN);
	vortex->detail = (float)obs_data_get_double(settings, S_DETAIL);
	vortex->fringe = (float)obs_data_get_double(settings, S_ROUGHNESS) / 100.0f;
	vortex->intensity = (float)obs_data_get_double(settings, S_INTENSITY) / 100.0f;
	vortex->smoke_scale = (float)obs_data_get_double(settings, S_SMOKE_SCALE);
	vortex->smoke_soft = (float)obs_data_get_double(settings, S_SMOKE_SOFT) / 100.0f;

	vortex->open_end = (float)obs_data_get_double(settings, S_OPEN_END) / 100.0f;
	vortex->reveal_start = (float)obs_data_get_double(settings, S_REVEAL_START) / 100.0f;

	if (obs_data_get_bool(settings, S_SWIRL)) {
		float amount = (float)obs_data_get_double(settings, S_SWIRL_AMOUNT) / 100.0f;

		vortex->warp = amount * 3.0f; /* 100% is a little over half a turn */
		vortex->warp_pull = amount * 0.35f;
	} else {
		vortex->warp = 0.0f;
		vortex->warp_pull = 0.0f;
	}

	tc_color_from_setting(&vortex->color_bright, settings, S_COLOR_CORE);

	for (i = 0; i < VORTEX_BODY_COLORS; i++)
		tc_color_from_setting(&vortex->body[i], settings, body_setting[i]);

	count = (int)obs_data_get_int(settings, S_COLOR_COUNT);
	if (count < 1)
		count = 1;
	if (count > VORTEX_BODY_COLORS)
		count = VORTEX_BODY_COLORS;

	/* A span of zero puts every blend weight in the shader at zero, so a
	 * single colour is the first colour exactly and the selector noise is
	 * never run. The colours past the count are left uploaded but
	 * unreachable. */
	vortex->body_span = (float)(count - 1);
	vortex->color_mix = (int)obs_data_get_int(settings, S_COLOR_MIX);

	bleed = (float)obs_data_get_double(settings, S_CORE_BLEED) / 100.0f;
	blend = (float)obs_data_get_double(settings, S_CORE_BLEND) / 100.0f;
	bleed = clampf(bleed, 0.0f, 1.0f);
	blend = clampf(blend, 0.0f, 1.0f);

	vortex->core_level = vortex_core_level(bleed);
	vortex->core_width = vortex_core_width(bleed, blend);

	vortex->arms = clampf(vortex->arms, 1.0f, 16.0f);
	vortex_clamp_phases(&vortex->open_end, &vortex->reveal_start);
	vortex->smoke_soft = clampf(vortex->smoke_soft, 0.02f, 1.0f);
}

/* clang-format off */

static const struct tc_param vortex_params[] = {
	TC_PARAM(struct vortex_info, ep_a_tex, "a_tex"),
	TC_PARAM(struct vortex_info, ep_b_tex, "b_tex"),
	TC_PARAM(struct vortex_info, ep_canvas, "canvas"),
	TC_PARAM(struct vortex_info, ep_origin_px, "origin_px"),
	TC_PARAM(struct vortex_info, ep_unit_px, "unit_px"),
	TC_PARAM(struct vortex_info, ep_reach, "reach"),
	TC_PARAM(struct vortex_info, ep_progress, "progress"),
	TC_PARAM(struct vortex_info, ep_open_end, "open_end"),
	TC_PARAM(struct vortex_info, ep_reveal_start, "reveal_start"),
	TC_PARAM(struct vortex_info, ep_cut, "cut"),
	TC_PARAM(struct vortex_info, ep_iris_k, "iris_k"),
	TC_PARAM(struct vortex_info, ep_reveal_k, "reveal_k"),
	TC_PARAM(struct vortex_info, ep_glow, "glow"),
	TC_PARAM(struct vortex_info, ep_arms, "arms"),
	TC_PARAM(struct vortex_info, ep_twist, "twist"),
	TC_PARAM(struct vortex_info, ep_spin, "spin"),
	TC_PARAM(struct vortex_info, ep_detail, "detail"),
	TC_PARAM(struct vortex_info, ep_fringe, "fringe"),
	TC_PARAM(struct vortex_info, ep_intensity, "intensity"),
	TC_PARAM(struct vortex_info, ep_smoke_scale, "smoke_scale"),
	TC_PARAM(struct vortex_info, ep_smoke_soft, "smoke_soft"),
	TC_PARAM(struct vortex_info, ep_swirl_angle, "swirl_angle"),
	TC_PARAM(struct vortex_info, ep_swirl_pull, "swirl_pull"),
	TC_PARAM(struct vortex_info, ep_color_bright, "color_bright"),
	TC_PARAM(struct vortex_info, ep_body_span, "body_span"),
	TC_PARAM(struct vortex_info, ep_color_mix, "color_mix"),
	TC_PARAM(struct vortex_info, ep_core_level, "core_level"),
	TC_PARAM(struct vortex_info, ep_core_width, "core_width"),
	TC_PARAM(struct vortex_info, ep_body[0], "color_deep"),
	TC_PARAM(struct vortex_info, ep_body[1], "color_deep2"),
	TC_PARAM(struct vortex_info, ep_body[2], "color_deep3"),
	TC_PARAM(struct vortex_info, ep_body[3], "color_deep4"),
};

/* clang-format on */

static void *vortex_create(obs_data_t *settings, obs_source_t *source)
{
	struct vortex_info *vortex = bzalloc(sizeof(struct vortex_info));

	if (!tc_base_init(&vortex->base, source, "vortex_transition.effect", vortex_params, TC_COUNT(vortex_params))) {
		bfree(vortex);
		return NULL;
	}

	/* Applied directly for the same reason the tiling transition does it: the
	 * source's data pointer is only assigned once create returns, so an update
	 * queued here would not reach us until a later tick. */
	vortex_update(vortex, settings);

	return vortex;
}

static void vortex_destroy(void *data)
{
	struct vortex_info *vortex = data;

	tc_base_free(&vortex->base);
	bfree(vortex);
}

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static bool vortex_layout_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	long long count = obs_data_get_int(settings, S_COLOR_COUNT);
	int i;

	UNUSED_PARAMETER(prop);

	obs_property_set_visible(obs_properties_get(props, S_SWIRL_AMOUNT), obs_data_get_bool(settings, S_SWIRL));

	/* A single colour is the old behaviour, so neither the extra colours nor
	 * the question of how to spread them is worth showing. */
	obs_property_set_visible(obs_properties_get(props, S_COLOR_MIX), count > 1);

	for (i = 1; i < VORTEX_BODY_COLORS; i++)
		obs_property_set_visible(obs_properties_get(props, body_setting[i]), count > (long long)i);

	return true;
}

static obs_properties_t *vortex_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	int i;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_int_slider(props, S_ARMS, T_("Vortex.Arms"), 1, 16, 1);
	obs_property_set_long_description(p, T_("Vortex.Arms.Description"));

	p = obs_properties_add_float_slider(props, S_TWIST, T_("Vortex.Twist"), 0.0, 6.0, 0.05);
	obs_property_set_long_description(p, T_("Vortex.Twist.Description"));

	p = obs_properties_add_float_slider(props, S_SPIN, T_("Vortex.Spin"), -3.0, 3.0, 0.05);
	obs_property_set_long_description(p, T_("Vortex.Spin.Description"));

	obs_properties_add_float_slider(props, S_DETAIL, T_("Vortex.Detail"), 0.5, 6.0, 0.1);

	p = obs_properties_add_float_slider(props, S_ROUGHNESS, T_("Vortex.Roughness"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Vortex.Roughness.Description"));

	p = obs_properties_add_float_slider(props, S_INTENSITY, T_("Vortex.Intensity"), 0.0, 300.0, 1.0);
	obs_property_float_set_suffix(p, " %");

	obs_properties_add_color(props, S_COLOR_CORE, T_("Vortex.ColorCore"));

	p = obs_properties_add_float_slider(props, S_CORE_BLEED, T_("Vortex.CoreBleed"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Vortex.CoreBleed.Description"));

	p = obs_properties_add_float_slider(props, S_CORE_BLEND, T_("Vortex.CoreBlend"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Vortex.CoreBlend.Description"));

	obs_properties_add_color(props, S_COLOR_BODY, T_("Vortex.ColorBody"));

	p = obs_properties_add_int_slider(props, S_COLOR_COUNT, T_("Vortex.ColorCount"), 1, VORTEX_BODY_COLORS, 1);
	obs_property_set_long_description(p, T_("Vortex.ColorCount.Description"));
	obs_property_set_modified_callback(p, vortex_layout_modified);

	p = obs_properties_add_list(props, S_COLOR_MIX, T_("Vortex.ColorMix"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Vortex.ColorMix.Arms"), VORTEX_MIX_ARMS);
	obs_property_list_add_int(p, T_("Vortex.ColorMix.Radial"), VORTEX_MIX_RADIAL);
	obs_property_list_add_int(p, T_("Vortex.ColorMix.Bands"), VORTEX_MIX_BANDS);
	obs_property_set_long_description(p, T_("Vortex.ColorMix.Description"));

	for (i = 1; i < VORTEX_BODY_COLORS; i++)
		obs_properties_add_color(props, body_setting[i], T_(body_label[i]));

	p = obs_properties_add_float_slider(props, S_ORIGIN_X, T_("Vortex.OriginX"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, S_ORIGIN_Y, T_("Vortex.OriginY"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_float_slider(props, S_OPEN_END, T_("Vortex.OpenEnd"), 2.0, 90.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Vortex.OpenEnd.Description"));

	p = obs_properties_add_float_slider(props, S_REVEAL_START, T_("Vortex.RevealStart"), 10.0, 95.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Vortex.RevealStart.Description"));

	obs_properties_add_float_slider(props, S_SMOKE_SCALE, T_("Vortex.SmokeScale"), 0.5, 8.0, 0.1);

	p = obs_properties_add_float_slider(props, S_SMOKE_SOFT, T_("Vortex.SmokeSoftness"), 5.0, 80.0, 1.0);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_bool(props, S_SWIRL, T_("Vortex.Swirl"));
	obs_property_set_long_description(p, T_("Vortex.Swirl.Description"));
	obs_property_set_modified_callback(p, vortex_layout_modified);

	p = obs_properties_add_float_slider(props, S_SWIRL_AMOUNT, T_("Vortex.SwirlAmount"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " %");

	return props;
}

static void vortex_defaults(obs_data_t *settings)
{
	int i;

	obs_data_set_default_double(settings, S_ORIGIN_X, 50.0);
	obs_data_set_default_double(settings, S_ORIGIN_Y, 50.0);
	obs_data_set_default_int(settings, S_ARMS, 5);
	obs_data_set_default_double(settings, S_TWIST, 1.6);
	obs_data_set_default_double(settings, S_SPIN, 0.9);
	obs_data_set_default_double(settings, S_DETAIL, 2.0);
	obs_data_set_default_double(settings, S_ROUGHNESS, 35.0);
	obs_data_set_default_double(settings, S_INTENSITY, 100.0);

	/* obs colours are 0xAABBGGRR: #E8DCFF core */
	obs_data_set_default_int(settings, S_COLOR_CORE, 0xFFFFDCE8);

	/* 50% on both is the ramp the look was tuned with */
	obs_data_set_default_double(settings, S_CORE_BLEED, 50.0);
	obs_data_set_default_double(settings, S_CORE_BLEND, 50.0);

	for (i = 0; i < VORTEX_BODY_COLORS; i++)
		obs_data_set_default_int(settings, body_setting[i], (long long)body_default[i]);

	/* one colour: the extra slots are there to be turned on, not on by default */
	obs_data_set_default_int(settings, S_COLOR_COUNT, 1);
	obs_data_set_default_int(settings, S_COLOR_MIX, VORTEX_MIX_ARMS);

	obs_data_set_default_double(settings, S_OPEN_END, 22.0);
	obs_data_set_default_double(settings, S_REVEAL_START, 62.0);
	obs_data_set_default_double(settings, S_SMOKE_SCALE, 4.0);
	obs_data_set_default_double(settings, S_SMOKE_SOFT, 30.0);
	obs_data_set_default_bool(settings, S_SWIRL, false);
	obs_data_set_default_double(settings, S_SWIRL_AMOUNT, 60.0);
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */

/* Field units are half the canvas height, so a circle in field space is a
 * circle on screen whatever the aspect ratio. */
static void vortex_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct vortex_info *vortex = data;
	struct vec2 canvas;
	struct vec2 origin;
	float unit_px = fmaxf((float)cy * 0.5f, 1.0f);
	float reach;
	struct vortex_clock clock;
	struct tc_frame frame;
	int i;

	vec2_set(&canvas, (float)cx, (float)cy);
	vec2_set(&origin, vortex->origin_x * (float)cx, vortex->origin_y * (float)cy);

	reach = vortex_reach(origin.x, origin.y, (float)cx, (float)cy, unit_px);

	vortex_clock_at(vortex->open_end, vortex->reveal_start, vortex->warp, vortex->warp_pull, t, &clock);

	tc_frame_begin(&frame);
	tc_set_texture(&frame, vortex->ep_a_tex, a);
	tc_set_texture(&frame, vortex->ep_b_tex, b);
	tc_set_color(&frame, vortex->ep_color_bright, &vortex->color_bright);

	for (i = 0; i < VORTEX_BODY_COLORS; i++)
		tc_set_color(&frame, vortex->ep_body[i], &vortex->body[i]);

	gs_effect_set_vec2(vortex->ep_canvas, &canvas);
	gs_effect_set_vec2(vortex->ep_origin_px, &origin);
	gs_effect_set_float(vortex->ep_unit_px, unit_px);
	gs_effect_set_float(vortex->ep_reach, reach);

	gs_effect_set_float(vortex->ep_progress, t);
	gs_effect_set_float(vortex->ep_open_end, vortex->open_end);
	gs_effect_set_float(vortex->ep_reveal_start, vortex->reveal_start);
	gs_effect_set_float(vortex->ep_cut, clock.cut);

	gs_effect_set_float(vortex->ep_iris_k, clock.iris_k);
	gs_effect_set_float(vortex->ep_reveal_k, clock.reveal_k);
	gs_effect_set_float(vortex->ep_glow, clock.glow);

	gs_effect_set_float(vortex->ep_arms, vortex->arms);
	gs_effect_set_float(vortex->ep_twist, vortex->twist);
	gs_effect_set_float(vortex->ep_spin, vortex->spin);
	gs_effect_set_float(vortex->ep_detail, vortex->detail);
	gs_effect_set_float(vortex->ep_fringe, vortex->fringe);
	gs_effect_set_float(vortex->ep_intensity, vortex->intensity);
	gs_effect_set_float(vortex->ep_smoke_scale, vortex->smoke_scale);
	gs_effect_set_float(vortex->ep_smoke_soft, vortex->smoke_soft);
	gs_effect_set_float(vortex->ep_swirl_angle, clock.swirl_angle);
	gs_effect_set_float(vortex->ep_swirl_pull, clock.swirl_pull);

	gs_effect_set_float(vortex->ep_body_span, vortex->body_span);
	gs_effect_set_int(vortex->ep_color_mix, vortex->color_mix);
	gs_effect_set_float(vortex->ep_core_level, vortex->core_level);
	gs_effect_set_float(vortex->ep_core_width, vortex->core_width);

	while (gs_effect_loop(vortex->base.effect, "Vortex"))
		gs_draw_sprite(NULL, 0, cx, cy);

	tc_frame_end(&frame);
}

static void vortex_video_render(void *data, gs_effect_t *effect)
{
	struct vortex_info *vortex = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(vortex->base.source, vortex_callback);
}

struct obs_source_info vortex_transition = {
	.id = "voidscape_vortex_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = vortex_get_name,
	.create = vortex_create,
	.destroy = vortex_destroy,
	.update = vortex_update,
	.video_render = vortex_video_render,
	.audio_render = tc_audio_render,
	.get_properties = vortex_properties,
	.get_defaults = vortex_defaults,
	.video_get_color_space = tc_video_get_color_space,
};
