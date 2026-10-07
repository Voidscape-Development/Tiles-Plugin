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

#include "portal-math.h"

#include <math.h>

static inline float saturatef_(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float smooth01(float x)
{
	x = saturatef_(x);
	return x * x * (3.0f - 2.0f * x);
}

/*
 * The shader's distance is (rho - radius) divided by the length of rho's
 * gradient, which on an oval lies between min(1, 1/aspect) and
 * max(1, 1/aspect). A corner is at least margin pixels inside the oval once
 * radius - rho reaches margin times the larger of the two, so that is what the
 * corner's rho is padded by.
 */
float portal_cover_radius(const struct portal_params *pp, float ox, float oy, float cx, float cy)
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float aspect = fmaxf(pp->aspect, 0.05f);
	float margin = PORTAL_RIM_SPAN * pp->rim_px + 2.0f;
	float far_rho = 0.0f;
	int i;

	margin = fmaxf(margin, PORTAL_INNER_RIMS * pp->rim_px + 2.0f);

	for (i = 0; i < 4; i++) {
		float dx = (corner_x[i] - ox) / aspect;
		float dy = corner_y[i] - oy;

		far_rho = fmaxf(far_rho, sqrtf(dx * dx + dy * dy));
	}

	return far_rho + margin * fmaxf(1.0f, 1.0f / aspect);
}

void portal_frame_at(const struct portal_params *pp, float cover, float t, struct portal_frame *f)
{
	float open_end = fmaxf(pp->open_end, 0.001f);

	f->radius = 0.0f;
	f->size_k = 0.0f;
	f->energy = 0.0f;
	f->grow = 0.0f;
	f->spark_env = 0.0f;

	if (t <= 0.0f)
		return;

	if (t >= 1.0f) {
		f->radius = cover;
		f->size_k = 1.0f;
		f->grow = 1.0f;
		return;
	}

	if (t < pp->open_end) {
		/* shot open: a back-out ease, so it pops slightly past its size
		 * and settles back onto it */
		float o = t / open_end;
		float y = o - 1.0f;

		f->radius = pp->settled * (1.0f + (PORTAL_POP + 1.0f) * y * y * y + PORTAL_POP * y * y);

		/* The energy fills the oval as it opens and clears from the
		 * middle outwards, finishing exactly as the pop does. */
		if (pp->spiral_open)
			f->energy = 1.0f - smooth01((o - 0.2f) / 0.8f);
	} else if (t < pp->hold_end) {
		f->radius = pp->settled;
	} else {
		f->grow = saturatef_((t - pp->hold_end) / fmaxf(1.0f - pp->hold_end, 0.001f));
		f->radius = pp->settled + (cover - pp->settled) * smooth01(f->grow);
	}

	f->radius = fmaxf(f->radius, 0.0f);
	f->size_k = saturatef_(f->radius / fmaxf(pp->settled, 0.001f));

	/* The sparks come off the rim while it is there to throw them, and die
	 * away as the portal swallows the canvas. */
	f->spark_env = f->size_k * (1.0f - f->grow) * (1.0f - f->grow);
}
