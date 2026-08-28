"""Insert the CentroidX overrides into whatever vnc.html the pinned noVNC ships.

Done at build time rather than by shipping a hand-edited copy. A copy taken
from one noVNC version and served alongside another's app/ui.js is broken in
ways that only show up when a user touches the wrong control: master had
dropped #noVNC_control_bar_hint while v1.7.0's ui.js still called it, so
grabbing the control bar handle threw "Cannot read properties of null".

If upstream ever moves the anchor this fails the build instead of shipping
something subtly wrong.
"""
import pathlib
import sys

VNC_HTML = pathlib.Path("/opt/novnc/vnc.html")
OVERRIDES = pathlib.Path("/tmp/overrides.js")
ANCHOR = "        UI.start({ settings:"

html = VNC_HTML.read_text()
if ANCHOR not in html:
    sys.exit(f"patch-vnc-html: anchor {ANCHOR!r} not found in {VNC_HTML}; "
             "noVNC's vnc.html has changed shape and the overrides need rework")
if "CentroidX:" in html:
    sys.exit("patch-vnc-html: overrides already present")

VNC_HTML.write_text(html.replace(ANCHOR, OVERRIDES.read_text() + ANCHOR, 1))
print(f"patch-vnc-html: overrides inserted into {VNC_HTML}")
