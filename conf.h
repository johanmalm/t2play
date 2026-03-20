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

	/* Colors */
	uint32_t background;
	uint32_t text;
	uint32_t button_background;
	uint32_t button_active;

	char *panel_items;
};

#define BUTTON_PADDING 8
#define BUTTON_MAX_WIDTH 200
#define PANEL_HEIGHT 30

void conf_load(struct conf *conf, const char *path);
void conf_init(struct conf *conf);
void conf_destroy(struct conf *conf);

#endif /* CONF_H */
