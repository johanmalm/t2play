// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <linux/input-event-codes.h>
#include <wlr/util/log.h>
#include "panel.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MENU_ITEMS 3
static const char *menu_items[MENU_ITEMS] = {"foo", "bar", "baz"};

#define MENU_ITEM_HEIGHT 30
#define MENU_WIDTH 120

/* Forward declaration */
static void startmenu_close(struct startmenu *menu);

static void
startmenu_render_button(struct startmenu *menu)
{
	struct widget *widget = &menu->base;
	struct panel *panel = widget->panel;

	const char *label = "^";
	PangoRectangle rect = get_text_size(panel->conf->font_description, label);
	widget->width = rect.width + 2 * BUTTON_PADDING;

	if (widget->surface) {
		cairo_surface_destroy(widget->surface);
	}
	widget->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		widget->width, panel->height);
	cairo_t *cr = cairo_create(widget->surface);

	cairo_set_source_u32(cr, panel->conf->button_background);
	cairo_rectangle(cr, 0, 0, widget->width, panel->height);
	cairo_fill(cr);

	cairo_set_source_u32(cr, panel->conf->text);
	cairo_move_to(cr, BUTTON_PADDING, (panel->height - rect.height) / 2.0);
	render_text(cr, panel->conf->font_description, 1, false, "%s", label);
	cairo_destroy(cr);
}

static void
startmenu_render_popup(struct startmenu *menu)
{
	struct panel *panel = menu->base.panel;
	int width = MENU_WIDTH;
	int height = MENU_ITEMS * MENU_ITEM_HEIGHT;

	struct pool_buffer *buffer = get_next_buffer(panel->shm,
		menu->popup_buffers, width, height);
	if (!buffer) {
		return;
	}

	cairo_t *cr = buffer->cairo;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);

	int highlighted = menu->selected >= 0 ? menu->selected : menu->hover;
	for (int i = 0; i < MENU_ITEMS; i++) {
		int y = i * MENU_ITEM_HEIGHT;

		if (i == highlighted) {
			cairo_set_source_u32(cr, panel->conf->button_active);
		} else {
			cairo_set_source_u32(cr, panel->conf->button_background);
		}
		cairo_rectangle(cr, 0, y, width, MENU_ITEM_HEIGHT);
		cairo_fill(cr);

		PangoRectangle text_rect = get_text_size(
			panel->conf->font_description, menu_items[i]);
		cairo_set_source_u32(cr, panel->conf->text);
		cairo_move_to(cr, BUTTON_PADDING,
			y + (MENU_ITEM_HEIGHT - text_rect.height) / 2.0);
		render_text(cr, panel->conf->font_description, 1, false, "%s",
			menu_items[i]);
	}

	wl_surface_set_buffer_scale(menu->popup_surface, 1);
	wl_surface_attach(menu->popup_surface, buffer->buffer, 0, 0);
	wl_surface_damage(menu->popup_surface, 0, 0, width, height);
	wl_surface_commit(menu->popup_surface);
}

static void
popup_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
	uint32_t serial)
{
	struct startmenu *menu = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	startmenu_render_popup(menu);
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
	.configure = popup_xdg_surface_configure,
};

static void
popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y,
	int32_t width, int32_t height)
{
	/* nop */
}

static void
popup_done(void *data, struct xdg_popup *popup)
{
	struct startmenu *menu = data;
	startmenu_close(menu);
	render_frame(menu->base.panel);
}

static void
popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
	/* nop */
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

static void
startmenu_close(struct startmenu *menu)
{
	struct panel *panel = menu->base.panel;
	if (!menu->popup_open) {
		return;
	}
	if (menu->xdg_popup) {
		xdg_popup_destroy(menu->xdg_popup);
		menu->xdg_popup = NULL;
	}
	if (menu->xdg_surface) {
		xdg_surface_destroy(menu->xdg_surface);
		menu->xdg_surface = NULL;
	}
	if (menu->popup_surface) {
		wl_surface_destroy(menu->popup_surface);
		menu->popup_surface = NULL;
	}
	destroy_buffer(&menu->popup_buffers[0]);
	destroy_buffer(&menu->popup_buffers[1]);
	menu->hover = -1;
	menu->selected = -1;
	menu->popup_open = false;
	panel->open_popup = NULL;
}

static void
startmenu_on_left_button_press(struct widget *widget, struct seat *seat)
{
	struct startmenu *menu = (struct startmenu *)widget;
	struct panel *panel = widget->panel;

	if (!panel->xdg_wm_base) {
		wlr_log(WLR_ERROR, "xdg_wm_base not available");
		return;
	}

	if (menu->popup_open) {
		startmenu_close(menu);
		render_frame(panel);
		return;
	}

	menu->popup_surface =
		wl_compositor_create_surface(panel->compositor);
	if (!menu->popup_surface) {
		wlr_log(WLR_ERROR, "failed to create popup surface");
		return;
	}

	menu->xdg_surface = xdg_wm_base_get_xdg_surface(panel->xdg_wm_base,
		menu->popup_surface);
	if (!menu->xdg_surface) {
		wlr_log(WLR_ERROR, "failed to create xdg_surface");
		wl_surface_destroy(menu->popup_surface);
		menu->popup_surface = NULL;
		return;
	}
	xdg_surface_add_listener(menu->xdg_surface,
		&popup_xdg_surface_listener, menu);

	struct xdg_positioner *positioner =
		xdg_wm_base_create_positioner(panel->xdg_wm_base);
	xdg_positioner_set_size(positioner, MENU_WIDTH,
		MENU_ITEMS * MENU_ITEM_HEIGHT);
	xdg_positioner_set_anchor_rect(positioner, widget->x, 0, widget->width,
		panel->height);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(positioner,
		XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);

	menu->xdg_popup = xdg_surface_get_popup(menu->xdg_surface, NULL,
		positioner);
	xdg_positioner_destroy(positioner);

	if (!menu->xdg_popup) {
		wlr_log(WLR_ERROR, "failed to create xdg_popup");
		xdg_surface_destroy(menu->xdg_surface);
		menu->xdg_surface = NULL;
		wl_surface_destroy(menu->popup_surface);
		menu->popup_surface = NULL;
		return;
	}

	zwlr_layer_surface_v1_get_popup(panel->layer_surface, menu->xdg_popup);
	xdg_popup_add_listener(menu->xdg_popup, &popup_listener, menu);
	xdg_popup_grab(menu->xdg_popup, seat->wl_seat,
		seat->pointer.button_serial);

	wl_surface_commit(menu->popup_surface);
	wl_display_roundtrip(panel->display);

	menu->popup_open = true;
	panel->open_popup = menu;
}

static const struct widget_impl startmenu_widget_impl = {
	.on_left_button_press = startmenu_on_left_button_press,
};

void
plugin_startmenu_key(struct panel *panel, uint32_t key)
{
	struct startmenu *menu = panel->open_popup;
	if (!menu) {
		return;
	}
	switch (key) {
	case KEY_UP:
		if (menu->selected < 0) {
			menu->selected = MENU_ITEMS - 1;
		} else {
			menu->selected =
				(menu->selected - 1 + MENU_ITEMS) % MENU_ITEMS;
		}
		startmenu_render_popup(menu);
		break;
	case KEY_DOWN:
		if (menu->selected < 0) {
			menu->selected = 0;
		} else {
			menu->selected = (menu->selected + 1) % MENU_ITEMS;
		}
		startmenu_render_popup(menu);
		break;
	case KEY_ENTER:
		wlr_log(WLR_DEBUG, "startmenu: item %d selected: %s",
			menu->selected,
			menu->selected >= 0 ? menu_items[menu->selected]
					    : "(none)");
		startmenu_close(menu);
		render_frame(panel);
		break;
	case KEY_ESC:
		startmenu_close(menu);
		render_frame(panel);
		break;
	default:
		break;
	}
}

void
plugin_startmenu_pointer_motion(struct startmenu *menu, int y)
{
	int item = y / MENU_ITEM_HEIGHT;
	if (item < 0 || item >= MENU_ITEMS) {
		item = -1;
	}
	if (item != menu->hover) {
		menu->hover = item;
		startmenu_render_popup(menu);
	}
}

void
plugin_startmenu_pointer_leave(struct startmenu *menu)
{
	if (menu->hover >= 0) {
		menu->hover = -1;
		startmenu_render_popup(menu);
	}
}

void
plugin_startmenu_popup_click(struct startmenu *menu, int y)
{
	int item = y / MENU_ITEM_HEIGHT;
	if (item >= 0 && item < MENU_ITEMS) {
		wlr_log(WLR_DEBUG, "startmenu: item %d clicked: %s", item,
			menu_items[item]);
		startmenu_close(menu);
		render_frame(menu->base.panel);
	}
}

void
plugin_startmenu_destroy(struct startmenu *menu)
{
	startmenu_close(menu);
	wl_list_remove(&menu->base.link);
	widget_free(&menu->base);
}

void
plugin_startmenu_update(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_STARTMENU) {
			startmenu_render_button((struct startmenu *)widget);
			break;
		}
	}
}

void
plugin_startmenu_create(struct panel *panel)
{
	struct startmenu *menu = znew(*menu);
	menu->base.type = WIDGET_STARTMENU;
	menu->base.panel = panel;
	menu->base.impl = &startmenu_widget_impl;
	menu->hover = -1;
	menu->selected = -1;
	wl_list_insert(panel->widgets.prev, &menu->base.link);
}
