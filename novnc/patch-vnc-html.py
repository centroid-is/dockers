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

html = html.replace(ANCHOR, OVERRIDES.read_text() + ANCHOR, 1)


# --- branding -------------------------------------------------------------
#
# The tab is the only part of noVNC an operator sees for more than a moment,
# and it read "Weston VNC backend - noVNC" under a noVNC favicon. Neither
# name means anything to someone looking at a CentroidX station.
#
# Each replacement is asserted, so a noVNC upgrade that renames any of this
# fails the build rather than quietly reverting to upstream branding.

def replace_once(haystack: str, old: str, new: str, what: str) -> str:
    if old not in haystack:
        sys.exit(f"patch-vnc-html: could not find {what}; noVNC's vnc.html has "
                 "changed and the branding needs rework")
    return haystack.replace(old, new, 1)


html = replace_once(html, "<title>noVNC</title>", "<title>CentroidX</title>", "the title")

# One SVG replaces the .ico and every apple-touch PNG. Browsers prefer the
# SVG when offered, and leaving the PNGs in place would keep noVNC's mark on
# an iOS home screen.
icon_start = html.index('<link rel="icon"')
icon_end = html.index(">", html.rindex('<link rel="apple-touch-icon"')) + 1
html = (html[:icon_start]
        + '<link rel="icon" type="image/svg+xml" href="app/images/icons/centroid-mark.svg">'
        + html[icon_end:])

# The wordmark on the connect and disconnect screens. Keeping the <span>
# preserves noVNC's two-tone treatment of it.
html = replace_once(html,
                    '<h1 class="noVNC_logo" translate="no"><span>no</span><br>VNC</h1>',
                    '<h1 class="noVNC_logo" translate="no"><span>Centroid</span><br>X</h1>',
                    "the connect-screen wordmark")
html = replace_once(html,
                    '<p class="noVNC_logo" translate="no"><span>no</span>VNC</p>',
                    '<p class="noVNC_logo" translate="no"><span>Centroid</span>X</p>',
                    "the status wordmark")

VNC_HTML.write_text(html)
print(f"patch-vnc-html: overrides and branding applied to {VNC_HTML}")
