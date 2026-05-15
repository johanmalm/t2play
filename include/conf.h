/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CONF_H
#define CONF_H
#include <pango/pangocairo.h>
#include <stdint.h>

struct conf {
	PangoFontDescription *font_description;
	char *output;
	uint32_t anchors;
	int32_t layer; /* enum zwlr_layer_shell_v1_layer or -1 if unset */

	/* colors */
	uint32_t background;
	uint32_t text;
	uint32_t button_background;
	uint32_t button_active;

	/* panel */
	char *panel_items;
	int panel_breadth;

	/* taskbar */
	int taskbar_padding;
	int taskbar_spacing;

	int task_padding;

	/* startmenu */
	char *startmenu_layout;
	int startmenu_padding;

	/* clock */
	int clock_padding;

	/* battery */
	int battery_padding;

	/* keyboard */
	int keyboard_padding;
};

#define BUTTON_MAX_WIDTH 200

void conf_load(struct conf *conf, const char *config_file);
void conf_init(struct conf *conf);
void conf_destroy(struct conf *conf);

#endif /* CONF_H */
