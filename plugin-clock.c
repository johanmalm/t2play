// SPDX-License-Identifier: GPL-2.0-only
#include <wlr/util/log.h>
#include "panel.h"

void
clock_update(struct panel *panel, struct widget *widget)
{
	time_t t = time(NULL);
	struct tm *tm_info = localtime(&t);
	char buf[6]; /* "HH:MM\0" */
	strftime(buf, sizeof(buf), "%H:%M", tm_info);

	PangoRectangle rect = get_text_size(panel->conf->font_description, buf);
	widget->width = rect.width + 2 * BUTTON_PADDING;
	if (widget->surface) {
		cairo_surface_destroy(widget->surface);
	}
	widget->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		widget->width, panel->height);

	cairo_t *cr = cairo_create(widget->surface);
	cairo_set_source_u32(cr, panel->conf->text);
	cairo_move_to(cr, BUTTON_PADDING, (panel->height - rect.height) / 2.0);
	render_text(cr, panel->conf->font_description, 1, false, "%s", buf);
	cairo_destroy(cr);
}

void
plugin_clock_update(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_CLOCK) {
			clock_update(panel, widget);
			break;
		}
	}
}

void
plugin_clock_create(struct panel *panel)
{
	struct clock *clock = znew(*clock);
	clock->base.type = WIDGET_CLOCK;
	wl_list_insert(panel->widgets.prev, &clock->base.link);
}
