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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>

#include "transition-common.h"

bool tc_base_init(struct transition_base *base, obs_source_t *source, const char *file, const struct tc_param *params,
		  size_t count)
{
	struct dstr rel = {0};
	char *path;
	char *errors = NULL;
	gs_effect_t *effect;
	size_t i;

	dstr_printf(&rel, "effects/%s", file);
	path = obs_module_file(rel.array);

	if (!path) {
		obs_log(LOG_ERROR, "%s is missing", rel.array);
		dstr_free(&rel);
		return false;
	}

	obs_enter_graphics();
	effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();

	bfree(path);

	if (!effect) {
		obs_log(LOG_ERROR, "failed to compile %s: %s", rel.array, errors ? errors : "(no error text)");
		bfree(errors);
		dstr_free(&rel);
		return false;
	}

	bfree(errors);

	/* The handles are written straight into the transition's struct, which
	 * starts where the base does. */
	for (i = 0; i < count; i++) {
		gs_eparam_t *param = gs_effect_get_param_by_name(effect, params[i].name);

		if (!param) {
			obs_log(LOG_ERROR, "%s has no uniform named '%s'", rel.array, params[i].name);
			obs_enter_graphics();
			gs_effect_destroy(effect);
			obs_leave_graphics();
			dstr_free(&rel);
			return false;
		}

		*(gs_eparam_t **)((char *)base + params[i].offset) = param;
	}

	dstr_free(&rel);

	base->source = source;
	base->effect = effect;
	return true;
}

void tc_base_free(struct transition_base *base)
{
	obs_enter_graphics();
	gs_effect_destroy(base->effect);
	obs_leave_graphics();

	base->effect = NULL;
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

bool tc_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		     size_t sample_rate)
{
	struct transition_base *base = data;

	return obs_transition_audio_render(base->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

enum gs_color_space tc_video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	struct transition_base *base = data;

	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return obs_transition_video_get_color_space(base->source);
}

void tc_color_from_rgba(struct tc_color *color, uint32_t rgba)
{
	vec4_from_rgba(&color->plain, rgba);
	vec4_from_rgba_srgb(&color->linear, rgba);
}

void tc_color_from_setting(struct tc_color *color, obs_data_t *settings, const char *name)
{
	tc_color_from_rgba(color, (uint32_t)obs_data_get_int(settings, name) | 0xFF000000);
}

void tc_frame_begin(struct tc_frame *frame)
{
	frame->nonlinear = gs_get_color_space() == GS_CS_SRGB;
	frame->previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(!frame->nonlinear);
}

void tc_frame_end(const struct tc_frame *frame)
{
	gs_enable_framebuffer_srgb(frame->previous_srgb);
}

void tc_set_texture(const struct tc_frame *frame, gs_eparam_t *param, gs_texture_t *texture)
{
	if (frame->nonlinear)
		gs_effect_set_texture(param, texture);
	else
		gs_effect_set_texture_srgb(param, texture);
}

void tc_set_color(const struct tc_frame *frame, gs_eparam_t *param, const struct tc_color *color)
{
	gs_effect_set_vec4(param, frame->nonlinear ? &color->plain : &color->linear);
}
