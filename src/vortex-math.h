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
 * The parts of the vortex transition worked out on the CPU, with no dependency
 * on libobs or on a graphics device: the phase schedule, the curves that are
 * the same for every pixel in a frame, the core ramp and the reach. The plugin
 * builds its uniforms from these, and the tests use the same functions.
 *
 * The per pixel half is in data/effects/vortex_math.effect.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The portal has to finish opening before the reveal starts, or the scenes
 * would be swapped while a corner of the outgoing one was still showing. */
#define VORTEX_PHASE_GAP 0.05f

/* How many colours the body of the vortex can mix between. The shader blends
 * them with a fixed chain of lerps, so this is a shader constant as much as a
 * property range - raising it means adding a link there too. */
#define VORTEX_BODY_COLORS 4

/*
 * Core Bleed and Core Blend are geometric about the values the look was
 * originally tuned at, so both sliders reproduce it exactly at 50% and either
 * direction has the same amount of travel on a ratio scale, which is how these
 * read on screen.
 *
 * The width is a fraction of the level rather than an absolute range: it is the
 * softness of the body-to-core edge, and an edge that stayed the same width in
 * energy while the level moved would harden as the core spread.
 */
#define VORTEX_CORE_LEVEL 1.1f
#define VORTEX_CORE_WIDTH 0.9f
#define VORTEX_CORE_LEVEL_RANGE 3.6f
#define VORTEX_CORE_WIDTH_RANGE 4.0f

/* The effect carries this as an integer, so the numbering is part of the
 * contract with it. */
enum vortex_color_mix {
	VORTEX_MIX_ARMS = 0,
	VORTEX_MIX_RADIAL = 1,
	VORTEX_MIX_BANDS = 2,
};

/* Keeps the hold between the portal opening and the reveal from ever closing,
 * however the two sliders are set. Both are shares of the transition. */
void vortex_clamp_phases(float *open_end, float *reveal_start);

/* The core ramp for Core Bleed and Core Blend, each 0..1. More bleed is a
 * lower threshold: the core colour starts taking over sooner, so it reaches
 * further down the arms. */
float vortex_core_level(float bleed);
float vortex_core_width(float bleed, float blend);

/*
 * Field units from the origin to the furthest canvas corner, a field unit being
 * unit_px pixels. The portal is scaled by it, so an origin pushed off to one
 * side still has the portal cover the whole canvas by the time it has finished
 * opening rather than leaving the far corner showing when the scenes swap.
 */
float vortex_reach(float origin_x, float origin_y, float cx, float cy, float unit_px);

/* Everything about a frame that is the same for every pixel in it, worked out
 * once rather than a couple of million times in the shader. */
struct vortex_clock {
	float cut;         /* progress at which the scenes are swapped */
	float iris_k;      /* portal growth, 0..1 */
	float reveal_k;    /* reveal progress, 0..1 */
	float glow;        /* brightness envelope */
	float swirl_angle; /* scene twist, radians */
	float swirl_pull;  /* the zoom that goes with it; 1 is none */
};

/* warp is the swirl in radians at its strongest and warp_pull the zoom that
 * goes with it; both 0 when the swirl is off. */
void vortex_clock_at(float open_end, float reveal_start, float warp, float warp_pull, float t,
		     struct vortex_clock *clock);

#ifdef __cplusplus
}
#endif
