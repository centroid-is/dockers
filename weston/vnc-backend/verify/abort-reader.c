/*
 * A clipboard reader that walks away in the middle of the transfer.
 *
 * wl-paste cannot be used for this: it drains the whole offer into memory as
 * fast as it arrives, so killing it never leaves the compositor holding a
 * half-written pipe. This client instead reads continuously — keeping the
 * compositor's write loop spinning — and then closes its end of the pipe
 * abruptly, which is the case that raises SIGPIPE on the next write().
 *
 * Needs keyboard focus to be offered the selection at all, so it maps a real
 * 1x1 xdg_toplevel and waits to be configured.
 *
 *   abort-reader [bytes-before-closing] [mime-type]
 *
 * Exits 0 once it has closed the pipe. Whether the compositor survives that
 * is the point of the test, and is checked by the caller.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct wl_data_offer *offer;
static bool configured;

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, "wl_compositor"))
		compositor = wl_registry_bind(registry, name,
					      &wl_compositor_interface, 4);
	else if (!strcmp(interface, "xdg_wm_base"))
		wm_base = wl_registry_bind(registry, name,
					   &xdg_wm_base_interface, 1);
	else if (!strcmp(interface, "wl_shm"))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, "wl_seat") && !seat)
		/* the first seat announced, the way the HMI's embedder does */
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 2);
	else if (!strcmp(interface, "wl_data_device_manager"))
		ddm = wl_registry_bind(registry, name,
				       &wl_data_device_manager_interface, 3);
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_global_remove,
};

static void
wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void
xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	xdg_surface_ack_configure(surface, serial);
	configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
		   int32_t height, struct wl_array *states)
{
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure, toplevel_close,
};

static void
data_offer_offer(void *data, struct wl_data_offer *o, const char *mime_type)
{
}

static void
data_offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions)
{
}

static void
data_offer_action(void *data, struct wl_data_offer *o, uint32_t action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
	data_offer_offer, data_offer_source_actions, data_offer_action,
};

static void
dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	wl_data_offer_add_listener(o, &data_offer_listener, NULL);
}

static void
dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	if (offer)
		wl_data_offer_destroy(offer);
	offer = o;
}

static void
dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
	 struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
	 struct wl_data_offer *o)
{
}

static void
dd_leave(void *data, struct wl_data_device *dd)
{
}

static void
dd_motion(void *data, struct wl_data_device *dd, uint32_t time, wl_fixed_t x,
	  wl_fixed_t y)
{
}

static void
dd_drop(void *data, struct wl_data_device *dd)
{
}

static const struct wl_data_device_listener dd_listener = {
	dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection,
};

static struct wl_buffer *
make_buffer(void)
{
	const int width = 1, height = 1, stride = 4, size = 4;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	void *map;
	int fd;

	fd = memfd_create("abort-reader", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return NULL;

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		return NULL;
	memset(map, 0x40, size);

	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

int
main(int argc, char *argv[])
{
	long abort_after = argc > 1 ? atol(argv[1]) : 1048576;
	const char *mime = argc > 2 ? argv[2] : "text/plain;charset=utf-8";
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_data_device *ddev;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	long total = 0;
	int pipe_fd[2];
	int spins;

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "abort-reader: no display\n");
		return 1;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !wm_base || !shm || !seat || !ddm) {
		fprintf(stderr, "abort-reader: missing globals\n");
		return 1;
	}

	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	ddev = wl_data_device_manager_get_data_device(ddm, seat);
	wl_data_device_add_listener(ddev, &dd_listener, NULL);

	/* A mapped, focused window: without focus the selection is never
	 * offered to us at all. */
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "abort-reader");
	wl_surface_commit(surface);

	for (spins = 0; spins < 200 && !configured; spins++)
		if (wl_display_roundtrip(display) < 0)
			return 1;
	if (!configured) {
		fprintf(stderr, "abort-reader: never configured\n");
		return 1;
	}

	buffer = make_buffer();
	if (!buffer) {
		fprintf(stderr, "abort-reader: no buffer\n");
		return 1;
	}
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, 1, 1);
	wl_surface_commit(surface);

	for (spins = 0; spins < 200 && !offer; spins++) {
		struct timespec ts = { 0, 20 * 1000 * 1000 };

		if (wl_display_roundtrip(display) < 0)
			return 1;
		if (!offer)
			nanosleep(&ts, NULL);
	}
	if (!offer) {
		fprintf(stderr, "abort-reader: no selection offered\n");
		return 1;
	}

	if (pipe2(pipe_fd, O_CLOEXEC) < 0) {
		perror("abort-reader: pipe2");
		return 1;
	}

	wl_data_offer_receive(offer, mime, pipe_fd[1]);
	close(pipe_fd[1]);
	wl_display_flush(display);

	/* Read continuously, so the compositor stays inside its write loop,
	 * and then drop the pipe on the floor part-way through. */
	while (total < abort_after) {
		char buf[4096];
		ssize_t len = read(pipe_fd[0], buf, sizeof buf);

		if (len < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (len == 0)
			break;
		total += len;
	}

	printf("abort-reader: read %ld bytes, closing mid-transfer\n", total);
	fflush(stdout);
	close(pipe_fd[0]);

	/* Give the compositor an event-loop pass or two to try writing into
	 * the pipe we just closed. */
	{
		struct timespec ts = { 0, 300 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}

	return 0;
}
