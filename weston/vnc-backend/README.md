# Patched `vnc-backend.so`

Stock weston 16 aborts the whole compositor the first time anything takes a
screenshot while the VNC output is up:

```
weston: ../libweston/output-capture.c:206: weston_output_capture_info_repaint_done:
  Assertion `wl_list_empty(&ci->pending_capture_list)' failed.
```

With `restart: unless-stopped` this reads as a flaky screen rather than a
crash. On the hq rig weston died 6 times this way and took the `flutter`
container down 23 times with it.

## Why it happens

`weston-screenshooter` has no way to pick an output — read
`clients/screenshot.c`, there is no such flag — so it files a capture task
against *every* output. That is also why it returns one 3840x1080 PNG of
`eDP-1` and the `vnc` output side by side.

Framebuffer capture tasks are drained by the **renderer**, not by the backend:
`gl_renderer_repaint_output()` calls `gl_renderer_do_capture_tasks()` on its
way out, and `gl_renderer_resize_output()` is what advertises the source to
clients in the first place. That advertisement is per-renderer, so the VNC
output offers capture whether or not anything can service it — there is no
config key that turns it off.

`vnc_output_repaint()` only reaches the renderer when the primary plane has
damage:

```c
if (pixman_region32_not_empty(&damage))
        vnc_update_buffer(output->display, &damage);
```

Filing a capture calls `weston_output_schedule_repaint()` but damages nothing,
so that repaint runs with an empty region, never invokes the renderer, and
reaches `weston_output_capture_info_repaint_done()` with the task still on
`pending_capture_list`. The assert is deliberate; its own comment says the
remaining tasks "would not be serviced by anything, so make sure none linger."

There is a second hole in the same function. When the last VNC client leaves,
the output powers itself off, and `weston_output_check_repaint()` drops
repaints on a powered-off output *before* it looks at capture state. A task
filed after that point is stranded with no repaint coming at all — the
screenshot blocks rather than crashing. Measured at 3m51s and ~5min before it
eventually came back.

## The patch

`backend-drm` and `backend-pipewire` both already guard against exactly this
(`grep -n weston_output_has_renderer_capture_tasks libweston/`); `backend-vnc`
is the one that does not. So the fix is theirs, applied to `vnc.c`:

* invoke the renderer when a renderer capture task is outstanding, not only
  when there is damage, and
* do not power the output off while one is outstanding.

Rendering with an empty damage region still produces a correct frame: the GL
renderer repaints from the renderbuffer's own accumulated damage
(`repaint_views(output, &rb->damage)`), and a renderbuffer freshly created for
an `nvnc` pool frame starts fully damaged.

The pipewire version also shoots down tasks it could not service. That branch
is unreachable here — whenever there is a task we do render — and
`weston_output_pull_capture_task()` asserts the size it is handed matches the
advertised capture size, which `current_mode` need not, so it is left out.

Still present in weston `main` as of this writing (`vnc.c` there is identical
to the 16.0.0 tag), so it is worth sending upstream.

## What was tested, and what was not

None of this ran on a station. It was exercised in a container on an arm64
Mac, with the VNC backend as the only backend (no DRM device present) and
`renderer=pixman`, driving the image this Dockerfile builds. **The station
runs `renderer=gl` and a `drm,vnc` pair, and neither of those was tested.**

The client is `neatvnc/verify/ra2_all.py` with a `time.sleep()` appended, so it
completes the RA2ne handshake and then holds the session open instead of
exiting. weston runs with `--debug`, because
otherwise screen capture is only authorized for the compositor's own
screenshooter client (`frontend/main.c`, `screenshot_allow_all`).

| scenario | stock | patched |
|---|---|---|
| VNC client attached, screen idle, `weston-screenshooter` | **abort** — `output-capture.c:206` assertion, weston exit 134 | screenshot in 0s, weston alive |
| same, PNG contents | no PNG | desktop background and cursor, matching a known-good capture |
| VNC client attached, `weston-flower` animating | screenshot in 0s | screenshot in 0s, flower and cursor present |
| **no** VNC client attached | blocks until the 30s timeout, no PNG | **blocks until the 30s timeout, no PNG** |

So the crash is fixed and the normal path is unchanged, but the last row is
not fixed. That hole is not reachable from the backend: the output has
already powered itself off by the time the task is filed, and
`weston_output_check_repaint()` drops the repaint on a powered-off output
*before* it looks at capture state, so `vnc_output_repaint()` never runs again
to notice. Closing it properly means a change in libweston, which would mean
rebuilding weston rather than swapping one module.

### A second shape that was tried and rejected

Replacing `weston_output_power_off()` with `repaint_only_on_capture = true`
(and clearing it in `vnc_new_client()`) does get past
`weston_output_check_repaint()`, and it does make the no-client screenshot
return in 0s instead of blocking. But the PNG it returns is **solid black** —
against a desktop that a known-good capture of the same compositor shows as
dark navy with a cursor. Trading a visible hang for a silently wrong image is
worse than the hang, so it is not what is shipped here. It was not root-caused
further.

# `0002` — null-checking the cursor state

A second, independent patch to the same file. It began as hardening. It is now
a **demonstrated fix for a reproduced SIGSEGV** — see "Reproduced" below.

It is **not** sufficient on its own: with it applied, the same load runs far
longer and then aborts on a *different* bug, in libweston rather than the
backend. Both are documented below.

## The asymmetry

`vnc_output_update_cursor()` dereferences two pointers that its sibling
`vnc_output_assign_cursor_plane()` guards:

```c
pointer = vnc_output_get_pointer(output, &pointer_pnode);   /* may be NULL */
...
cursor_surface = output->cursor_surface;                    /* may be NULL  */
buffer = cursor_surface->buffer_ref.buffer;                 /* vnc.c:599-600 */
...
nvnc_set_cursor(..., pointer->hotspot.c.x, ...);            /* vnc.c:614     */
```

`vnc_output_assign_cursor_plane()` opens with `if (!pointer) return;`.
`vnc_output_update_cursor()` does not, and never checks `cursor_surface`
either.

Both can be NULL:

* `vnc_output_get_pointer()` returns NULL when the first peer's seat has no
  pointer, or when the pointer's sprite is not in the output's z-order list.
  A newly connected client hits the second case — `vnc_new_client()` does
  `wl_list_insert(&output->peers, &peer->link)`, inserting at the **head**, so
  it immediately becomes the peer `vnc_output_get_pointer()` picks, while no
  sprite has been set for its seat yet.
* `output->cursor_surface` is assigned in exactly one place, `vnc.c:645` in
  `vnc_output_assign_cursor_plane()`, and is **never cleared**. It carries no
  destroy listener. It is NULL until that first succeeds.

What makes it reachable rather than theoretical is that the writer is
conditional and the reader is not. `vnc_output_repaint()` calls
`vnc_output_update_cursor()` unconditionally. `vnc_output_assign_planes()`
only calls `vnc_output_assign_cursor_plane()` while the peer list is non-empty
**and** `vnc_clients_support_cursor()` is true — and that returns false if
*any* connected client fails to advertise `RFB_ENCODING_CURSOR`. So a second
client that does not advertise it stops the refresh while the read continues.

## What was actually observed

Under gdb on the rig, on the real image (weston 16.0.0, neatvnc 1.0.1),
headless + VNC, breakpoints printing and continuing:

```
ASSIGN 0x563aa1b1f1e0     (x8, cursor-capable client alone)
-- second client joins, advertising no cursor encoding --
DEREF  0x563aa1b1f1e0     (x6, no intervening ASSIGN)
```

So the stale-read window is real and measurable. It is also self-limiting:
once the sprite paint node leaves the cursor plane the plane stops taking
damage, `update_cursor()` returns at its `!update_cursor` check, and the stale
pointer stops being read. That is very likely why this is not a constant
crash.

## Reproduced

Two clients was the wrong shape. Production runs **many** concurrent sessions,
and with a mixed population `vnc_clients_support_cursor()` does not flip once —
it *flaps*, because it is false whenever any one client lacks the encoding.

Load: ~20 concurrent sessions, continuous churn, roughly one client in three
advertising `RFB_ENCODING_CURSOR`, one in nine stalling mid-handshake, and one
in seventeen connecting `shared=0` (which makes neatvnc's `on_init_message`
call `disconnect_all_other_clients()`, tearing down every other seat at once).
Run natively under gdb on weston 16.0.0 + neatvnc 1.0.1, headless + VNC.

It segfaults in about two minutes, twice out of two runs:

```
Thread 1 "weston" received signal SIGSEGV, Segmentation fault.
#0  vnc_output_update_cursor (output=0x…) at ../libweston/backend-vnc/vnc.c:615
        pointer = 0x0
        pointer_pnode = 0x0
        update_cursor = true
        cursor_surface = 0x55e4eae65d90
        buffer = 0x55e4eadf1b90
#1  vnc_output_repaint (base=0x…) at ../libweston/backend-vnc/vnc.c:1078
#2  weston_output_repaint (…) at ../libweston/compositor.c:4010
#3  output_repaint_timer_handler (…) at ../libweston/compositor.c:4454
```

So it is the **NULL `pointer`** path, not the stale-`cursor_surface` one:
`cursor_surface` and `buffer` are both valid, and `vnc_output_get_pointer()`
returned NULL while the cursor plane had damage. That is precisely the case
`vnc_output_assign_cursor_plane()` guards with `if (!pointer) return;` and
this function did not.

(Line 615 rather than 614 because `0001` adds an include above it.)

## Does this patch fix it?

Yes, for that crash. Same load, same harness, module rebuilt with this patch:
the SIGSEGV does not recur — 919 sessions in one run and 323 in another, 1242
total, with zero occurrences.

**But weston still dies**, later, on a different and unrelated failure:

```
weston: ../libweston/compositor.c:3950: weston_output_repaint:
  Assertion `!wl_list_empty(&output->paint_node_z_order_list) &&
             "empty scene graph at repaint"' failed.
```

The abort itself is in **libweston core**, not in the backend. The guess
recorded here was that this therefore needed an upstream fix or a weston
rebuild; that turned out to be wrong, and `0004` fixes it from module code.
The reasoning above was right as far as it went — the assert is pre-existing
and this patch only masked it by crashing first — but the cause is in the
backend even though the abort is not. The scene graph is empty because the
backend has just had weston kill `weston-desktop-shell`. See `0004`.

## Still not tested

The station's `drm,vnc` mirrored pair — two outputs, two cursor planes, one
shared sprite view. Mirroring needs a real DRM output
(`wet_output_compute_output_from_mirror()` asserts on `native_mode_copy.width`
for a headless one), and that means the live compositor.

## Still upstream

Present unchanged in weston `main`; there are no commits to
`libweston/backend-vnc/` since the `16.0.0` tag at all.

# `0004` — pooling the per-client seats

This is the "empty scene graph at repaint" abort that `0002` ran into and
left open. It is not a second bug in libweston; it is the last step of a
chain that starts in `vnc.c`.

## The chain

Every VNC client gets its own `wl_seat`: `vnc_new_client()` allocates a
`weston_seat` and `vnc_client_cleanup()` releases it. `weston_seat_release()`
ends in `wl_global_destroy()`, which unlinks the global from the display's
list immediately.

`registry_bind()` in libwayland answers a bind for a name that is no longer
in that list with a fatal protocol error:

```
wl_display@1.error(wl_registry@2, 0, "global wl_seat (23) is unavailable")
```

No client can avoid this. It binds from the registry it was handed, and the
seat can disappear between the `wl_registry.global` event and the bind
arriving at the compositor. Weston kills the offending client — and the
clients most often mid-bind are weston's own, `weston-desktop-shell` and the
input method (`centroidx-keyboard`). **Both** die, not just the shell. Weston
respawns them, but the output is left with no paint nodes in the meantime and
`weston_output_repaint()` asserts.

So the fatal event is not "desktop-shell cannot run at all" — weston does try
to respawn it. It is the empty scene graph in the window before the respawn
maps anything.

## The patch

Rather than trying to time the destroy, stop destroying. A disconnecting
client's seat goes on a free list on the backend and the next client takes it
back. The seat global is created once per pooled seat and outlives every
individual client, so a bind cannot lose a race with anything.

Parking releases the pointer and keyboard, which is the path a physical seat
already takes when its last device is unplugged: focus cleared, grabs
cancelled, sprite unmapped, an empty capabilities event sent — but
`pointer_state` and `keyboard_state` deliberately kept, with a comment in
`libweston/input.c` saying so, "so that a newly attached pointer on this seat
will retain the previous cursor co-ordinates". Reattaching in
`vnc_new_client()` is then all a reused seat needs. Reuse is what the API is
built for, not a trick played on it.

Two properties worth having:

* **Bounded resources.** Each live seat holds a `memfd:weston-shared` for its
  xkb keymap. The pool grows only to the high-water mark of *concurrent*
  clients, so fd use tracks concurrency rather than the reconnect rate.
  Measured below.
* **A leak fixed.** The `weston_seat` that `vnc_new_client()` allocates was
  never freed — upstream leaks one per session. Pooled seats are reused, and
  `vnc_destroy()` drains the pool for real once `nvnc_del()` has disconnected
  everyone.

## Reproduced

Container on the hq rig, `backends=vnc` only, `renderer=pixman`, 1280x720,
`desktop-shell.so` plus the `[input-method]` keyboard, loopback port. Load is
ten clients looping: full RA2ne (security type 6) handshake through
`ServerInit`, brief hold, then a clean `close()`. Stock `pr-16`:

| run | sessions before death | exit |
| --- | --- | --- |
| 1 | 10 | 134 (SIGABRT, the assert) |
| 2 | 10 | 1 |

Both documented shapes, within a minute. With `WAYLAND_DEBUG=1` the same run
shows the cause directly, twice in the same millisecond — once per internal
client:

```
[12:10:48.996175]  -> wl_display#1.error(wl_registry#2, 0, "global wl_seat (23) is unavailable")
[12:10:48.997233]  -> wl_display#1.error(wl_registry#2, 0, "global wl_seat (23) is unavailable")
```

It is a race. Single clean runs mean nothing; twenty *persistent* clients are
fine, so it is the churn that matters, not the client count.

## Post-fix

One compositor instance, three runs back to back, **7189 sessions total**:

| load | duration | sessions | rate | result |
| --- | --- | --- | --- | --- |
| 10 churning | 300 s | 4494 | 14.9/s | survived |
| 20 churning | 181 s | 2675 | 14.8/s | survived |
| 20 persistent | 91 s | 20 | — | survived |

Zero occurrences across the whole log of `"is unavailable"`, of the assert, of
`"error in client communication"`, of `"Too many open files"`, and zero
`respawning...` lines — the shell and the keyboard were never killed once.

File descriptors on the compositor: **25 at rest, 34 after the 4494-session
run, 45 at the end** with 20 concurrent clients. Flat in the number of
sessions, linear in concurrency, which is the point.

## A shape that was tried and rejected

The first attempt was libwayland's race-free removal: `wl_global_remove()` to
unpublish the seat while leaving it bindable, plus
`wl_global_set_withdrawn_listener()` to destroy it once every registry has
acknowledged. That is the textbook fix and it *did* kill the protocol error —
zero occurrences in 1517 sessions, against death at 10 without it.

It was rejected because it still crashed, for a new reason. Acknowledgements
arrive in bursts tens of seconds later, so under sustained churn the pending
seats pile up, each holding its keymap `memfd`. Descriptors climbed to ~515
against a soft `RLIMIT_NOFILE` of 1024 and the compositor died of
`creating timer failed: Too many open files`.

That is survivable with a higher `nofile` — which is being raised separately —
but a fix that silently depends on that limit is a trap for anyone who deploys
without it. Pooling has no such dependency, so it replaced the withdrawn
approach rather than joining it. (Keeping both would also have left the
withdrawn path as dead code: with a pool, no global is ever removed while the
compositor is running.)

## Not tested here

* The station's mirrored `drm,vnc` pair, for the same reason as `0001` and
  `0002` — mirroring needs a literal DRM connector, which a container has not
  got.
* `renderer=gl`. The lab runs pixman.
* Cursor-flap and keyboard-slide cycles were not re-run as named regressions;
  the earlier agent's scripts for them were deleted at teardown. The churn
  load does exercise the `0002` path hard on its own — 7189 pointer
  create/destroy cycles through `vnc_output_get_pointer()` with no segfault —
  but that is a side effect, not the targeted test.
* The `nofile` limit itself. It is being handled separately and no compose
  change is folded in here.

## Still upstream

`vnc_client_cleanup()` is unchanged in weston `main`, so the race is too.
Worth sending upstream along with `0001`.

## Why the module is swapped rather than weston rebuilt

`vnc-backend.so` is a dlopened module, so only it has to be replaced — the
same trick already used for `libneatvnc.so.1`, and it keeps the runtime image
on Debian's `weston` package.

The catch is that the module links `dep_libweston_private`, libweston's
*internal* ABI, not the stable public one. So it must be built from the same
sources the runtime installs. Three things make that hold, and all three
need re-checking if the base image moves:

* Debian forky ships weston **16.0.0** and carries only two patches,
  `Disable-vulkan-output-damage-test.patch` and `Update-PAM-config.patch`
  (`apt-get source weston`, `debian/patches/series`). Neither touches
  libweston or the VNC backend, so the upstream `16.0.0` tag is the right
  source.
* The meson options are the ones from Debian's own `debian/rules`
  (`-Dbackend-rdp=true -Dbackend-vnc=true -Drenderer-vulkan=true
  -Dsystemd=true`), so the generated `config.h` matches.
* `vnc.c` has no `#ifdef` of its own, and the one config-conditional thing it
  can see through libweston's headers, `HAVE_COMPOSE_AND_KANA`, only adds
  values to `enum weston_led` — no struct layout depends on it.

## Not fixed here

Weston's VNC backend has **zero** clipboard references against the RDP
backend's 24 (`grep -ci clipboard libweston/backend-vnc/vnc.c` vs
`libweston/backend-rdp/rdp.c`), so paste-from-client does not work over VNC at
all. That is a separate gap, not addressed by this patch.
