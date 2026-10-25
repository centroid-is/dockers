# libweston patches

Patches to weston's core library, `libweston-16.so.0.0.0`. They are applied
in the same source tree as the VNC backend patches, after them, and the
library is built with Debian's own flags (`dpkg-buildflags`,
`--buildtype=plain`) and directories (`--prefix=/usr`, the multiarch libdir:
the library compiles its module path in) and copied over Debian's in the
runtime image. Debian forky still ships 16.0.0-1, the tag it is built from;
if Debian moves past 16.0.0, `WESTON_VERSION` has to follow, or the stock
modules load a library from another release.

## 0001: re-take a cursor surface another pointer has released

**The crash.** batcher3-hmi, 2026-10-21 09:58:37, while an operator typed into
the HMI's IP-settings dialog over noVNC with a second VNC client connected:

```
weston[200488]: segfault at 90 ip 00007f2b04778bd2 sp 00007fffbc3cb160 error 4
  in libweston-16.so.0.0.0[3ebd2,7f2b04754000+42000]

#0 pointer_cursor_surface_committed (es=0x55de28282aa0) at libweston/input.c:3584
#1 pointer_set_cursor (serial=5266, x=1, y=1) at libweston/input.c:3662
```

`0x3ebd2` is the offset from the library's load base (= its file offset; the
text segment starts at file offset and vaddr `0x1a000`, so it is `0x24bd2`
into the segment). The core (`core.19`, `kernel.core_pattern=core`, written in
weston's working directory inside the container) was symbolized against the
shipped library itself (build-id `4f1c7a09a99109875cb87d87f20ed8b8d80e242d`,
Debian `libweston-16-0 16.0.0-1`) with Debian's debug info from
debuginfod.debian.net:

```
mov 0x328(%rdi),%rbp    ; pointer = es->committed_private   -> NULL
mov 0x90(%rbp),%rdi     ; pointer->sprite                   -> fault at 0x90
```

**Why committed_private is NULL.** flutter-elinux keeps one cursor
`wl_surface` and calls `wl_pointer.set_cursor` with it on whichever
`wl_pointer` entered last. With two VNC clients there are two `VNC Client`
seats, so both seats' pointers end up with a sprite view of that one surface,
while the surface's single `committed` hook and `committed_private` name only
the last of them.

When one pointer drops its sprite, `pointer_unmap_sprite()` clears the
surface's `committed`/`committed_private` whoever they belong to. That pointer
drops it when its seat loses the pointer (a VNC client disconnects; the
pooled seat of `../vnc-backend/0004` is released the same way), when it enters
another client that sets its own cursor (the on-screen keyboard panel), or on
a NULL cursor.

The other pointer still has `sprite->surface == surface`. Flutter's next
`set_cursor` on it with the same surface and a new hotspot (a cursor shape
change: arrow, I-beam, hand) skips the role setup in `pointer_set_cursor()`,
because the sprite already shows that surface, and calls
`pointer_cursor_surface_committed()` directly.

In the core: seat 2's pointer (the operator's: last button 09:58:37.019,
170 ms before the crash) was over the Flutter window, with its sprite on
Flutter's surface and that surface's hook already cleared. Seat 1's pointer
had been idle since the day before, parked on the bottom row (542,1079), where
the on-screen keyboard appears; it had just entered the input panel (serial
7986, after the operator's click at 7972) and taken the keyboard's cursor
surface.

**The fix.** In `pointer_set_cursor()` a sprite whose surface this pointer
does not own (`committed_private != pointer`) counts as no sprite: unmap it,
then set the surface up again. Both the early return for an unchanged cursor
and the setup branch check ownership. Weston main is unchanged here as of
this writing; weston#174 (2018) is the assert-build form of the same mismatch.

**Re-checking.** `verify/` reproduces it with the production image under
`-B vnc`: `cursor-share.c` is the flutter-elinux cursor model (one surface,
last-entered pointer, the hotspot changes with x), and `pointer.py` is an
RA2ne RFB client that only moves the pointer. Client A enters, B enters, A
disconnects, then B crosses to the right half of the window. The unpatched
image exits 139 there. With the patch, or without the crossing, it keeps
running. See `verify/run.sh`.
