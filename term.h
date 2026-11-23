#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Combining marks (wcwidth == 0) attached to a base cell. Khmer clusters
 * rarely need more than 2-3 per cell. */
#define MAXCOMB 5

enum {
	ATTR_BOLD      = 1 << 0,
	ATTR_FAINT     = 1 << 1,
	ATTR_ITALIC    = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	ATTR_BLINK     = 1 << 4,
	ATTR_REVERSE   = 1 << 5,
	ATTR_INVISIBLE = 1 << 6,
	ATTR_STRIKE    = 1 << 7,
	ATTR_STYLE     = 0xff,
	ATTR_WIDE      = 1 << 8, /* first half of a double-width char */
	ATTR_WDUMMY    = 1 << 9, /* second half of a double-width char */
};

/* Colour encoding: 0 = default, 0x01xxxxxx = palette index, 0x02rrggbb = RGB */
#define COL_DEFAULT    0u
#define COL_PAL(i)     (0x01000000u | (uint32_t)(i))
#define COL_RGB(r,g,b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COL_TYPE(c)    ((c) >> 24)

typedef struct {
	uint32_t cp; /* 0 = empty */
	uint32_t comb[MAXCOMB];
	uint32_t fg, bg;
	uint16_t attr;
	uint8_t ncomb;
} Cell;

typedef struct {
	int cols;
	bool wrapped;
	Cell c[];
} Line;

typedef struct {
	int x, y;
	Cell pen;
	int charsets[2], gl;
	bool origin, wrapnext;
} SavedCursor;

typedef struct Term {
	int cols, rows;
	Line **lines; /* == main_lines or alt_lines */
	Line **main_lines, **alt_lines;
	bool altscreen;

	Line **sb; /* scrollback ring buffer */
	int sb_cap, sb_count, sb_head;
	int sb_added; /* lines pushed since last reset by the UI */

	int cx, cy;
	bool wrapnext;
	Cell pen;
	int top, bot;
	bool *tabs;
	uint32_t lastcp;

	bool m_appcursor, m_autowrap, m_cursor_visible, m_bracketed_paste;
	bool m_insert, m_origin, m_lnm, m_appkeypad;
	bool m_sync; /* 2026: synchronized output, UI holds frames while set */
	int mouse_mode; /* 0, 1000, 1002, 1003 */
	bool mouse_sgr;
	int cursor_style; /* DECSCUSR: 0-2 block, 3-4 underline, 5-6 bar */
	int charsets[2], gl;
	SavedCursor saved[2];

	/* parser */
	int state;
	uint32_t u_cp, u_min;
	int u_need;
	int params[32];
	bool sub[32];
	int np, nparams;
	bool had_digit;
	char priv, inter;
	char osc[4096];
	int osclen;

	char title[512];
	bool title_changed;
	bool dirty;

	void (*write)(void *ud, const char *s, size_t n);
	void *ud;
} Term;

extern uint32_t term_palette[256];
extern uint32_t term_default_fg, term_default_bg;

Term *term_new(int cols, int rows, int sb_cap,
               void (*write)(void *, const char *, size_t), void *ud);
void term_feed(Term *t, const char *s, size_t n);
void term_resize(Term *t, int cols, int rows);
/* Visible line for screen row `row` when scrolled back `scroll` lines. */
const Line *term_view_line(const Term *t, int row, int scroll);
