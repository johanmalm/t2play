#include "conf.h"
#include <cyaml/cyaml.h>
#include <sfdo-basedir.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include <unistd.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "array.h"
#include "mem.h"
#include "string-helpers.h"
#include "hex.h"

struct yaml_conf {
	/* colors */
	char *background;
	char *text;
	char *button_background;
	char *button_active;

	/* other */
	char *panel_items;
};

static const cyaml_schema_field_t yaml_conf_fields[] = {
	CYAML_FIELD_STRING_PTR("background", CYAML_FLAG_OPTIONAL, struct yaml_conf, background, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("text", CYAML_FLAG_OPTIONAL, struct yaml_conf, text, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("button_background", CYAML_FLAG_OPTIONAL, struct yaml_conf, button_background, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("button_active", CYAML_FLAG_OPTIONAL, struct yaml_conf, button_active, 0, CYAML_UNLIMITED),

	CYAML_FIELD_STRING_PTR("panel_items", CYAML_FLAG_OPTIONAL, struct yaml_conf, panel_items, 0, CYAML_UNLIMITED),
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

# define PARSE_COLOR(str) if (data->str) { conf->str = parse_hex(data->str); }

static void
parse(struct conf *conf, struct yaml_conf *data)
{
	PARSE_COLOR(background);
	PARSE_COLOR(text);
	PARSE_COLOR(button_background);
	PARSE_COLOR(button_active);
	if (data->panel_items) {
		xstrdup_replace(conf->panel_items, data->panel_items);
	}
}

#undef PARSE_COLOR

static void
load(struct conf *conf, const char *path)
{
	wlr_log(WLR_INFO, "reading config file '%s'", path);

	struct yaml_conf *data = NULL;
	cyaml_err_t err = cyaml_load_file(path, &yaml_cyaml_config,
		&yaml_conf_schema, (cyaml_data_t **)&data, NULL);
	if (err == CYAML_ERR_FILE_OPEN) {
		wlr_log(WLR_INFO, "no config file '%s'", path);
		return;
	}
	if (err != CYAML_OK) {
		wlr_log(WLR_ERROR, "failed to load config '%s': %s", path,
			cyaml_strerror(err));
		return;
	}
	if (!data) {
		wlr_log(WLR_ERROR, "no config file data");
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
		wlr_log(WLR_ERROR, "sfdo_basedir_ctx_create() failed");
		exit(EXIT_FAILURE);
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
			array_add(paths, strdup_printf("%st2play/config.yaml",
				dirs[i].data));
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
				wlr_log(WLR_INFO, "no config file '%s'", *path);
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
	conf->panel_items = xstrdup("STKC");
}

void
conf_destroy(struct conf *conf)
{
	pango_font_description_free(conf->font_description);
	zfree(conf->panel_items);
}
