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

#include <math.h>

#include "vortex-math.h"

static inline float clamp_unit(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void vortex_clamp_phases(float *open_end, float *reveal_start)
{
	*open_end = fminf(fmaxf(*open_end, 0.02f), 1.0f - 2.0f * VORTEX_PHASE_GAP);
	*reveal_start = fminf(fmaxf(*reveal_start, *open_end + VORTEX_PHASE_GAP), 1.0f - VORTEX_PHASE_GAP);
}

float vortex_core_level(float bleed)
{
	return VORTEX_CORE_LEVEL * powf(VORTEX_CORE_LEVEL_RANGE, 1.0f - 2.0f * bleed);
}

float vortex_core_width(float bleed, float blend)
{
	float width = vortex_core_level(bleed) * (VORTEX_CORE_WIDTH / VORTEX_CORE_LEVEL) *
		      powf(VORTEX_CORE_WIDTH_RANGE, 2.0f * blend - 1.0f);

	/* the shader divides by it */
	return fmaxf(width, 0.0001f);
}

float vortex_reach(float origin_x, float origin_y, float cx, float cy, float unit_px)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float max_radius = 0.0f;
	int i;

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - origin_x;
		float dy = corner_y[i] - origin_y;
		float radius = sqrtf(dx * dx + dy * dy);

		if (radius > max_radius)
			max_radius = radius;
	}

	return fmaxf(max_radius / unit_px, 0.0001f);
}

/*
 * Brightness envelope over the transition. The light ramps up as the portal
 * opens, peaks as it fills the canvas, then decays across the hold and the
 * reveal so that what is left carving up the incoming scene is dark smoke
 * rather than a bright spiral. The rise is the portal clock itself.
 */
static float vortex_glow(float rise, float progress, float open_end)
{
	float fall = 1.0f - clamp_unit((progress - open_end) / fmaxf(1.0f - open_end, 0.0001f));

	/* written the way the shader's lerp(0.06, 1.0, x) expands, so moving the
	 * curve off the GPU did not move the curve */
	return rise * rise * (0.06f + (1.0f - 0.06f) * powf(fall, 1.8f));
}

void vortex_clock_at(float open_end, float reveal_start, float warp, float warp_pull, float t,
		     struct vortex_clock *clock)
{
	/* The scenes are swapped halfway through the hold, where the overlay is
	 * opaque from edge to edge and the cut cannot show. */
	clock->cut = (open_end + reveal_start) * 0.5f;

	clock->iris_k = clamp_unit(t / fmaxf(open_end, 0.0001f));
	clock->reveal_k = clamp_unit((t - reveal_start) / fmaxf(1.0f - reveal_start, 0.0001f));
	clock->glow = vortex_glow(clock->iris_k, t, open_end);

	/* The swirl for whichever scene is showing this frame. Both ends of the
	 * transition are identity: at progress 0 and 1 the travel is zero, so
	 * the angle is zero and the shader leaves the scene coordinate alone. */
	if (warp <= 0.0f) {
		clock->swirl_angle = 0.0f;
		clock->swirl_pull = 1.0f;
	} else {
		float travel;

		if (t < clock->cut)
			travel = clamp_unit(t / fmaxf(clock->cut, 0.0001f));
		else
			travel = clamp_unit((1.0f - t) / fmaxf(1.0f - clock->cut, 0.0001f));

		/* the outgoing scene screws in, the incoming one unwinds back out */
		clock->swirl_angle = warp * powf(travel, 1.5f) * (t < clock->cut ? 1.0f : -1.0f);
		clock->swirl_pull = 1.0f + warp_pull * travel;
	}
}
