// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <unistd.h>
#include "conf.h"
#include "common/mem.h"
#include "panel.h"

static void
battery_update(struct panel *panel, struct widget *widget)
{
	char buf[16] = "--";
	if (panel->battery_path[0]) {
		FILE *f = fopen(panel->battery_path, "r");
		if (f) {
			int capacity;
			if (fscanf(f, "%d", &capacity) == 1) {
				snprintf(buf, sizeof(buf), "%d%%", capacity);
			}
			fclose(f);
		}
	}

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
plugin_battery_update(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_BATTERY) {
			battery_update(panel, widget);
			break;
		}
	}
}

void
plugin_battery_create(struct panel *panel)
{
	struct battery *battery = znew(*battery);
	battery->base.panel = panel;
	battery->base.type = WIDGET_BATTERY;

	/* Determine battery path: try BAT0 first, then BAT1 */
	if (access("/sys/class/power_supply/BAT0/capacity", R_OK) == 0) {
		snprintf(panel->battery_path, sizeof(panel->battery_path),
			"/sys/class/power_supply/BAT0/capacity");
	} else if (access("/sys/class/power_supply/BAT1/capacity", R_OK) == 0) {
		snprintf(panel->battery_path, sizeof(panel->battery_path),
			"/sys/class/power_supply/BAT1/capacity");
	}

	wl_list_insert(panel->widgets.prev, &battery->base.link);
}
