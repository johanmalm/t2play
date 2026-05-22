// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "panel.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define THUMBNAIL_WIDTH 200

/* ========================================================================= */
/* ext_foreign_toplevel_list: track ext handles alongside wlr handles         */
/* ========================================================================= */

static void
ext_handle_title(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle,
	const char *title)
{
	struct ext_toplevel *t = data;
	xstrdup_replace(t->title, title);
}

static void
ext_handle_app_id(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle,
	const char *app_id)
{
	struct ext_toplevel *t = data;
	xstrdup_replace(t->app_id, app_id);
}

static void
ext_handle_identifier(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle,
	const char *identifier)
{
	/* nop: identifier not used for matching */
}

static void
ext_handle_done(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle)
{
	/* nop */
}

static void
ext_handle_closed(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct ext_toplevel *t = data;
	wl_list_remove(&t->link);
	ext_foreign_toplevel_handle_v1_destroy(t->handle);
	zfree(t->title);
	zfree(t->app_id);
	zfree(t);
}

static const struct ext_foreign_toplevel_handle_v1_listener ext_handle_listener = {
	.title = ext_handle_title,
	.app_id = ext_handle_app_id,
	.identifier = ext_handle_identifier,
	.done = ext_handle_done,
	.closed = ext_handle_closed,
};

static void
ext_list_toplevel(void *data,
	struct ext_foreign_toplevel_list_v1 *list,
	struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct panel *panel = data;
	struct ext_toplevel *t = znew(*t);
	t->handle = handle;
	wl_list_insert(&panel->ext_toplevels, &t->link);
	ext_foreign_toplevel_handle_v1_add_listener(handle, &ext_handle_listener, t);
}

static void
ext_list_finished(void *data,
	struct ext_foreign_toplevel_list_v1 *list)
{
	struct panel *panel = data;
	ext_foreign_toplevel_list_v1_destroy(panel->ext_toplevel_list);
	panel->ext_toplevel_list = NULL;
}

static const struct ext_foreign_toplevel_list_v1_listener ext_list_listener = {
	.toplevel = ext_list_toplevel,
	.finished = ext_list_finished,
};

void
thumbnail_init(struct panel *panel)
{
	/* ext_toplevels list is initialized during panel setup */
}

void
thumbnail_bind_ext_toplevel_list(struct panel *panel)
{
	if (panel->ext_toplevel_list) {
		ext_foreign_toplevel_list_v1_add_listener(panel->ext_toplevel_list,
			&ext_list_listener, panel);
	}
}

/*
 * Find the ext_foreign_toplevel_handle_v1 that corresponds to the given
 * wlr toplevel by matching app_id+title, falling back to app_id only.
 */
static struct ext_foreign_toplevel_handle_v1 *
find_ext_handle(struct panel *panel, struct toplevel *toplevel)
{
	struct ext_toplevel *t;

	/* Exact match: app_id and title */
	wl_list_for_each(t, &panel->ext_toplevels, link) {
		if (toplevel->app_id && t->app_id
				&& strcmp(toplevel->app_id, t->app_id) == 0
				&& toplevel->title && t->title
				&& strcmp(toplevel->title, t->title) == 0) {
			return t->handle;
		}
	}
	/* Fallback: app_id only */
	wl_list_for_each(t, &panel->ext_toplevels, link) {
		if (toplevel->app_id && t->app_id
				&& strcmp(toplevel->app_id, t->app_id) == 0) {
			return t->handle;
		}
	}
	return NULL;
}

/* ========================================================================= */
/* Capture session listeners                                                   */
/* ========================================================================= */

static void
session_buffer_size(void *data,
	struct ext_image_copy_capture_session_v1 *session,
	uint32_t width, uint32_t height)
{
	struct thumbnail *thumb = data;
	thumb->capture_width = width;
	thumb->capture_height = height;
}

static void
session_shm_format(void *data,
	struct ext_image_copy_capture_session_v1 *session,
	uint32_t format)
{
	struct thumbnail *thumb = data;
	if (!thumb->has_shm_format && format == WL_SHM_FORMAT_ARGB8888) {
		thumb->has_shm_format = true;
	}
}

static void
session_dmabuf_device(void *data,
	struct ext_image_copy_capture_session_v1 *session,
	struct wl_array *device)
{
	/* nop */
}

static void
session_dmabuf_format(void *data,
	struct ext_image_copy_capture_session_v1 *session,
	uint32_t format, struct wl_array *modifiers)
{
	/* nop */
}

static void
session_done(void *data,
	struct ext_image_copy_capture_session_v1 *session)
{
	struct thumbnail *thumb = data;
	thumb->got_constraints = true;
}

static void
session_stopped(void *data,
	struct ext_image_copy_capture_session_v1 *session)
{
	struct thumbnail *thumb = data;
	thumb->frame_failed = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

/* ========================================================================= */
/* Capture frame listeners                                                     */
/* ========================================================================= */

static void
frame_transform(void *data,
	struct ext_image_copy_capture_frame_v1 *frame,
	uint32_t transform)
{
	/* nop */
}

static void
frame_damage(void *data,
	struct ext_image_copy_capture_frame_v1 *frame,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	/* nop */
}

static void
frame_presentation_time(void *data,
	struct ext_image_copy_capture_frame_v1 *frame,
	uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	/* nop */
}

static void
frame_ready(void *data,
	struct ext_image_copy_capture_frame_v1 *frame)
{
	struct thumbnail *thumb = data;
	thumb->frame_done = true;
}

static void
frame_failed(void *data,
	struct ext_image_copy_capture_frame_v1 *frame,
	uint32_t reason)
{
	struct thumbnail *thumb = data;
	thumb->frame_failed = true;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

/* ========================================================================= */
/* Popup surface listeners                                                     */
/* ========================================================================= */

static void
thumbnail_render_popup(struct thumbnail *thumb)
{
	if (!thumb->image || !thumb->popup_surface) {
		return;
	}
	struct panel *panel = thumb->panel;
	int width = thumb->image_width;
	int height = thumb->image_height;

	struct pool_buffer *buf = get_next_buffer(panel->shm,
		thumb->popup_buffers, width, height);
	if (!buf) {
		return;
	}

	cairo_t *cr = buf->cairo;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, thumb->image, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);

	wl_surface_set_buffer_scale(thumb->popup_surface, 1);
	wl_surface_attach(thumb->popup_surface, buf->buffer, 0, 0);
	wl_surface_damage(thumb->popup_surface, 0, 0, width, height);
	wl_surface_commit(thumb->popup_surface);
}

static void
thumbnail_xdg_surface_configure(void *data,
	struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct thumbnail *thumb = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	thumbnail_render_popup(thumb);
}

static const struct xdg_surface_listener thumbnail_xdg_surface_listener = {
	.configure = thumbnail_xdg_surface_configure,
};

static void
thumbnail_popup_configure(void *data, struct xdg_popup *popup,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	/* nop */
}

static void
thumbnail_popup_done(void *data, struct xdg_popup *popup)
{
	struct thumbnail *thumb = data;
	thumbnail_hide(thumb->panel);
	thumb->panel->hovered_toplevel = NULL;
}

static void
thumbnail_popup_repositioned(void *data, struct xdg_popup *popup,
	uint32_t token)
{
	/* nop */
}

static const struct xdg_popup_listener thumbnail_popup_listener = {
	.configure = thumbnail_popup_configure,
	.popup_done = thumbnail_popup_done,
	.repositioned = thumbnail_popup_repositioned,
};

/* ========================================================================= */
/* Capture and display                                                         */
/* ========================================================================= */

static bool
do_capture(struct thumbnail *thumb,
	struct ext_foreign_toplevel_handle_v1 *ext_handle)
{
	struct panel *panel = thumb->panel;

	if (!panel->ext_image_capture_source_mgr
			|| !panel->ext_image_copy_capture_mgr) {
		wlr_log(WLR_DEBUG, "thumbnail: capture managers not available");
		return false;
	}

	/* Create image capture source for this toplevel */
	struct ext_image_capture_source_v1 *source =
		ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
			panel->ext_image_capture_source_mgr, ext_handle);
	if (!source) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create capture source");
		return false;
	}

	/* Create capture session */
	thumb->session = ext_image_copy_capture_manager_v1_create_session(
		panel->ext_image_copy_capture_mgr, source, 0);
	ext_image_capture_source_v1_destroy(source);
	if (!thumb->session) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create capture session");
		return false;
	}
	ext_image_copy_capture_session_v1_add_listener(thumb->session,
		&session_listener, thumb);

	/* Wait for buffer constraints (buffer_size, shm_format, done) */
	thumb->got_constraints = false;
	thumb->has_shm_format = false;
	thumb->capture_width = 0;
	thumb->capture_height = 0;

	wl_display_roundtrip(panel->display);

	/* Note: we ignore has_shm_format here, which is what grim does */
	if (!thumb->got_constraints || !thumb->capture_width || !thumb->capture_height) {
		wlr_log(WLR_DEBUG, "thumbnail: missing buffer constraints");
		ext_image_copy_capture_session_v1_destroy(thumb->session);
		thumb->session = NULL;
		return false;
	}

	/* Allocate the capture buffer using the pool */
	struct pool_buffer *cap_buf = get_next_buffer(panel->shm,
		thumb->capture_buffers,
		thumb->capture_width, thumb->capture_height);
	if (!cap_buf) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to allocate capture buffer");
		ext_image_copy_capture_session_v1_destroy(thumb->session);
		thumb->session = NULL;
		return false;
	}

	/* Create frame, attach buffer, damage entire buffer, then capture */
	thumb->frame_done = false;
	thumb->frame_failed = false;
	thumb->frame = ext_image_copy_capture_session_v1_create_frame(
		thumb->session);
	if (!thumb->frame) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create capture frame");
		ext_image_copy_capture_session_v1_destroy(thumb->session);
		thumb->session = NULL;
		return false;
	}
	ext_image_copy_capture_frame_v1_add_listener(thumb->frame,
		&frame_listener, thumb);
	ext_image_copy_capture_frame_v1_attach_buffer(thumb->frame,
		cap_buf->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(thumb->frame, 0, 0,
		(int32_t)thumb->capture_width, (int32_t)thumb->capture_height);
	ext_image_copy_capture_frame_v1_capture(thumb->frame);

	/*
	 * Wait for the frame to be ready. Most compositors deliver the frame
	 * within one roundtrip, but allow up to MAX_CAPTURE_ROUNDTRIPS to
	 * handle compositors that need a full repaint cycle before the frame
	 * can be captured.
	 */
#define MAX_CAPTURE_ROUNDTRIPS 5
	for (int i = 0; i < MAX_CAPTURE_ROUNDTRIPS
			&& !thumb->frame_done && !thumb->frame_failed; i++) {
		wl_display_roundtrip(panel->display);
	}
#undef MAX_CAPTURE_ROUNDTRIPS

	ext_image_copy_capture_frame_v1_destroy(thumb->frame);
	thumb->frame = NULL;
	ext_image_copy_capture_session_v1_destroy(thumb->session);
	thumb->session = NULL;

	if (!thumb->frame_done) {
		wlr_log(WLR_DEBUG, "thumbnail: frame capture did not complete");
		return false;
	}

	/* Scale the captured image down to THUMBNAIL_WIDTH pixels wide */
	double scale = (double)THUMBNAIL_WIDTH / thumb->capture_width;
	thumb->image_width = THUMBNAIL_WIDTH;
	thumb->image_height = (int)(thumb->capture_height * scale);
	if (thumb->image_height < 1) {
		thumb->image_height = 1;
	}

	thumb->image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		thumb->image_width, thumb->image_height);
	cairo_t *cr = cairo_create(thumb->image);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, cap_buf->surface, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);

	/*
	 * Mark capture buffer as not-busy so it can be reused next time.
	 * We already copied the data into thumb->image above.
	 */
	cap_buf->busy = false;

	return true;
}

/* ========================================================================= */
/* Public API                                                                  */
/* ========================================================================= */

void
thumbnail_show(struct panel *panel, struct toplevel *toplevel)
{
	/* Hide any existing thumbnail first */
	thumbnail_hide(panel);

	if (!panel->xdg_wm_base) {
		return;
	}

	struct ext_foreign_toplevel_handle_v1 *ext_handle =
		find_ext_handle(panel, toplevel);
	if (!ext_handle) {
		wlr_log(WLR_DEBUG, "thumbnail: no ext handle for '%s'",
			toplevel->title ? toplevel->title : "(null)");
		return;
	}

	struct thumbnail *thumb = znew(*thumb);
	thumb->panel = panel;
	panel->thumbnail = thumb;

	if (!do_capture(thumb, ext_handle)) {
		zfree(panel->thumbnail);
		return;
	}

	/* Create the popup surface */
	thumb->popup_surface = wl_compositor_create_surface(panel->compositor);
	if (!thumb->popup_surface) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create popup surface");
		goto cleanup_image;
	}

	thumb->xdg_surface = xdg_wm_base_get_xdg_surface(panel->xdg_wm_base,
		thumb->popup_surface);
	if (!thumb->xdg_surface) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create xdg_surface");
		goto cleanup_surface;
	}
	xdg_surface_add_listener(thumb->xdg_surface,
		&thumbnail_xdg_surface_listener, thumb);

	struct xdg_positioner *positioner =
		xdg_wm_base_create_positioner(panel->xdg_wm_base);
	xdg_positioner_set_size(positioner, thumb->image_width,
		thumb->image_height);
	/*
	 * Anchor rect covers the task button's column in panel coordinates.
	 * BOTTOM_LEFT anchor + BOTTOM_RIGHT gravity opens the popup downward
	 * from the bottom of the panel (or upward for a bottom panel via
	 * FLIP_Y constraint adjustment), matching the startmenu behaviour.
	 */
	xdg_positioner_set_anchor_rect(positioner, toplevel->base.box.x, 0,
		toplevel->base.box.width, panel->box.height);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(positioner,
		XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);

	thumb->xdg_popup = xdg_surface_get_popup(thumb->xdg_surface,
		NULL, positioner);
	xdg_positioner_destroy(positioner);
	if (!thumb->xdg_popup) {
		wlr_log(WLR_DEBUG, "thumbnail: failed to create xdg_popup");
		goto cleanup_xdg_surface;
	}
	zwlr_layer_surface_v1_get_popup(panel->layer_surface, thumb->xdg_popup);
	xdg_popup_add_listener(thumb->xdg_popup, &thumbnail_popup_listener,
		thumb);

	wl_surface_commit(thumb->popup_surface);
	wl_display_roundtrip(panel->display);
	return;

cleanup_xdg_surface:
	xdg_surface_destroy(thumb->xdg_surface);
	thumb->xdg_surface = NULL;
cleanup_surface:
	wl_surface_destroy(thumb->popup_surface);
	thumb->popup_surface = NULL;
cleanup_image:
	if (thumb->image) {
		cairo_surface_destroy(thumb->image);
		thumb->image = NULL;
	}
	zfree(panel->thumbnail);
}

void
thumbnail_hide(struct panel *panel)
{
	struct thumbnail *thumb = panel->thumbnail;
	if (!thumb) {
		return;
	}

	/* Destroy popup */
	if (thumb->xdg_popup) {
		xdg_popup_destroy(thumb->xdg_popup);
		thumb->xdg_popup = NULL;
	}
	if (thumb->xdg_surface) {
		xdg_surface_destroy(thumb->xdg_surface);
		thumb->xdg_surface = NULL;
	}
	if (thumb->popup_surface) {
		wl_surface_destroy(thumb->popup_surface);
		thumb->popup_surface = NULL;
	}
	destroy_buffer(&thumb->popup_buffers[0]);
	destroy_buffer(&thumb->popup_buffers[1]);

	/* Destroy any in-progress capture resources */
	if (thumb->frame) {
		ext_image_copy_capture_frame_v1_destroy(thumb->frame);
		thumb->frame = NULL;
	}
	if (thumb->session) {
		ext_image_copy_capture_session_v1_destroy(thumb->session);
		thumb->session = NULL;
	}
	destroy_buffer(&thumb->capture_buffers[0]);
	destroy_buffer(&thumb->capture_buffers[1]);

	/* Free scaled image */
	if (thumb->image) {
		cairo_surface_destroy(thumb->image);
		thumb->image = NULL;
	}

	zfree(panel->thumbnail);
}

void
thumbnail_destroy_all(struct panel *panel)
{
	thumbnail_hide(panel);

	/* Destroy ext toplevel list and all tracked handles */
	struct ext_toplevel *t, *next;
	wl_list_for_each_safe(t, next, &panel->ext_toplevels, link) {
		wl_list_remove(&t->link);
		ext_foreign_toplevel_handle_v1_destroy(t->handle);
		zfree(t->title);
		zfree(t->app_id);
		zfree(t);
	}
	if (panel->ext_toplevel_list) {
		ext_foreign_toplevel_list_v1_destroy(panel->ext_toplevel_list);
		panel->ext_toplevel_list = NULL;
	}

	/* Destroy capture managers */
	if (panel->ext_image_capture_source_mgr) {
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
			panel->ext_image_capture_source_mgr);
		panel->ext_image_capture_source_mgr = NULL;
	}
	if (panel->ext_image_copy_capture_mgr) {
		ext_image_copy_capture_manager_v1_destroy(
			panel->ext_image_copy_capture_mgr);
		panel->ext_image_copy_capture_mgr = NULL;
	}
}
