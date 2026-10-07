#include "conf.h"
#include <assert.h>
#include <ctype.h>
#include <sfdo-basedir.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "common/cleanup.h"
#include "common/array.h"
#include "common/log.h"
#include "common/mem.h"
#include "common/string-helpers.h"
#include "common/hex.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "panel.h"

static void
rtrim(char *s)
{
	assert(s);
	size_t len = strlen(s);
	if (!len) {
		return;
	}
	char *end = s + len - 1;
	while (end >= s && isspace((unsigned char)*end)) {
		end--;
	}
	*(end + 1) = '\0';
}

static char *
string_strip(char *s)
{
	assert(s);
	rtrim(s);
	while (isspace((unsigned char)*s)) {
		s++;
	}
	return s;
}

static void
parse_key_value_pair(char *line, char **key, char **value, char delim)
{
	assert(line);
	char *p = line;
	while ((p[0] == ' ') || p[0] == '\t') {
		p++;
	}
	if (p[0] == '#') {
		return;
	}
	p = strchr(line, delim);
	if (!p) {
		return;
	}
	p[0] = '\0';
	*key = string_strip(line);
	*value = string_strip(++p);
}

static void
process_line(struct conf *conf, char *line)
{
	char *key = NULL, *value = NULL;
	parse_key_value_pair(line, &key, &value, '=');
	if (!key || !value) {
		return;
	}

	/* colors */
	if (!strcmp(key, "background")) {
		conf->background = parse_hex(value);
	} else if (!strcmp(key, "text")) {
		conf->text = parse_hex(value);
	} else if (!strcmp(key, "task_background_color")) {
		conf->task_background_color = parse_hex(value);
	} else if (!strcmp(key, "task_active_background_color")) {
		conf->task_active_background_color = parse_hex(value);

	/* panel */
	} else if (!strcmp(key, "panel_items")) {
		xstrdup_replace(conf->panel_items, value);
	} else if (!strcmp(key, "panel_breadth")) {
		conf->panel_breadth = atoi(value);

	/* taskbar */
	} else if (!strcmp(key, "taskbar_padding")) {
		conf->taskbar_padding = atoi(value);
	} else if (!strcmp(key, "taskbar_spacing")) {
		conf->taskbar_spacing = atoi(value);
	} else if (!strcmp(key, "task_padding")) {
		conf->task_padding = atoi(value);

	/* startmenu */
	} else if (!strcmp(key, "startmenu_layout")) {
		xstrdup_replace(conf->startmenu_layout, value);
	} else if (!strcmp(key, "startmenu_padding")) {
		conf->startmenu_padding = atoi(value);

	/* clock */
	} else if (!strcmp(key, "clock_padding")) {
		conf->clock_padding = atoi(value);

	/* battery */
	} else if (!strcmp(key, "battery_padding")) {
		conf->battery_padding = atoi(value);

	/* keyboard */
	} else if (!strcmp(key, "keyboard_paddnig")) {
		conf->keyboard_padding = atoi(value);
	}
}

static int
load(struct conf *conf, const char *path)
{
	info("reading config file '%s'", path);

	cleanup_fclose FILE *fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}

	cleanup_free char *line = NULL;
	size_t capacity = 0;
	ssize_t len;

	while ((len = getline(&line, &capacity, fp)) != -1) {
		if (len > 0 && line[len - 1] == '\n') {
			line[--len] = '\0';
		}
		process_line(conf, line);
	}
	return 0;
}

static void
get_paths(struct wl_array *paths)
{
	struct sfdo_basedir_ctx *ctx = sfdo_basedir_ctx_create();
	if (!ctx) {
		die("sfdo_basedir_ctx_create() failed");
	}
	/* Build XDG_CONFIG_HOME path */
	const char *dir;
	size_t dir_len;
	dir = sfdo_basedir_get_config_home(ctx, &dir_len);
	if (dir) {
		array_add(paths, strdup_printf("%st2play/config", dir));
	}

	/* Build XDG_CONFIG_DIRS paths */
	const struct sfdo_string *dirs;
	size_t n_dirs;
	dirs = sfdo_basedir_get_config_system_dirs(ctx, &n_dirs);
	for (size_t i = 0; i < n_dirs; i++) {
		if (dirs[i].data) {
			array_add(paths, strdup_printf("%st2play/config.yaml", dirs[i].data));
		}
	}
	sfdo_basedir_ctx_destroy(ctx);
}

void
conf_load(struct conf *conf, const char *config_file)
{
	if (config_file) {
		/* If user specified `-c <file>`, then use that */
		load(conf, config_file);
	} else {
		struct wl_array paths;
		wl_array_init(&paths);
		get_paths(&paths);

		char **path;
		wl_array_for_each(path, &paths) {
			if (access(*path, R_OK) != 0) {
				info("no config file '%s'", *path);
				continue;
			}
			load(conf, *path);
			break;
		}
		wl_array_for_each(path, &paths) {
			free(*path);
		}
		wl_array_release(&paths);
	}
}

void
conf_init(struct conf *conf)
{
	conf->font_description = pango_font_description_from_string("pango:Sans 10");
	conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;

	conf->background = 0x323232FF;
	conf->text = 0xFFFFFFFF;
	conf->task_background_color = 0x4A4A4AFF;
	conf->task_active_background_color = 0x5A8AC6FF;

	conf->panel_items = xstrdup("STBKC");
	conf->panel_breadth = 40;

	conf->taskbar_padding = 8;
	conf->taskbar_spacing = 6;
	conf->task_padding = 8;

	conf->startmenu_layout = xstrdup("<vbox><search/><applist/></vbox>");
	conf->startmenu_padding = 8;

	conf->clock_padding = 8;

	conf->battery_padding = 8;

	conf->keyboard_padding = 8;
}

void
conf_destroy(struct conf *conf)
{
	text_measure_fini();
	pango_font_description_free(conf->font_description);
	zfree(conf->panel_items);
	zfree(conf->startmenu_layout);
}
