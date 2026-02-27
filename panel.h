/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PANEL_H
#define PANEL_H
#include <cairo.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct panel;
struct seat;
struct widget;

struct pool_buffer {
	struct wl_buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cairo;
	PangoContext *pango;
	uint32_t width, height;
	void *data;
	size_t size;
	bool busy;
};

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

#define BUTTON_PADDING 8
#define BUTTON_MAX_WIDTH 200

enum widget_type {
	WIDGET_TOPLEVEL,
	WIDGET_CLOCK,
};

struct widget_impl {
	void (*on_left_button_press)(struct widget *widget, struct seat *seat);
};

struct widget {
	int x;
	int width;
	enum widget_type type;
	const struct widget_impl *impl;
	struct wl_list link; /* panel.widgets */
};

struct toplevel {
	struct widget base;
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title;
	char *app_id;
	bool active;
	struct panel *panel;
	struct wl_list link; /* panel.toplevels */
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
	struct panel *panel;
	struct pointer pointer;
	struct wl_list link; /* panel.seats */
};

struct output {
	char *name;
	struct wl_output *wl_output;
	uint32_t wl_name;
	uint32_t scale;
	struct panel *panel;
	struct wl_list link; /* panel.outputs */
};

enum {
	FD_WAYLAND,
	FD_SIGNAL,
	FD_CLOCK,

	NR_FDS,
};

struct panel {
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
	struct wl_list widgets; /* struct widget.link */

	uint32_t width;
	uint32_t height;
	int32_t scale;
	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;

	struct conf *conf;
	char *message;
	struct pollfd pollfds[NR_FDS];
};

struct pool_buffer *get_next_buffer(struct wl_shm *shm,
		struct pool_buffer pool[static 2], uint32_t width, uint32_t height);
void destroy_buffer(struct pool_buffer *buffer);
void render_text(cairo_t *cairo, const PangoFontDescription *desc, double scale,
	bool markup, const char *fmt, ...);
void get_text_size(cairo_t *cairo, const PangoFontDescription *desc, int *width,
	int *height, int *baseline, double scale, bool markup, const char *fmt, ...);
void cairo_set_source_u32(cairo_t *cairo, uint32_t color);

#endif /* PANEL_H */
