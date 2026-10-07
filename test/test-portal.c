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
 * Checks for the portal transition's schedule, linked against
 * src/portal-math.c.
 *
 *   1. the first frame has no portal at all, and the last has one so large that
 *      every pixel of the canvas takes the shader's early exit to the untouched
 *      incoming scene - for any centre, on the canvas or off it, any width and
 *      any rim
 *   2. the size is continuous across the phase boundaries: the pop lands on the
 *      settled size, the hold stays there, and the grow starts from it and only
 *      ever grows
 *   3. the pop overshoots, but only a little
 *   4. the energy inside fills and clears within the opening, and never shows
 *      at all with the spiral turned off
 *   5. the sparks are gone by the last frame and every envelope stays in 0..1
 *
 * The distance below mirrors PSPortal() in data/effects/portal_transition.effect,
 * which the GPU check in test/portal-check.c renders for real.
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "portal-math.h"

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

/* mirror of the signed edge distance in PSPortal() */
static float edge_distance(float px, float py, float aspect, float radius)
{
	float qx = px / aspect, qy = py;
	float rho = sqrtf(qx * qx + qy * qy);
	float gx = qx / aspect, gy = qy;
	float grad = sqrtf(gx * gx + gy * gy) / fmaxf(rho, 0.0001f);

	return (rho - radius) / fmaxf(grad, 0.0001f);
}

/* mirror of the early exit in PSPortal(): past this far inside, nothing the
 * edge draws reaches */
static float inside_reach(float rim)
{
	return fmaxf(PORTAL_RIM_SPAN * rim + 1.0f, PORTAL_INNER_RIMS * rim) + 1.0f;
}

static void check_ends(void)
{
	const float cx = 1280.0f, cy = 720.0f;
	const float origins[][2] = {{0.5f, 0.5f}, {0.0f, 0.0f}, {0.2f, 0.9f}, {-0.5f, 1.5f}, {1.5f, -0.5f}};
	const float aspects[] = {0.2f, 0.62f, 1.0f, 2.0f};
	const float rims[] = {1.0f, 14.0f, 60.0f};
	const float sizes[] = {0.1f, 0.55f, 1.5f};

	for (size_t o = 0; o < sizeof(origins) / sizeof(origins[0]); o++)
		for (size_t a = 0; a < sizeof(aspects) / sizeof(aspects[0]); a++)
			for (size_t r = 0; r < sizeof(rims) / sizeof(rims[0]); r++)
				for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
					struct portal_params pp = {
						aspects[a], 0.5f * sizes[s] * cy, 0.2f, 0.5f, rims[r], true};
					float ox = origins[o][0] * cx, oy = origins[o][1] * cy;
					float cover = portal_cover_radius(&pp, ox, oy, cx, cy);
					struct portal_frame f0, f1;
					float worst = -INFINITY;
					int x, y;

					portal_frame_at(&pp, cover, 0.0f, &f0);
					portal_frame_at(&pp, cover, 1.0f, &f1);

					check(f0.radius == 0.0f && f0.energy == 0.0f && f0.spark_env == 0.0f,
					      "origin %zu aspect %zu rim %zu size %zu: something shows on the first frame",
					      o, a, r, s);
					check(f1.radius == cover && f1.energy == 0.0f && f1.spark_env == 0.0f &&
						      f1.size_k == 1.0f,
					      "origin %zu aspect %zu rim %zu size %zu: the last frame is not the cover",
					      o, a, r, s);

					/* every pixel well inside the oval on the last frame */
					for (y = 0; y < (int)cy; y += (y == 0 || y >= (int)cy - 8) ? 1 : 8)
						for (x = 0; x < (int)cx; x += (y == 0 || y >= (int)cy - 8) ? 1 : 8) {
							float d = edge_distance(x + 0.5f - ox, y + 0.5f - oy, pp.aspect,
										f1.radius);

							worst = fmaxf(worst, d);
						}
					for (y = 0; y < (int)cy; y++) {
						worst = fmaxf(worst, edge_distance(0.5f - ox, y + 0.5f - oy, pp.aspect,
										   f1.radius));
						worst = fmaxf(worst, edge_distance(cx - 0.5f - ox, y + 0.5f - oy,
										   pp.aspect, f1.radius));
					}

					check(-worst > inside_reach(pp.rim_px * f1.size_k),
					      "origin %zu aspect %zu rim %zu size %zu: a pixel is only %.2f inside the "
					      "portal on the last frame (needs %.2f)",
					      o, a, r, s, -worst, inside_reach(pp.rim_px));
				}
}

static void check_schedule(void)
{
	const float opens[] = {0.02f, 0.2f, 0.5f};
	const float holds[] = {0.0f, 0.25f, 0.4f};

	for (size_t i = 0; i < 3; i++)
		for (size_t j = 0; j < 3; j++)
			for (int spiral = 0; spiral < 2; spiral++) {
				struct portal_params pp = {0.62f, 200.0f,     opens[i], opens[i] + holds[j],
							   14.0f, spiral != 0};
				float cover = 1500.0f;
				struct portal_frame f, prev = {0};
				float peak = 0.0f;
				int k;

				for (k = 1; k <= 4000; k++) {
					float t = (float)k / 4000.0f;

					portal_frame_at(&pp, cover, t, &f);

					check(f.size_k >= 0.0f && f.size_k <= 1.0f && f.energy >= 0.0f &&
						      f.energy <= 1.0f && f.spark_env >= 0.0f && f.spark_env <= 1.0f,
					      "open %zu hold %zu: an envelope leaves 0..1 at %.4f", i, j, t);

					if (t < pp.open_end)
						peak = fmaxf(peak, f.radius);
					else
						check(f.energy == 0.0f, "open %zu hold %zu: energy left after opening",
						      i, j);

					if (!spiral)
						check(f.energy == 0.0f, "open %zu hold %zu: energy with no spiral", i,
						      j);

					if (t >= pp.open_end && t < pp.hold_end)
						check(f.radius == pp.settled, "open %zu hold %zu: the hold moves", i,
						      j);

					if (t > pp.hold_end && prev.radius > 0.0f && k > 1)
						check(f.radius >= prev.radius - 1e-3f,
						      "open %zu hold %zu: the grow shrinks at %.4f", i, j, t);

					/* nothing jumps: the steps here are 1/4000 of the
					 * transition, so a continuous size moves little */
					if (k > 1)
						check(fabsf(f.radius - prev.radius) < 0.02f * cover,
						      "open %zu hold %zu: the size jumps at %.4f (%.1f -> %.1f)", i, j,
						      t, prev.radius, f.radius);

					prev = f;
				}

				check(peak > pp.settled * 1.03f && peak < pp.settled * 1.15f,
				      "open %zu hold %zu: the pop peaks at %.3f of the settled size", i, j,
				      peak / pp.settled);

				/* the energy really is there at the start of a spiral open */
				portal_frame_at(&pp, cover, 0.1f * pp.open_end, &f);
				if (spiral)
					check(f.energy > 0.9f, "open %zu hold %zu: the spiral starts clear", i, j);
			}
}

int main(void)
{
	check_ends();
	check_schedule();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
