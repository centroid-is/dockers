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
 * Same input-method-v1 / input-panel-v1 plumbing as the stock client, with:
 *  - Gboard-like visuals: opaque light sheet, rounded flat keys, staggered
 *    home row, accent-colored Enter, pressed-key highlight, drawn icons.
 *  - A real numeric keypad (with decimal point and minus) for
 *    digits/number content purposes.
 *  - Direct commit_string typing (no preedit), one-shot shift.
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
#include "shared/xalloc.h"

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
};

enum key_type {
	keytype_default,
	keytype_backspace,
	keytype_enter,
	keytype_space,
	keytype_switch,
	keytype_symbols,
	keytype_arrow_left,
	keytype_arrow_right,
	keytype_spacer
};

struct key {
	enum key_type key_type;

	char *label;
	char *uppercase;
	char *symbol;

	/* width in layout grid units */
	unsigned int width;
};

struct layout {
	const struct key *keys;
	uint32_t count;

	uint32_t columns;
	uint32_t rows;

	/* pixel width of one grid unit */
	double unit;

	const char *language;
	uint32_t text_direction;
};

/*
 * Alpha layout: 20 grid units of 36 px -> 720 px wide, the stock
 * footprint. Letter keys are 2 units; the home row is staggered with
 * 1-unit spacers like Gboard.
 */
static const struct key normal_keys[] = {
	{ keytype_default, "q", "Q", "1", 2},
	{ keytype_default, "w", "W", "2", 2},
	{ keytype_default, "e", "E", "3", 2},
	{ keytype_default, "r", "R", "4", 2},
	{ keytype_default, "t", "T", "5", 2},
	{ keytype_default, "y", "Y", "6", 2},
	{ keytype_default, "u", "U", "7", 2},
	{ keytype_default, "i", "I", "8", 2},
	{ keytype_default, "o", "O", "9", 2},
	{ keytype_default, "p", "P", "0", 2},

	{ keytype_spacer, "", "", "", 1},
	{ keytype_default, "a", "A", "-", 2},
	{ keytype_default, "s", "S", "/", 2},
	{ keytype_default, "d", "D", ":", 2},
	{ keytype_default, "f", "F", ";", 2},
	{ keytype_default, "g", "G", "(", 2},
	{ keytype_default, "h", "H", ")", 2},
	{ keytype_default, "j", "J", "&", 2},
	{ keytype_default, "k", "K", "@", 2},
	{ keytype_default, "l", "L", "\"", 2},
	{ keytype_spacer, "", "", "", 1},

	{ keytype_switch, "", "", "", 3},
	{ keytype_default, "z", "Z", "*", 2},
	{ keytype_default, "x", "X", "'", 2},
	{ keytype_default, "c", "C", "#", 2},
	{ keytype_default, "v", "V", "!", 2},
	{ keytype_default, "b", "B", "?", 2},
	{ keytype_default, "n", "N", "+", 2},
	{ keytype_default, "m", "M", "=", 2},
	{ keytype_backspace, "", "", "", 3},

	{ keytype_symbols, "?123", "?123", "ABC", 3},
	{ keytype_default, ",", ",", "_", 2},
	{ keytype_space, "", "", "", 10},
	{ keytype_default, ".", ".", "%", 2},
	{ keytype_enter, "", "", "", 3}
};

/*
 * Numeric keypad: 12 grid units of 60 px -> 720 px wide, 4 rows — the
 * same panel footprint as the alpha layout, so the input panel surface
 * never needs to resize (weston keeps the initial panel size). The
 * keypad itself is 8 units, centered by 2-unit spacers on each side.
 * Phone-style digit order, backspace/minus/enter down the right,
 * decimal point next to 0.
 */
static const struct key numeric_keys[] = {
	{ keytype_spacer, "", "", "", 2},
	{ keytype_default, "1", "1", "1", 2},
	{ keytype_default, "2", "2", "2", 2},
	{ keytype_default, "3", "3", "3", 2},
	{ keytype_backspace, "", "", "", 2},
	{ keytype_spacer, "", "", "", 2},

	{ keytype_spacer, "", "", "", 2},
	{ keytype_default, "4", "4", "4", 2},
	{ keytype_default, "5", "5", "5", 2},
	{ keytype_default, "6", "6", "6", 2},
	{ keytype_default, "-", "-", "-", 2},
	{ keytype_spacer, "", "", "", 2},

	{ keytype_spacer, "", "", "", 2},
	{ keytype_default, "7", "7", "7", 2},
	{ keytype_default, "8", "8", "8", 2},
	{ keytype_default, "9", "9", "9", 2},
	{ keytype_enter, "", "", "", 2},
	{ keytype_spacer, "", "", "", 2},

	{ keytype_spacer, "", "", "", 2},
	{ keytype_arrow_left, "", "", "", 2},
	{ keytype_default, "0", "0", "0", 2},
	{ keytype_default, ".", ".", ".", 2},
	{ keytype_arrow_right, "", "", "", 2},
	{ keytype_spacer, "", "", "", 2}
};

static const struct layout normal_layout = {
	normal_keys,
	sizeof(normal_keys) / sizeof(*normal_keys),
	20,
	4,
	36,
	"en",
	ZWP_TEXT_INPUT_V1_TEXT_DIRECTION_LTR
};

static const struct layout numeric_layout = {
	numeric_keys,
	sizeof(numeric_keys) / sizeof(*numeric_keys),
	12,
	4,
	60,
	"en",
	ZWP_TEXT_INPUT_V1_TEXT_DIRECTION_LTR
};

static const double key_height = 50;

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

enum keyboard_state {
	KEYBOARD_STATE_DEFAULT,
	KEYBOARD_STATE_UPPERCASE,
	KEYBOARD_STATE_SYMBOLS
};

struct keyboard {
	struct virtual_keyboard *keyboard;
	struct window *window;
	struct widget *widget;

	enum keyboard_state state;
	const struct key *pressed_key;
};

static const struct layout *
get_current_layout(struct virtual_keyboard *keyboard);

static const char *
label_from_key(struct keyboard *keyboard,
	       const struct key *key)
{
	switch(keyboard->state) {
	case KEYBOARD_STATE_DEFAULT:
		return key->label;
	case KEYBOARD_STATE_UPPERCASE:
		return key->uppercase;
	case KEYBOARD_STATE_SYMBOLS:
		return key->symbol;
	}

	return "";
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

static void
draw_icon_shift(cairo_t *cr, double cx, double cy, bool filled)
{
	/* upward arrow with a stem, ~22 px tall */
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy - 10);
	cairo_line_to(cr, cx + 9, cy);
	cairo_line_to(cr, cx + 4, cy);
	cairo_line_to(cr, cx + 4, cy + 9);
	cairo_line_to(cr, cx - 4, cy + 9);
	cairo_line_to(cr, cx - 4, cy);
	cairo_line_to(cr, cx - 9, cy);
	cairo_close_path(cr);
	if (filled) {
		cairo_fill(cr);
	} else {
		cairo_set_line_width(cr, 1.8);
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
draw_icon_arrow(cairo_t *cr, double cx, double cy, int dir)
{
	/* chevron; dir -1 = left, +1 = right */
	cairo_set_line_width(cr, 2.0);
	cairo_new_path(cr);
	cairo_move_to(cr, cx + 4 * -dir, cy - 7);
	cairo_line_to(cr, cx + 4 * dir, cy);
	cairo_line_to(cr, cx + 4 * -dir, cy + 7);
	cairo_stroke(cr);
}

static void
draw_key(struct keyboard *keyboard,
	 const struct key *key,
	 cairo_t *cr,
	 unsigned int row,
	 unsigned int col)
{
	const char *label;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	double unit, x, y, w, h, cx, cy;
	bool pressed, accent;
	const double gap_x = 3.5, gap_y = 4.5, radius = 6;

	if (key->key_type == keytype_spacer)
		return;

	unit = get_current_layout(keyboard->keyboard)->unit;

	x = col * unit + gap_x;
	y = row * key_height + gap_y;
	w = key->width * unit - 2 * gap_x;
	h = key_height - 2 * gap_y;
	cx = x + w / 2;
	cy = y + h / 2;

	pressed = keyboard->pressed_key == key;
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
		draw_icon_shift(cr, cx, cy,
				keyboard->state == KEYBOARD_STATE_UPPERCASE);
		break;
	case keytype_backspace:
		draw_icon_backspace(cr, cx, cy);
		break;
	case keytype_enter:
		draw_icon_enter(cr, cx, cy);
		break;
	case keytype_arrow_left:
		draw_icon_arrow(cr, cx, cy, -1);
		break;
	case keytype_arrow_right:
		draw_icon_arrow(cr, cx, cy, 1);
		break;
	case keytype_space:
	case keytype_spacer:
		break;
	default:
		label = label_from_key(keyboard, key);
		cairo_set_font_size(cr, strlen(label) > 1 ? 14 : 19);
		cairo_text_extents(cr, label, &extents);
		cairo_font_extents(cr, &font_extents);
		cairo_move_to(cr,
			      cx - extents.width / 2 - extents.x_bearing,
			      cy + font_extents.height / 2 -
				      font_extents.descent);
		cairo_show_text(cr, label);
		break;
	}

	cairo_restore(cr);
}

static const struct layout *
get_current_layout(struct virtual_keyboard *keyboard)
{
	switch (keyboard->content_purpose) {
		case ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DIGITS:
		case ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NUMBER:
			return &numeric_layout;
		default:
			return &normal_layout;
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

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgb(cr, COL(color_sheet));
	cairo_rectangle(cr, 0, 0,
			layout->columns * layout->unit,
			layout->rows * key_height);
	cairo_paint(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (i = 0; i < layout->count; ++i) {
		draw_key(keyboard, &layout->keys[i], cr, row, col);
		col += layout->keys[i].width;
		if (col >= layout->columns) {
			row += 1;
			col = 0;
		}
	}

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
	zwp_input_method_context_v1_cursor_position(keyboard->context, 0, 0);
	zwp_input_method_context_v1_commit_string(keyboard->context,
						  keyboard->serial,
						  text);
}

static void
send_keysym(struct virtual_keyboard *keyboard, uint32_t time,
	    xkb_keysym_t sym, uint32_t key_state, xkb_mod_mask_t mod_mask)
{
	zwp_input_method_context_v1_keysym(keyboard->context,
					   display_get_serial(keyboard->display),
					   time, sym, key_state, mod_mask);
}

static void
keyboard_handle_key(struct keyboard *keyboard, uint32_t time, const struct key *key, struct input *input, enum wl_pointer_button_state state)
{
	const char *label = label_from_key(keyboard, key);
	xkb_mod_mask_t mod_mask = keyboard->state == KEYBOARD_STATE_DEFAULT ? 0 : keyboard->keyboard->keysym.shift_mask;
	uint32_t key_state = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;

	switch (key->key_type) {
		case keytype_default:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;

			commit_text(keyboard->keyboard, label);

			/* one-shot shift */
			if (keyboard->state == KEYBOARD_STATE_UPPERCASE)
				keyboard->state = KEYBOARD_STATE_DEFAULT;
			break;
		case keytype_backspace:
			send_keysym(keyboard->keyboard, time,
				    XKB_KEY_BackSpace, key_state, 0);
			break;
		case keytype_enter:
			send_keysym(keyboard->keyboard, time,
				    XKB_KEY_Return, key_state, mod_mask);
			break;
		case keytype_space:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			commit_text(keyboard->keyboard, " ");
			break;
		case keytype_switch:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			switch(keyboard->state) {
			case KEYBOARD_STATE_DEFAULT:
				keyboard->state = KEYBOARD_STATE_UPPERCASE;
				break;
			case KEYBOARD_STATE_UPPERCASE:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			case KEYBOARD_STATE_SYMBOLS:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			}
			break;
		case keytype_symbols:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			switch(keyboard->state) {
			case KEYBOARD_STATE_DEFAULT:
			case KEYBOARD_STATE_UPPERCASE:
				keyboard->state = KEYBOARD_STATE_SYMBOLS;
				break;
			case KEYBOARD_STATE_SYMBOLS:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			}
			break;
		case keytype_arrow_left:
			send_keysym(keyboard->keyboard, time,
				    XKB_KEY_Left, key_state, mod_mask);
			break;
		case keytype_arrow_right:
			send_keysym(keyboard->keyboard, time,
				    XKB_KEY_Right, key_state, mod_mask);
			break;
		case keytype_spacer:
			break;
	}
}

static const struct key *
lookup_key(const struct layout *layout, double x, double y)
{
	int row, col;
	unsigned int i;

	row = (int)(y / key_height);
	col = (int)(x / layout->unit) + row * layout->columns;
	if (row < 0 || x < 0)
		return NULL;

	for (i = 0; i < layout->count; ++i) {
		col -= layout->keys[i].width;
		if (col < 0)
			return &layout->keys[i];
	}

	return NULL;
}

static void
handle_press(struct keyboard *keyboard, uint32_t time,
	     struct input *input, float x, float y,
	     enum wl_pointer_button_state state)
{
	struct rectangle allocation;
	const struct layout *layout;
	const struct key *key;

	layout = get_current_layout(keyboard->keyboard);

	widget_get_allocation(keyboard->widget, &allocation);
	x -= allocation.x;
	y -= allocation.y;

	key = lookup_key(layout, x, y);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		keyboard->pressed_key = key;
	else
		keyboard->pressed_key = NULL;

	if (key)
		keyboard_handle_key(keyboard, time, key, input, state);

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
	handle_press(keyboard, time, input, x, y, state);
}

static void
touch_down_handler(struct widget *widget, struct input *input,
		   uint32_t serial, uint32_t time, int32_t id,
		   float x, float y, void *data)
{
	struct keyboard *keyboard = data;

	handle_press(keyboard, time, input, x, y,
		     WL_POINTER_BUTTON_STATE_PRESSED);
}

static void
touch_up_handler(struct widget *widget, struct input *input,
		 uint32_t serial, uint32_t time, int32_t id,
		 void *data)
{
	struct keyboard *keyboard = data;
	float x, y;

	input_get_touch(input, id, &x, &y);

	handle_press(keyboard, time, input, x, y,
		     WL_POINTER_BUTTON_STATE_RELEASED);
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
	const struct layout *layout;

	keyboard->serial = serial;

	layout = get_current_layout(keyboard);

	window_schedule_resize(keyboard->keyboard->window,
			       layout->columns * layout->unit,
			       layout->rows * key_height);

	zwp_input_method_context_v1_language(context,
					     keyboard->serial,
					     layout->language);
	zwp_input_method_context_v1_text_direction(context,
						   keyboard->serial,
						   layout->text_direction);

	widget_schedule_redraw(keyboard->keyboard->widget);
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
	const struct layout *layout;

	keyboard->keyboard->state = KEYBOARD_STATE_DEFAULT;
	keyboard->keyboard->pressed_key = NULL;

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

	layout = get_current_layout(keyboard);

	window_schedule_resize(keyboard->keyboard->window,
			       layout->columns * layout->unit,
			       layout->rows * key_height);

	zwp_input_method_context_v1_language(context,
					     keyboard->serial,
					     layout->language);
	zwp_input_method_context_v1_text_direction(context,
						   keyboard->serial,
						   layout->text_direction);

	widget_schedule_redraw(keyboard->keyboard->widget);
}

static void
input_method_deactivate(void *data,
			struct zwp_input_method_v1 *input_method,
			struct zwp_input_method_context_v1 *context)
{
	struct virtual_keyboard *keyboard = data;

	if (!keyboard->context)
		return;

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

	layout = get_current_layout(virtual_keyboard);

	keyboard = xzalloc(sizeof *keyboard);
	keyboard->keyboard = virtual_keyboard;
	keyboard->window = window_create_custom(virtual_keyboard->display);
	keyboard->widget = window_add_widget(keyboard->window, keyboard);

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
	widget_set_touch_down_handler(keyboard->widget, touch_down_handler);
	widget_set_touch_up_handler(keyboard->widget, touch_up_handler);

	window_schedule_resize(keyboard->window,
			       layout->columns * layout->unit,
			       layout->rows * key_height);

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

	widget_destroy(virtual_keyboard->keyboard->widget);
	window_destroy(virtual_keyboard->keyboard->window);
	free(virtual_keyboard->keyboard);
}

int
main(int argc, char *argv[])
{
	struct virtual_keyboard virtual_keyboard;

	memset(&virtual_keyboard, 0, sizeof virtual_keyboard);

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
