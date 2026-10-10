# Patches on noVNC

The image builds noVNC from the tag in `NOVNC_REF` (v1.7.0) and applies these
on top, in order. The build fails if one does not apply.

| Patch | Origin | What it does |
|---|---|---|
| `0001-Add-permissions-exclusive-async-clipboard.patch` | upstream `f5a4eed` | Shares the clipboard through the browser's async clipboard API. |
| `0002-Disable-clipboard-button-with-async-clipboard.patch` | upstream `d9b45d3` | Hides the clipboard panel button where the async clipboard is in use. |
| `0003-CentroidX-a-setting-that-switches-the-shared-clipboa.patch` | ours | Adds the `shared_clipboard` setting. Upstream has no off switch. |
| `0004-CentroidX-resend-clipboard-text-that-lost-characters.patch` | ours | Stops the first text after connecting from arriving with `?` in it. |

0001 and 0002 are the two commits of
[novnc/noVNC#1993](https://github.com/novnc/noVNC/pull/1993), merged to
upstream `master` in October 2025 and in no release as of v1.7.0. They are
`git format-patch` output of a clean cherry-pick onto v1.7.0, tests included,
with nothing edited.

## What the operator gets

With the clipboard shared, text copied on the operator's PC can be pasted in
the station, and text copied in the station can be pasted on the PC. There is
no clipboard panel to open.

* **PC to station** happens when the noVNC canvas gains focus, not when
  something is pasted. Connecting counts, and so does clicking into the
  session or switching back to its tab or window: each sends whatever text the
  PC's clipboard holds at that moment. Copying something else on the PC while
  the canvas keeps focus sends nothing until focus comes back.
* **Station to PC** happens when the station's selection changes, and the
  browser only accepts it while the page has focus. A copy made on the station
  while the operator is in another tab or window does not reach the PC, and is
  replaced on the station by the PC's clipboard when the operator comes back.
* Text only. On Windows the browser hands line ends to the clipboard as CRLF.

## Where it works

The browser has to offer the async clipboard API with queryable
`clipboard-read` and `clipboard-write` permissions, and that needs a secure
context: https, or `localhost`. A station is reached over https, because
`redirect.py` sends plain http there. Plain http to a station's address never
worked and still does not: without a secure context the login handshake fails
before a session exists.

* **Chrome, Edge** (Chromium): works. The first time the canvas gains focus
  the browser asks whether the site may see the clipboard, once per station
  address. Allow keeps it shared. Block switches it off for that site, and the
  clipboard panel does not come back: upstream hides it for a denied
  permission as well, so the way back is the browser's site settings.
* **Firefox, Safari**: unchanged from v1.7.0. They do not answer
  `navigator.permissions.query` for the two permissions, so noVNC treats the
  async clipboard as unsupported and the clipboard panel in the control bar
  stays.

## Switching it off

`shared_clipboard` is an ordinary noVNC setting without a control in the
settings panel. Off means v1.7.0 behaviour: nothing is read from or written
to the PC's clipboard, the browser never asks for the permission, and the
clipboard panel is back.

| Where | How | Reach |
|---|---|---|
| A station | `NOVNC_SHARED_CLIPBOARD=off` on the novnc container | Every browser; the URL cannot turn it back on. |
| One browser | `?shared_clipboard=false` on the URL | That page load. |
| The image | `"shared_clipboard": false` in `defaults.json` or `mandatory.json` | Every station running the image. |

## Why 0004

`UI.connectFinished` focuses the canvas as soon as a connection is up, so the
PC's clipboard is sent at once. The server's extended clipboard capabilities
arrive some milliseconds later; until they do noVNC can only send a plain
`ClientCutText`, which is ISO 8859-1 with every other character replaced by
`?`. Icelandic survives that. Polish does not, nor does `→`, `–` or `€`.
Later sends use the extended clipboard and arrive whole, which makes the loss
look random.

0004 remembers a text that lost characters that way and sends it again through
the extended clipboard once the capabilities are in. This is a defect in
upstream `master` as well and has not been reported there.

## Moving `NOVNC_REF`

To a release that contains #1993: delete 0001 and 0002, then regenerate 0003
and 0004 against the new tag. 0003 touches `UI.initSettings`, `UI.connect` and
`UI.updateClipboard` in `app/ui.js`, and the constructor and `_writeClipboard`
in `core/rfb.js`. 0004 touches `clipboardPasteFrom` and the capabilities
branch of `_handleServerCutText` in `core/rfb.js`.

To a release that still lacks it: cherry-pick `f5a4eed` and `d9b45d3` onto the
new tag, apply 0003 and 0004, and regenerate all four with
`git format-patch --no-signature --zero-commit -N <tag>..HEAD`.
