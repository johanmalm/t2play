// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cairo.h>
#include <getopt.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <wayland-util.h>
#ifdef __FreeBSD__
#include <sys/event.h> /* For signalfd() */
#endif
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wlr/util/log.h>
#include "cursor-shape-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "conf.h"
#include "mem.h"
#include "panel.h"

static void
init_plugins(struct panel *panel)
{
	if (!panel->conf->panel_items) {
		return;
	}
	for (const char *p = panel->conf->panel_items; *p; p++) {
		if (*p == 'S') {
			plugin_startmenu_create(panel);
		} else if (*p == 'T') {
			plugin_taskbar_create(panel);
		} else if (*p == 'C') {
			plugin_clock_create(panel);
		} else if (*p == 'K') {
			plugin_kbdlayout_create(panel);
		} else if (*p == 'B') {
			plugin_battery_create(panel);
		} else {
			wlr_log(WLR_ERROR, "unknown panel_items code '%c'", *p);
		}
	}
}

static void
update_widget_positions(struct panel *panel)
{
	struct widget *widget, *taskbar = NULL;

	/* Start by working out the width of the taskbar (the expandable widget)
	 */
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_TASKBAR) {
			taskbar = widget;
			break;
		}
	}

	if (taskbar) {
		int width_left_for_expandable_widget = panel->width;
		wl_list_for_each(widget, &panel->widgets, link) {
			if (!widget_is_plugin(widget)) {
				continue;
			}
			/* For the time being, only the taskbar can be
			 * expandable */
			if (widget == taskbar) {
				continue;
			}
			width_left_for_expandable_widget -= widget->width;
		}
		taskbar->width = width_left_for_expandable_widget;
	}

	/* Set plugin x-positions */
	int x = 0;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (!widget_is_plugin(widget)) {
			continue;
		}
		widget->x = x;
		x += widget->width;
	}

	/* Set taskbar toplevel x-positions */
	if (taskbar) {

		// TODO: Set toplevel widths more intelligently

		x = taskbar->x;
		wl_list_for_each(widget, &panel->widgets, link) {
			if (widget->type != WIDGET_TOPLEVEL) {
				continue;
			}
			widget->x = x;
			x += widget->width;
		}
	}
}

static void
render_panel(cairo_t *cairo, struct panel *panel)
{
	// TODO: Create a taskbar->base.surface for this instead
	/* Draw background */
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, panel->conf->background);
	cairo_paint(cairo);
	cairo_restore(cairo);

	/* Render all widgets */
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (!widget->surface) {
			wlr_log(WLR_DEBUG, "no wiget surface for %s",
				widget_type(widget->type));
			continue;
		}
		cairo_save(cairo);
		cairo_set_source_surface(cairo, widget->surface, widget->x, 0);
		cairo_paint(cairo);
		cairo_restore(cairo);
	}

	/* Draw border */
	cairo_save(cairo);
	cairo_set_source_u32(cairo, panel->conf->text);
	cairo_rectangle(cairo, 0, 0, panel->width, panel->height);
	cairo_stroke(cairo);
	cairo_restore(cairo);
}

void
render_frame(struct panel *panel)
{
	if (!panel->run_display || !panel->width || !panel->height) {
		return;
	}

	cairo_surface_t *surface =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(surface);
	cairo_scale(cairo, panel->scale, panel->scale);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	update_widget_positions(panel);
	render_panel(cairo, panel);

	panel->current_buffer = get_next_buffer(panel->shm, panel->buffers,
		panel->width * panel->scale, panel->height * panel->scale);
	if (!panel->current_buffer) {
		goto cleanup;
	}

	cairo_t *shm = panel->current_buffer->cairo;
	cairo_save(shm);
	cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
	cairo_paint(shm);
	cairo_restore(shm);
	cairo_set_source_surface(shm, surface, 0.0, 0.0);
	cairo_paint(shm);

	wl_surface_set_buffer_scale(panel->surface, panel->scale);
	wl_surface_attach(panel->surface, panel->current_buffer->buffer, 0, 0);
	wl_surface_damage(panel->surface, 0, 0, panel->width, panel->height);
	wl_surface_commit(panel->surface);
	wl_display_roundtrip(panel->display);

cleanup:
	cairo_surface_destroy(surface);
	cairo_destroy(cairo);
}

static void
seat_destroy(struct seat *seat)
{
	if (seat->pointer.cursor_theme) {
		wl_cursor_theme_destroy(seat->pointer.cursor_theme);
	}
	if (seat->pointer.pointer) {
		wl_pointer_destroy(seat->pointer.pointer);
	}
	if (seat->keyboard) {
		wl_keyboard_destroy(seat->keyboard);
		seat->keyboard = NULL;
	}
	if (seat->xkb_keymap) {
		xkb_keymap_unref(seat->xkb_keymap);
	}
	if (seat->xkb_state) {
		xkb_state_unref(seat->xkb_state);
	}
	if (seat->xkb_context) {
		xkb_context_unref(seat->xkb_context);
	}
	wl_seat_destroy(seat->wl_seat);
	wl_list_remove(&seat->link);
	zfree(seat);
}

static void
close_pollfd(struct pollfd *pollfd)
{
	if (pollfd->fd == -1) {
		return;
	}
	close(pollfd->fd);
	pollfd->fd = -1;
	pollfd->events = 0;
	pollfd->revents = 0;
}

static void
panel_destroy(struct panel *panel)
{
	panel->run_display = false;

	conf_destroy(panel->conf);

	if (panel->layer_surface) {
		zwlr_layer_surface_v1_destroy(panel->layer_surface);
	}

	if (panel->surface) {
		wl_surface_destroy(panel->surface);
	}

	if (panel->layer_shell) {
		zwlr_layer_shell_v1_destroy(panel->layer_shell);
	}

	if (panel->cursor_shape_manager) {
		wp_cursor_shape_manager_v1_destroy(panel->cursor_shape_manager);
	}

	struct seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &panel->seats, link) {
		seat_destroy(seat);
	}

	widgets_free(panel);

	if (panel->toplevel_manager) {
		zwlr_foreign_toplevel_manager_v1_destroy(
			panel->toplevel_manager);
		panel->toplevel_manager = NULL;
	}

	destroy_buffer(&panel->buffers[0]);
	destroy_buffer(&panel->buffers[1]);

	struct output *output, *temp;
	wl_list_for_each_safe(output, temp, &panel->outputs, link) {
		wl_output_destroy(output->wl_output);
		zfree(output->name);
		wl_list_remove(&output->link);
		zfree(output);
	};

	if (panel->xdg_wm_base) {
		xdg_wm_base_destroy(panel->xdg_wm_base);
		panel->xdg_wm_base = NULL;
	}

	if (panel->compositor) {
		wl_compositor_destroy(panel->compositor);
	}

	if (panel->shm) {
		wl_shm_destroy(panel->shm);
	}

	if (panel->display) {
		wl_display_disconnect(panel->display);
	}
	pango_cairo_font_map_set_default(NULL);

	close_pollfd(&panel->pollfds[FD_SIGNAL]);
	close_pollfd(&panel->pollfds[FD_CLOCK]);
	close_pollfd(&panel->pollfds[FD_BATTERY]);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
	uint32_t serial, uint32_t width, uint32_t height)
{
	struct panel *panel = data;
	panel->width = width;
	panel->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_frame(panel);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct panel *panel = data;
	panel_destroy(panel);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	struct panel *panel = data;
	struct output *panel_output;
	wl_list_for_each(panel_output, &panel->outputs, link) {
		if (panel_output->wl_output != output) {
			continue;
		}
		wlr_log(WLR_DEBUG, "surface enter on output %s",
			panel_output->name);
		panel->output = panel_output;
		panel->scale = panel->output->scale;
		render_frame(panel);
		break;
	}
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
	struct wl_output *output)
{
	/* nop */
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void
update_cursor(struct seat *seat)
{
	struct pointer *pointer = &seat->pointer;
	struct panel *panel = seat->panel;
	if (pointer->cursor_theme) {
		wl_cursor_theme_destroy(pointer->cursor_theme);
	}
	const char *cursor_theme = getenv("XCURSOR_THEME");
	unsigned int cursor_size = 24;
	const char *env_cursor_size = getenv("XCURSOR_SIZE");
	if (env_cursor_size && *env_cursor_size) {
		errno = 0;
		char *end;
		unsigned int size = strtoul(env_cursor_size, &end, 10);
		if (!*end && errno == 0) {
			cursor_size = size;
		}
	}
	pointer->cursor_theme = wl_cursor_theme_load(cursor_theme,
		cursor_size * panel->scale, panel->shm);
	if (!pointer->cursor_theme) {
		wlr_log(WLR_ERROR, "failed to load cursor theme");
		return;
	}
	struct wl_cursor *cursor =
		wl_cursor_theme_get_cursor(pointer->cursor_theme, "default");
	if (!cursor) {
		wlr_log(WLR_ERROR, "failed to get default cursor from theme");
		return;
	}
	pointer->cursor_image = cursor->images[0];
	wl_surface_set_buffer_scale(pointer->cursor_surface, panel->scale);
	wl_surface_attach(pointer->cursor_surface,
		wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(pointer->pointer, pointer->serial,
		pointer->cursor_surface,
		pointer->cursor_image->hotspot_x / panel->scale,
		pointer->cursor_image->hotspot_y / panel->scale);
	wl_surface_damage_buffer(pointer->cursor_surface, 0, 0, INT32_MAX,
		INT32_MAX);
	wl_surface_commit(pointer->cursor_surface);
}

static void
update_all_cursors(struct panel *panel)
{
	struct seat *seat;
	wl_list_for_each(seat, &panel->seats, link) {
		if (seat->pointer.pointer) {
			update_cursor(seat);
		}
	}
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct seat *seat = data;

	struct pointer *pointer = &seat->pointer;
	pointer->x = wl_fixed_to_int(surface_x);
	pointer->y = wl_fixed_to_int(surface_y);
	pointer->focus_surface = surface;

	if (seat->panel->cursor_shape_manager) {
		struct wp_cursor_shape_device_v1 *device =
			wp_cursor_shape_manager_v1_get_pointer(
				seat->panel->cursor_shape_manager, wl_pointer);
		wp_cursor_shape_device_v1_set_shape(device, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
	} else {
		pointer->serial = serial;
		update_cursor(seat);
	}
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface)
{
	struct seat *seat = data;
	if (seat->panel->open_popup
		&& surface == seat->panel->open_popup->popup_surface) {
		plugin_startmenu_pointer_leave(seat->panel->open_popup);
	}
	seat->pointer.focus_surface = NULL;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct seat *seat = data;
	seat->pointer.x = wl_fixed_to_int(surface_x);
	seat->pointer.y = wl_fixed_to_int(surface_y);
	if (seat->panel->open_popup
		&& seat->pointer.focus_surface
			!= seat->panel->surface) {
		plugin_startmenu_pointer_motion(seat->panel->open_popup,
			seat->pointer.y);
	}
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	struct seat *seat = data;
	struct panel *panel = seat->panel;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	seat->pointer.button_serial = serial;

	if (panel->open_popup
		&& seat->pointer.focus_surface != panel->surface) {
		plugin_startmenu_popup_click(panel->open_popup,
			seat->pointer.y);
		return;
	}

	int x = seat->pointer.x;

	struct widget *widget;
	wl_list_for_each_reverse(widget, &panel->widgets, link) {
		if (x >= widget->x && x < widget->x + widget->width) {
			widget_on_left_button_press(widget, seat);
			break;
		}
	}
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	struct seat *seat = data;
	/* WL_POINTER_AXIS_VERTICAL_SCROLL = 0 */
	if (seat->panel->open_popup && axis == 0
			&& seat->pointer.focus_surface
				!= seat->panel->surface) {
		plugin_startmenu_scroll(seat->panel->open_popup,
			wl_fixed_to_double(value));
	}
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
	/* nop */
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis_source)
{
	/* nop */
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis)
{
	/* nop */
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis, int32_t discrete)
{
	/* nop */
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static void
wl_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
	int32_t fd, uint32_t size)
{
	struct seat *seat = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	if (size == 0) {
		close(fd);
		return;
	}
	char *keymap_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (keymap_str == MAP_FAILED) {
		return;
	}
	if (!seat->xkb_context) {
		seat->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	}
	if (seat->xkb_keymap) {
		xkb_keymap_unref(seat->xkb_keymap);
	}
	seat->xkb_keymap = xkb_keymap_new_from_string(seat->xkb_context,
		keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(keymap_str, size);
	if (seat->xkb_keymap) {
		if (seat->xkb_state) {
			xkb_state_unref(seat->xkb_state);
		}
		seat->xkb_state = xkb_state_new(seat->xkb_keymap);
		const char *layout_name =
			xkb_keymap_layout_get_name(seat->xkb_keymap, 0);
		if (layout_name) {
			snprintf(seat->panel->kbd_layout,
				sizeof(seat->panel->kbd_layout),
				"%s", layout_name);
		}
	}
}

static void
wl_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	struct wl_surface *surface, struct wl_array *keys)
{
	/* nop */
}

static void
wl_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	struct wl_surface *surface)
{
	/* nop */
}

static void
wl_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	uint32_t time, uint32_t key, uint32_t state)
{
	struct seat *seat = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}
	if (seat->panel->open_popup) {
		plugin_startmenu_key(seat->panel, key);
		/* Handle printable characters for type-to-search */
		if (seat->xkb_state) {
			char utf8[8] = {0};
			if (xkb_state_key_get_utf8(seat->xkb_state,
					key + 8, utf8, sizeof(utf8)) > 0
					&& (unsigned char)utf8[0] >= 0x20) {
				plugin_startmenu_text_input(seat->panel, utf8);
			}
		}
	}
}

static void
wl_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
	uint32_t mods_locked, uint32_t group)
{
	struct seat *seat = data;
	if (seat->xkb_state) {
		xkb_state_update_mask(seat->xkb_state, mods_depressed,
			mods_latched, mods_locked, 0, 0, group);
	}
	if (!seat->xkb_keymap) {
		return;
	}
	const char *layout_name =
		xkb_keymap_layout_get_name(seat->xkb_keymap, group);
	if (!layout_name) {
		return;
	}
	struct panel *panel = seat->panel;
	snprintf(panel->kbd_layout, sizeof(panel->kbd_layout), "%s",
		layout_name);
	plugin_kbdlayout_update(panel);
	render_frame(panel);
}

static void
wl_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
	int32_t rate, int32_t delay)
{
	/* nop */
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
	enum wl_seat_capability caps)
{
	struct seat *seat = data;
	bool cap_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	if (cap_pointer && !seat->pointer.pointer) {
		seat->pointer.pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->pointer.pointer,
			&pointer_listener, seat);
	} else if (!cap_pointer && seat->pointer.pointer) {
		wl_pointer_destroy(seat->pointer.pointer);
		seat->pointer.pointer = NULL;
	}
	bool cap_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (cap_keyboard && !seat->keyboard) {
		seat->keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(seat->keyboard, &keyboard_listener,
			seat);
	} else if (!cap_keyboard && seat->keyboard) {
		wl_keyboard_destroy(seat->keyboard);
		seat->keyboard = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
	/* nop */
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
	int32_t physical_width, int32_t physical_height, int32_t subpixel,
	const char *make, const char *model, int32_t transform)
{
	/* nop */
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh)
{
	/* nop */
}

static void
output_done(void *data, struct wl_output *output)
{
	/* nop */
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	struct output *panel_output = data;
	panel_output->scale = factor;
	if (panel_output->panel->output == panel_output) {
		panel_output->panel->scale = panel_output->scale;
		if (!panel_output->panel->cursor_shape_manager) {
			update_all_cursors(panel_output->panel);
		}
		render_frame(panel_output->panel);
	}
}

static void
output_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct output *output = data;
	xstrdup_replace(output->name, name);

	struct panel *panel = output->panel;
	struct conf *conf = panel->conf;

	if (!panel->output && conf->output && !strcmp(conf->output, name)) {
		wlr_log(WLR_DEBUG, "Using output %s", name);
		panel->output = output;
	}
}

static void
output_description(void *data, struct wl_output *wl_output,
	const char *description)
{
	/* nop */
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *xdg_wm_base,
	uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_handle_ping,
};

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	struct panel *panel = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		panel->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct seat *seat = znew(*seat);
		seat->panel = panel;
		seat->wl_name = name;
		seat->wl_seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
		wl_list_insert(&panel->seats, &seat->link);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		panel->shm =
			wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (!panel->output) {
			struct output *output = znew(*output);
			output->wl_output = wl_registry_bind(registry, name,
				&wl_output_interface, 4);
			output->wl_name = name;
			output->scale = 1;
			output->panel = panel;
			wl_list_insert(&panel->outputs, &output->link);
			wl_output_add_listener(output->wl_output,
				&output_listener, output);
		}
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		panel->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name)
		== 0) {
		panel->cursor_shape_manager = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface,
			   zwlr_foreign_toplevel_manager_v1_interface.name)
		== 0) {
		panel->toplevel_manager = wl_registry_bind(registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface, 3);
		plugin_taskbar_init(panel);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		panel->xdg_wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 2);
		xdg_wm_base_add_listener(panel->xdg_wm_base,
			&xdg_wm_base_listener, NULL);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct panel *panel = data;
	if (panel->output->wl_name == name) {
		panel->run_display = false;
	}

	struct seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &panel->seats, link) {
		if (seat->wl_name == name) {
			seat_destroy(seat);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void
panel_setup_cursors(struct panel *panel)
{
	struct seat *seat;

	wl_list_for_each(seat, &panel->seats, link) {
		struct pointer *p = &seat->pointer;

		p->cursor_surface =
			wl_compositor_create_surface(panel->compositor);
		assert(p->cursor_surface);
	}
}

static void
panel_setup(struct panel *panel)
{
	panel->display = wl_display_connect(NULL);
	if (!panel->display) {
		wlr_log(WLR_ERROR, "unable to connect to the compositor");
		exit(EXIT_FAILURE);
	}

	panel->scale = 1;

	struct wl_registry *registry = wl_display_get_registry(panel->display);
	wl_registry_add_listener(registry, &registry_listener, panel);
	if (wl_display_roundtrip(panel->display) < 0) {
		wlr_log(WLR_ERROR,
			"failed to register with the wayland display");
		exit(EXIT_FAILURE);
	}

	assert(panel->compositor && panel->layer_shell && panel->shm);

	/* Second roundtrip to get wl_output properties */
	if (wl_display_roundtrip(panel->display) < 0) {
		wlr_log(WLR_ERROR, "error during outputs init");
		panel_destroy(panel);
		exit(EXIT_FAILURE);
	}

	if (!panel->output && panel->conf->output) {
		wlr_log(WLR_ERROR, "output '%s' not found",
			panel->conf->output);
		panel_destroy(panel);
		exit(EXIT_FAILURE);
	}

	if (!panel->cursor_shape_manager) {
		panel_setup_cursors(panel);
	}

	panel->surface = wl_compositor_create_surface(panel->compositor);
	assert(panel->surface);
	wl_surface_add_listener(panel->surface, &surface_listener, panel);

	panel->layer_surface = zwlr_layer_shell_v1_get_layer_surface(panel->layer_shell,
		panel->surface, panel->output ? panel->output->wl_output : NULL,
		panel->conf->layer, "t2play");
	assert(panel->layer_surface);
	zwlr_layer_surface_v1_add_listener(panel->layer_surface,
		&layer_surface_listener, panel);
	zwlr_layer_surface_v1_set_anchor(panel->layer_surface,
		panel->conf->anchors);

	wl_registry_destroy(registry);

	panel->pollfds[FD_WAYLAND].fd = wl_display_get_fd(panel->display);
	panel->pollfds[FD_WAYLAND].events = POLLIN;

	if (panel->conf->panel_items && strchr(panel->conf->panel_items, 'C')) {
		panel->pollfds[FD_CLOCK].fd =
			timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
		panel->pollfds[FD_CLOCK].events = POLLIN;
		/* Fire at the start of the next minute, then every 60 seconds
		 */
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		struct itimerspec clock_timer = {
			.it_interval.tv_sec = 60,
			.it_value.tv_sec = now.tv_sec - (now.tv_sec % 60) + 60,
		};
		timerfd_settime(panel->pollfds[FD_CLOCK].fd, TFD_TIMER_ABSTIME,
			&clock_timer, NULL);
	} else {
		panel->pollfds[FD_CLOCK].fd = -1;
	}

	if (panel->conf->panel_items && strchr(panel->conf->panel_items, 'B')
			&& panel->battery_path[0]) {
		int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		if (tfd < 0) {
			wlr_log(WLR_ERROR, "timerfd_create failed for battery");
			panel->pollfds[FD_BATTERY].fd = -1;
		} else {
			struct itimerspec battery_timer = {
				.it_interval.tv_sec = 30,
				.it_value.tv_sec = 30,
			};
			if (timerfd_settime(tfd, 0, &battery_timer, NULL) < 0) {
				wlr_log(WLR_ERROR,
					"timerfd_settime failed for battery");
				close(tfd);
				panel->pollfds[FD_BATTERY].fd = -1;
			} else {
				panel->pollfds[FD_BATTERY].fd = tfd;
				panel->pollfds[FD_BATTERY].events = POLLIN;
			}
		}
	} else {
		panel->pollfds[FD_BATTERY].fd = -1;
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	panel->pollfds[FD_SIGNAL].fd =
		signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	panel->pollfds[FD_SIGNAL].events = POLLIN;
}

static void
panel_run(struct panel *panel)
{
	panel->run_display = true;

	zwlr_layer_surface_v1_set_size(panel->layer_surface, 0, PANEL_HEIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(panel->layer_surface, PANEL_HEIGHT);
	wl_surface_commit(panel->surface);
	wl_display_roundtrip(panel->display);

	plugin_clock_update(panel);
	plugin_startmenu_update(panel);
	plugin_kbdlayout_update(panel);
	plugin_battery_update(panel);

	render_frame(panel);
	while (panel->run_display) {
		while (wl_display_prepare_read(panel->display) != 0) {
			wl_display_dispatch_pending(panel->display);
		}

		errno = 0;
		if (wl_display_flush(panel->display) == -1 && errno != EAGAIN) {
			break;
		}

		if (!panel->run_display) {
			break;
		}

		poll(panel->pollfds, NR_FDS, -1);
		if (panel->pollfds[FD_WAYLAND].revents & POLLIN) {
			wl_display_read_events(panel->display);
		} else {
			wl_display_cancel_read(panel->display);
		}
		if (panel->pollfds[FD_SIGNAL].revents & POLLIN) {
			break;
		}
		if (panel->pollfds[FD_CLOCK].revents & POLLIN) {
			uint64_t exp;
			read(panel->pollfds[FD_CLOCK].fd, &exp, sizeof(exp));
			plugin_clock_update(panel);
			render_frame(panel);
		}
		if (panel->pollfds[FD_BATTERY].revents & POLLIN) {
			uint64_t exp;
			read(panel->pollfds[FD_BATTERY].fd, &exp, sizeof(exp));
			plugin_battery_update(panel);
			render_frame(panel);
		}
	}
}

static const struct option long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"debug", no_argument, NULL, 'd'},
	{"help", no_argument, NULL, 'h'},
	{"output", required_argument, NULL, 'o'},
	{"verbose", no_argument, NULL, 'V'},
	{0, 0, 0, 0}
};

static const char t2conf_usage[] =
"Usage: t2conf [options...]\n"
"  -c, --config <file>      Specify config file (with path)\n"
"  -d, --debug              Enable full logging, including debug information\n"
"  -h, --help               Show help message and quit\n"
"  -o  --output <name>      Specify output (monitor)\n"
"  -V, --verbose            Enable more verbose logging\n";

static void
usage(void)
{
	printf("%s", t2conf_usage);
	exit(0);
}

int
main(int argc, char **argv)
{
	enum wlr_log_importance verbosity = WLR_ERROR;
	const char *config_file = NULL;

	struct conf conf = { 0 };
	conf_init(&conf);

	int c;
	while (1) {
		int index = 0;
		c = getopt_long(argc, argv, "c:dho:V", long_options, &index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			verbosity = WLR_DEBUG;
			break;
		case 'V':
			verbosity = WLR_INFO;
			break;
		case 'o':
			xstrdup_replace(conf.output, optarg);
			break;
		case 'h':
		default:
			usage();
		}
	}
	if (optind < argc) {
		usage();
	}

	wlr_log_init(verbosity, NULL);

	conf_load(&conf, config_file);

	struct panel panel = {
		.conf = &conf,
	};

	wl_list_init(&panel.outputs);
	wl_list_init(&panel.seats);
	wl_list_init(&panel.widgets);

	init_plugins(&panel);

	panel_setup(&panel);
	panel_run(&panel);
	panel_destroy(&panel);

	return EXIT_SUCCESS;
}
