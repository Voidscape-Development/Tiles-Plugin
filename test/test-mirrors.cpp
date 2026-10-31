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
 * Two pieces of shader math the plugin also needs on the CPU, while it runs:
 *
 *   - the matte cutout's field, which the plugin samples along the canvas edge
 *     to find the range the front has to cover (matte_field() in
 *     src/matte-math.c), and
 *   - the glass transition's shard motion, which the plugin evaluates for
 *     every shard every frame to draw them back to front (glass_shard_xform()
 *     in src/glass-mesh.c).
 *
 * Each exists twice, once in C and once in HLSL. This compiles the HLSL as well,
 * through test/hlsl.hpp, and holds the two together over every shape and style
 * and a spread of random inputs, so neither can drift from the other without a
 * failure here. A field that drifted would cut the matte short; a motion that
 * drifted would sort the shards by where they are not.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "glass-mesh.h"
#include "hlsl.hpp"
#include "matte-math.h"

/* ------------------------------------------------------------------ */
/* the effects                                                        */
/* ------------------------------------------------------------------ */

/* The silhouette profile both copies of the image field read. */
static float profile[MATTE_PROFILE_SIZE];

namespace matte_effect {
using namespace hlsl;

int shape;
float star_n;
float star_k;
float arms;
float pitch;
float amp;
float wavelen;
float lobes;
float bar_px;
float phase;

/* What the effect reads from profile_tex: a linear, wrapping sampler over the
 * profile, taken at texel centres. */
float profile_radius(float u)
{
	float f = (u - floorf(u)) * (float)MATTE_PROFILE_SIZE - 0.5f;
	float fl = floorf(f);
	float w = f - fl;
	int i0 = ((int)fl % MATTE_PROFILE_SIZE + MATTE_PROFILE_SIZE) % MATTE_PROFILE_SIZE;
	int i1 = (i0 + 1) % MATTE_PROFILE_SIZE;

	return profile[i0] + (profile[i1] - profile[i0]) * w;
}

#include "matte_math.effect"
} // namespace matte_effect

namespace glass_effect {
using namespace hlsl;

float progress;
float hold_end;
float crack_front;
int style;
bool flat_shards;
bool arriving;
float2 impact_px;
float reach;
float2 gravity_dir;
float gravity;
float throw_amt;
float spin;
float scatter;

#include "glass_math.effect"
} // namespace glass_effect

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *fmt, ...)
{
	va_list args;

	checks++;
	if (ok)
		return;

	failures++;
	fputs("FAIL: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

static float uniform(float lo, float hi)
{
	return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static bool close_enough(float a, float b, float scale)
{
	return fabsf(a - b) <= 1e-4f * fmaxf(scale, fmaxf(fabsf(a), fabsf(b)));
}

/* ------------------------------------------------------------------ */
/* the matte field                                                    */
/* ------------------------------------------------------------------ */

static const char *const shape_names[MATTE_SHAPE_COUNT] = {
	"circle", "square", "triangle", "diamond", "hexagon", "octagon", "star",   "heart", "cross",
	"wave",   "curl",   "ripple",   "clock",   "spiral",  "blinds",  "zigzag", "image", "luma",
};

/*
 * The two copies agree at q, or the C copy agrees with the shader's a hairline
 * away from it. Several fields jump - the clock at twelve o'clock, the curl
 * where its lip starts - and on the jump itself a last bit of rounding can put
 * the two copies on opposite sides. A copy that had drifted would not agree a
 * thousandth of a pixel away either.
 */
static bool fields_agree(const struct matte_shape_params *sp, float qx, float qy, float phase, float shader)
{
	static const float nudge[][2] = {{0.0f, 0.0f}, {1e-3f, 0.0f}, {-1e-3f, 0.0f}, {0.0f, 1e-3f}, {0.0f, -1e-3f}};

	for (const auto &n : nudge)
		if (close_enough(matte_field(sp, qx + n[0], qy + n[1], phase), shader, 1.0f))
			return true;
	return false;
}

static void check_matte_field(void)
{
	/* a lopsided silhouette, so the image shape is not just a circle */
	for (int i = 0; i < MATTE_PROFILE_SIZE; i++) {
		float a = (float)i / (float)MATTE_PROFILE_SIZE * 6.2831853f;

		profile[i] = 1.0f + 0.4f * sinf(3.0f * a) + 0.2f * cosf(7.0f * a);
	}

	for (int shape = 0; shape < MATTE_SHAPE_COUNT; shape++) {
		/* the luma wipe's field is the image itself, not a function of q */
		if (shape == MATTE_SHAPE_LUMA)
			continue;

		for (int trial = 0; trial < 40; trial++) {
			struct matte_shape_params sp = {};
			float phase = uniform(-2.0f, 2.0f);

			sp.shape = shape;
			sp.star_n = (float)(3 + rand() % 10);
			sp.star_k = matte_star_slope(sp.star_n, uniform(0.1f, 0.95f));
			sp.arms = (float)(1 + rand() % 12);
			sp.pitch = uniform(10.0f, 800.0f);
			sp.amp = uniform(0.0f, 300.0f);
			sp.wavelen = fmaxf(uniform(20.0f, 1500.0f), MATTE_CURL_MIN_LENGTH * sp.amp);
			sp.lobes = (float)(1 + rand() % 24);
			sp.bar_px = uniform(2.0f, 400.0f);
			sp.profile = profile;

			matte_effect::shape = shape;
			matte_effect::star_n = sp.star_n;
			matte_effect::star_k = sp.star_k;
			matte_effect::arms = sp.arms;
			matte_effect::pitch = sp.pitch;
			matte_effect::amp = sp.amp;
			matte_effect::wavelen = sp.wavelen;
			matte_effect::lobes = sp.lobes;
			matte_effect::bar_px = sp.bar_px;
			matte_effect::phase = phase;

			for (int i = 0; i < 400; i++) {
				float qx = uniform(-2500.0f, 2500.0f);
				float qy = uniform(-2500.0f, 2500.0f);
				float c = matte_field(&sp, qx, qy, phase);
				float s = matte_effect::matte_field(hlsl::float2(qx, qy));

				check(fields_agree(&sp, qx, qy, phase, s),
				      "%s field: C gives %.6f and the shader %.6f at q %.3f/%.3f, phase %.3f",
				      shape_names[shape], c, s, qx, qy, phase);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* the shard motion                                                   */
/* ------------------------------------------------------------------ */

static const char *const style_names[] = {"window", "blow", "wipe", "assemble"};

static void check_shard_motion(void)
{
	for (int style = GLASS_STYLE_WINDOW; style <= GLASS_STYLE_ASSEMBLE; style++) {
		for (int trial = 0; trial < 400; trial++) {
			struct glass_motion motion = {};
			struct glass_shard shard = {};
			struct glass_xform c;
			float angle = uniform(0.0f, 6.2831853f);
			float crack_end = uniform(0.02f, 0.8f);

			motion.style = style;
			motion.progress = uniform(0.0f, 1.0f);
			motion.hold_end = uniform(crack_end, 0.9f);
			motion.crack_front = glass_crack_front(motion.progress, crack_end, rand() % 3);
			motion.gravity_x = cosf(angle);
			motion.gravity_y = sinf(angle);
			motion.gravity = uniform(0.0f, 2.0f);
			motion.throw_px = uniform(0.0f, 2.0f);
			motion.spin = uniform(0.0f, 3.0f);
			motion.scatter = uniform(0.0f, 1.0f);
			motion.reach = uniform(100.0f, 3000.0f);
			motion.impact_x = uniform(-960.0f, 2880.0f);
			motion.impact_y = uniform(-540.0f, 1620.0f);
			motion.flat = rand() % 4 == 0;
			motion.arriving = style == GLASS_STYLE_ASSEMBLE && rand() % 2;

			shard.cx = uniform(0.0f, 1920.0f);
			shard.cy = uniform(0.0f, 1080.0f);
			shard.launch = uniform(0.0f, 1.0f);
			shard.rnd[0] = uniform(0.0f, 1.0f);
			shard.rnd[1] = uniform(0.0f, 1.0f);
			shard.rnd[2] = uniform(0.0f, 1.0f);

			glass_effect::progress = motion.progress;
			glass_effect::hold_end = motion.hold_end;
			glass_effect::crack_front = motion.crack_front;
			glass_effect::style = style;
			glass_effect::flat_shards = motion.flat;
			glass_effect::arriving = motion.arriving;
			glass_effect::impact_px = hlsl::float2(motion.impact_x, motion.impact_y);
			glass_effect::reach = motion.reach;
			glass_effect::gravity_dir = hlsl::float2(motion.gravity_x, motion.gravity_y);
			glass_effect::gravity = motion.gravity;
			glass_effect::throw_amt = motion.throw_px;
			glass_effect::spin = motion.spin;
			glass_effect::scatter = motion.scatter;

			glass_shard_xform(&motion, &shard, &c);
			glass_effect::ShardState s =
				glass_effect::shard_xform(hlsl::float2(shard.cx, shard.cy), shard.launch,
							  hlsl::float3(shard.rnd[0], shard.rnd[1], shard.rnd[2]));

			check(close_enough(c.ox, s.off.x, motion.reach) && close_enough(c.oy, s.off.y, motion.reach) &&
				      close_enough(c.oz, s.off.z, motion.reach),
			      "%s shard placed at %.4f/%.4f/%.4f in C and %.4f/%.4f/%.4f by the shader",
			      style_names[style], c.ox, c.oy, c.oz, s.off.x, s.off.y, s.off.z);
			check(close_enough(c.axis[0], s.axis.x, 1.0f) && close_enough(c.axis[1], s.axis.y, 1.0f) &&
				      close_enough(c.axis[2], s.axis.z, 1.0f) && close_enough(c.angle, s.angle, 1.0f),
			      "%s shard turned about %.4f/%.4f/%.4f by %.4f in C and %.4f/%.4f/%.4f by %.4f by the "
			      "shader",
			      style_names[style], c.axis[0], c.axis[1], c.axis[2], c.angle, s.axis.x, s.axis.y,
			      s.axis.z, s.angle);
			check(close_enough(c.alpha, s.alpha, 1.0f) && close_enough(c.crack, s.crack, 1.0f) &&
				      close_enough(c.reveal, s.reveal, 1.0f) && close_enough(c.settle, s.settle, 1.0f),
			      "%s shard alpha/crack/reveal/settle %.4f/%.4f/%.4f/%.4f in C and %.4f/%.4f/%.4f/%.4f "
			      "by the shader",
			      style_names[style], c.alpha, c.crack, c.reveal, c.settle, s.alpha, s.crack, s.reveal,
			      s.settle);
		}
	}
}

/* The constants each copy keeps for itself. The effects' are #defines, so
 * they are in scope here unqualified. */
static void check_constants(void)
{
	check(CROSS_THICKNESS == MATTE_CROSS_THICKNESS, "cross thickness differs");
	check(DIAMOND_TALL == MATTE_DIAMOND_TALL, "diamond height differs");
	check(CRACK_EDGE == GLASS_CRACK_EDGE, "crack edge differs");
}

int main(void)
{
	srand(20261031);

	check_constants();
	check_matte_field();
	check_shard_motion();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
