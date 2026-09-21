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
 * Renders the glass transition's real mesh on a real graphics device, across
 * every break style and crack pattern, and checks what lands on screen.
 *
 * test/test-glass.c proves the shards partition the canvas. It cannot prove
 * that the partition survives being uploaded, rasterised and lit: a vertex
 * layout that does not match what the shader declares, a triangle wound the
 * wrong way against the cull mode, a seam between two shards that the bleed
 * fails to close, or a first frame that is not quite the outgoing scene would
 * all get past it. Those only show up on a device, which is what this is for.
 *
 * The outgoing scene is red, the incoming scene is blue and the crack colour is
 * green, so those three and blends of them are the only things that may appear.
 * A black pixel means a gap the background did not cover, a bad sample or a
 * NaN.
 *
 * The two frames that matter most are the first and the last. OBS shows them as
 * the join between two scenes, so the pane at rest has to be the outgoing scene
 * exactly - no seams between shards, no rim light, no tint - and the final
 * frame has to be the incoming scene the same way.
 *
 * Needs a graphics device, so it is not part of the ctest suite. Build with
 * -DENABLE_GPU_TESTS=ON and run it against the effect file:
 *
 *     xvfb-run -a ./glass-check ../data/effects/glass_transition.effect
 *
 * The mesh upload and the uniform setup mirror glass_mesh_build() and
 * glass_callback() in src/glass-transition.c.
 */

#include <obs.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glass-mesh.h"

#ifndef GPU_CHECK_GRAPHICS_MODULE
#define GPU_CHECK_GRAPHICS_MODULE "libobs-opengl.so.1"
#endif

#define CW 640
#define CH 360

/* mirrors GLASS_ASSEMBLE_BG in src/glass-transition.c */
#define ASSEMBLE_BG 0.18f

static uint32_t px_a[CW * CH], px_b[CW * CH];
static uint8_t out[CH][CW][4];
static int failures, checks;

static gs_effect_t *eff;
static gs_texture_t *ta, *tb, *target;
static gs_stagesurf_t *stage;

#define P(n) gs_effect_get_param_by_name(eff, n)

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

static const char *style_name(int style)
{
	switch (style) {
	case GLASS_STYLE_WINDOW:
		return "window";
	case GLASS_STYLE_BLOW:
		return "blow";
	case GLASS_STYLE_WIPE:
		return "wipe";
	default:
		return "assemble";
	}
}

static const char *pattern_name(int pattern)
{
	switch (pattern) {
	case GLASS_PATTERN_WEB:
		return "web";
	case GLASS_PATTERN_VORONOI:
		return "voronoi";
	default:
		return "ice";
	}
}

/* ------------------------------------------------------------------ */
/* the mesh                                                           */
/* ------------------------------------------------------------------ */

static struct glass_field field;
static gs_vertbuffer_t *vb;
static gs_indexbuffer_t *ib;
static uint32_t *base_indices;
static struct glass_span *spans;
static uint32_t *order;
static struct glass_sortkey *keys;

static void mesh_free(void)
{
	if (vb) {
		gs_vertexbuffer_destroy(vb);
		vb = NULL;
	}
	if (ib) {
		gs_indexbuffer_destroy(ib);
		ib = NULL;
	}

	free(base_indices);
	free(spans);
	free(order);
	free(keys);
	base_indices = NULL;
	spans = NULL;
	order = NULL;
	keys = NULL;

	glass_field_free(&field);
}

/* mirrors glass_mesh_build() in src/glass-transition.c */
static bool mesh_build(int pattern, float shard_px, float ix, float iy)
{
	struct glass_build build;
	struct glass_vertex *verts;
	struct gs_vb_data *vbd;
	uint32_t *indices;
	size_t nverts, nindices, i;

	mesh_free();

	build.canvas_cx = CW;
	build.canvas_cy = CH;
	build.impact_x = ix;
	build.impact_y = iy;
	build.shard_px = shard_px;
	build.pattern = pattern;
	build.seed = 7u;

	if (!glass_field_build(&field, &build))
		return false;

	nverts = glass_vertex_count(&field);
	nindices = glass_index_count(&field);

	verts = malloc(nverts * sizeof(*verts));
	base_indices = malloc(nindices * sizeof(*base_indices));
	spans = malloc((size_t)field.count * sizeof(*spans));
	order = malloc((size_t)field.count * sizeof(*order));
	keys = malloc((size_t)field.count * sizeof(*keys));
	if (!verts || !base_indices || !spans || !order || !keys) {
		free(verts);
		return false;
	}

	glass_emit(&field, verts, base_indices, spans);

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

	free(verts);

	indices = bmalloc(nindices * sizeof(*indices));
	memcpy(indices, base_indices, nindices * sizeof(*indices));

	vb = gs_vertexbuffer_create(vbd, 0);
	ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, nindices, GS_DYNAMIC);

	return vb && ib;
}

static void order_pass(const struct glass_motion *motion)
{
	uint32_t *dst = gs_indexbuffer_get_data(ib);
	size_t at = 0;

	if (!dst)
		return;

	glass_sort_order(&field, motion, order, keys);

	for (int i = 0; i < field.count; i++) {
		memcpy(dst + at, base_indices + spans[order[i]].first, spans[order[i]].count * sizeof(uint32_t));
		at += spans[order[i]].count;
	}

	gs_indexbuffer_flush(ib);
}

/* Everything the effect declares, re-uploaded before each technique.
 *
 * Ending a technique returns every parameter in an effect to its default, and
 * these have none, so a frame that sets them once and then runs two techniques
 * loses the lot on the second. Mirrors glass_set_uniforms() in
 * src/glass-transition.c, which is there for the same reason. */
static void set_uniforms(int style, float t, float ix, float iy, bool flat, float crack_front, float hold_end,
			 const struct glass_motion *motion, bool arriving, bool use_b)
{
	struct vec2 canvas, impact, gravity_dir, glint_dir;
	struct vec4 green;

	vec2_set(&canvas, CW, CH);
	vec2_set(&impact, ix, iy);
	vec2_set(&gravity_dir, motion->gravity_x, motion->gravity_y);
	vec2_set(&glint_dir, 0.7071068f, 0.7071068f);
	vec4_from_rgba(&green, 0xFF00FF00);

	gs_effect_set_texture(P("a_tex"), ta);
	gs_effect_set_texture(P("b_tex"), tb);
	gs_effect_set_vec2(P("canvas"), &canvas);
	gs_effect_set_vec2(P("impact_px"), &impact);
	gs_effect_set_float(P("reach"), field.reach);
	gs_effect_set_float(P("progress"), t);
	gs_effect_set_float(P("hold_end"), hold_end);
	gs_effect_set_float(P("crack_front"), crack_front);
	gs_effect_set_int(P("style"), style);
	gs_effect_set_bool(P("flat_shards"), flat);
	gs_effect_set_bool(P("arriving"), arriving);
	gs_effect_set_bool(P("use_b"), use_b);
	gs_effect_set_vec2(P("gravity_dir"), &gravity_dir);
	gs_effect_set_float(P("gravity"), motion->gravity);
	gs_effect_set_float(P("throw_amt"), motion->throw_px);
	gs_effect_set_float(P("spin"), motion->spin);
	gs_effect_set_float(P("scatter"), motion->scatter);
	gs_effect_set_float(P("focal"), (float)CH * 1.75f);
	gs_effect_set_vec4(P("crack_color"), &green);
	gs_effect_set_float(P("crack_px"), 1.6f);
	gs_effect_set_float(P("crack_glow"), 1.4f);
	gs_effect_set_float(P("rim_amount"), 0.85f);
	gs_effect_set_float(P("rim_px"), 7.0f);
	gs_effect_set_float(P("refract_px"), 9.0f);
	gs_effect_set_float(P("tint_amount"), 0.22f);
	gs_effect_set_float(P("glint_amount"), 0.6f);
	gs_effect_set_float(P("glint_pos"), -0.2f + 1.4f * (t / 0.75f > 1.0f ? 1.0f : t / 0.75f));
	gs_effect_set_vec2(P("glint_dir"), &glint_dir);
	gs_effect_set_float(P("bg_level"), style == GLASS_STYLE_ASSEMBLE ? ASSEMBLE_BG : 1.0f);
}

static void draw_pass(int style, float t, float ix, float iy, bool flat, float crack_front, float hold_end,
		      const struct glass_motion *motion, bool arriving, bool use_b)
{
	order_pass(motion);
	set_uniforms(style, t, ix, iy, flat, crack_front, hold_end, motion, arriving, use_b);

	while (gs_effect_loop(eff, "Glass")) {
		gs_load_vertexbuffer(vb);
		gs_load_indexbuffer(ib);
		gs_draw(GS_TRIS, 0, 0);
	}
}

/* mirrors glass_callback() in src/glass-transition.c */
static void draw(int style, float t, float ix, float iy, bool flat)
{
	struct glass_motion motion;
	float crack_front;
	bool assemble = style == GLASS_STYLE_ASSEMBLE;
	uint8_t *data;
	uint32_t linesize;

	crack_front = glass_crack_front(t, 0.28f, GLASS_EASE_OUT);

	memset(&motion, 0, sizeof(motion));
	motion.style = style;
	motion.progress = t;
	motion.crack_front = crack_front;
	motion.hold_end = 0.40f;
	motion.gravity_x = 0.0f;
	motion.gravity_y = 1.0f;
	motion.gravity = 0.9f;
	motion.throw_px = 0.35f;
	motion.spin = 0.8f;
	motion.scatter = 0.45f;
	motion.reach = field.reach;
	motion.impact_x = ix;
	motion.impact_y = iy;
	motion.flat = flat;

	gs_set_render_target(target, NULL);
	gs_set_viewport(0, 0, CW, CH);
	gs_ortho(0.0f, CW, 0.0f, CH, -100.0f, 100.0f);
	gs_clear(GS_CLEAR_COLOR, &(struct vec4){{{0, 0, 0, 1}}}, 0.0f, 0);
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);
	gs_matrix_identity();

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	set_uniforms(style, t, ix, iy, flat, crack_front, motion.hold_end, &motion, false, false);
	while (gs_effect_loop(eff, "Background"))
		gs_draw_sprite(NULL, 0, CW, CH);

	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	if (assemble) {
		motion.arriving = true;
		draw_pass(style, t, ix, iy, flat, crack_front, motion.hold_end, &motion, true, true);
		motion.arriving = false;
		draw_pass(style, t, ix, iy, flat, crack_front, motion.hold_end, &motion, false, false);
	} else {
		draw_pass(style, t, ix, iy, flat, crack_front, motion.hold_end, &motion, false, false);
	}

	gs_load_indexbuffer(NULL);
	gs_load_vertexbuffer(NULL);
	gs_blend_state_pop();

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
	const float times[] = {0.0f, 0.15f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f};
	char *effect_errors = NULL;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to glass_transition.effect>\n", argv[0]);
		return 2;
	}
	effect_path = argv[1];

	for (int i = 0; i < CW * CH; i++) {
		px_a[i] = 0xFF0000FF; /* red   - the outgoing scene */
		px_b[i] = 0xFFFF0000; /* blue  - the incoming scene */
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

	for (int pattern = 0; pattern <= GLASS_PATTERN_ICE; pattern++) {
		if (!mesh_build(pattern, 56.0f, CW * 0.5f, CH * 0.5f)) {
			check(false, "[%s] the mesh would not build", pattern_name(pattern));
			continue;
		}

		for (int style = 0; style <= GLASS_STYLE_ASSEMBLE; style++) {
			char ctx[128];

			snprintf(ctx, sizeof(ctx), "%s / %s", pattern_name(pattern), style_name(style));

			for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
				double black;

				draw(style, times[i], CW * 0.5f, CH * 0.5f, false);
				black = share_of(0, 0, 0);

				check(black < 0.0005, "[%s] t=%.2f: %.4f of the canvas is neither scene nor glass", ctx,
				      times[i], black);

				/*
				 * The pane at rest has to be the outgoing scene
				 * and nothing else. Any shortfall here is a
				 * seam between two shards, a rim light that
				 * started early, or a shard that moved before
				 * it was asked to.
				 */
				if (times[i] == 0.0f)
					check(share_of(255, 0, 0) > 0.9995,
					      "[%s] t=0: the pane is only %.4f the outgoing scene", ctx,
					      share_of(255, 0, 0));

				if (times[i] == 1.0f)
					check(share_of(0, 0, 255) > 0.9995,
					      "[%s] t=1: the canvas is only %.4f the incoming scene", ctx,
					      share_of(0, 0, 255));
			}

			/* flat shards take a different path through the vertex
			 * shader, and skip the depth sort entirely */
			draw(style, 0.0f, CW * 0.5f, CH * 0.5f, true);
			check(share_of(255, 0, 0) > 0.9995, "[%s] flat, t=0: the pane is only %.4f the outgoing scene",
			      ctx, share_of(255, 0, 0));

			draw(style, 1.0f, CW * 0.5f, CH * 0.5f, true);
			check(share_of(0, 0, 255) > 0.9995,
			      "[%s] flat, t=1: the canvas is only %.4f the incoming scene", ctx, share_of(0, 0, 255));

			draw(style, 0.6f, CW * 0.5f, CH * 0.5f, true);
			check(share_of(0, 0, 0) < 0.0005, "[%s] flat, t=0.6: %.4f of the canvas is blank", ctx,
			      share_of(0, 0, 0));
		}
	}

	/*
	 * An impact pushed outside the frame. The break is sized to the
	 * furthest corner from wherever it is struck, so the pane still has to
	 * be whole on the first frame and gone on the last.
	 */
	for (int pattern = 0; pattern <= GLASS_PATTERN_ICE; pattern++) {
		if (!mesh_build(pattern, 44.0f, CW * -0.35f, CH * 1.3f))
			continue;

		draw(GLASS_STYLE_WINDOW, 0.0f, CW * -0.35f, CH * 1.3f, false);
		check(share_of(255, 0, 0) > 0.9995,
		      "[%s off-canvas impact] t=0: the pane is only %.4f the outgoing scene", pattern_name(pattern),
		      share_of(255, 0, 0));

		draw(GLASS_STYLE_WINDOW, 1.0f, CW * -0.35f, CH * 1.3f, false);
		check(share_of(0, 0, 255) > 0.9995,
		      "[%s off-canvas impact] t=1: the canvas is only %.4f the incoming scene", pattern_name(pattern),
		      share_of(0, 0, 255));

		draw(GLASS_STYLE_WINDOW, 0.5f, CW * -0.35f, CH * 1.3f, false);
		check(share_of(0, 0, 0) < 0.0005, "[%s off-canvas impact] t=0.5: %.4f of the canvas is blank",
		      pattern_name(pattern), share_of(0, 0, 0));
	}

	/* the smallest and largest shards the size slider reaches, where the
	 * shard cap and the sliver clipping are both under most pressure */
	for (float size = 16.0f; size <= 400.0f; size *= 2.5f) {
		if (!mesh_build(GLASS_PATTERN_WEB, size, CW * 0.5f, CH * 0.5f))
			continue;

		draw(GLASS_STYLE_WINDOW, 0.0f, CW * 0.5f, CH * 0.5f, false);
		check(share_of(255, 0, 0) > 0.9995, "[shard %.0fpx] t=0: the pane is only %.4f the outgoing scene",
		      (double)size, share_of(255, 0, 0));

		draw(GLASS_STYLE_WINDOW, 1.0f, CW * 0.5f, CH * 0.5f, false);
		check(share_of(0, 0, 255) > 0.9995, "[shard %.0fpx] t=1: the canvas is only %.4f the incoming scene",
		      (double)size, share_of(0, 0, 255));
	}

	mesh_free();
	obs_leave_graphics();

	printf("%d GPU checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
