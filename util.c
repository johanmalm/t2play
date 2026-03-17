// SPDX-License-Identifier: GPL-2.0-only
#include <wlr/util/log.h>
#include "panel.h"

void
die_if_null(void *ptr)
{
	if (!ptr) {
		perror("Failed to allocate memory");
		exit(EXIT_FAILURE);
	}
}

void *
xzalloc(size_t size)
{
	if (!size) {
		return NULL;
	}
	void *ptr = calloc(1, size);
	die_if_null(ptr);
	return ptr;
}

PangoRectangle
get_text_size(const PangoFontDescription *desc, const char *string)
{
	PangoRectangle rect = {0};
	if (!string || !*string) {
		return rect;
	}
	cairo_surface_t *surface;
	cairo_t *c;
	PangoLayout *layout;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	c = cairo_create(surface);
	layout = pango_cairo_create_layout(c);
	pango_context_set_round_glyph_positions(
		pango_layout_get_context(layout), false);

	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, string, -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
	pango_layout_get_extents(layout, NULL, &rect);
	pango_extents_to_pixels(&rect, NULL);

	cairo_destroy(c);
	cairo_surface_destroy(surface);
	g_object_unref(layout);
	return rect;
}

static PangoLayout *
get_pango_layout(cairo_t *cairo, const PangoFontDescription *desc,
	const char *text, double scale, bool markup)
{
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	pango_context_set_round_glyph_positions(
		pango_layout_get_context(layout), false);

	PangoAttrList *attrs;
	if (markup) {
		char *buf;
		GError *error = NULL;
		if (pango_parse_markup(text, -1, 0, &attrs, &buf, NULL,
			    &error)) {
			pango_layout_set_text(layout, buf, -1);
			free(buf);
		} else {
			wlr_log(WLR_ERROR,
				"pango_parse_markup '%s' -> error %s", text,
				error->message);
			g_error_free(error);
			markup = false; /* fallback to plain text */
		}
	}
	if (!markup) {
		attrs = pango_attr_list_new();
		pango_layout_set_text(layout, text, -1);
	}

	pango_attr_list_insert(attrs, pango_attr_scale_new(scale));
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_single_paragraph_mode(layout, 1);
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
	return layout;
}

void
render_text(cairo_t *cairo, const PangoFontDescription *desc, double scale,
	bool markup, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}

	PangoLayout *layout = get_pango_layout(cairo, desc, buf, scale, markup);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_get_font_options(cairo, fo);
	pango_cairo_context_set_font_options(pango_layout_get_context(layout),
		fo);
	cairo_font_options_destroy(fo);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);

	g_free(buf);
}

void
cairo_set_source_u32(cairo_t *cairo, uint32_t color)
{
	cairo_set_source_rgba(cairo, (color >> (3 * 8) & 0xFF) / 255.0,
		(color >> (2 * 8) & 0xFF) / 255.0,
		(color >> (1 * 8) & 0xFF) / 255.0,
		(color >> (0 * 8) & 0xFF) / 255.0);
}
