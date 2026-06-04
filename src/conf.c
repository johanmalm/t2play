#include "conf.h"
#include <cyaml/cyaml.h>
#include <sfdo-basedir.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "common/array.h"
#include "common/log.h"
#include "common/mem.h"
#include "common/string-helpers.h"
#include "common/hex.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "panel.h"

struct yaml_conf {
	/* colors */
	char *background;
	char *text;
	char *button_background;
	char *button_active;

	/* panel */
	char *panel_items;
	int panel_breadth;

	/* taskbar */
	int taskbar_padding;
	int taskbar_spacing;
	int task_padding;

	/* startmenu */
	char *startmenu_layout;
	int startmenu_padding;

	/* clock */
	int clock_padding;

	/* battery */
	int battery_padding;

	/* keyboard */
	int keyboard_padding;
};

static const cyaml_schema_field_t yaml_conf_fields[] = {
	CYAML_FIELD_STRING_PTR("background", CYAML_FLAG_OPTIONAL, struct yaml_conf, background, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("text", CYAML_FLAG_OPTIONAL, struct yaml_conf, text, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("button_background", CYAML_FLAG_OPTIONAL, struct yaml_conf, button_background, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("button_active", CYAML_FLAG_OPTIONAL, struct yaml_conf, button_active, 0, CYAML_UNLIMITED),

	CYAML_FIELD_STRING_PTR("panel_items", CYAML_FLAG_OPTIONAL, struct yaml_conf, panel_items, 0, CYAML_UNLIMITED),
	CYAML_FIELD_INT("panel_breadth", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, panel_breadth),

	CYAML_FIELD_INT("taskbar_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, taskbar_padding),
	CYAML_FIELD_INT("taskbar_spacing", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, taskbar_spacing),
	CYAML_FIELD_INT("task_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, task_padding),

	CYAML_FIELD_STRING_PTR("startmenu_layout", CYAML_FLAG_OPTIONAL, struct yaml_conf, startmenu_layout, 0, CYAML_UNLIMITED),
	CYAML_FIELD_INT("startmenu_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, startmenu_padding),

	CYAML_FIELD_INT("clock_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, clock_padding),

	CYAML_FIELD_INT("battery_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, battery_padding),

	CYAML_FIELD_INT("keyboard_padding", CYAML_FLAG_OPTIONAL | CYAML_FLAG_POINTER, struct yaml_conf, battery_padding),

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

#define PARSE_COL(str) if (data->str) { conf->str = parse_hex(data->str); }
#define PARSE_STR(str) if (data->str) { xstrdup_replace(conf->str, data->str); }
#define PARSE_INT(str) if (data->str) { conf->str = data->str; }

static void
parse(struct conf *conf, struct yaml_conf *data)
{
	PARSE_COL(background);
	PARSE_COL(text);
	PARSE_COL(button_background);
	PARSE_COL(button_active);

	PARSE_STR(panel_items);
	PARSE_INT(panel_breadth);

	PARSE_INT(taskbar_padding);
	PARSE_INT(taskbar_spacing);
	PARSE_INT(task_padding);

	PARSE_STR(startmenu_layout);
	PARSE_INT(startmenu_padding);

	PARSE_INT(clock_padding);

	PARSE_INT(battery_padding);

	PARSE_INT(keyboard_padding);
}

#undef PARSE_COLOR

static void
load(struct conf *conf, const char *path)
{
	info("reading config file '%s'", path);

	struct yaml_conf *data = NULL;
	cyaml_err_t err = cyaml_load_file(path, &yaml_cyaml_config,
		&yaml_conf_schema, (cyaml_data_t **)&data, NULL);
	if (err == CYAML_ERR_FILE_OPEN) {
		info("no config file '%s'", path);
		return;
	}
	if (err != CYAML_OK) {
		warn("failed to load config '%s': %s", path, cyaml_strerror(err));
		return;
	}
	if (!data) {
		warn("no config file data");
		return;
	}
	parse(conf, data);
	cyaml_free(&yaml_cyaml_config, &yaml_conf_schema, data, 0);
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
		array_add(paths, strdup_printf("%st2play/config.yaml", dir));
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
	conf->button_background = 0x4A4A4AFF;
	conf->button_active = 0x5A8AC6FF;

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
	pango_font_description_free(conf->font_description);
	zfree(conf->panel_items);
	zfree(conf->startmenu_layout);
}
