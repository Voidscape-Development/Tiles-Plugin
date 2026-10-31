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
 * Starts video for the GPU checks on whichever graphics module this libobs
 * has. On Windows and macOS the build names it. On Linux the OpenGL module's
 * file name carries libobs's soname version, which has changed between
 * releases, so each known name is tried in turn.
 */

#pragma once

#include <obs.h>

static inline int gpu_check_reset_video(struct obs_video_info *ovi)
{
#ifdef GPU_CHECK_GRAPHICS_MODULE
	ovi->graphics_module = GPU_CHECK_GRAPHICS_MODULE;
	return obs_reset_video(ovi);
#else
	static const char *const modules[] = {"libobs-opengl.so.30", "libobs-opengl.so.1", "libobs-opengl"};
	int result = OBS_VIDEO_MODULE_NOT_FOUND;

	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
		ovi->graphics_module = modules[i];
		result = obs_reset_video(ovi);
		if (result != OBS_VIDEO_MODULE_NOT_FOUND)
			break;
	}

	return result;
#endif
}
