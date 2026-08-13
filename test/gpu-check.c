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
 * Renders the effect on a real graphics device, across every transition type,
 * shape and direction, and checks what actually lands on screen.
 *
 * The outgoing scene is red, the incoming scene is blue and the tiles are
 * green, so those three colours and blends between them are the only things
 * that may appear. A black pixel means a gap, a NaN or an undefined sample.
 *
 * Needs a graphics device, so it is not part of the ctest suite. Build with
 * -DENABLE_GPU_TESTS=ON and run it against the effect file:
 *
 *     xvfb-run -a ./gpu-check ../data/effects/tiles_transition.effect
 *
 * The uniform setup mirrors tiles_callback() in src/tiles-transition.c.
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

#ifndef GPU_CHECK_GRAPHICS_MODULE
#define GPU_CHECK_GRAPHICS_MODULE "libobs-opengl.so.1"
#endif

#define CW 640
#define CH 360

static uint32_t px_a[CW * CH], px_b[CW * CH];
static uint8_t out[CH][CW][4];
static int failures, checks;

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

static gs_effect_t *eff;
static gs_texture_t *ta, *tb, *target;
static gs_stagesurf_t *stage;

#define P(n) gs_effect_get_param_by_name(eff, n)

static float circumradius(int shape, float tile_px)
{
	return shape == 0 ? tile_px * 0.7071068f : tile_px * 0.5773503f;
}

static float edge_width(int shape, float tile_px)
{
	if (shape == 1)
		return 1.7320508f / tile_px;
	if (shape == 3)
		return 3.4641016f / tile_px;
	return 2.0f / tile_px;
}

static void draw(int mode, int shape, int direction, float t, float angle_deg, float ox_pct, float oy_pct,
		 float grid_deg, bool invert, bool reverse_exit, bool b_tiles)
{
	struct vec2 canvas, origin, grid_rot, dir, range;
	struct vec4 green;
	float tile_px = 64.0f, tile_dur = 0.35f, band, pad;
	float angle = angle_deg * (float)M_PI / 180.0f;
	float corner_x[4] = {0.0f, CW, 0.0f, CW}, corner_y[4] = {0.0f, 0.0f, CH, CH};
	float max_radius = 0.0f, pmin = 0.0f, pmax = 0.0f;
	uint8_t *data;
	uint32_t linesize;

	vec2_set(&canvas, CW, CH);
	vec2_set(&origin, ox_pct / 100.0f * CW, oy_pct / 100.0f * CH);
	vec2_set(&grid_rot, cosf(grid_deg * (float)M_PI / 180.0f), sinf(grid_deg * (float)M_PI / 180.0f));
	vec2_set(&dir, cosf(angle), sinf(angle));
	pad = circumradius(shape, tile_px);

	for (int i = 0; i < 4; i++) {
		float dx = corner_x[i] - origin.x, dy = corner_y[i] - origin.y;
		float rad = sqrtf(dx * dx + dy * dy), proj = dx * dir.x + dy * dir.y;
		if (rad > max_radius)
			max_radius = rad;
		if (i == 0 || proj < pmin)
			pmin = proj;
		if (i == 0 || proj > pmax)
			pmax = proj;
	}

	if (direction == 4)
		vec2_set(&range, pmin - pad, fmaxf(pmax - pmin + 2.0f * pad, 1.0f));
	else if (direction == 2 || direction == 3)
		vec2_set(&range, 0.0f, fmaxf(fmaxf(fabsf(pmin), fabsf(pmax)) + pad, 1.0f));
	else
		vec2_set(&range, 0.0f, fmaxf(max_radius + pad, 1.0f));

	band = 240.0f / range.y;
	if (mode == 2) {
		float total = 2.0f * tile_dur + band;
		if (total > 0.95f) {
			tile_dur *= 0.95f / total;
			band *= 0.95f / total;
		}
	}

	vec4_from_rgba(&green, 0xFF00FF00);

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
	gs_effect_set_vec2(P("origin_px"), &origin);
	gs_effect_set_vec2(P("grid_rot"), &grid_rot);
	gs_effect_set_vec2(P("dir_vec"), &dir);
	gs_effect_set_vec2(P("dir_range"), &range);
	gs_effect_set_vec4(P("color_a"), &green);
	gs_effect_set_vec4(P("color_b"), &green);
	gs_effect_set_float(P("progress"), t);
	gs_effect_set_float(P("tile_px"), tile_px);
	gs_effect_set_float(P("edge"), edge_width(shape, tile_px));
	gs_effect_set_float(P("tile_dur"), tile_dur);
	gs_effect_set_float(P("band"), band);
	gs_effect_set_float(P("randomness"), 0.0f);
	gs_effect_set_float(P("variation"), 0.0f);
	gs_effect_set_int(P("easing"), 0); /* linear order, so t maps straight to the sweep */
	gs_effect_set_int(P("mode"), mode);
	gs_effect_set_int(P("shape"), shape);
	gs_effect_set_int(P("direction"), direction);
	gs_effect_set_bool(P("invert"), invert);
	gs_effect_set_bool(P("reverse_exit"), reverse_exit);
	gs_effect_set_bool(P("b_tiles"), b_tiles);

	while (gs_effect_loop(eff, "Tiles"))
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
	const char *mode_name[] = {"cover", "keyhole", "wave", "crop shrink", "scale shrink"};
	const char *shape_name[] = {"square", "circle", "hexagon", "triangle"};
	const char *dir_name[] = {"inside out", "outside in", "mirrored in", "mirrored out", "directional"};
	const float times[] = {0.0f, 0.2f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f};
	char *effect_errors = NULL;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to tiles_transition.effect>\n", argv[0]);
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

	for (int mode = 0; mode < 5; mode++) {
		for (int shape = 0; shape < 4; shape++) {
			for (int direction = 0; direction < 5; direction++) {
				char ctx[128];
				snprintf(ctx, sizeof(ctx), "%s / %s / %s", mode_name[mode], shape_name[shape],
					 dir_name[direction]);

				for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
					double black;
					draw(mode, shape, direction, times[i], 37.0f, 30.0f, 70.0f, 12.0f, false, false,
					     false);
					black = share_of(0, 0, 0);
					check(black < 0.001,
					      "[%s] t=%.2f: %.4f of the canvas is neither scene "
					      "nor tile colour",
					      ctx, times[i], black);

					if (times[i] == 0.0f)
						check(share_of(255, 0, 0) > 0.999,
						      "[%s] t=0: outgoing scene only %.4f visible", ctx,
						      share_of(255, 0, 0));
					if (times[i] == 1.0f)
						check(share_of(0, 0, 255) > 0.999,
						      "[%s] t=1: incoming scene only %.4f visible", ctx,
						      share_of(0, 0, 255));
					if (mode == 0 && times[i] == 0.5f)
						check(share_of(0, 255, 0) > 0.999,
						      "[%s] t=0.5: tiles cover only %.4f of the canvas", ctx,
						      share_of(0, 255, 0));
				}
			}
		}
	}

	/* the option toggles, and an origin pushed outside the frame */
	for (int shape = 0; shape < 4; shape++) {
		draw(1, shape, 0, 1.0f, 0.0f, 50.0f, 50.0f, 0.0f, true, false, false);
		check(share_of(0, 0, 255) > 0.999, "keyhole inverted / %s: t=1 incoming scene only %.4f visible",
		      shape_name[shape], share_of(0, 0, 255));

		draw(0, shape, 0, 1.0f, 0.0f, 50.0f, 50.0f, 0.0f, false, true, false);
		check(share_of(0, 0, 255) > 0.999, "cover reverse exit / %s: t=1 incoming scene only %.4f visible",
		      shape_name[shape], share_of(0, 0, 255));

		draw(4, shape, 0, 1.0f, 0.0f, 50.0f, 50.0f, 0.0f, false, false, true);
		check(share_of(0, 0, 255) > 0.999, "scale shrink + tiled entry / %s: t=1 incoming scene only %.4f",
		      shape_name[shape], share_of(0, 0, 255));

		draw(0, shape, 4, 0.5f, 214.0f, -40.0f, 130.0f, 0.0f, false, false, false);
		check(share_of(0, 255, 0) > 0.999,
		      "cover from an off-canvas origin / %s: tiles cover only %.4f at t=0.5", shape_name[shape],
		      share_of(0, 255, 0));
	}

	obs_leave_graphics();
	printf("%d GPU checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
