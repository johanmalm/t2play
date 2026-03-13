#include "panel.h"
#include <wlr/util/log.h>

void
widget_on_left_button_press(struct widget *widget, struct seat *seat)
{
	if (widget->impl && widget->impl->on_left_button_press) {
		widget->impl->on_left_button_press(widget, seat);
	}
}

void
widgets_free(struct panel *panel)
{
	struct widget *widget, *tmp;
	wl_list_for_each_safe(widget, tmp, &panel->widgets, link) {
		/*
		 * WIDGET_TOPLEVEL entries are not individually allocated; they
		 * are embedded within struct toplevel which is managed by
		 * panel->toplevels and freed by toplevel_destroy(). Just
		 * disconnect the link here so the toplevel can be re-added to
		 * panel->widgets on the next frame. Plugin widgets
		 * (WIDGET_CLOCK, WIDGET_TASKBAR, etc.) are persistent and
		 * freed explicitly in panel_destroy().
		 */
		if (widget->type == WIDGET_TOPLEVEL) {
			wl_list_remove(&widget->link);
			wl_list_init(&widget->link);
		}
	}
}
