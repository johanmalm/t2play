// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <cairo.h>
#include <dbus/dbus.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include "panel.h"

/* D-Bus names and paths for the StatusNotifier protocol */
#define SNI_WATCHER_BUS   "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH  "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define SNI_HOST_PREFIX   "org.kde.StatusNotifierHost"
#define SNI_ITEM_IFACE    "org.kde.StatusNotifierItem"
#define SNI_ITEM_PATH     "/StatusNotifierItem"
#define DBUS_IFACE        "org.freedesktop.DBus"
#define PROPS_IFACE       "org.freedesktop.DBus.Properties"

/* Icon size: panel height minus small top/bottom padding */
#define SNI_ICON_SIZE 22

struct sni_icon {
	int32_t width;
	int32_t height;
	uint32_t *data; /* ARGB32, native byte order */
};

/* An individual status-notifier item (one tray icon) */
struct sni_item {
	struct sni *tray;
	char *id;             /* full id: "service/path" */
	char *service;        /* D-Bus service (unique or well-known name) */
	char *path;           /* D-Bus object path */
	char *icon_name;      /* freedesktop icon name */
	char *icon_theme_path; /* item-supplied icon search directory */
	struct sni_icon icon; /* pixmap data, if provided */
	struct wl_list link;  /* sni->items */
};

/* Forward declarations */
static void sni_item_fetch(struct sni_item *item);
static void sni_update_surface(struct sni *sni);

static inline DBusConnection *
sni_dbus(struct sni *sni)
{
	return (DBusConnection *)sni->conn;
}

/*
 * Parse a StatusNotifier item identifier.
 * Format is "service/path" where the path starts with '/'.
 * If no '/' is found, the path defaults to SNI_ITEM_PATH.
 */
static void
sni_parse_id(const char *id, char **service_out, char **path_out)
{
	assert(id);
	if (id[0] == '/') {
		/* Just an object path – should not normally happen here */
		*service_out = NULL;
		*path_out = strdup(id);
		return;
	}
	const char *slash = strchr(id, '/');
	if (slash) {
		*service_out = strndup(id, (size_t)(slash - id));
		*path_out = strdup(slash);
	} else {
		*service_out = strdup(id);
		*path_out = strdup(SNI_ITEM_PATH);
	}
}

static struct sni_item *
sni_item_find_by_id(struct sni *sni, const char *id)
{
	struct sni_item *item;
	wl_list_for_each(item, &sni->items, link) {
		if (strcmp(item->id, id) == 0) {
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
	/* Remove the per-item signal match (only if service is set) */
	if (item->service) {
		DBusConnection *conn = sni_dbus(item->tray);
		char match[512];
		snprintf(match, sizeof(match),
			"type='signal',sender='%s',path='%s'"
			",interface='" SNI_ITEM_IFACE "'",
			item->service, item->path);
		dbus_bus_remove_match(conn, match, NULL);
	}

	wl_list_remove(&item->link);
	free(item->id);
	free(item->service);
	free(item->path);
	free(item->icon_name);
	free(item->icon_theme_path);
	free(item->icon.data);
	free(item);
}

/* Load a PNG file into a cairo surface; returns NULL on failure. */
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
 * Returns a cairo surface on success, NULL on failure.
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

	/* Try the item's own icon theme path first */
	if (theme_path && theme_path[0]) {
		for (int s = 0; !surf && size_dirs[s]; s++) {
			for (int c = 0; !surf && categories[c]; c++) {
				snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
					theme_path, size_dirs[s], categories[c],
					name);
				surf = load_png(path);
			}
		}
		if (!surf) {
			snprintf(path, sizeof(path), "%s/%s.png", theme_path,
				name);
			surf = load_png(path);
		}
	}

	/* Try standard hicolor icon theme */
	const char *hicolor = "/usr/share/icons/hicolor";
	for (int s = 0; !surf && size_dirs[s]; s++) {
		for (int c = 0; !surf && categories[c]; c++) {
			snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
				hicolor, size_dirs[s], categories[c], name);
			surf = load_png(path);
		}
	}

	/* Try pixmaps directory */
	if (!surf) {
		snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png",
			name);
		surf = load_png(path);
	}

	return surf;
}

/* Return true if the new pixmap size (w) is a better match than best_w. */
static bool
is_better_pixmap(int w, int best_w)
{
	if (best_w == 0) {
		return true; /* no best yet */
	}
	int dist_new = abs(w - SNI_ICON_SIZE);
	int dist_best = abs(best_w - SNI_ICON_SIZE);
	/* Prefer closer to target; on tie, prefer larger */
	return dist_new < dist_best || (dist_new == dist_best && w > best_w);
}

/*
 * Fetch icon and title properties from the item via D-Bus Properties.GetAll.
 * Handles both IconPixmap (raw ARGB32) and IconName (theme lookup).
 */
static void
sni_item_fetch(struct sni_item *item)
{
	if (!item->service) {
		return; /* can't make D-Bus call without a service name */
	}
	DBusConnection *conn = sni_dbus(item->tray);
	DBusError err = DBUS_ERROR_INIT;

	DBusMessage *msg = dbus_message_new_method_call(item->service,
		item->path, PROPS_IFACE, "GetAll");
	if (!msg) {
		return;
	}

	const char *iface = SNI_ITEM_IFACE;
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
		DBUS_TYPE_INVALID);

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn,
		msg, 2000, &err);
	dbus_message_unref(msg);

	if (!reply) {
		if (dbus_error_is_set(&err)) {
			wlr_log(WLR_DEBUG, "sni: GetAll failed for %s%s: %s",
				item->service, item->path, err.message);
			dbus_error_free(&err);
		}
		return;
	}

	/* Reply body is a{sv} */
	DBusMessageIter iter, array;
	dbus_message_iter_init(reply, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		dbus_message_unref(reply);
		return;
	}
	dbus_message_iter_recurse(&iter, &array);

	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter dict_entry, variant;
		dbus_message_iter_recurse(&array, &dict_entry);

		if (dbus_message_iter_get_arg_type(&dict_entry)
			!= DBUS_TYPE_STRING) {
			dbus_message_iter_next(&array);
			continue;
		}
		const char *key;
		dbus_message_iter_get_basic(&dict_entry, &key);
		dbus_message_iter_next(&dict_entry);

		if (dbus_message_iter_get_arg_type(&dict_entry)
			!= DBUS_TYPE_VARIANT) {
			dbus_message_iter_next(&array);
			continue;
		}
		dbus_message_iter_recurse(&dict_entry, &variant);

		if (strcmp(key, "IconName") == 0
			&& dbus_message_iter_get_arg_type(&variant)
				== DBUS_TYPE_STRING) {
			const char *icon_name;
			dbus_message_iter_get_basic(&variant, &icon_name);
			free(item->icon_name);
			item->icon_name = (icon_name && icon_name[0])
				? strdup(icon_name)
				: NULL;

		} else if (strcmp(key, "IconThemePath") == 0
			&& dbus_message_iter_get_arg_type(&variant)
				== DBUS_TYPE_STRING) {
			const char *tp;
			dbus_message_iter_get_basic(&variant, &tp);
			free(item->icon_theme_path);
			item->icon_theme_path =
				(tp && tp[0]) ? strdup(tp) : NULL;

		} else if (strcmp(key, "IconPixmap") == 0
			&& dbus_message_iter_get_arg_type(&variant)
				== DBUS_TYPE_ARRAY) {
			/*
			 * IconPixmap is a(iiay): array of structs, each with
			 * (width int32, height int32, pixels byte-array).
			 * Pixels are ARGB32 in network byte order (big-endian).
			 */
			DBusMessageIter pix_arr;
			dbus_message_iter_recurse(&variant, &pix_arr);

			int best_w = 0, best_h = 0;
			uint8_t *best_data = NULL;
			int best_len = 0;

			while (dbus_message_iter_get_arg_type(&pix_arr)
				== DBUS_TYPE_STRUCT) {
				DBusMessageIter pix_struct;
				dbus_message_iter_recurse(&pix_arr,
					&pix_struct);

				int32_t w = 0, h = 0;
				if (dbus_message_iter_get_arg_type(&pix_struct)
					== DBUS_TYPE_INT32) {
					dbus_message_iter_get_basic(&pix_struct,
						&w);
				}
				dbus_message_iter_next(&pix_struct);
				if (dbus_message_iter_get_arg_type(&pix_struct)
					== DBUS_TYPE_INT32) {
					dbus_message_iter_get_basic(&pix_struct,
						&h);
				}
				dbus_message_iter_next(&pix_struct);

				uint8_t *data = NULL;
				int data_len = 0;
				if (dbus_message_iter_get_arg_type(&pix_struct)
					== DBUS_TYPE_ARRAY) {
					DBusMessageIter bytes_iter;
					dbus_message_iter_recurse(&pix_struct,
						&bytes_iter);
					dbus_message_iter_get_fixed_array(
						&bytes_iter, (void **)&data,
						&data_len);
				}

				/* Pick the size closest to SNI_ICON_SIZE */
				if (data && data_len == w * h * 4
					&& is_better_pixmap(w, best_w)) {
					best_w = w;
					best_h = h;
					best_data = data;
					best_len = data_len;
				}
				dbus_message_iter_next(&pix_arr);
			}

			if (best_data && best_len > 0) {
				uint32_t *new_data = malloc(
					(size_t)(best_w * best_h)
					* sizeof(uint32_t));
				if (new_data) {
					/*
					 * Convert big-endian ARGB to native
					 * ARGB32 (Cairo format).
					 */
					for (int i = 0; i < best_w * best_h;
						i++) {
						int offset = i * 4;
						uint8_t a = best_data[offset];
						uint8_t r =
							best_data[offset + 1];
						uint8_t g =
							best_data[offset + 2];
						uint8_t b =
							best_data[offset + 3];
						new_data[i] =
							((uint32_t)a << 24)
							| ((uint32_t)r << 16)
							| ((uint32_t)g << 8)
							| (uint32_t)b;
					}
					free(item->icon.data);
					item->icon.data = new_data;
					item->icon.width = best_w;
					item->icon.height = best_h;
				}
			}
		}
		dbus_message_iter_next(&array);
	}
	dbus_message_unref(reply);
}

/* Redraw the SNI tray widget surface from current item list. */
static void
sni_update_surface(struct sni *sni)
{
	struct panel *panel = sni->base.panel;
	int n = wl_list_length(&sni->items);
	int slot = panel->height; /* each icon occupies a square slot */
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
		total_w, panel->height);
	cairo_t *cr = cairo_create(sni->base.surface);

	int icon_size = SNI_ICON_SIZE;
	if (icon_size > panel->height) {
		icon_size = panel->height;
	}

	int x = 0;
	struct sni_item *item;
	wl_list_for_each(item, &sni->items, link) {
		int icon_x = x + (slot - icon_size) / 2;
		int icon_y = (panel->height - icon_size) / 2;

		cairo_surface_t *icon_surf = NULL;

		if (item->icon.data && item->icon.width > 0
			&& item->icon.height > 0) {
			/* Use pixmap data provided by the application */
			icon_surf = cairo_image_surface_create_for_data(
				(uint8_t *)item->icon.data,
				CAIRO_FORMAT_ARGB32, item->icon.width,
				item->icon.height,
				item->icon.width * (int)sizeof(uint32_t));
		} else if (item->icon_name) {
			/* Fall back to icon-theme file lookup */
			icon_surf = load_icon_by_name(item->icon_name,
				item->icon_theme_path);
		}

		if (icon_surf) {
			int iw = cairo_image_surface_get_width(icon_surf);
			int ih = cairo_image_surface_get_height(icon_surf);
			cairo_save(cr);
			cairo_translate(cr, icon_x, icon_y);
			if (iw > 0 && ih > 0) {
				cairo_scale(cr, (double)icon_size / iw,
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

/* Add a new item to the tray (idempotent). */
static void
sni_item_add(struct sni *sni, const char *id)
{
	if (!id || !id[0]) {
		return;
	}
	if (sni_item_find_by_id(sni, id)) {
		return; /* already present */
	}

	struct sni_item *item = calloc(1, sizeof(*item));
	if (!item) {
		return;
	}

	item->tray = sni;
	item->id = strdup(id);
	sni_parse_id(id, &item->service, &item->path);
	wl_list_insert(sni->items.prev, &item->link);

	/* Subscribe to signals from this specific item (only if service known) */
	if (item->service) {
		DBusError err = DBUS_ERROR_INIT;
		char match[512];
		snprintf(match, sizeof(match),
			"type='signal',sender='%s',path='%s'"
			",interface='" SNI_ITEM_IFACE "'",
			item->service, item->path);
		dbus_bus_add_match(sni_dbus(sni), match, &err);
		if (dbus_error_is_set(&err)) {
			wlr_log(WLR_DEBUG, "sni: add_match for %s: %s", id,
				err.message);
			dbus_error_free(&err);
		}
	}

	/* Fetch icon/title */
	sni_item_fetch(item);
	sni_update_surface(sni);
	render_frame(sni->base.panel);
}

/* Remove all items whose service name matches (service disappeared). */
static void
sni_item_remove_by_service(struct sni *sni, const char *service)
{
	if (!service) {
		return;
	}
	struct sni_item *item, *tmp;
	bool changed = false;
	wl_list_for_each_safe(item, tmp, &sni->items, link) {
		if (item->service && strcmp(item->service, service) == 0) {
			sni_item_destroy(item);
			changed = true;
		}
	}
	if (changed) {
		sni_update_surface(sni);
		render_frame(sni->base.panel);
	}
}

/* --- Watcher method handlers -------------------------------------------- */

/*
 * Build the canonical "service/path" id from the argument passed to
 * RegisterStatusNotifierItem and the message sender.
 * The SNI spec allows:
 *   - An object path starting with '/'  -> sender + arg
 *   - A "service/path" string           -> arg as-is
 *   - A bare service name               -> arg + SNI_ITEM_PATH
 *   - Empty / NULL                      -> sender + SNI_ITEM_PATH
 */
static void
build_canonical_id(const char *arg, const char *sender, char *out, size_t len)
{
	if (arg && arg[0] == '/') {
		snprintf(out, len, "%s%s", sender, arg);
	} else if (arg && strchr(arg, '/')) {
		snprintf(out, len, "%s", arg);
	} else if (arg && arg[0]) {
		snprintf(out, len, "%s%s", arg, SNI_ITEM_PATH);
	} else {
		snprintf(out, len, "%s%s", sender, SNI_ITEM_PATH);
	}
}

static DBusHandlerResult
handle_watcher_method(struct sni *sni, DBusConnection *conn, DBusMessage *msg)
{
	const char *member = dbus_message_get_member(msg);

	if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
		const char *arg = NULL;
		dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &arg,
			DBUS_TYPE_INVALID);

		const char *sender = dbus_message_get_sender(msg);

		/* Build canonical item id */
		char id[512];
		build_canonical_id(arg, sender, id, sizeof(id));

		/* Reply with empty success */
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply) {
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}

		/* Add item before emitting the signal */
		sni_item_add(sni, id);

		/* Emit StatusNotifierItemRegistered */
		DBusMessage *sig = dbus_message_new_signal(SNI_WATCHER_PATH,
			SNI_WATCHER_IFACE, "StatusNotifierItemRegistered");
		if (sig) {
			const char *id_p = id;
			dbus_message_append_args(sig, DBUS_TYPE_STRING, &id_p,
				DBUS_TYPE_INVALID);
			dbus_connection_send(conn, sig, NULL);
			dbus_message_unref(sig);
		}
		return DBUS_HANDLER_RESULT_HANDLED;

	} else if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply) {
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		DBusMessage *sig = dbus_message_new_signal(SNI_WATCHER_PATH,
			SNI_WATCHER_IFACE, "StatusNotifierHostRegistered");
		if (sig) {
			dbus_connection_send(conn, sig, NULL);
			dbus_message_unref(sig);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* Respond to Properties.Get requests for the watcher interface. */
static DBusHandlerResult
handle_watcher_props_get(struct sni *sni, DBusConnection *conn,
	DBusMessage *msg)
{
	const char *iface = NULL, *prop = NULL;
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface,
		    DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (strcmp(iface, SNI_WATCHER_IFACE) != 0) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	DBusMessage *reply = dbus_message_new_method_return(msg);
	if (!reply) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	DBusMessageIter ri, var;
	dbus_message_iter_init_append(reply, &ri);

	if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
		dbus_message_iter_open_container(&ri, DBUS_TYPE_VARIANT, "as",
			&var);
		DBusMessageIter arr;
		dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s",
			&arr);
		struct sni_item *item;
		wl_list_for_each(item, &sni->items, link) {
			const char *id = item->id;
			dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING,
				&id);
		}
		dbus_message_iter_close_container(&var, &arr);
		dbus_message_iter_close_container(&ri, &var);

	} else if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
		dbus_message_iter_open_container(&ri, DBUS_TYPE_VARIANT, "b",
			&var);
		dbus_bool_t val = TRUE;
		dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &val);
		dbus_message_iter_close_container(&ri, &var);

	} else if (strcmp(prop, "ProtocolVersion") == 0) {
		dbus_message_iter_open_container(&ri, DBUS_TYPE_VARIANT, "i",
			&var);
		int32_t val = 0;
		dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &val);
		dbus_message_iter_close_container(&ri, &var);

	} else {
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* --- Global D-Bus message filter ----------------------------------------- */

static DBusHandlerResult
message_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
	struct sni *sni = data;
	int type = dbus_message_get_type(msg);
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	const char *path = dbus_message_get_object_path(msg);

	if (!iface || !member) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* Handle method calls directed to our watcher object */
	if (type == DBUS_MESSAGE_TYPE_METHOD_CALL && sni->is_watcher && path
		&& strcmp(path, SNI_WATCHER_PATH) == 0) {
		if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
			return handle_watcher_method(sni, conn, msg);
		}
		if (strcmp(iface, PROPS_IFACE) == 0
			&& strcmp(member, "Get") == 0) {
			return handle_watcher_props_get(sni, conn, msg);
		}
	}

	/* Handle signals */
	if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
		/* Item registered/unregistered signals from the watcher */
		if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
			if (strcmp(member, "StatusNotifierItemRegistered")
				== 0) {
				const char *id = NULL;
				dbus_message_get_args(msg, NULL,
					DBUS_TYPE_STRING, &id,
					DBUS_TYPE_INVALID);
				sni_item_add(sni, id);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			if (strcmp(member, "StatusNotifierItemUnregistered")
				== 0) {
				const char *id = NULL;
				dbus_message_get_args(msg, NULL,
					DBUS_TYPE_STRING, &id,
					DBUS_TYPE_INVALID);
				if (id) {
					char *svc = NULL, *obj_path = NULL;
					sni_parse_id(id, &svc, &obj_path);
					sni_item_remove_by_service(sni, svc);
					free(svc);
					free(obj_path);
				}
				return DBUS_HANDLER_RESULT_HANDLED;
			}
		}

		/* Icon change signals from an item */
		if (strcmp(iface, SNI_ITEM_IFACE) == 0
			&& (strcmp(member, "NewIcon") == 0
				|| strcmp(member, "NewAttentionIcon") == 0
				|| strcmp(member, "NewOverlayIcon") == 0)) {
			const char *sender = dbus_message_get_sender(msg);
			struct sni_item *item =
				sni_item_find_by_service(sni, sender);
			if (item) {
				sni_item_fetch(item);
				sni_update_surface(sni);
				render_frame(sni->base.panel);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		/* Service disappeared: clean up its items */
		if (strcmp(iface, DBUS_IFACE) == 0
			&& strcmp(member, "NameOwnerChanged") == 0) {
			const char *name = NULL, *old_owner = NULL,
				   *new_owner = NULL;
			dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING,
				&name, DBUS_TYPE_STRING, &old_owner,
				DBUS_TYPE_STRING, &new_owner,
				DBUS_TYPE_INVALID);
			/* new_owner == "" means the name was released */
			if (name && old_owner && old_owner[0]
				&& (!new_owner || !new_owner[0])) {
				sni_item_remove_by_service(sni, name);
				sni_item_remove_by_service(sni, old_owner);
			}
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* Register ourselves as a StatusNotifierHost with the watcher. */
static void
sni_register_host(struct sni *sni)
{
	DBusConnection *conn = sni_dbus(sni);
	DBusError err = DBUS_ERROR_INIT;

	const char *unique = dbus_bus_get_unique_name(conn);
	char host_name[256];
	snprintf(host_name, sizeof(host_name), "%s-%s", SNI_HOST_PREFIX,
		unique ? unique : "unknown");

	/* Claim the host bus name (best-effort) */
	dbus_bus_request_name(conn, host_name, DBUS_NAME_FLAG_DO_NOT_QUEUE,
		&err);
	dbus_error_free(&err);

	/* Call RegisterStatusNotifierHost on the watcher */
	DBusMessage *msg = dbus_message_new_method_call(SNI_WATCHER_BUS,
		SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
		"RegisterStatusNotifierHost");
	if (msg) {
		const char *name_p = host_name;
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &name_p,
			DBUS_TYPE_INVALID);
		DBusMessage *reply = dbus_connection_send_with_reply_and_block(
			conn, msg, 2000, &err);
		dbus_message_unref(msg);
		if (reply) {
			dbus_message_unref(reply);
		} else {
			dbus_error_free(&err);
		}
	}
	sni->host_registered = true;
}

/*
 * Query the watcher for its current list of registered items and add them.
 * Called once at startup when an existing watcher is found.
 */
static void
sni_fetch_registered_items(struct sni *sni)
{
	DBusConnection *conn = sni_dbus(sni);
	DBusError err = DBUS_ERROR_INIT;

	DBusMessage *msg = dbus_message_new_method_call(SNI_WATCHER_BUS,
		SNI_WATCHER_PATH, PROPS_IFACE, "Get");
	if (!msg) {
		return;
	}

	const char *iface = SNI_WATCHER_IFACE;
	const char *prop = "RegisteredStatusNotifierItems";
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
		DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn,
		msg, 2000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		dbus_error_free(&err);
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

/* --- Public plugin API --------------------------------------------------- */

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

	/* Connect to the D-Bus session bus */
	DBusError err = DBUS_ERROR_INIT;
	DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!conn) {
		wlr_log(WLR_ERROR, "sni: failed to connect to D-Bus: %s",
			err.message);
		dbus_error_free(&err);
		return;
	}
	sni->conn = conn;
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	/* Obtain the unix socket fd for integration with poll() */
	if (!dbus_connection_get_unix_fd(conn, &sni->unix_fd)) {
		sni->unix_fd = -1;
	}

	/* Install the global message filter */
	dbus_connection_add_filter(conn, message_filter, sni, NULL);

	/* Subscribe to StatusNotifier watcher signals */
	DBusError merr = DBUS_ERROR_INIT;
	dbus_bus_add_match(conn,
		"type='signal'"
		",sender='" SNI_WATCHER_BUS "'"
		",interface='" SNI_WATCHER_IFACE "'",
		&merr);
	dbus_error_free(&merr);

	/* Subscribe to all SNI item signals (for icon-change notifications) */
	dbus_bus_add_match(conn,
		"type='signal'"
		",interface='" SNI_ITEM_IFACE "'",
		&merr);
	dbus_error_free(&merr);

	/* Subscribe to NameOwnerChanged so we can track service lifetimes */
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
		/* We are both watcher and host */
		sni_register_host(sni);
	} else {
		dbus_error_free(&err);
		wlr_log(WLR_DEBUG, "sni: connecting to existing watcher");
		sni_register_host(sni);
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
 * Reads from the socket and dispatches all pending messages.
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
				/* drain the queue */
			}
			break;
		}
	}
}

void
plugin_sni_destroy(struct sni *sni)
{
	/* Destroy all tracked items */
	struct sni_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &sni->items, link) {
		sni_item_destroy(item);
	}

	if (sni->conn) {
		DBusConnection *conn = sni_dbus(sni);
		dbus_connection_remove_filter(conn, message_filter, sni);

		if (sni->is_watcher) {
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
