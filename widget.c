// SPDX-License-Identifier: GPL-2.0-only
#include <cairo/cairo.h>
#include <stdlib.h>
#include "panel.h"

void
widget_on_left_button_press(struct widget *widget, struct seat *seat)
{
	if (widget->impl && widget->impl->on_left_button_press) {
		widget->impl->on_left_button_press(widget, seat);
	}
}

char *
widget_type(enum widget_type type)
{
	if (type == WIDGET_CLOCK)
		return "clock";
	if (type == WIDGET_KBDLAYOUT)
		return "kbdlayout";
	if (type == WIDGET_SNI)
		return "sni";
	if (type == WIDGET_STARTMENU)
		return "startmenu";
	if (type == WIDGET_TASKBAR)
		return "taskbar";
	if (type == WIDGET_TOPLEVEL)
		return "toplevel";
	return "other";
}

bool
widget_is_plugin(struct widget *widget)
{
	return widget->type > WIDGET_PLUGINS_BEGIN
		&& widget->type < WIDGET_PLUGINS_END;
}

void
widget_free(struct widget *widget)
{
	if (widget->surface) {
		cairo_surface_destroy(widget->surface);
	}
	free(widget);
}

static void
widget_destroy(struct widget *widget)
{
	wl_list_remove(&widget->link);
	widget_free(widget);
}

void
widgets_free(struct panel *panel)
{
	struct widget *widget, *next;

	wl_list_for_each_safe(widget, next, &panel->widgets, link) {
		if (widget->type == WIDGET_TOPLEVEL) {
			struct toplevel *toplevel =
				toplevel_from_widget(widget);
			toplevel_destroy(toplevel);
		} else if (widget->type == WIDGET_STARTMENU) {
			struct startmenu *menu =
				(struct startmenu *)widget;
			plugin_startmenu_destroy(menu);
		} else if (widget->type == WIDGET_SNI) {
			plugin_sni_destroy((struct sni *)widget);
		} else {
			widget_destroy(widget);
		}
	}
}
