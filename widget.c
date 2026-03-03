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
		wl_list_remove(&widget->link);
		// TODO: is this really needed?
		if (widget->type == WIDGET_TOPLEVEL) {
			wl_list_init(&widget->link);
		} else {
			free(widget);
		}
	}
}

void
widget_add(struct panel *panel, int x, int width)
{
	struct widget *widget = znew(*widget);
	widget->x = x;
	widget->width = width;
	// TODO: does not look right
	widget->type = WIDGET_CLOCK;
	wl_list_insert(panel->widgets.prev, &widget->link);
}
