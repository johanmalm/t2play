/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DESKTOP_ENTRY_H
#define DESKTOP_ENTRY_H
#include <cairo.h>

struct panel;

void desktop_entry_init(struct panel *panel);
void desktop_entry_finish(struct panel *panel);
void desktop_entry_load_icon(cairo_t *cairo, struct panel *panel, const char *icon_name, int size, float scale);
void desktop_entry_load_icon_from_app_id(cairo_t *cairo, struct panel *panel, const char *app_id, int size, float scale);
const char *desktop_entry_name_lookup(struct panel *panel, const char *app_id);

#endif /* DESKTOP_ENTRY_H */
