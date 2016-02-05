#include "sysdeps.h"
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "gstmfxwindow_wayland.h"
#include "gstmfxwindow_priv.h"
#include "gstmfxdisplay_wayland.h"
#include "gstmfxdisplay_wayland_priv.h"

#define DEBUG 1
#include "gstmfxdebug.h"

#define GST_MFX_WINDOW_WAYLAND_CAST(obj) \
	((GstMfxWindowWayland *)(obj))

#define GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(obj) \
	(&GST_MFX_WINDOW_WAYLAND_CAST(obj)->priv)

typedef struct _GstMfxWindowWaylandPrivate GstMfxWindowWaylandPrivate;
typedef struct _GstMfxWindowWaylandClass GstMfxWindowWaylandClass;


struct _GstMfxWindowWaylandPrivate
{
	struct wl_shell_surface *shell_surface;
	struct wl_surface *surface;
	struct wl_region *opaque_region;
	struct wl_event_queue *event_queue;
	struct wl_egl_window *egl_window;
	GstPoll *poll;
	GstPollFD pollfd;
	guint is_shown : 1;
	guint fullscreen_on_show : 1;
	guint sync_failed : 1;
	volatile guint num_frames_pending;
};

/**
* GstMfxWindowWayland:
*
* A Wayland window abstraction.
*/
struct _GstMfxWindowWayland
{
	/*< private >*/
	GstMfxWindow parent_instance;

	GstMfxWindowWaylandPrivate priv;
};

/**
* GstMfxWindowWaylandClass:
*
* An Wayland #Window wrapper class.
*/
struct _GstMfxWindowWaylandClass
{
	/*< private >*/
	GstMfxWindowClass parent_class;
};

static gboolean
gst_mfx_window_wayland_show(GstMfxWindow * window)
{
	GST_WARNING("unimplemented GstMfxWindowWayland::show()");

	return TRUE;
}

static gboolean
gst_mfx_window_wayland_hide(GstMfxWindow * window)
{
	GST_WARNING("unimplemented GstMfxWindowWayland::hide()");

	return TRUE;
}

static gboolean
gst_mfx_window_wayland_sync(GstMfxWindow * window)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);
	struct wl_display *const wl_display =
		GST_MFX_OBJECT_NATIVE_DISPLAY(window);

	if (priv->sync_failed)
		return FALSE;

	if (priv->pollfd.fd < 0) {
		priv->pollfd.fd = wl_display_get_fd(wl_display);
		gst_poll_add_fd(priv->poll, &priv->pollfd);
		gst_poll_fd_ctl_read(priv->poll, &priv->pollfd, TRUE);
	}

	while (g_atomic_int_get(&priv->num_frames_pending) > 0) {
		while (wl_display_prepare_read_queue(wl_display, priv->event_queue) < 0) {
			if (wl_display_dispatch_queue_pending(wl_display, priv->event_queue) < 0)
				goto error;
		}

		if (wl_display_flush(wl_display) < 0)
			goto error;

	again:
		if (gst_poll_wait(priv->poll, GST_CLOCK_TIME_NONE) < 0) {
			int saved_errno = errno;
			if (saved_errno == EAGAIN || saved_errno == EINTR)
				goto again;
			if (saved_errno == EBUSY) {       /* closing */
				wl_display_cancel_read(wl_display);
				return FALSE;
			}
			goto error;
		}

		if (wl_display_read_events(wl_display) < 0)
			goto error;
		if (wl_display_dispatch_queue_pending(wl_display, priv->event_queue) < 0)
			goto error;
	}
	return TRUE;

error:
	priv->sync_failed = TRUE;
	GST_ERROR("Error on dispatching events: %s", g_strerror(errno));
	return FALSE;
}

static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
	uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
	uint32_t edges, int32_t width, int32_t height)
{
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	handle_ping,
	handle_configure,
	handle_popup_done
};

static gboolean
gst_mfx_window_wayland_set_fullscreen(GstMfxWindow * window,
gboolean fullscreen)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);

	if (!priv->is_shown) {
		priv->fullscreen_on_show = fullscreen;
		return TRUE;
	}

	if (!fullscreen)
		wl_shell_surface_set_toplevel(priv->shell_surface);
	else {
		wl_shell_surface_set_fullscreen(priv->shell_surface,
			WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
	}

	return TRUE;
}

static gboolean
gst_mfx_window_wayland_create(GstMfxWindow * window,
	guint * width, guint * height)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);
	GstMfxDisplayWaylandPrivate *const priv_display =
		GST_MFX_DISPLAY_WAYLAND_GET_PRIVATE(GST_MFX_OBJECT_DISPLAY(window));

	GST_DEBUG("create window, size %ux%u", *width, *height);

	g_return_val_if_fail(priv_display->compositor != NULL, FALSE);
	g_return_val_if_fail(priv_display->shell != NULL, FALSE);

	GST_MFX_OBJECT_LOCK_DISPLAY(window);
	priv->event_queue = wl_display_create_queue(priv_display->wl_display);
	GST_MFX_OBJECT_UNLOCK_DISPLAY(window);
	if (!priv->event_queue)
		return FALSE;

	GST_MFX_OBJECT_LOCK_DISPLAY(window);
	priv->surface = wl_compositor_create_surface(priv_display->compositor);
	GST_MFX_OBJECT_UNLOCK_DISPLAY(window);
	if (!priv->surface)
		return FALSE;
	wl_proxy_set_queue((struct wl_proxy *) priv->surface, priv->event_queue);

	GST_MFX_OBJECT_LOCK_DISPLAY(window);
	priv->shell_surface =
		wl_shell_get_shell_surface(priv_display->shell, priv->surface);
	GST_MFX_OBJECT_UNLOCK_DISPLAY(window);
	if (!priv->shell_surface)
		return FALSE;
	wl_proxy_set_queue((struct wl_proxy *) priv->shell_surface,
		priv->event_queue);

	wl_shell_surface_add_listener(priv->shell_surface,
		&shell_surface_listener, priv);
	wl_shell_surface_set_toplevel(priv->shell_surface);

	priv->poll = gst_poll_new(TRUE);
	gst_poll_fd_init(&priv->pollfd);

	if (priv->fullscreen_on_show)
		gst_mfx_window_wayland_set_fullscreen(window, TRUE);

    priv->egl_window = wl_egl_window_create(priv->surface, *width, *height);
    GST_MFX_OBJECT_ID(window) = priv->egl_window;

	priv->is_shown = TRUE;

	return TRUE;
}

static void
gst_mfx_window_wayland_destroy(GstMfxWindow * window)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);

	/* Wait for the last frame to complete redraw */
	gst_mfx_window_wayland_sync(window);

	if (priv->shell_surface) {
		wl_shell_surface_destroy(priv->shell_surface);
		priv->shell_surface = NULL;
	}

	if (priv->surface) {
		wl_surface_destroy(priv->surface);
		priv->surface = NULL;
	}

	if (priv->event_queue) {
		wl_event_queue_destroy(priv->event_queue);
		priv->event_queue = NULL;
	}

	if (priv->egl_window) {
		wl_egl_window_destroy(priv->egl_window);
		priv->egl_window = NULL;
	}

	gst_poll_free(priv->poll);
}

static gboolean
gst_mfx_window_wayland_resize(GstMfxWindow * window,
	guint width, guint height)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);
	GstMfxDisplayWaylandPrivate *const priv_display =
		GST_MFX_DISPLAY_WAYLAND_GET_PRIVATE(GST_MFX_OBJECT_DISPLAY(window));

	GST_DEBUG("resize window, new size %ux%u", width, height);

	if (priv->opaque_region)
		wl_region_destroy(priv->opaque_region);
	GST_MFX_OBJECT_LOCK_DISPLAY(window);
	priv->opaque_region = wl_compositor_create_region(priv_display->compositor);
	GST_MFX_OBJECT_UNLOCK_DISPLAY(window);
	wl_region_add(priv->opaque_region, 0, 0, width, height);

	return TRUE;
}

static gboolean
gst_mfx_window_wayland_unblock(GstMfxWindow * window)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);

	gst_poll_set_flushing(priv->poll, TRUE);

	return TRUE;
}

static gboolean
gst_mfx_window_wayland_unblock_cancel(GstMfxWindow * window)
{
	GstMfxWindowWaylandPrivate *const priv =
		GST_MFX_WINDOW_WAYLAND_GET_PRIVATE(window);

	gst_poll_set_flushing(priv->poll, FALSE);

	return TRUE;
}

static void
gst_mfx_window_wayland_class_init(GstMfxWindowWaylandClass * klass)
{
	GstMfxObjectClass *const object_class = GST_MFX_OBJECT_CLASS(klass);
	GstMfxWindowClass *const window_class = GST_MFX_WINDOW_CLASS(klass);

	object_class->finalize = (GstMfxObjectFinalizeFunc)
		gst_mfx_window_wayland_destroy;

	window_class->create = gst_mfx_window_wayland_create;
	window_class->show = gst_mfx_window_wayland_show;
	window_class->hide = gst_mfx_window_wayland_hide;
	window_class->resize = gst_mfx_window_wayland_resize;
	window_class->set_fullscreen = gst_mfx_window_wayland_set_fullscreen;
	window_class->unblock = gst_mfx_window_wayland_unblock;
	window_class->unblock_cancel = gst_mfx_window_wayland_unblock_cancel;
}

#define gst_mfx_window_wayland_finalize \
	gst_mfx_window_wayland_destroy

GST_MFX_OBJECT_DEFINE_CLASS_WITH_CODE(GstMfxWindowWayland,
	gst_mfx_window_wayland, gst_mfx_window_wayland_class_init(&g_class));

/**
* gst_mfx_window_wayland_new:
* @display: a #GstMfxDisplay
* @width: the requested window width, in pixels
* @height: the requested windo height, in pixels
*
* Creates a window with the specified @width and @height. The window
* will be attached to the @display and remains invisible to the user
* until gst_mfx_window_show() is called.
*
* Return value: the newly allocated #GstMfxWindow object
*/
GstMfxWindow *
gst_mfx_window_wayland_new(GstMfxDisplay * display,
    guint width, guint height)
{
	GST_DEBUG("new window, size %ux%u", width, height);

	g_return_val_if_fail(GST_MFX_IS_DISPLAY_WAYLAND(display), NULL);

	return gst_mfx_window_new_internal(GST_MFX_WINDOW_CLASS
		(gst_mfx_window_wayland_class()), display, width, height);
}
