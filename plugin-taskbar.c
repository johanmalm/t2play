#include "panel.h"
#include <assert.h>
#include <wlr/util/log.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static struct toplevel *
toplevel_from_widget(struct widget *widget)
{
	assert(widget->type == WIDGET_TOPLEVEL);
	return (struct toplevel *)widget;
}

static void
toplevel_on_left_button_press(struct widget *widget, struct seat *seat)
{
	struct toplevel *toplevel = toplevel_from_widget(widget);
	zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, seat->wl_seat);
}

static const struct widget_impl toplevel_widget_impl = {
	.on_left_button_press = toplevel_on_left_button_press,
};

void
toplevel_destroy(struct toplevel *toplevel)
{
	wl_list_remove(&toplevel->base.link);
	zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
	free(toplevel->title);
	free(toplevel->app_id);
	wl_list_remove(&toplevel->link);
	free(toplevel);
}

static void
handle_toplevel_title(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct toplevel *toplevel = data;
	free(toplevel->title);
	toplevel->title = strdup(title);
	if (!toplevel->title) {
		perror("strdup");
	}
}

static void
handle_toplevel_app_id(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *app_id)
{
	struct toplevel *toplevel = data;
	free(toplevel->app_id);
	toplevel->app_id = strdup(app_id);
	if (!toplevel->app_id) {
		perror("strdup");
	}
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
		struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_array *state)
{
	struct toplevel *toplevel = data;
	toplevel->active = false;
	uint32_t *entry;
	wl_array_for_each(entry, state) {
		if (*entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
			toplevel->active = true;
		}
	}
}

static void
handle_toplevel_done(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	render_frame(toplevel->panel);
}

static void
handle_toplevel_closed(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	struct panel *panel = toplevel->panel;
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

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
	.title = handle_toplevel_title,
	.app_id = handle_toplevel_app_id,
	.output_enter = handle_toplevel_output_enter,
	.output_leave = handle_toplevel_output_leave,
	.state = handle_toplevel_state,
	.done = handle_toplevel_done,
	.closed = handle_toplevel_closed,
	.parent = handle_toplevel_parent,
};

static void
handle_toplevel_manager_toplevel(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct panel *panel = data;
	struct toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		perror("calloc");
		return;
	}
	toplevel->handle = handle;
	toplevel->panel = panel;
	wl_list_init(&toplevel->base.link);
	toplevel->base.type = WIDGET_TOPLEVEL;
	toplevel->base.impl = &toplevel_widget_impl;
	wl_list_insert(panel->toplevels.prev, &toplevel->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle,
		&toplevel_handle_listener, toplevel);
}

static void
handle_toplevel_manager_finished(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager)
{
	struct panel *panel = data;
	zwlr_foreign_toplevel_manager_v1_destroy(panel->toplevel_manager);
	panel->toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = handle_toplevel_manager_toplevel,
	.finished = handle_toplevel_manager_finished,
};

void
plugin_taskbar_init(struct panel *panel)
{
	zwlr_foreign_toplevel_manager_v1_add_listener(panel->toplevel_manager,
		&toplevel_manager_listener, panel);
}
