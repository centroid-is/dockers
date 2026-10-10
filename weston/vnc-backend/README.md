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

## `0005` — clipboard bridging

> Numbered `0005`, after the pooling patch, not `0003`. Glob order is the only
> ordering mechanism the build has, and this patch and `0004` both add fields
> to `struct vnc_backend`, so one has to be written against the other. `0004`
> is merged and this one is not, so this one moved.

Stock `backend-vnc` implements no clipboard at all, where `backend-rdp` bridges
MS-RDPECLIP to the seat's selection in `rdpclip.c`:

```
$ grep -ci clipboard libweston/backend-vnc/vnc.c   # 0
$ grep -ci clipboard libweston/backend-rdp/rdp.c   # 24
```

So nothing a client copies reaches a Wayland application, and nothing an
application copies reaches the client. neatvnc parses the RFB `ClientCutText`
message perfectly well and then drops it, because weston installs no
`nvnc_set_cut_text_fn()` handler.

The patch bridges both directions, following `rdpclip.c`:

* a client's cut text becomes a `weston_data_source` published with
  `weston_seat_set_selection()`, offering `text/plain;charset=utf-8`,
  `text/plain`, `UTF8_STRING`, `STRING` and `TEXT`;
* `weston_seat::selection_signal` drives a read of the new selection over a
  pipe, and the result goes out through `nvnc_send_cut_text()`.

Sources this backend published are recognised by their `accept` callback and
skipped, so a paste is not echoed straight back at the client that sent it.

Debian's `libneatvnc.so.1` exports both entry points, so nothing outside
`vnc.c` has to change:

```
$ nm -D --defined-only /usr/lib/*/libneatvnc.so.1 | grep cut_text
0000000000009f48 T nvnc_send_cut_text
000000000000adc8 T nvnc_set_cut_text_fn
```

### Every seat, not just the peer's

This is the one place the patch deliberately departs from `rdpclip.c`, which
publishes only on the seat belonging to its own peer.

`vnc_new_client()` creates a fresh `weston_seat` named `VNC Client` when a
peer connects. The HMI container's `flutter_elinux_wayland` binds its
`wl_data_device` once, during start-up, to the seats that exist at that moment
(`elinux_window_wayland.cc`, the `for (auto& [seat, _] : seat_inputs_map_)`
loop right after the first registry roundtrip). On a station that is the DRM
backend's seat and nothing else — the VNC peer's seat does not exist yet and
never gets a data device. A selection published only on the peer seat would be
invisible to the application it is meant for.

So the patch tracks `weston_compositor::seat_list` plus `seat_created_signal`
and publishes on all of them — **except the seats it created for its own
peers**. That also means a copy made at the station's own keyboard reaches the
remote client, not just the other way round.

The peer-seat exception is not a detail; publishing there caused a real bug.
A selection is per seat, so a client holding a `wl_data_device` on more than
one seat is offered the same text once per seat, and a client that reads all
of its offers ends up with the payload repeated once for each. It scales with
the seat count — two seats give twice the payload, three give three times:

```
$ multiseat-reader                  # before
seats=2 offers=2 total=46
  seat[0] default     offer=yes
  seat[1] VNC Client  offer=yes
$ multiseat-reader                  # after
seats=2 offers=1 total=23
  seat[0] default     offer=yes
  seat[1] VNC Client  offer=no
```

**What this does not explain.** A 23-byte copy was seen pasting back as 46
bytes on the rig, measured with plain `wl-paste`, and that is *not* this bug.
`wl-clipboard` 2.3.0 binds a single seat — with two seats present it sees both
`wl_seat` globals but makes one `get_data_device` call, receives one
`selection` event and issues one `receive` (checked under `WAYLAND_DEBUG=1`).
Against the unpatched build in a two-seat setup it returns 23 bytes, before
and after the publishing client disconnects, and still 23 with three seats,
with five publishes in a row, and with two peers connected at once. So
something else produced that 46, and it is still open. The fix below is worth
having on its own merits; it should not be taken as having closed that.

Skipping the peer's own seat costs nothing worth having: the text just came
*from* that peer, and the seat is created when the peer connects — long after
the clients that want to paste have bound their data devices to whatever seat
existed then. On a station that leaves exactly one seat carrying the
selection, the DRM one, which is the seat the HMI is listening on.

The one thing it gives up is a Wayland client that binds a data device *only*
to a VNC peer's seat, having started after that peer connected. Such a client
would not be offered the text. Nothing on a station does this.

### Seat pooling

`0004` parks a disconnecting client's seat on a free list and hands it to the
next client instead of destroying it, which changes two things this patch
depends on.

`weston_seat_init()` now runs only the first time a given seat is created, so
`seat_created_signal` fires once per *pooled* seat rather than once per
client. That is exactly what is wanted: the flag that marks a seat as one of
this backend's own is set around that one call, and a seat taken back off the
pool needs no re-marking — it was marked when it was first created, and the
record has been kept ever since.

`weston_seat_release()` no longer runs per client either, only when the
backend goes away, so the per-seat record and any selection on it outlive
individual clients by design rather than by accident. `vnc_clipboard_destroy()`
runs at the top of `vnc_destroy()`, before `nvnc_del()` and before `0004`
drains the pool, so the seat-destroy listeners are gone before those seats are
released and nothing fires into freed state.

Checked rather than assumed — one client connects and publishes, leaves, and
two more take the same recycled seat in turn:

```
client A, fresh seat        seats=2 offers=1 total=23
A gone, seat parked         seats=2 offers=1 total=23
client B, recycled seat     seats=2 offers=1 total=23   (offer on default, none on VNC Client)
client C, same seat again   seats=2 offers=1 total=23
```

Worth noting for the unexplained 46 below: with pooling the seat count no
longer falls when a client leaves, so a seat-count-driven duplication would be
*persistent*, not intermittent. The rig's 46 was intermittent.

### Text encoding

RFB 3.8 specifies cut text as latin-1. Clients that negotiate the extended
clipboard send UTF-8 instead — noVNC does, via `encodeUTF8` in
`RFB.messages.extendedClipboardProvide` — and neatvnc hands both to the same
callback unconverted. Incoming text that is not valid UTF-8 is therefore
treated as latin-1 and converted, which leaves ASCII alone either way.
Outgoing text is passed through as UTF-8; on the legacy path that is a
deliberate deviation from the letter of the spec, and the same one every
client in practice expects.

### What was tested, and what was not

None of this ran on a station. It was exercised in a container on an arm64
Mac against the image this Dockerfile builds, driven by the two scripts in
`verify/`. **The station runs `renderer=gl` and a `drm,vnc` pair, and neither
of those was tested.**

weston ran as `-B headless,vnc --fake-seat`, which reproduces the thing that
matters: a `default` seat that exists at start-up, plus a `VNC Client` seat
created later, exactly the split a station has. The Wayland side is
`wl-copy`/`wl-paste`; `WAYLAND_DEBUG=1` confirms `wl-paste` binds its data
device to `wl_seat` `"default"`, so every paste below crossed from one seat to
the other.

| scenario | result |
|---|---|
| legacy `ClientCutText` → `wl-paste` | `hello-from-vnc`, on the `default` seat, offering all five mime types |
| `wl-copy` → legacy `ServerCutText` | exact bytes back at the client |
| latin-1 in: `fisk\xfeur` | pasted as `fisk\xc3\xbeur` — converted |
| UTF-8 in: `\xc3\xbe` | pasted as `\xc3\xbe` — not double-encoded |
| UTF-8 out: `íslenskt-þðáé` | arrives as UTF-8, byte for byte |
| **extended** clipboard, both directions | `íslenskt-þ` in, `ext-copy-þðá` out, both intact |
| 512 KiB in and out | 524288 bytes each way, sha256 matching on both sides |
| 40 selections in a row | all 40 delivered, no drops |
| client disconnects holding the selection | weston alive, selection still pasteable on `default` |
| `weston-screenshooter` with a client attached | 1s, `0001`'s fix still holds |
| `SIGTERM` with a selection live | exit 0 |
| retained selection, 23 B and 4 MiB, pasted repeatedly | exact byte count every time |
| reader closing mid-transfer, 8 rounds | weston alive, selection still whole |
| a client with a data device on every seat | one copy, not one per seat |
| three copies in a row, client side | one distinct `ServerCutText` length |

The last three rows are `verify/retained.sh`, which asserts byte **counts**
rather than content. A selection that comes back subtly wrong — truncated, or
served twice over — reads correctly at a glance and is only visible when
counted.

The 512 KiB case matters because it is the only one that exercises the partial
write (the requesting client's pipe fills, `EAGAIN`, and the transfer finishes
from a `WL_EVENT_WRITABLE` handler) and the multi-read on the way back.

What was **not** tested: `renderer=gl`, a `drm,vnc` pair, a real browser, and
the actual HMI application. The claim about `flutter_elinux_wayland` above is
read out of its source, not observed.

### SIGPIPE

Worth knowing about, because it is not obvious and it is fatal.

The fd this backend writes clipboard text into belongs to the *requesting
client*, which is free to exit part-way through the transfer. Writing to a
pipe whose reader has gone raises SIGPIPE, and:

```
$ grep -rn SIGPIPE libweston/ frontend/ shared/
$
```

weston installs no handler for it anywhere, so the default disposition
applies and the entire compositor dies — exit 141, one line of `Broken pipe`,
every client taken down with it. This was seen during development as a container
exiting 141 immediately after a clipboard transfer, and independently on the
hq rig under `weston:pr-14`, where it killed the compositor and every client
with it.

So the write goes through `vnc_write_nosigpipe()`, which blocks SIGPIPE across
the `write()` and consumes the pending signal if it fired, leaving the caller
with a plain `EPIPE` to handle. `backend-rdp` has the same exposure in
`clipboard_data_source_write()` and no such guard; its clipboard is just
exercised far less.

It reproduces on demand, but not with `wl-paste`: that drains the whole offer
into memory as fast as it arrives, so killing it mid-transfer never leaves a
write stranded, and an unguarded build sails through nine different kill
delays from 5 ms to 1.5 s. What is needed is a reader that keeps reading —
holding the compositor inside its write loop — and then drops the pipe
part-way. That is `verify/abort-reader.c`, and against a 9 MB selection:

| build | result |
|---|---|
| before the guard | **weston exit 141, `Broken pipe`, on the first round** |
| after | 8 rounds, weston alive, selection still reads back whole |

`verify/retained.sh` fails on the unguarded build and passes on the guarded
one, so it is a real regression test rather than one that happens to pass
either way. It catches the per-seat duplication the same way.

### One neatvnc oddity, harmless

`ext_clipboard_save_provide_msg()` writes a length prefix that excludes the
terminating NUL but a payload that includes it, so a Provide message from the
server declares 15 bytes and carries 16. Confirmed on the wire here
(`declared=15 inflated_after_prefix=16 trailing=b'\x00'`). Both readers cope:
noVNC builds a fresh `Inflator` per message and discards the leftover byte
(`core/rfb.js`, `_handleExtendedClipboardProvide`), and neatvnc's own receive
path expects the NUL to be counted. Nothing to do about it here; noted so the
next person who stares at a hexdump does not chase it.

### A browser still cannot paste with Cmd+V

Worth being blunt about, because the patch does not change it. noVNC 1.7.0 —
the pinned version in `novnc/Dockerfile` — contains no reference to
`navigator.clipboard` anywhere in `app/` or `core/`. Its only clipboard route
is the sidebar panel: a textarea whose `change` event calls
`UI.clipboardSend()` (`app/ui.js`). So the remote user opens the clipboard
panel, pastes into it, clicks away for `change` to fire, and only then does the
text cross. Text coming the other way lands in that same textarea and has to
be copied out of it by hand.

That is a browser limitation, not a weston one: a page cannot read the system
clipboard on a keypress without an explicit user gesture and permission, and
noVNC does not ask for one.

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

# `0006` — keymap variant and options on the VNC seats, and reused seats

## Options never reached a VNC client

The VNC backend compiles a keymap of its own for the per-client seats, from
the compositor's rule names, but copies only three of the five:

```c
backend->xkb_rule_name.rules = strdup(compositor->xkb_names.rules);
backend->xkb_rule_name.model = strdup(compositor->xkb_names.model);
backend->xkb_rule_name.layout = strdup(compositor->xkb_names.layout);
```

So weston.ini's `keymap_variant` and `keymap_options` apply to the panel's own
libinput seat and are dropped for every VNC client. The layout-switch shortcut
is an xkb *option* (`grp:alt_shift_toggle`), so with
`keymap_layout=is,us,pl` a keyboard on the panel cycles layouts on Alt+Shift
and a VNC keyboard is stuck in the first one forever. Nothing is logged; the
key simply does nothing.

The patch copies `variant` and `options` too. Both are NULL unless weston.ini
sets them (`weston_compositor_set_xkb_rule_names` only defaults rules, model
and layout), and `strdup(NULL)` is undefined, so each copy is guarded.
`vnc_destroy()` now frees all five names; it never freed the original three.

## A reused seat told its client the wrong group

`0004` parks a disconnecting client's seat and hands it to the next client.
Parking goes through `weston_seat_release_keyboard()`, whose
`weston_keyboard_reset_state()` replaces the `xkb_state` with a fresh one —
group 0, nothing held, no locks. It does not touch `keyboard->modifiers`, the
serialized copy `weston_keyboard_set_focus()` sends to a newly focused client
(`send_modifiers_to_resource`, libweston/input.c). The next client was
therefore told the previous one's group and Caps Lock while the seat typed
group 0 in lower case, until its first modifier key resynchronised the two.

On reuse the patch calls `notify_modifiers()` once, which re-serializes the
real state into that copy before any focus is set. A freshly created seat has
no stale copy and is left alone.

## Not tested here

Built and exercised only through the image: the patch applies on 0001–0005
in the Dockerfile's order, and the keymap `is,us,pl` with
`grp:alt_shift_toggle` compiles in a headless start. Switching over a live
noVNC session on a panel still wants an eyes-on check.

# `0007` — draining a superseded clipboard read

The second Ctrl+C in the HMI over noVNC took the panel down: weston exited
141 with one line, `Broken pipe`, and the `flutter` container followed it
(exit 133, a CEF `DCHECK` on the way out, which is the core dump a station
keeps). The first Ctrl+C was always fine.

## The chain

`0005` reads every new selection over a pipe and sends it to the VNC clients.
One read at a time: when a newer selection arrived, the read in flight was
destroyed, which closed the pipe's read end. The write end is by then in the
source's hands, and nothing says the source has written yet.

The source is not always a client. `libweston/clipboard.c` keeps its own copy
of every selection, so that it survives the application that made it, and
publishes that copy when the owner's `wl_data_source` goes away:

```c
if (source == NULL) {
        if (clipboard->source)
                weston_seat_set_selection(seat, &clipboard->source->base,
                                          clipboard->source->serial);
        return;
}
```

That is called from *inside* the `selection_signal` emit that announced the
NULL, and `weston_seat_set_selection()` emits the signal again. So for one
destroyed source this backend's listener runs twice:

1. the owner destroys its source; `destroy_selection_data_source()` sets the
   selection to NULL and emits;
2. `clipboard.c`, first on the list because `weston_seat_init()` registers it,
   publishes its copy — a nested emit, in which the bridge starts reading that
   copy. `clipboard_source_send()` only queues the write, for the event loop;
3. the outer emit moves on to the bridge's listener, which looks at the seat,
   finds a selection and a read in flight, and supersedes it: read end closed;
4. the event loop runs `clipboard_client_data()`, a bare `write()` into that
   pipe. weston handles SIGPIPE nowhere (`0005`, "SIGPIPE"), so it dies.

flutter-elinux destroys its previous `wl_data_source` before creating the next
one on every `Clipboard.setData` (`ELinuxWindowWayland::SetClipboardData`), so
its first copy takes no part in this and every later one walks straight into
it. No timing is involved; it is every time.

## The patch

A read that is no longer wanted is kept open and drained to EOF with the data
thrown away, instead of being closed under its writer. Until the writer is
done it costs the compositor two descriptors: the pipe's read end, and the dup
that `wl_event_loop_add_fd()` keeps of every fd it watches. The write end is
the writer's — `client_source_send()` passes it to the client and closes its
own copy. The same is done for a selection over the size limit,
which was also hung up on mid-write — there the writer could just as well be
the application, and flutter-elinux answers `wl_data_source.send` with a
blocking `write()` and no SIGPIPE handling of its own.

Draining also meant the superseded read of step 2 sent nothing, so one copy
was still one cut-text message. Since `0008` that read is never started: a
withdrawal has the bridge reading nothing at all, and the drain is left with
what it still covers — a client's source superseded before it has written,
and a selection over the size limit.

## Reproduced, and checked

On the real images, `weston:latest` and `centroid-hmi:latest`, weston as
`-B vnc,headless --fake-seat` with llvmpipe, an RFB client pressing real keys
(Control_L down, c down, c up, Control_L up) into a focused text field:

| build | result |
|---|---|
| before | first Ctrl+C: text arrives at the client. Second: **weston exit 141, `Broken pipe`**, flutter exit 133 four seconds later |
| after | 17 Ctrl+C in a row, single, held and back to back: both containers up, the text arrives each time |

Without the application, the same event is a publisher that exits:
`wl-copy`, then kill it. `verify/retained.sh` now does that five times over —
weston exited on the first round before the patch and survives all five
after, with the retained copy still pasting at the right length.

## Not tested, and not fixed

As for every patch here: no station, no `drm,vnc` pair, no real GPU.

`clipboard.c` itself is unchanged, because weston is not rebuilt, so its bare
`write()` is still there for any *other* reader that closes early — a Wayland
client that asks for a paste and exits mid-transfer of a selection larger
than a pipe buffer. Nothing on a station does that today. The complete answer
is weston ignoring SIGPIPE, which is upstream's to take; `backend-rdp` has the
same exposure.

A drained read lives until its writer closes the pipe. A client that accepts
`send` and then never writes or closes holds two descriptors of the
compositor's for as long as it runs, the read end and the event loop's dup of
it. Measured with a source that sleeps on `send` and a `wl-copy` to supersede
it: 4 pipe ends in `/proc/<pid>/fd` at rest, 6 while the drain waited, 4
again once the sleeper was gone. (The two others of the 8 seen before the
supersede were `clipboard.c`'s own read of the same source, which it closes
when superseded — under its writer, so that is where the sleeper's SIGPIPE
came from.)

# `0008` — skipping the compositor's own clipboard sources

`0007` stopped a withdrawal killing the compositor. It left it able to hang on
one, which is worse: a hung weston is still `running` to docker, so
`restart: unless-stopped` never fires, nothing is logged, no core is kept, and
every client is stuck with it until somebody notices the panel.

## The chain

The same two listener runs as in `0007`, with the pipe now kept open:

1. the owner withdraws; `clipboard.c` republishes its retained copy from
   inside the signal, and the bridge asks that copy to `send` into a fresh
   pipe;
2. the outer run of the listener finds a read in flight, drains it, and asks
   the same copy to `send` again, into a second pipe;
3. `clipboard_source_send()` answers each with `clipboard_client_create()`,
   which queues `clipboard_client_data()` on the compositor's event loop — one
   plain `write()` of the whole text. The fd it writes to is blocking: the
   bridge sets `O_NONBLOCK` on its read end only;
4. a pipe holds 64 KiB. Up to that the `write()` returns at once and the
   bridge drains the pipe on the next turn of the loop. Past it, `write()`
   waits for a reader, the only reader is the bridge's handler on this same
   event loop, and nothing runs again.

In `/proc/<pid>/fd` that is eight pipe ends more than at rest — both pipes,
read end and write end, each with the event loop's dup — and `wchan` says
`anon_pipe_write`. `wl-paste` never returns, a VNC client's keys go nowhere,
and the container is `running`.

| withdrawn selection | `0007` alone | with `0008` |
|---|---|---|
| 60 KB | pastes back, 60000 | pastes back, 60000 |
| 70 KB | **hung** in `write()`, `wl-paste` times out | pastes back, 70000 |
| 200 KB | **hung** | pastes back, 200000 |
| 12 MB, over the bridge's own 10 MB limit | **hung** | pastes back, 12000000 |

Every time, not sometimes. The threshold is the pipe's capacity, not the
bridge's limit.

## The patch

The retained copy is nothing the VNC clients lack: they were sent the text
when its owner published it, and `clipboard.c` keeps it for Wayland clients,
which read it over pipes of their own. So the bridge does not read it. A
source of the compositor's own is told from a client's by the one thing it
cannot have, a `wl_resource`:

```c
if (!source->resource)
        return;
```

right after the `accept` check that already skips the bridge's own sources.
In libweston only two kinds of source are made without a resource:
`clipboard.c`'s retained copies and Xwayland's X11 selections
(`xwayland/selection.c`), and this image runs no Xwayland. Were it ever to,
X11 applications' copies would stop reaching the VNC clients, and this check
would have to look at `send` instead.

With it, a withdrawal starts no read at all — neither run of the listener
finds anything to do — so `0007`'s drain is off that path. It stays for what
it still covers: a client's own source superseded before it has written, and
a selection over the bridge's limit. Both have a client at the far end of the
pipe, which the compositor never waits on.

Making the write end non-blocking instead would not have done:
`clipboard_client_data()` takes a short or failed `write()` as the end of the
transfer (`if (client->offset == size || len <= 0)`), so `EAGAIN` would have
closed the pipe after the first 64 KiB and sent the VNC clients a truncated
copy of text they already had.

## Reproduced, and checked

The table above is this image, `-B headless,vnc --fake-seat`: `wl-copy` N
bytes, `kill` it, `timeout 5 wl-paste -n | wc -c`. `verify/retained.sh` now
has that as a round of its own, at 200 KB and 12 MB, judged by whether the
paste comes back within five seconds and at the right length — the 23-byte
round and the container's status both pass on a hung compositor, which is
how `0007` shipped without seeing this. Against `0007` alone it fails,
`got 0, wanted 200000` and the same for 12 MB; with this patch every round
of the script passes.

What the earlier patches were for still holds on the same build: the HMI's
Ctrl+C over RFB into a focused text field, sixteen single presses each
answered with the text, then five held, both containers up throughout; twenty
`wl-copy; kill; wl-copy` with no pause between; twenty withdrawals; and the
descriptor count back at rest afterwards (4 pipe ends, as before any of it).

## Not tested, and not fixed

As for every patch here: no station, no `drm,vnc` pair, no real GPU.

`clipboard.c`'s `write()` is still blocking, and still on the compositor's
thread, for any Wayland client that asks for the retained copy over a
blocking pipe and then stops reading without closing; the compositor would
wait in `write()` for as long as that client held the pipe. `wl-paste` and the
HMI read what they ask for, so nothing on a station does this. The answer is
upstream's, in `clipboard.c`.
