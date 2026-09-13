/*
 * Checks the on-screen keyboard's layout tables without building weston.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -Wno-missing-field-initializers \
 *      -o layouts-check weston/keyboard/test/layouts-check.c && ./layouts-check
 *
 * Needs only libxkbcommon's headers (for the keysym names in the tables).
 * Nothing here is drawn: a layout that passes can still look wrong, but one
 * that fails is certainly broken — keys shifted into the wrong cells, a
 * popup that runs off the panel, or a character the embedder will truncate.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../keyboard-layouts.h"

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			failures++;					\
			fprintf(stderr, "FAIL line %d: ", __LINE__);	\
			fprintf(stderr, __VA_ARGS__);			\
			fputc('\n', stderr);				\
		}							\
	} while (0)

/* Exactly one well-formed UTF-8 character, inside the Basic Multilingual
 * Plane: the embedder commits only the first UTF-16 unit of a string. */
static bool
single_bmp_character(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	uint32_t cp;
	int extra, i;

	if (!p || !p[0])
		return false;

	if (p[0] < 0x80) {
		cp = p[0];
		extra = 0;
	} else if ((p[0] & 0xe0) == 0xc0) {
		cp = p[0] & 0x1f;
		extra = 1;
	} else if ((p[0] & 0xf0) == 0xe0) {
		cp = p[0] & 0x0f;
		extra = 2;
	} else {
		return false;
	}

	for (i = 1; i <= extra; i++) {
		if ((p[i] & 0xc0) != 0x80)
			return false;
		cp = (cp << 6) | (p[i] & 0x3f);
	}

	if (p[extra + 1] != '\0')
		return false;
	if (extra == 1 && cp < 0x80)
		return false;
	if (extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
		return false;

	return true;
}

static void
check_more(const char *name, const struct layout *layout,
	   const struct key *key)
{
	size_t lower = 0, upper = 0, cells;

	CHECK(key->more_upper != NULL,
	      "%s: '%s' has long-press alternates but no shifted ones",
	      name, key->label);

	for (; key->more[lower]; lower++)
		CHECK(single_bmp_character(key->more[lower]),
		      "%s: alternate '%s' on '%s' is not one BMP character",
		      name, key->more[lower], key->label);

	for (; key->more_upper && key->more_upper[upper]; upper++)
		CHECK(single_bmp_character(key->more_upper[upper]),
		      "%s: alternate '%s' on '%s' is not one BMP character",
		      name, key->more_upper[upper], key->label);

	CHECK(key->more_upper == NULL || lower == upper,
	      "%s: '%s' has %zu alternates but %zu shifted ones",
	      name, key->label, lower, upper);

	cells = (key->hint ? 1 : 0) + lower;
	CHECK(cells <= POPUP_MAX, "%s: '%s' popup has %zu cells, max %d",
	      name, key->label, cells, POPUP_MAX);
	CHECK(cells * key->width <= layout->columns,
	      "%s: '%s' popup is wider than the panel", name, key->label);
}

static void
check_layout(const char *name, const struct layout *layout,
	     double panel_height)
{
	unsigned int i, row = 0, col = 0;
	double width = layout->columns * layout->unit;

	for (i = 0; i < layout->count; i++) {
		const struct key *key = &layout->keys[i];

		col += key->width;
		CHECK(col <= layout->columns,
		      "%s: row %u runs past %u columns at key %u ('%s')",
		      name, row, layout->columns, i, key->label);

		if (key->key_type == keytype_default) {
			CHECK(single_bmp_character(key->label),
			      "%s: label '%s' is not one BMP character",
			      name, key->label);
			CHECK(!key->uppercase ||
			      single_bmp_character(key->uppercase),
			      "%s: shifted label '%s' is not one BMP character",
			      name, key->uppercase);
			CHECK(!key->hint || single_bmp_character(key->hint),
			      "%s: hint '%s' is not one BMP character",
			      name, key->hint);
		}

		CHECK(!key->marked || (key->more && key->more[0]),
		      "%s: '%s' is marked but has no long-press alternate",
		      name, key->label);

		if (key->more)
			check_more(name, layout, key);
		else
			CHECK(!key->more_upper,
			      "%s: '%s' has shifted alternates only",
			      name, key->label);

		if (col >= layout->columns) {
			row++;
			col = 0;
		}
	}

	CHECK(col == 0, "%s: last row is %u units short of %u",
	      name, layout->columns - col, layout->columns);
	CHECK(row == layout->rows, "%s: %u rows, the layout says %u",
	      name, row, layout->rows);
	CHECK(width > PANEL_WIDTH - 0.001 && width < PANEL_WIDTH + 0.001,
	      "%s: %.3f px wide, the panel is %.0f", name, width, PANEL_WIDTH);
	CHECK(layout->rows * layout->row_h == panel_height,
	      "%s: %.0f px tall, the other pages are %.0f", name,
	      layout->rows * layout->row_h, panel_height);
}

/* Every letter in want is the visible corner mark of some key. */
static void
expect_marks(const char *name, const struct layout *layout,
	     const char *const *want)
{
	unsigned int i;

	for (; *want; want++) {
		bool found = false;

		for (i = 0; i < layout->count; i++) {
			const struct key *key = &layout->keys[i];

			if (key->marked && key->more &&
			    !strcmp(key->more[0], *want))
				found = true;
		}
		CHECK(found, "%s: '%s' is not marked on any key", name, *want);
	}
}

static unsigned int
globe_width(const struct layout *layout)
{
	unsigned int i;

	for (i = 0; i < layout->count; i++)
		if (layout->keys[i].key_type == keytype_lang)
			return layout->keys[i].width;

	return 0;
}

static int unknown_seen;

static void
count_unknown(const char *token, size_t len)
{
	(void)token;
	(void)len;
	unknown_seen++;
}

static void
expect_parse(const char *input, int max, const char *want, int want_unknown)
{
	const struct language *out[LANGUAGE_MAX];
	char got[64] = "";
	int n, i;

	unknown_seen = 0;
	n = parse_keyboard_layouts(input, out, max, count_unknown);
	for (i = 0; i < n; i++) {
		if (i)
			strcat(got, ",");
		strcat(got, out[i]->code);
	}

	CHECK(!strcmp(got, want), "parse(\"%s\") gave \"%s\", want \"%s\"",
	      input ? input : "(null)", got, want);
	CHECK(unknown_seen == want_unknown,
	      "parse(\"%s\") reported %d unknown tokens, want %d",
	      input ? input : "(null)", unknown_seen, want_unknown);
}

int
main(void)
{
	double panel_height = symbols_layout.rows * symbols_layout.row_h;
	size_t i;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		bool show = pass == 0;

		languages_set_globe(show);
		for (i = 0; i < LANGUAGE_COUNT; i++) {
			const struct layout *alpha = languages[i].alpha;

			check_layout(languages[i].code, alpha, panel_height);
			CHECK(globe_width(alpha) == (show ? GLOBE_WIDTH : 0),
			      "%s: globe is %u wide with the globe %s",
			      languages[i].code, globe_width(alpha),
			      show ? "shown" : "hidden");
		}
	}

	/* hiding twice and showing again must land back where it started */
	languages_set_globe(false);
	languages_set_globe(true);
	for (i = 0; i < LANGUAGE_COUNT; i++)
		check_layout(languages[i].code, languages[i].alpha,
			     panel_height);

	expect_marks("pl", &pl_layout,
		     MORE("ą", "ć", "ę", "ł", "ń", "ó", "ś", "ż"));
	expect_marks("is", &is_layout, MORE("á", "é", "í", "ó", "ú", "ý"));

	check_layout("symbols", &symbols_layout, panel_height);
	check_layout("symbols2", &symbols2_layout, panel_height);
	check_layout("numeric", &numeric_layout, panel_height);

	CHECK(!strcmp(languages[0].code, "en"),
	      "languages[0] is the fallback and must be en, not %s",
	      languages[0].code);
	CHECK(LANGUAGE_COUNT <= LANGUAGE_MAX, "LANGUAGE_MAX is too small");

	expect_parse(NULL, LANGUAGE_MAX, "", 0);
	expect_parse("", LANGUAGE_MAX, "", 0);
	expect_parse(" , ,", LANGUAGE_MAX, "", 0);
	expect_parse("is,en,pl", LANGUAGE_MAX, "is,en,pl", 0);
	expect_parse("pl", LANGUAGE_MAX, "pl", 0);
	expect_parse("PL, us", LANGUAGE_MAX, "pl,en", 0);
	expect_parse("en,us,EN,is", LANGUAGE_MAX, "en,is", 0);
	expect_parse("de,is,,xx", LANGUAGE_MAX, "is", 2);
	expect_parse("isl,i,is", LANGUAGE_MAX, "is", 2);
	expect_parse("is en\tpl", LANGUAGE_MAX, "is,en,pl", 0);
	expect_parse("is,en,pl", 2, "is,en", 0);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	printf("keyboard layouts: all checks passed\n");
	return EXIT_SUCCESS;
}
