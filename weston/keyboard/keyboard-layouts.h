/*
 * Copyright © 2012 Openismus GmbH
 * Copyright © 2012 Intel Corporation
 * Copyright © 2026 Centroid ehf.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * The CentroidX on-screen keyboard's layout tables: every page, every
 * language, and the long-press alternates.
 *
 * Kept apart from keyboard.c so the tables can be compiled on their own,
 * without weston's toolkit, by test/layouts-check.c. The grid arithmetic is
 * the thing that goes wrong here: redraw_handler() and lookup_key() both
 * walk the keys wrapping at `columns`, so a row that is a unit short or long
 * silently shifts every key after it. The check catches that in CI.
 *
 * Languages follow Gboard:
 *
 *   en  QWERTY. Accents by long-press only.
 *   pl  QWERTY, the same sheet as English. Polish letters are long-press
 *       alternates (ą on a, ł on l, ż on z, ...), first in their popups.
 *   is  QWERTY with the four Icelandic letters on the sheet: ð after p,
 *       æ and ö after l, þ after m. Acute vowels by long-press.
 *
 * Sources. The long-press sets are Android's own, as published in Unicode
 * CLDR's keyboards/android/{en,is,pl}-t-k0-android.xml and in AOSP
 * LatinIME's KeyboardTextsTable.java. Those describe Android 4.4's keyboard,
 * whose Icelandic sheet is still plain QWERTY; the Icelandic row
 * arrangement is the one modern Gboard-style keyboards ship (FUTO Keyboard's
 * LatinScript/icelandic.yaml and FlorisBoard's characters/icelandic.json
 * agree key for key), and matches the Icelandic hardware layout.
 */

#ifndef CENTROIDX_KEYBOARD_LAYOUTS_H
#define CENTROIDX_KEYBOARD_LAYOUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <xkbcommon/xkbcommon.h>

enum keyboard_state {
	KEYBOARD_STATE_DEFAULT,
	KEYBOARD_STATE_UPPERCASE,	/* one-shot shift */
	KEYBOARD_STATE_LOCKED,		/* caps lock */
	KEYBOARD_STATE_SYMBOLS,		/* ?123: digits and everyday punctuation */
	KEYBOARD_STATE_SYMBOLS2		/* =\<: currency, brackets and maths */
};

enum key_type {
	keytype_default,
	keytype_backspace,
	keytype_enter,
	keytype_space,
	keytype_switch,
	/* switches to the page named on the key itself */
	keytype_page,
	keytype_spacer,
	/* taps the keysym carried on the key itself: cursor movement, Home,
	 * End, Delete. Drawn as an icon when the label is empty, as text
	 * otherwise. */
	keytype_arrow,
	/* the globe: cycles through the enabled languages. Zero width, and
	 * so neither drawn nor hit, when only one language is enabled. */
	keytype_lang
};

struct key {
	enum key_type key_type;

	const char *label;
	/* shifted label; NULL on keys that do not shift — the symbol pages
	 * carry no shift key, so their keys leave it unset */
	const char *uppercase;

	/* width in layout grid units */
	unsigned int width;

	/* small corner hint, the first entry of the long-press popup (NULL
	 * for none) */
	const char *hint;

	/* the keysym this key taps; set for keytype_arrow and
	 * keytype_backspace, which share the tap-and-repeat path */
	xkb_keysym_t keysym;

	/* keytype_page: the page this key switches to */
	enum keyboard_state page;

	/* NULL-terminated long-press alternates after the hint, lowercase and
	 * shifted. Both or neither; the same length. Each entry is a single
	 * character, because the embedder commits only the first UTF-16 unit
	 * of a commit_string. */
	const char *const *more;
	const char *const *more_upper;
};

struct layout {
	const struct key *keys;
	uint32_t count;

	uint32_t columns;
	uint32_t rows;

	/* pixel width of one grid unit */
	double unit;
	/* pixel height of one row; rows * row_h must be equal for all
	 * layouts (weston keeps the initial input-panel surface size) */
	double row_h;

	/* horizontal inset of the visible sheet; the strip outside it is
	 * drawn fully transparent (used to make the numpad look narrow
	 * while the panel surface keeps the shared footprint) */
	double sheet_inset;
};

/*
 * Every layout must come out the same size: window_schedule_resize() is
 * driven by columns * unit, but weston keeps whichever input-panel
 * surface was created first, so a layout that disagrees gets clipped or
 * letterboxed rather than resizing the panel.
 */
#define PANEL_WIDTH 900.0

/* the most cells a long-press popup holds: the hint plus the alternates */
#define POPUP_MAX 12

/* width the globe takes out of the space bar when it is shown */
#define GLOBE_WIDTH 2

#define KEY_COUNT(keys) (sizeof(keys) / sizeof(*(keys)))

/* a NULL-terminated list of long-press alternates */
#define MORE(...) ((const char *const[]){ __VA_ARGS__, NULL })

/*
 * English alpha layout: 26 grid units of PANEL_WIDTH/26 px, 4 rows of 50 px.
 * The typing block is the stock 20 units; the 6 units on the right are
 * the navigation cluster. Letter keys are 2 units; the home row is
 * staggered with 1-unit spacers like Gboard. The top row carries digit
 * hints typed by long-press; the ?123 page has the digits full-size.
 *
 * The globe sits between ?123 and the comma, where Gboard keeps it, and
 * comes out of the space bar's width. Not const: languages_set_globe()
 * folds it back into the space bar when only one language is enabled.
 */
static struct key en_keys[] = {
	{ keytype_default, "q", "Q", 2, "1"},
	{ keytype_default, "w", "W", 2, "2"},
	{ keytype_default, "e", "E", 2, "3",
	  .more = MORE("è", "é", "ê", "ë", "ē"),
	  .more_upper = MORE("È", "É", "Ê", "Ë", "Ē")},
	{ keytype_default, "r", "R", 2, "4"},
	{ keytype_default, "t", "T", 2, "5"},
	{ keytype_default, "y", "Y", 2, "6"},
	{ keytype_default, "u", "U", 2, "7",
	  .more = MORE("û", "ü", "ù", "ú", "ū"),
	  .more_upper = MORE("Û", "Ü", "Ù", "Ú", "Ū")},
	{ keytype_default, "i", "I", 2, "8",
	  .more = MORE("î", "ï", "í", "ī", "ì"),
	  .more_upper = MORE("Î", "Ï", "Í", "Ī", "Ì")},
	{ keytype_default, "o", "O", 2, "9",
	  .more = MORE("ô", "ö", "ò", "ó", "œ", "ø", "ō", "õ"),
	  .more_upper = MORE("Ô", "Ö", "Ò", "Ó", "Œ", "Ø", "Ō", "Õ")},
	{ keytype_default, "p", "P", 2, "0"},
	{ keytype_spacer, "", "", 6},

	{ keytype_spacer, "", "", 1},
	{ keytype_default, "a", "A", 2,
	  .more = MORE("à", "á", "â", "ä", "æ", "ã", "å", "ā"),
	  .more_upper = MORE("À", "Á", "Â", "Ä", "Æ", "Ã", "Å", "Ā")},
	/* Android offers "SS" for a shifted ß; ẞ is the one-character
	 * capital, and a single character is all the embedder commits */
	{ keytype_default, "s", "S", 2,
	  .more = MORE("ß"), .more_upper = MORE("ẞ")},
	{ keytype_default, "d", "D", 2},
	{ keytype_default, "f", "F", 2},
	{ keytype_default, "g", "G", 2},
	{ keytype_default, "h", "H", 2},
	{ keytype_default, "j", "J", 2},
	{ keytype_default, "k", "K", 2},
	{ keytype_default, "l", "L", 2},
	{ keytype_spacer, "", "", 1},
	{ keytype_spacer, "", "", 6},

	{ keytype_switch, "", "", 3},
	{ keytype_default, "z", "Z", 2},
	{ keytype_default, "x", "X", 2},
	{ keytype_default, "c", "C", 2,
	  .more = MORE("ç"), .more_upper = MORE("Ç")},
	{ keytype_default, "v", "V", 2},
	{ keytype_default, "b", "B", 2},
	{ keytype_default, "n", "N", 2,
	  .more = MORE("ñ"), .more_upper = MORE("Ñ")},
	{ keytype_default, "m", "M", 2},
	{ keytype_backspace, "", "", 3, NULL, XKB_KEY_BackSpace},
	{ keytype_arrow, "Home", "Home", 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", "End", 2, NULL, XKB_KEY_End},

	{ keytype_page, "?123", NULL, 3, NULL, 0, KEYBOARD_STATE_SYMBOLS},
	{ keytype_lang, "", "", GLOBE_WIDTH},
	{ keytype_default, ",", ",", 2},
	{ keytype_space, "", "", 8},
	{ keytype_default, ".", ".", 2},
	{ keytype_enter, "", "", 3},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Right}
};

/*
 * Polish: Gboard gives Polish the English sheet unchanged. The nine Polish
 * letters are the first long-press alternate of their base letter.
 */
static struct key pl_keys[] = {
	{ keytype_default, "q", "Q", 2, "1"},
	{ keytype_default, "w", "W", 2, "2"},
	{ keytype_default, "e", "E", 2, "3",
	  .more = MORE("ę", "è", "é", "ê", "ë", "ė", "ē"),
	  .more_upper = MORE("Ę", "È", "É", "Ê", "Ë", "Ė", "Ē")},
	{ keytype_default, "r", "R", 2, "4"},
	{ keytype_default, "t", "T", 2, "5"},
	{ keytype_default, "y", "Y", 2, "6"},
	{ keytype_default, "u", "U", 2, "7"},
	{ keytype_default, "i", "I", 2, "8"},
	{ keytype_default, "o", "O", 2, "9",
	  .more = MORE("ó", "ö", "ô", "ò", "õ", "œ", "ø", "ō"),
	  .more_upper = MORE("Ó", "Ö", "Ô", "Ò", "Õ", "Œ", "Ø", "Ō")},
	{ keytype_default, "p", "P", 2, "0"},
	{ keytype_spacer, "", "", 6},

	{ keytype_spacer, "", "", 1},
	{ keytype_default, "a", "A", 2,
	  .more = MORE("ą", "á", "à", "â", "ä", "æ", "ã", "å", "ā"),
	  .more_upper = MORE("Ą", "Á", "À", "Â", "Ä", "Æ", "Ã", "Å", "Ā")},
	{ keytype_default, "s", "S", 2,
	  .more = MORE("ś", "ß", "š"), .more_upper = MORE("Ś", "ẞ", "Š")},
	{ keytype_default, "d", "D", 2},
	{ keytype_default, "f", "F", 2},
	{ keytype_default, "g", "G", 2},
	{ keytype_default, "h", "H", 2},
	{ keytype_default, "j", "J", 2},
	{ keytype_default, "k", "K", 2},
	{ keytype_default, "l", "L", 2,
	  .more = MORE("ł"), .more_upper = MORE("Ł")},
	{ keytype_spacer, "", "", 1},
	{ keytype_spacer, "", "", 6},

	{ keytype_switch, "", "", 3},
	{ keytype_default, "z", "Z", 2,
	  .more = MORE("ż", "ź", "ž"), .more_upper = MORE("Ż", "Ź", "Ž")},
	{ keytype_default, "x", "X", 2},
	{ keytype_default, "c", "C", 2,
	  .more = MORE("ć", "ç", "č"), .more_upper = MORE("Ć", "Ç", "Č")},
	{ keytype_default, "v", "V", 2},
	{ keytype_default, "b", "B", 2},
	{ keytype_default, "n", "N", 2,
	  .more = MORE("ń", "ñ"), .more_upper = MORE("Ń", "Ñ")},
	{ keytype_default, "m", "M", 2},
	{ keytype_backspace, "", "", 3, NULL, XKB_KEY_BackSpace},
	{ keytype_arrow, "Home", "Home", 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", "End", 2, NULL, XKB_KEY_End},

	{ keytype_page, "?123", NULL, 3, NULL, 0, KEYBOARD_STATE_SYMBOLS},
	{ keytype_lang, "", "", GLOBE_WIDTH},
	{ keytype_default, ",", ",", 2},
	{ keytype_space, "", "", 8},
	{ keytype_default, ".", ".", 2},
	{ keytype_enter, "", "", 3},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Right}
};

/*
 * Icelandic: 28 grid units of PANEL_WIDTH/28 px. The two extra units hold
 * the eleventh key of the top and home rows — ð after p, æ and ö after l —
 * so the home row loses the stagger, as it does on Gboard's Nordic sheets;
 * þ takes the eighth slot of the bottom letter row. The navigation cluster
 * keeps its 6 units, which at this unit is 15 px narrower than on the
 * 26-unit pages.
 */
static struct key is_keys[] = {
	{ keytype_default, "q", "Q", 2, "1"},
	{ keytype_default, "w", "W", 2, "2"},
	{ keytype_default, "e", "E", 2, "3",
	  .more = MORE("é", "ë", "è", "ê", "ę", "ė", "ē"),
	  .more_upper = MORE("É", "Ë", "È", "Ê", "Ę", "Ė", "Ē")},
	{ keytype_default, "r", "R", 2, "4"},
	{ keytype_default, "t", "T", 2, "5",
	  .more = MORE("þ"), .more_upper = MORE("Þ")},
	{ keytype_default, "y", "Y", 2, "6",
	  .more = MORE("ý", "ÿ"), .more_upper = MORE("Ý", "Ÿ")},
	{ keytype_default, "u", "U", 2, "7",
	  .more = MORE("ú", "ü", "û", "ù", "ū"),
	  .more_upper = MORE("Ú", "Ü", "Û", "Ù", "Ū")},
	{ keytype_default, "i", "I", 2, "8",
	  .more = MORE("í", "ï", "î", "ì", "į", "ī"),
	  .more_upper = MORE("Í", "Ï", "Î", "Ì", "Į", "Ī")},
	{ keytype_default, "o", "O", 2, "9",
	  .more = MORE("ó", "ö", "ô", "ò", "õ", "œ", "ø", "ō"),
	  .more_upper = MORE("Ó", "Ö", "Ô", "Ò", "Õ", "Œ", "Ø", "Ō")},
	{ keytype_default, "p", "P", 2, "0"},
	{ keytype_default, "ð", "Ð", 2},
	{ keytype_spacer, "", "", 6},

	{ keytype_default, "a", "A", 2,
	  .more = MORE("á", "ä", "æ", "å", "à", "â", "ã", "ā"),
	  .more_upper = MORE("Á", "Ä", "Æ", "Å", "À", "Â", "Ã", "Ā")},
	{ keytype_default, "s", "S", 2},
	{ keytype_default, "d", "D", 2,
	  .more = MORE("ð"), .more_upper = MORE("Ð")},
	{ keytype_default, "f", "F", 2},
	{ keytype_default, "g", "G", 2},
	{ keytype_default, "h", "H", 2},
	{ keytype_default, "j", "J", 2},
	{ keytype_default, "k", "K", 2},
	{ keytype_default, "l", "L", 2},
	{ keytype_default, "æ", "Æ", 2},
	{ keytype_default, "ö", "Ö", 2},
	{ keytype_spacer, "", "", 6},

	{ keytype_switch, "", "", 3},
	{ keytype_default, "z", "Z", 2},
	{ keytype_default, "x", "X", 2},
	{ keytype_default, "c", "C", 2},
	{ keytype_default, "v", "V", 2},
	{ keytype_default, "b", "B", 2},
	{ keytype_default, "n", "N", 2},
	{ keytype_default, "m", "M", 2},
	{ keytype_default, "þ", "Þ", 2},
	{ keytype_backspace, "", "", 3, NULL, XKB_KEY_BackSpace},
	{ keytype_arrow, "Home", "Home", 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", "End", 2, NULL, XKB_KEY_End},

	{ keytype_page, "?123", NULL, 3, NULL, 0, KEYBOARD_STATE_SYMBOLS},
	{ keytype_lang, "", "", GLOBE_WIDTH},
	{ keytype_default, ",", ",", 2},
	{ keytype_space, "", "", 10},
	{ keytype_default, ".", ".", 2},
	{ keytype_enter, "", "", 3},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", "", 2, NULL, XKB_KEY_Right}
};

/*
 * Symbol page 1 (?123): Gboard's own arrangement — the digit row on top,
 * the punctuation most text fields want below it, and =\< in the shift
 * slot leading to page 2. Same 26-unit grid and navigation cluster as the
 * English alpha layout, so the panel footprint is unchanged. Shared by
 * every language, as on Gboard.
 */
static const struct key symbols_keys[] = {
	{ keytype_default, "1", NULL, 2},
	{ keytype_default, "2", NULL, 2},
	{ keytype_default, "3", NULL, 2},
	{ keytype_default, "4", NULL, 2},
	{ keytype_default, "5", NULL, 2},
	{ keytype_default, "6", NULL, 2},
	{ keytype_default, "7", NULL, 2},
	{ keytype_default, "8", NULL, 2},
	{ keytype_default, "9", NULL, 2},
	{ keytype_default, "0", NULL, 2},
	{ keytype_spacer, "", NULL, 6},

	{ keytype_default, "@", NULL, 2},
	{ keytype_default, "#", NULL, 2},
	{ keytype_default, "$", NULL, 2},
	{ keytype_default, "_", NULL, 2},
	{ keytype_default, "&", NULL, 2},
	{ keytype_default, "-", NULL, 2},
	{ keytype_default, "+", NULL, 2},
	{ keytype_default, "(", NULL, 2},
	{ keytype_default, ")", NULL, 2},
	{ keytype_default, "/", NULL, 2},
	{ keytype_spacer, "", NULL, 6},

	{ keytype_page, "=\\<", NULL, 3, NULL, 0, KEYBOARD_STATE_SYMBOLS2},
	{ keytype_default, "*", NULL, 2},
	{ keytype_default, "\"", NULL, 2},
	{ keytype_default, "'", NULL, 2},
	{ keytype_default, ":", NULL, 2},
	{ keytype_default, ";", NULL, 2},
	{ keytype_default, "!", NULL, 2},
	{ keytype_default, "?", NULL, 2},
	{ keytype_backspace, "", NULL, 3, NULL, XKB_KEY_BackSpace},
	{ keytype_arrow, "Home", NULL, 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", NULL, 2, NULL, XKB_KEY_End},

	{ keytype_page, "ABC", NULL, 3, NULL, 0, KEYBOARD_STATE_DEFAULT},
	{ keytype_default, ",", NULL, 2},
	{ keytype_space, "", NULL, 10},
	{ keytype_default, ".", NULL, 2},
	{ keytype_enter, "", NULL, 3},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Right}
};

/*
 * Symbol page 2 (=\<): the rest of Gboard's set — maths, currency,
 * brackets and the legal marks. The nine-key currency row is inset by a
 * unit on each side, the same stagger the alpha home row uses. ?123 in
 * the shift slot goes back to page 1; ABC goes back to letters.
 *
 * One deviation from Gboard: % takes the slot Gboard gives ℅. Percent is
 * everywhere on this HMI (and was reachable before the pages split),
 * care-of is never; the two are near-indistinguishable at key size.
 */
static const struct key symbols2_keys[] = {
	{ keytype_default, "~", NULL, 2},
	{ keytype_default, "`", NULL, 2},
	{ keytype_default, "|", NULL, 2},
	{ keytype_default, "•", NULL, 2},
	{ keytype_default, "√", NULL, 2},
	{ keytype_default, "π", NULL, 2},
	{ keytype_default, "÷", NULL, 2},
	{ keytype_default, "×", NULL, 2},
	{ keytype_default, "¶", NULL, 2},
	{ keytype_default, "∆", NULL, 2},
	{ keytype_spacer, "", NULL, 6},

	{ keytype_spacer, "", NULL, 1},
	{ keytype_default, "£", NULL, 2},
	{ keytype_default, "¢", NULL, 2},
	{ keytype_default, "€", NULL, 2},
	{ keytype_default, "¥", NULL, 2},
	{ keytype_default, "^", NULL, 2},
	{ keytype_default, "°", NULL, 2},
	{ keytype_default, "=", NULL, 2},
	{ keytype_default, "{", NULL, 2},
	{ keytype_default, "}", NULL, 2},
	{ keytype_spacer, "", NULL, 7},

	{ keytype_page, "?123", NULL, 3, NULL, 0, KEYBOARD_STATE_SYMBOLS},
	{ keytype_default, "\\", NULL, 2},
	{ keytype_default, "©", NULL, 2},
	{ keytype_default, "®", NULL, 2},
	{ keytype_default, "™", NULL, 2},
	{ keytype_default, "%", NULL, 2},
	{ keytype_default, "[", NULL, 2},
	{ keytype_default, "]", NULL, 2},
	{ keytype_backspace, "", NULL, 3, NULL, XKB_KEY_BackSpace},
	{ keytype_arrow, "Home", NULL, 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", NULL, 2, NULL, XKB_KEY_End},

	{ keytype_page, "ABC", NULL, 3, NULL, 0, KEYBOARD_STATE_DEFAULT},
	{ keytype_default, ",", NULL, 2},
	{ keytype_default, "<", NULL, 2},
	{ keytype_space, "", NULL, 6},
	{ keytype_default, ">", NULL, 2},
	{ keytype_default, ".", NULL, 2},
	{ keytype_enter, "", NULL, 3},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Right}
};

/*
 * Numeric keypad: 14 grid units of PANEL_WIDTH/14 px, 4 rows — the same
 * panel footprint as the alpha layout. The keypad is the leftmost 8
 * units, flush with the edge, and the navigation cluster takes the 6 on
 * the right. Phone-style digit order; backspace / minus / delete down the
 * keypad's right; double-width 0, decimal point, Enter along the bottom.
 *
 * No plus key: a leading + is not part of any numeric-field convention
 * (HTML's valid floating-point number does not permit one, and neither
 * iOS's decimal pad nor Android's numberDecimal offers the key), and the
 * HMI's own filters strip it, so it could only ever look broken.
 */
static const struct key numeric_keys[] = {
	{ keytype_default, "1", NULL, 2},
	{ keytype_default, "2", NULL, 2},
	{ keytype_default, "3", NULL, 2},
	{ keytype_backspace, "", NULL, 2, NULL, XKB_KEY_BackSpace},
	{ keytype_spacer, "", NULL, 6},

	{ keytype_default, "4", NULL, 2},
	{ keytype_default, "5", NULL, 2},
	{ keytype_default, "6", NULL, 2},
	{ keytype_default, "-", NULL, 2},
	{ keytype_spacer, "", NULL, 6},

	{ keytype_default, "7", NULL, 2},
	{ keytype_default, "8", NULL, 2},
	{ keytype_default, "9", NULL, 2},
	{ keytype_arrow, "Del", NULL, 2, NULL, XKB_KEY_Delete},
	{ keytype_arrow, "Home", NULL, 2, NULL, XKB_KEY_Home},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Up},
	{ keytype_arrow, "End", NULL, 2, NULL, XKB_KEY_End},

	{ keytype_default, "0", NULL, 4},
	{ keytype_default, ".", NULL, 2},
	{ keytype_enter, "", NULL, 2},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Left},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Down},
	{ keytype_arrow, "", NULL, 2, NULL, XKB_KEY_Right}
};

static const struct layout en_layout = {
	en_keys, KEY_COUNT(en_keys), 26, 4, PANEL_WIDTH / 26, 50, 0
};

static const struct layout pl_layout = {
	pl_keys, KEY_COUNT(pl_keys), 26, 4, PANEL_WIDTH / 26, 50, 0
};

static const struct layout is_layout = {
	is_keys, KEY_COUNT(is_keys), 28, 4, PANEL_WIDTH / 28, 50, 0
};

static const struct layout symbols_layout = {
	symbols_keys, KEY_COUNT(symbols_keys), 26, 4, PANEL_WIDTH / 26, 50, 0
};

static const struct layout symbols2_layout = {
	symbols2_keys, KEY_COUNT(symbols2_keys), 26, 4, PANEL_WIDTH / 26, 50, 0
};

static const struct layout numeric_layout = {
	numeric_keys, KEY_COUNT(numeric_keys), 14, 4, PANEL_WIDTH / 14, 50, 0
};

struct language {
	/* the KEYBOARD_LAYOUTS token, and the language the text-input
	 * client is told about */
	const char *code;
	/* what the space bar shows, in the language itself, as on Gboard */
	const char *name;
	/* the letters page; the symbol pages and keypad are shared */
	const struct layout *alpha;
};

/* English first: it is the keyboard's fallback when KEYBOARD_LAYOUTS is
 * unset or names nothing it knows. */
static const struct language languages[] = {
	{ "en", "English", &en_layout },
	{ "is", "Íslenska", &is_layout },
	{ "pl", "Polski", &pl_layout },
};

#define LANGUAGE_COUNT KEY_COUNT(languages)

/* room for every language, with headroom for more to be added */
#define LANGUAGE_MAX 8

/*
 * Parses a KEYBOARD_LAYOUTS value — "is,en,pl", in order, first is the
 * default — into out[], returning how many languages it named. Tokens are
 * separated by commas or blanks and matched case-insensitively; "us" is
 * accepted for "en", since that is the xkb name. Repeats are dropped, and
 * so are unknown tokens, each reported through unknown() when it is not
 * NULL. A NULL or empty list yields 0, and the caller picks the fallback.
 */
static inline int
parse_keyboard_layouts(const char *list, const struct language **out, int max,
		       void (*unknown)(const char *token, size_t len))
{
	const char *p = list;
	int n = 0;

	if (!list)
		return 0;

	while (*p && n < max) {
		const struct language *found = NULL;
		const char *token;
		size_t len, i;
		int j;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		token = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		len = (size_t)(p - token);
		if (len == 0)
			continue;

		if (len == 2 && !strncasecmp(token, "us", 2))
			token = "en";

		for (i = 0; i < LANGUAGE_COUNT; i++) {
			const char *code = languages[i].code;

			if (strlen(code) == len &&
			    !strncasecmp(token, code, len)) {
				found = &languages[i];
				break;
			}
		}

		if (!found) {
			if (unknown)
				unknown(token, len);
			continue;
		}

		for (j = 0; j < n; j++)
			if (out[j] == found)
				break;
		if (j == n)
			out[n++] = found;
	}

	return n;
}

/* Shows or hides the globe on one letters page, handing its width to or
 * from the space bar that follows it. Idempotent. */
static inline void
keys_set_globe(struct key *keys, size_t count, bool show)
{
	struct key *globe = NULL, *space = NULL;
	size_t i;

	for (i = 0; i < count && !space; i++) {
		if (keys[i].key_type == keytype_lang)
			globe = &keys[i];
		else if (globe && keys[i].key_type == keytype_space)
			space = &keys[i];
	}

	if (!globe || !space)
		return;

	if (show && globe->width == 0) {
		globe->width = GLOBE_WIDTH;
		space->width -= GLOBE_WIDTH;
	} else if (!show && globe->width != 0) {
		space->width += globe->width;
		globe->width = 0;
	}
}

/* Gboard shows no globe to someone with one language: a key that does
 * nothing is worse than a wider space bar. */
static inline void
languages_set_globe(bool show)
{
	keys_set_globe(en_keys, KEY_COUNT(en_keys), show);
	keys_set_globe(is_keys, KEY_COUNT(is_keys), show);
	keys_set_globe(pl_keys, KEY_COUNT(pl_keys), show);
}

#endif /* CENTROIDX_KEYBOARD_LAYOUTS_H */
