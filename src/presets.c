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

#include <math.h>

#include "presets.h"

/* Sliders step in tenths at the finest, so this only has to absorb rounding. */
#define PRESET_TOLERANCE 0.001

const struct preset *preset_find(const struct preset_set *set, long long id)
{
	size_t i;

	if (id == PRESET_CUSTOM)
		return NULL;

	for (i = 0; i < set->count; i++)
		if (set->presets[i].id == id)
			return &set->presets[i];

	return NULL;
}

void preset_set_defaults(const struct preset_set *set, const struct preset *preset, obs_data_t *settings)
{
	size_t i;

	obs_data_set_default_int(settings, PRESET_KEY, preset->id);

	for (i = 0; i < set->column_count; i++) {
		const struct preset_column *c = &set->columns[i];
		double v = preset->value[i];

		switch (c->type) {
		case PRESET_INT:
			obs_data_set_default_int(settings, c->key, (long long)v);
			break;
		case PRESET_COLOR:
			obs_data_set_default_int(settings, c->key, (long long)(uint32_t)v);
			break;
		case PRESET_BOOL:
			obs_data_set_default_bool(settings, c->key, v != 0.0);
			break;
		case PRESET_DOUBLE:
			obs_data_set_default_double(settings, c->key, v);
			break;
		}
	}
}

void preset_fill_list(const struct preset_set *set, obs_property_t *list)
{
	size_t i;

	obs_property_list_add_int(list, obs_module_text(set->custom_label), PRESET_CUSTOM);
	for (i = 0; i < set->count; i++)
		obs_property_list_add_int(list, obs_module_text(set->presets[i].label), set->presets[i].id);
}

static void preset_apply(const struct preset_set *set, const struct preset *preset, obs_data_t *settings)
{
	size_t i;

	for (i = 0; i < set->column_count; i++) {
		const struct preset_column *c = &set->columns[i];
		double v = preset->value[i];

		switch (c->type) {
		case PRESET_INT:
			obs_data_set_int(settings, c->key, (long long)v);
			break;
		case PRESET_COLOR:
			obs_data_set_int(settings, c->key, (long long)(uint32_t)v);
			break;
		case PRESET_BOOL:
			obs_data_set_bool(settings, c->key, v != 0.0);
			break;
		case PRESET_DOUBLE:
			obs_data_set_double(settings, c->key, v);
			break;
		}
	}
}

static bool preset_matches(const struct preset_set *set, const struct preset *preset, obs_data_t *settings)
{
	size_t i;

	for (i = 0; i < set->column_count; i++) {
		const struct preset_column *c = &set->columns[i];
		double v = preset->value[i];
		bool same = false;

		switch (c->type) {
		case PRESET_INT:
			same = obs_data_get_int(settings, c->key) == (long long)v;
			break;
		case PRESET_COLOR:
			same = ((uint32_t)obs_data_get_int(settings, c->key) & 0xFFFFFF) == ((uint32_t)v & 0xFFFFFF);
			break;
		case PRESET_BOOL:
			same = obs_data_get_bool(settings, c->key) == (v != 0.0);
			break;
		case PRESET_DOUBLE:
			same = fabs(obs_data_get_double(settings, c->key) - v) < PRESET_TOLERANCE;
			break;
		}

		if (!same)
			return false;
	}

	return true;
}

void preset_selected(const struct preset_set *set, obs_data_t *settings)
{
	const struct preset *preset = preset_find(set, obs_data_get_int(settings, PRESET_KEY));

	if (preset)
		preset_apply(set, preset, settings);
}

void preset_touched(const struct preset_set *set, obs_data_t *settings)
{
	const struct preset *preset = preset_find(set, obs_data_get_int(settings, PRESET_KEY));

	if (preset && !preset_matches(set, preset, settings))
		obs_data_set_int(settings, PRESET_KEY, PRESET_CUSTOM);
}
