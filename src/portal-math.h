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
 * The clock and the sizing behind the portal transition, with no dependency on
 * libobs or on a graphics device, so the plugin, the tests and the GPU check
 * all run the same schedule.
 *
 * The portal is an upright oval measured in "rho" units: rho = |(x / aspect, y)|
 * from its centre, so a rho of R is the oval with a half height of R pixels and
 * a half width of aspect * R. Everything the shader draws hangs off the signed
 * pixel distance from that oval's edge, positive outside.
 */

#pragma once

#include <stdbool.h>

/* How far past the canvas the portal finishes, beyond the furthest corner, as
 * a share of the rim width: the rim, the light trailing inside it and the lens
 * bulge all have to be off the canvas on the last frame. */
#define PORTAL_RIM_SPAN 2.7f

/* The inside glow and the lens bulge reach this many rim widths in from the
 * edge. */
#define PORTAL_INNER_RIMS 4.0f

/* The pop when the portal is shot open: the pull of a back-out ease, which at
 * this strength overshoots the settled size by about 7% before it lands. */
#define PORTAL_POP 1.4f

struct portal_params {
	float aspect;     /* oval width over height */
	float settled;    /* settled half height, px */
	float open_end;   /* 0..1: the portal has popped open by here */
	float hold_end;   /* 0..1: it starts growing to fill the canvas here */
	float rim_px;     /* rim width at the settled size */
	bool spiral_open; /* the inside swirls with energy until it clears */
};

/* Half height of the oval that just covers a cx by cy canvas from (ox, oy) with
 * the rim, the inner light and the lens all off the canvas. */
float portal_cover_radius(const struct portal_params *pp, float ox, float oy, float cx, float cy);

/* What one frame needs. */
struct portal_frame {
	float radius;    /* half height of the oval, px; 0 is no portal at all */
	float size_k;    /* 0..1, how far the portal has opened towards its settled size */
	float energy;    /* 0..1, how much of the inside is still swirling energy */
	float grow;      /* 0..1 through the final grow */
	float spark_env; /* 0..1, how many sparks are flying */
};

void portal_frame_at(const struct portal_params *pp, float cover, float t, struct portal_frame *f);
