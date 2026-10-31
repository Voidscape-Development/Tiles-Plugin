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
 * What every transition in this module does the same way: loading its effect
 * and finding its uniforms, mixing the audio, reporting its colour space, and
 * choosing between gamma and linear blending for the frame.
 *
 * Each transition's own struct starts with a struct transition_base. That is
 * what lets the callbacks below be put straight into its obs_source_info: the
 * data pointer OBS hands back is a pointer to the transition's struct, and so
 * also to the base at its start.
 */

#pragma once

#include <obs-module.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TC_COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct transition_base {
	obs_source_t *source;
	gs_effect_t *effect;
};

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/*
 * One uniform the transition sets: where its handle lives in the transition's
 * struct, and its name in the effect file. Offsets are from the start of that
 * struct, which is also where its struct transition_base sits.
 */
struct tc_param {
	size_t offset;
	const char *name;
};

#define TC_PARAM(type, field, name) {offsetof(type, field), name}

/*
 * Compiles data/effects/<file> and fills in the handle of every uniform in
 * params. A uniform missing from the effect fails the whole load, with the
 * name in the log, rather than leaving a NULL handle behind that every frame
 * would then log an error for. On failure nothing is left allocated.
 */
bool tc_base_init(struct transition_base *base, obs_source_t *source, const char *file, const struct tc_param *params,
		  size_t count);

/* Destroys the effect. Safe to call inside or outside the graphics context. */
void tc_base_free(struct transition_base *base);

/* For obs_source_info: a plain crossfade of the two scenes' audio. */
bool tc_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		     size_t sample_rate);

/* For obs_source_info: whatever space the two scenes need between them. */
enum gs_color_space tc_video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces);

/* A colour setting, kept both as stored and decoded to linear light, since
 * which one a frame wants depends on the space it is drawn in. */
struct tc_color {
	struct vec4 plain;
	struct vec4 linear;
};

void tc_color_from_rgba(struct tc_color *color, uint32_t rgba);

/* Reads a colour setting, opaque whatever alpha it was stored with. */
void tc_color_from_setting(struct tc_color *color, obs_data_t *settings, const char *name);

/*
 * The blending choice for one frame. On an SDR canvas the scenes are blended
 * as stored, the nonlinear blend users expect from a transition and the one
 * OBS's own fade makes there; anywhere else they are decoded to linear light
 * first, where a nonlinear blend would be too far off.
 */
struct tc_frame {
	bool nonlinear;
	bool previous_srgb;
};

/* Picks the blend and sets the framebuffer to match. Pair with tc_frame_end(). */
void tc_frame_begin(struct tc_frame *frame);
void tc_frame_end(const struct tc_frame *frame);

/* Binds a scene texture or a colour the way the frame blends. */
void tc_set_texture(const struct tc_frame *frame, gs_eparam_t *param, gs_texture_t *texture);
void tc_set_color(const struct tc_frame *frame, gs_eparam_t *param, const struct tc_color *color);

#ifdef __cplusplus
}
#endif
