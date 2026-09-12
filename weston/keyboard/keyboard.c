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
 * CentroidX on-screen keyboard: a restyled fork of weston-keyboard.
 *
 * Same input-method-v1 / input-panel-v1 plumbing as the stock client, with
 * a Gboard-like look and feel:
 *  - opaque light sheet, rounded flat keys, staggered home row,
 *    accent-colored Enter, pressed-key highlight, cairo-drawn icons
 *  - three pages like Gboard: letters, ?123 and =\<, reached by the
 *    key in the bottom-left and (between the symbol pages) the key in
 *    the shift slot
 *  - English, Icelandic and Polish letters pages, enabled and ordered by
 *    KEYBOARD_LAYOUTS (first is the default); with more than one, a globe
 *    key cycles them and the space bar names the one showing
 *  - long-press popups: the digit hint on the top row, then the accented
 *    letters, picked by sliding along the strip and typed on release
 *  - auto-repeat on hold for backspace and the navigation keys
 *  - one-shot shift, double-tap for caps lock
 *  - a real numeric keypad (decimal point, minus, delete) for
 *    digits/number content purposes
 *  - a navigation cluster on every layout: Home/Up/End over
 *    Left/Down/Right, laid out like a physical keyboard
 *  - direct commit_string typing (no preedit)
 *
 * The layout tables live in keyboard-layouts.h.
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <linux/input.h>
#include <cairo.h>

#include "window.h"
#include "input-method-unstable-v1-client-protocol.h"
#include "text-input-unstable-v1-client-protocol.h"
#include "shared/helpers.h"
#include "shared/xalloc.h"

#include "keyboard-layouts.h"

/* long-press delay for popups, auto-repeat delay/rate */
#define LONGPRESS_USEC (500 * 1000)
#define REPEAT_DELAY_MSEC 500
#define REPEAT_RATE_MSEC 60
/* double-tap window for caps lock */
#define CAPS_DOUBLE_TAP_MSEC 350
/* corner radius of the panel sheet's top edges */
#define SHEET_RADIUS 10

struct keyboard;

struct virtual_keyboard {
	struct zwp_input_panel_v1 *input_panel;
	struct zwp_input_method_v1 *input_method;
	struct zwp_input_method_context_v1 *context;
	struct display *display;
	struct output *output;
	struct {
		xkb_mod_mask_t shift_mask;
	} keysym;
	uint32_t serial;
	uint32_t content_hint;
	uint32_t content_purpose;
	char *preferred_language;
	char *surrounding_text;
	uint32_t surrounding_cursor;
	struct keyboard *keyboard;
	bool toplevel;
	bool overlay;
	struct zwp_input_panel_surface_v1 *ips;

	/* the languages KEYBOARD_LAYOUTS enables, default first, and the one
	 * the letters page shows. Kept across activations, the way Gboard
	 * remembers the last language used. */
	const struct language *languages[LANGUAGE_MAX];
	int language_count;
	int language_index;
};

/* Gboard-like palette */
#define COL(hex) \
	((hex) >> 16 & 0xff) / 255.0, \
	((hex) >> 8 & 0xff) / 255.0, \
	((hex) & 0xff) / 255.0

static const uint32_t color_sheet       = 0xf1f3f4;
static const uint32_t color_key         = 0xffffff;
static const uint32_t color_key_special = 0xdadce0;
static const uint32_t color_key_pressed = 0xc6c9ce;
static const uint32_t color_accent      = 0x1a73e8;
static const uint32_t color_accent_down = 0x1765cc;
static const uint32_t color_text        = 0x202124;
static const uint32_t color_icon        = 0x3c4043;
static const uint32_t color_hint        = 0x80868b;

struct keyboard {
	struct virtual_keyboard *keyboard;
	struct window *window;
	struct widget *widget;

	enum keyboard_state state;

	/* the key currently held down (commit happens on release) */
	const struct key *held_key;
	uint32_t held_time;
	uint32_t last_shift_time;

	/* the long-press strip over the held key, open from the long-press
	 * timeout until release. Geometry in widget-local pixels. */
	struct {
		bool open;
		const char *entries[POPUP_MAX];
		unsigned int count;
		unsigned int selected;
		double x, y;
		double cell_w;
	} popup;

	struct toytimer longpress_timer;
	struct toytimer repeat_timer;
};

static const struct layout *
get_current_layout(struct virtual_keyboard *keyboard);

static const struct language *
current_language(struct virtual_keyboard *keyboard)
{
	return keyboard->languages[keyboard->language_index];
}

static bool
keyboard_shifted(struct keyboard *keyboard)
{
	return keyboard->state == KEYBOARD_STATE_UPPERCASE ||
	       keyboard->state == KEYBOARD_STATE_LOCKED;
}

static const char *
label_from_key(struct keyboard *keyboard,
	       const struct key *key)
{
	if (keyboard_shifted(keyboard) && key->uppercase)
		return key->uppercase;

	return key->label;
}

/* Codepoints, not bytes: the symbol pages are full of multi-byte
 * labels that are still a single character on the key. */
static size_t
utf8_length(const char *s)
{
	size_t n = 0;

	for (; *s; s++)
		if ((*s & 0xc0) != 0x80)
			n++;

	return n;
}

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* Draws text centred on (cx, cy), the way every key label is placed. */
static void
show_centered(cairo_t *cr, const char *text, double cx, double cy)
{
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;

	cairo_text_extents(cr, text, &extents);
	cairo_font_extents(cr, &font_extents);
	cairo_move_to(cr,
		      cx - extents.width / 2 - extents.x_bearing,
		      cy + font_extents.height / 2 - font_extents.descent);
	cairo_show_text(cr, text);
}

static void
draw_icon_shift(cairo_t *cr, double cx, double cy, bool filled, bool locked)
{
	/* upward arrow with a stem, ~22 px tall */
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy - 10);
	cairo_line_to(cr, cx + 9, cy);
	cairo_line_to(cr, cx + 4, cy);
	cairo_line_to(cr, cx + 4, cy + 7);
	cairo_line_to(cr, cx - 4, cy + 7);
	cairo_line_to(cr, cx - 4, cy);
	cairo_line_to(cr, cx - 9, cy);
	cairo_close_path(cr);
	if (filled) {
		cairo_fill(cr);
	} else {
		cairo_set_line_width(cr, 1.8);
		cairo_stroke(cr);
	}

	if (locked) {
		cairo_set_line_width(cr, 2.0);
		cairo_move_to(cr, cx - 4, cy + 11);
		cairo_line_to(cr, cx + 4, cy + 11);
		cairo_stroke(cr);
	}
}

static void
draw_icon_backspace(cairo_t *cr, double cx, double cy)
{
	/* left-pointing tag outline with an x inside */
	cairo_set_line_width(cr, 1.8);
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 10, cy);
	cairo_line_to(cr, cx - 3, cy - 7);
	cairo_line_to(cr, cx + 10, cy - 7);
	cairo_line_to(cr, cx + 10, cy + 7);
	cairo_line_to(cr, cx - 3, cy + 7);
	cairo_close_path(cr);
	cairo_stroke(cr);

	cairo_move_to(cr, cx - 0.5, cy - 3);
	cairo_line_to(cr, cx + 5.5, cy + 3);
	cairo_move_to(cr, cx + 5.5, cy - 3);
	cairo_line_to(cr, cx - 0.5, cy + 3);
	cairo_stroke(cr);
}

static void
draw_icon_enter(cairo_t *cr, double cx, double cy)
{
	/* return arrow: down, then left, with arrowhead */
	cairo_set_line_width(cr, 2.0);
	cairo_new_path(cr);
	cairo_move_to(cr, cx + 8, cy - 8);
	cairo_line_to(cr, cx + 8, cy + 3);
	cairo_line_to(cr, cx - 6, cy + 3);
	cairo_stroke(cr);

	cairo_new_path(cr);
	cairo_move_to(cr, cx - 2, cy - 2);
	cairo_line_to(cr, cx - 8, cy + 3);
	cairo_line_to(cr, cx - 2, cy + 8);
	cairo_stroke(cr);
}

static void
draw_icon_globe(cairo_t *cr, double cx, double cy)
{
	/* Gboard's language key: a circle with a meridian and three
	 * parallels, stroked at the backspace icon's weight */
	cairo_set_line_width(cr, 1.6);

	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
	cairo_stroke(cr);

	/* the meridian is the same circle squeezed horizontally; the scale
	 * is undone before stroking so the line keeps its width */
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, 0.45, 1);
	cairo_new_path(cr);
	cairo_arc(cr, 0, 0, 8, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_stroke(cr);

	cairo_new_path(cr);
	cairo_move_to(cr, cx - 8, cy);
	cairo_line_to(cr, cx + 8, cy);
	/* chords at y = +-4 of a radius-8 circle are 6.9 either side */
	cairo_move_to(cr, cx - 6.9, cy - 4);
	cairo_line_to(cr, cx + 6.9, cy - 4);
	cairo_move_to(cr, cx - 6.9, cy + 4);
	cairo_line_to(cr, cx + 6.9, cy + 4);
	cairo_stroke(cr);
}

static void
draw_icon_arrow(cairo_t *cr, double cx, double cy, xkb_keysym_t sym)
{
	/* stemmed arrow with a chevron head, stroked to sit alongside the
	 * backspace and enter icons. dx/dy is the direction it points. */
	double dx = 0, dy = 0;

	switch (sym) {
	case XKB_KEY_Left:
		dx = -1;
		break;
	case XKB_KEY_Right:
		dx = 1;
		break;
	case XKB_KEY_Up:
		dy = -1;
		break;
	default:
		dy = 1;
		break;
	}

	cairo_set_line_width(cr, 2.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

	cairo_new_path(cr);
	cairo_move_to(cr, cx - dx * 8, cy - dy * 8);
	cairo_line_to(cr, cx + dx * 8, cy + dy * 8);
	cairo_stroke(cr);

	/* head: back off the tip by 6 along the axis, 6 to either side */
	cairo_new_path(cr);
	cairo_move_to(cr, cx + dx * 2 - dy * 6, cy + dy * 2 - dx * 6);
	cairo_line_to(cr, cx + dx * 8, cy + dy * 8);
	cairo_line_to(cr, cx + dx * 2 + dy * 6, cy + dy * 2 + dx * 6);
	cairo_stroke(cr);
}

static void
draw_key(struct keyboard *keyboard,
	 const struct key *key,
	 cairo_t *cr,
	 unsigned int row,
	 unsigned int col)
{
	struct virtual_keyboard *vk = keyboard->keyboard;
	const struct layout *layout = get_current_layout(vk);
	const char *label;
	cairo_text_extents_t extents;
	double x, y, w, h, cx, cy;
	bool pressed, accent;
	const double gap_x = 3.5, gap_y = 4.5, radius = 6;

	/* a zero-width key is the globe with one language enabled */
	if (key->key_type == keytype_spacer || key->width == 0)
		return;

	x = col * layout->unit + gap_x;
	y = row * layout->row_h + gap_y;
	w = key->width * layout->unit - 2 * gap_x;
	h = layout->row_h - 2 * gap_y;
	cx = x + w / 2;
	cy = y + h / 2;

	pressed = keyboard->held_key == key;
	accent = key->key_type == keytype_enter;

	cairo_save(cr);

	/* soft bottom shadow */
	if (!pressed) {
		rounded_rect(cr, x, y + 1.5, w, h, radius);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.14);
		cairo_fill(cr);
	}

	/* key body */
	rounded_rect(cr, x, y, w, h, radius);
	if (accent) {
		cairo_set_source_rgb(cr, COL(pressed ? color_accent_down
						     : color_accent));
	} else if (pressed) {
		cairo_set_source_rgb(cr, COL(color_key_pressed));
	} else {
		switch (key->key_type) {
		case keytype_default:
		case keytype_space:
			cairo_set_source_rgb(cr, COL(color_key));
			break;
		default:
			cairo_set_source_rgb(cr, COL(color_key_special));
			break;
		}
	}
	cairo_fill(cr);

	/* content */
	if (accent)
		cairo_set_source_rgb(cr, 1, 1, 1);
	else if (key->key_type == keytype_default ||
		 key->key_type == keytype_space)
		cairo_set_source_rgb(cr, COL(color_text));
	else
		cairo_set_source_rgb(cr, COL(color_icon));

	switch (key->key_type) {
	case keytype_switch:
		draw_icon_shift(cr, cx, cy, keyboard_shifted(keyboard),
				keyboard->state == KEYBOARD_STATE_LOCKED);
		break;
	case keytype_backspace:
		draw_icon_backspace(cr, cx, cy);
		break;
	case keytype_enter:
		draw_icon_enter(cr, cx, cy);
		break;
	case keytype_lang:
		draw_icon_globe(cr, cx, cy);
		break;
	case keytype_space:
		/* Gboard names the language on the space bar, and only when
		 * there is more than one to tell apart */
		if (vk->language_count > 1 &&
		    layout == current_language(vk)->alpha) {
			cairo_set_source_rgb(cr, COL(color_hint));
			cairo_set_font_size(cr, 13);
			show_centered(cr, current_language(vk)->name, cx, cy);
		}
		break;
	case keytype_spacer:
		break;
	case keytype_arrow:
		if (!key->label[0]) {
			draw_icon_arrow(cr, cx, cy, key->keysym);
			break;
		}
		/* labelled nav keys (Home/End) fall through and draw as
		 * text; their three state labels are identical, so
		 * label_from_key() is stable across shift. */
		/* fallthrough */
	default:
		label = label_from_key(keyboard, key);
		cairo_set_font_size(cr, utf8_length(label) > 1 ? 14 : 19);
		show_centered(cr, label, cx, cy);

		/* digit hint in the top-right corner */
		if (key->hint) {
			cairo_set_source_rgb(cr, COL(color_hint));
			cairo_set_font_size(cr, 11);
			cairo_text_extents(cr, key->hint, &extents);
			cairo_move_to(cr,
				      x + w - extents.width - 7,
				      y + 15);
			cairo_show_text(cr, key->hint);
		}
		break;
	}

	cairo_restore(cr);
}

/* The long-press strip: a raised white bar of key-sized cells, the
 * selected one filled with the accent color like Gboard's. */
static void
draw_popup(struct keyboard *keyboard, cairo_t *cr,
	   const struct layout *layout)
{
	const double pad = 3.5, radius = 8;
	double x = keyboard->popup.x;
	double y = keyboard->popup.y;
	double cw = keyboard->popup.cell_w;
	double w = keyboard->popup.count * cw;
	double h = layout->row_h;
	unsigned int i;

	cairo_save(cr);

	rounded_rect(cr, x, y + 2, w, h, radius);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
	cairo_fill(cr);

	rounded_rect(cr, x, y, w, h, radius);
	cairo_set_source_rgb(cr, COL(color_key));
	cairo_fill(cr);

	cairo_set_font_size(cr, 19);

	for (i = 0; i < keyboard->popup.count; i++) {
		double cell_x = x + i * cw;

		if (i == keyboard->popup.selected) {
			rounded_rect(cr, cell_x + pad, y + pad,
				     cw - 2 * pad, h - 2 * pad, 6);
			cairo_set_source_rgb(cr, COL(color_accent));
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
		} else {
			cairo_set_source_rgb(cr, COL(color_text));
		}

		show_centered(cr, keyboard->popup.entries[i],
			      cell_x + cw / 2, y + h / 2);
	}

	cairo_restore(cr);
}

static const struct layout *
get_current_layout(struct virtual_keyboard *keyboard)
{
	switch (keyboard->content_purpose) {
		case ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DIGITS:
		case ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NUMBER:
			/* the keypad has every digit and sign a numeric
			 * field takes, so it has no symbol pages */
			return &numeric_layout;
		default:
			break;
	}

	switch (keyboard->keyboard->state) {
		case KEYBOARD_STATE_SYMBOLS:
			return &symbols_layout;
		case KEYBOARD_STATE_SYMBOLS2:
			return &symbols2_layout;
		default:
			return current_language(keyboard)->alpha;
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct keyboard *keyboard = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;
	unsigned int i;
	unsigned int row = 0, col = 0;
	const struct layout *layout;

	layout = get_current_layout(keyboard->keyboard);

	surface = window_get_surface(keyboard->window);
	widget_get_allocation(keyboard->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);

	cairo_translate(cr, allocation.x, allocation.y);

	/* the sheet: rounded top corners, flush with the screen edge at the
	 * bottom, and anything outside it (the numpad's side strips) left
	 * fully transparent */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	{
		double w = layout->columns * layout->unit;
		double h = layout->rows * layout->row_h;
		double r = SHEET_RADIUS;
		double x = layout->sheet_inset;

		cairo_set_source_rgba(cr, 0, 0, 0, 0);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_fill(cr);

		cairo_set_source_rgb(cr, COL(color_sheet));
		cairo_new_sub_path(cr);
		cairo_arc(cr, w - x - r, r, r, -M_PI / 2, 0);
		cairo_line_to(cr, w - x, h);
		cairo_line_to(cr, x, h);
		cairo_arc(cr, x + r, r, r, M_PI, 3 * M_PI / 2);
		cairo_close_path(cr);
		cairo_fill(cr);
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (i = 0; i < layout->count; ++i) {
		draw_key(keyboard, &layout->keys[i], cr, row, col);
		col += layout->keys[i].width;
		if (col >= layout->columns) {
			row += 1;
			col = 0;
		}
	}

	/* last, so it sits over the keys it covers */
	if (keyboard->popup.open)
		draw_popup(keyboard, cr, layout);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
}

static void
commit_text(struct virtual_keyboard *keyboard, const char *text)
{
	if (!keyboard->context)
		return;

	zwp_input_method_context_v1_cursor_position(keyboard->context, 0, 0);
	zwp_input_method_context_v1_commit_string(keyboard->context,
						  keyboard->serial,
						  text);
}

static void
send_keysym(struct virtual_keyboard *keyboard, uint32_t time,
	    xkb_keysym_t sym, uint32_t key_state)
{
	if (!keyboard->context)
		return;

	zwp_input_method_context_v1_keysym(keyboard->context,
					   display_get_serial(keyboard->display),
					   time, sym, key_state, 0);
}

static void
tap_keysym(struct virtual_keyboard *keyboard, uint32_t time, xkb_keysym_t sym)
{
	send_keysym(keyboard, time, sym, WL_KEYBOARD_KEY_STATE_PRESSED);
	send_keysym(keyboard, time, sym, WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Resizes the panel to the current page and tells the text-input client
 * which language it is typing in. */
static void
announce_layout(struct virtual_keyboard *keyboard)
{
	const struct layout *layout = get_current_layout(keyboard);

	window_schedule_resize(keyboard->keyboard->window,
			       layout->columns * layout->unit,
			       layout->rows * layout->row_h);

	if (keyboard->context) {
		zwp_input_method_context_v1_language(keyboard->context,
						     keyboard->serial,
						     current_language(keyboard)->code);
		zwp_input_method_context_v1_text_direction(keyboard->context,
							   keyboard->serial,
							   ZWP_TEXT_INPUT_V1_TEXT_DIRECTION_LTR);
	}

	widget_schedule_redraw(keyboard->keyboard->widget);
}

/* Where a key sits in its layout's grid; false when it is not in it. */
static bool
key_position(const struct layout *layout, const struct key *key,
	     unsigned int *row, unsigned int *col)
{
	unsigned int i, r = 0, c = 0;

	for (i = 0; i < layout->count; i++) {
		if (&layout->keys[i] == key) {
			*row = r;
			*col = c;
			return true;
		}
		c += layout->keys[i].width;
		if (c >= layout->columns) {
			r += 1;
			c = 0;
		}
	}

	return false;
}

/*
 * Opens the long-press strip over the held key: its hint first, so a
 * long-press released without sliding still types the digit as it always
 * has, then the accented letters in the key's current case.
 *
 * The strip starts over the key and runs right, pulled back left where it
 * would leave the sheet. It goes in the row above the key, as on Gboard;
 * for the top row, which has nothing above it inside the panel surface
 * (weston keeps the panel's first size, see PANEL_WIDTH), in the row below.
 */
static void
popup_open(struct keyboard *keyboard, const struct key *key)
{
	const struct layout *layout = get_current_layout(keyboard->keyboard);
	const char *const *more = key->more;
	double panel_w = layout->columns * layout->unit - layout->sheet_inset;
	unsigned int row, col, n = 0, i;
	double x;

	if (!key_position(layout, key, &row, &col))
		return;

	if (keyboard_shifted(keyboard) && key->more_upper)
		more = key->more_upper;

	if (key->hint)
		keyboard->popup.entries[n++] = key->hint;
	for (i = 0; more && more[i] && n < POPUP_MAX; i++)
		keyboard->popup.entries[n++] = more[i];

	if (n == 0)
		return;

	keyboard->popup.count = n;
	keyboard->popup.selected = 0;
	keyboard->popup.cell_w = key->width * layout->unit;

	x = col * layout->unit;
	if (x + n * keyboard->popup.cell_w > panel_w)
		x = panel_w - n * keyboard->popup.cell_w;
	if (x < layout->sheet_inset)
		x = layout->sheet_inset;
	keyboard->popup.x = x;
	keyboard->popup.y = (row > 0 ? row - 1 : row + 1) * layout->row_h;

	keyboard->popup.open = true;
}

/* Moves the selection to the cell under x, clamped to the strip's ends so
 * a finger that overshoots keeps the last entry. */
static void
popup_track(struct keyboard *keyboard, float x)
{
	int cell;

	if (!keyboard->popup.open)
		return;

	cell = (int)floor((x - keyboard->popup.x) / keyboard->popup.cell_w);
	if (cell < 0)
		cell = 0;
	if (cell >= (int)keyboard->popup.count)
		cell = (int)keyboard->popup.count - 1;

	if ((unsigned int)cell != keyboard->popup.selected) {
		keyboard->popup.selected = (unsigned int)cell;
		widget_schedule_redraw(keyboard->widget);
	}
}

static void
longpress_handler(struct toytimer *tt)
{
	struct keyboard *keyboard =
		container_of(tt, struct keyboard, longpress_timer);

	if (!keyboard->held_key)
		return;

	popup_open(keyboard, keyboard->held_key);
	widget_schedule_redraw(keyboard->widget);
}

static void
repeat_handler(struct toytimer *tt)
{
	struct keyboard *keyboard =
		container_of(tt, struct keyboard, repeat_timer);

	/* Only keys that carry a keysym arm this timer, but the release that
	 * disarms it and a pending expiry can race. */
	if (!keyboard->held_key)
		return;

	tap_keysym(keyboard->keyboard, keyboard->held_time,
		   keyboard->held_key->keysym);
}

/* Repeat while held, at the same delay and rate for every key that does it. */
static void
arm_repeat(struct keyboard *keyboard)
{
	struct itimerspec its;

	its.it_value.tv_sec = 0;
	its.it_value.tv_nsec = REPEAT_DELAY_MSEC * 1000000L;
	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = REPEAT_RATE_MSEC * 1000000L;
	toytimer_arm(&keyboard->repeat_timer, &its);
}

/* The globe: on to the next enabled language, back on its letters. */
static void
language_next(struct keyboard *keyboard)
{
	struct virtual_keyboard *vk = keyboard->keyboard;

	if (vk->language_count < 2)
		return;

	vk->language_index = (vk->language_index + 1) % vk->language_count;
	keyboard->state = KEYBOARD_STATE_DEFAULT;
	announce_layout(vk);
}

static void
key_press(struct keyboard *keyboard, uint32_t time, const struct key *key)
{
	keyboard->held_key = key;
	keyboard->held_time = time;
	keyboard->popup.open = false;

	switch (key->key_type) {
	case keytype_default:
		if (key->hint || key->more)
			toytimer_arm_once_usec(&keyboard->longpress_timer,
					       LONGPRESS_USEC);
		break;
	case keytype_backspace:
	case keytype_arrow:
		tap_keysym(keyboard->keyboard, time, key->keysym);
		arm_repeat(keyboard);
		break;
	case keytype_enter:
		send_keysym(keyboard->keyboard, time, XKB_KEY_Return,
			    WL_KEYBOARD_KEY_STATE_PRESSED);
		break;
	case keytype_switch:
		if (keyboard->state == KEYBOARD_STATE_UPPERCASE &&
		    time - keyboard->last_shift_time < CAPS_DOUBLE_TAP_MSEC) {
			keyboard->state = KEYBOARD_STATE_LOCKED;
		} else {
			switch (keyboard->state) {
			case KEYBOARD_STATE_UPPERCASE:
			case KEYBOARD_STATE_LOCKED:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			default:
				/* the symbol pages carry no shift key, so
				 * this only ever fires from the letters */
				keyboard->state = KEYBOARD_STATE_UPPERCASE;
				break;
			}
		}
		keyboard->last_shift_time = time;
		break;
	case keytype_page:
		keyboard->state = key->page;
		break;
	case keytype_lang:
		language_next(keyboard);
		break;
	case keytype_space:
	case keytype_spacer:
		break;
	}
}

static void
key_release(struct keyboard *keyboard, uint32_t time)
{
	const struct key *key = keyboard->held_key;

	if (!key)
		return;

	keyboard->held_key = NULL;

	switch (key->key_type) {
	case keytype_default:
		toytimer_disarm(&keyboard->longpress_timer);

		if (keyboard->popup.open) {
			commit_text(keyboard->keyboard,
				    keyboard->popup.entries[keyboard->popup.selected]);
			keyboard->popup.open = false;
		} else {
			commit_text(keyboard->keyboard,
				    label_from_key(keyboard, key));
		}

		/* one-shot shift; caps lock stays */
		if (keyboard->state == KEYBOARD_STATE_UPPERCASE)
			keyboard->state = KEYBOARD_STATE_DEFAULT;
		break;
	case keytype_space:
		commit_text(keyboard->keyboard, " ");
		break;
	case keytype_backspace:
	case keytype_arrow:
		toytimer_disarm(&keyboard->repeat_timer);
		break;
	case keytype_enter:
		send_keysym(keyboard->keyboard, time, XKB_KEY_Return,
			    WL_KEYBOARD_KEY_STATE_RELEASED);
		break;
	default:
		break;
	}
}

static const struct key *
lookup_key(const struct layout *layout, double x, double y)
{
	int row, col;
	unsigned int i;

	row = (int)(y / layout->row_h);
	col = (int)(x / layout->unit) + row * layout->columns;
	if (row < 0 || x < 0)
		return NULL;

	/* a zero-width key never takes col below zero, so the hidden
	 * globe is skipped here without a special case */
	for (i = 0; i < layout->count; ++i) {
		col -= layout->keys[i].width;
		if (col < 0)
			return &layout->keys[i];
	}

	return NULL;
}

static void
handle_press(struct keyboard *keyboard, uint32_t time,
	     float x, float y,
	     enum wl_pointer_button_state state)
{
	struct rectangle allocation;
	const struct layout *layout;
	const struct key *key;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		layout = get_current_layout(keyboard->keyboard);

		widget_get_allocation(keyboard->widget, &allocation);
		x -= allocation.x;
		y -= allocation.y;

		key = lookup_key(layout, x, y);
		if (key && key->key_type != keytype_spacer)
			key_press(keyboard, time, key);
	} else {
		/* the release always acts on the key that was pressed,
		 * wherever the finger ended up */
		key_release(keyboard, time);
	}

	widget_schedule_redraw(keyboard->widget);
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button,
	       enum wl_pointer_button_state state, void *data)
{
	struct keyboard *keyboard = data;
	int32_t x, y;

	if (button != BTN_LEFT) {
		return;
	}

	input_get_position(input, &x, &y);
	handle_press(keyboard, time, x, y, state);
}

static int
motion_handler(struct widget *widget, struct input *input,
	       uint32_t time, float x, float y, void *data)
{
	struct keyboard *keyboard = data;
	struct rectangle allocation;

	widget_get_allocation(keyboard->widget, &allocation);
	popup_track(keyboard, x - allocation.x);

	return CURSOR_LEFT_PTR;
}

static void
touch_down_handler(struct widget *widget, struct input *input,
		   uint32_t serial, uint32_t time, int32_t id,
		   float x, float y, void *data)
{
	struct keyboard *keyboard = data;

	handle_press(keyboard, time, x, y,
		     WL_POINTER_BUTTON_STATE_PRESSED);
}

static void
touch_up_handler(struct widget *widget, struct input *input,
		 uint32_t serial, uint32_t time, int32_t id,
		 void *data)
{
	struct keyboard *keyboard = data;

	handle_press(keyboard, time, 0, 0,
		     WL_POINTER_BUTTON_STATE_RELEASED);
}

static void
touch_motion_handler(struct widget *widget, struct input *input,
		     uint32_t time, int32_t id,
		     float x, float y, void *data)
{
	struct keyboard *keyboard = data;
	struct rectangle allocation;

	widget_get_allocation(keyboard->widget, &allocation);
	popup_track(keyboard, x - allocation.x);
}

static void
handle_surrounding_text(void *data,
			struct zwp_input_method_context_v1 *context,
			const char *text,
			uint32_t cursor,
			uint32_t anchor)
{
	struct virtual_keyboard *keyboard = data;

	free(keyboard->surrounding_text);
	keyboard->surrounding_text = strdup(text);

	keyboard->surrounding_cursor = cursor;
}

static void
handle_reset(void *data,
	     struct zwp_input_method_context_v1 *context)
{
}

static void
handle_content_type(void *data,
		    struct zwp_input_method_context_v1 *context,
		    uint32_t hint,
		    uint32_t purpose)
{
	struct virtual_keyboard *keyboard = data;

	keyboard->content_hint = hint;
	keyboard->content_purpose = purpose;
}

static void
handle_invoke_action(void *data,
		     struct zwp_input_method_context_v1 *context,
		     uint32_t button,
		     uint32_t index)
{
}

static void
handle_commit_state(void *data,
		    struct zwp_input_method_context_v1 *context,
		    uint32_t serial)
{
	struct virtual_keyboard *keyboard = data;

	keyboard->serial = serial;

	announce_layout(keyboard);
}

static void
handle_preferred_language(void *data,
			  struct zwp_input_method_context_v1 *context,
			  const char *language)
{
	struct virtual_keyboard *keyboard = data;

	if (keyboard->preferred_language)
		free(keyboard->preferred_language);

	keyboard->preferred_language = NULL;

	if (language)
		keyboard->preferred_language = strdup(language);
}

static const struct zwp_input_method_context_v1_listener input_method_context_listener = {
	handle_surrounding_text,
	handle_reset,
	handle_content_type,
	handle_invoke_action,
	handle_commit_state,
	handle_preferred_language
};

static void
input_method_activate(void *data,
		      struct zwp_input_method_v1 *input_method,
		      struct zwp_input_method_context_v1 *context)
{
	struct virtual_keyboard *keyboard = data;
	struct wl_array modifiers_map;
	const char *start_state = getenv("WESTON_KEYBOARD_START_STATE");
	const char *start_lang = getenv("WESTON_KEYBOARD_START_LANG");
	int i;

	/* debug hooks so non-default states and languages can be
	 * screenshotted headlessly */
	if (start_state && !strcmp(start_state, "symbols"))
		keyboard->keyboard->state = KEYBOARD_STATE_SYMBOLS;
	else if (start_state && !strcmp(start_state, "symbols2"))
		keyboard->keyboard->state = KEYBOARD_STATE_SYMBOLS2;
	else if (start_state && !strcmp(start_state, "uppercase"))
		keyboard->keyboard->state = KEYBOARD_STATE_UPPERCASE;
	else
		keyboard->keyboard->state = KEYBOARD_STATE_DEFAULT;
	for (i = 0; start_lang && i < keyboard->language_count; i++)
		if (!strcmp(start_lang, keyboard->languages[i]->code))
			keyboard->language_index = i;
	keyboard->keyboard->held_key = NULL;
	keyboard->keyboard->popup.open = false;
	toytimer_disarm(&keyboard->keyboard->longpress_timer);
	toytimer_disarm(&keyboard->keyboard->repeat_timer);

	if (keyboard->context)
		zwp_input_method_context_v1_destroy(keyboard->context);

	keyboard->content_hint = 0;
	keyboard->content_purpose = 0;
	free(keyboard->preferred_language);
	keyboard->preferred_language = NULL;
	free(keyboard->surrounding_text);
	keyboard->surrounding_text = NULL;

	keyboard->serial = 0;

	keyboard->context = context;
	zwp_input_method_context_v1_add_listener(context,
						 &input_method_context_listener,
						 keyboard);

	wl_array_init(&modifiers_map);
	keysym_modifiers_add(&modifiers_map, "Shift");
	keysym_modifiers_add(&modifiers_map, "Control");
	keysym_modifiers_add(&modifiers_map, "Mod1");
	zwp_input_method_context_v1_modifiers_map(context, &modifiers_map);
	keyboard->keysym.shift_mask = keysym_modifiers_get_mask(&modifiers_map, "Shift");
	wl_array_release(&modifiers_map);

	announce_layout(keyboard);
}

static void
input_method_deactivate(void *data,
			struct zwp_input_method_v1 *input_method,
			struct zwp_input_method_context_v1 *context)
{
	struct virtual_keyboard *keyboard = data;

	if (!keyboard->context)
		return;

	toytimer_disarm(&keyboard->keyboard->longpress_timer);
	toytimer_disarm(&keyboard->keyboard->repeat_timer);
	keyboard->keyboard->held_key = NULL;
	keyboard->keyboard->popup.open = false;

	zwp_input_method_context_v1_destroy(keyboard->context);
	keyboard->context = NULL;
}

static const struct zwp_input_method_v1_listener input_method_listener = {
	input_method_activate,
	input_method_deactivate
};

static void
global_handler(struct display *display, uint32_t name,
	       const char *interface, uint32_t version, void *data)
{
	struct virtual_keyboard *keyboard = data;

	if (!strcmp(interface, "zwp_input_panel_v1")) {
		keyboard->input_panel =
			display_bind(display, name, &zwp_input_panel_v1_interface, 1);
	} else if (!strcmp(interface, "zwp_input_method_v1")) {
		keyboard->input_method =
			display_bind(display, name,
				     &zwp_input_method_v1_interface, 1);
		zwp_input_method_v1_add_listener(keyboard->input_method,
						 &input_method_listener,
						 keyboard);
	}
}

static void
set_toplevel(struct output *output, struct virtual_keyboard *virtual_keyboard)
{
	zwp_input_panel_surface_v1_set_toplevel(virtual_keyboard->ips,
						output_get_wl_output(output),
						ZWP_INPUT_PANEL_SURFACE_V1_POSITION_CENTER_BOTTOM);
	virtual_keyboard->toplevel = true;
	virtual_keyboard->overlay = false;
}

static void
set_overlay(struct output *output, struct virtual_keyboard *virtual_keyboard)
{
	zwp_input_panel_surface_v1_set_overlay_panel(virtual_keyboard->ips);
	virtual_keyboard->toplevel = false;
	virtual_keyboard->overlay = true;
}

static void
display_output_handler(struct output *output, void *data) {
	struct virtual_keyboard *keyboard = data;
	const char *type = getenv("WESTON_KEYBOARD_SURFACE_TYPE");

	if (type && strcasecmp("overlay", type) == 0) {
		if (!keyboard->overlay)
			set_overlay(output, keyboard);
	} else {
		if (!keyboard->toplevel)
			set_toplevel(output, keyboard);
	}
}

static void
keyboard_create(struct virtual_keyboard *virtual_keyboard)
{
	struct keyboard *keyboard;
	const struct layout *layout;

	keyboard = xzalloc(sizeof *keyboard);
	keyboard->keyboard = virtual_keyboard;
	keyboard->window = window_create_custom(virtual_keyboard->display);
	keyboard->widget = window_add_widget(keyboard->window, keyboard);

	toytimer_init(&keyboard->longpress_timer, CLOCK_MONOTONIC,
		      virtual_keyboard->display, longpress_handler);
	toytimer_init(&keyboard->repeat_timer, CLOCK_MONOTONIC,
		      virtual_keyboard->display, repeat_handler);

	virtual_keyboard->ips =
		zwp_input_panel_v1_get_input_panel_surface(virtual_keyboard->input_panel,
							   window_get_wl_surface(keyboard->window));
	virtual_keyboard->keyboard = keyboard;

	window_set_title(keyboard->window, "Virtual keyboard");
	window_set_appid(keyboard->window,
			 "org.freedesktop.weston.virtual-keyboard");
	window_set_user_data(keyboard->window, keyboard);

	widget_set_redraw_handler(keyboard->widget, redraw_handler);
	widget_set_resize_handler(keyboard->widget, resize_handler);
	widget_set_button_handler(keyboard->widget, button_handler);
	widget_set_motion_handler(keyboard->widget, motion_handler);
	widget_set_touch_down_handler(keyboard->widget, touch_down_handler);
	widget_set_touch_up_handler(keyboard->widget, touch_up_handler);
	widget_set_touch_motion_handler(keyboard->widget, touch_motion_handler);

	/* after virtual_keyboard->keyboard is set: the layout depends on
	 * the page the keyboard is on */
	layout = get_current_layout(virtual_keyboard);

	window_schedule_resize(keyboard->window,
			       layout->columns * layout->unit,
			       layout->rows * layout->row_h);

	display_set_output_configure_handler(virtual_keyboard->display,
					     display_output_handler);
}

static void
keyboard_destroy(struct virtual_keyboard *virtual_keyboard)
{
	if (virtual_keyboard->ips)
		zwp_input_panel_surface_v1_destroy(virtual_keyboard->ips);

	if (virtual_keyboard->input_panel)
		zwp_input_panel_v1_destroy(virtual_keyboard->input_panel);

	if (virtual_keyboard->input_method)
		zwp_input_method_v1_destroy(virtual_keyboard->input_method);

	toytimer_fini(&virtual_keyboard->keyboard->longpress_timer);
	toytimer_fini(&virtual_keyboard->keyboard->repeat_timer);

	widget_destroy(virtual_keyboard->keyboard->widget);
	window_destroy(virtual_keyboard->keyboard->window);
	free(virtual_keyboard->keyboard);
}

static void
report_unknown_language(const char *token, size_t len)
{
	fprintf(stderr, "centroidx-keyboard: ignoring unknown keyboard "
		"layout '%.*s'\n", (int)len, token);
}

/*
 * KEYBOARD_LAYOUTS reaches this process through weston, which starts the
 * input method with its own environment. The weston wrapper has already
 * resolved the station's default to the front of the list, so all that is
 * left here is to take the list in order. Unset — an image run without the
 * wrapper — gives the English-only keyboard this client always was.
 */
static void
languages_init(struct virtual_keyboard *virtual_keyboard)
{
	virtual_keyboard->language_count =
		parse_keyboard_layouts(getenv("KEYBOARD_LAYOUTS"),
				       virtual_keyboard->languages,
				       LANGUAGE_MAX, report_unknown_language);

	if (virtual_keyboard->language_count == 0) {
		virtual_keyboard->languages[0] = &languages[0];
		virtual_keyboard->language_count = 1;
	}
	virtual_keyboard->language_index = 0;

	languages_set_globe(virtual_keyboard->language_count > 1);
}

int
main(int argc, char *argv[])
{
	struct virtual_keyboard virtual_keyboard;

	memset(&virtual_keyboard, 0, sizeof virtual_keyboard);

	languages_init(&virtual_keyboard);

	virtual_keyboard.display = display_create(&argc, argv);
	if (virtual_keyboard.display == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	display_set_user_data(virtual_keyboard.display, &virtual_keyboard);
	display_set_global_handler(virtual_keyboard.display, global_handler);

	if (virtual_keyboard.input_panel == NULL) {
		fprintf(stderr, "No input panel global\n");
		return -1;
	}

	keyboard_create(&virtual_keyboard);

	display_run(virtual_keyboard.display);

	keyboard_destroy(&virtual_keyboard);
	display_destroy(virtual_keyboard.display);

	return 0;
}
