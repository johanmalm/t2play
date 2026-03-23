/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PANEL_H
#define PANEL_H
#include <cairo.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"

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

enum widget_type {
	/* Plugins */
	WIDGET_PLUGINS_BEGIN = 0,
	WIDGET_STARTMENU,
	WIDGET_TASKBAR,
	WIDGET_CLOCK,
	WIDGET_KBDLAYOUT,
	WIDGET_BATTERY,
	WIDGET_PLUGINS_END,

	/* Other */
	WIDGET_TOPLEVEL, /* Child of taskbar plugin */
};

struct widget_impl {
	void (*on_left_button_press)(struct widget *widget, struct seat *seat);
};

/* Base class */
struct widget {
	int x;
	int width;
	enum widget_type type;
	cairo_surface_t *surface;
	const struct widget_impl *impl;
	struct panel *panel;
	struct wl_list link; /* panel.widgets */
};

/* Derived classes */
struct clock {
	struct widget base;
};

struct kbdlayout {
	struct widget base;
};

struct battery {
	struct widget base;
};

struct taskbar {
	struct widget base;
};

struct startmenu {
	struct widget base;
	int hover;    /* highlighted item under mouse pointer, or -1 */
	int selected; /* highlighted item via keyboard, or -1 */
	bool popup_open;
	struct wl_surface *popup_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_popup *xdg_popup;
	struct pool_buffer popup_buffers[2];

	/* Application list loaded from .desktop files */
	char **app_names;  /* display names */
	char **app_execs;  /* executable paths (exec arg0) */
	int n_apps;        /* total number of apps */

	/* Type-to-search state */
	char search[256];
	int search_len;

	/* Filtered / scrollable view */
	int *filtered;      /* indices into app_names/app_execs that match search */
	int n_filtered;     /* number of matching apps */
	int scroll_offset;  /* index of first visible item in filtered[] */
	int n_visible;      /* max items shown at once (set when popup opens) */
};

struct toplevel {
	struct widget base;
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title;
	char *app_id;
	bool active;
};

struct pointer {
	struct wl_pointer *pointer;
	uint32_t serial;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_image;
	struct wl_surface *cursor_surface;
	int x;
	int y;
	uint32_t button_serial;
	struct wl_surface *focus_surface;
};

struct seat {
	struct wl_seat *wl_seat;
	uint32_t wl_name;
	struct panel *panel;
	struct pointer pointer;
	struct wl_keyboard *keyboard;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
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
	FD_BATTERY,

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
	struct xdg_wm_base *xdg_wm_base;

	struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
	struct wl_list widgets; /* struct widget.link */
	struct startmenu *open_popup; /* currently open start menu popup */

	uint32_t width;
	uint32_t height;
	int32_t scale;
	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;

	char kbd_layout[64]; /* current keyboard layout name */
	char battery_path[64]; /* path to battery capacity file */

	struct conf *conf;
	char *message;
	struct pollfd pollfds[NR_FDS];
};

void render_frame(struct panel *panel);

struct pool_buffer *get_next_buffer(struct wl_shm *shm,
	struct pool_buffer pool[static 2], uint32_t width, uint32_t height);
void destroy_buffer(struct pool_buffer *buffer);

void render_text(cairo_t *cairo, const PangoFontDescription *desc, double scale,
	bool markup, const char *fmt, ...);
PangoRectangle get_text_size(const PangoFontDescription *desc,
	const char *string);
void cairo_set_source_u32(cairo_t *cairo, uint32_t color);

void plugin_taskbar_init(struct panel *panel);
void plugin_taskbar_create(struct panel *panel);
void toplevel_destroy(struct toplevel *toplevel);
struct toplevel *toplevel_from_widget(struct widget *widget);

void plugin_clock_update(struct panel *panel);
void plugin_clock_create(struct panel *panel);

void plugin_kbdlayout_update(struct panel *panel);
void plugin_kbdlayout_create(struct panel *panel);

void plugin_battery_create(struct panel *panel);
void plugin_battery_update(struct panel *panel);

void plugin_startmenu_create(struct panel *panel);
void plugin_startmenu_update(struct panel *panel);
void plugin_startmenu_destroy(struct startmenu *menu);
void plugin_startmenu_key(struct panel *panel, uint32_t key);
void plugin_startmenu_text_input(struct panel *panel, const char *utf8);
void plugin_startmenu_pointer_motion(struct startmenu *menu, int y);
void plugin_startmenu_pointer_leave(struct startmenu *menu);
void plugin_startmenu_popup_click(struct startmenu *menu, int y);
void plugin_startmenu_scroll(struct startmenu *menu, double delta);

void widget_on_left_button_press(struct widget *widget, struct seat *seat);
char *widget_type(enum widget_type type);
bool widget_is_plugin(struct widget *widget);
void widget_free(struct widget *widget);
void widgets_free(struct panel *panel);

#endif /* PANEL_H */
