/* VT100/xterm-ish terminal state machine and screen model. */
#define _XOPEN_SOURCE 700
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_OSC, S_STR };

uint32_t term_default_fg = 0xd8d8d8, term_default_bg = 0x000000;
uint32_t term_palette[256] = {
	0x000000, 0xcc6666, 0x8fb85a, 0xe0b85a, 0x6a9fd5, 0xb48ad0, 0x62bcb8, 0xc8c8c8,
	0x666666, 0xf07a7a, 0xa8d470, 0xf2d27a, 0x86b8ef, 0xcca4e8, 0x80d8d2, 0xffffff,
};

/* DEC special graphics, 0x5f..0x7e */
static const uint16_t acs[32] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static void init_palette(void)
{
	static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
	for (int i = 0; i < 216; i++)
		term_palette[16 + i] = (uint32_t)steps[i / 36] << 16 |
		                       (uint32_t)steps[(i / 6) % 6] << 8 | (uint32_t)steps[i % 6];
	for (int i = 0; i < 24; i++) {
		uint32_t v = 8 + 10 * (uint32_t)i;
		term_palette[232 + i] = v << 16 | v << 8 | v;
	}
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static Cell blank(const Term *t)
{
	Cell c;
	memset(&c, 0, sizeof c);
	c.fg = t->pen.fg;
	c.bg = t->pen.bg;
	return c;
}

static inline bool cell_is_default(const Cell *c)
{
	return !c->cp && !c->fg && !c->bg && !c->attr && !c->ncomb;
}

static void line_clear(Line *l, int x0, int x1, Cell b)
{
	x0 = imax(x0, 0);
	x1 = imin(x1, l->cols);
	if (x1 <= x0)
		return;
	if (cell_is_default(&b)) {
		/* the common case: default colours, so all-zero bytes */
		memset(&l->c[x0], 0, (size_t)(x1 - x0) * sizeof(Cell));
		return;
	}
	for (int i = x0; i < x1; i++)
		l->c[i] = b;
}

static Line *line_new(int cols, Cell b)
{
	Line *l = malloc(sizeof(Line) + (size_t)cols * sizeof(Cell));
	l->cols = cols;
	l->wrapped = false;
	line_clear(l, 0, cols, b);
	return l;
}

static Line *line_resize(Line *l, int cols)
{
	int old = l->cols;
	if (old == cols)
		return l;
	l = realloc(l, sizeof(Line) + (size_t)cols * sizeof(Cell));
	Cell b;
	memset(&b, 0, sizeof b);
	for (int i = old; i < cols; i++)
		l->c[i] = b;
	l->cols = cols;
	if (cols > 0 && cols < old && (l->c[cols - 1].attr & ATTR_WIDE))
		l->c[cols - 1] = b;
	return l;
}

/* If x is half of a wide char, blank the other half. */
static void fix_wide(Line *l, int x, Cell b)
{
	if (x < 0 || x >= l->cols)
		return;
	if ((l->c[x].attr & ATTR_WIDE) && x + 1 < l->cols)
		l->c[x + 1] = b;
	if ((l->c[x].attr & ATTR_WDUMMY) && x > 0)
		l->c[x - 1] = b;
}

/* ---- scrollback ---- */

/* Copy `src` into the scrollback, dropping trailing default cells (readers
 * treat cells past l->cols as blank). Most lines are much shorter than the
 * screen, so this saves both memory and bandwidth. The caller keeps `src`. */
static void sb_store(Term *t, const Line *src)
{
	if (t->sb_cap == 0)
		return;
	int n = src->cols;
	while (n > 0 && cell_is_default(&src->c[n - 1]))
		n--;
	size_t sz = sizeof(Line) + (size_t)n * sizeof(Cell);
	Line *l;
	if (t->sb_count < t->sb_cap) {
		l = malloc(sz);
		t->sb[(t->sb_head + t->sb_count) % t->sb_cap] = l;
		t->sb_count++;
	} else {
		/* full: recycle the oldest line's allocation */
		l = realloc(t->sb[t->sb_head], sz);
		t->sb[t->sb_head] = l;
		t->sb_head = (t->sb_head + 1) % t->sb_cap;
	}
	l->cols = n;
	l->wrapped = src->wrapped;
	memcpy(l->c, src->c, (size_t)n * sizeof(Cell));
	t->sb_added++;
}

static Line *sb_pop(Term *t)
{
	if (t->sb_count == 0)
		return NULL;
	t->sb_count--;
	return t->sb[(t->sb_head + t->sb_count) % t->sb_cap];
}

static void sb_clear(Term *t)
{
	for (int i = 0; i < t->sb_count; i++)
		free(t->sb[(t->sb_head + i) % t->sb_cap]);
	t->sb_count = t->sb_head = 0;
}

const Line *term_view_line(const Term *t, int row, int scroll)
{
	if (t->altscreen)
		scroll = 0;
	scroll = clamp(scroll, 0, t->sb_count);
	if (row < scroll) {
		int i = t->sb_count - scroll + row;
		return t->sb[(t->sb_head + i) % t->sb_cap];
	}
	return t->lines[row - scroll];
}

/* ---- scrolling ---- */

static void scroll_up(Term *t, int top, int bot, int n, bool to_sb)
{
	n = clamp(n, 0, bot - top + 1);
	Cell b = blank(t);
	for (int i = 0; i < n; i++) {
		Line *l = t->lines[top];
		if (to_sb && top == 0 && !t->altscreen)
			sb_store(t, l);
		line_clear(l, 0, l->cols, b);
		l->wrapped = false;
		memmove(&t->lines[top], &t->lines[top + 1], (size_t)(bot - top) * sizeof(Line *));
		t->lines[bot] = l;
	}
	t->dirty = true;
}

static void scroll_down(Term *t, int top, int bot, int n)
{
	n = clamp(n, 0, bot - top + 1);
	Cell b = blank(t);
	for (int i = 0; i < n; i++) {
		Line *l = t->lines[bot];
		line_clear(l, 0, l->cols, b);
		l->wrapped = false;
		memmove(&t->lines[top + 1], &t->lines[top], (size_t)(bot - top) * sizeof(Line *));
		t->lines[top] = l;
	}
	t->dirty = true;
}

static void index_down(Term *t)
{
	if (t->cy == t->bot)
		scroll_up(t, t->top, t->bot, 1, true);
	else if (t->cy < t->rows - 1)
		t->cy++;
	t->wrapnext = false;
}

static void index_up(Term *t)
{
	if (t->cy == t->top)
		scroll_down(t, t->top, t->bot, 1);
	else if (t->cy > 0)
		t->cy--;
	t->wrapnext = false;
}

static void moveto(Term *t, int x, int y)
{
	int lo = 0, hi = t->rows - 1;
	if (t->m_origin) {
		lo = t->top;
		hi = t->bot;
	}
	t->cx = clamp(x, 0, t->cols - 1);
	t->cy = clamp(y, lo, hi);
	t->wrapnext = false;
}

static void tab(Term *t, int n)
{
	int x = t->cx;
	for (; n > 0; n--)
		do x++; while (x < t->cols - 1 && !t->tabs[x]);
	for (; n < 0; n++)
		do x--; while (x > 0 && !t->tabs[x]);
	t->cx = clamp(x, 0, t->cols - 1);
	t->wrapnext = false;
}

static void reset_tabs(Term *t)
{
	for (int i = 0; i < t->cols; i++)
		t->tabs[i] = (i % 8 == 0) && i;
}

static void save_cursor(Term *t)
{
	SavedCursor *s = &t->saved[t->altscreen];
	s->x = t->cx;
	s->y = t->cy;
	s->pen = t->pen;
	s->charsets[0] = t->charsets[0];
	s->charsets[1] = t->charsets[1];
	s->gl = t->gl;
	s->origin = t->m_origin;
	s->wrapnext = t->wrapnext;
}

static void restore_cursor(Term *t)
{
	SavedCursor *s = &t->saved[t->altscreen];
	t->pen = s->pen;
	t->charsets[0] = s->charsets[0];
	t->charsets[1] = s->charsets[1];
	t->gl = s->gl;
	t->m_origin = s->origin;
	t->cx = clamp(s->x, 0, t->cols - 1);
	t->cy = clamp(s->y, 0, t->rows - 1);
	t->wrapnext = s->wrapnext;
}

static void reset_state(Term *t)
{
	memset(&t->pen, 0, sizeof t->pen);
	t->top = 0;
	t->bot = t->rows - 1;
	t->cx = t->cy = 0;
	t->wrapnext = false;
	t->m_appcursor = t->m_bracketed_paste = t->m_insert = false;
	t->m_origin = t->m_lnm = t->m_appkeypad = t->m_sync = false;
	t->m_autowrap = t->m_cursor_visible = true;
	t->mouse_mode = 0;
	t->mouse_sgr = false;
	t->cursor_style = 0;
	t->charsets[0] = t->charsets[1] = 'B';
	t->gl = 0;
	t->state = S_GROUND;
	save_cursor(t);
	t->saved[0] = t->saved[1] = t->saved[t->altscreen];
}

Term *term_new(int cols, int rows, int sb_cap,
               void (*write)(void *, const char *, size_t), void *ud)
{
	init_palette();
	Term *t = calloc(1, sizeof *t);
	t->write = write;
	t->ud = ud;
	t->cols = cols;
	t->rows = rows;
	t->sb_cap = sb_cap;
	t->sb = calloc((size_t)imax(sb_cap, 1), sizeof(Line *));
	t->main_lines = calloc((size_t)rows, sizeof(Line *));
	t->alt_lines = calloc((size_t)rows, sizeof(Line *));
	Cell b;
	memset(&b, 0, sizeof b);
	for (int i = 0; i < rows; i++) {
		t->main_lines[i] = line_new(cols, b);
		t->alt_lines[i] = line_new(cols, b);
	}
	t->lines = t->main_lines;
	t->tabs = calloc((size_t)cols, 1);
	reset_tabs(t);
	reset_state(t);
	t->dirty = true;
	return t;
}

static void reply(Term *t, const char *s)
{
	if (t->write)
		t->write(t->ud, s, strlen(s));
}

/* ---- printing ---- */

static void put_combining(Term *t, uint32_t cp)
{
	Line *l = t->lines[t->cy];
	int x = t->wrapnext ? t->cx : t->cx - 1;
	if (x < 0)
		return;
	if ((l->c[x].attr & ATTR_WDUMMY) && x > 0)
		x--;
	Cell *c = &l->c[x];
	if (c->cp == 0 || c->ncomb >= MAXCOMB)
		return;
	c->comb[c->ncomb++] = cp;
	t->dirty = true;
}

static void insert_blank(Term *t, int n)
{
	Line *l = t->lines[t->cy];
	n = clamp(n, 0, t->cols - t->cx);
	Cell b = blank(t);
	fix_wide(l, t->cx, b);
	memmove(&l->c[t->cx + n], &l->c[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(Cell));
	line_clear(l, t->cx, t->cx + n, b);
	if (l->c[t->cols - 1].attr & ATTR_WIDE)
		l->c[t->cols - 1] = b;
	t->dirty = true;
}

static void delete_chars(Term *t, int n)
{
	Line *l = t->lines[t->cy];
	n = clamp(n, 0, t->cols - t->cx);
	Cell b = blank(t);
	fix_wide(l, t->cx, b);
	fix_wide(l, t->cx + n - 1, b);
	memmove(&l->c[t->cx], &l->c[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof(Cell));
	line_clear(l, t->cols - n, t->cols, b);
	t->dirty = true;
}

static void tputc(Term *t, uint32_t cp)
{
	if (t->charsets[t->gl] == '0' && cp >= 0x5f && cp <= 0x7e)
		cp = acs[cp - 0x5f];

	int w = cp >= 0x20 && cp < 0x7f ? 1 : wcwidth((wchar_t)cp);
	if (w == 0) {
		put_combining(t, cp);
		return;
	}
	if (w < 0 || w > 2)
		w = 1;

	if (t->wrapnext && t->m_autowrap) {
		t->lines[t->cy]->wrapped = true;
		index_down(t);
		t->cx = 0;
	}
	if (w == 2 && t->cx == t->cols - 1) {
		if (t->m_autowrap) {
			t->lines[t->cy]->c[t->cx] = blank(t);
			t->lines[t->cy]->wrapped = true;
			index_down(t);
			t->cx = 0;
		} else {
			w = 1;
		}
	}
	if (t->m_insert)
		insert_blank(t, w);

	Line *l = t->lines[t->cy];
	if (((l->c[t->cx].attr | (w == 2 ? l->c[t->cx + 1].attr : 0)) & (ATTR_WIDE | ATTR_WDUMMY))) {
		Cell b = blank(t);
		fix_wide(l, t->cx, b);
		if (w == 2)
			fix_wide(l, t->cx + 1, b);
	}

	Cell *c = &l->c[t->cx];
	*c = t->pen;
	c->cp = cp;
	c->ncomb = 0;
	c->attr = (t->pen.attr & ATTR_STYLE) | (w == 2 ? ATTR_WIDE : 0);
	if (w == 2) {
		Cell *d = &l->c[t->cx + 1];
		*d = t->pen;
		d->cp = 0;
		d->ncomb = 0;
		d->attr = (t->pen.attr & ATTR_STYLE) | ATTR_WDUMMY;
	}
	t->lastcp = cp;
	if (t->cx + w < t->cols) {
		t->cx += w;
	} else {
		t->cx = t->cols - 1;
		t->wrapnext = true;
	}
	t->dirty = true;
}

/* ---- erase ---- */

static void clear_line_range(Term *t, int y, int x0, int x1)
{
	Line *l = t->lines[y];
	Cell b = blank(t);
	fix_wide(l, x0, b);
	fix_wide(l, x1 - 1, b);
	line_clear(l, x0, x1, b);
	t->dirty = true;
}

static void erase_display(Term *t, int mode)
{
	switch (mode) {
	case 0:
		clear_line_range(t, t->cy, t->cx, t->cols);
		for (int y = t->cy + 1; y < t->rows; y++)
			clear_line_range(t, y, 0, t->cols);
		break;
	case 1:
		for (int y = 0; y < t->cy; y++)
			clear_line_range(t, y, 0, t->cols);
		clear_line_range(t, t->cy, 0, t->cx + 1);
		break;
	case 2:
		for (int y = 0; y < t->rows; y++) {
			clear_line_range(t, y, 0, t->cols);
			t->lines[y]->wrapped = false;
		}
		break;
	case 3:
		sb_clear(t);
		t->dirty = true;
		break;
	}
}

static void set_alt(Term *t, bool on)
{
	if (t->altscreen == on)
		return;
	t->altscreen = on;
	t->lines = on ? t->alt_lines : t->main_lines;
	if (on) {
		Cell b = blank(t);
		for (int y = 0; y < t->rows; y++)
			line_clear(t->lines[y], 0, t->cols, b);
	}
	t->dirty = true;
}

/* ---- SGR ---- */

/* Parse an extended colour (38/48/58) starting at params[*i]; advances *i. */
static bool ext_color(Term *t, int *i, uint32_t *out)
{
	int k = *i;
	int nsub = 0;
	while (k + 1 + nsub < t->nparams && t->sub[k + 1 + nsub])
		nsub++;
	if (nsub > 0) { /* colon form: 38:2:[cs]:r:g:b or 38:5:n */
		int type = t->params[k + 1];
		*i = k + nsub;
		if (type == 5 && nsub >= 2) {
			*out = COL_PAL(t->params[k + 2] & 0xff);
			return true;
		}
		if (type == 2 && nsub >= 4) {
			int o = nsub >= 5 ? k + 3 : k + 2;
			*out = COL_RGB(t->params[o] & 0xff, t->params[o + 1] & 0xff, t->params[o + 2] & 0xff);
			return true;
		}
		return false;
	}
	if (k + 1 >= t->nparams)
		return false;
	if (t->params[k + 1] == 5 && k + 2 < t->nparams) {
		*out = COL_PAL(t->params[k + 2] & 0xff);
		*i = k + 2;
		return true;
	}
	if (t->params[k + 1] == 2 && k + 4 < t->nparams) {
		*out = COL_RGB(t->params[k + 2] & 0xff, t->params[k + 3] & 0xff, t->params[k + 4] & 0xff);
		*i = k + 4;
		return true;
	}
	*i = t->nparams;
	return false;
}

static void sgr(Term *t)
{
	if (t->nparams == 0) {
		t->pen.attr = 0;
		t->pen.fg = t->pen.bg = COL_DEFAULT;
		return;
	}
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		uint32_t col;
		switch (p) {
		case 0: t->pen.attr = 0; t->pen.fg = t->pen.bg = COL_DEFAULT; break;
		case 1: t->pen.attr |= ATTR_BOLD; break;
		case 2: t->pen.attr |= ATTR_FAINT; break;
		case 3: t->pen.attr |= ATTR_ITALIC; break;
		case 4:
			/* 4:0 turns underline off, 4:n (curly etc.) on */
			if (i + 1 < t->nparams && t->sub[i + 1]) {
				if (t->params[i + 1] == 0)
					t->pen.attr &= ~ATTR_UNDERLINE;
				else
					t->pen.attr |= ATTR_UNDERLINE;
				while (i + 1 < t->nparams && t->sub[i + 1])
					i++;
			} else {
				t->pen.attr |= ATTR_UNDERLINE;
			}
			break;
		case 5: case 6: t->pen.attr |= ATTR_BLINK; break;
		case 7: t->pen.attr |= ATTR_REVERSE; break;
		case 8: t->pen.attr |= ATTR_INVISIBLE; break;
		case 9: t->pen.attr |= ATTR_STRIKE; break;
		case 21: t->pen.attr |= ATTR_UNDERLINE; break;
		case 22: t->pen.attr &= ~(ATTR_BOLD | ATTR_FAINT); break;
		case 23: t->pen.attr &= ~ATTR_ITALIC; break;
		case 24: t->pen.attr &= ~ATTR_UNDERLINE; break;
		case 25: t->pen.attr &= ~ATTR_BLINK; break;
		case 27: t->pen.attr &= ~ATTR_REVERSE; break;
		case 28: t->pen.attr &= ~ATTR_INVISIBLE; break;
		case 29: t->pen.attr &= ~ATTR_STRIKE; break;
		case 38: if (ext_color(t, &i, &col)) t->pen.fg = col; break;
		case 39: t->pen.fg = COL_DEFAULT; break;
		case 48: if (ext_color(t, &i, &col)) t->pen.bg = col; break;
		case 49: t->pen.bg = COL_DEFAULT; break;
		case 58: ext_color(t, &i, &col); break; /* underline colour: ignored */
		default:
			if (p >= 30 && p <= 37) t->pen.fg = COL_PAL(p - 30);
			else if (p >= 40 && p <= 47) t->pen.bg = COL_PAL(p - 40);
			else if (p >= 90 && p <= 97) t->pen.fg = COL_PAL(p - 90 + 8);
			else if (p >= 100 && p <= 107) t->pen.bg = COL_PAL(p - 100 + 8);
			break;
		}
	}
}

/* ---- modes ---- */

static void set_mode(Term *t, bool on)
{
	for (int i = 0; i < imax(t->nparams, 1); i++) {
		int p = t->nparams ? t->params[i] : 0;
		if (t->priv == '?') {
			switch (p) {
			case 1: t->m_appcursor = on; break;
			case 6: t->m_origin = on; moveto(t, 0, on ? t->top : 0); break;
			case 7: t->m_autowrap = on; break;
			case 25: t->m_cursor_visible = on; t->dirty = true; break;
			case 47: case 1047: set_alt(t, on); break;
			case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
			case 1049:
				if (on) {
					save_cursor(t);
					set_alt(t, true);
				} else {
					set_alt(t, false);
					restore_cursor(t);
				}
				break;
			case 1000: case 1002: case 1003: t->mouse_mode = on ? p : 0; break;
			case 1006: t->mouse_sgr = on; break;
			case 2004: t->m_bracketed_paste = on; break;
			case 2026: t->m_sync = on; break;
			default: break;
			}
		} else if (!t->priv) {
			switch (p) {
			case 4: t->m_insert = on; break;
			case 20: t->m_lnm = on; break;
			default: break;
			}
		}
	}
}

/* ---- CSI ---- */

#define P(i, d) ((i) < t->nparams && t->params[i] ? t->params[i] : (d))
#define P0(i)   ((i) < t->nparams ? t->params[i] : 0)

static void csi_dispatch(Term *t, uint32_t c)
{
	char buf[64];
	int n = P(0, 1);

	if (t->inter == '$' && c == 'p' && t->priv == '?') { /* DECRQM */
		int m = P0(0), v = 0;
		switch (m) {
		case 1: v = t->m_appcursor; break;
		case 7: v = t->m_autowrap; break;
		case 25: v = t->m_cursor_visible; break;
		case 1049: v = t->altscreen; break;
		case 2004: v = t->m_bracketed_paste; break;
		case 2026: v = t->m_sync; break;
		default: v = -1; break;
		}
		snprintf(buf, sizeof buf, "\x1b[?%d;%d$y", m, v < 0 ? 0 : v ? 1 : 2);
		reply(t, buf);
		return;
	}
	if (t->inter == ' ') {
		if (c == 'q')
			t->cursor_style = P0(0), t->dirty = true;
		return;
	}
	if (t->inter)
		return;

	switch (c) {
	case '@': if (!t->priv) insert_blank(t, n); break;
	case 'A': {
		int y = t->cy - n;
		if (t->cy >= t->top && y < t->top) y = t->top;
		moveto(t, t->cx, y);
		break;
	}
	case 'B': case 'e': {
		int y = t->cy + n;
		if (t->cy <= t->bot && y > t->bot) y = t->bot;
		moveto(t, t->cx, y);
		break;
	}
	case 'C': case 'a': moveto(t, t->cx + n, t->cy); break;
	case 'D': moveto(t, t->cx - n, t->cy); break;
	case 'E': moveto(t, 0, t->cy + n); break;
	case 'F': moveto(t, 0, t->cy - n); break;
	case 'G': case '`': moveto(t, n - 1, t->cy); break;
	case 'H': case 'f':
		moveto(t, P(1, 1) - 1, P(0, 1) - 1 + (t->m_origin ? t->top : 0));
		break;
	case 'd': moveto(t, t->cx, n - 1 + (t->m_origin ? t->top : 0)); break;
	case 'I': tab(t, n); break;
	case 'Z': tab(t, -n); break;
	case 'J': erase_display(t, P0(0)); break;
	case 'K':
		switch (P0(0)) {
		case 0: clear_line_range(t, t->cy, t->cx, t->cols); break;
		case 1: clear_line_range(t, t->cy, 0, t->cx + 1); break;
		case 2: clear_line_range(t, t->cy, 0, t->cols); break;
		}
		break;
	case 'L':
		if (t->cy >= t->top && t->cy <= t->bot) {
			scroll_down(t, t->cy, t->bot, n);
			t->cx = 0;
			t->wrapnext = false;
		}
		break;
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bot) {
			scroll_up(t, t->cy, t->bot, n, false);
			t->cx = 0;
			t->wrapnext = false;
		}
		break;
	case 'P': delete_chars(t, n); break;
	case 'S': if (!t->priv) scroll_up(t, t->top, t->bot, n, false); break;
	case 'T': if (!t->priv && t->nparams <= 1) scroll_down(t, t->top, t->bot, n); break;
	case 'X': clear_line_range(t, t->cy, t->cx, t->cx + n); break;
	case 'b':
		if (t->lastcp)
			for (int i = 0; i < imin(n, 65535); i++)
				tputc(t, t->lastcp);
		break;
	case 'c':
		if (!t->priv && P0(0) == 0)
			reply(t, "\x1b[?62;22c");
		else if (t->priv == '>')
			reply(t, "\x1b[>1;10;0c");
		break;
	case 'g':
		if (P0(0) == 0) t->tabs[t->cx] = false;
		else if (P0(0) == 3) memset(t->tabs, 0, (size_t)t->cols);
		break;
	case 'h': set_mode(t, true); break;
	case 'l': set_mode(t, false); break;
	case 'm': if (!t->priv) sgr(t); break;
	case 'n':
		if (t->priv)
			break;
		if (P0(0) == 5) {
			reply(t, "\x1b[0n");
		} else if (P0(0) == 6) {
			snprintf(buf, sizeof buf, "\x1b[%d;%dR",
			         t->cy + 1 - (t->m_origin ? t->top : 0), t->cx + 1);
			reply(t, buf);
		}
		break;
	case 'r':
		if (!t->priv) {
			int top = P(0, 1) - 1, bot = P(1, t->rows) - 1;
			top = clamp(top, 0, t->rows - 1);
			bot = clamp(bot, 0, t->rows - 1);
			if (top < bot) {
				t->top = top;
				t->bot = bot;
				moveto(t, 0, t->m_origin ? top : 0);
			}
		}
		break;
	case 's': if (!t->priv) save_cursor(t); break;
	case 'u': if (!t->priv) restore_cursor(t); break;
	default: break;
	}
}

/* ---- OSC ---- */

static void osc_dispatch(Term *t)
{
	t->osc[t->osclen] = 0;
	char *semi = strchr(t->osc, ';');
	if (!semi)
		return;
	int num = atoi(t->osc);
	char *arg = semi + 1;
	switch (num) {
	case 0: case 2:
		snprintf(t->title, sizeof t->title, "%s", arg);
		t->title_changed = true;
		break;
	case 10: case 11:
		if (!strcmp(arg, "?")) {
			uint32_t c = num == 10 ? term_default_fg : term_default_bg;
			char buf[64];
			unsigned r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
			snprintf(buf, sizeof buf, "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
			         num, r, r, g, g, b, b);
			reply(t, buf);
		}
		break;
	default: break;
	}
}

static void osc_append(Term *t, uint32_t c)
{
	if (t->osclen > (int)sizeof t->osc - 5)
		return;
	char *p = t->osc + t->osclen;
	if (c < 0x80) {
		*p++ = (char)c;
	} else if (c < 0x800) {
		*p++ = (char)(0xc0 | c >> 6);
		*p++ = (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		*p++ = (char)(0xe0 | c >> 12);
		*p++ = (char)(0x80 | (c >> 6 & 0x3f));
		*p++ = (char)(0x80 | (c & 0x3f));
	} else {
		*p++ = (char)(0xf0 | c >> 18);
		*p++ = (char)(0x80 | (c >> 12 & 0x3f));
		*p++ = (char)(0x80 | (c >> 6 & 0x3f));
		*p++ = (char)(0x80 | (c & 0x3f));
	}
	t->osclen = (int)(p - t->osc);
}

/* ---- ESC ---- */

static void full_reset(Term *t)
{
	set_alt(t, false);
	Cell b;
	memset(&b, 0, sizeof b);
	for (int y = 0; y < t->rows; y++) {
		line_clear(t->main_lines[y], 0, t->cols, b);
		line_clear(t->alt_lines[y], 0, t->cols, b);
	}
	reset_tabs(t);
	reset_state(t);
	t->dirty = true;
}

static void esc_dispatch(Term *t, uint32_t c)
{
	t->state = S_GROUND;
	switch (c) {
	case '[':
		t->state = S_CSI;
		t->np = 0;
		t->params[0] = 0;
		t->sub[0] = false;
		t->had_digit = false;
		t->priv = t->inter = 0;
		break;
	case ']': t->state = S_OSC; t->osclen = 0; break;
	case 'P': case 'X': case '^': case '_': t->state = S_STR; break;
	case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
		t->inter = (char)c;
		t->state = S_ESC_INTER;
		break;
	case '7': save_cursor(t); break;
	case '8': restore_cursor(t); break;
	case 'D': index_down(t); break;
	case 'E': index_down(t); t->cx = 0; break;
	case 'H': t->tabs[t->cx] = true; break;
	case 'M': index_up(t); break;
	case 'c': full_reset(t); break;
	case '=': t->m_appkeypad = true; break;
	case '>': t->m_appkeypad = false; break;
	default: break;
	}
}

static void control(Term *t, uint32_t c)
{
	switch (c) {
	case 0x08:
		if (t->cx > 0)
			t->cx--;
		t->wrapnext = false;
		break;
	case 0x09: tab(t, 1); break;
	case 0x0a: case 0x0b: case 0x0c:
		index_down(t);
		if (t->m_lnm)
			t->cx = 0;
		break;
	case 0x0d: t->cx = 0; t->wrapnext = false; break;
	case 0x0e: t->gl = 1; break;
	case 0x0f: t->gl = 0; break;
	case 0x18: case 0x1a: t->state = S_GROUND; break;
	case 0x1b: t->state = S_ESC; t->inter = 0; break;
	default: break;
	}
}

static void handle(Term *t, uint32_t c)
{
	if (t->state == S_OSC || t->state == S_STR) {
		if (c == 0x07 || c == 0x1b) {
			if (t->state == S_OSC)
				osc_dispatch(t);
			/* ESC \ (ST): the backslash is swallowed by esc_dispatch */
			t->state = c == 0x1b ? S_ESC : S_GROUND;
		} else if (c == 0x18 || c == 0x1a) {
			t->state = S_GROUND;
		} else if (t->state == S_OSC) {
			osc_append(t, c);
		}
		return;
	}
	if (c < 0x20 || c == 0x7f) {
		control(t, c);
		return;
	}
	switch (t->state) {
	case S_GROUND:
		if (c >= 0x80 && c < 0xa0)
			return;
		tputc(t, c);
		break;
	case S_ESC:
		esc_dispatch(t, c);
		break;
	case S_ESC_INTER:
		if (c >= 0x20 && c <= 0x2f)
			return;
		if (t->inter == '(')
			t->charsets[0] = (int)c;
		else if (t->inter == ')')
			t->charsets[1] = (int)c;
		t->state = S_GROUND;
		break;
	case S_CSI:
		if (c >= '0' && c <= '9') {
			int *p = &t->params[t->np];
			*p = imin(*p * 10 + (int)(c - '0'), 99999);
			t->had_digit = true;
		} else if (c == ';' || c == ':') {
			if (t->np < 31) {
				t->np++;
				t->params[t->np] = 0;
				t->sub[t->np] = c == ':';
			}
		} else if (c >= '<' && c <= '?') {
			t->priv = (char)c;
		} else if (c >= 0x20 && c <= 0x2f) {
			t->inter = (char)c;
		} else if (c >= 0x40 && c <= 0x7e) {
			t->nparams = (t->np > 0 || t->had_digit) ? t->np + 1 : 0;
			t->state = S_GROUND;
			csi_dispatch(t, c);
		} else {
			t->state = S_GROUND;
		}
		break;
	}
}

void term_feed(Term *t, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t b = (uint8_t)s[i];
		if (t->u_need) {
			if ((b & 0xc0) == 0x80) {
				t->u_cp = t->u_cp << 6 | (b & 0x3f);
				if (--t->u_need == 0) {
					uint32_t cp = t->u_cp;
					if (cp < t->u_min || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000))
						cp = 0xfffd;
					handle(t, cp);
				}
				continue;
			}
			t->u_need = 0;
			handle(t, 0xfffd);
		}
		if (b < 0x80) {
			handle(t, b);
		} else if ((b & 0xe0) == 0xc0) {
			t->u_cp = b & 0x1f; t->u_need = 1; t->u_min = 0x80;
		} else if ((b & 0xf0) == 0xe0) {
			t->u_cp = b & 0x0f; t->u_need = 2; t->u_min = 0x800;
		} else if ((b & 0xf8) == 0xf0) {
			t->u_cp = b & 0x07; t->u_need = 3; t->u_min = 0x10000;
		} else {
			handle(t, 0xfffd);
		}
	}
}

/* ---- resize ---- */

static void resize_screen(Term *t, Line ***plines, int oldrows, int cols, int rows,
                          int *cy, bool is_main)
{
	Line **lines = *plines;
	Cell b;
	memset(&b, 0, sizeof b);
	for (int i = 0; i < oldrows; i++)
		lines[i] = line_resize(lines[i], cols);

	if (rows < oldrows) {
		/* Drop lines from the top only as far as needed to keep the cursor
		 * on screen; the rest come off the bottom. */
		int shift = imax(0, *cy - (rows - 1));
		for (int i = 0; i < shift; i++) {
			if (is_main)
				sb_store(t, lines[i]);
			free(lines[i]);
		}
		for (int i = shift + rows; i < oldrows; i++)
			free(lines[i]);
		memmove(lines, lines + shift, (size_t)rows * sizeof(Line *));
		*cy -= shift;
		lines = realloc(lines, (size_t)rows * sizeof(Line *));
	} else if (rows > oldrows) {
		lines = realloc(lines, (size_t)rows * sizeof(Line *));
		int pull = is_main ? imin(t->sb_count, rows - oldrows) : 0;
		memmove(lines + pull, lines, (size_t)oldrows * sizeof(Line *));
		for (int i = pull - 1; i >= 0; i--)
			lines[i] = line_resize(sb_pop(t), cols);
		for (int i = oldrows + pull; i < rows; i++)
			lines[i] = line_new(cols, b);
		*cy += pull;
	}
	*plines = lines;
}

void term_resize(Term *t, int cols, int rows)
{
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	if (cols == t->cols && rows == t->rows)
		return;
	int oldrows = t->rows;
	int dummy = 0;
	int *main_cy = t->altscreen ? &t->saved[0].y : &t->cy;
	int *alt_cy = t->altscreen ? &t->cy : &dummy;

	resize_screen(t, &t->main_lines, oldrows, cols, rows, main_cy, true);
	resize_screen(t, &t->alt_lines, oldrows, cols, rows, alt_cy, false);
	t->lines = t->altscreen ? t->alt_lines : t->main_lines;

	t->tabs = realloc(t->tabs, (size_t)cols);
	int oldcols = t->cols;
	t->cols = cols;
	t->rows = rows;
	for (int i = oldcols; i < cols; i++)
		t->tabs[i] = (i % 8 == 0);
	t->top = 0;
	t->bot = rows - 1;
	t->cx = clamp(t->cx, 0, cols - 1);
	t->cy = clamp(t->cy, 0, rows - 1);
	for (int i = 0; i < 2; i++) {
		t->saved[i].x = clamp(t->saved[i].x, 0, cols - 1);
		t->saved[i].y = clamp(t->saved[i].y, 0, rows - 1);
	}
	t->wrapnext = false;
	t->dirty = true;
}
