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
 * Presets as data.
 *
 * A transition describes the settings its presets cover once, as columns - a
 * key and how it is stored - and each preset is a row of values in that order.
 * Applying a preset, recognising one, and taking the defaults from one all walk
 * the same columns, so a setting cannot be added to one of them and forgotten
 * in another.
 *
 * Picking a preset writes every column, so a preset is a whole look rather
 * than a starting point. Editing any column afterwards drops the list back to
 * Custom, and edits that land back on the preset exactly leave it selected.
 * Settings that are not columns - where an effect happens, a mask image - are
 * left alone by both.
 */

#pragma once

#include <obs-module.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The setting holding the selected preset, and the id meaning none. */
#define PRESET_KEY "preset"
#define PRESET_CUSTOM 0

/* The most columns a transition's presets may cover. */
#define PRESET_MAX_COLUMNS 32

enum preset_type {
	PRESET_INT,
	PRESET_DOUBLE,
	PRESET_BOOL,
	PRESET_COLOR, /* compared on RGB only: the colour settings are opaque */
};

struct preset_column {
	const char *key;
	enum preset_type type;
};

struct preset {
	/*
	 * Saved in users' scene collections as the selected preset, so an id
	 * is permanent: never renumber one or reuse a retired one. The order
	 * of the rows is only the order of the list.
	 */
	long long id;
	const char *label; /* locale key */
	double value[PRESET_MAX_COLUMNS];
};

struct preset_set {
	const struct preset_column *columns;
	size_t column_count;
	const struct preset *presets;
	size_t count;
	const char *custom_label; /* locale key for the Custom entry */
};

/* The preset with this id, or NULL for Custom or an id no longer offered. */
const struct preset *preset_find(const struct preset_set *set, long long id);

/* Makes the preset's values the defaults, for the transition's get_defaults. */
void preset_set_defaults(const struct preset_set *set, const struct preset *preset, obs_data_t *settings);

/* Fills the preset list: Custom, then every preset under its id. */
void preset_fill_list(const struct preset_set *set, obs_property_t *list);

/* For the preset list's modified callback: writes the selected preset's
 * values into settings. Custom changes nothing. */
void preset_selected(const struct preset_set *set, obs_data_t *settings);

/* For every other column's modified callback: drops the list to Custom once
 * the settings no longer match the selected preset. */
void preset_touched(const struct preset_set *set, obs_data_t *settings);

#ifdef __cplusplus
}
#endif
