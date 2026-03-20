#include "conf.h"
#include <cyaml/cyaml.h>
#include <wlr/util/log.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct yaml_conf {
	char *panel_items;
};

static const cyaml_schema_field_t yaml_conf_fields[] = {
	CYAML_FIELD_STRING_PTR("panel_items", CYAML_FLAG_OPTIONAL,
		struct yaml_conf, panel_items, 0, CYAML_UNLIMITED),
	CYAML_FIELD_END};

static const cyaml_schema_value_t yaml_conf_schema = {
	CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, struct yaml_conf,
		yaml_conf_fields),
};

static const cyaml_config_t yaml_cyaml_config = {
	.log_fn = cyaml_log,
	.mem_fn = cyaml_mem,
	.log_level = CYAML_LOG_WARNING,
	.flags = CYAML_CFG_IGNORE_UNKNOWN_KEYS,
};

void
conf_load(struct conf *conf, const char *path)
{
	struct yaml_conf *data = NULL;
	cyaml_err_t err = cyaml_load_file(path, &yaml_cyaml_config,
		&yaml_conf_schema, (cyaml_data_t **)&data, NULL);
	if (err == CYAML_ERR_FILE_OPEN) {
		return;
	}
	if (err != CYAML_OK) {
		wlr_log(WLR_ERROR, "Failed to load config '%s': %s", path,
			cyaml_strerror(err));
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

void
conf_init(struct conf *conf)
{
	conf->font_description =
		pango_font_description_from_string("pango:Sans 10");
	conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	conf->background = 0x323232FF;
	conf->text = 0xFFFFFFFF;
	conf->button_background = 0x4A4A4AFF;
	conf->button_active = 0x5A8AC6FF;
	conf->panel_items = strdup("STKC");
}

void
conf_destroy(struct conf *conf)
{
	pango_font_description_free(conf->font_description);
	free(conf->panel_items);
	conf->panel_items = NULL;
}
