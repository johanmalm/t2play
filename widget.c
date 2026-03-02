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
		if (widget->type != WIDGET_TOPLEVEL) {
			wl_list_remove(&widget->link);
			free(widget);
		}
	}
}

struct widget *
widget_add(struct panel *panel, int x, int width, enum widget_type type)
{
	struct widget *widget = calloc(1, sizeof(*widget));
	if (!widget) {
		wlr_log(WLR_ERROR, "Failed to allocate widget");
		return NULL;
	}
	widget->x = x;
	widget->width = width;
	widget->type = type;
	wl_list_insert(panel->widgets.prev, &widget->link);
	return widget;
}
