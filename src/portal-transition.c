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
 * A portal transition: an oval portal is shot open over the outgoing scene,
 * the incoming scene showing through it, holds a beat, and then grows until
 * it has swallowed the canvas.
 *
 * The look is generated in data/effects/portal_transition.effect, and the
 * schedule - how big the portal is on any frame, and how big it has to get to
 * clear the canvas - is in src/portal-math.c, shared with the tests. This file
 * turns properties into uniforms.
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <math.h>

#include "portal-math.h"
#include "portal-transition.h"

/* clang-format off */

#define S_PRESET       "preset"
#define S_COLOR        "portal_color"
#define S_CUSTOM_COLOR "custom_color"
#define S_ORIGIN_X     "origin_x"
#define S_ORIGIN_Y     "origin_y"
#define S_WIDTH        "width"
#define S_SIZE         "size"
#define S_OPEN         "open"
#define S_HOLD         "hold"
#define S_RIM          "rim"
#define S_GLOW         "glow"
#define S_GLOW_REACH   "glow_reach"
#define S_SWIRL        "swirl"
#define S_SPIRAL       "spiral_open"
#define S_SPARKS       "sparks"
#define S_DISTORT      "distortion"

#define T_(x)          obs_module_text(x)

/* obs colours are 0xAABBGGRR */
#define RGB_(r, g, b)  (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* clang-format on */

/* The grow has to have time to happen, so the open and the hold together stop
 * short of the end. */
#define PORTAL_HOLD_CEILING 0.9f

/* How far a spark flies, in rim widths, and the least it flies at all. */
#define PORTAL_SPARK_RIMS 7.0f
#define PORTAL_SPARK_MIN 50.0f

enum portal_color_id {
	PORTAL_COLOR_BLUE = 0,
	PORTAL_COLOR_ORANGE = 1,
	PORTAL_COLOR_CUSTOM = 2,
};

/* The two portals, rim and core. */
#define BLUE_RIM RGB_(0x1E, 0x9B, 0xFF)
#define BLUE_CORE RGB_(0xD2, 0xF0, 0xFF)
#define ORANGE_RIM RGB_(0xFF, 0x7A, 0x00)
#define ORANGE_CORE RGB_(0xFF, 0xE4, 0xC0)

enum portal_preset_id {
	PORTAL_PRESET_CUSTOM = 0,
};

/* A whole look in one row; the centre and a custom colour are left alone. */
struct portal_preset {
	const char *label;
	int color;
	double width;
	double size;
	double open;
	double hold;
	double rim;
	double glow;
	double glow_reach;
	double swirl;
	bool spiral;
	double sparks;
	double distort;
};

/* clang-format off */

static const struct portal_preset portal_presets[] = {
	/* label                       colour               width size  open  hold  rim   glow   reach swirl spiral sparks distort */
	{"Portal.Preset.Blue",        PORTAL_COLOR_BLUE,   62.0, 55.0, 22.0, 28.0, 14.0, 100.0, 40.0, 1.0,  true,  100.0, 10.0},
	{"Portal.Preset.Orange",      PORTAL_COLOR_ORANGE, 62.0, 55.0, 22.0, 28.0, 14.0, 100.0, 40.0, 1.0,  true,  100.0, 10.0},
	{"Portal.Preset.Quick",       PORTAL_COLOR_BLUE,   62.0, 50.0, 14.0, 10.0, 12.0, 120.0, 35.0, 1.5,  false, 140.0,  8.0},
	{"Portal.Preset.Slow",        PORTAL_COLOR_ORANGE, 62.0, 60.0, 30.0, 35.0, 16.0,  90.0, 50.0, 0.7,  true,   60.0, 14.0},
};

/* clang-format on */

#define PORTAL_PRESET_COUNT ((int)(sizeof(portal_presets) / sizeof(portal_presets[0])))

struct portal_info {
	obs_source_t *source;
	gs_effect_t *effect;

	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_canvas;
	gs_eparam_t *ep_centre_px;
	gs_eparam_t *ep_aspect;
	gs_eparam_t *ep_radius;
	gs_eparam_t *ep_progress;
	gs_eparam_t *ep_rim;
	gs_eparam_t *ep_outer;
	gs_eparam_t *ep_distort;
	gs_eparam_t *ep_energy;
	gs_eparam_t *ep_sparks;
	gs_eparam_t *ep_spark_reach;
	gs_eparam_t *ep_swirl;
	gs_eparam_t *ep_intensity;
	gs_eparam_t *ep_color_rim;
	gs_eparam_t *ep_color_core;

	float origin_x; /* 0..1 of the canvas */
	float origin_y;
	float size; /* settled oval height as a share of the canvas height */
	float glow_reach;
	float swirl;
	float sparks;
	float distort;
	float intensity;
	struct portal_params params; /* settled is filled in per frame */

	struct vec4 rim_color, rim_color_srgb;
	struct vec4 core_color, core_color_srgb;
};

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static const char *portal_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Portal.Transition");
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */

static void portal_update(void *data, obs_data_t *settings)
{
	struct portal_info *portal = data;
	int color_id = (int)obs_data_get_int(settings, S_COLOR);
	uint32_t rim, core;
	float hold;

	portal->origin_x = (float)obs_data_get_double(settings, S_ORIGIN_X) / 100.0f;
	portal->origin_y = (float)obs_data_get_double(settings, S_ORIGIN_Y) / 100.0f;
	portal->size = clampf((float)obs_data_get_double(settings, S_SIZE) / 100.0f, 0.05f, 1.5f);
	portal->glow_reach = fmaxf((float)obs_data_get_double(settings, S_GLOW_REACH), 0.0f);
	portal->swirl = (float)obs_data_get_double(settings, S_SWIRL);
	portal->sparks = fmaxf((float)obs_data_get_double(settings, S_SPARKS) / 100.0f, 0.0f);
	portal->distort = fmaxf((float)obs_data_get_double(settings, S_DISTORT), 0.0f);
	portal->intensity = fmaxf((float)obs_data_get_double(settings, S_GLOW) / 100.0f, 0.0f);

	portal->params.aspect = clampf((float)obs_data_get_double(settings, S_WIDTH) / 100.0f, 0.2f, 2.0f);
	portal->params.rim_px = fmaxf((float)obs_data_get_double(settings, S_RIM), 1.0f);
	portal->params.spiral_open = obs_data_get_bool(settings, S_SPIRAL);

	/* Like the glass transition, the open keeps the value it was given and the
	 * hold gives way, so the grow always has time left to happen in. */
	portal->params.open_end =
		clampf((float)obs_data_get_double(settings, S_OPEN) / 100.0f, 0.02f, PORTAL_HOLD_CEILING);
	hold = fmaxf((float)obs_data_get_double(settings, S_HOLD) / 100.0f, 0.0f);
	portal->params.hold_end = clampf(portal->params.open_end + hold, portal->params.open_end, PORTAL_HOLD_CEILING);

	switch (color_id) {
	case PORTAL_COLOR_ORANGE:
		rim = ORANGE_RIM;
		core = ORANGE_CORE;
		break;
	case PORTAL_COLOR_CUSTOM: {
		struct vec4 c;

		rim = (uint32_t)obs_data_get_int(settings, S_CUSTOM_COLOR) | 0xFF000000;

		/* the core is the custom colour most of the way to white, which
		 * is how both of the stock portals are built */
		vec4_from_rgba(&c, rim);
		vec4_set(&c, c.x + (1.0f - c.x) * 0.75f, c.y + (1.0f - c.y) * 0.75f, c.z + (1.0f - c.z) * 0.75f, 1.0f);
		core = vec4_to_rgba(&c) | 0xFF000000;
		break;
	}
	case PORTAL_COLOR_BLUE:
	default:
		rim = BLUE_RIM;
		core = BLUE_CORE;
		break;
	}

	vec4_from_rgba(&portal->rim_color, rim);
	vec4_from_rgba_srgb(&portal->rim_color_srgb, rim);
	vec4_from_rgba(&portal->core_color, core);
	vec4_from_rgba_srgb(&portal->core_color_srgb, core);
}

static void portal_defaults(obs_data_t *settings)
{
	const struct portal_preset *p = &portal_presets[0];

	obs_data_set_default_int(settings, S_PRESET, 1);
	obs_data_set_default_int(settings, S_COLOR, p->color);
	obs_data_set_default_int(settings, S_CUSTOM_COLOR, RGB_(0x3D, 0xFF, 0x8C));
	obs_data_set_default_double(settings, S_ORIGIN_X, 50.0);
	obs_data_set_default_double(settings, S_ORIGIN_Y, 50.0);
	obs_data_set_default_double(settings, S_WIDTH, p->width);
	obs_data_set_default_double(settings, S_SIZE, p->size);
	obs_data_set_default_double(settings, S_OPEN, p->open);
	obs_data_set_default_double(settings, S_HOLD, p->hold);
	obs_data_set_default_double(settings, S_RIM, p->rim);
	obs_data_set_default_double(settings, S_GLOW, p->glow);
	obs_data_set_default_double(settings, S_GLOW_REACH, p->glow_reach);
	obs_data_set_default_double(settings, S_SWIRL, p->swirl);
	obs_data_set_default_bool(settings, S_SPIRAL, p->spiral);
	obs_data_set_default_double(settings, S_SPARKS, p->sparks);
	obs_data_set_default_double(settings, S_DISTORT, p->distort);
}

static bool near_enough(double a, double b)
{
	return fabs(a - b) < 0.001;
}

static bool preset_matches(const struct portal_preset *p, obs_data_t *s)
{
	return (int)obs_data_get_int(s, S_COLOR) == p->color && obs_data_get_bool(s, S_SPIRAL) == p->spiral &&
	       near_enough(obs_data_get_double(s, S_WIDTH), p->width) &&
	       near_enough(obs_data_get_double(s, S_SIZE), p->size) &&
	       near_enough(obs_data_get_double(s, S_OPEN), p->open) &&
	       near_enough(obs_data_get_double(s, S_HOLD), p->hold) &&
	       near_enough(obs_data_get_double(s, S_RIM), p->rim) &&
	       near_enough(obs_data_get_double(s, S_GLOW), p->glow) &&
	       near_enough(obs_data_get_double(s, S_GLOW_REACH), p->glow_reach) &&
	       near_enough(obs_data_get_double(s, S_SWIRL), p->swirl) &&
	       near_enough(obs_data_get_double(s, S_SPARKS), p->sparks) &&
	       near_enough(obs_data_get_double(s, S_DISTORT), p->distort);
}

static void preset_apply(const struct portal_preset *p, obs_data_t *s)
{
	obs_data_set_int(s, S_COLOR, p->color);
	obs_data_set_bool(s, S_SPIRAL, p->spiral);
	obs_data_set_double(s, S_WIDTH, p->width);
	obs_data_set_double(s, S_SIZE, p->size);
	obs_data_set_double(s, S_OPEN, p->open);
	obs_data_set_double(s, S_HOLD, p->hold);
	obs_data_set_double(s, S_RIM, p->rim);
	obs_data_set_double(s, S_GLOW, p->glow);
	obs_data_set_double(s, S_GLOW_REACH, p->glow_reach);
	obs_data_set_double(s, S_SWIRL, p->swirl);
	obs_data_set_double(s, S_SPARKS, p->sparks);
	obs_data_set_double(s, S_DISTORT, p->distort);
}

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static void portal_layout(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_set_visible(obs_properties_get(props, S_CUSTOM_COLOR),
				 (int)obs_data_get_int(settings, S_COLOR) == PORTAL_COLOR_CUSTOM);
}

static bool portal_preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= 1 && id <= PORTAL_PRESET_COUNT)
		preset_apply(&portal_presets[id - 1], settings);

	portal_layout(props, settings);
	return true;
}

static bool portal_touched(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= 1 && id <= PORTAL_PRESET_COUNT && !preset_matches(&portal_presets[id - 1], settings))
		obs_data_set_int(settings, S_PRESET, PORTAL_PRESET_CUSTOM);

	portal_layout(props, settings);
	return true;
}

static obs_property_t *watched(obs_property_t *prop)
{
	obs_property_set_modified_callback(prop, portal_touched);
	return prop;
}

static obs_properties_t *portal_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	int i;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_list(props, S_PRESET, T_("Portal.Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Portal.Preset.Custom"), PORTAL_PRESET_CUSTOM);
	for (i = 0; i < PORTAL_PRESET_COUNT; i++)
		obs_property_list_add_int(p, T_(portal_presets[i].label), i + 1);
	obs_property_set_long_description(p, T_("Portal.Preset.Description"));
	obs_property_set_modified_callback(p, portal_preset_modified);

	p = watched(
		obs_properties_add_list(props, S_COLOR, T_("Portal.Color"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
	obs_property_list_add_int(p, T_("Portal.Color.Blue"), PORTAL_COLOR_BLUE);
	obs_property_list_add_int(p, T_("Portal.Color.Orange"), PORTAL_COLOR_ORANGE);
	obs_property_list_add_int(p, T_("Portal.Color.Custom"), PORTAL_COLOR_CUSTOM);

	obs_properties_add_color(props, S_CUSTOM_COLOR, T_("Portal.CustomColor"));

	p = obs_properties_add_float_slider(props, S_ORIGIN_X, T_("Portal.OriginX"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, S_ORIGIN_Y, T_("Portal.OriginY"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");

	p = watched(obs_properties_add_float_slider(props, S_WIDTH, T_("Portal.Width"), 20.0, 200.0, 1.0));
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Portal.Width.Description"));

	p = watched(obs_properties_add_float_slider(props, S_SIZE, T_("Portal.Size"), 10.0, 120.0, 1.0));
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Portal.Size.Description"));

	p = watched(obs_properties_add_float_slider(props, S_OPEN, T_("Portal.Open"), 2.0, 80.0, 1.0));
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Portal.Open.Description"));

	p = watched(obs_properties_add_float_slider(props, S_HOLD, T_("Portal.Hold"), 0.0, 70.0, 1.0));
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Portal.Hold.Description"));

	p = watched(obs_properties_add_bool(props, S_SPIRAL, T_("Portal.SpiralOpen")));
	obs_property_set_long_description(p, T_("Portal.SpiralOpen.Description"));

	p = watched(obs_properties_add_float_slider(props, S_RIM, T_("Portal.Rim"), 2.0, 60.0, 0.5));
	obs_property_float_set_suffix(p, " px");

	p = watched(obs_properties_add_float_slider(props, S_GLOW, T_("Portal.Glow"), 0.0, 300.0, 1.0));
	obs_property_float_set_suffix(p, " %");

	p = watched(obs_properties_add_float_slider(props, S_GLOW_REACH, T_("Portal.GlowReach"), 0.0, 200.0, 1.0));
	obs_property_float_set_suffix(p, " px");

	p = watched(obs_properties_add_float_slider(props, S_SWIRL, T_("Portal.Swirl"), -4.0, 4.0, 0.05));
	obs_property_set_long_description(p, T_("Portal.Swirl.Description"));

	p = watched(obs_properties_add_float_slider(props, S_SPARKS, T_("Portal.Sparks"), 0.0, 300.0, 1.0));
	obs_property_float_set_suffix(p, " %");

	p = watched(obs_properties_add_float_slider(props, S_DISTORT, T_("Portal.Distortion"), 0.0, 60.0, 0.5));
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, T_("Portal.Distortion.Description"));

	return props;
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */

static void *portal_create(obs_data_t *settings, obs_source_t *source)
{
	struct portal_info *portal;
	gs_effect_t *effect;
	char *file = obs_module_file("effects/portal_transition.effect");

	if (!file) {
		obs_log(LOG_ERROR, "effects/portal_transition.effect is missing");
		return NULL;
	}

	obs_enter_graphics();
	effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();

	bfree(file);

	if (!effect) {
		obs_log(LOG_ERROR, "failed to compile effects/portal_transition.effect");
		return NULL;
	}

	portal = bzalloc(sizeof(struct portal_info));
	portal->source = source;
	portal->effect = effect;

	portal->ep_a_tex = gs_effect_get_param_by_name(effect, "a_tex");
	portal->ep_b_tex = gs_effect_get_param_by_name(effect, "b_tex");
	portal->ep_canvas = gs_effect_get_param_by_name(effect, "canvas");
	portal->ep_centre_px = gs_effect_get_param_by_name(effect, "centre_px");
	portal->ep_aspect = gs_effect_get_param_by_name(effect, "aspect");
	portal->ep_radius = gs_effect_get_param_by_name(effect, "radius");
	portal->ep_progress = gs_effect_get_param_by_name(effect, "progress");
	portal->ep_rim = gs_effect_get_param_by_name(effect, "rim");
	portal->ep_outer = gs_effect_get_param_by_name(effect, "outer");
	portal->ep_distort = gs_effect_get_param_by_name(effect, "distort");
	portal->ep_energy = gs_effect_get_param_by_name(effect, "energy");
	portal->ep_sparks = gs_effect_get_param_by_name(effect, "sparks");
	portal->ep_spark_reach = gs_effect_get_param_by_name(effect, "spark_reach");
	portal->ep_swirl = gs_effect_get_param_by_name(effect, "swirl");
	portal->ep_intensity = gs_effect_get_param_by_name(effect, "intensity");
	portal->ep_color_rim = gs_effect_get_param_by_name(effect, "color_rim");
	portal->ep_color_core = gs_effect_get_param_by_name(effect, "color_core");

	/* applied directly, as the other transitions do */
	portal_update(portal, settings);

	return portal;
}

static void portal_destroy(void *data)
{
	struct portal_info *portal = data;

	obs_enter_graphics();
	gs_effect_destroy(portal->effect);
	obs_leave_graphics();

	bfree(portal);
}

static void portal_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct portal_info *portal = data;
	struct portal_params pp = portal->params;
	struct portal_frame frame;
	struct vec2 canvas, centre;
	float fcx = (float)cx, fcy = (float)cy;
	float cover;

	bool nonlinear = gs_get_color_space() == GS_CS_SRGB;
	bool previous_srgb = gs_framebuffer_srgb_enabled();

	/* Size is the oval's whole height as a share of the canvas height, so a
	 * portal looks the same size at any resolution. */
	pp.settled = 0.5f * portal->size * fcy;

	vec2_set(&canvas, fcx, fcy);
	vec2_set(&centre, portal->origin_x * fcx, portal->origin_y * fcy);

	cover = portal_cover_radius(&pp, centre.x, centre.y, fcx, fcy);
	portal_frame_at(&pp, cover, t, &frame);

	gs_enable_framebuffer_srgb(!nonlinear);

	if (nonlinear) {
		gs_effect_set_texture(portal->ep_a_tex, a);
		gs_effect_set_texture(portal->ep_b_tex, b);
		gs_effect_set_vec4(portal->ep_color_rim, &portal->rim_color);
		gs_effect_set_vec4(portal->ep_color_core, &portal->core_color);
	} else {
		gs_effect_set_texture_srgb(portal->ep_a_tex, a);
		gs_effect_set_texture_srgb(portal->ep_b_tex, b);
		gs_effect_set_vec4(portal->ep_color_rim, &portal->rim_color_srgb);
		gs_effect_set_vec4(portal->ep_color_core, &portal->core_color_srgb);
	}

	gs_effect_set_vec2(portal->ep_canvas, &canvas);
	gs_effect_set_vec2(portal->ep_centre_px, &centre);
	gs_effect_set_float(portal->ep_aspect, pp.aspect);
	gs_effect_set_float(portal->ep_radius, frame.radius);
	gs_effect_set_float(portal->ep_progress, t);

	/* Every width scales with the portal while it opens, so a portal a few
	 * pixels across is not all rim. */
	gs_effect_set_float(portal->ep_rim, pp.rim_px * frame.size_k);
	gs_effect_set_float(portal->ep_outer, portal->glow_reach * frame.size_k);
	gs_effect_set_float(portal->ep_distort, portal->distort * frame.size_k);
	gs_effect_set_float(portal->ep_energy, frame.energy);
	gs_effect_set_float(portal->ep_sparks, portal->sparks * frame.spark_env);
	gs_effect_set_float(portal->ep_spark_reach,
			    fmaxf(PORTAL_SPARK_RIMS * pp.rim_px, PORTAL_SPARK_MIN) * frame.size_k);
	gs_effect_set_float(portal->ep_swirl, portal->swirl);
	gs_effect_set_float(portal->ep_intensity, portal->intensity);

	while (gs_effect_loop(portal->effect, "Portal"))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous_srgb);
}

static void portal_video_render(void *data, gs_effect_t *effect)
{
	struct portal_info *portal = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(portal->source, portal_callback);
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

static bool portal_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
				size_t channels, size_t sample_rate)
{
	struct portal_info *portal = data;

	return obs_transition_audio_render(portal->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static enum gs_color_space portal_video_get_color_space(void *data, size_t count,
							const enum gs_color_space *preferred_spaces)
{
	struct portal_info *portal = data;

	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return obs_transition_video_get_color_space(portal->source);
}

struct obs_source_info portal_transition = {
	.id = "voidscape_portal_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = portal_get_name,
	.create = portal_create,
	.destroy = portal_destroy,
	.update = portal_update,
	.video_render = portal_video_render,
	.audio_render = portal_audio_render,
	.get_properties = portal_properties,
	.get_defaults = portal_defaults,
	.video_get_color_space = portal_video_get_color_space,
};
