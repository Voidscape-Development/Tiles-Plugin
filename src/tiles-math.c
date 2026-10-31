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

#include "tiles-math.h"

float tiles_circumradius(int shape, float tile_px)
{
	switch (shape) {
	case TILES_SHAPE_SQUARE:
	case TILES_SHAPE_MOSAIC: /* the largest cell is a whole square */
		return tile_px * 0.7071068f;
	case TILES_SHAPE_DIAMOND:
		/* the long diagonal, two triangle heights across */
		return tile_px * 0.8660254f;
	case TILES_SHAPE_BRICK:
		return tile_px * 0.5590170f;
	case TILES_SHAPE_OCTAGON:
		return tile_px * 0.5411961f;
	case TILES_SHAPE_CAIRO:
		return tile_px * 0.5176381f;
	case TILES_SHAPE_SHATTER:
		/* a jittered cell can reach much further than a regular one */
		return tile_px * 1.4f;
	default:
		return tile_px * 0.5773503f;
	}
}

float tiles_edge_width(int shape, float tile_px)
{
	switch (shape) {
	case TILES_SHAPE_CIRCLE:
		return 1.7320508f / tile_px;
	case TILES_SHAPE_TRIANGLE:
		return 3.4641016f / tile_px;
	case TILES_SHAPE_DIAMOND:
		/* its faces lean, so they are crossed faster than either axis
		 * on its own suggests */
		return 2.3094011f / tile_px;
	case TILES_SHAPE_BRICK:
		/* a brick is half as tall as it is wide, so its metric climbs
		 * twice as fast across the long edges as it does the short */
		return 4.0f / tile_px;
	case TILES_SHAPE_CAIRO:
		return 2.7320508f / tile_px;
	default:
		/* squares, hexagons, octagons, mosaic and shatter cells all
		 * gain one metric unit over half a lattice unit at their
		 * steepest. Where a tiling mixes sizes, the shader widens this
		 * per cell - see Cell.aa in the effect. */
		return 2.0f / tile_px;
	}
}

void tiles_sweep_extent(int direction, float angle, float origin_x, float origin_y, float cx, float cy, float pad,
			float dir[2], float range[2])
{
	float corner_x[4] = {0.0f, cx, 0.0f, cx};
	float corner_y[4] = {0.0f, 0.0f, cy, cy};
	float max_radius = 0.0f;
	float proj_min = 0.0f;
	float proj_max = 0.0f;
	int i;

	dir[0] = cosf(angle);
	dir[1] = sinf(angle);

	for (i = 0; i < 4; i++) {
		float dx = corner_x[i] - origin_x;
		float dy = corner_y[i] - origin_y;
		float radius = sqrtf(dx * dx + dy * dy);
		float proj = dx * dir[0] + dy * dir[1];

		if (radius > max_radius)
			max_radius = radius;
		if (i == 0 || proj < proj_min)
			proj_min = proj;
		if (i == 0 || proj > proj_max)
			proj_max = proj;
	}

	switch (direction) {
	case TILES_DIR_ANGLE:
		range[0] = proj_min - pad;
		range[1] = fmaxf(proj_max - proj_min + 2.0f * pad, 1.0f);
		break;
	case TILES_DIR_MIRRORED_IN:
	case TILES_DIR_MIRRORED_OUT:
		range[0] = 0.0f;
		range[1] = fmaxf(fmaxf(fabsf(proj_min), fabsf(proj_max)) + pad, 1.0f);
		break;
	default:
		range[0] = 0.0f;
		range[1] = fmaxf(max_radius + pad, 1.0f);
		break;
	}
}

void tiles_wave_fit(float *tile_dur, float *band)
{
	float total = 2.0f * *tile_dur + *band;

	if (total > 0.95f) {
		*tile_dur *= 0.95f / total;
		*band *= 0.95f / total;
	}
}
