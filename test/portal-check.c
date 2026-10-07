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
 * Renders the portal effect on a real graphics device and checks what lands
 * on screen: the first frame is the outgoing scene and the last the incoming
 * one, pixel for pixel, for portals of every shape, size and position, with
 * every flourish turned up; nothing in between is black; and the portal is
 * really open in the hold, with the incoming scene inside it and the outgoing
 * one outside.
 *
 * Needs a graphics device, so it is not part of the ctest suite:
 *
 *     xvfb-run -a ./portal-check ../data/effects/portal_transition.effect
 *
 * The uniform setup mirrors portal_callback() in src/portal-transition.c, and
 * the schedule is src/portal-math.c itself.
 */
#include <obs.h>
#include <graphics/vec4.h>
#include <graphics/vec2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#include "portal-math.h"

#ifndef GPU_CHECK_GRAPHICS_MODULE
#define GPU_CHECK_GRAPHICS_MODULE "libobs-opengl.so.1"
#endif

#define CW 640
#define CH 360

static uint32_t px_a[CW * CH], px_b[CW * CH];
static uint8_t out[CH][CW][4];
static int failures, checks;

static gs_effect_t *eff;
static gs_texture_t *ta, *tb, *target;
static gs_stagesurf_t *stage;

#define P(n) gs_effect_get_param_by_name(eff, n)

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

struct config {
	float ox, oy; /* percent */
	float aspect;
	float size; /* share of canvas height */
	float open, hold;
	float rim;
	bool spiral;
	float sparks;
	float distort;
	float glow_reach;
	float swirl;
};

/* mirrors portal_callback() */
static void draw(const struct config *c, float t)
{
	struct portal_params pp = {c->aspect, 0.5f * c->size * CH, c->open, c->open + c->hold, c->rim, c->spiral};
	struct portal_frame f;
	struct vec2 canvas, centre;
	struct vec4 rim, core;
	float cover;
	uint8_t *data;
	uint32_t linesize;

	vec2_set(&canvas, CW, CH);
	vec2_set(&centre, c->ox / 100.0f * CW, c->oy / 100.0f * CH);
	cover = portal_cover_radius(&pp, centre.x, centre.y, CW, CH);
	portal_frame_at(&pp, cover, t, &f);

	/* green rim and white core, so the light is easy to tell from either
	 * scene and a black pixel can only be a fault */
	vec4_from_rgba(&rim, 0xFF00FF00);
	vec4_from_rgba(&core, 0xFFFFFFFF);

	gs_set_render_target(target, NULL);
	gs_set_viewport(0, 0, CW, CH);
	gs_ortho(0.0f, CW, 0.0f, CH, -100.0f, 100.0f);
	gs_clear(GS_CLEAR_COLOR, &(struct vec4){{{0, 0, 0, 1}}}, 0.0f, 0);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);
	gs_matrix_identity();

	gs_effect_set_texture(P("a_tex"), ta);
	gs_effect_set_texture(P("b_tex"), tb);
	gs_effect_set_vec2(P("canvas"), &canvas);
	gs_effect_set_vec2(P("centre_px"), &centre);
	gs_effect_set_float(P("aspect"), pp.aspect);
	gs_effect_set_float(P("radius"), f.radius);
	gs_effect_set_float(P("progress"), t);
	gs_effect_set_float(P("rim"), pp.rim_px * f.size_k);
	gs_effect_set_float(P("outer"), c->glow_reach * f.size_k);
	gs_effect_set_float(P("distort"), c->distort * f.size_k);
	gs_effect_set_float(P("energy"), f.energy);
	gs_effect_set_float(P("sparks"), c->sparks * f.spark_env);
	gs_effect_set_float(P("spark_reach"), fmaxf(7.0f * pp.rim_px, 50.0f) * f.size_k);
	gs_effect_set_float(P("swirl"), c->swirl);
	gs_effect_set_float(P("intensity"), 1.0f);
	gs_effect_set_vec4(P("color_rim"), &rim);
	gs_effect_set_vec4(P("color_core"), &core);

	while (gs_effect_loop(eff, "Portal"))
		gs_draw_sprite(NULL, 0, CW, CH);

	gs_set_render_target(NULL, NULL);
	gs_stage_texture(stage, target);
	gs_stagesurface_map(stage, &data, &linesize);
	for (int y = 0; y < CH; y++)
		memcpy(out[y], data + (size_t)y * linesize, CW * 4);
	gs_stagesurface_unmap(stage);
}

int main(int argc, char **argv)
{
	struct obs_video_info ovi = {0};
	const char *effect_path;
	const uint8_t *pa = (const uint8_t *)px_a, *pb = (const uint8_t *)px_b;
	const float times[] = {0.0f, 0.05f, 0.12f, 0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 0.9f, 0.97f, 1.0f};
	char *effect_errors = NULL;

	/* clang-format off */
	const struct config configs[] = {
		/* ox     oy     aspect size  open  hold  rim   spiral sparks distort reach swirl */
		{50.0f,  50.0f,  0.62f, 0.55f, 0.22f, 0.28f, 14.0f, true,  1.0f, 10.0f, 40.0f,  1.0f},
		{50.0f,  50.0f,  1.00f, 0.30f, 0.10f, 0.00f,  4.0f, false, 0.0f,  0.0f,  0.0f,  0.0f},
		{20.0f,  80.0f,  0.40f, 0.80f, 0.30f, 0.40f, 40.0f, true,  3.0f, 40.0f, 150.0f, -2.0f},
		{-40.0f, 130.0f, 0.62f, 0.55f, 0.15f, 0.10f, 14.0f, true,  1.4f, 10.0f, 40.0f,  1.5f},
		{110.0f, -20.0f, 1.80f, 1.20f, 0.05f, 0.70f, 60.0f, false, 2.0f, 60.0f, 200.0f, 4.0f},
	};
	/* clang-format on */

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to portal_transition.effect>\n", argv[0]);
		return 2;
	}
	effect_path = argv[1];

	for (int i = 0; i < CW * CH; i++) {
		px_a[i] = 0xFF0000FF;
		px_b[i] = 0xFFFF0000;
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

	obs_enter_graphics();
	eff = gs_effect_create_from_file(effect_path, &effect_errors);
	if (!eff) {
		fprintf(stderr, "could not compile %s: %s\n", effect_path, effect_errors ? effect_errors : "?");
		return 1;
	}
	ta = gs_texture_create(CW, CH, GS_RGBA, 1, &pa, 0);
	tb = gs_texture_create(CW, CH, GS_RGBA, 1, &pb, 0);
	target = gs_texture_create(CW, CH, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
	stage = gs_stagesurface_create(CW, CH, GS_RGBA);

	for (size_t k = 0; k < sizeof(configs) / sizeof(configs[0]); k++) {
		const struct config *c = &configs[k];

		for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
			double black;

			draw(c, times[i]);
			black = share_of(0, 0, 0);
			check(black < 0.001, "[config %zu] t=%.2f: %.4f of the canvas is black", k, times[i], black);

			if (times[i] == 0.0f)
				check(share_of(255, 0, 0) == 1.0, "[config %zu] t=0: outgoing scene only %.6f visible",
				      k, share_of(255, 0, 0));
			if (times[i] == 1.0f)
				check(share_of(0, 0, 255) == 1.0, "[config %zu] t=1: incoming scene only %.6f visible",
				      k, share_of(0, 0, 255));
		}

		/* in the hold, a centred portal is open: the incoming scene is at
		 * its centre, the outgoing one is at a far corner, and the rim
		 * has lit something */
		if (c->hold > 0.0f && c->ox >= 0.0f && c->ox <= 100.0f && c->oy >= 0.0f && c->oy <= 100.0f) {
			int cxp = (int)(c->ox / 100.0f * (CW - 1)), cyp = (int)(c->oy / 100.0f * (CH - 1));
			int fx = c->ox < 50.0f ? CW - 1 : 0, fy = c->oy < 50.0f ? CH - 1 : 0;
			uint8_t *mid, *far;
			double lit;

			draw(c, c->open + 0.5f * c->hold);
			mid = out[cyp][cxp];
			far = out[fy][fx];
			lit = 1.0 - share_of(255, 0, 0) - share_of(0, 0, 255);

			check(mid[2] > 200 && mid[0] < 60, "[config %zu] hold: centre is not the incoming scene", k);
			check(far[0] > 200 && far[2] < 60, "[config %zu] hold: far corner is not the outgoing scene",
			      k);
			check(lit > 0.002, "[config %zu] hold: the rim lights only %.4f of the canvas", k, lit);
		}
	}

	obs_leave_graphics();
	printf("%d GPU checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
