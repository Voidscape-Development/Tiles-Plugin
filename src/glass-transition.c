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
 * A shattering glass transition: the outgoing scene cracks from a point of
 * impact, holds a beat, and the shards fall away to leave the incoming one.
 *
 * The two transitions beside it in this module draw one full screen sprite and
 * decide per pixel what belongs there. This one cannot: a shard that leaves the
 * frame, turns over or sails past the camera is not a function of the pixel it
 * lands on, and finding out which shard did land on a pixel would mean
 * searching the break. So the pane is a real mesh - built by src/glass-mesh.c,
 * uploaded once, and flown by its vertex shader.
 *
 * That leaves this file three jobs the others do not have:
 *
 *   - build and cache the mesh, and rebuild it when the canvas or any setting
 *     that changes the break moves;
 *   - order the shards back to front every frame, because there is no depth
 *     buffer to lean on;
 *   - draw the incoming scene behind the mesh, since the shards no longer cover
 *     the whole canvas once they start to go.
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <math.h>

#include "glass-mesh.h"
#include "glass-transition.h"

/* clang-format off */

#define S_PRESET        "preset"
#define S_STYLE         "style"
#define S_PATTERN       "pattern"
#define S_SHARD_SIZE    "shard_size"
#define S_SEED          "seed"
#define S_IMPACT_X      "impact_x"
#define S_IMPACT_Y      "impact_y"
#define S_CRACK_SPREAD  "crack_spread"
#define S_HOLD          "hold"
#define S_EASING        "easing"
#define S_GRAVITY_ANGLE "gravity_angle"
#define S_GRAVITY       "gravity"
#define S_THROW         "throw"
#define S_SPIN          "spin"
#define S_SCATTER       "scatter"
#define S_FLAT          "flat_shards"
#define S_CRACK_COLOR   "crack_color"
#define S_CRACK_WIDTH   "crack_width"
#define S_CRACK_GLOW    "crack_glow"
#define S_RIM           "rim"
#define S_RIM_WIDTH     "rim_width"
#define S_REFRACT       "refract"
#define S_TINT          "tint"
#define S_GLINT         "glint"

#define T_(x)           obs_module_text(x)

/* clang-format on */

/* The cracks have to finish before the shards let go, or the pane would come
 * apart along breaks that had not happened yet. The hold is what is left
 * between them, and it is never allowed to close completely. */
#define GLASS_HOLD_GAP 0.02f

/* How far the flight reaches before the transition ends. Held below 1 so that
 * the shards are still going when the last frame arrives rather than having
 * stopped short of it. */
#define GLASS_HOLD_CEILING 0.90f

/*
 * How far the camera sits from the pane, in canvas heights. Shards pass it at
 * z = focal, so this also sets how large one can blow up on its way out: too
 * near and a shard fills the frame the moment it lifts, too far and the tumble
 * flattens into a slide.
 */
#define GLASS_FOCAL_HEIGHTS 1.75f

/* The incoming scene is held down to this behind an assembling break, so a
 * shard that has not landed yet reads as missing rather than as already in
 * place. Every other style shows it at full strength. */
#define GLASS_ASSEMBLE_BG 0.18f

/* The glint crosses the canvas over this much of the transition, starting as
 * the pane begins to crack. */
#define GLINT_TRAVEL 0.75f

enum glass_preset_id {
	GLASS_PRESET_CUSTOM = 0,
	GLASS_PRESET_WINDOW = 1,
	GLASS_PRESET_EXPLOSION = 2,
	GLASS_PRESET_GENTLE = 3,
	GLASS_PRESET_FREEZE = 4,
};

/*
 * A whole look in one row. Picking a preset writes every one of these, the
 * dropdowns included, so a preset is a finished effect rather than a starting
 * point that still needs the break style set to match. Touching anything
 * afterwards puts the list back to Custom - see glass_touched().
 */
struct glass_preset {
	const char *label;
	int style;
	int pattern;
	double shard_size;
	double crack_spread;
	double hold;
	int easing;
	double gravity_angle;
	double gravity;
	double throw_amt;
	double spin;
	double scatter;
	bool flat;
	double crack_width;
	double crack_glow;
	double rim;
	double rim_width;
	double refract;
	double tint;
	double glint;
};

/* clang-format off */

static const struct glass_preset glass_presets[] = {
	/* label                    style                  pattern                 size  crack  hold  easing              angle  grav  throw  spin  scat  flat   cw   glow   rim  rimw  refr  tint  glint */
	{"Glass.Preset.Window",     GLASS_STYLE_WINDOW,    GLASS_PATTERN_WEB,      90.0, 28.0,  12.0, GLASS_EASE_OUT,     90.0,  90.0,  22.0, 0.60, 40.0, false, 1.6, 140.0, 85.0, 7.0,  9.0, 22.0,  60.0},
	{"Glass.Preset.Explosion",  GLASS_STYLE_BLOW,      GLASS_PATTERN_WEB,      70.0, 18.0,   6.0, GLASS_EASE_OUT,     90.0,  25.0, 120.0, 1.40, 75.0, false, 1.8, 200.0, 100.0, 8.0, 12.0, 28.0,  90.0},
	{"Glass.Preset.Gentle",     GLASS_STYLE_WINDOW,    GLASS_PATTERN_VORONOI, 130.0, 40.0,  18.0, GLASS_EASE_IN_OUT,  90.0,  55.0,   6.0, 0.25, 25.0, false, 1.2,  80.0, 60.0, 6.0,  5.0, 14.0,  35.0},
	{"Glass.Preset.Freeze",     GLASS_STYLE_WIPE,      GLASS_PATTERN_ICE,     110.0, 45.0,  10.0, GLASS_EASE_LINEAR,  90.0,  70.0,  15.0, 0.40, 35.0, false, 2.0, 170.0, 70.0, 6.0,  7.0, 18.0,  50.0},
};

/* clang-format on */

#define GLASS_PRESET_COUNT ((int)(sizeof(glass_presets) / sizeof(glass_presets[0])))

struct glass_info {
	obs_source_t *source;
	gs_effect_t *effect;

	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_canvas;
	gs_eparam_t *ep_impact_px;
	gs_eparam_t *ep_reach;
	gs_eparam_t *ep_progress;
	gs_eparam_t *ep_hold_end;
	gs_eparam_t *ep_crack_front;
	gs_eparam_t *ep_style;
	gs_eparam_t *ep_flat;
	gs_eparam_t *ep_arriving;
	gs_eparam_t *ep_use_b;
	gs_eparam_t *ep_gravity_dir;
	gs_eparam_t *ep_gravity;
	gs_eparam_t *ep_throw;
	gs_eparam_t *ep_spin;
	gs_eparam_t *ep_scatter;
	gs_eparam_t *ep_focal;
	gs_eparam_t *ep_crack_color;
	gs_eparam_t *ep_crack_px;
	gs_eparam_t *ep_crack_glow;
	gs_eparam_t *ep_rim_amount;
	gs_eparam_t *ep_rim_px;
	gs_eparam_t *ep_refract_px;
	gs_eparam_t *ep_tint;
	gs_eparam_t *ep_glint;
	gs_eparam_t *ep_glint_pos;
	gs_eparam_t *ep_glint_dir;
	gs_eparam_t *ep_bg_level;

	/* settings */
	int style;
	int pattern;
	int easing;
	float shard_px;
	uint32_t seed;
	float impact_x; /* 0..1 of the canvas */
	float impact_y;
	float crack_end;
	float hold_end;
	float gravity_x;
	float gravity_y;
	float gravity;
	float throw_amt;
	float spin;
	float scatter;
	bool flat;
	float crack_px;
	float crack_glow;
	float rim_amount;
	float rim_px;
	float refract_px;
	float tint;
	float glint;

	struct vec4 crack_color;
	struct vec4 crack_color_srgb;

	/* mesh */
	struct glass_field field;
	gs_vertbuffer_t *vb;
	gs_indexbuffer_t *ib;
	uint32_t *base_indices; /* the emitted order, kept to re-sort from */
	struct glass_span *spans;
	uint32_t *order;
	struct glass_sortkey *keys;
	size_t index_count;
	bool have_mesh;
	bool warned; /* the mesh failed and has already been complained about */
};

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static const char *glass_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Glass.Transition");
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */

static void glass_update(void *data, obs_data_t *settings)
{
	struct glass_info *glass = data;
	uint32_t color;
	float angle;
	float hold;

	glass->style = (int)obs_data_get_int(settings, S_STYLE);
	glass->pattern = (int)obs_data_get_int(settings, S_PATTERN);
	glass->easing = (int)obs_data_get_int(settings, S_EASING);
	glass->shard_px = (float)obs_data_get_double(settings, S_SHARD_SIZE);
	glass->seed = (uint32_t)obs_data_get_int(settings, S_SEED);

	glass->impact_x = (float)obs_data_get_double(settings, S_IMPACT_X) / 100.0f;
	glass->impact_y = (float)obs_data_get_double(settings, S_IMPACT_Y) / 100.0f;

	/*
	 * The crack phase and the hold are both given as shares of the
	 * transition, and the flight gets whatever is left. Clamping the hold
	 * rather than the crack keeps the setting the user reached for - the
	 * spread - and gives way on the one they did not.
	 */
	glass->crack_end = clampf((float)obs_data_get_double(settings, S_CRACK_SPREAD) / 100.0f, 0.02f,
				  GLASS_HOLD_CEILING - GLASS_HOLD_GAP);
	hold = (float)obs_data_get_double(settings, S_HOLD) / 100.0f;
	glass->hold_end = clampf(glass->crack_end + hold, glass->crack_end + GLASS_HOLD_GAP, GLASS_HOLD_CEILING);

	angle = (float)obs_data_get_double(settings, S_GRAVITY_ANGLE) * (float)M_PI / 180.0f;
	glass->gravity_x = cosf(angle);
	glass->gravity_y = sinf(angle);

	glass->gravity = (float)obs_data_get_double(settings, S_GRAVITY) / 100.0f;
	glass->throw_amt = (float)obs_data_get_double(settings, S_THROW) / 100.0f;
	glass->spin = (float)obs_data_get_double(settings, S_SPIN);
	glass->scatter = (float)obs_data_get_double(settings, S_SCATTER) / 100.0f;
	glass->flat = obs_data_get_bool(settings, S_FLAT);

	glass->crack_px = (float)obs_data_get_double(settings, S_CRACK_WIDTH);
	glass->crack_glow = (float)obs_data_get_double(settings, S_CRACK_GLOW) / 100.0f;
	glass->rim_amount = (float)obs_data_get_double(settings, S_RIM) / 100.0f;
	glass->rim_px = (float)obs_data_get_double(settings, S_RIM_WIDTH);
	glass->refract_px = (float)obs_data_get_double(settings, S_REFRACT);
	glass->tint = (float)obs_data_get_double(settings, S_TINT) / 100.0f;
	glass->glint = (float)obs_data_get_double(settings, S_GLINT) / 100.0f;

	color = (uint32_t)obs_data_get_int(settings, S_CRACK_COLOR) | 0xFF000000;
	vec4_from_rgba(&glass->crack_color, color);
	vec4_from_rgba_srgb(&glass->crack_color_srgb, color);
}

static void glass_defaults(obs_data_t *settings)
{
	const struct glass_preset *p = &glass_presets[0]; /* Window Break */

	obs_data_set_default_int(settings, S_PRESET, GLASS_PRESET_WINDOW);
	obs_data_set_default_int(settings, S_STYLE, p->style);
	obs_data_set_default_int(settings, S_PATTERN, p->pattern);
	obs_data_set_default_double(settings, S_SHARD_SIZE, p->shard_size);
	obs_data_set_default_int(settings, S_SEED, 1);
	obs_data_set_default_double(settings, S_IMPACT_X, 50.0);
	obs_data_set_default_double(settings, S_IMPACT_Y, 50.0);
	obs_data_set_default_double(settings, S_CRACK_SPREAD, p->crack_spread);
	obs_data_set_default_double(settings, S_HOLD, p->hold);
	obs_data_set_default_int(settings, S_EASING, p->easing);
	obs_data_set_default_double(settings, S_GRAVITY_ANGLE, p->gravity_angle);
	obs_data_set_default_double(settings, S_GRAVITY, p->gravity);
	obs_data_set_default_double(settings, S_THROW, p->throw_amt);
	obs_data_set_default_double(settings, S_SPIN, p->spin);
	obs_data_set_default_double(settings, S_SCATTER, p->scatter);
	obs_data_set_default_bool(settings, S_FLAT, p->flat);

	/* obs colours are 0xAABBGGRR: #DCEBFF, the cold white a cracked edge
	 * picks up rather than a pure one */
	obs_data_set_default_int(settings, S_CRACK_COLOR, 0xFFFFEBDC);

	obs_data_set_default_double(settings, S_CRACK_WIDTH, p->crack_width);
	obs_data_set_default_double(settings, S_CRACK_GLOW, p->crack_glow);
	obs_data_set_default_double(settings, S_RIM, p->rim);
	obs_data_set_default_double(settings, S_RIM_WIDTH, p->rim_width);
	obs_data_set_default_double(settings, S_REFRACT, p->refract);
	obs_data_set_default_double(settings, S_TINT, p->tint);
	obs_data_set_default_double(settings, S_GLINT, p->glint);
}

static bool near_enough(double a, double b)
{
	return fabs(a - b) < 0.001;
}

static bool preset_matches(const struct glass_preset *p, obs_data_t *s)
{
	return (int)obs_data_get_int(s, S_STYLE) == p->style && (int)obs_data_get_int(s, S_PATTERN) == p->pattern &&
	       (int)obs_data_get_int(s, S_EASING) == p->easing && obs_data_get_bool(s, S_FLAT) == p->flat &&
	       near_enough(obs_data_get_double(s, S_SHARD_SIZE), p->shard_size) &&
	       near_enough(obs_data_get_double(s, S_CRACK_SPREAD), p->crack_spread) &&
	       near_enough(obs_data_get_double(s, S_HOLD), p->hold) &&
	       near_enough(obs_data_get_double(s, S_GRAVITY_ANGLE), p->gravity_angle) &&
	       near_enough(obs_data_get_double(s, S_GRAVITY), p->gravity) &&
	       near_enough(obs_data_get_double(s, S_THROW), p->throw_amt) &&
	       near_enough(obs_data_get_double(s, S_SPIN), p->spin) &&
	       near_enough(obs_data_get_double(s, S_SCATTER), p->scatter) &&
	       near_enough(obs_data_get_double(s, S_CRACK_WIDTH), p->crack_width) &&
	       near_enough(obs_data_get_double(s, S_CRACK_GLOW), p->crack_glow) &&
	       near_enough(obs_data_get_double(s, S_RIM), p->rim) &&
	       near_enough(obs_data_get_double(s, S_RIM_WIDTH), p->rim_width) &&
	       near_enough(obs_data_get_double(s, S_REFRACT), p->refract) &&
	       near_enough(obs_data_get_double(s, S_TINT), p->tint) &&
	       near_enough(obs_data_get_double(s, S_GLINT), p->glint);
}

static void preset_apply(const struct glass_preset *p, obs_data_t *s)
{
	obs_data_set_int(s, S_STYLE, p->style);
	obs_data_set_int(s, S_PATTERN, p->pattern);
	obs_data_set_int(s, S_EASING, p->easing);
	obs_data_set_bool(s, S_FLAT, p->flat);
	obs_data_set_double(s, S_SHARD_SIZE, p->shard_size);
	obs_data_set_double(s, S_CRACK_SPREAD, p->crack_spread);
	obs_data_set_double(s, S_HOLD, p->hold);
	obs_data_set_double(s, S_GRAVITY_ANGLE, p->gravity_angle);
	obs_data_set_double(s, S_GRAVITY, p->gravity);
	obs_data_set_double(s, S_THROW, p->throw_amt);
	obs_data_set_double(s, S_SPIN, p->spin);
	obs_data_set_double(s, S_SCATTER, p->scatter);
	obs_data_set_double(s, S_CRACK_WIDTH, p->crack_width);
	obs_data_set_double(s, S_CRACK_GLOW, p->crack_glow);
	obs_data_set_double(s, S_RIM, p->rim);
	obs_data_set_double(s, S_RIM_WIDTH, p->rim_width);
	obs_data_set_double(s, S_REFRACT, p->refract);
	obs_data_set_double(s, S_TINT, p->tint);
	obs_data_set_double(s, S_GLINT, p->glint);
}

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static void glass_layout(obs_properties_t *props, obs_data_t *settings)
{
	bool moves = (int)obs_data_get_int(settings, S_STYLE) != GLASS_STYLE_WIPE;

	/* Nothing flies in the wipe, so the ballistics would be controls with
	 * nothing on the end of them. The crack and the glass look still
	 * apply - that style is all crack. */
	obs_property_set_visible(obs_properties_get(props, S_GRAVITY_ANGLE), moves);
	obs_property_set_visible(obs_properties_get(props, S_GRAVITY), moves);
	obs_property_set_visible(obs_properties_get(props, S_THROW), moves);
	obs_property_set_visible(obs_properties_get(props, S_SPIN), moves);
	obs_property_set_visible(obs_properties_get(props, S_FLAT), moves);
}

static bool glass_preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= GLASS_PRESET_WINDOW && id <= GLASS_PRESET_COUNT)
		preset_apply(&glass_presets[id - 1], settings);

	glass_layout(props, settings);
	return true;
}

/*
 * Every control but the preset list runs through here.
 *
 * A preset that stayed selected while its values were edited out from under it
 * would be claiming a look the transition no longer has, so the first change
 * that takes the settings away from the preset drops the list back to Custom.
 * Changes that land back on the preset exactly leave it alone.
 */
static bool glass_touched(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int id = (int)obs_data_get_int(settings, S_PRESET);

	UNUSED_PARAMETER(prop);

	if (id >= GLASS_PRESET_WINDOW && id <= GLASS_PRESET_COUNT && !preset_matches(&glass_presets[id - 1], settings))
		obs_data_set_int(settings, S_PRESET, GLASS_PRESET_CUSTOM);

	glass_layout(props, settings);
	return true;
}

/* Adds the modified callback that watches for the preset being edited away
 * from, so no control can be added without being wired up to it. */
static obs_property_t *watched(obs_properties_t *props, obs_property_t *prop)
{
	UNUSED_PARAMETER(props);
	obs_property_set_modified_callback(prop, glass_touched);
	return prop;
}

static obs_properties_t *glass_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	int i;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_list(props, S_PRESET, T_("Glass.Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Glass.Preset.Custom"), GLASS_PRESET_CUSTOM);
	for (i = 0; i < GLASS_PRESET_COUNT; i++)
		obs_property_list_add_int(p, T_(glass_presets[i].label), i + 1);
	obs_property_set_long_description(p, T_("Glass.Preset.Description"));
	obs_property_set_modified_callback(p, glass_preset_modified);

	p = obs_properties_add_list(props, S_STYLE, T_("Glass.Style"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Glass.Style.Window"), GLASS_STYLE_WINDOW);
	obs_property_list_add_int(p, T_("Glass.Style.Blow"), GLASS_STYLE_BLOW);
	obs_property_list_add_int(p, T_("Glass.Style.Wipe"), GLASS_STYLE_WIPE);
	obs_property_list_add_int(p, T_("Glass.Style.Assemble"), GLASS_STYLE_ASSEMBLE);
	obs_property_set_long_description(p, T_("Glass.Style.Description"));
	watched(props, p);

	p = obs_properties_add_list(props, S_PATTERN, T_("Glass.Pattern"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Glass.Pattern.Web"), GLASS_PATTERN_WEB);
	obs_property_list_add_int(p, T_("Glass.Pattern.Voronoi"), GLASS_PATTERN_VORONOI);
	obs_property_list_add_int(p, T_("Glass.Pattern.Ice"), GLASS_PATTERN_ICE);
	obs_property_set_long_description(p, T_("Glass.Pattern.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_SHARD_SIZE, T_("Glass.ShardSize"), 16.0, 400.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, T_("Glass.ShardSize.Description"));
	watched(props, p);

	p = obs_properties_add_int_slider(props, S_SEED, T_("Glass.Seed"), 1, 9999, 1);
	obs_property_set_long_description(p, T_("Glass.Seed.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_IMPACT_X, T_("Glass.ImpactX"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_IMPACT_Y, T_("Glass.ImpactY"), -50.0, 150.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_CRACK_SPREAD, T_("Glass.CrackSpread"), 2.0, 80.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.CrackSpread.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_HOLD, T_("Glass.Hold"), 0.0, 50.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Hold.Description"));
	watched(props, p);

	p = obs_properties_add_list(props, S_EASING, T_("Glass.Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Glass.Easing.Linear"), GLASS_EASE_LINEAR);
	obs_property_list_add_int(p, T_("Glass.Easing.InOut"), GLASS_EASE_IN_OUT);
	obs_property_list_add_int(p, T_("Glass.Easing.Out"), GLASS_EASE_OUT);
	obs_property_set_long_description(p, T_("Glass.Easing.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_GRAVITY_ANGLE, T_("Glass.GravityAngle"), 0.0, 359.0, 1.0);
	obs_property_float_set_suffix(p, "°");
	obs_property_set_long_description(p, T_("Glass.GravityAngle.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_GRAVITY, T_("Glass.Gravity"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_THROW, T_("Glass.Throw"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Throw.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_SPIN, T_("Glass.Spin"), 0.0, 3.0, 0.05);
	obs_property_set_long_description(p, T_("Glass.Spin.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_SCATTER, T_("Glass.Scatter"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Scatter.Description"));
	watched(props, p);

	p = obs_properties_add_bool(props, S_FLAT, T_("Glass.Flat"));
	obs_property_set_long_description(p, T_("Glass.Flat.Description"));
	watched(props, p);

	p = obs_properties_add_color(props, S_CRACK_COLOR, T_("Glass.CrackColor"));
	obs_property_set_long_description(p, T_("Glass.CrackColor.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_CRACK_WIDTH, T_("Glass.CrackWidth"), 0.0, 8.0, 0.1);
	obs_property_float_set_suffix(p, " px");
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_CRACK_GLOW, T_("Glass.CrackGlow"), 0.0, 300.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.CrackGlow.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_RIM, T_("Glass.Rim"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Rim.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_RIM_WIDTH, T_("Glass.RimWidth"), 0.0, 30.0, 0.5);
	obs_property_float_set_suffix(p, " px");
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_REFRACT, T_("Glass.Refract"), 0.0, 40.0, 0.5);
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, T_("Glass.Refract.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_TINT, T_("Glass.Tint"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Tint.Description"));
	watched(props, p);

	p = obs_properties_add_float_slider(props, S_GLINT, T_("Glass.Glint"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Glass.Glint.Description"));
	watched(props, p);

	return props;
}

/* ------------------------------------------------------------------ */
/* the mesh                                                           */
/* ------------------------------------------------------------------ */

/* Graphics context required. */
static void glass_mesh_free(struct glass_info *glass)
{
	if (glass->vb) {
		gs_vertexbuffer_destroy(glass->vb);
		glass->vb = NULL;
	}
	if (glass->ib) {
		gs_indexbuffer_destroy(glass->ib);
		glass->ib = NULL;
	}

	bfree(glass->base_indices);
	bfree(glass->spans);
	bfree(glass->order);
	bfree(glass->keys);
	glass->base_indices = NULL;
	glass->spans = NULL;
	glass->order = NULL;
	glass->keys = NULL;

	glass_field_free(&glass->field);
	glass->index_count = 0;
	glass->have_mesh = false;
}

/*
 * Builds the break for this canvas and uploads it.
 *
 * This is the expensive call in the whole transition - a few thousand cells,
 * each clipped against its neighbours - and it runs on the graphics thread,
 * which is why it is guarded by glass_field_stale() and not by a frame counter.
 * Cutting the pane depends on the canvas size, the impact, the shard size, the
 * pattern and the seed, and on nothing else; every other setting is a uniform,
 * so dragging the gravity or the rim light about never rebuilds anything.
 */
static void glass_mesh_build(struct glass_info *glass, uint32_t cx, uint32_t cy)
{
	struct glass_build build;
	struct glass_vertex *verts;
	struct gs_vb_data *vbd;
	uint32_t *indices;
	size_t nverts;
	size_t nindices;
	size_t i;

	build.canvas_cx = (float)cx;
	build.canvas_cy = (float)cy;
	build.impact_x = glass->impact_x * (float)cx;
	build.impact_y = glass->impact_y * (float)cy;
	build.shard_px = glass->shard_px;
	build.pattern = glass->pattern;
	build.seed = glass->seed;

	if (glass->have_mesh && !glass_field_stale(&glass->field, &build))
		return;

	glass_mesh_free(glass);

	if (!glass_field_build(&glass->field, &build)) {
		/* Once only: the callback runs every frame, and a break that
		 * could not be allocated will not allocate on the next one
		 * either. */
		if (!glass->warned) {
			obs_log(LOG_ERROR, "could not break a %ux%u canvas into shards", cx, cy);
			glass->warned = true;
		}
		return;
	}

	nverts = glass_vertex_count(&glass->field);
	nindices = glass_index_count(&glass->field);

	verts = bmalloc(nverts * sizeof(*verts));
	glass->base_indices = bmalloc(nindices * sizeof(*glass->base_indices));
	glass->spans = bmalloc((size_t)glass->field.count * sizeof(*glass->spans));
	glass->order = bmalloc((size_t)glass->field.count * sizeof(*glass->order));
	glass->keys = bmalloc((size_t)glass->field.count * sizeof(*glass->keys));

	glass_emit(&glass->field, verts, glass->base_indices, glass->spans);

	/*
	 * Scattered into the layout the vertex shader declares. The position is
	 * in canvas pixels, like the sprite the other two transitions draw, so
	 * the projection OBS has already set up still applies.
	 */
	vbd = gs_vbdata_create();
	vbd->num = nverts;
	vbd->points = bmalloc(nverts * sizeof(struct vec3));
	vbd->num_tex = 3;
	vbd->tvarray = bmalloc(3 * sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 2;
	vbd->tvarray[0].array = bmalloc(nverts * 2 * sizeof(float));
	vbd->tvarray[1].width = 4;
	vbd->tvarray[1].array = bmalloc(nverts * 4 * sizeof(float));
	vbd->tvarray[2].width = 4;
	vbd->tvarray[2].array = bmalloc(nverts * 4 * sizeof(float));

	for (i = 0; i < nverts; i++) {
		float *uv = (float *)vbd->tvarray[0].array + i * 2;
		float *shard = (float *)vbd->tvarray[1].array + i * 4;
		float *rnd = (float *)vbd->tvarray[2].array + i * 4;

		vec3_set(&vbd->points[i], verts[i].x, verts[i].y, 0.0f);

		uv[0] = verts[i].u;
		uv[1] = verts[i].v;

		shard[0] = verts[i].cx;
		shard[1] = verts[i].cy;
		shard[2] = verts[i].core;
		shard[3] = verts[i].launch;

		rnd[0] = verts[i].rnd[0];
		rnd[1] = verts[i].rnd[1];
		rnd[2] = verts[i].rnd[2];
		rnd[3] = verts[i].inner;
	}

	bfree(verts);

	/* The index buffer is rewritten every frame to order the shards back to
	 * front, so it is dynamic and owns its own copy. */
	indices = bmalloc(nindices * sizeof(*indices));
	memcpy(indices, glass->base_indices, nindices * sizeof(*indices));

	glass->vb = gs_vertexbuffer_create(vbd, 0);
	glass->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, nindices, GS_DYNAMIC);
	glass->index_count = nindices;

	if (!glass->vb || !glass->ib) {
		if (!glass->warned) {
			obs_log(LOG_ERROR, "could not upload the shard mesh");
			glass->warned = true;
		}
		glass_mesh_free(glass);
		return;
	}

	glass->have_mesh = true;
	glass->warned = false;
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */

static void *glass_create(obs_data_t *settings, obs_source_t *source)
{
	struct glass_info *glass;
	gs_effect_t *effect;
	char *file = obs_module_file("effects/glass_transition.effect");

	if (!file) {
		obs_log(LOG_ERROR, "effects/glass_transition.effect is missing");
		return NULL;
	}

	obs_enter_graphics();
	effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();

	bfree(file);

	if (!effect) {
		obs_log(LOG_ERROR, "failed to compile effects/glass_transition.effect");
		return NULL;
	}

	glass = bzalloc(sizeof(struct glass_info));
	glass->source = source;
	glass->effect = effect;

	glass->ep_a_tex = gs_effect_get_param_by_name(effect, "a_tex");
	glass->ep_b_tex = gs_effect_get_param_by_name(effect, "b_tex");
	glass->ep_canvas = gs_effect_get_param_by_name(effect, "canvas");
	glass->ep_impact_px = gs_effect_get_param_by_name(effect, "impact_px");
	glass->ep_reach = gs_effect_get_param_by_name(effect, "reach");
	glass->ep_progress = gs_effect_get_param_by_name(effect, "progress");
	glass->ep_hold_end = gs_effect_get_param_by_name(effect, "hold_end");
	glass->ep_crack_front = gs_effect_get_param_by_name(effect, "crack_front");
	glass->ep_style = gs_effect_get_param_by_name(effect, "style");
	glass->ep_flat = gs_effect_get_param_by_name(effect, "flat_shards");
	glass->ep_arriving = gs_effect_get_param_by_name(effect, "arriving");
	glass->ep_use_b = gs_effect_get_param_by_name(effect, "use_b");
	glass->ep_gravity_dir = gs_effect_get_param_by_name(effect, "gravity_dir");
	glass->ep_gravity = gs_effect_get_param_by_name(effect, "gravity");
	glass->ep_throw = gs_effect_get_param_by_name(effect, "throw_amt");
	glass->ep_spin = gs_effect_get_param_by_name(effect, "spin");
	glass->ep_scatter = gs_effect_get_param_by_name(effect, "scatter");
	glass->ep_focal = gs_effect_get_param_by_name(effect, "focal");
	glass->ep_crack_color = gs_effect_get_param_by_name(effect, "crack_color");
	glass->ep_crack_px = gs_effect_get_param_by_name(effect, "crack_px");
	glass->ep_crack_glow = gs_effect_get_param_by_name(effect, "crack_glow");
	glass->ep_rim_amount = gs_effect_get_param_by_name(effect, "rim_amount");
	glass->ep_rim_px = gs_effect_get_param_by_name(effect, "rim_px");
	glass->ep_refract_px = gs_effect_get_param_by_name(effect, "refract_px");
	glass->ep_tint = gs_effect_get_param_by_name(effect, "tint_amount");
	glass->ep_glint = gs_effect_get_param_by_name(effect, "glint_amount");
	glass->ep_glint_pos = gs_effect_get_param_by_name(effect, "glint_pos");
	glass->ep_glint_dir = gs_effect_get_param_by_name(effect, "glint_dir");
	glass->ep_bg_level = gs_effect_get_param_by_name(effect, "bg_level");

	/* Applied directly for the same reason the other two transitions do it:
	 * the source's data pointer is only assigned once create returns, so an
	 * update queued here would not reach us until a later tick. */
	glass_update(glass, settings);

	return glass;
}

static void glass_destroy(void *data)
{
	struct glass_info *glass = data;

	obs_enter_graphics();
	glass_mesh_free(glass);
	gs_effect_destroy(glass->effect);
	obs_leave_graphics();

	bfree(glass);
}

/* Everything about the frame that both the background and the shard passes
 * need, worked out once in glass_callback() and handed to each of them. */
struct glass_frame {
	gs_texture_t *a;
	gs_texture_t *b;
	struct vec2 canvas;
	struct vec2 impact;
	struct vec2 gravity_dir;
	struct vec2 glint_dir;
	float progress;
	float crack_front;
	float glint_pos;
	float focal;
	float bg_level;
	bool nonlinear;
};

/*
 * Uploads every uniform the effect declares.
 *
 * This is called again before each technique rather than once for the frame,
 * because ending a technique returns every parameter in the effect to its
 * default - and these have no defaults, so what the next technique would find
 * is nothing. The other two transitions never notice: they run one technique
 * per frame. This one draws the incoming scene and then the mesh, and the
 * assemble style draws the mesh twice, so it notices every time.
 */
static void glass_set_uniforms(struct glass_info *glass, const struct glass_frame *f, bool arriving, bool use_b)
{
	if (f->nonlinear) {
		gs_effect_set_texture(glass->ep_a_tex, f->a);
		gs_effect_set_texture(glass->ep_b_tex, f->b);
		gs_effect_set_vec4(glass->ep_crack_color, &glass->crack_color);
	} else {
		gs_effect_set_texture_srgb(glass->ep_a_tex, f->a);
		gs_effect_set_texture_srgb(glass->ep_b_tex, f->b);
		gs_effect_set_vec4(glass->ep_crack_color, &glass->crack_color_srgb);
	}

	gs_effect_set_vec2(glass->ep_canvas, &f->canvas);
	gs_effect_set_vec2(glass->ep_impact_px, &f->impact);
	gs_effect_set_float(glass->ep_reach, glass->field.reach);

	gs_effect_set_float(glass->ep_progress, f->progress);
	gs_effect_set_float(glass->ep_hold_end, glass->hold_end);
	gs_effect_set_float(glass->ep_crack_front, f->crack_front);

	gs_effect_set_int(glass->ep_style, glass->style);
	gs_effect_set_bool(glass->ep_flat, glass->flat);
	gs_effect_set_bool(glass->ep_arriving, arriving);
	gs_effect_set_bool(glass->ep_use_b, use_b);

	gs_effect_set_vec2(glass->ep_gravity_dir, &f->gravity_dir);
	gs_effect_set_float(glass->ep_gravity, glass->gravity);
	gs_effect_set_float(glass->ep_throw, glass->throw_amt);
	gs_effect_set_float(glass->ep_spin, glass->spin);
	gs_effect_set_float(glass->ep_scatter, glass->scatter);
	gs_effect_set_float(glass->ep_focal, f->focal);

	gs_effect_set_float(glass->ep_crack_px, glass->crack_px);
	gs_effect_set_float(glass->ep_crack_glow, glass->crack_glow);
	gs_effect_set_float(glass->ep_rim_amount, glass->rim_amount);
	gs_effect_set_float(glass->ep_rim_px, glass->rim_px);
	gs_effect_set_float(glass->ep_refract_px, glass->refract_px);
	gs_effect_set_float(glass->ep_tint, glass->tint);
	gs_effect_set_float(glass->ep_glint, glass->glint);
	gs_effect_set_float(glass->ep_glint_pos, f->glint_pos);
	gs_effect_set_vec2(glass->ep_glint_dir, &f->glint_dir);
	gs_effect_set_float(glass->ep_bg_level, f->bg_level);
}

/*
 * The draw order for one pass.
 *
 * There is no depth buffer here - the transition renders into whatever target
 * OBS gives it - so overlapping shards are resolved by drawing them back to
 * front. A shard is a flat plane and flat planes that never intersect sort
 * exactly, which falling glass does not violate.
 *
 * With the tumble off, or in the wipe where nothing moves, every shard sits at
 * the same depth and the emitted order is already correct, so the sort and the
 * upload that goes with it are both skipped.
 */
static void glass_order_pass(struct glass_info *glass, const struct glass_motion *motion, bool depth_varies)
{
	uint32_t *dst;
	size_t at = 0;
	int i;

	if (!depth_varies)
		return;

	dst = gs_indexbuffer_get_data(glass->ib);
	if (!dst)
		return;

	glass_sort_order(&glass->field, motion, glass->order, glass->keys);

	for (i = 0; i < glass->field.count; i++) {
		const struct glass_span *span = &glass->spans[glass->order[i]];

		memcpy(dst + at, glass->base_indices + span->first, span->count * sizeof(uint32_t));
		at += span->count;
	}

	gs_indexbuffer_flush(glass->ib);
}

static void glass_draw_pass(struct glass_info *glass, const struct glass_frame *frame,
			    const struct glass_motion *motion, bool arriving, bool use_b, bool depth_varies)
{
	glass_order_pass(glass, motion, depth_varies);
	glass_set_uniforms(glass, frame, arriving, use_b);

	while (gs_effect_loop(glass->effect, "Glass")) {
		gs_load_vertexbuffer(glass->vb);
		gs_load_indexbuffer(glass->ib);
		gs_draw(GS_TRIS, 0, 0);
	}
}

static void glass_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct glass_info *glass = data;
	struct glass_motion motion;
	struct glass_frame frame;
	bool assemble = glass->style == GLASS_STYLE_ASSEMBLE;
	bool depth_varies = !glass->flat && glass->style != GLASS_STYLE_WIPE;
	bool previous_srgb = gs_framebuffer_srgb_enabled();
	enum gs_cull_mode previous_cull = gs_get_cull_mode();

	glass_mesh_build(glass, cx, cy);
	if (!glass->have_mesh)
		return;

	memset(&frame, 0, sizeof(frame));
	frame.a = a;
	frame.b = b;
	frame.progress = t;
	frame.nonlinear = gs_get_color_space() == GS_CS_SRGB;
	frame.focal = (float)cy * GLASS_FOCAL_HEIGHTS;
	frame.bg_level = assemble ? GLASS_ASSEMBLE_BG : 1.0f;

	vec2_set(&frame.canvas, (float)cx, (float)cy);
	vec2_set(&frame.impact, glass->impact_x * (float)cx, glass->impact_y * (float)cy);
	vec2_set(&frame.gravity_dir, glass->gravity_x, glass->gravity_y);
	vec2_set(&frame.glint_dir, 0.7071068f, 0.7071068f);

	frame.crack_front = glass_crack_front(t, glass->crack_end, glass->easing);

	/*
	 * Where the highlight has got to. It is a function of the transition
	 * clock alone, so it is worked out once here rather than per pixel -
	 * the same split the vortex transition uses for its phase curves. It
	 * starts off the edge of the canvas so the sweep enters rather than
	 * appearing.
	 */
	frame.glint_pos = -0.2f + 1.4f * clampf(t / GLINT_TRAVEL, 0.0f, 1.0f);

	memset(&motion, 0, sizeof(motion));
	motion.style = glass->style;
	motion.progress = t;
	motion.crack_front = frame.crack_front;
	motion.hold_end = glass->hold_end;
	motion.gravity_x = glass->gravity_x;
	motion.gravity_y = glass->gravity_y;
	motion.gravity = glass->gravity;
	motion.throw_px = glass->throw_amt;
	motion.spin = glass->spin;
	motion.scatter = glass->scatter;
	motion.reach = glass->field.reach;
	motion.impact_x = frame.impact.x;
	motion.impact_y = frame.impact.y;
	motion.flat = glass->flat;

	gs_enable_framebuffer_srgb(!frame.nonlinear);

	gs_blend_state_push();
	gs_set_cull_mode(GS_NEITHER); /* a shard that turns over shows its back */
	gs_enable_depth_test(false);

	/* The incoming scene, behind everything. Opaque: it is the floor the
	 * shards are falling off, and the first thing in the target. */
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	glass_set_uniforms(glass, &frame, false, false);
	while (gs_effect_loop(glass->effect, "Background"))
		gs_draw_sprite(NULL, 0, cx, cy);

	/* Shards fade rather than pop as they finish, so they need blending. */
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	if (assemble) {
		/*
		 * Two passes that cross in the middle: the incoming scene flies
		 * in as shards while the outgoing one is still leaving. The
		 * arriving pass runs its flight backwards, so it starts
		 * scattered and lands exactly on the final frame - which is why
		 * the last frame is the incoming scene whole, even though it is
		 * a mesh rather than the plain sprite behind it.
		 */
		motion.arriving = true;
		glass_draw_pass(glass, &frame, &motion, true, true, depth_varies);

		motion.arriving = false;
		glass_draw_pass(glass, &frame, &motion, false, false, depth_varies);
	} else {
		glass_draw_pass(glass, &frame, &motion, false, false, depth_varies);
	}

	gs_load_indexbuffer(NULL);
	gs_load_vertexbuffer(NULL);

	gs_set_cull_mode(previous_cull);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

static void glass_video_render(void *data, gs_effect_t *effect)
{
	struct glass_info *glass = data;

	UNUSED_PARAMETER(effect);
	obs_transition_video_render(glass->source, glass_callback);
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

static bool glass_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
			       size_t channels, size_t sample_rate)
{
	struct glass_info *glass = data;

	return obs_transition_audio_render(glass->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static enum gs_color_space glass_video_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	struct glass_info *glass = data;

	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return obs_transition_video_get_color_space(glass->source);
}

struct obs_source_info glass_transition = {
	.id = "voidscape_glass_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = glass_get_name,
	.create = glass_create,
	.destroy = glass_destroy,
	.update = glass_update,
	.video_render = glass_video_render,
	.audio_render = glass_audio_render,
	.get_properties = glass_properties,
	.get_defaults = glass_defaults,
	.video_get_color_space = glass_video_get_color_space,
};
