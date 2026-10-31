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
 * Renders the vortex effect on a real graphics device and checks what lands on
 * screen: the first frame is the outgoing scene and the last the incoming one,
 * pixel for pixel; through the hold not a pixel of either scene shows, which is
 * what hides the swap; and the portal and the reveal really are part way across
 * the canvas while they run. Across centres on and off the canvas, every colour
 * mix, the swirl on and off, and rough and clean rims.
 *
 * Needs a graphics device, so ctest runs it under the "gpu" label. Build with
 * -DENABLE_GPU_TESTS=ON; on a headless machine, `xvfb-run -a ctest -L gpu`, or
 * on its own against the effect file:
 *
 *     xvfb-run -a ./vortex-check ../data/effects/vortex_transition.effect
 *
 * The uniforms are worked out with src/vortex-math.c, as vortex_callback() in
 * src/vortex-transition.c does.
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

#include "vortex-math.h"

#include "gpu-check-video.h"

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
	float open_end, reveal_start;
	float arms, twist, spin, detail;
	float fringe;
	float smoke_scale, smoke_soft;
	float warp; /* radians; 0 is the swirl off */
	int colors;
	int mix;
};

/* green, white and two greys for the body, so no generated colour can be
 * mistaken for either scene */
static const uint32_t body_rgba[VORTEX_BODY_COLORS] = {0xFF00C000, 0xFFFFFFFF, 0xFF808080, 0xFF40A040};

static void draw(const struct config *c, float t)
{
	struct vortex_clock clock;
	struct vec2 canvas, origin;
	struct vec4 bright, body[VORTEX_BODY_COLORS];
	float unit_px = CH * 0.5f;
	float open_end = c->open_end, reveal_start = c->reveal_start;
	static const char *const body_param[VORTEX_BODY_COLORS] = {"color_deep", "color_deep2", "color_deep3",
								   "color_deep4"};
	uint8_t *data;
	uint32_t linesize;

	vortex_clamp_phases(&open_end, &reveal_start);
	vortex_clock_at(open_end, reveal_start, c->warp, c->warp * 0.35f / 3.0f, t, &clock);

	vec2_set(&canvas, CW, CH);
	vec2_set(&origin, c->ox / 100.0f * CW, c->oy / 100.0f * CH);
	vec4_from_rgba(&bright, 0xFFFFFFFF);
	for (int i = 0; i < VORTEX_BODY_COLORS; i++)
		vec4_from_rgba(&body[i], body_rgba[i]);

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
	gs_effect_set_float(P("unit_px"), unit_px);
	gs_effect_set_float(P("reach"), vortex_reach(origin.x, origin.y, CW, CH, unit_px));
	gs_effect_set_float(P("progress"), t);
	gs_effect_set_float(P("open_end"), open_end);
	gs_effect_set_float(P("reveal_start"), reveal_start);
	gs_effect_set_float(P("cut"), clock.cut);
	gs_effect_set_float(P("iris_k"), clock.iris_k);
	gs_effect_set_float(P("reveal_k"), clock.reveal_k);
	gs_effect_set_float(P("glow"), clock.glow);
	gs_effect_set_float(P("arms"), c->arms);
	gs_effect_set_float(P("twist"), c->twist);
	gs_effect_set_float(P("spin"), c->spin);
	gs_effect_set_float(P("detail"), c->detail);
	gs_effect_set_float(P("fringe"), c->fringe);
	gs_effect_set_float(P("intensity"), 1.0f);
	gs_effect_set_float(P("smoke_scale"), c->smoke_scale);
	gs_effect_set_float(P("smoke_soft"), c->smoke_soft);
	gs_effect_set_float(P("swirl_angle"), clock.swirl_angle);
	gs_effect_set_float(P("swirl_pull"), clock.swirl_pull);
	gs_effect_set_vec4(P("color_bright"), &bright);
	for (int i = 0; i < VORTEX_BODY_COLORS; i++)
		gs_effect_set_vec4(P(body_param[i]), &body[i]);
	gs_effect_set_float(P("body_span"), (float)(c->colors - 1));
	gs_effect_set_int(P("color_mix"), c->mix);
	gs_effect_set_float(P("core_level"), vortex_core_level(0.5f));
	gs_effect_set_float(P("core_width"), vortex_core_width(0.5f, 0.5f));

	while (gs_effect_loop(eff, "Vortex"))
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
	char *effect_errors = NULL;

	/* clang-format off */
	const struct config configs[] = {
		/* ox      oy     open   reveal arms  twist spin  detail fringe scale soft  warp  colours mix */
		{50.0f,   50.0f,  0.22f, 0.62f, 5.0f, 1.6f, 0.9f, 2.0f, 0.35f, 4.0f, 0.30f, 0.0f, 1, VORTEX_MIX_ARMS},
		{50.0f,   50.0f,  0.22f, 0.62f, 5.0f, 1.6f, 0.9f, 2.0f, 0.35f, 4.0f, 0.30f, 1.8f, 4, VORTEX_MIX_ARMS},
		{20.0f,   75.0f,  0.10f, 0.50f, 1.0f, 0.0f, -3.0f, 6.0f, 1.00f, 0.5f, 0.80f, 6.0f, 3, VORTEX_MIX_RADIAL},
		{-40.0f, 130.0f,  0.40f, 0.45f, 16.0f, 6.0f, 3.0f, 0.5f, 0.00f, 8.0f, 0.05f, 0.0f, 4, VORTEX_MIX_BANDS},
		{110.0f, -20.0f,  0.02f, 0.95f, 3.0f, 2.5f, 0.0f, 3.0f, 0.60f, 2.0f, 0.02f, 3.0f, 2, VORTEX_MIX_BANDS},
	};
	/* clang-format on */

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to vortex_transition.effect>\n", argv[0]);
		return 2;
	}
	effect_path = argv[1];

	for (int i = 0; i < CW * CH; i++) {
		px_a[i] = 0xFF0000FF;
		px_b[i] = 0xFFFF0000;
	}

	if (!obs_startup("en-US", NULL, NULL))
		return 3;
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
	if (gpu_check_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
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
		float open_end = c->open_end, reveal_start = c->reveal_start;
		double red, blue;

		vortex_clamp_phases(&open_end, &reveal_start);

		draw(c, 0.0f);
		check(share_of(255, 0, 0) == 1.0, "[config %zu] t=0: outgoing scene only %.6f visible", k,
		      share_of(255, 0, 0));

		draw(c, 1.0f);
		check(share_of(0, 0, 255) == 1.0, "[config %zu] t=1: incoming scene only %.6f visible", k,
		      share_of(0, 0, 255));

		/* the hold, from just after the portal fills the canvas to just
		 * before the reveal: neither scene may show anywhere */
		for (int i = 0; i <= 4; i++) {
			float t = open_end + (reveal_start - open_end) * (0.02f + 0.96f * (float)i / 4.0f);

			draw(c, t);
			red = share_of(255, 0, 0);
			blue = share_of(0, 0, 255);
			check(red == 0.0 && blue == 0.0,
			      "[config %zu] hold t=%.3f: a scene shows through (red %.6f, blue %.6f)", k, t, red, blue);
		}

		/* part way through opening, and most of the way through the
		 * reveal, the vortex covers some of the canvas and not all of it.
		 * Late in the reveal, because the softest smoke leaves every pixel
		 * partly covered until well past halfway. */
		draw(c, open_end * 0.5f);
		red = share_of(255, 0, 0);
		check(red > 0.0 && red < 1.0, "[config %zu] opening: outgoing scene is %.4f of the canvas", k, red);

		draw(c, reveal_start + (1.0f - reveal_start) * 0.85f);
		blue = share_of(0, 0, 255);
		check(blue > 0.0 && blue < 1.0, "[config %zu] reveal: incoming scene is %.4f of the canvas", k, blue);
	}

	obs_leave_graphics();
	printf("%d GPU checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
