/*
 * The cursor model of flutter-elinux's ELinuxWindowWayland, cut down to
 * what matters: one wl_surface for the cursor, shared by the wl_pointer of
 * every seat; set_cursor goes to whichever pointer entered last; the hotspot
 * changes with the cursor shape (left half "basic" 1,1 / right half "text"
 * 4,8) while the surface stays the same.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface, *cursor_surface;
static struct wl_buffer *cursor_buf[2];
static int width = 1280, height = 800;

static struct wl_pointer *cur_pointer;
static uint32_t cur_serial;
static int cur_kind = -1;

static struct wl_buffer *
make_buffer(int w, int h, uint32_t color)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("buf", 0);
	ftruncate(fd, size);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++)
		p[i] = color;
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
						       WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void
update_cursor(int kind)
{
	static const int hx[2] = { 1, 4 }, hy[2] = { 1, 8 };

	if (kind == cur_kind || !cur_pointer)
		return;
	cur_kind = kind;
	fprintf(stderr, "set_cursor pointer=%p serial=%u kind=%d hotspot=%d,%d\n",
		(void *)cur_pointer, cur_serial, kind, hx[kind], hy[kind]);
	wl_pointer_set_cursor(cur_pointer, cur_serial, cursor_surface,
			      hx[kind], hy[kind]);
	wl_surface_attach(cursor_surface, cursor_buf[kind], 0, 0);
	wl_surface_damage(cursor_surface, 0, 0, 16, 16);
	wl_surface_commit(cursor_surface);
}

static void
p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
	wl_fixed_t x, wl_fixed_t y)
{
	fprintf(stderr, "enter pointer=%p serial=%u at %.0f,%.0f\n", (void *)p,
		serial, wl_fixed_to_double(x), wl_fixed_to_double(y));
	cur_pointer = p;
	cur_serial = serial;
	cur_kind = -1;	/* "force redraw even though it is the same cursor" */
	update_cursor(wl_fixed_to_double(x) < width / 2 ? 0 : 1);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *f) {}
static void
p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	update_cursor(wl_fixed_to_double(x) < width / 2 ? 0 : 1);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	p_enter, p_leave, p_motion, p_button, p_axis, p_frame,
	p_axis_source, p_axis_stop, p_axis_discrete,
};

static void
seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	struct wl_pointer **pp = d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !*pp) {
		*pp = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(*pp, &pointer_listener, NULL);
		fprintf(stderr, "seat %p: got pointer %p\n", (void *)seat, (void *)*pp);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && *pp) {
		fprintf(stderr, "seat %p: pointer %p gone\n", (void *)seat, (void *)*pp);
		if (cur_pointer == *pp)
			cur_pointer = NULL;
		wl_pointer_release(*pp);
		*pp = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void
wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface,
	   uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		struct wl_seat *seat = wl_registry_bind(r, name, &wl_seat_interface,
							ver < 5 ? ver : 5);
		wl_seat_add_listener(seat, &seat_listener,
				     calloc(1, sizeof(struct wl_pointer *)));
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static int configured;
static void
xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void
tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
	     struct wl_array *s)
{
	if (w > 0 && h > 0) {
		width = w;
		height = h;
	}
}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close };

int
main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		perror("connect");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);

	surface = wl_compositor_create_surface(compositor);
	cursor_surface = wl_compositor_create_surface(compositor);
	cursor_buf[0] = make_buffer(9, 16, 0xffffffff);
	cursor_buf[1] = make_buffer(9, 16, 0xff00ff00);

	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_title(tl, "cursor-share");
	xdg_toplevel_set_fullscreen(tl, NULL);
	wl_surface_commit(surface);
	while (!configured)
		wl_display_dispatch(dpy);
	wl_surface_attach(surface, make_buffer(width, height, 0xff202040), 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	fprintf(stderr, "mapped %dx%d\n", width, height);

	while (wl_display_dispatch(dpy) != -1)
		;
	fprintf(stderr, "display gone\n");
	return 0;
}
