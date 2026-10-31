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
 * Loads the built plugin into libobs and drives every transition the way OBS
 * does: created as a source, given two scenes, set through its properties and
 * rendered through obs_source_video_render(). The other GPU checks render each
 * effect with uniforms they work out themselves; this one runs the plugin's
 * own code between the settings and the shader, so it is what catches a
 * uniform the effect does not declare, a preset whose values do not round trip
 * through the properties, or a mask image that never arrives.
 *
 * The outgoing scene is red and the incoming one blue. Every transition has to
 * be the outgoing scene on its first frame, the incoming one on its last, and
 * something else in between, at its defaults and with each of its presets.
 *
 * Needs a graphics device:
 *
 *     xvfb-run -a ./plugin-check ./tiles.so ../data
 */

#include <obs.h>
#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef GPU_CHECK_GRAPHICS_MODULE
#define GPU_CHECK_GRAPHICS_MODULE "libobs-opengl.so.1"
#endif

#define CW 320
#define CH 180

/* How long to give the video thread to tick the transition, which is when a
 * manual time or a settings change is picked up. Many frames at 30 fps. */
#define TICK_WAIT_MS 150

static uint8_t out[CH][CW][4];
static int failures, checks;

static void check(bool ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;
	failures++;
	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static double share_of(int r, int g, int b)
{
	long hit = 0;

	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++) {
			int dr = out[y][x][0] - r, dg = out[y][x][1] - g, db = out[y][x][2] - b;

			if (dr > -10 && dr < 10 && dg > -10 && dg < 10 && db > -10 && db < 10)
				hit++;
		}
	return (double)hit / (double)(CW * CH);
}

/* ------------------------------------------------------------------ */
/* a solid colour scene                                               */
/* ------------------------------------------------------------------ */

struct color_scene {
	struct vec4 color;
};

static const char *color_scene_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "plugin-check colour";
}

static void *color_scene_create(obs_data_t *settings, obs_source_t *source)
{
	struct color_scene *scene = bzalloc(sizeof(*scene));

	UNUSED_PARAMETER(source);
	vec4_from_rgba(&scene->color, (uint32_t)obs_data_get_int(settings, "color"));
	return scene;
}

static void color_scene_destroy(void *data)
{
	bfree(data);
}

static uint32_t color_scene_width(void *data)
{
	UNUSED_PARAMETER(data);
	return CW;
}

static uint32_t color_scene_height(void *data)
{
	UNUSED_PARAMETER(data);
	return CH;
}

static void color_scene_render(void *data, gs_effect_t *effect)
{
	struct color_scene *scene = data;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

	UNUSED_PARAMETER(effect);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &scene->color);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(NULL, 0, CW, CH);
}

static struct obs_source_info color_scene_info = {
	.id = "plugin_check_color",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = color_scene_name,
	.create = color_scene_create,
	.destroy = color_scene_destroy,
	.get_width = color_scene_width,
	.get_height = color_scene_height,
	.video_render = color_scene_render,
};

static obs_source_t *make_scene(const char *name, uint32_t rgba)
{
	obs_data_t *settings = obs_data_create();
	obs_source_t *source;

	obs_data_set_int(settings, "color", rgba);
	source = obs_source_create(color_scene_info.id, name, settings, NULL);
	obs_data_release(settings);
	return source;
}

/* ------------------------------------------------------------------ */
/* driving a transition                                               */
/* ------------------------------------------------------------------ */

static obs_source_t *scene_a, *scene_b;
static gs_texrender_t *texrender;
static gs_stagesurf_t *stage;

static void wait_for_tick(void)
{
	os_sleep_ms(TICK_WAIT_MS);
}

/* Holds the transition at t between the two scenes and reads back the frame. */
static void render_at(obs_source_t *transition, float t)
{
	uint8_t *data;
	uint32_t linesize;

	obs_transition_set_manual_time(transition, t);
	wait_for_tick();

	obs_enter_graphics();
	gs_texrender_reset(texrender);
	if (gs_texrender_begin_with_color_space(texrender, CW, CH, GS_CS_SRGB)) {
		struct vec4 clear;

		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)CW, 0.0f, (float)CH, -100.0f, 100.0f);
		obs_source_video_render(transition);
		gs_texrender_end(texrender);
	}

	gs_stage_texture(stage, gs_texrender_get_texture(texrender));
	if (gs_stagesurface_map(stage, &data, &linesize)) {
		for (int y = 0; y < CH; y++)
			memcpy(out[y], data + (size_t)y * linesize, CW * 4);
		gs_stagesurface_unmap(stage);
	} else {
		memset(out, 0, sizeof(out));
	}
	obs_leave_graphics();
}

/* Starts a manual transition from red to blue, held just off its first frame
 * so that it is running: at exactly 0 OBS ends a manual transition. */
static void begin(obs_source_t *transition)
{
	obs_transition_set(transition, scene_a);
	obs_transition_set_size(transition, CW, CH);
	obs_transition_set_scale_type(transition, OBS_TRANSITION_SCALE_ASPECT);
	obs_transition_start(transition, OBS_TRANSITION_MODE_MANUAL, 0, scene_b);
}

/* The three properties every transition here has to have, whatever it looks
 * like in between. */
static void check_ends(obs_source_t *transition, const char *what)
{
	double red, blue;

	begin(transition);

	render_at(transition, 0.0001f);
	red = share_of(255, 0, 0);
	check(red > 0.99, "%s: first frame is only %.4f outgoing scene", what, red);

	render_at(transition, 0.5f);
	red = share_of(255, 0, 0);
	blue = share_of(0, 0, 255);
	check(red < 0.999 && blue < 0.999, "%s: halfway is still one scene (red %.4f, blue %.4f)", what, red, blue);

	render_at(transition, 0.9999f);
	blue = share_of(0, 0, 255);
	check(blue > 0.99, "%s: last frame is only %.4f incoming scene", what, blue);

	obs_transition_set(transition, scene_a);
}

static obs_source_t *make_transition(const char *id, obs_data_t *settings)
{
	obs_source_t *transition = obs_source_create(id, id, settings, NULL);

	check(transition != NULL, "%s: could not be created", id);
	if (!transition)
		return NULL;

	/* NULL data is what a failed create leaves: the effect did not compile
	 * or declares a uniform under a different name than the plugin sets */
	check(obs_obj_get_data(transition) != NULL, "%s: create failed - see the log above", id);
	if (!obs_obj_get_data(transition)) {
		obs_source_release(transition);
		return NULL;
	}

	return transition;
}

/* ------------------------------------------------------------------ */
/* presets                                                            */
/* ------------------------------------------------------------------ */

struct preset_case {
	const char *id;
	const char *edit_key; /* a numeric setting every preset sets */
};

/*
 * Picks each preset through the properties, as the dropdown does, then runs
 * every other property's modified callback the way the properties view does
 * when it refreshes. A preset that does not round trip - one whose values the
 * list does not recognise as itself once applied - drops back to Custom here.
 * Then an edit has to drop it back to Custom, and the transition has to still
 * render both ends with the preset's look.
 */
static void check_presets(const struct preset_case *pc)
{
	obs_source_t *transition = make_transition(pc->id, NULL);
	obs_properties_t *props;
	obs_property_t *preset;
	size_t count;

	if (!transition)
		return;

	props = obs_source_properties(transition);
	preset = obs_properties_get(props, "preset");
	check(preset != NULL, "%s: no preset list", pc->id);
	if (!preset) {
		obs_properties_destroy(props);
		obs_source_release(transition);
		return;
	}

	count = obs_property_list_item_count(preset);
	check(count > 1, "%s: the preset list has nothing but Custom", pc->id);

	for (size_t i = 0; i < count; i++) {
		long long id = obs_property_list_item_int(preset, i);
		const char *name = obs_property_list_item_name(preset, i);
		obs_data_t *settings = obs_source_get_settings(transition);
		char what[256];

		if (id == 0) {
			obs_data_release(settings);
			continue;
		}

		obs_data_set_int(settings, "preset", id);
		obs_property_modified(preset, settings);

		for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
			if (p != preset)
				obs_property_modified(p, settings);

		check(obs_data_get_int(settings, "preset") == id,
		      "%s: preset %lld (%s) does not recognise itself once applied", pc->id, id, name);

		obs_source_update(transition, settings);
		wait_for_tick();
		snprintf(what, sizeof(what), "%s preset %lld (%s)", pc->id, id, name);
		check_ends(transition, what);

		obs_data_set_double(settings, pc->edit_key, obs_data_get_double(settings, pc->edit_key) + 1.0);
		obs_property_modified(obs_properties_get(props, pc->edit_key), settings);
		check(obs_data_get_int(settings, "preset") == 0, "%s: editing '%s' left preset %lld selected", pc->id,
		      pc->edit_key, id);

		obs_data_release(settings);
	}

	obs_properties_destroy(props);
	obs_source_release(transition);
}

/* ------------------------------------------------------------------ */
/* matte mask images                                                  */
/* ------------------------------------------------------------------ */

/* A 64x64 BMP: a white square on black, which as a silhouette is a square and
 * as a luma wipe changes the middle last. */
static bool write_mask(const char *path)
{
	enum { N = 64 };
	uint8_t header[54] = {'B', 'M'};
	uint32_t row = N * 3, size = 54 + row * N;
	FILE *f = fopen(path, "wb");

	if (!f)
		return false;

	header[2] = (uint8_t)size;
	header[3] = (uint8_t)(size >> 8);
	header[10] = 54;
	header[14] = 40;
	header[18] = N;
	header[22] = N;
	header[26] = 1;
	header[28] = 24;
	fwrite(header, 1, sizeof(header), f);

	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			uint8_t v = (x >= 16 && x < 48 && y >= 16 && y < 48) ? 255 : 0;
			uint8_t px[3] = {v, v, v};

			fwrite(px, 1, 3, f);
		}

	fclose(f);
	return true;
}

static uint8_t circle_frame[CH][CW][4];

/* The mask loads off the graphics thread, so it shows up some frames after the
 * setting does. Waits for the frame to stop being the circle that stands in
 * for a missing image. */
static bool wait_for_mask(obs_source_t *transition)
{
	for (int tries = 0; tries < 40; tries++) {
		render_at(transition, 0.5f);
		if (memcmp(out, circle_frame, sizeof(out)) != 0)
			return true;
	}
	return false;
}

static void check_matte_images(const char *mask_path)
{
	const char *id = "voidscape_matte_transition";
	obs_data_t *settings = obs_data_create();
	obs_source_t *transition;

	obs_data_set_int(settings, "preset", 0);
	obs_data_set_int(settings, "shape", 0); /* circle */
	transition = make_transition(id, settings);
	if (!transition) {
		obs_data_release(settings);
		return;
	}

	begin(transition);
	render_at(transition, 0.5f);
	memcpy(circle_frame, out, sizeof(out));

	/* image silhouette, then luma wipe, each from the same file */
	for (int shape = 16; shape <= 17; shape++) {
		obs_data_set_int(settings, "shape", shape);
		obs_data_set_string(settings, "image", mask_path);
		obs_source_update(transition, settings);
		check(wait_for_mask(transition), "matte shape %d: the mask image never took effect", shape);

		/* and the ends still hold with the image in place */
		check_ends(transition, shape == 16 ? "matte image silhouette" : "matte luma wipe");
		begin(transition);

		obs_data_set_string(settings, "image", "");
		obs_source_update(transition, settings);
		render_at(transition, 0.5f);
		check(memcmp(out, circle_frame, sizeof(out)) == 0,
		      "matte shape %d: clearing the image did not fall back to the circle", shape);
	}

	obs_transition_set(transition, scene_a);
	obs_source_release(transition);

	/*
	 * A transition released while its image is still loading. The load is
	 * started by the deferred update on the next tick, so the release lands
	 * while it is in flight; it must neither crash nor hang.
	 */
	for (int i = 0; i < 8; i++) {
		obs_data_set_int(settings, "shape", 16);
		obs_data_set_string(settings, "image", mask_path);
		transition = obs_source_create(id, "matte release", NULL, NULL);
		obs_source_update(transition, settings);
		os_sleep_ms(i * 5);
		obs_source_release(transition);
	}

	obs_data_release(settings);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	static const char *ids[] = {
		"voidscape_tiles_transition", "voidscape_vortex_transition", "voidscape_glass_transition",
		"voidscape_matte_transition", "voidscape_portal_transition",
	};
	static const struct preset_case presets[] = {
		{"voidscape_glass_transition", "gravity"},
		{"voidscape_matte_transition", "border"},
		{"voidscape_portal_transition", "rim"},
	};
	struct obs_video_info ovi = {0};
	obs_module_t *module = NULL;
	const char *mask_path = "plugin-check-mask.bmp";

	if (argc < 3) {
		fprintf(stderr, "usage: %s <path to the plugin module> <path to its data directory>\n", argv[0]);
		return 2;
	}

	if (!obs_startup("en-US", NULL, NULL))
		return 3;

	ovi.graphics_module = GPU_CHECK_GRAPHICS_MODULE;
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = CW;
	ovi.base_height = CH;
	ovi.output_width = CW;
	ovi.output_height = CH;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 4;

	if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(module)) {
		fprintf(stderr, "could not load the plugin from %s\n", argv[1]);
		return 1;
	}

	obs_register_source(&color_scene_info);
	scene_a = make_scene("outgoing", 0xFF0000FF);
	scene_b = make_scene("incoming", 0xFFFF0000);

	obs_enter_graphics();
	texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	stage = gs_stagesurface_create(CW, CH, GS_RGBA);
	obs_leave_graphics();

	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		obs_source_t *transition = make_transition(ids[i], NULL);
		char what[256];

		if (!transition)
			continue;

		snprintf(what, sizeof(what), "%s at its defaults", ids[i]);
		check_ends(transition, what);
		obs_source_release(transition);
	}

	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
		check_presets(&presets[i]);

	if (write_mask(mask_path))
		check_matte_images(mask_path);
	else
		check(false, "could not write %s", mask_path);

	obs_source_release(scene_a);
	obs_source_release(scene_b);

	obs_enter_graphics();
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	printf("%d plugin checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
