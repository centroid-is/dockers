/*
 * A clipboard reader that holds a wl_data_device on *every* seat.
 *
 * This backend publishes a client's cut text as the selection on all of the
 * compositor's seats, not just the VNC peer's, because the HMI's embedder
 * binds its data device to whatever seat existed when it started. The obvious
 * worry about that design is a client with more than one data device being
 * offered the same text once per seat — and, if it reads them all into one
 * pipe the way a naive implementation would, ending up with the payload
 * repeated.
 *
 * So: bind every seat, take an offer from each, read them all into a single
 * pipe, and report both the per-seat count and the total. A total that is a
 * multiple of the payload is the doubling reproduced.
 *
 *   multiseat-reader [mime-type]
 *
 * Prints "seats=N offers=M total=B" and exits 0.
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

#define MAX_SEATS 8

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wl_data_device_manager *ddm;
static bool configured;

static struct wl_seat *seats[MAX_SEATS];
static char *seat_names[MAX_SEATS];
static int n_seats;

struct device_state {
	struct wl_data_device *device;
	struct wl_data_offer *offer;
	int index;
};

static struct device_state devices[MAX_SEATS];

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	int i = (int)(intptr_t)data;

	free(seat_names[i]);
	seat_names[i] = strdup(name);
}

static const struct wl_seat_listener seat_listener = {
	seat_capabilities, seat_name,
};

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
	else if (!strcmp(interface, "wl_data_device_manager"))
		ddm = wl_registry_bind(registry, name,
				       &wl_data_device_manager_interface, 3);
	else if (!strcmp(interface, "wl_seat") && n_seats < MAX_SEATS) {
		int i = n_seats++;

		seats[i] = wl_registry_bind(registry, name, &wl_seat_interface, 2);
		wl_seat_add_listener(seats[i], &seat_listener,
				     (void *)(intptr_t)i);
	}
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
	struct device_state *state = data;

	if (state->offer)
		wl_data_offer_destroy(state->offer);
	state->offer = o;
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
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	void *map;
	int fd;

	fd = memfd_create("multiseat-reader", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, 4) < 0)
		return NULL;
	map = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		return NULL;
	memset(map, 0x40, 4);
	pool = wl_shm_create_pool(shm, fd, 4);
	buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
					   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

int
main(int argc, char *argv[])
{
	const char *mime = argc > 1 ? argv[1] : "text/plain;charset=utf-8";
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	long total = 0;
	int pipe_fd[2];
	int spins, i, offers = 0;

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "multiseat-reader: no display\n");
		return 1;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);          /* let the seat names land */

	if (!compositor || !wm_base || !shm || !ddm || n_seats == 0) {
		fprintf(stderr, "multiseat-reader: missing globals\n");
		return 1;
	}

	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	for (i = 0; i < n_seats; i++) {
		devices[i].index = i;
		devices[i].device =
			wl_data_device_manager_get_data_device(ddm, seats[i]);
		wl_data_device_add_listener(devices[i].device, &dd_listener,
					    &devices[i]);
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "multiseat-reader");
	wl_surface_commit(surface);

	for (spins = 0; spins < 200 && !configured; spins++)
		if (wl_display_roundtrip(display) < 0)
			return 1;

	buffer = make_buffer();
	if (!buffer)
		return 1;
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, 1, 1);
	wl_surface_commit(surface);

	for (spins = 0; spins < 50; spins++) {
		struct timespec ts = { 0, 20 * 1000 * 1000 };

		if (wl_display_roundtrip(display) < 0)
			return 1;
		nanosleep(&ts, NULL);
	}

	if (pipe2(pipe_fd, O_CLOEXEC) < 0) {
		perror("multiseat-reader: pipe2");
		return 1;
	}

	/* Every offer we hold, into the one pipe. */
	for (i = 0; i < n_seats; i++) {
		if (!devices[i].offer)
			continue;
		offers++;
		wl_data_offer_receive(devices[i].offer, mime, pipe_fd[1]);
	}
	close(pipe_fd[1]);
	wl_display_flush(display);
	wl_display_roundtrip(display);

	for (;;) {
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
	close(pipe_fd[0]);

	printf("seats=%d offers=%d total=%ld\n", n_seats, offers, total);
	for (i = 0; i < n_seats; i++)
		printf("  seat[%d] %s offer=%s\n", i,
		       seat_names[i] ? seat_names[i] : "?",
		       devices[i].offer ? "yes" : "no");
	return 0;
}
