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
 * Renders the matte cutout effect on a real graphics device, over every shape
 * and both directions, and checks what lands on screen.
 *
 * The outgoing scene is red, the incoming one blue, and the bands green and
 * yellow, so a black pixel means a gap or a bad sample. On top of the first and
 * last frame being exactly one scene each, it renders every shape with no bands
 * mid-transition and compares, pixel for pixel, which scene each pixel shows
 * against matte_field() evaluated on the CPU. That is what holds the shader's
 * field and its C mirror in src/matte-math.c to each other: the plugin finds
 * the front's range with the mirror, so a mirror that drifted would cut the
 * transition short or leave it starting late.
 *
 * Needs a graphics device, so it is not part of the ctest suite:
 *
 *     xvfb-run -a ./matte-check ../data/effects/matte_transition.effect
 *
 * The uniform setup mirrors matte_callback() in src/matte-transition.c.
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

#include "matte-math.h"

#ifndef GPU_CHECK_GRAPHICS_MODULE
#define GPU_CHECK_GRAPHICS_MODULE "libobs-opengl.so.1"
#endif

#define CW 640
#define CH 360

static uint32_t px_a[CW * CH], px_b[CW * CH], px_luma[CW * CH];
static uint8_t out[CH][CW][4];
static int failures, checks;

static gs_effect_t *eff;
static gs_texture_t *ta, *tb, *tluma, *tprofile, *target;
static gs_stagesurf_t *stage;
static float profile[MATTE_PROFILE_SIZE];

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
	int shape;
	int direction;
	float ox, oy; /* percent */
	float angle_deg;
	float spin;
	float stretch;
	float border, echo;
	int echoes;
	float feather;
	float glow;
	struct matte_timing timing;
};

/* mirrors matte_frame_for() and matte_shape_params() */
static void setup(const struct config *c, struct matte_frame *f, struct matte_shape_params *sp)
{
	float far_x, far_y, reach;

	f->origin_x = c->ox / 100.0f * CW;
	f->origin_y = c->oy / 100.0f * CH;
	f->angle0 = c->angle_deg * (float)M_PI / 180.0f;
	f->angle1 = f->angle0 + c->spin * 2.0f * (float)M_PI;
	if (matte_is_sweep(c->shape) || c->shape == MATTE_SHAPE_BLINDS) {
		f->stretch_x = f->stretch_y = 1.0f;
	} else {
		f->stretch_x = sqrtf(c->stretch);
		f->stretch_y = 1.0f / f->stretch_x;
	}

	far_x = fmaxf(fabsf(f->origin_x), fabsf(CW - f->origin_x));
	far_y = fmaxf(fabsf(f->origin_y), fabsf(CH - f->origin_y));
	reach = fmaxf(sqrtf(far_x * far_x + far_y * far_y), 1.0f);

	memset(sp, 0, sizeof(*sp));
	sp->shape = c->shape;
	sp->star_n = 5.0f;
	sp->star_k = matte_star_slope(5.0f, 0.45f);
	sp->arms = 3.0f;
	sp->amp = 40.0f;
	sp->wavelen = 180.0f;
	sp->lobes = 7.0f;
	sp->profile = profile;
	sp->pitch = c->shape == MATTE_SHAPE_CLOCK ? (float)M_PI * reach / sp->arms : reach / 2.5f;
	if (c->shape == MATTE_SHAPE_BLINDS) {
		float span = (CW * fabsf(sinf(f->angle0)) + CH * fabsf(cosf(f->angle0))) / f->stretch_y;
		sp->bar_px = fmaxf(span / 6.0f, 2.0f);
	}
}

static const float DRIFT = 1.3f;

static void draw(const struct config *c, float t, float *front_out)
{
	struct matte_frame f;
	struct matte_shape_params sp;
	struct matte_bands bands = {c->border, c->echo, c->echoes, fmaxf(c->feather, 1.0f), c->glow};
	struct vec2 canvas, origin, rot, inv_stretch;
	struct vec4 green, yellow, magenta, white;
	float lo, hi, u, front, angle;
	float luma_scale = 0.5f * CH;
	uint8_t *data;
	uint32_t linesize;

	setup(c, &f, &sp);
	if (c->shape == MATTE_SHAPE_LUMA) {
		lo = 0.0f;
		hi = luma_scale;
	} else {
		matte_field_range(&sp, &f, CW, CH, &lo, &hi);
	}
	u = matte_travel(&c->timing, t);
	front = matte_front(c->direction, lo, hi, &bands, u);
	angle = f.angle0 + (f.angle1 - f.angle0) * t;
	if (front_out)
		*front_out = front;

	vec2_set(&canvas, CW, CH);
	vec2_set(&origin, f.origin_x, f.origin_y);
	vec2_set(&rot, cosf(angle), sinf(angle));
	vec2_set(&inv_stretch, 1.0f / f.stretch_x, 1.0f / f.stretch_y);
	vec4_from_rgba(&green, 0xFF00FF00);
	vec4_from_rgba(&yellow, 0xFF00FFFF);
	vec4_from_rgba(&magenta, 0xFFFF00FF);
	vec4_from_rgba(&white, 0xFFFFFFFF);

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
	gs_effect_set_texture(P("luma_tex"), tluma);
	gs_effect_set_texture(P("profile_tex"), tprofile);
	gs_effect_set_vec2(P("canvas"), &canvas);
	gs_effect_set_vec2(P("origin_px"), &origin);
	gs_effect_set_vec2(P("rot"), &rot);
	gs_effect_set_vec2(P("inv_stretch"), &inv_stretch);
	gs_effect_set_int(P("shape"), c->shape);
	gs_effect_set_float(P("star_n"), sp.star_n);
	gs_effect_set_float(P("star_k"), sp.star_k);
	gs_effect_set_float(P("arms"), sp.arms);
	gs_effect_set_float(P("pitch"), sp.pitch);
	gs_effect_set_float(P("amp"), sp.amp);
	gs_effect_set_float(P("wavelen"), sp.wavelen);
	gs_effect_set_float(P("lobes"), sp.lobes);
	gs_effect_set_float(P("bar_px"), fmaxf(sp.bar_px, 1.0f));
	gs_effect_set_float(P("phase"), DRIFT * t);
	gs_effect_set_float(P("luma_scale"), luma_scale);
	gs_effect_set_float(P("front"), front);
	gs_effect_set_float(P("dir_sign"), matte_sign(c->direction));
	gs_effect_set_float(P("feather"), bands.feather);
	gs_effect_set_float(P("border"), bands.border);
	gs_effect_set_float(P("echo"), bands.echo);
	gs_effect_set_int(P("echo_count"), bands.echo_count);
	gs_effect_set_float(P("band_end"), matte_band_width(&bands));
	gs_effect_set_float(P("glow"), bands.glow);
	gs_effect_set_float(P("glow_amt"), 1.0f);
	gs_effect_set_vec4(P("color_border"), &green);
	gs_effect_set_vec4(P("color_echo1"), &yellow);
	gs_effect_set_vec4(P("color_echo2"), &magenta);
	gs_effect_set_vec4(P("color_echo3"), &yellow);
	gs_effect_set_vec4(P("color_echo4"), &magenta);
	gs_effect_set_vec4(P("color_glow"), &white);

	while (gs_effect_loop(eff, "Matte"))
		gs_draw_sprite(NULL, 0, CW, CH);

	gs_set_render_target(NULL, NULL);
	gs_stage_texture(stage, target);
	gs_stagesurface_map(stage, &data, &linesize);
	for (int y = 0; y < CH; y++)
		memcpy(out[y], data + (size_t)y * linesize, CW * 4);
	gs_stagesurface_unmap(stage);
}

/*
 * Renders c with hard edges at t and compares each pixel's scene against the C
 * field. Pixels within a couple of field units of the front are allowed either
 * way - that is the anti-aliasing band, and float precision on the two sides
 * differs - but anywhere else the two must agree.
 */
static void compare_with_mirror(const struct config *c, float t, const char *ctx)
{
	struct matte_frame f;
	struct matte_shape_params sp;
	float front, angle;
	long wrong = 0, worst_x = -1, worst_y = -1;

	draw(c, t, &front);
	setup(c, &f, &sp);
	angle = f.angle0 + (f.angle1 - f.angle0) * t;

	for (int y = 0; y < CH; y++) {
		for (int x = 0; x < CW; x++) {
			float qx, qy, d;
			bool is_b, is_a;

			matte_to_shape(&f, angle, x + 0.5f, y + 0.5f, &qx, &qy);
			d = matte_sign(c->direction) * (front - matte_field(&sp, qx, qy, DRIFT * t));

			is_b = out[y][x][2] > 200 && out[y][x][0] < 55;
			is_a = out[y][x][0] > 200 && out[y][x][2] < 55;

			if ((d > 2.0f && !is_b) || (d < -2.0f && !is_a)) {
				if (wrong++ == 0) {
					worst_x = x;
					worst_y = y;
				}
			}
		}
	}

	/* the seams of the clock, spiral and the curl's lip are hard steps in the
	 * field, where a pixel may legitimately land on either side */
	check(wrong <= CW * CH / 2000, "[%s] t=%.2f: %ld pixels disagree with the C field (first at %ld,%ld)", ctx, t,
	      wrong, worst_x, worst_y);
}

int main(int argc, char **argv)
{
	struct obs_video_info ovi = {0};
	const char *effect_path;
	const uint8_t *pa = (const uint8_t *)px_a, *pb = (const uint8_t *)px_b, *pl = (const uint8_t *)px_luma;
	const uint8_t *pp = (const uint8_t *)profile;
	const char *shape_name[MATTE_SHAPE_COUNT] = {"circle", "square", "triangle", "diamond", "hexagon", "octagon",
						     "star",   "heart",  "cross",    "wave",    "curl",    "ripple",
						     "clock",  "spiral", "blinds",   "zigzag",  "image",   "luma"};
	const float times[] = {0.0f, 0.15f, 0.35f, 0.5f, 0.65f, 0.85f, 1.0f};
	static uint8_t mask[96 * 64 * 4];
	char *effect_errors = NULL;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to matte_transition.effect>\n", argv[0]);
		return 2;
	}
	effect_path = argv[1];

	for (int i = 0; i < CW * CH; i++) {
		px_a[i] = 0xFF0000FF;
		px_b[i] = 0xFFFF0000;
	}

	/* a luma ramp across the canvas, dark at the left */
	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++) {
			uint32_t v = (uint32_t)((x * 255) / (CW - 1));
			px_luma[y * CW + x] = 0xFF000000 | (v << 16) | (v << 8) | v;
		}

	/* an image silhouette: a lopsided blob with a notch out of it, on a
	 * transparent background */
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 96; x++) {
			float dx = (x - 48.0f) / 40.0f, dy = (y - 32.0f) / 26.0f;
			bool in = dx * dx + dy * dy < 1.0f && !(x > 60 && y > 28 && y < 36);
			uint8_t *m = mask + (y * 96 + x) * 4;
			m[0] = m[1] = m[2] = 255;
			m[3] = in ? 255 : 0;
		}
	if (!matte_build_profile(mask, 96, 64, profile)) {
		fprintf(stderr, "could not build the test silhouette\n");
		return 1;
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
	tluma = gs_texture_create(CW, CH, GS_RGBA, 1, &pl, 0);
	tprofile = gs_texture_create(MATTE_PROFILE_SIZE, 1, GS_R32F, 1, &pp, 0);
	target = gs_texture_create(CW, CH, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
	stage = gs_stagesurface_create(CW, CH, GS_RGBA);

	for (int shape = 0; shape < MATTE_SHAPE_COUNT; shape++) {
		for (int direction = 0; direction < 2; direction++) {
			/* a plain cut, then the full stinger with an offset centre,
			 * a turn, a spin and a stretch */
			struct config cfg[2] = {
				{shape, direction, 50, 50, 0, 0, 1.0f, 0, 0, 0, 1, 0, {MATTE_EASE_LINEAR, 0, 0, 0}},
				{shape,
				 direction,
				 30,
				 70,
				 25,
				 0.6f,
				 1.8f,
				 10,
				 24,
				 2,
				 3,
				 0,
				 {MATTE_EASE_OVERSHOOT, 0.7f, 0.3f, 0.25f}},
			};

			for (int k = 0; k < 2; k++) {
				char ctx[128];
				snprintf(ctx, sizeof(ctx), "%s / %s / %s", shape_name[shape],
					 direction ? "outside in" : "inside out", k ? "stinger" : "plain");

				for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
					double black;

					draw(&cfg[k], times[i], NULL);
					black = share_of(0, 0, 0);
					check(black < 0.001, "[%s] t=%.2f: %.4f of the canvas is black", ctx, times[i],
					      black);

					if (times[i] == 0.0f)
						check(share_of(255, 0, 0) > 0.9999,
						      "[%s] t=0: outgoing scene only %.4f visible", ctx,
						      share_of(255, 0, 0));
					if (times[i] == 1.0f)
						check(share_of(0, 0, 255) > 0.9999,
						      "[%s] t=1: incoming scene only %.4f visible", ctx,
						      share_of(0, 0, 255));
				}

				/* the luma wipe's field is a texture, not matte_field() */
				if (k == 0 && shape != MATTE_SHAPE_LUMA) {
					compare_with_mirror(&cfg[k], 0.3f, ctx);
					compare_with_mirror(&cfg[k], 0.6f, ctx);
				}
			}

			/* the stinger's bands really are drawn: mid-transition some of
			 * the canvas is border or echo */
			{
				struct config c = {shape,
						   direction,
						   50,
						   50,
						   0,
						   0,
						   1.0f,
						   12,
						   40,
						   2,
						   1,
						   0,
						   {MATTE_EASE_LINEAR, 0, 0, 0}};
				double bands;

				draw(&c, 0.5f, NULL);
				bands = share_of(0, 255, 0) + share_of(255, 255, 0) + share_of(255, 0, 255);
				check(bands > 0.002, "[%s / %s] t=0.5: the bands cover only %.4f of the canvas",
				      shape_name[shape], direction ? "outside in" : "inside out", bands);
			}

			/* a glow, which is compact, still leaves both ends clean */
			{
				struct config c = {shape,
						   direction,
						   50,
						   50,
						   0,
						   0.3f,
						   1.0f,
						   8,
						   0,
						   0,
						   4,
						   30,
						   {MATTE_EASE_IN_OUT, 0, 0, 0}};

				draw(&c, 0.0f, NULL);
				check(share_of(255, 0, 0) > 0.9999, "[%s glow] t=0: outgoing only %.4f",
				      shape_name[shape], share_of(255, 0, 0));
				draw(&c, 1.0f, NULL);
				check(share_of(0, 0, 255) > 0.9999, "[%s glow] t=1: incoming only %.4f",
				      shape_name[shape], share_of(0, 0, 255));
			}
		}

		/* an origin pushed off the canvas still finishes on time */
		{
			struct config c = {shape, 0, -40, 130, 200, 0, 1.0f, 6, 20, 1, 1, 0, {MATTE_EASE_OUT, 0, 0, 0}};

			draw(&c, 1.0f, NULL);
			check(share_of(0, 0, 255) > 0.9999, "[%s off-canvas] t=1: incoming only %.4f",
			      shape_name[shape], share_of(0, 0, 255));
			draw(&c, 0.0f, NULL);
			check(share_of(255, 0, 0) > 0.9999, "[%s off-canvas] t=0: outgoing only %.4f",
			      shape_name[shape], share_of(255, 0, 0));
		}
	}

	obs_leave_graphics();
	printf("%d GPU checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
