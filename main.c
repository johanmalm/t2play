// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cairo.h>
#include <ctype.h>
#include <getopt.h>
#include <glib.h>
#include <limits.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
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
#include <cyaml/cyaml.h>
#include "pool.h"
#include "cursor-shape-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct conf {
	PangoFontDescription *font_description;
	char *output;
	uint32_t anchors;
	int32_t layer; /* enum zwlr_layer_shell_v1_layer or -1 if unset */

	/* Colors */
	uint32_t background;
	uint32_t text;
	uint32_t button_background;
	uint32_t button_active;

	/* Panel layout: string of item codes, e.g. "TC" (T=Taskbar, C=Clock) */
	char *panel_items;
};

struct nag;

#define BUTTON_PADDING 8
#define BUTTON_MAX_WIDTH 200

struct toplevel {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title;
	char *app_id;
	bool active;
	struct nag *nag;
	/* button geometry for click detection */
	int btn_x;
	int btn_width;
	struct wl_list link; /* nag.toplevels */
};

struct pointer {
	struct wl_pointer *pointer;
	uint32_t serial;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_image;
	struct wl_surface *cursor_surface;
	int x;
	int y;
};

struct seat {
	struct wl_seat *wl_seat;
	uint32_t wl_name;
	struct nag *nag;
	struct pointer pointer;
	struct wl_list link; /* nag.seats */
};

struct output {
	char *name;
	struct wl_output *wl_output;
	uint32_t wl_name;
	uint32_t scale;
	struct nag *nag;
	struct wl_list link; /* nag.outputs */
};

enum {
	FD_WAYLAND,
	FD_TIMER,
	FD_SIGNAL,
	FD_CLOCK,

	NR_FDS,
};

struct nag {
	bool run_display;

	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_shm *shm;
	struct wl_list outputs;
	struct wl_list seats;
	struct output *output;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wl_surface *surface;

	struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
	struct wl_list toplevels; /* struct toplevel.link */

	uint32_t width;
	uint32_t height;
	int32_t scale;
	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;

	struct conf *conf;
	char *message;
	struct pollfd pollfds[NR_FDS];

	struct {
		bool visible;
		char *message;
		char *details_text;
		int close_timeout;
		bool use_exclusive_zone;

		int x;
		int y;
		int width;
		int height;

		int offset;
		int visible_lines;
		int total_lines;
	} details;
};

static void close_pollfd(struct pollfd *pollfd);

static PangoLayout *
get_pango_layout(cairo_t *cairo, const PangoFontDescription *desc,
		const char *text, double scale, bool markup)
{
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);

	PangoAttrList *attrs;
	if (markup) {
		char *buf;
		GError *error = NULL;
		if (pango_parse_markup(text, -1, 0, &attrs, &buf, NULL, &error)) {
			pango_layout_set_text(layout, buf, -1);
			free(buf);
		} else {
			wlr_log(WLR_ERROR, "pango_parse_markup '%s' -> error %s",
				text, error->message);
			g_error_free(error);
			markup = false; /* fallback to plain text */
		}
	}
	if (!markup) {
		attrs = pango_attr_list_new();
		pango_layout_set_text(layout, text, -1);
	}

	pango_attr_list_insert(attrs, pango_attr_scale_new(scale));
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_single_paragraph_mode(layout, 1);
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
	return layout;
}

static void
get_text_size(cairo_t *cairo, const PangoFontDescription *desc, int *width, int *height,
		int *baseline, double scale, bool markup, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}

	PangoLayout *layout = get_pango_layout(cairo, desc, buf, scale, markup);
	pango_cairo_update_layout(cairo, layout);
	pango_layout_get_pixel_size(layout, width, height);
	if (baseline) {
		*baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	}
	g_object_unref(layout);

	g_free(buf);
}

static void
render_text(cairo_t *cairo, const PangoFontDescription *desc, double scale,
		bool markup, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}

	PangoLayout *layout = get_pango_layout(cairo, desc, buf, scale, markup);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_get_font_options(cairo, fo);
	pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
	cairo_font_options_destroy(fo);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);

	g_free(buf);
}

static void
cairo_set_source_u32(cairo_t *cairo, uint32_t color)
{
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

static void
render_taskbar(cairo_t *cairo, struct nag *nag)
{
	int x = BUTTON_PADDING;
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &nag->toplevels, link) {
		const char *label = toplevel->title ? toplevel->title
			: (toplevel->app_id ? toplevel->app_id : "?");

		int text_width, text_height;
		get_text_size(cairo, nag->conf->font_description,
			&text_width, &text_height, NULL, 1, false, "%s", label);

		int btn_width = text_width + 2 * BUTTON_PADDING;
		if (btn_width > BUTTON_MAX_WIDTH) {
			btn_width = BUTTON_MAX_WIDTH;
		}

		/* Record button geometry for click detection */
		toplevel->btn_x = x;
		toplevel->btn_width = btn_width;

		/* Draw button background */
		if (toplevel->active) {
			cairo_set_source_u32(cairo, nag->conf->button_active);
		} else {
			cairo_set_source_u32(cairo, nag->conf->button_background);
		}
		cairo_rectangle(cairo, x, 2, btn_width, nag->height - 4);
		cairo_fill(cairo);

		/* Draw button label, clipped to button width */
		cairo_save(cairo);
		cairo_rectangle(cairo, x + BUTTON_PADDING, 0, btn_width - 2 * BUTTON_PADDING, nag->height);
		cairo_clip(cairo);
		cairo_set_source_u32(cairo, nag->conf->text);
		cairo_move_to(cairo, x + BUTTON_PADDING,
			(nag->height - text_height) / 2);
		render_text(cairo, nag->conf->font_description, 1, false, "%s", label);
		cairo_restore(cairo);

		x += btn_width + BUTTON_PADDING;
	}
}

static void
render_clock(cairo_t *cairo, struct nag *nag)
{
	time_t t = time(NULL);
	struct tm *tm_info = localtime(&t);
	char buf[6]; /* "HH:MM\0" */
	strftime(buf, sizeof(buf), "%H:%M", tm_info);

	int text_width, text_height;
	get_text_size(cairo, nag->conf->font_description,
		&text_width, &text_height, NULL, 1, false, "%s", buf);

	int x = nag->width - text_width - BUTTON_PADDING;
	cairo_set_source_u32(cairo, nag->conf->text);
	cairo_move_to(cairo, x, (nag->height - text_height) / 2.0);
	render_text(cairo, nag->conf->font_description, 1, false, "%s", buf);
}

static void
render_to_cairo(cairo_t *cairo, struct nag *nag)
{
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, nag->conf->background);
	cairo_paint(cairo);

	if (nag->conf->panel_items) {
		for (const char *p = nag->conf->panel_items; *p; p++) {
			if (*p == 'T') {
				render_taskbar(cairo, nag);
			} else if (*p == 'C') {
				render_clock(cairo, nag);
			} else {
				wlr_log(WLR_ERROR, "Unknown panel_items code '%c'", *p);
			}
		}
	}

	cairo_set_source_u32(cairo, nag->conf->text);
	cairo_rectangle(cairo, 0, nag->height, nag->width, 1);
	cairo_fill(cairo);
}

static void
render_frame(struct nag *nag)
{
	if (!nag->run_display || !nag->width || !nag->height) {
		return;
	}

	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_scale(cairo, nag->scale, nag->scale);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	render_to_cairo(cairo, nag);

	nag->current_buffer = get_next_buffer(nag->shm, nag->buffers,
		nag->width * nag->scale, nag->height * nag->scale);
	if (!nag->current_buffer) {
		goto cleanup;
	}

	cairo_t *shm = nag->current_buffer->cairo;
	cairo_save(shm);
	cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
	cairo_paint(shm);
	cairo_restore(shm);
	cairo_set_source_surface(shm, recorder, 0.0, 0.0);
	cairo_paint(shm);

	wl_surface_set_buffer_scale(nag->surface, nag->scale);
	wl_surface_attach(nag->surface, nag->current_buffer->buffer, 0, 0);
	wl_surface_damage(nag->surface, 0, 0, nag->width, nag->height);
	wl_surface_commit(nag->surface);
	wl_display_roundtrip(nag->display);

cleanup:
	cairo_surface_destroy(recorder);
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
	wl_seat_destroy(seat->wl_seat);
	wl_list_remove(&seat->link);
	free(seat);
}

static void
toplevel_destroy(struct toplevel *toplevel)
{
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
	render_frame(toplevel->nag);
}

static void
handle_toplevel_closed(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	struct nag *nag = toplevel->nag;
	toplevel_destroy(toplevel);
	render_frame(nag);
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
	struct nag *nag = data;
	struct toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		perror("calloc");
		return;
	}
	toplevel->handle = handle;
	toplevel->nag = nag;
	wl_list_insert(nag->toplevels.prev, &toplevel->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle,
		&toplevel_handle_listener, toplevel);
}

static void
handle_toplevel_manager_finished(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager)
{
	struct nag *nag = data;
	zwlr_foreign_toplevel_manager_v1_destroy(nag->toplevel_manager);
	nag->toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = handle_toplevel_manager_toplevel,
	.finished = handle_toplevel_manager_finished,
};

static void
nag_destroy(struct nag *nag)
{
	nag->run_display = false;

	pango_font_description_free(nag->conf->font_description);
	free(nag->conf->panel_items);
	nag->conf->panel_items = NULL;

	if (nag->layer_surface) {
		zwlr_layer_surface_v1_destroy(nag->layer_surface);
	}

	if (nag->surface) {
		wl_surface_destroy(nag->surface);
	}

	if (nag->layer_shell) {
		zwlr_layer_shell_v1_destroy(nag->layer_shell);
	}

	if (nag->cursor_shape_manager) {
		wp_cursor_shape_manager_v1_destroy(nag->cursor_shape_manager);
	}

	struct seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &nag->seats, link) {
		seat_destroy(seat);
	}

	struct toplevel *toplevel, *tmptoplevel;
	wl_list_for_each_safe(toplevel, tmptoplevel, &nag->toplevels, link) {
		toplevel_destroy(toplevel);
	}

	if (nag->toplevel_manager) {
		zwlr_foreign_toplevel_manager_v1_destroy(nag->toplevel_manager);
		nag->toplevel_manager = NULL;
	}

	destroy_buffer(&nag->buffers[0]);
	destroy_buffer(&nag->buffers[1]);

	if (nag->outputs.prev || nag->outputs.next) {
		struct output *output, *temp;
		wl_list_for_each_safe(output, temp, &nag->outputs, link) {
			wl_output_destroy(output->wl_output);
			free(output->name);
			wl_list_remove(&output->link);
			free(output);
		};
	}

	if (nag->compositor) {
		wl_compositor_destroy(nag->compositor);
	}

	if (nag->shm) {
		wl_shm_destroy(nag->shm);
	}

	if (nag->display) {
		wl_display_disconnect(nag->display);
	}
	pango_cairo_font_map_set_default(NULL);

	close_pollfd(&nag->pollfds[FD_TIMER]);
	close_pollfd(&nag->pollfds[FD_SIGNAL]);
	close_pollfd(&nag->pollfds[FD_CLOCK]);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct nag *nag = data;
	nag->width = width;
	nag->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_frame(nag);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct nag *nag = data;
	nag_destroy(nag);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	struct nag *nag = data;
	struct output *nag_output;
	wl_list_for_each(nag_output, &nag->outputs, link) {
		if (nag_output->wl_output == output) {
			wlr_log(WLR_DEBUG, "Surface enter on output %s",
					nag_output->name);
			nag->output = nag_output;
			nag->scale = nag->output->scale;
			render_frame(nag);
			break;
		}
	}
}

static void
surface_leave(void *data, struct wl_surface *wl_surface, struct wl_output *output)
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
	struct nag *nag = seat->nag;
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
	pointer->cursor_theme = wl_cursor_theme_load(
		cursor_theme, cursor_size * nag->scale, nag->shm);
	if (!pointer->cursor_theme) {
		wlr_log(WLR_ERROR, "Failed to load cursor theme");
		return;
	}
	struct wl_cursor *cursor =
		wl_cursor_theme_get_cursor(pointer->cursor_theme, "default");
	if (!cursor) {
		wlr_log(WLR_ERROR, "Failed to get default cursor from theme");
		return;
	}
	pointer->cursor_image = cursor->images[0];
	wl_surface_set_buffer_scale(pointer->cursor_surface,
			nag->scale);
	wl_surface_attach(pointer->cursor_surface,
			wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(pointer->pointer, pointer->serial,
			pointer->cursor_surface,
			pointer->cursor_image->hotspot_x / nag->scale,
			pointer->cursor_image->hotspot_y / nag->scale);
	wl_surface_damage_buffer(pointer->cursor_surface, 0, 0,
			INT32_MAX, INT32_MAX);
	wl_surface_commit(pointer->cursor_surface);
}

static void
update_all_cursors(struct nag *nag)
{
	struct seat *seat;
	wl_list_for_each(seat, &nag->seats, link) {
		if (seat->pointer.pointer) {
			update_cursor(seat);
		}
	}
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t surface_x,
		wl_fixed_t surface_y)
{
	struct seat *seat = data;

	struct pointer *pointer = &seat->pointer;
	pointer->x = wl_fixed_to_int(surface_x);
	pointer->y = wl_fixed_to_int(surface_y);

	if (seat->nag->cursor_shape_manager) {
		struct wp_cursor_shape_device_v1 *device =
			wp_cursor_shape_manager_v1_get_pointer(
				seat->nag->cursor_shape_manager, wl_pointer);
		wp_cursor_shape_device_v1_set_shape(device, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
	} else {
		pointer->serial = serial;
		update_cursor(seat);
	}
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface)
{
	/* nop */
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct seat *seat = data;
	seat->pointer.x = wl_fixed_to_int(surface_x);
	seat->pointer.y = wl_fixed_to_int(surface_y);
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state)
{
	struct seat *seat = data;
	struct nag *nag = seat->nag;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	int x = seat->pointer.x;

	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &nag->toplevels, link) {
		if (x >= toplevel->btn_x && x < toplevel->btn_x + toplevel->btn_width) {
			zwlr_foreign_toplevel_handle_v1_activate(
				toplevel->handle, seat->wl_seat);
			break;
		}
	}
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value)
{
	/* nop */
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
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis)
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
		int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform)
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
	struct output *nag_output = data;
	nag_output->scale = factor;
	if (nag_output->nag->output == nag_output) {
		nag_output->nag->scale = nag_output->scale;
		if (!nag_output->nag->cursor_shape_manager) {
			update_all_cursors(nag_output->nag);
		}
		render_frame(nag_output->nag);
	}
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
	struct output *nag_output = data;
	nag_output->name = strdup(name);

	const char *outname = nag_output->nag->conf->output;
	if (!nag_output->nag->output && outname &&
			strcmp(outname, name) == 0) {
		wlr_log(WLR_DEBUG, "Using output %s", name);
		nag_output->nag->output = nag_output;
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
handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	struct nag *nag = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		nag->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct seat *seat = calloc(1, sizeof(*seat));
		if (!seat) {
			perror("calloc");
			return;
		}

		seat->nag = nag;
		seat->wl_name = name;
		seat->wl_seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 5);

		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);

		wl_list_insert(&nag->seats, &seat->link);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		nag->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (!nag->output) {
			struct output *output = calloc(1, sizeof(*output));
			if (!output) {
				perror("calloc");
				return;
			}
			output->wl_output = wl_registry_bind(registry, name,
					&wl_output_interface, 4);
			output->wl_name = name;
			output->scale = 1;
			output->nag = nag;
			wl_list_insert(&nag->outputs, &output->link);
			wl_output_add_listener(output->wl_output,
					&output_listener, output);
		}
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		nag->layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		nag->cursor_shape_manager = wl_registry_bind(
				registry, name, &wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface,
			zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		nag->toplevel_manager = wl_registry_bind(registry, name,
				&zwlr_foreign_toplevel_manager_v1_interface, 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(
				nag->toplevel_manager,
				&toplevel_manager_listener, nag);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct nag *nag = data;
	if (nag->output->wl_name == name) {
		nag->run_display = false;
	}

	struct seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &nag->seats, link) {
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
nag_setup_cursors(struct nag *nag)
{
	struct seat *seat;

	wl_list_for_each(seat, &nag->seats, link) {
		struct pointer *p = &seat->pointer;

		p->cursor_surface =
			wl_compositor_create_surface(nag->compositor);
		assert(p->cursor_surface);
	}
}

static void
nag_setup(struct nag *nag)
{
	nag->display = wl_display_connect(NULL);
	if (!nag->display) {
		wlr_log(WLR_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		exit(EXIT_FAILURE);
	}

	nag->scale = 1;

	struct wl_registry *registry = wl_display_get_registry(nag->display);
	wl_registry_add_listener(registry, &registry_listener, nag);
	if (wl_display_roundtrip(nag->display) < 0) {
		wlr_log(WLR_ERROR, "failed to register with the wayland display");
		exit(EXIT_FAILURE);
	}

	assert(nag->compositor && nag->layer_shell && nag->shm);

	/* Second roundtrip to get wl_output properties */
	if (wl_display_roundtrip(nag->display) < 0) {
		wlr_log(WLR_ERROR, "Error during outputs init.");
		nag_destroy(nag);
		exit(EXIT_FAILURE);
	}

	if (!nag->output && nag->conf->output) {
		wlr_log(WLR_ERROR, "Output '%s' not found", nag->conf->output);
		nag_destroy(nag);
		exit(EXIT_FAILURE);
	}

	if (!nag->cursor_shape_manager) {
		nag_setup_cursors(nag);
	}

	nag->surface = wl_compositor_create_surface(nag->compositor);
	assert(nag->surface);
	wl_surface_add_listener(nag->surface, &surface_listener, nag);

	nag->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			nag->layer_shell, nag->surface,
			nag->output ? nag->output->wl_output : NULL,
			nag->conf->layer,
			"nag");
	assert(nag->layer_surface);
	zwlr_layer_surface_v1_add_listener(nag->layer_surface,
			&layer_surface_listener, nag);
	zwlr_layer_surface_v1_set_anchor(nag->layer_surface,
			nag->conf->anchors);

	wl_registry_destroy(registry);

	nag->pollfds[FD_WAYLAND].fd = wl_display_get_fd(nag->display);
	nag->pollfds[FD_WAYLAND].events = POLLIN;

	if (nag->details.close_timeout != 0) {
		nag->pollfds[FD_TIMER].fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		nag->pollfds[FD_TIMER].events = POLLIN;
		struct itimerspec timeout = {
			.it_value.tv_sec = nag->details.close_timeout,
		};
		timerfd_settime(nag->pollfds[FD_TIMER].fd, 0, &timeout, NULL);
	} else {
		nag->pollfds[FD_TIMER].fd = -1;
	}

	if (nag->conf->panel_items && strchr(nag->conf->panel_items, 'C')) {
		nag->pollfds[FD_CLOCK].fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
		nag->pollfds[FD_CLOCK].events = POLLIN;
		/* Fire at the start of the next minute, then every 60 seconds */
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		struct itimerspec clock_timer = {
			.it_interval.tv_sec = 60,
			.it_value.tv_sec = now.tv_sec - (now.tv_sec % 60) + 60,
		};
		timerfd_settime(nag->pollfds[FD_CLOCK].fd,
			TFD_TIMER_ABSTIME, &clock_timer, NULL);
	} else {
		nag->pollfds[FD_CLOCK].fd = -1;
	}

	sigset_t mask;
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	nag->pollfds[FD_SIGNAL].fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	nag->pollfds[FD_SIGNAL].events = POLLIN;
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
nag_run(struct nag *nag)
{
	nag->run_display = true;

	zwlr_layer_surface_v1_set_size(nag->layer_surface, 0, 30);
	zwlr_layer_surface_v1_set_exclusive_zone(nag->layer_surface, 30);
	wl_surface_commit(nag->surface);
	wl_display_roundtrip(nag->display);

	render_frame(nag);
	while (nag->run_display) {
		while (wl_display_prepare_read(nag->display) != 0) {
			wl_display_dispatch_pending(nag->display);
		}

		errno = 0;
		if (wl_display_flush(nag->display) == -1 && errno != EAGAIN) {
			break;
		}

		if (!nag->run_display) {
			break;
		}

		poll(nag->pollfds, NR_FDS, -1);
		if (nag->pollfds[FD_WAYLAND].revents & POLLIN) {
			wl_display_read_events(nag->display);
		} else {
			wl_display_cancel_read(nag->display);
		}
		if (nag->pollfds[FD_TIMER].revents & POLLIN) {
			break;
		}
		if (nag->pollfds[FD_SIGNAL].revents & POLLIN) {
			break;
		}
		if (nag->pollfds[FD_CLOCK].revents & POLLIN) {
			uint64_t exp;
			read(nag->pollfds[FD_CLOCK].fd, &exp, sizeof(exp));
			render_frame(nag);
		}
	}
}

struct yaml_conf {
	char *panel_items;
};

static const cyaml_schema_field_t yaml_conf_fields[] = {
	CYAML_FIELD_STRING_PTR("panel_items", CYAML_FLAG_OPTIONAL,
		struct yaml_conf, panel_items, 0, CYAML_UNLIMITED),
	CYAML_FIELD_END
};

static const cyaml_schema_value_t yaml_conf_schema = {
	CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, struct yaml_conf, yaml_conf_fields),
};

static const cyaml_config_t yaml_cyaml_config = {
	.log_fn = cyaml_log,
	.mem_fn = cyaml_mem,
	.log_level = CYAML_LOG_WARNING,
	.flags = CYAML_CFG_IGNORE_UNKNOWN_KEYS,
};

static void
load_config(struct conf *conf, const char *path)
{
	struct yaml_conf *data = NULL;
	cyaml_err_t err = cyaml_load_file(path, &yaml_cyaml_config,
		&yaml_conf_schema, (cyaml_data_t **)&data, NULL);
	if (err == CYAML_ERR_FILE_OPEN) {
		return;
	}
	if (err != CYAML_OK) {
		wlr_log(WLR_ERROR, "Failed to load config '%s': %s",
			path, cyaml_strerror(err));
		return;
	}
	if (data) {
		if (data->panel_items) {
			free(conf->panel_items);
			conf->panel_items = data->panel_items;
			data->panel_items = NULL;
		}
		cyaml_free(&yaml_cyaml_config, &yaml_conf_schema, data, 0);
	}
}

static void
conf_init(struct conf *conf)
{
	conf->font_description = pango_font_description_from_string("pango:Sans 10");
	conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	conf->background = 0x323232FF;
	conf->text = 0xFFFFFFFF;
	conf->button_background = 0x4A4A4AFF;
	conf->button_active = 0x5A8AC6FF;
	conf->panel_items = strdup("TC");
}

int
main(int argc, char **argv)
{
	struct conf conf = { 0 };
	conf_init(&conf);

	/* Load config file from $XDG_CONFIG_HOME/t2play/config.yaml */
	char config_path[PATH_MAX];
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	if (xdg_config_home && *xdg_config_home) {
		int n = snprintf(config_path, sizeof(config_path),
			"%s/t2play/config.yaml", xdg_config_home);
		if (n < 0 || (size_t)n >= sizeof(config_path)) {
			config_path[0] = '\0';
		}
	} else {
		const char *home = getenv("HOME");
		if (home) {
			int n = snprintf(config_path, sizeof(config_path),
				"%s/.config/t2play/config.yaml", home);
			if (n < 0 || (size_t)n >= sizeof(config_path)) {
				config_path[0] = '\0';
			}
		} else {
			config_path[0] = '\0';
		}
	}
	if (config_path[0]) {
		load_config(&conf, config_path);
	}

	struct nag nag = {
		.conf = &conf,
	};

	wl_list_init(&nag.outputs);
	wl_list_init(&nag.seats);
	wl_list_init(&nag.toplevels);

	wlr_log_init(WLR_DEBUG, NULL);

	nag_setup(&nag);
	nag_run(&nag);
	nag_destroy(&nag);

	return EXIT_SUCCESS;
}
