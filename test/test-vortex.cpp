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
 * Checks for the vortex transition.
 *
 * The noise, the phases and the palette are not restated here: this file
 * compiles data/effects/vortex_math.effect itself, through test/hlsl.hpp, and
 * the clamps and curves come from src/vortex-math.c, the same as the plugin.
 * What is checked is the claims the effect rests on:
 *
 *   1. the transition starts on an untouched outgoing scene and finishes on an
 *      untouched incoming one, for every pixel and every setting
 *   2. the portal is opaque edge to edge for the whole hold, so the scene swap
 *      in the middle of it can never show
 *   3. the angular noise wraps, so the spiral and the portal rim have no seam
 *      where the circle closes
 *   4. the reach covers the far corner, so an offset centre does not leave a
 *      corner of the outgoing scene showing when the scenes swap
 *   5. the work the shader skips is work that had nothing to contribute: where
 *      vortex_cover() settles a pixel from its radius alone, the answer is the
 *      one the noise would have given
 *   6. the core ramp still lands on the values the look was tuned at when both
 *      of its sliders are left at the default
 *   7. the body palette returns a colour the user picked - exactly the first one
 *      when the mix is off - and never invents one in between
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "hlsl.hpp"
#include "vortex-math.h"

/* ------------------------------------------------------------------ */
/* the effect                                                         */
/* ------------------------------------------------------------------ */

namespace effect {
using namespace hlsl;

/* The uniforms vortex_math.effect reads, set below the way the plugin sets
 * them. */
float2 canvas;
float2 origin_px;
float unit_px;
float reach;
float progress;
float open_end;
float reveal_start;
float cut;
float iris_k;
float reveal_k;
float glow;
float arms;
float twist;
float spin;
float detail;
float fringe;
float intensity;
float smoke_scale;
float smoke_soft;
float swirl_angle;
float swirl_pull;
float4 color_bright;
float4 color_deep;
float4 color_deep2;
float4 color_deep3;
float4 color_deep4;
float body_span;
int color_mix;
float core_level;
float core_width;

#include "vortex_math.effect"
} // namespace effect

using hlsl::float2;
using hlsl::float3;

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

/* Sets the phase uniforms for progress t, through the plugin's own clock. */
static void use_clock(float open_end, float reveal_start, float t)
{
	struct vortex_clock clock;

	vortex_clamp_phases(&open_end, &reveal_start);
	vortex_clock_at(open_end, reveal_start, 0.0f, 0.0f, t, &clock);

	effect::progress = t;
	effect::open_end = open_end;
	effect::reveal_start = reveal_start;
	effect::cut = clock.cut;
	effect::iris_k = clock.iris_k;
	effect::reveal_k = clock.reveal_k;
	effect::glow = clock.glow;
}

/* A point in field units, and what the shader works out from it first. */
struct point {
	float2 p;
	float r;
	float ang;
};

static struct point at(float x, float y)
{
	struct point pt;
	float2 pa;

	pt.p = float2(x, y);
	pa = pt.p + 0.00001f;
	pt.r = hlsl::length(pt.p);
	pt.ang = atan2f(pa.y, pa.x);
	return pt;
}

/* ------------------------------------------------------------------ */
/* checks                                                             */
/* ------------------------------------------------------------------ */

static const float roughness[] = {0.0f, 0.15f, 0.35f, 0.6f, 1.0f};
static const float reaches[] = {1.0f, 1.803f, 2.5f, 4.0f};

/* The portal's smallest and largest radius over a full turn at growth k. */
static void iris_extent(float k, float *under, float *over)
{
	*under = 1e30f;
	*over = -1e30f;

	for (int a = 0; a < 2048; a++) {
		float ang = -3.14159265f + 2.0f * 3.14159265f * (float)a / 2048.0f;
		float radius = effect::iris_radius(ang, k);

		*under = fminf(*under, radius);
		*over = fmaxf(*over, radius);
	}
}

/*
 * 1a. Nothing is covered at progress 0. The fringe is subtracted from an
 * oversized radius, so a rim spike must never push the boundary out past the
 * centre before the portal has started to open.
 *
 * 1b. Everything is covered by the time the portal has finished opening, which
 * is what lets the scenes be swapped without the cut showing. The check is run
 * against the reach, which is measured to the far corner, so it also covers an
 * offset centre.
 */
static void check_portal_ends(void)
{
	for (float reach : reaches) {
		for (float fringe : roughness) {
			float under, over;

			effect::reach = reach;
			effect::fringe = fringe;

			iris_extent(0.0f, &under, &over);
			check(over <= 0.0f, "portal already open at progress 0: reach %g roughness %g radius %g",
			      (double)reach, (double)fringe, (double)over);

			iris_extent(1.0f, &under, &over);
			check(under >= reach, "portal leaves a gap when open: reach %g roughness %g radius %g",
			      (double)reach, (double)fringe, (double)under);
		}
	}
}

/*
 * 2. The reveal runs from nothing to everything. At k = 0 the incoming scene is
 * entirely hidden, so the hold stays opaque right up to the moment the reveal
 * begins; at k = 1 none of the outgoing scene is left anywhere.
 */
static void check_reveal_ends(void)
{
	static const float softs[] = {0.05f, 0.3f, 0.8f};
	static const float scales[] = {0.5f, 2.5f, 8.0f};

	effect::reach = 2.5f;

	for (float soft : softs) {
		for (float scale : scales) {
			float lo_worst = 0.0f;
			float hi_worst = 1.0f;

			effect::smoke_soft = soft;
			effect::smoke_scale = scale;

			for (int y = 0; y < 96; y++) {
				for (int x = 0; x < 96; x++) {
					struct point pt =
						at(-2.0f + 4.0f * (float)x / 95.0f, -2.0f + 4.0f * (float)y / 95.0f);

					lo_worst = fmaxf(lo_worst, effect::reveal_amount(pt.p, pt.r, 0.0f));
					hi_worst = fminf(hi_worst, effect::reveal_amount(pt.p, pt.r, 1.0f));
				}
			}

			check(lo_worst == 0.0f, "reveal shows through before it starts: scale %g soft %g got %g",
			      (double)scale, (double)soft, (double)lo_worst);
			check(hi_worst == 1.0f, "reveal never finishes: scale %g soft %g got %g", (double)scale,
			      (double)soft, (double)hi_worst);
		}
	}
}

/*
 * 2b. The hold, as the shader's own coverage sees it: every pixel of the canvas
 * covered completely, from the frame the portal has finished opening to the
 * frame the reveal begins, so the swap at the cut cannot show.
 */
static void check_hold_is_opaque(void)
{
	static const float opens[] = {0.02f, 0.22f, 0.5f};
	static const float reveals[] = {0.1f, 0.62f, 0.95f};

	effect::unit_px = 540.0f;
	effect::reach = vortex_reach(-300.0f, 200.0f, 1920.0f, 1080.0f, effect::unit_px);
	effect::fringe = 0.6f;
	effect::smoke_soft = 0.3f;
	effect::smoke_scale = 4.0f;

	for (float open_end : opens) {
		for (float reveal_start : reveals) {
			float lo = open_end, hi = reveal_start;
			float least = 1.0f;

			vortex_clamp_phases(&lo, &hi);

			for (int i = 0; i <= 8; i++) {
				use_clock(open_end, reveal_start, lo + (hi - lo) * (float)i / 8.0f);

				for (int y = 0; y <= 24; y++) {
					for (int x = 0; x <= 24; x++) {
						float px = 1920.0f * (float)x / 24.0f, py = 1080.0f * (float)y / 24.0f;
						struct point pt = at((px + 300.0f) / effect::unit_px,
								     (py - 200.0f) / effect::unit_px);

						least = fminf(least, effect::vortex_cover(pt.p, pt.r, pt.ang).x);
					}
				}
			}

			check(least == 1.0f, "hold not opaque: open %g reveal %g least cover %g", (double)open_end,
			      (double)reveal_start, (double)least);
		}
	}
}

/*
 * 3. The angular noise wraps. Both the spiral arms and the portal rim sample it
 * on a coordinate that comes from an angle, so a lattice that did not close at
 * the period would draw a hard seam along the +x axis.
 */
static void check_noise_wraps(void)
{
	static const float periods[] = {1.0f, 3.0f, 5.0f, 16.0f, FIBRES};

	for (float per : periods) {
		float worst = 0.0f;

		for (int i = 0; i < 512; i++) {
			float x = per * (float)i / 512.0f;
			float d =
				fabsf(effect::fbm_p(float2(x, 3.7f), per) - effect::fbm_p(float2(x + per, 3.7f), per));

			worst = fmaxf(worst, d);
		}

		check(worst < 1e-4f, "angular noise does not wrap at period %g: worst gap %g", (double)per,
		      (double)worst);
	}
}

/*
 * 4. The phase clamps keep the hold non-empty however the two sliders are set,
 * including when the user drags the reveal below the portal. The scene swap
 * sits in the middle of the hold, so an empty hold would put the cut on a frame
 * where the vortex was not yet opaque.
 */
static void check_phase_schedule(void)
{
	for (int i = 0; i <= 20; i++) {
		for (int j = 0; j <= 20; j++) {
			float open_end = (float)i / 20.0f;
			float reveal_start = (float)j / 20.0f;
			struct vortex_clock clock;

			vortex_clamp_phases(&open_end, &reveal_start);
			vortex_clock_at(open_end, reveal_start, 0.0f, 0.0f, 0.5f, &clock);

			check(reveal_start - open_end >= VORTEX_PHASE_GAP - 1e-6f, "hold collapsed: open %g reveal %g",
			      (double)open_end, (double)reveal_start);
			check(clock.cut > open_end && clock.cut < reveal_start,
			      "scene swap outside the hold: cut %g in (%g, %g)", (double)clock.cut, (double)open_end,
			      (double)reveal_start);
			check(open_end > 0.0f && reveal_start < 1.0f,
			      "phases reach the ends of the transition: open %g reveal %g", (double)open_end,
			      (double)reveal_start);
		}
	}
}

/*
 * 5. The reach covers the far corner from wherever the centre has been pushed,
 * including well outside the frame, so the portal never finishes opening while
 * a corner of the outgoing scene is still visible.
 */
static void check_reach_covers_canvas(void)
{
	static const float offsets[] = {-0.5f, 0.0f, 0.25f, 0.5f, 1.0f, 1.5f};
	const float cx = 1920.0f;
	const float cy = 1080.0f;
	const float unit_px = cy * 0.5f;

	for (float oy : offsets) {
		for (float ox : offsets) {
			float origin_x = ox * cx;
			float origin_y = oy * cy;
			float reach = vortex_reach(origin_x, origin_y, cx, cy, unit_px);
			float worst = 0.0f;

			for (int y = 0; y <= 32; y++) {
				for (int x = 0; x <= 32; x++) {
					float dx = (cx * (float)x / 32.0f - origin_x) / unit_px;
					float dy = (cy * (float)y / 32.0f - origin_y) / unit_px;

					worst = fmaxf(worst, sqrtf(dx * dx + dy * dy));
				}
			}

			check(worst <= reach + 1e-4f, "canvas reaches past the portal: origin %g,%g r %g reach %g",
			      (double)ox, (double)oy, (double)worst, (double)reach);
		}
	}
}

/*
 * 6. The portal boundary stays inside the band the shader's radius test
 * assumes. iris_edge() is a saturate(), so the noise can only ever eat into an
 * oversized radius, and the boundary is therefore between k * reach and
 * k * reach * (1 + fringe) at every angle.
 */
static void check_iris_bounds(void)
{
	static const float ks[] = {0.05f, 0.25f, 0.5f, 0.8f, 1.0f};

	for (float reach : reaches) {
		for (float fringe : roughness) {
			effect::reach = reach;
			effect::fringe = fringe;

			for (float k : ks) {
				float lo = k * reach;
				float hi = lo * (1.0f + fringe);
				float under, over;

				iris_extent(k, &under, &over);

				check(under >= lo - 1e-4f,
				      "portal boundary below its bound: k %g reach %g roughness %g got %g want >= %g",
				      (double)k, (double)reach, (double)fringe, (double)under, (double)lo);
				check(over <= hi + 1e-4f,
				      "portal boundary past its bound: k %g reach %g roughness %g got %g want <= %g",
				      (double)k, (double)reach, (double)fringe, (double)over, (double)hi);
			}
		}
	}
}

/*
 * 7. The work vortex_cover() skips had nothing to contribute. Opening, a pixel
 * past the portal's widest reach is settled as uncovered without the rim noise;
 * revealing, the noise term is bounded, so the inner disc is settled as fully
 * back and the outer ring as untouched without the smoke. Each is checked
 * against the full computation on every pixel, and the skips have to fire on
 * some pixels or the check would be passing on an empty set.
 */
static void check_skips_are_sound(void)
{
	static const float softs[] = {0.05f, 0.3f, 0.8f};
	static const float scales[] = {0.5f, 2.5f, 8.0f};
	static const float ks[] = {0.0f, 0.15f, 0.35f, 0.5f, 0.7f, 0.9f, 1.0f};
	long skipped = 0;
	long evaluated = 0;

	effect::unit_px = 540.0f;
	effect::reach = 2.5f;
	effect::fringe = 0.6f;

	for (float soft : softs) {
		for (float scale : scales) {
			effect::smoke_soft = soft;
			effect::smoke_scale = scale;

			for (float k : ks) {
				/* the same k, once opening and once revealing; a
				 * portal fully open is the hold, not the opening */
				for (int phase = 0; phase < 2; phase++) {
					if (phase == 0 && k >= 1.0f)
						continue;
					if (phase == 0)
						use_clock(0.5f, 0.6f, 0.5f * k);
					else
						use_clock(0.2f, 0.3f, 0.3f + 0.7f * k);

					for (int y = 0; y < 64; y++) {
						for (int x = 0; x < 64; x++) {
							struct point pt = at(-2.5f + 5.0f * (float)x / 63.0f,
									     -2.5f + 5.0f * (float)y / 63.0f);
							float got = effect::vortex_cover(pt.p, pt.r, pt.ang).x;
							float aa = 1.5f / effect::unit_px;
							float want;

							if (phase == 0)
								want = hlsl::saturate(
									(effect::iris_radius(pt.ang, effect::iris_k) -
									 pt.r) / aa +
									0.5f);
							else
								want = 1.0f - effect::reveal_amount(pt.p, pt.r,
												    effect::reveal_k);

							check(got == want,
							      "vortex_cover() skipped a pixel it got wrong: %s k %g soft %g "
							      "r %g got %g want %g",
							      phase == 0 ? "opening" : "revealing", (double)k,
							      (double)soft, (double)pt.r, (double)got, (double)want);

							/* a skipped pixel is one that is wholly settled */
							if (got == 0.0f || got == 1.0f)
								skipped++;
							else
								evaluated++;
						}
					}
				}
			}
		}
	}

	check(skipped > 0, "no pixel was ever settled from its radius");
	check(evaluated > 0, "no pixel was ever left to the noise");
}

/*
 * 8. The light envelope, which the host works out once a frame instead of the
 * shader working it out per pixel. Nothing may light up before the portal has
 * opened at all, the peak has to sit where the portal fills the canvas, and the
 * whole curve has to stay inside 0..1 so intensity means what it says.
 */
static void check_glow_envelope(void)
{
	static const float opens[] = {0.02f, 0.22f, 0.5f, 0.9f};

	for (float open_end : opens) {
		struct vortex_clock clock;
		float peak;

		vortex_clock_at(open_end, 0.95f, 0.0f, 0.0f, open_end, &clock);
		peak = clock.glow;

		vortex_clock_at(open_end, 0.95f, 0.0f, 0.0f, 0.0f, &clock);
		check(clock.glow == 0.0f, "vortex lit at progress 0: open %g got %g", (double)open_end,
		      (double)clock.glow);

		for (int i = 0; i <= 200; i++) {
			float progress = (float)i / 200.0f;

			vortex_clock_at(open_end, 0.95f, 0.0f, 0.0f, progress, &clock);
			check(clock.glow >= 0.0f && clock.glow <= 1.0f + 1e-6f,
			      "glow left 0..1: open %g progress %g got %g", (double)open_end, (double)progress,
			      (double)clock.glow);
			check(clock.glow <= peak + 1e-6f,
			      "glow peaks away from the open: open %g progress %g got %g over %g", (double)open_end,
			      (double)progress, (double)clock.glow, (double)peak);
		}
	}
}

/*
 * 9. The swirl is identity at both ends, so the first and last frames are the
 * scenes untouched whatever the swirl is set to.
 */
static void check_swirl_ends(void)
{
	static const float warps[] = {0.0f, 1.8f, 6.0f};

	for (float warp : warps) {
		struct vortex_clock clock;

		vortex_clock_at(0.22f, 0.62f, warp, warp * 0.1f, 0.0f, &clock);
		check(clock.swirl_angle == 0.0f && clock.swirl_pull == 1.0f, "swirl %g moves the first frame: %g %g",
		      (double)warp, (double)clock.swirl_angle, (double)clock.swirl_pull);

		vortex_clock_at(0.22f, 0.62f, warp, warp * 0.1f, 1.0f, &clock);
		check(clock.swirl_angle == 0.0f && clock.swirl_pull == 1.0f, "swirl %g moves the last frame: %g %g",
		      (double)warp, (double)clock.swirl_angle, (double)clock.swirl_pull);
	}
}

/*
 * 10. The core ramp. Both sliders are geometric about the values the effect was
 * originally tuned at, so the middle of each has to reproduce them exactly or
 * every existing scene collection would come back looking different. More bleed
 * has to mean a lower threshold, and the width can never reach zero, which the
 * shader divides by.
 */
static void check_core_ramp(void)
{
	float previous = 1e30f;

	check(fabsf(vortex_core_level(0.5f) - VORTEX_CORE_LEVEL) < 1e-5f, "core level default moved: got %g want %g",
	      (double)vortex_core_level(0.5f), (double)VORTEX_CORE_LEVEL);
	check(fabsf(vortex_core_width(0.5f, 0.5f) - VORTEX_CORE_WIDTH) < 1e-5f,
	      "core width default moved: got %g want %g", (double)vortex_core_width(0.5f, 0.5f),
	      (double)VORTEX_CORE_WIDTH);

	for (int i = 0; i <= 100; i++) {
		float bleed = (float)i / 100.0f;
		float level = vortex_core_level(bleed);

		check(level > 0.0f, "core level not positive: bleed %g got %g", (double)bleed, (double)level);
		check(level < previous, "core level not falling with bleed: bleed %g got %g after %g", (double)bleed,
		      (double)level, (double)previous);
		previous = level;

		for (int j = 0; j <= 10; j++) {
			float blend = (float)j / 10.0f;
			float width = vortex_core_width(bleed, blend);

			check(width >= 0.0001f, "core width collapsed: bleed %g blend %g got %g", (double)bleed,
			      (double)blend, (double)width);
		}
	}
}

/*
 * 11. The body palette. A single colour has to come back bit for bit unchanged,
 * because that is the whole of the previous behaviour; a whole palette position
 * has to be the colour at that slot rather than something near it; and anything
 * in between has to stay inside the two entries it sits between, so a mix can
 * never invent a colour the user did not pick.
 */
static void check_palette(void)
{
	static const float pal[VORTEX_BODY_COLORS][3] = {
		{0.48f, 0.25f, 0.82f},
		{0.25f, 0.71f, 0.82f},
		{0.82f, 0.25f, 0.62f},
		{0.25f, 0.35f, 0.82f},
	};
	hlsl::float4 *slots[VORTEX_BODY_COLORS] = {&effect::color_deep, &effect::color_deep2, &effect::color_deep3,
						   &effect::color_deep4};

	for (int i = 0; i < VORTEX_BODY_COLORS; i++)
		*slots[i] = hlsl::float4(pal[i][0], pal[i][1], pal[i][2], 1.0f);

	for (int i = 0; i < VORTEX_BODY_COLORS; i++) {
		float3 out = effect::body_colour((float)i);

		for (int c = 0; c < 3; c++)
			check(out.v[c] == pal[i][c], "palette entry %d is not itself: channel %d got %g want %g", i, c,
			      (double)out.v[c], (double)pal[i][c]);
	}

	for (int i = 0; i + 1 < VORTEX_BODY_COLORS; i++) {
		for (int j = 0; j <= 32; j++) {
			float t = (float)i + (float)j / 32.0f;
			float3 out = effect::body_colour(t);

			for (int c = 0; c < 3; c++) {
				float lo = fminf(pal[i][c], pal[i + 1][c]);
				float hi = fmaxf(pal[i][c], pal[i + 1][c]);

				check(out.v[c] >= lo - 1e-6f && out.v[c] <= hi + 1e-6f,
				      "palette left its segment: t %g channel %d got %g not in [%g, %g]", (double)t, c,
				      (double)out.v[c], (double)lo, (double)hi);
			}
		}
	}

	/* a single colour: no selector reaches past the first entry, whatever
	 * the mix or the noise */
	effect::body_span = 0.0f;
	effect::reach = 2.5f;
	effect::arms = 5.0f;
	for (int mix = VORTEX_MIX_ARMS; mix <= VORTEX_MIX_BANDS; mix++) {
		effect::color_mix = mix;
		for (int i = 0; i < 64; i++) {
			float2 q((float)i * 0.37f, (float)i * -0.53f);
			float3 out = effect::body_colour(effect::body_mix(q, (float)i * 0.05f));

			for (int c = 0; c < 3; c++)
				check(out.v[c] == pal[0][c],
				      "single colour body drifted: mix %d channel %d got %g want %g", mix, c,
				      (double)out.v[c], (double)pal[0][c]);
		}
	}
}

/*
 * 12. The mix selectors. The gain has to stay inside 0..1 so it cannot index
 * past the palette, and the banded snap has to land exactly on whole entries,
 * stay inside the span, and never run backwards - a snap that overshot would
 * put a colour the band does not belong to along its edge.
 */
static void check_mix_selectors(void)
{
	const float span = 3.0f;
	float previous = 0.0f;

	for (int i = 0; i <= 200; i++) {
		float n = -0.5f + 2.0f * (float)i / 200.0f;
		float g = effect::mix_gain(n);

		check(g >= 0.0f && g <= 1.0f, "mix selector left 0..1: n %g got %g", (double)n, (double)g);
	}

	effect::color_mix = VORTEX_MIX_BANDS;
	effect::body_span = span;

	for (int i = 0; i <= 3; i++) {
		float t = effect::body_position((float)i / span);

		check(t == (float)i, "banded mix does not land on entry %d: got %g", i, (double)t);
	}

	for (int i = 0; i <= 600; i++) {
		float m = (float)i / 600.0f;
		float snapped = effect::body_position(m);

		check(snapped >= 0.0f && snapped <= span, "banded mix left the palette: m %g got %g", (double)m,
		      (double)snapped);
		check(snapped >= previous - 1e-6f, "banded mix ran backwards: m %g got %g after %g", (double)m,
		      (double)snapped, (double)previous);
		previous = snapped;
	}
}

int main(void)
{
	check_portal_ends();
	check_reveal_ends();
	check_hold_is_opaque();
	check_noise_wraps();
	check_phase_schedule();
	check_reach_covers_canvas();
	check_iris_bounds();
	check_skips_are_sound();
	check_glow_envelope();
	check_swirl_ends();
	check_core_ramp();
	check_palette();
	check_mix_selectors();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
