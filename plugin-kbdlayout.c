// SPDX-License-Identifier: GPL-2.0-only
#include "panel.h"

static void
kbdlayout_update(struct panel *panel, struct widget *widget)
{
	const char *layout = panel->kbd_layout[0] ? panel->kbd_layout : "--";

	PangoRectangle rect = get_text_size(panel->conf->font_description, layout);
	widget->width = rect.width + 2 * BUTTON_PADDING;
	if (widget->surface) {
		cairo_surface_destroy(widget->surface);
	}
	widget->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		widget->width, panel->height);

	cairo_t *cr = cairo_create(widget->surface);
	cairo_set_source_u32(cr, panel->conf->text);
	cairo_move_to(cr, BUTTON_PADDING, (panel->height - rect.height) / 2.0);
	render_text(cr, panel->conf->font_description, 1, false, "%s", layout);
	cairo_destroy(cr);
}

void
plugin_kbdlayout_update(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_KBDLAYOUT) {
			kbdlayout_update(panel, widget);
			break;
		}
	}
}

void
plugin_kbdlayout_create(struct panel *panel)
{
	struct kbdlayout *kbdlayout = znew(*kbdlayout);
	kbdlayout->base.panel = panel;
	kbdlayout->base.type = WIDGET_KBDLAYOUT;
	wl_list_insert(panel->widgets.prev, &kbdlayout->base.link);
}
