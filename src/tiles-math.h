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
 * The parts of the tiling transition worked out on the CPU, once a frame, with
 * no dependency on libobs or on a graphics device: how far the sweep has to
 * reach and how wide the coverage ramp is. The plugin builds its uniforms from
 * these, and the tests and the GPU check use the same functions rather than
 * copies.
 *
 * The per pixel half - the lattice, the sweep order and the tile clock - is in
 * data/effects/tiles_math.effect.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The effect carries these as integers, so the numbering is part of the
 * contract with it. */
enum tiles_mode {
	TILES_MODE_COVER = 0,
	TILES_MODE_KEYHOLE = 1,
	TILES_MODE_WAVE = 2,
	TILES_MODE_CROP_SHRINK = 3,
	TILES_MODE_SCALE_SHRINK = 4,
};

enum tiles_shape {
	TILES_SHAPE_SQUARE = 0,
	TILES_SHAPE_CIRCLE = 1,
	TILES_SHAPE_HEXAGON = 2,
	TILES_SHAPE_TRIANGLE = 3,
	TILES_SHAPE_DIAMOND = 4,
	TILES_SHAPE_BRICK = 5,
	TILES_SHAPE_OCTAGON = 6,
	TILES_SHAPE_CAIRO = 7,
	TILES_SHAPE_MOSAIC = 8,
	TILES_SHAPE_SHATTER = 9,
};

#define TILES_SHAPE_COUNT 10

enum tiles_direction {
	TILES_DIR_INSIDE_OUT = 0,
	TILES_DIR_OUTSIDE_IN = 1,
	TILES_DIR_MIRRORED_IN = 2,
	TILES_DIR_MIRRORED_OUT = 3,
	TILES_DIR_ANGLE = 4,
};

#define TILES_DIRECTION_COUNT 5

enum tiles_easing {
	TILES_EASE_LINEAR = 0,
	TILES_EASE_IN_OUT = 1,
	TILES_EASE_OUT = 2,
};

/* Distance from a tile centre to the furthest pixel it owns, in pixels. */
float tiles_circumradius(int shape, float tile_px);

/* The coverage ramp's width in metric units, so it comes out about a pixel
 * wide on screen. A tiling that mixes tile sizes widens it per cell in the
 * effect - see Cell.aa. */
float tiles_edge_width(int shape, float tile_px);

/*
 * The sweep axis and how far the sweep has to run, for the effect's dir_vec and
 * dir_range: x is where it starts and y how far it goes, in pixels along the
 * axis for a directional sweep and out from the origin for the others.
 *
 * The extent is measured to the far canvas corner as seen from the origin, so
 * every tile still starts and finishes inside the transition when the origin
 * is offset or pushed outside the frame. It is padded by the circumradius
 * because a tile straddling the canvas edge has its centre outside the frame,
 * and it still has to take its turn.
 */
void tiles_sweep_extent(int direction, float angle, float origin_x, float origin_y, float cx, float cy, float pad,
			float dir[2], float range[2]);

/* The wave has to grow, hold and shrink inside the same sweep, so a band and
 * tile duration that would not fit are scaled down together until they do.
 * Both are shares of the sweep. */
void tiles_wave_fit(float *tile_dur, float *band);

#ifdef __cplusplus
}
#endif
