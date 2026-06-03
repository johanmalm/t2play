// SPDX-License-Identifier: GPL-2.0-only
#include "desktop-entry.h"
#include <assert.h>
#include <cairo.h>
#include <math.h>
#include <sfdo-common.h>
#include <sfdo-desktop.h>
#include <sfdo-icon.h>
#include <sfdo-basedir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "conf.h"
#include "common/log.h"
#include "common/mem.h"
#include "common/string-helpers.h"
#include "panel.h"

static const char *debug_libsfdo;

static void
___log(const char *fmt, va_list args)
{
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}

static void
log_handler(enum sfdo_log_level level, const char *fmt, va_list args, void *tag)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "[%s] %s", (const char *)tag, fmt);
	___log(buf, args);
}

void
desktop_entry_init(struct panel *panel)
{
	struct sfdo *sfdo = znew(*sfdo);

	debug_libsfdo = getenv("LABWC_DEBUG_LIBSFDO");

	struct sfdo_basedir_ctx *basedir_ctx = sfdo_basedir_ctx_create();
	if (!basedir_ctx) {
		goto err_basedir_ctx;
	}
	sfdo->desktop_ctx = sfdo_desktop_ctx_create(basedir_ctx);
	if (!sfdo->desktop_ctx) {
		goto err_desktop_ctx;
	}
	sfdo->icon_ctx = sfdo_icon_ctx_create(basedir_ctx);
	if (!sfdo->icon_ctx) {
		goto err_icon_ctx;
	}

	/* sfdo_log_level and wlr_log_importance are compatible */
	enum sfdo_log_level level = SFDO_LOG_LEVEL_INFO;
	sfdo_desktop_ctx_set_log_handler(sfdo->desktop_ctx, level, log_handler, "sfdo-desktop");
	sfdo_icon_ctx_set_log_handler(sfdo->icon_ctx, level, log_handler, "sfdo-icon");

	char *locale = NULL;

	sfdo->desktop_db = sfdo_desktop_db_load(sfdo->desktop_ctx, locale);
	if (!sfdo->desktop_db) {
		goto err_desktop_db;
	}

	/*
	 * We set some relaxed load options to accommodate delinquent themes in
	 * the wild, namely:
	 *
	 * - SFDO_ICON_THEME_LOAD_OPTION_RELAXED to "impose less restrictions
	 *   on the format of icon theme files"
	 *
	 * - SFDO_ICON_THEME_LOAD_OPTION_ALLOW_MISSING to "continue loading
	 *   even if it fails to find a theme or one of its dependencies."
	 */
	int load_options = SFDO_ICON_THEME_LOAD_OPTIONS_DEFAULT
		| SFDO_ICON_THEME_LOAD_OPTION_RELAXED
		| SFDO_ICON_THEME_LOAD_OPTION_ALLOW_MISSING;

	// TODO: Make icon theme name configurable
	char *icon_theme = "Papirus";
	sfdo->icon_theme = sfdo_icon_theme_load(sfdo->icon_ctx, icon_theme, load_options);
	if (!sfdo->icon_theme) {
		warn("Failed to load icon theme %s, falling back to 'hicolor'", icon_theme);
		sfdo->icon_theme = sfdo_icon_theme_load(sfdo->icon_ctx, "hicolor", load_options);
	}
	if (!sfdo->icon_theme) {
		goto err_icon_theme;
	}

	/* basedir_ctx is not referenced by other objects */
	sfdo_basedir_ctx_destroy(basedir_ctx);

	panel->sfdo = sfdo;
	return;

err_icon_theme:
	sfdo_desktop_db_destroy(sfdo->desktop_db);
err_desktop_db:
	sfdo_icon_ctx_destroy(sfdo->icon_ctx);
err_icon_ctx:
	sfdo_desktop_ctx_destroy(sfdo->desktop_ctx);
err_desktop_ctx:
	sfdo_basedir_ctx_destroy(basedir_ctx);
err_basedir_ctx:
	free(sfdo);
	warn("Failed to initialize icon loader");
}

void
desktop_entry_finish(struct panel *panel)
{
	struct sfdo *sfdo = panel->sfdo;
	if (!sfdo) {
		return;
	}

	sfdo_icon_theme_destroy(sfdo->icon_theme);
	sfdo_desktop_db_destroy(sfdo->desktop_db);
	sfdo_icon_ctx_destroy(sfdo->icon_ctx);
	sfdo_desktop_ctx_destroy(sfdo->desktop_ctx);
	free(sfdo);
	panel->sfdo = NULL;
}

struct icon_ctx {
	char *path;
	enum sfdo_icon_file_format format;
};

/* Return the length of a filename minus any known extension */
static size_t
length_without_extension(const char *name)
{
	size_t len = strlen(name);
	if (len >= 4 && name[len - 4] == '.') {
		const char *ext = &name[len - 3];
		if (!strcmp(ext, "png") || !strcmp(ext, "svg")
				|| !strcmp(ext, "xpm")) {
			len -= 4;
		}
	}
	return len;
}

/*
 * Return 0 on success and -1 on error
 * The calling function is responsible for free()ing ctx->path
 */
static int
process_rel_name(struct icon_ctx *ctx, const char *icon_name,
		struct sfdo *sfdo, int size, int scale)
{
	int ret = 0;
	int lookup_options = SFDO_ICON_THEME_LOOKUP_OPTIONS_DEFAULT;
#if !HAVE_RSVG
	lookup_options |= SFDO_ICON_THEME_LOOKUP_OPTION_NO_SVG;
#endif

	/*
	 * Relative icon names are not supposed to include an extension,
	 * but some .desktop files include one anyway. libsfdo does not
	 * allow this in lookups, so strip the extension first.
	 */
	size_t name_len = length_without_extension(icon_name);
	struct sfdo_icon_file *icon_file = sfdo_icon_theme_lookup(
		sfdo->icon_theme, icon_name, name_len, size, scale,
		lookup_options);
	if (!icon_file || icon_file == SFDO_ICON_FILE_INVALID) {
		ret = -1;
		goto out;
	}
	ctx->path = xstrdup(sfdo_icon_file_get_path(icon_file, NULL));
	ctx->format = sfdo_icon_file_get_format(icon_file);
out:
	sfdo_icon_file_destroy(icon_file);
	return ret;
}

static int
process_abs_name(struct icon_ctx *ctx, const char *icon_name)
{
	ctx->path = xstrdup(icon_name);
	if (str_endswith_ignore_case(icon_name, ".png")) {
		ctx->format = SFDO_ICON_FILE_FORMAT_PNG;
	} else if (str_endswith_ignore_case(icon_name, ".svg")) {
		ctx->format = SFDO_ICON_FILE_FORMAT_SVG;
	} else if (str_endswith_ignore_case(icon_name, ".xpm")) {
		ctx->format = SFDO_ICON_FILE_FORMAT_XPM;
	} else {
		goto err;
	}
	return 0;
err:
	warn("'%s' has invalid file extension", icon_name);
	free(ctx->path);
	return -1;
}

/*
 * Looks up an application desktop entry using fuzzy matching
 * (e.g. "thunderbird" matches "org.mozilla.Thunderbird.desktop"
 * and "XTerm" matches "xterm.desktop"). This is not per any spec
 * but is needed to find icons for existing applications.
 *
 * The second loop tries to match more partial strings, for
 * example "gimp-2.0" would match "org.something.gimp.desktop".
 */
static struct sfdo_desktop_entry *
get_db_entry_by_id_fuzzy(struct sfdo_desktop_db *db, const char *app_id)
{
	size_t n_entries;
	struct sfdo_desktop_entry **entries = sfdo_desktop_db_get_entries(db, &n_entries);

	/* Would match "org.foobar.xterm" when given app-id "XTerm" */
	for (size_t i = 0; i < n_entries; i++) {
		struct sfdo_desktop_entry *entry = entries[i];
		const char *desktop_id = sfdo_desktop_entry_get_id(entry, NULL);
		/* Get portion of desktop ID after last '.' */
		const char *dot = strrchr(desktop_id, '.');
		const char *desktop_id_base = dot ? (dot + 1) : desktop_id;

		if (!strcasecmp(app_id, desktop_id_base)) {
			warn("'%s' to '%s.desktop' via case-insensitive match", app_id, desktop_id);
			return entry;
		}

		/* sfdo_desktop_entry_get_startup_wm_class() asserts against APPLICATION */
		if (sfdo_desktop_entry_get_type(entry) != SFDO_DESKTOP_ENTRY_APPLICATION) {
			continue;
		}

		/* Try desktop entry's StartupWMClass also */
		const char *wm_class =
			sfdo_desktop_entry_get_startup_wm_class(entry, NULL);
		if (wm_class && !strcasecmp(app_id, wm_class)) {
			warn("'%s' to '%s.desktop' via StartupWMClass", app_id, desktop_id);
			return entry;
		}
	}

	/* Would match "org.foobar.xterm-unicode" when given app-id "XTerm" */
	const int app_id_len = strlen(app_id);
	for (size_t i = 0; i < n_entries; i++) {
		struct sfdo_desktop_entry *entry = entries[i];
		const char *desktop_id = sfdo_desktop_entry_get_id(entry, NULL);
		const char *dot = strrchr(desktop_id, '.');
		const char *desktop_id_base = dot ? (dot + 1) : desktop_id;
		const int dlen = strlen(desktop_id_base);
		const int cmp_len = MIN(app_id_len, dlen);
		if (cmp_len < 3) {
			/*
			 * Without this check, app-id "foot" would match
			 * "something.f" and any app-id would match "R.E.P.O."
			 */
			continue;
		}
		if (!strncasecmp(app_id, desktop_id_base, cmp_len)) {
			debug("'%s' to '%s.desktop' via partial match", app_id, desktop_id);
			return entry;
		}
	}

	return NULL;
}

static struct sfdo_desktop_entry *
get_desktop_entry(struct sfdo *sfdo, const char *app_id)
{
	assert(sfdo);
	struct sfdo_desktop_entry *entry = sfdo_desktop_db_get_entry_by_id(sfdo->desktop_db, app_id, SFDO_NT);
	if (entry) {
		debug("matched '%s.desktop' via exact match", app_id);
		return entry;
	}
	entry = get_db_entry_by_id_fuzzy(sfdo->desktop_db, app_id);
	if (!entry) {
		debug("failed to find .desktop file for '%s'", app_id);
	}
	return entry;
}

static void
add_png(cairo_t *cairo, struct panel *panel, const char *filename, int icon_size)
{
	cairo_save(cairo);
	cairo_surface_t *image = cairo_image_surface_create_from_png(filename);
	if (cairo_surface_status(image)) {
		cairo_surface_destroy(image);
		warn("bad png icon (%s)", filename);
		return;
	}

	cairo_translate(cairo, panel->conf->task_padding, 0);

	/* TODO: use cairo_image_surface_scale() */
	double w, h, max;
	w = cairo_image_surface_get_width(image);
	h = cairo_image_surface_get_height(image);
	max = h > w ? h : w;
	if (max != icon_size) {
		cairo_scale(cairo, icon_size / max, icon_size / max);
	}
	cairo_set_source_surface(cairo, image, 0, 0);
	cairo_paint_with_alpha(cairo, 1.0);
	cairo_surface_destroy(image);
	cairo_restore(cairo);
}

void
desktop_entry_load_icon(cairo_t *cairo, struct panel *panel, const char *icon_name,
		int size, float scale)
{
	/* static analyzer isn't able to detect the NULL check in string_null_or_empty() */
	if (string_null_or_empty(icon_name) || !icon_name) {
		return;
	}

	struct sfdo *sfdo = panel->sfdo;
	if (!sfdo) {
		return;
	}

	/*
	 * libsfdo doesn't support loading icons for fractional scales,
	 * so round down and increase the icon size to compensate.
	 */
	int lookup_scale = MAX((int)scale, 1);
	int lookup_size = lroundf(size * scale / lookup_scale);

	struct icon_ctx ctx = {0};
	int ret;
	if (icon_name[0] == '/') {
		ret = process_abs_name(&ctx, icon_name);
	} else {
		ret = process_rel_name(&ctx, icon_name, sfdo, lookup_size, lookup_scale);
	}
	if (ret < 0) {
		info("failed to load icon file %s", icon_name);
		return;
	}

	debug("loading icon file %s", ctx.path);
	if (ctx.format == SFDO_ICON_FILE_FORMAT_PNG) {
		add_png(cairo, panel, ctx.path, 22);
	}
	// TODO: Handle SVG too

	free(ctx.path);
}

void
desktop_entry_load_icon_from_app_id(cairo_t *cairo, struct panel *panel, const char *app_id,
		int size, float scale)
{
	if (string_null_or_empty(app_id)) {
		return;
	}

	struct sfdo *sfdo = panel->sfdo;
	if (!sfdo) {
		return;
	}

	const char *icon_name = NULL;
	struct sfdo_desktop_entry *entry = get_desktop_entry(sfdo, app_id);
	if (entry) {
		icon_name = sfdo_desktop_entry_get_icon(entry, NULL);
	}

	desktop_entry_load_icon(cairo, panel, icon_name, size, scale);
	// TODO
	// if (above failed) {
	// 	/* Icon not defined in .desktop file or could not be loaded */
	// 	desktop_entry_load_icon(cairo, app_id, size, scale);
	// }
}

const char *
desktop_entry_name_lookup(struct panel *panel, const char *app_id)
{
	if (string_null_or_empty(app_id)) {
		return NULL;
	}

	struct sfdo *sfdo = panel->sfdo;
	if (!sfdo) {
		return NULL;
	}

	struct sfdo_desktop_entry *entry = get_desktop_entry(sfdo, app_id);
	if (!entry) {
		return NULL;
	}

	size_t len;
	const char *name = sfdo_desktop_entry_get_name(entry, &len);
	if (!len) {
		return NULL;
	}

	return name;
}
