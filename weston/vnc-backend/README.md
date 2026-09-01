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

That is an abort (SIGABRT) in **libweston core**, not in the backend, so
swapping `vnc-backend.so` cannot address it — it needs an upstream fix or a
weston rebuild. It reproduced in both runs with this patch applied. It is
almost certainly pre-existing and merely masked before, since the segfault
arrived first and this patch only adds early returns to
`vnc_output_update_cursor()` — it cannot add or remove paint nodes. That
reasoning has not been separately proven.

## Still not tested

The station's `drm,vnc` mirrored pair — two outputs, two cursor planes, one
shared sprite view. Mirroring needs a real DRM output
(`wet_output_compute_output_from_mirror()` asserts on `native_mode_copy.width`
for a headless one), and that means the live compositor.

## Still upstream

Present unchanged in weston `main`; there are no commits to
`libweston/backend-vnc/` since the `16.0.0` tag at all.

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
