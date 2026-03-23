// SPDX-License-Identifier: GPL-2.0-only
/*
 * SNI (StatusNotifierItem) system tray plugin.
 *
 * Architecture based on sway's swaybar/tray/ implementation:
 *   - PID-based host name (avoids invalid ':' from unique D-Bus name)
 *   - Watcher object path registered via dbus_connection_register_object_path
 *     so method calls are handled robustly with Introspect + GetAll support
 *   - Host registration sent asynchronously (DBusPendingCall) to avoid a
 *     deadlock when we are ourselves the watcher
 *   - Item properties fetched asynchronously; re-render on each callback
 *   - Item interface selected by ID format (sway pattern):
 *       no '/' -> org.freedesktop.StatusNotifierItem
 *       has '/' -> org.kde.StatusNotifierItem
 *   - Network-byte-order ARGB converted with ntohl (matches sway)
 */
#include <arpa/inet.h>
#include <assert.h>
#include <cairo.h>
#include <dbus/dbus.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "panel.h"

/* Well-known names / paths */
#define SNI_WATCHER_BUS    "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH   "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE  "org.kde.StatusNotifierWatcher"
#define SNI_HOST_PREFIX    "org.kde.StatusNotifierHost"
#define SNI_ITEM_IFACE_KDE "org.kde.StatusNotifierItem"
#define SNI_ITEM_IFACE_FDO "org.freedesktop.StatusNotifierItem"
#define SNI_ITEM_PATH      "/StatusNotifierItem"
#define DBUS_IFACE         "org.freedesktop.DBus"
#define PROPS_IFACE        "org.freedesktop.DBus.Properties"
#define INTROSPECT_IFACE   "org.freedesktop.DBus.Introspectable"

/* Introspection XML advertised for /StatusNotifierWatcher */
static const char watcher_introspect_xml[] =
	"<!DOCTYPE node PUBLIC"
	" \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
	" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
	"<node>"
	" <interface name=\"org.kde.StatusNotifierWatcher\">"
	"  <method name=\"RegisterStatusNotifierItem\">"
	"   <arg name=\"service\" type=\"s\" direction=\"in\"/>"
	"  </method>"
	"  <method name=\"RegisterStatusNotifierHost\">"
	"   <arg name=\"service\" type=\"s\" direction=\"in\"/>"
	"  </method>"
	"  <property name=\"RegisteredStatusNotifierItems\""
	"            type=\"as\" access=\"read\"/>"
	"  <property name=\"IsStatusNotifierHostRegistered\""
	"            type=\"b\" access=\"read\"/>"
	"  <property name=\"ProtocolVersion\""
	"            type=\"i\" access=\"read\"/>"
	"  <signal name=\"StatusNotifierItemRegistered\">"
	"   <arg type=\"s\"/>"
	"  </signal>"
	"  <signal name=\"StatusNotifierItemUnregistered\">"
	"   <arg type=\"s\"/>"
	"  </signal>"
	"  <signal name=\"StatusNotifierHostRegistered\"/>"
	"  <signal name=\"StatusNotifierHostUnregistered\"/>"
	" </interface>"
	" <interface name=\"org.freedesktop.DBus.Introspectable\">"
	"  <method name=\"Introspect\">"
	"   <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>"
	"  </method>"
	" </interface>"
	" <interface name=\"org.freedesktop.DBus.Properties\">"
	"  <method name=\"Get\">"
	"   <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
	"   <arg name=\"property_name\" type=\"s\" direction=\"in\"/>"
	"   <arg name=\"value\" type=\"v\" direction=\"out\"/>"
	"  </method>"
	"  <method name=\"GetAll\">"
	"   <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
	"   <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>"
	"  </method>"
	" </interface>"
	"</node>";

/* One entry in a per-item pixmap list. */
struct sni_pixmap {
	int size;             /* width == height */
	uint32_t pixels[];   /* ARGB32 native byte order, size*size entries */
};

/* An individual status-notifier item (one tray icon). */
struct sni_item {
	struct sni *tray;
	char *watcher_id;     /* canonical "service/path" used as unique key */
	char *service;        /* D-Bus service name */
	char *path;           /* D-Bus object path */
	const char *iface;    /* SNI_ITEM_IFACE_KDE or SNI_ITEM_IFACE_FDO */
	char *icon_name;
	char *icon_theme_path;
	/* Best-fit pixmap decoded from IconPixmap, or NULL */
	uint32_t *pixmap_data;
	int pixmap_size;      /* pixels per side (square assumed) */
	struct wl_list link;  /* sni->items */
};

/* Forward declarations */
static void sni_update_surface(struct sni *sni);
static void sni_item_fetch_async(struct sni_item *item);

static inline DBusConnection *
sni_dbus(struct sni *sni)
{
	return (DBusConnection *)sni->conn;
}

/* ---------- item helpers ------------------------------------------------- */

static struct sni_item *
sni_item_find(struct sni *sni, const char *watcher_id)
{
	struct sni_item *item;
	wl_list_for_each(item, &sni->items, link) {
		if (strcmp(item->watcher_id, watcher_id) == 0) {
			return item;
		}
	}
	return NULL;
}

static struct sni_item *
sni_item_find_by_service(struct sni *sni, const char *service)
{
	if (!service) {
		return NULL;
	}
	struct sni_item *item;
	wl_list_for_each(item, &sni->items, link) {
		if (item->service && strcmp(item->service, service) == 0) {
			return item;
		}
	}
	return NULL;
}

static void
sni_item_destroy(struct sni_item *item)
{
	wl_list_remove(&item->link);
	free(item->watcher_id);
	free(item->service);
	free(item->path);
	free(item->icon_name);
	free(item->icon_theme_path);
	free(item->pixmap_data);
	free(item);
}

/* ---------- icon rendering ----------------------------------------------- */

/* Load a PNG into a Cairo surface; returns NULL on failure. */
static cairo_surface_t *
load_png(const char *path)
{
	cairo_surface_t *surf = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
		return surf;
	}
	cairo_surface_destroy(surf);
	return NULL;
}

/*
 * Search for an icon PNG by name in common icon theme directories.
 * Returns a Cairo surface on success, NULL on failure.
 */
static cairo_surface_t *
load_icon_by_name(const char *name, const char *theme_path)
{
	if (!name || !name[0]) {
		return NULL;
	}

	char path[PATH_MAX];
	cairo_surface_t *surf = NULL;

	static const char *size_dirs[] = {
		"22x22", "24x24", "32x32", "16x16", "48x48", NULL
	};
	static const char *categories[] = {
		"apps", "status", "devices", "actions", NULL
	};

	/* Try the item-supplied theme path first */
	if (theme_path && theme_path[0]) {
		for (int s = 0; !surf && size_dirs[s]; s++) {
			for (int c = 0; !surf && categories[c]; c++) {
				snprintf(path, sizeof(path),
					"%s/%s/%s/%s.png",
					theme_path, size_dirs[s],
					categories[c], name);
				surf = load_png(path);
			}
		}
		if (!surf) {
			snprintf(path, sizeof(path), "%s/%s.png",
				theme_path, name);
			surf = load_png(path);
		}
	}

	/* Standard hicolor icon theme */
	static const char *icon_dirs[] = {
		"/usr/share/icons/hicolor",
		"/usr/local/share/icons/hicolor",
		NULL
	};
	for (int d = 0; !surf && icon_dirs[d]; d++) {
		for (int s = 0; !surf && size_dirs[s]; s++) {
			for (int c = 0; !surf && categories[c]; c++) {
				snprintf(path, sizeof(path),
					"%s/%s/%s/%s.png",
					icon_dirs[d], size_dirs[s],
					categories[c], name);
				surf = load_png(path);
			}
		}
	}

	/* Pixmaps directory */
	if (!surf) {
		snprintf(path, sizeof(path),
			"/usr/share/pixmaps/%s.png", name);
		surf = load_png(path);
	}

	return surf;
}

/* Redraw the SNI tray widget surface from the current item list. */
static void
sni_update_surface(struct sni *sni)
{
	struct panel *panel = sni->base.panel;
	if (!panel->height) {
		return;
	}

	int n = wl_list_length(&sni->items);
	int slot = (int)panel->height;
	int total_w = n * slot;

	if (total_w == 0) {
		sni->base.width = 0;
		if (sni->base.surface) {
			cairo_surface_destroy(sni->base.surface);
			sni->base.surface = NULL;
		}
		return;
	}

	sni->base.width = total_w;
	if (sni->base.surface) {
		cairo_surface_destroy(sni->base.surface);
	}
	sni->base.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		total_w, (int)panel->height);
	cairo_t *cr = cairo_create(sni->base.surface);

	int icon_size = (int)panel->height - 4;
	if (icon_size < 1) {
		icon_size = 1;
	}

	int x = 0;
	struct sni_item *item;
	wl_list_for_each(item, &sni->items, link) {
		int icon_x = x + (slot - icon_size) / 2;
		int icon_y = ((int)panel->height - icon_size) / 2;

		cairo_surface_t *icon_surf = NULL;

		if (item->pixmap_data && item->pixmap_size > 0) {
			/* Use pixmap provided by the application */
			icon_surf = cairo_image_surface_create_for_data(
				(uint8_t *)item->pixmap_data,
				CAIRO_FORMAT_ARGB32, item->pixmap_size,
				item->pixmap_size,
				item->pixmap_size * (int)sizeof(uint32_t));
		} else if (item->icon_name) {
			icon_surf = load_icon_by_name(item->icon_name,
				item->icon_theme_path);
		}

		if (icon_surf) {
			int iw = cairo_image_surface_get_width(icon_surf);
			int ih = cairo_image_surface_get_height(icon_surf);
			cairo_save(cr);
			cairo_translate(cr, icon_x, icon_y);
			if (iw > 0 && ih > 0) {
				cairo_scale(cr,
					(double)icon_size / iw,
					(double)icon_size / ih);
			}
			cairo_set_source_surface(cr, icon_surf, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(icon_surf);
		}

		x += slot;
	}
	cairo_destroy(cr);
}

/* ---------- async property fetch ----------------------------------------- */

/*
 * Async callback for Properties.Get("IconPixmap").
 * Decodes the a(iiay) pixmap array and stores the best-fit entry.
 */
static void
on_icon_pixmap(DBusPendingCall *pending, void *user_data)
{
	struct sni_item *item = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	if (!reply) {
		return;
	}
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		wlr_log(WLR_DEBUG, "sni: IconPixmap Get error for %s: %s",
			item->watcher_id,
			dbus_message_get_error_name(reply));
		dbus_message_unref(reply);
		return;
	}

	/* Reply is v(a(iiay)) */
	DBusMessageIter iter, variant, arr;
	dbus_message_iter_init(reply, &iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
		dbus_message_unref(reply);
		return;
	}
	dbus_message_iter_recurse(&iter, &variant);
	if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
		dbus_message_unref(reply);
		return;
	}
	dbus_message_iter_recurse(&variant, &arr);

	int best_size = 0;
	uint8_t *best_raw = NULL;
	int best_npx = 0;
	int target = (int)item->tray->base.panel->height;

	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		DBusMessageIter st;
		dbus_message_iter_recurse(&arr, &st);

		int32_t w = 0, h = 0;
		if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&st, &w);
		}
		dbus_message_iter_next(&st);
		if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&st, &h);
		}
		dbus_message_iter_next(&st);

		uint8_t *raw = NULL;
		int npx = 0;
		if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY) {
			DBusMessageIter by;
			dbus_message_iter_recurse(&st, &by);
			dbus_message_iter_get_fixed_array(&by,
				(void **)&raw, &npx);
		}

		/* Only square, validly-sized pixmaps */
		if (raw && w > 0 && w == h && npx == w * h * 4) {
			bool better = (best_size == 0)
				|| (abs(w - target) < abs(best_size - target))
				|| (abs(w - target) == abs(best_size - target)
					&& w > best_size);
			if (better) {
				best_size = w;
				best_raw = raw;
				best_npx = npx;
			}
		}
		dbus_message_iter_next(&arr);
	}

	if (best_raw && best_npx > 0) {
		uint32_t *data = malloc((size_t)(best_size * best_size)
			* sizeof(uint32_t));
		if (data) {
			/*
			 * Convert from network byte order (big-endian ARGB)
			 * to host byte order, matching sway's ntohl approach.
			 */
			const uint32_t *src = (const uint32_t *)best_raw;
			for (int i = 0; i < best_size * best_size; i++) {
				data[i] = ntohl(src[i]);
			}
			free(item->pixmap_data);
			item->pixmap_data = data;
			item->pixmap_size = best_size;
		}
	}

	dbus_message_unref(reply);
	sni_update_surface(item->tray);
	render_frame(item->tray->base.panel);
}

/*
 * Async callback for Properties.Get("IconName").
 */
static void
on_icon_name(DBusPendingCall *pending, void *user_data)
{
	struct sni_item *item = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	if (!reply) {
		return;
	}
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		dbus_message_unref(reply);
		return;
	}

	/* Reply is v(s) */
	DBusMessageIter iter, variant;
	dbus_message_iter_init(reply, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant)
			== DBUS_TYPE_STRING) {
			const char *name;
			dbus_message_iter_get_basic(&variant, &name);
			free(item->icon_name);
			item->icon_name = (name && name[0])
				? strdup(name) : NULL;
		}
	}
	dbus_message_unref(reply);
	sni_update_surface(item->tray);
	render_frame(item->tray->base.panel);
}

/*
 * Async callback for Properties.Get("IconThemePath").
 */
static void
on_icon_theme_path(DBusPendingCall *pending, void *user_data)
{
	struct sni_item *item = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	if (!reply) {
		return;
	}
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		dbus_message_unref(reply);
		return;
	}

	DBusMessageIter iter, variant;
	dbus_message_iter_init(reply, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant)
			== DBUS_TYPE_STRING) {
			const char *tp;
			dbus_message_iter_get_basic(&variant, &tp);
			free(item->icon_theme_path);
			item->icon_theme_path = (tp && tp[0])
				? strdup(tp) : NULL;
		}
	}
	dbus_message_unref(reply);
}

/*
 * Send an async Properties.Get call for one property on an SNI item.
 * callback is invoked from dbus_connection_dispatch when the reply arrives.
 */
static void
sni_get_property_async(struct sni_item *item, const char *prop,
	DBusPendingCallNotifyFunction callback)
{
	DBusConnection *conn = sni_dbus(item->tray);
	DBusMessage *msg = dbus_message_new_method_call(item->service,
		item->path, PROPS_IFACE, "Get");
	if (!msg) {
		return;
	}
	dbus_message_append_args(msg,
		DBUS_TYPE_STRING, &item->iface,
		DBUS_TYPE_STRING, &prop,
		DBUS_TYPE_INVALID);

	DBusPendingCall *pending = NULL;
	if (dbus_connection_send_with_reply(conn, msg, &pending, 5000)
		&& pending) {
		dbus_pending_call_set_notify(pending, callback, item, NULL);
	}
	dbus_message_unref(msg);
}

/*
 * Kick off async property fetches for a newly registered item.
 */
static void
sni_item_fetch_async(struct sni_item *item)
{
	sni_get_property_async(item, "IconName", on_icon_name);
	sni_get_property_async(item, "IconPixmap", on_icon_pixmap);
	/* IconThemePath is a KDE extension; ignore errors */
	if (strcmp(item->iface, SNI_ITEM_IFACE_KDE) == 0) {
		sni_get_property_async(item, "IconThemePath",
			on_icon_theme_path);
	}
}

/* ---------- item registration -------------------------------------------- */

static void
sni_item_add(struct sni *sni, const char *watcher_id)
{
	if (!watcher_id || !watcher_id[0]) {
		return;
	}
	if (sni_item_find(sni, watcher_id)) {
		return; /* already present */
	}

	struct sni_item *item = calloc(1, sizeof(*item));
	if (!item) {
		wlr_log(WLR_ERROR, "sni: out of memory adding item %s",
			watcher_id);
		return;
	}
	item->tray = sni;
	wl_list_init(&item->link); /* safe to destroy before list insertion */
	item->watcher_id = strdup(watcher_id);

	/*
	 * Parse service / path from the watcher ID, matching sway's logic:
	 *   - ID with '/' -> KDE protocol; split on first '/'
	 *   - ID without '/' -> freedesktop protocol; path = /StatusNotifierItem
	 */
	const char *slash = strchr(watcher_id, '/');
	if (slash) {
		item->service = strndup(watcher_id,
			(size_t)(slash - watcher_id));
		item->path = strdup(slash);
		item->iface = SNI_ITEM_IFACE_KDE;
	} else {
		item->service = strdup(watcher_id);
		item->path = strdup(SNI_ITEM_PATH);
		item->iface = SNI_ITEM_IFACE_FDO;
	}

	if (!item->watcher_id || !item->service || !item->path) {
		wlr_log(WLR_ERROR, "sni: out of memory for item strings");
		sni_item_destroy(item);
		return;
	}

	wl_list_insert(sni->items.prev, &item->link);
	wlr_log(WLR_DEBUG, "sni: added item %s (iface=%s)",
		watcher_id, item->iface);

	sni_item_fetch_async(item);
}

static void
sni_item_remove_by_service(struct sni *sni, const char *service)
{
	if (!service) {
		return;
	}
	bool changed = false;
	struct sni_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &sni->items, link) {
		if (item->service && strcmp(item->service, service) == 0) {
			wlr_log(WLR_DEBUG, "sni: removed item %s",
				item->watcher_id);
			sni_item_destroy(item);
			changed = true;
		}
	}
	if (changed) {
		sni_update_surface(sni);
		render_frame(sni->base.panel);
	}
}

/* ---------- watcher object path handler ---------------------------------- */

/*
 * Append all watcher properties into an open a{sv} DBusMessageIter.
 * Used by both Properties.Get and Properties.GetAll.
 */
static void
watcher_append_prop_items(struct sni *sni, DBusMessageIter *arr)
{
	/* RegisteredStatusNotifierItems : as */
	{
		const char *key = "RegisteredStatusNotifierItems";
		DBusMessageIter entry, var, as;
		dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
			"as", &var);
		dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY,
			"s", &as);
		struct sni_item *it;
		wl_list_for_each(it, &sni->items, link) {
			const char *id = it->watcher_id;
			dbus_message_iter_append_basic(&as,
				DBUS_TYPE_STRING, &id);
		}
		dbus_message_iter_close_container(&var, &as);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(arr, &entry);
	}

	/* IsStatusNotifierHostRegistered : b */
	{
		const char *key = "IsStatusNotifierHostRegistered";
		DBusMessageIter entry, var;
		dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
			"b", &var);
		dbus_bool_t val = TRUE;
		dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &val);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(arr, &entry);
	}

	/* ProtocolVersion : i */
	{
		const char *key = "ProtocolVersion";
		DBusMessageIter entry, var;
		dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
			"i", &var);
		int32_t ver = 0;
		dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &ver);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(arr, &entry);
	}
}

static DBusHandlerResult
watcher_handle_message(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	struct sni *sni = user_data;
	int type = dbus_message_get_type(msg);
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);

	if (!iface || !member) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* --- SNI watcher methods --- */
	if (type == DBUS_MESSAGE_TYPE_METHOD_CALL
		&& strcmp(iface, SNI_WATCHER_IFACE) == 0) {

		if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
			const char *arg = NULL;
			dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &arg,
				DBUS_TYPE_INVALID);
			const char *sender = dbus_message_get_sender(msg);

			/*
			 * Build canonical watcher ID (mirrors sway register_sni):
			 *   arg starts with '/' -> sender + arg
			 *   arg has '/'         -> arg as-is
			 *   bare name / empty   -> arg (or sender) + /StatusNotifierItem
			 */
			char id[512];
			if (arg && arg[0] == '/') {
				snprintf(id, sizeof(id), "%s%s",
					sender ? sender : "", arg);
			} else if (arg && strchr(arg, '/')) {
				snprintf(id, sizeof(id), "%s", arg);
			} else if (arg && arg[0]) {
				snprintf(id, sizeof(id), "%s%s",
					arg, SNI_ITEM_PATH);
			} else {
				snprintf(id, sizeof(id), "%s%s",
					sender ? sender : "", SNI_ITEM_PATH);
			}

			/* Reply first, then do work */
			DBusMessage *reply =
				dbus_message_new_method_return(msg);
			if (reply) {
				dbus_connection_send(conn, reply, NULL);
				dbus_message_unref(reply);
			}

			sni_item_add(sni, id);

			/* Emit StatusNotifierItemRegistered */
			DBusMessage *sig = dbus_message_new_signal(
				SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
				"StatusNotifierItemRegistered");
			if (sig) {
				const char *id_p = id;
				dbus_message_append_args(sig,
					DBUS_TYPE_STRING, &id_p,
					DBUS_TYPE_INVALID);
				dbus_connection_send(conn, sig, NULL);
				dbus_message_unref(sig);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
			DBusMessage *reply =
				dbus_message_new_method_return(msg);
			if (reply) {
				dbus_connection_send(conn, reply, NULL);
				dbus_message_unref(reply);
			}
			DBusMessage *sig = dbus_message_new_signal(
				SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
				"StatusNotifierHostRegistered");
			if (sig) {
				dbus_connection_send(conn, sig, NULL);
				dbus_message_unref(sig);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

	/* --- Properties.Get --- */
	if (type == DBUS_MESSAGE_TYPE_METHOD_CALL
		&& strcmp(iface, PROPS_IFACE) == 0
		&& strcmp(member, "Get") == 0) {

		const char *req_iface = NULL, *prop = NULL;
		if (!dbus_message_get_args(msg, NULL,
			DBUS_TYPE_STRING, &req_iface,
			DBUS_TYPE_STRING, &prop,
			DBUS_TYPE_INVALID)) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		if (strcmp(req_iface, SNI_WATCHER_IFACE) != 0) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}

		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (!reply) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		DBusMessageIter ri, var;
		dbus_message_iter_init_append(reply, &ri);

		if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
			dbus_message_iter_open_container(&ri,
				DBUS_TYPE_VARIANT, "as", &var);
			DBusMessageIter as;
			dbus_message_iter_open_container(&var,
				DBUS_TYPE_ARRAY, "s", &as);
			struct sni_item *it;
			wl_list_for_each(it, &sni->items, link) {
				const char *id = it->watcher_id;
				dbus_message_iter_append_basic(&as,
					DBUS_TYPE_STRING, &id);
			}
			dbus_message_iter_close_container(&var, &as);
			dbus_message_iter_close_container(&ri, &var);
		} else if (strcmp(prop,
			"IsStatusNotifierHostRegistered") == 0) {
			dbus_message_iter_open_container(&ri,
				DBUS_TYPE_VARIANT, "b", &var);
			dbus_bool_t val = TRUE;
			dbus_message_iter_append_basic(&var,
				DBUS_TYPE_BOOLEAN, &val);
			dbus_message_iter_close_container(&ri, &var);
		} else if (strcmp(prop, "ProtocolVersion") == 0) {
			dbus_message_iter_open_container(&ri,
				DBUS_TYPE_VARIANT, "i", &var);
			int32_t ver = 0;
			dbus_message_iter_append_basic(&var,
				DBUS_TYPE_INT32, &ver);
			dbus_message_iter_close_container(&ri, &var);
		} else {
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* --- Properties.GetAll --- */
	if (type == DBUS_MESSAGE_TYPE_METHOD_CALL
		&& strcmp(iface, PROPS_IFACE) == 0
		&& strcmp(member, "GetAll") == 0) {

		const char *req_iface = NULL;
		dbus_message_get_args(msg, NULL,
			DBUS_TYPE_STRING, &req_iface,
			DBUS_TYPE_INVALID);
		/* Serve our properties regardless of which interface is asked */

		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (!reply) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		DBusMessageIter ri, arr;
		dbus_message_iter_init_append(reply, &ri);
		dbus_message_iter_open_container(&ri, DBUS_TYPE_ARRAY,
			"{sv}", &arr);
		watcher_append_prop_items(sni, &arr);
		dbus_message_iter_close_container(&ri, &arr);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* --- Introspect --- */
	if (type == DBUS_MESSAGE_TYPE_METHOD_CALL
		&& strcmp(iface, INTROSPECT_IFACE) == 0
		&& strcmp(member, "Introspect") == 0) {

		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (!reply) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		const char *xml = watcher_introspect_xml;
		dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &xml,
			DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
watcher_unregister(DBusConnection *conn, void *user_data)
{
	(void)conn;
	(void)user_data;
}

static const DBusObjectPathVTable watcher_vtable = {
	.unregister_function = watcher_unregister,
	.message_function = watcher_handle_message,
};

/* ---------- signal filter ------------------------------------------------- */

static DBusHandlerResult
signal_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
	(void)conn;
	struct sni *sni = data;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!iface || !member) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* Watcher signals: item registered / unregistered */
	if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
		if (strcmp(member, "StatusNotifierItemRegistered") == 0) {
			const char *id = NULL;
			dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &id,
				DBUS_TYPE_INVALID);
			sni_item_add(sni, id);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (strcmp(member, "StatusNotifierItemUnregistered") == 0) {
			const char *id = NULL;
			dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &id,
				DBUS_TYPE_INVALID);
			if (id) {
				const char *slash = strchr(id, '/');
				char *svc = slash
					? strndup(id, (size_t)(slash - id))
					: strdup(id);
				sni_item_remove_by_service(sni, svc);
				free(svc);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

	/* Icon change signals from an item */
	if ((strcmp(iface, SNI_ITEM_IFACE_KDE) == 0
		|| strcmp(iface, SNI_ITEM_IFACE_FDO) == 0)
		&& (strcmp(member, "NewIcon") == 0
			|| strcmp(member, "NewAttentionIcon") == 0
			|| strcmp(member, "NewOverlayIcon") == 0)) {
		const char *sender = dbus_message_get_sender(msg);
		struct sni_item *item =
			sni_item_find_by_service(sni, sender);
		if (item) {
			sni_get_property_async(item, "IconName", on_icon_name);
			sni_get_property_async(item, "IconPixmap",
				on_icon_pixmap);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* Service disappeared */
	if (strcmp(iface, DBUS_IFACE) == 0
		&& strcmp(member, "NameOwnerChanged") == 0) {
		const char *name = NULL, *old_owner = NULL,
			   *new_owner = NULL;
		dbus_message_get_args(msg, NULL,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_STRING, &old_owner,
			DBUS_TYPE_STRING, &new_owner,
			DBUS_TYPE_INVALID);
		if (name && old_owner && old_owner[0]
			&& (!new_owner || !new_owner[0])) {
			sni_item_remove_by_service(sni, name);
			sni_item_remove_by_service(sni, old_owner);
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- host registration -------------------------------------------- */

/*
 * Async callback for RegisterStatusNotifierHost.
 * We don't need the reply content; just note that registration completed.
 */
static void
on_register_host_reply(DBusPendingCall *pending, void *user_data)
{
	struct sni *sni = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	if (reply) {
		if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
			wlr_log(WLR_DEBUG,
				"sni: RegisterStatusNotifierHost failed: %s",
				dbus_message_get_error_name(reply));
		}
		dbus_message_unref(reply);
	}
	sni->host_registered = true;
}

/*
 * Register ourselves as a StatusNotifierHost with the watcher.
 * Uses an async send so we do not deadlock when we are the watcher.
 */
static void
sni_register_host(struct sni *sni)
{
	DBusConnection *conn = sni_dbus(sni);
	DBusError err = DBUS_ERROR_INIT;

	/*
	 * Use the process PID for the host name so the name is a valid
	 * D-Bus bus name (no ':' characters, unlike a unique name).
	 */
	char host_name[256];
	snprintf(host_name, sizeof(host_name), "%s-%d",
		SNI_HOST_PREFIX, (int)getpid());

	dbus_bus_request_name(conn, host_name, DBUS_NAME_FLAG_DO_NOT_QUEUE,
		&err);
	dbus_error_free(&err);

	DBusMessage *msg = dbus_message_new_method_call(SNI_WATCHER_BUS,
		SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
		"RegisterStatusNotifierHost");
	if (!msg) {
		return;
	}
	const char *name_p = host_name;
	dbus_message_append_args(msg,
		DBUS_TYPE_STRING, &name_p,
		DBUS_TYPE_INVALID);

	/*
	 * Send asynchronously.  This avoids a blocking self-call when we are
	 * the watcher (the reply would never arrive while we are blocked).
	 */
	DBusPendingCall *pending = NULL;
	if (dbus_connection_send_with_reply(conn, msg, &pending, 5000)
		&& pending) {
		dbus_pending_call_set_notify(pending,
			on_register_host_reply, sni, NULL);
	}
	dbus_message_unref(msg);
}

/*
 * Async callback for Properties.Get("RegisteredStatusNotifierItems").
 * Adds any items already registered with the external watcher.
 */
static void
on_registered_items(DBusPendingCall *pending, void *user_data)
{
	struct sni *sni = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	if (!reply) {
		return;
	}
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		wlr_log(WLR_DEBUG,
			"sni: get RegisteredStatusNotifierItems failed: %s",
			dbus_message_get_error_name(reply));
		dbus_message_unref(reply);
		return;
	}

	DBusMessageIter iter, variant, array;
	dbus_message_iter_init(reply, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant)
			== DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&variant, &array);
			while (dbus_message_iter_get_arg_type(&array)
				== DBUS_TYPE_STRING) {
				const char *id = NULL;
				dbus_message_iter_get_basic(&array, &id);
				sni_item_add(sni, id);
				dbus_message_iter_next(&array);
			}
		}
	}
	dbus_message_unref(reply);
}

/*
 * Async fetch of the watcher's RegisteredStatusNotifierItems property.
 * Called when connecting to an existing (external) watcher.
 */
static void
sni_fetch_registered_items(struct sni *sni)
{
	DBusConnection *conn = sni_dbus(sni);
	DBusMessage *msg = dbus_message_new_method_call(SNI_WATCHER_BUS,
		SNI_WATCHER_PATH, PROPS_IFACE, "Get");
	if (!msg) {
		return;
	}
	const char *iface = SNI_WATCHER_IFACE;
	const char *prop = "RegisteredStatusNotifierItems";
	dbus_message_append_args(msg,
		DBUS_TYPE_STRING, &iface,
		DBUS_TYPE_STRING, &prop,
		DBUS_TYPE_INVALID);

	DBusPendingCall *pending = NULL;
	if (dbus_connection_send_with_reply(conn, msg, &pending, 5000)
		&& pending) {
		dbus_pending_call_set_notify(pending,
			on_registered_items, sni, NULL);
	}
	dbus_message_unref(msg);
}

/* ---------- public plugin API -------------------------------------------- */

void
plugin_sni_create(struct panel *panel)
{
	struct sni *sni = calloc(1, sizeof(*sni));
	if (!sni) {
		return;
	}
	sni->base.panel = panel;
	sni->base.type = WIDGET_SNI;
	sni->unix_fd = -1;
	wl_list_init(&sni->items);
	wl_list_insert(panel->widgets.prev, &sni->base.link);

	/* Connect to the session bus */
	DBusError err = DBUS_ERROR_INIT;
	DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!conn) {
		wlr_log(WLR_ERROR, "sni: D-Bus connect failed: %s",
			err.message);
		dbus_error_free(&err);
		return;
	}
	sni->conn = conn;
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	/* Get the unix fd for integration with poll() */
	if (!dbus_connection_get_unix_fd(conn, &sni->unix_fd)) {
		sni->unix_fd = -1;
	}

	/* Global signal filter (method calls to the watcher go via vtable) */
	dbus_connection_add_filter(conn, signal_filter, sni, NULL);

	/* Subscribe to watcher signals */
	DBusError merr = DBUS_ERROR_INIT;
	dbus_bus_add_match(conn,
		"type='signal'"
		",sender='" SNI_WATCHER_BUS "'"
		",interface='" SNI_WATCHER_IFACE "'",
		&merr);
	dbus_error_free(&merr);

	/* Subscribe to SNI item signals (icon change notifications) */
	dbus_bus_add_match(conn,
		"type='signal'"
		",interface='" SNI_ITEM_IFACE_KDE "'",
		&merr);
	dbus_error_free(&merr);
	dbus_bus_add_match(conn,
		"type='signal'"
		",interface='" SNI_ITEM_IFACE_FDO "'",
		&merr);
	dbus_error_free(&merr);

	/* Subscribe to NameOwnerChanged for service lifetime tracking */
	dbus_bus_add_match(conn,
		"type='signal'"
		",sender='" DBUS_IFACE "'"
		",interface='" DBUS_IFACE "'"
		",member='NameOwnerChanged'",
		&merr);
	dbus_error_free(&merr);

	/* Try to become the StatusNotifierWatcher */
	int result = dbus_bus_request_name(conn, SNI_WATCHER_BUS,
		DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		sni->is_watcher = true;
		wlr_log(WLR_DEBUG, "sni: became StatusNotifierWatcher");

		/* Register the watcher object path (method calls go here) */
		dbus_connection_register_object_path(conn, SNI_WATCHER_PATH,
			&watcher_vtable, sni);
	} else {
		dbus_error_free(&err);
		wlr_log(WLR_DEBUG, "sni: connecting to existing watcher");
	}

	/*
	 * Register as host (async to avoid deadlock when we are the watcher)
	 * and request the current item list from the watcher.
	 */
	sni_register_host(sni);
	if (!sni->is_watcher) {
		sni_fetch_registered_items(sni);
	}

	dbus_connection_flush(conn);
}

void
plugin_sni_update(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_SNI) {
			sni_update_surface((struct sni *)widget);
			break;
		}
	}
}

/*
 * Called from the main event loop when the D-Bus fd is readable.
 * Reads from the socket and dispatches all pending messages / callbacks.
 */
void
plugin_sni_dispatch(struct panel *panel)
{
	struct widget *widget;
	wl_list_for_each(widget, &panel->widgets, link) {
		if (widget->type == WIDGET_SNI) {
			struct sni *sni = (struct sni *)widget;
			if (!sni->conn) {
				break;
			}
			DBusConnection *conn = sni_dbus(sni);
			dbus_connection_read_write(conn, 0);
			while (dbus_connection_dispatch(conn)
				== DBUS_DISPATCH_DATA_REMAINS) {
				/* drain: pending-call callbacks fire here */
			}
			break;
		}
	}
}

void
plugin_sni_destroy(struct sni *sni)
{
	struct sni_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &sni->items, link) {
		sni_item_destroy(item);
	}

	if (sni->conn) {
		DBusConnection *conn = sni_dbus(sni);
		dbus_connection_remove_filter(conn, signal_filter, sni);

		if (sni->is_watcher) {
			dbus_connection_unregister_object_path(conn,
				SNI_WATCHER_PATH);
			DBusError err = DBUS_ERROR_INIT;
			dbus_bus_release_name(conn, SNI_WATCHER_BUS, &err);
			dbus_error_free(&err);
		}
		dbus_connection_unref(conn);
		sni->conn = NULL;
	}

	wl_list_remove(&sni->base.link);
	if (sni->base.surface) {
		cairo_surface_destroy(sni->base.surface);
	}
	free(sni);
}
