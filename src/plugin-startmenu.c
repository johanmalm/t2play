// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sfdo-basedir.h>
#include <sfdo-desktop.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "conf.h"
#include "common/mem.h"
#include "panel.h"
#include "common/string-helpers.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MENU_ITEM_HEIGHT 30
#define MENU_WIDTH 200

/* Maximum number of items visible at once (excluding the search box row) */
#define MENU_MAX_VISIBLE 12

/* ------------------------- Popup UI mini-framework ------------------------ */

struct sm_rect {
	int x;
	int y;
	int w;
	int h;
};

enum sm_node_type {
	SM_NODE_HBOX,
	SM_NODE_VBOX,
	SM_NODE_SEARCH,
	SM_NODE_APPLIST,
};

struct sm_node {
	enum sm_node_type type;
	struct sm_node **children;
	int n_children;
};

static void sm_node_free(struct sm_node *n)
{
	if (!n) {
		return;
	}
	for (int i = 0; i < n->n_children; i++) {
		sm_node_free(n->children[i]);
	}
	free(n->children);
	free(n);
}

static struct sm_node *sm_node_new(enum sm_node_type type)
{
	struct sm_node *n = calloc(1, sizeof(*n));
	if (!n) {
		return NULL;
	}
	n->type = type;
	return n;
}

static bool sm_node_add_child(struct sm_node *parent, struct sm_node *child)
{
	if (!parent || !child) {
		return false;
	}
	struct sm_node **new_children =
		realloc(parent->children, (parent->n_children + 1) * sizeof(*new_children));
	if (!new_children) {
		return false;
	}
	parent->children = new_children;
	parent->children[parent->n_children++] = child;
	return true;
}

static enum sm_node_type sm_name_to_type(const char *name)
{
	if (strcasecmp(name, "vbox") == 0) {
		return SM_NODE_VBOX;
	}
	if (strcasecmp(name, "hbox") == 0) {
		return SM_NODE_HBOX;
	}
	if (strcasecmp(name, "search") == 0) {
		return SM_NODE_SEARCH;
	}
	if (strcasecmp(name, "applist") == 0) {
		return SM_NODE_APPLIST;
	}
	return -1;
}

static struct sm_node *sm_from_xml_node(xmlNode *xn)
{
	if (!xn || xn->type != XML_ELEMENT_NODE) {
		return NULL;
	}

	const char *name = (const char *)xn->name;
	enum sm_node_type type = sm_name_to_type(name);
	if ((int)type < 0) {
		/* Forgiving: unknown tags are ignored, except the toplevel. */
		return NULL;
	}

	struct sm_node *node = sm_node_new(type);
	if (!node) {
		return NULL;
	}

	if (type == SM_NODE_SEARCH || type == SM_NODE_APPLIST) {
		/* Forgiving: ignore children even if present. */
		return node;
	}

	for (xmlNode *c = xn->children; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE) {
			continue;
		}
		enum sm_node_type ct = sm_name_to_type((const char *)c->name);
		if ((int)ct < 0) {
			/* Forgiving: ignore unknown elements in containers. */
			continue;
		}
		struct sm_node *child = sm_from_xml_node(c);
		if (!child) {
			continue;
		}
		if (!sm_node_add_child(node, child)) {
			sm_node_free(child);
			sm_node_free(node);
			return NULL;
		}
	}

	return node;
}

static struct sm_node *sm_parse_layout(const char *xml)
{
	if (string_null_or_empty(xml)) {
		return NULL;
	}

	xmlDocPtr doc = xmlReadMemory(
		xml,
		(int)strlen(xml),
		"startmenu_layout.xml",
		NULL,
		XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS
	);
	if (!doc) {
		return NULL;
	}

	xmlNode *root = xmlDocGetRootElement(doc);
	if (!root || root->type != XML_ELEMENT_NODE) {
		xmlFreeDoc(doc);
		return NULL;
	}
	enum sm_node_type root_type = sm_name_to_type((const char *)root->name);
	if (root_type != SM_NODE_VBOX && root_type != SM_NODE_HBOX) {
		xmlFreeDoc(doc);
		return NULL;
	}

	struct sm_node *out = sm_from_xml_node(root);
	xmlFreeDoc(doc);
	return out;
}

static void sm_measure(struct startmenu *menu, const struct sm_node *n,
	int *out_w, int *out_h)
{
	if (!n) {
		*out_w = 0;
		*out_h = 0;
		return;
	}

	switch (n->type) {
	case SM_NODE_SEARCH:
		*out_w = 0;
		*out_h = MENU_ITEM_HEIGHT;
		return;
	case SM_NODE_APPLIST:
		*out_w = 0;
		*out_h = menu->n_visible * MENU_ITEM_HEIGHT;
		return;
	case SM_NODE_VBOX: {
		int w = 0, h = 0;
		for (int i = 0; i < n->n_children; i++) {
			int cw = 0, ch = 0;
			sm_measure(menu, n->children[i], &cw, &ch);
			if (cw > w) {
				w = cw;
			}
			h += ch;
		}
		*out_w = w;
		*out_h = h;
		return;
	}
	case SM_NODE_HBOX: {
		int h = 0;
		for (int i = 0; i < n->n_children; i++) {
			int cw = 0, ch = 0;
			sm_measure(menu, n->children[i], &cw, &ch);
			if (ch > h) {
				h = ch;
			}
		}
		*out_w = 0;
		*out_h = h;
		return;
	}
	}
}

static void sm_layout_and_cache(struct startmenu *menu, const struct sm_node *n,
	struct sm_rect r)
{
	if (!n) {
		return;
	}

	switch (n->type) {
	case SM_NODE_SEARCH:
		menu->ui_search_x = r.x;
		menu->ui_search_y = r.y;
		menu->ui_search_w = r.w;
		menu->ui_search_h = r.h;
		return;
	case SM_NODE_APPLIST:
		menu->ui_list_x = r.x;
		menu->ui_list_y = r.y;
		menu->ui_list_w = r.w;
		menu->ui_list_h = r.h;
		return;
	case SM_NODE_VBOX: {
		int y = r.y;
		for (int i = 0; i < n->n_children; i++) {
			int cw = 0, ch = 0;
			sm_measure(menu, n->children[i], &cw, &ch);
			struct sm_rect cr = {
				.x = r.x,
				.y = y,
				.w = r.w,
				.h = ch,
			};
			sm_layout_and_cache(menu, n->children[i], cr);
			y += ch;
		}
		return;
	}
	case SM_NODE_HBOX: {
		int x = r.x;
		int n_children = n->n_children > 0 ? n->n_children : 1;
		int base_w = r.w / n_children;
		int rem = r.w - base_w * n_children;
		for (int i = 0; i < n->n_children; i++) {
			int cw = 0, ch = 0;
			sm_measure(menu, n->children[i], &cw, &ch);
			(void)cw;
			(void)ch;
			int w = base_w + (i == n->n_children - 1 ? rem : 0);
			struct sm_rect cr = {
				.x = x,
				.y = r.y,
				.w = w,
				.h = r.h,
			};
			sm_layout_and_cache(menu, n->children[i], cr);
			x += w;
		}
		return;
	}
	}
}

/* Forward declaration */
static void startmenu_close(struct startmenu *menu);
static void startmenu_render_popup(struct startmenu *menu);

/*
 * Case-insensitive substring search (portable, no strcasestr needed).
 */
static bool
str_contains_icase(const char *haystack, const char *needle)
{
	if (!haystack) {
		return false;
	}
	if (string_null_or_empty(needle)) {
		return true;
	}
	size_t nlen = strlen(needle);
	for (; *haystack; haystack++) {
		if (strncasecmp(haystack, needle, nlen) == 0) {
			return true;
		}
	}
	return false;
}

/*
 * Rebuild the filtered[] index array based on the current search string.
 * Resets scroll_offset, hover, and selected.
 */
static void
update_filtered(struct startmenu *menu)
{
	zfree(menu->filtered);
	menu->n_filtered = 0;
	menu->scroll_offset = 0;
	menu->hover = -1;
	menu->selected = -1;

	if (menu->n_apps == 0) {
		return;
	}

	menu->filtered = calloc(menu->n_apps, sizeof(int));
	if (!menu->filtered) {
		return;
	}

	for (int i = 0; i < menu->n_apps; i++) {
		if (str_contains_icase(menu->app_names[i], menu->search)) {
			menu->filtered[menu->n_filtered++] = i;
		}
	}
}

/*
 * Launch an application by its executable name/path using a double-fork
 * so the grandchild is orphaned and no zombie is left.
 */
static void
launch_app(const char *exec)
{
	if (string_null_or_empty(exec)) {
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "startmenu: fork failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		pid_t grandchild = fork();
		if (grandchild == 0) {
			setsid();
			/* Redirect stdin/stdout/stderr to /dev/null */
			int devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			/* Execute via the shell to handle relative paths / PATH lookup */
			execl("/bin/sh", "sh", "-c", exec, NULL);
			_exit(127);
		}
		_exit(0);
	}
	/* Parent: wait for first child (exits immediately) */
	waitpid(pid, NULL, 0);
}

static void
startmenu_render_button(struct startmenu *menu)
{
	struct widget *widget = &menu->base;
	struct panel *panel = widget->panel;

	const char *label = "^";
	PangoRectangle rect = get_text_size(panel->conf->font_description, label);
	widget->width = rect.width + 2 * panel->conf->taskbar_padding;

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
	cairo_move_to(cr, panel->conf->taskbar_padding, (panel->height - rect.height) / 2.0);
	render_text(cr, panel->conf->font_description, 1, false, "%s", label);
	cairo_destroy(cr);
}

static void
startmenu_render_popup(struct startmenu *menu)
{
	struct panel *panel = menu->base.panel;
	int width = MENU_WIDTH;
	int height = (1 + menu->n_visible) * MENU_ITEM_HEIGHT;

	/* Layout pass */
	menu->ui_search_x = 0;
	menu->ui_search_y = 0;
	menu->ui_search_w = 0;
	menu->ui_search_h = 0;
	menu->ui_list_x = 0;
	menu->ui_list_y = 0;
	menu->ui_list_w = 0;
	menu->ui_list_h = 0;
	if (menu->ui_root) {
		int mw = 0, mh = 0;
		sm_measure(menu, (const struct sm_node *)menu->ui_root, &mw, &mh);
		if (mh > 0) {
			height = mh;
		}
		sm_layout_and_cache(menu, (const struct sm_node *)menu->ui_root,
			(struct sm_rect){ .x = 0, .y = 0, .w = width, .h = height });
	} else {
		/* Fallback to the classic layout. */
		menu->ui_search_x = 0;
		menu->ui_search_y = 0;
		menu->ui_search_w = width;
		menu->ui_search_h = MENU_ITEM_HEIGHT;
		menu->ui_list_x = 0;
		menu->ui_list_y = MENU_ITEM_HEIGHT;
		menu->ui_list_w = width;
		menu->ui_list_h = menu->n_visible * MENU_ITEM_HEIGHT;
	}

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

	/* --- Search widget --- */
	if (menu->ui_search_h > 0) {
		struct sm_rect r = {
			.x = menu->ui_search_x,
			.y = menu->ui_search_y,
			.w = menu->ui_search_w ? menu->ui_search_w : width,
			.h = menu->ui_search_h,
		};
		cairo_set_source_u32(cr, panel->conf->button_background);
		cairo_rectangle(cr, r.x, r.y, r.w, r.h);
		cairo_fill(cr);

		cairo_set_source_u32(cr, panel->conf->text);
		cairo_set_line_width(cr, 1.0);
		cairo_rectangle(cr, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
		cairo_stroke(cr);

		PangoRectangle search_rect = get_text_size(panel->conf->font_description,
			*menu->search ? menu->search : " ");
		cairo_set_source_u32(cr, panel->conf->text);
		cairo_move_to(cr, r.x + panel->conf->taskbar_padding,
			r.y + (r.h - search_rect.height) / 2.0);
		if (*menu->search) {
			render_text(cr, panel->conf->font_description, 1, false, "%s",
				menu->search);
		} else {
			render_text(cr, panel->conf->font_description, 1, false,
				"Search...");
		}
	}

	/* --- App list widget --- */
	if (menu->ui_list_h > 0) {
		struct sm_rect r = {
			.x = menu->ui_list_x,
			.y = menu->ui_list_y,
			.w = menu->ui_list_w ? menu->ui_list_w : width,
			.h = menu->ui_list_h,
		};
		int visible_rows = r.h / MENU_ITEM_HEIGHT;
		if (visible_rows < 1) {
			visible_rows = 1;
		}

		int highlighted = -1;
		if (menu->selected >= 0) {
			int row_of_selected = menu->selected - menu->scroll_offset;
			if (row_of_selected >= 0 && row_of_selected < visible_rows) {
				highlighted = row_of_selected;
			}
		} else if (menu->hover >= 0) {
			highlighted = menu->hover;
		}

		if (menu->n_filtered == 0) {
			int y = r.y;
			cairo_set_source_u32(cr, panel->conf->button_background);
			cairo_rectangle(cr, r.x, y, r.w, MENU_ITEM_HEIGHT);
			cairo_fill(cr);
			PangoRectangle text_rect = get_text_size(
				panel->conf->font_description, "No results");
			cairo_set_source_u32(cr, panel->conf->text);
			cairo_move_to(cr, r.x + panel->conf->taskbar_padding,
				y + (MENU_ITEM_HEIGHT - text_rect.height) / 2.0);
			render_text(cr, panel->conf->font_description, 1, false,
				"No results");
			for (int row = 1; row < visible_rows; row++) {
				y = r.y + row * MENU_ITEM_HEIGHT;
				cairo_set_source_u32(cr, panel->conf->button_background);
				cairo_rectangle(cr, r.x, y, r.w, MENU_ITEM_HEIGHT);
				cairo_fill(cr);
			}
		} else {
			for (int row = 0; row < visible_rows; row++) {
				int idx = menu->scroll_offset + row;
				int y = r.y + row * MENU_ITEM_HEIGHT;

				if (idx >= menu->n_filtered) {
					cairo_set_source_u32(cr, panel->conf->button_background);
					cairo_rectangle(cr, r.x, y, r.w, MENU_ITEM_HEIGHT);
					cairo_fill(cr);
					continue;
				}

				if (row == highlighted) {
					cairo_set_source_u32(cr, panel->conf->button_active);
				} else {
					cairo_set_source_u32(cr, panel->conf->button_background);
				}
				cairo_rectangle(cr, r.x, y, r.w, MENU_ITEM_HEIGHT);
				cairo_fill(cr);

				int app_idx = menu->filtered[idx];
				const char *name = menu->app_names[app_idx];
				PangoRectangle text_rect = get_text_size(
					panel->conf->font_description, name);
				cairo_set_source_u32(cr, panel->conf->text);
				cairo_move_to(cr, r.x + panel->conf->taskbar_padding,
					y + (MENU_ITEM_HEIGHT - text_rect.height) / 2.0);
				render_text(cr, panel->conf->font_description, 1, false,
					"%s", name);
			}
		}

		if (menu->scroll_offset > 0) {
			int y = r.y;
			PangoRectangle arr_rect =
				get_text_size(panel->conf->font_description, "▲");
			cairo_set_source_u32(cr, panel->conf->text);
			cairo_move_to(cr, r.x + r.w - arr_rect.width - panel->conf->taskbar_padding,
				y + (MENU_ITEM_HEIGHT - arr_rect.height) / 2.0);
			render_text(cr, panel->conf->font_description, 1, false, "▲");
		}
		if (menu->scroll_offset + visible_rows < menu->n_filtered) {
			int y = r.y + (visible_rows - 1) * MENU_ITEM_HEIGHT;
			PangoRectangle arr_rect =
				get_text_size(panel->conf->font_description, "▼");
			cairo_set_source_u32(cr, panel->conf->text);
			cairo_move_to(cr, r.x + r.w - arr_rect.width - panel->conf->taskbar_padding,
				y + (MENU_ITEM_HEIGHT - arr_rect.height) / 2.0);
			render_text(cr, panel->conf->font_description, 1, false, "▼");
		}
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
	menu->search[0] = '\0';
	menu->search_len = 0;
	menu->popup_open = false;
	panel->open_popup = NULL;
}

/* Temporary struct for sorting parallel app_names/app_execs arrays */
struct app_entry {
	char *name;
	char *exec;
};

static int
app_entry_cmp(const void *a, const void *b)
{
	const struct app_entry *ea = a;
	const struct app_entry *eb = b;
	return strcasecmp(ea->name, eb->name);
}

/*
 * Load all installed applications from XDG .desktop files using libsfdo.
 * Fills menu->app_names, menu->app_execs, menu->n_apps.
 * The arrays are sorted alphabetically by display name.
 */
static void
load_apps(struct startmenu *menu)
{
	struct sfdo_basedir_ctx *basedir_ctx = sfdo_basedir_ctx_create();
	if (!basedir_ctx) {
		wlr_log(WLR_ERROR, "startmenu: failed to create basedir context");
		return;
	}

	struct sfdo_desktop_ctx *desktop_ctx =
		sfdo_desktop_ctx_create(basedir_ctx);
	sfdo_basedir_ctx_destroy(basedir_ctx);
	if (!desktop_ctx) {
		wlr_log(WLR_ERROR, "startmenu: failed to create desktop context");
		return;
	}

	struct sfdo_desktop_db *db = sfdo_desktop_db_load(desktop_ctx, NULL);
	sfdo_desktop_ctx_destroy(desktop_ctx);
	if (!db) {
		wlr_log(WLR_ERROR, "startmenu: failed to load desktop db");
		return;
	}

	size_t n_entries;
	struct sfdo_desktop_entry **entries =
		sfdo_desktop_db_get_entries(db, &n_entries);

	/* First pass: count valid application entries */
	int count = 0;
	for (size_t i = 0; i < n_entries; i++) {
		struct sfdo_desktop_entry *entry = entries[i];
		if (sfdo_desktop_entry_get_type(entry)
				!= SFDO_DESKTOP_ENTRY_APPLICATION) {
			continue;
		}
		if (sfdo_desktop_entry_get_no_display(entry)) {
			continue;
		}
		if (!sfdo_desktop_entry_get_exec(entry)) {
			continue;
		}
		count++;
	}

	if (count == 0) {
		sfdo_desktop_db_destroy(db);
		return;
	}

	struct app_entry *entries_buf = calloc(count, sizeof(struct app_entry));
	if (!entries_buf) {
		sfdo_desktop_db_destroy(db);
		return;
	}

	/* Second pass: populate entries_buf */
	int j = 0;
	for (size_t i = 0; i < n_entries; i++) {
		struct sfdo_desktop_entry *entry = entries[i];
		if (sfdo_desktop_entry_get_type(entry)
				!= SFDO_DESKTOP_ENTRY_APPLICATION) {
			continue;
		}
		if (sfdo_desktop_entry_get_no_display(entry)) {
			continue;
		}
		struct sfdo_desktop_exec *exec_tmpl =
			sfdo_desktop_entry_get_exec(entry);
		if (!exec_tmpl) {
			continue;
		}
		/*
		 * Format the exec template without a file argument to get a
		 * concrete argv list, then take args[0] as the executable.
		 * This uses only the stable libsfdo API available in all
		 * releases, avoiding sfdo_desktop_entry_get_exec_arg0() which
		 * was added later.
		 */
		struct sfdo_desktop_exec_command *cmd =
			sfdo_desktop_exec_format(exec_tmpl, NULL);
		if (!cmd) {
			continue;
		}
		size_t n_args;
		const char **args =
			sfdo_desktop_exec_command_get_args(cmd, &n_args);
		const char *exec_str =
			(n_args > 0 && args && args[0]) ? args[0] : NULL;
		if (string_null_or_empty(exec_str)) {
			sfdo_desktop_exec_command_destroy(cmd);
			continue;
		}
		const char *name = sfdo_desktop_entry_get_name(entry, NULL);
		entries_buf[j].name = strdup(name && *name ? name : exec_str);
		entries_buf[j].exec = strdup(exec_str);
		sfdo_desktop_exec_command_destroy(cmd);
		j++;
	}
	sfdo_desktop_db_destroy(db);

	/* Sort alphabetically by display name */
	qsort(entries_buf, j, sizeof(struct app_entry), app_entry_cmp);

	/* Unpack into the parallel arrays */
	menu->app_names = calloc(j, sizeof(char *));
	menu->app_execs = calloc(j, sizeof(char *));
	if (!menu->app_names || !menu->app_execs) {
		for (int k = 0; k < j; k++) {
			free(entries_buf[k].name);
			free(entries_buf[k].exec);
		}
		free(entries_buf);
		free(menu->app_names);
		free(menu->app_execs);
		menu->app_names = NULL;
		menu->app_execs = NULL;
		return;
	}
	for (int k = 0; k < j; k++) {
		menu->app_names[k] = entries_buf[k].name;
		menu->app_execs[k] = entries_buf[k].exec;
	}
	free(entries_buf);
	menu->n_apps = j;

	wlr_log(WLR_DEBUG, "startmenu: loaded %d applications", menu->n_apps);
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

	/* Reset search and rebuild filter */
	menu->search[0] = '\0';
	menu->search_len = 0;
	update_filtered(menu);

	/* n_visible: how many item rows to show (capped at MENU_MAX_VISIBLE) */
	menu->n_visible = menu->n_apps < MENU_MAX_VISIBLE
		? menu->n_apps : MENU_MAX_VISIBLE;
	if (menu->n_visible < 1) {
		menu->n_visible = 1;
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

	int popup_height = (1 + menu->n_visible) * MENU_ITEM_HEIGHT;
	if (menu->ui_root) {
		int mw = 0, mh = 0;
		sm_measure(menu, (const struct sm_node *)menu->ui_root, &mw, &mh);
		if (mh > 0) {
			popup_height = mh;
		}
	}
	struct xdg_positioner *positioner =
		xdg_wm_base_create_positioner(panel->xdg_wm_base);
	xdg_positioner_set_size(positioner, MENU_WIDTH, popup_height);
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
			menu->selected = menu->n_filtered - 1;
		} else if (menu->selected > 0) {
			menu->selected--;
		}
		/* Scroll to keep selection visible */
		if (menu->selected >= 0) {
			if (menu->selected < menu->scroll_offset) {
				menu->scroll_offset = menu->selected;
			} else if (menu->selected
					>= menu->scroll_offset + menu->n_visible) {
				menu->scroll_offset =
					menu->selected - menu->n_visible + 1;
			}
		}
		startmenu_render_popup(menu);
		break;
	case KEY_DOWN:
		if (menu->selected < 0) {
			menu->selected = 0;
		} else if (menu->selected < menu->n_filtered - 1) {
			menu->selected++;
		}
		/* Scroll down if needed to keep selection visible */
		if (menu->selected >= menu->scroll_offset + menu->n_visible) {
			menu->scroll_offset =
				menu->selected - menu->n_visible + 1;
		}
		startmenu_render_popup(menu);
		break;
	case KEY_ENTER: {
		int idx = menu->selected >= 0 ? menu->selected
			: (menu->hover >= 0
				? menu->scroll_offset + menu->hover : -1);
		if (idx >= 0 && idx < menu->n_filtered) {
			int app_idx = menu->filtered[idx];
			wlr_log(WLR_DEBUG, "startmenu: launching %s (%s)",
				menu->app_names[app_idx],
				menu->app_execs[app_idx]);
			launch_app(menu->app_execs[app_idx]);
		}
		startmenu_close(menu);
		render_frame(panel);
		break;
	}
	case KEY_ESC:
		startmenu_close(menu);
		render_frame(panel);
		break;
	case KEY_BACKSPACE:
		if (menu->search_len > 0) {
			/* UTF-8 aware: remove last code point (max 4 bytes) */
			int max_iter = 4;
			while (menu->search_len > 0 && max_iter-- > 0
					&& ((unsigned char)menu->search[menu->search_len - 1]
						& 0xC0) == 0x80) {
				menu->search_len--;
			}
			if (menu->search_len > 0) {
				menu->search_len--;
			}
			menu->search[menu->search_len] = '\0';
			update_filtered(menu);
			startmenu_render_popup(menu);
		}
		break;
	default:
		break;
	}
}

void
plugin_startmenu_text_input(struct panel *panel, const char *utf8)
{
	struct startmenu *menu = panel->open_popup;
	if (!menu || string_null_or_empty(utf8)) {
		return;
	}

	size_t len = strlen(utf8);
	if (menu->search_len + len >= sizeof(menu->search) - 1) {
		return; /* search buffer full */
	}

	memcpy(menu->search + menu->search_len, utf8, len);
	menu->search_len += len;
	menu->search[menu->search_len] = '\0';

	update_filtered(menu);
	startmenu_render_popup(menu);
}

void
plugin_startmenu_pointer_motion(struct startmenu *menu, int y)
{
	/* Search widget - no hover for it */
	if (y >= menu->ui_search_y && y < menu->ui_search_y + menu->ui_search_h) {
		if (menu->hover != -1) {
			menu->hover = -1;
			startmenu_render_popup(menu);
		}
		return;
	}
	if (menu->ui_list_h <= 0 || y < menu->ui_list_y
			|| y >= menu->ui_list_y + menu->ui_list_h) {
		if (menu->hover != -1) {
			menu->hover = -1;
			startmenu_render_popup(menu);
		}
		return;
	}
	int visible_rows = menu->ui_list_h / MENU_ITEM_HEIGHT;
	if (visible_rows < 1) {
		visible_rows = 1;
	}
	int row = (y - menu->ui_list_y) / MENU_ITEM_HEIGHT;
	if (row < 0 || row >= visible_rows
			|| (menu->scroll_offset + row) >= menu->n_filtered) {
		row = -1;
	}
	if (row != menu->hover) {
		menu->hover = row;
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
	/* Ignore clicks on the search widget */
	if (y >= menu->ui_search_y && y < menu->ui_search_y + menu->ui_search_h) {
		return;
	}
	if (menu->ui_list_h <= 0 || y < menu->ui_list_y
			|| y >= menu->ui_list_y + menu->ui_list_h) {
		return;
	}
	int visible_rows = menu->ui_list_h / MENU_ITEM_HEIGHT;
	if (visible_rows < 1) {
		visible_rows = 1;
	}
	int row = (y - menu->ui_list_y) / MENU_ITEM_HEIGHT;
	if (row < 0 || row >= visible_rows) {
		return;
	}
	int idx = menu->scroll_offset + row;
	if (idx < menu->n_filtered) {
		int app_idx = menu->filtered[idx];
		wlr_log(WLR_DEBUG, "startmenu: launching %s (%s)",
			menu->app_names[app_idx], menu->app_execs[app_idx]);
		launch_app(menu->app_execs[app_idx]);
		startmenu_close(menu);
		render_frame(menu->base.panel);
	}
}

void
plugin_startmenu_scroll(struct startmenu *menu, double delta)
{
	int dir = delta > 0 ? 1 : -1;
	int new_offset = menu->scroll_offset + dir;
	int max_offset = menu->n_filtered - menu->n_visible;
	if (max_offset < 0) {
		max_offset = 0;
	}
	if (new_offset < 0) {
		new_offset = 0;
	}
	if (new_offset > max_offset) {
		new_offset = max_offset;
	}
	if (new_offset != menu->scroll_offset) {
		menu->scroll_offset = new_offset;
		menu->hover = -1;
		startmenu_render_popup(menu);
	}
}

void
plugin_startmenu_destroy(struct startmenu *menu)
{
	startmenu_close(menu);

	sm_node_free((struct sm_node *)menu->ui_root);
	menu->ui_root = NULL;

	for (int i = 0; i < menu->n_apps; i++) {
		free(menu->app_names[i]);
		free(menu->app_execs[i]);
	}
	free(menu->app_names);
	free(menu->app_execs);
	free(menu->filtered);

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
	menu->ui_root = NULL;
	menu->ui_search_x = 0;
	menu->ui_search_y = 0;
	menu->ui_search_w = MENU_WIDTH;
	menu->ui_search_h = MENU_ITEM_HEIGHT;
	menu->ui_list_x = 0;
	menu->ui_list_y = MENU_ITEM_HEIGHT;
	menu->ui_list_w = MENU_WIDTH;
	menu->ui_list_h = MENU_MAX_VISIBLE * MENU_ITEM_HEIGHT;
	wl_list_insert(panel->widgets.prev, &menu->base.link);

	/* Default layout: search box above scrollable app list. */
	const char *layout_xml = panel->conf && panel->conf->startmenu_layout
		? panel->conf->startmenu_layout
		: "<vbox><search/><applist/></vbox>";
	menu->ui_root = sm_parse_layout(layout_xml);
	if (!menu->ui_root) {
		wlr_log(WLR_ERROR, "startmenu: failed to parse startmenu_layout, falling back");
		menu->ui_root = sm_parse_layout("<vbox><search/><applist/></vbox>");
	}

	load_apps(menu);
}
