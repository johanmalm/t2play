// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "conf.h"
#include "common/mem.h"
#include "panel.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static struct wlr_box
button_size(PangoRectangle rect)
{
	struct wlr_box box = {
		.width = rect.width + 2 * BUTTON_PADDING,
		.height = PANEL_HEIGHT,
	};
	box.width = MIN(box.width, BUTTON_MAX_WIDTH);
	return box;
}

// TODO: should we pass 'width' to this function?
static void
toplevel_update_surface(struct toplevel *toplevel)
{
	assert(toplevel);
	struct widget *widget = &toplevel->base;
	struct panel *panel = widget->panel;
	assert(panel);

	if (widget->surface) {
		cairo_surface_destroy(widget->surface);
	}
	const char *label = toplevel->title
		? toplevel->title
		: (toplevel->app_id ? toplevel->app_id : "?");
	PangoRectangle rect =
		get_text_size(panel->conf->font_description, label);
	struct wlr_box box = button_size(rect);

	widget->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		box.width, box.height);
	cairo_t *cairo = cairo_create(widget->surface);

	toplevel->base.width = box.width;

	/* Draw background */
	if (toplevel->active) {
		cairo_set_source_u32(cairo, panel->conf->button_active);
	} else {
		cairo_set_source_u32(cairo, panel->conf->button_background);
	}
	cairo_rectangle(cairo, 0, 0, box.width, box.height);
	cairo_fill(cairo);

	/* Draw text */
	cairo_set_source_u32(cairo, panel->conf->text);
	cairo_move_to(cairo, BUTTON_PADDING, (box.height - rect.height) / 2.0);
	render_text(cairo, panel->conf->font_description, 1, false, "%s",
		label);
	cairo_destroy(cairo);
}

struct toplevel *
toplevel_from_widget(struct widget *widget)
{
	assert(widget->type == WIDGET_TOPLEVEL);
	return (struct toplevel *)widget;
}

static void
toplevel_on_left_button_press(struct widget *widget, struct seat *seat)
{
	struct toplevel *toplevel = toplevel_from_widget(widget);
	zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle,
		seat->wl_seat);
}

static const struct widget_impl toplevel_widget_impl = {
	.on_left_button_press = toplevel_on_left_button_press,
};

void
toplevel_destroy(struct toplevel *toplevel)
{
	wl_list_remove(&toplevel->base.link);
	zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
	zfree(toplevel->title);
	zfree(toplevel->app_id);
	widget_free(&toplevel->base);
}

static void
handle_toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct toplevel *toplevel = data;
	xstrdup_replace(toplevel->title, title);
	toplevel_update_surface(toplevel);
}

static void
handle_toplevel_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
	       const char *app_id)
{
	struct toplevel *toplevel = data;
	xstrdup_replace(toplevel->app_id, app_id);
	toplevel_update_surface(toplevel);
}

static void
handle_toplevel_output_enter(void *data,
	struct zwlr_foreign_toplevel_handle_v1 *handle,
	struct wl_output *output)
{
	/* nop */
}

static void
handle_toplevel_output_leave(void *data,
	struct zwlr_foreign_toplevel_handle_v1 *handle,
	struct wl_output *output)
{
	/* nop */
}

static void
handle_toplevel_state(void *data,
	struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_array *state)
{
	struct toplevel *toplevel = data;
	toplevel->active = false;
	uint32_t *entry;
	wl_array_for_each(entry, state) {
		if (*entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
			toplevel->active = true;
		}
	}
	toplevel_update_surface(toplevel);
}

static void
handle_toplevel_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	render_frame(toplevel->base.panel);
}

static void
handle_toplevel_closed(void *data,
	struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	struct panel *panel = toplevel->base.panel;
	toplevel_destroy(toplevel);
	render_frame(panel);
}

static void
handle_toplevel_parent(void *data,
	struct zwlr_foreign_toplevel_handle_v1 *handle,
	struct zwlr_foreign_toplevel_handle_v1 *parent)
{
	/* nop */
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
	toplevel_handle_listener = {
		.title = handle_toplevel_title,
		.app_id = handle_toplevel_app_id,
		.output_enter = handle_toplevel_output_enter,
		.output_leave = handle_toplevel_output_leave,
		.state = handle_toplevel_state,
		.done = handle_toplevel_done,
		.closed = handle_toplevel_closed,
		.parent = handle_toplevel_parent,
};

static struct toplevel *
toplevel_create(struct panel *panel,
	struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = znew(*toplevel);
	toplevel->handle = handle;
	toplevel->base.panel = panel;
	toplevel->base.type = WIDGET_TOPLEVEL;
	toplevel->base.impl = &toplevel_widget_impl;
	wl_list_init(&toplevel->base.link);
	return toplevel;
}

static void
handle_toplevel_manager_toplevel(void *data,
	struct zwlr_foreign_toplevel_manager_v1 *manager,
	struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct panel *panel = data;
	struct toplevel *toplevel = toplevel_create(panel, handle);

	wl_list_insert(panel->widgets.prev, &toplevel->base.link);

	zwlr_foreign_toplevel_handle_v1_add_listener(handle,
		&toplevel_handle_listener, toplevel);

	toplevel_update_surface(toplevel);
}

static void
handle_toplevel_manager_finished(void *data,
	struct zwlr_foreign_toplevel_manager_v1 *manager)
{
	struct panel *panel = data;
	zwlr_foreign_toplevel_manager_v1_destroy(panel->toplevel_manager);
	panel->toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
	toplevel_manager_listener = {
		.toplevel = handle_toplevel_manager_toplevel,
		.finished = handle_toplevel_manager_finished,
};

void
plugin_taskbar_init(struct panel *panel)
{
	zwlr_foreign_toplevel_manager_v1_add_listener(panel->toplevel_manager,
		&toplevel_manager_listener, panel);
}

void
plugin_taskbar_create(struct panel *panel)
{
	struct taskbar *taskbar = znew(*taskbar);
	taskbar->base.panel = panel;
	taskbar->base.type = WIDGET_TASKBAR;
	wl_list_insert(panel->widgets.prev, &taskbar->base.link);
}
