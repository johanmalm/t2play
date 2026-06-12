// SPDX-License-Identifier: GPL-2.0-only
#include "panel.h"
#include "common/log.h"
#include "common/string-helpers.h"
#include "pango/pango-layout.h"

static PangoContext *text_measure_context;
static PangoLayout *text_measure_layout;

static void
text_measure_init(void)
{
	if (!text_measure_context) {
		PangoFontMap *fontmap = pango_cairo_font_map_get_default();

		text_measure_context = pango_font_map_create_context(fontmap);
		pango_context_set_round_glyph_positions(
			text_measure_context, false);
		text_measure_layout = pango_layout_new(text_measure_context);
	}
}

void
text_measure_fini(void)
{
	if (text_measure_layout) {
		g_object_unref(text_measure_layout);
		text_measure_layout = NULL;
	}
	if (text_measure_context) {
		g_object_unref(text_measure_context);
		text_measure_context = NULL;
	}
}

PangoRectangle
get_text_size(const PangoFontDescription *desc, const char *string)
{
	PangoRectangle rect = {0};
	if (string_null_or_empty(string)) {
		return rect;
	}

	text_measure_init();

	pango_layout_set_font_description(text_measure_layout, desc);
	pango_layout_set_text(text_measure_layout, string, -1);
	pango_layout_set_single_paragraph_mode(text_measure_layout, TRUE);
	pango_layout_set_width(text_measure_layout, -1);
	pango_layout_set_ellipsize(text_measure_layout, PANGO_ELLIPSIZE_MIDDLE);
	pango_layout_get_extents(text_measure_layout, NULL, &rect);
	pango_extents_to_pixels(&rect, NULL);

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
			warn("pango_parse_markup '%s' -> error %s",
				text, error->message);
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
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
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
	pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
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

/* 1 degree in radians (=2π/360) */
static const double deg = 0.017453292519943295;

void
rounded_rect(cairo_t *cairo, double w, double h, double r)
{
	/* set transparent background */
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);

	/* Create outline path and fill */
	cairo_set_line_width(cairo, 0.0);
	cairo_new_sub_path(cairo);

	// TL
	cairo_arc(cairo, r, r, r, 180 * deg, 270 * deg);
	cairo_line_to(cairo, w - r, 0);

	// TR
	cairo_arc(cairo, w - r, r, r, -90 * deg, 0);
	cairo_line_to(cairo, w, h - r);

	// BR
	cairo_arc(cairo, w - r, h - r, r, 0, 90 * deg);
	cairo_line_to(cairo, r, h);

	// BL
	cairo_arc(cairo, r, h - r, r, 90 * deg, 180 * deg);
	cairo_line_to(cairo, 0, r);

	cairo_close_path(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
}

