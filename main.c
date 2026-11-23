/* tnelterm: a small terminal emulator with proper complex-script (Khmer)
 * rendering.
 *
 * Layout model ("elastic grid"): the terminal keeps a normal cell grid so
 * cursor addressing matches what programs expect (via wcwidth), but runs of
 * complex-script text are shaped with HarfBuzz and drawn at their natural
 * width.  Simple text snaps back to its grid column whenever the preceding
 * text left room, so tables and prompts stay aligned as much as possible
 * while Khmer is never squeezed into monospace cells. */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "font.h"
#include "term.h"

#define SCROLLBACK 5000

typedef struct {
	int font;    /* -1: procedurally drawn box/block char (gid = codepoint) */
	uint32_t gid;
	float x, yoff;
	int col;     /* cell whose colours apply */
	int w;       /* cells covered (simple glyphs only, else 0) */
	bool bold;
} DItem;

static struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	Term *t;
	int fd;
	pid_t pid;
	int pad;
	float density;
	int scroll;
	bool focused, dirty, running;
	Uint32 pty_event;
	SDL_Semaphore *sem;

	int *rowx; /* rows * (cols + 1) x positions from last layout */
	int rowx_cap, rowx_cols, rowx_rows;

	DItem *items;
	int nitems, items_cap;
	uint32_t *cps, *cls;
	ShapedGlyph *sg;
	int scratch_cap;

	bool selecting, has_sel;
	int sel_r0, sel_c0, sel_r1, sel_c1;
	int mouse_btn, mouse_row, mouse_col;

	char preedit[256];
	int cur_x, cur_y;
	SDL_Rect ime_rect;

	/* frame pacing for pty output */
	bool pty_pending, was_sync;
	Uint64 first_data, last_data, sync_since;

	const char *dump_path;
	Uint64 dump_at;
	bool x11;

	/* font settings, kept for zooming */
	const char *mono, *khmer;
	float size, base_size, kscale, ui_scale;
} G;

/* ---- pty ---- */

static void pty_write(void *ud, const char *s, size_t n)
{
	(void)ud;
	while (n > 0) {
		ssize_t r = write(G.fd, s, n);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				struct pollfd p = { G.fd, POLLOUT, 0 };
				poll(&p, 1, 100);
				continue;
			}
			return;
		}
		s += r;
		n -= (size_t)r;
	}
}

static void send_str(const char *s)
{
	pty_write(NULL, s, strlen(s));
	if (G.scroll) {
		G.scroll = 0;
		G.dirty = true;
	}
}

static void spawn(char **argv, int cols, int rows)
{
	struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
	G.pid = forkpty(&G.fd, NULL, NULL, &ws);
	if (G.pid < 0) {
		perror("forkpty");
		exit(1);
	}
	if (G.pid == 0) {
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("COLUMNS");
		unsetenv("LINES");
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		if (argv && argv[0]) {
			execvp(argv[0], argv);
		} else {
			const char *sh = getenv("SHELL");
			struct passwd *pw = getpwuid(getuid());
			if (!sh && pw)
				sh = pw->pw_shell;
			if (!sh || !*sh)
				sh = "/bin/sh";
			execl(sh, sh, (char *)NULL);
		}
		perror("exec");
		_exit(127);
	}
	fcntl(G.fd, F_SETFL, fcntl(G.fd, F_GETFL) | O_NONBLOCK);
}

/* Wakes the SDL loop when the pty is readable, then waits until the main
 * thread has drained it. */
static int reader_thread(void *ud)
{
	(void)ud;
	struct pollfd p = { G.fd, POLLIN, 0 };
	for (;;) {
		int r = poll(&p, 1, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		SDL_Event e;
		SDL_zero(e);
		e.type = G.pty_event;
		SDL_PushEvent(&e);
		SDL_WaitSemaphore(G.sem);
	}
	return 0;
}

static void read_pty(void)
{
	static char buf[65536];
	size_t total = 0;
	while (total < (1 << 20)) {
		ssize_t n = read(G.fd, buf, sizeof buf);
		if (n > 0) {
			Uint64 now = SDL_GetTicksNS();
			if (!G.pty_pending) {
				G.pty_pending = true;
				G.first_data = now;
			}
			G.last_data = now;
			term_feed(G.t, buf, (size_t)n);
			total += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		G.running = false; /* EOF / EIO: child exited */
		break;
	}
	if (G.t->m_sync && !G.was_sync)
		G.sync_since = SDL_GetTicksNS();
	else if (!G.t->m_sync && G.was_sync)
		G.first_data = 0; /* update finished: draw it right away */
	G.was_sync = G.t->m_sync;
	if (G.t->title_changed) {
		SDL_SetWindowTitle(G.win, G.t->title);
		G.t->title_changed = false;
	}
	if (G.t->sb_added) {
		if (G.scroll)
			G.scroll = SDL_min(G.scroll + G.t->sb_added, G.t->sb_count);
		G.t->sb_added = 0;
	}
	if (G.t->dirty) {
		G.has_sel = G.selecting = false;
		G.dirty = true;
		G.t->dirty = false;
	}
	SDL_SignalSemaphore(G.sem);
}

/* ---- layout ---- */

static const Cell empty_cell;

static inline const Cell *cell_at(const Line *l, int c)
{
	return (l && c < l->cols) ? &l->c[c] : &empty_cell;
}

/* Scripts that need shaping and shouldn't be forced into cells. */
static bool is_complex(const Cell *c)
{
	if (c->ncomb)
		return true;
	uint32_t u = c->cp;
	if (u < 0x0590)
		return false;
	return (u <= 0x08ff) ||                  /* Hebrew, Arabic, Syriac, ... */
	       (u >= 0x0900 && u <= 0x0dff) ||   /* Indic */
	       (u >= 0x0e00 && u <= 0x0fff) ||   /* Thai, Lao, Tibetan */
	       (u >= 0x1000 && u <= 0x109f) ||   /* Myanmar */
	       (u >= 0x1700 && u <= 0x1cff) ||   /* Khmer, Mongolian, Tai, Balinese, ... */
	       (u >= 0xa800 && u <= 0xaaff) ||   /* Javanese, Cham, Tai Viet, ... */
	       (u >= 0xfb1d && u <= 0xfdff) ||   /* presentation forms */
	       (u >= 0xfe70 && u <= 0xfeff);
}

static bool same_style(const Cell *a, const Cell *b)
{
	return a->fg == b->fg && a->bg == b->bg &&
	       (a->attr & ATTR_STYLE) == (b->attr & ATTR_STYLE);
}

static bool is_blank(const Cell *c)
{
	return (c->cp == 0 || c->cp == ' ') && !c->ncomb && !(c->attr & ATTR_WDUMMY);
}

static DItem *push_item(void)
{
	if (G.nitems == G.items_cap) {
		G.items_cap = G.items_cap ? G.items_cap * 2 : 512;
		G.items = realloc(G.items, (size_t)G.items_cap * sizeof *G.items);
	}
	return &G.items[G.nitems++];
}

static bool is_box(uint32_t cp)
{
	return cp >= 0x2500 && cp <= 0x259f;
}

/* Lays out one row into G.items and fills X[0..cols] with cell x positions. */
static void layout_row(const Line *l, int cols, int *X)
{
	int cw = fm.cw;
	G.nitems = 0;
	float pen = 0;
	bool flow = false; /* directly after shaped text: don't snap mid-word */
	int c = 0;
	while (c < cols) {
		const Cell *cell = cell_at(l, c);
		bool bold = cell->attr & ATTR_BOLD;
		if (is_blank(cell)) {
			int k = c;
			while (k < cols && is_blank(cell_at(l, k)))
				k++;
			if (k - c == 1 && k < cols) {
				/* a single space is prose: keep flowing */
				X[c++] = (int)lroundf(pen);
				pen += (float)cw;
				continue;
			}
			/* Runs of blanks (columns, tabs, padding) absorb the slack so
			 * the next field lands back on its grid column if there's room. */
			for (; c < k; c++) {
				X[c] = (int)lroundf(pen);
				pen = fmaxf(pen + (float)cw, (float)((c + 1) * cw));
			}
			flow = false;
			continue;
		}
		if (!is_complex(cell)) {
			/* box drawing always snaps so TUI borders line up */
			bool snap = !flow || is_box(cell->cp);
			float x = snap ? fmaxf(pen, (float)(c * cw)) : pen;
			int w = (cell->attr & ATTR_WIDE) && c + 1 < cols ? 2 : 1;
			X[c] = (int)lroundf(x);
			if (w == 2)
				X[c + 1] = (int)lroundf(x + cw);
			if (cell->cp > ' ' && !(cell->attr & ATTR_WDUMMY)) {
				DItem *it = push_item();
				it->x = x;
				it->yoff = 0;
				it->col = c;
				it->w = w;
				it->bold = bold;
				if (is_box(cell->cp)) {
					it->font = -1;
					it->gid = cell->cp;
				} else {
					it->font = font_for_cp(cell->cp);
					it->gid = font_glyph_index(it->font, cell->cp);
				}
			}
			pen = x + (float)(w * cw);
			c += w;
			continue;
		}

		/* Complex segment: same style + font, may span single spaces. */
		int fnt = font_for_cp(cell->cp);
		int end = c + 1;
		while (end < cols) {
			const Cell *n = cell_at(l, end);
			if (!same_style(n, cell))
				break;
			if (is_complex(n) && font_for_cp(n->cp) == fnt) {
				end++;
				continue;
			}
			if (is_blank(n)) {
				int k = end;
				while (k < cols && is_blank(cell_at(l, k)) && same_style(cell_at(l, k), cell))
					k++;
				const Cell *nk = cell_at(l, k);
				if (k == end + 1 && k < cols && is_complex(nk) && same_style(nk, cell) &&
				    font_for_cp(nk->cp) == fnt) {
					end = k;
					continue;
				}
			}
			break;
		}

		int n = 0;
		for (int k = c; k < end; k++) {
			const Cell *e = cell_at(l, k);
			G.cps[n] = e->cp ? e->cp : ' ';
			G.cls[n++] = (uint32_t)k;
			for (int j = 0; j < e->ncomb; j++) {
				G.cps[n] = e->comb[j];
				G.cls[n++] = (uint32_t)k;
			}
		}
		int ng = font_shape(fnt, G.cps, G.cls, n, G.sg, G.scratch_cap);

		float x0 = flow ? pen : fmaxf(pen, (float)(c * cw)), x = x0;
		for (int k = c; k < end; k++)
			X[k] = INT_MIN;
		for (int i = 0; i < ng; i++) {
			int col = (int)G.sg[i].cluster;
			if (col < c || col >= end)
				col = c;
			if (X[col] == INT_MIN)
				X[col] = (int)lroundf(x);
			DItem *it = push_item();
			it->font = fnt;
			it->gid = G.sg[i].gid;
			it->x = x + G.sg[i].x_off;
			it->yoff = G.sg[i].y_off;
			it->col = col;
			it->w = 0;
			it->bold = bold;
			x += G.sg[i].x_adv;
		}
		/* Cells merged into a cluster share its start position. */
		int prev = (int)lroundf(x0);
		for (int k = c; k < end; k++) {
			if (X[k] == INT_MIN || X[k] < prev)
				X[k] = prev;
			prev = X[k];
		}
		pen = x;
		flow = true;
		c = end;
	}
	X[cols] = (int)lroundf(fmaxf(pen, (float)(cols * cw)));
}

/* ---- drawing ---- */

static void set_color(uint32_t rgb, Uint8 a)
{
	SDL_SetRenderDrawColor(G.ren, rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff, a);
}

static void fill(int x, int y, int w, int h, uint32_t rgb)
{
	if (w <= 0 || h <= 0)
		return;
	set_color(rgb, 255);
	SDL_FRect r = { (float)x, (float)y, (float)w, (float)h };
	SDL_RenderFillRect(G.ren, &r);
}

static void rect(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	SDL_FRect r = { (float)x, (float)y, (float)w, (float)h };
	SDL_RenderFillRect(G.ren, &r);
}

#define B(l, r, u, d) ((l) | (r) << 2 | (u) << 4 | (d) << 6)
static const uint8_t box_table[128] = {
	/* 2500 */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
	/* 2504 */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
	/* 2508 */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
	/* 250C */ B(0,1,0,1), B(0,2,0,1), B(0,1,0,2), B(0,2,0,2),
	/* 2510 */ B(1,0,0,1), B(2,0,0,1), B(1,0,0,2), B(2,0,0,2),
	/* 2514 */ B(0,1,1,0), B(0,2,1,0), B(0,1,2,0), B(0,2,2,0),
	/* 2518 */ B(1,0,1,0), B(2,0,1,0), B(1,0,2,0), B(2,0,2,0),
	/* 251C */ B(0,1,1,1), B(0,2,1,1), B(0,1,2,1), B(0,1,1,2),
	/* 2520 */ B(0,1,2,2), B(0,2,2,1), B(0,2,1,2), B(0,2,2,2),
	/* 2524 */ B(1,0,1,1), B(2,0,1,1), B(1,0,2,1), B(1,0,1,2),
	/* 2528 */ B(1,0,2,2), B(2,0,2,1), B(2,0,1,2), B(2,0,2,2),
	/* 252C */ B(1,1,0,1), B(2,1,0,1), B(1,2,0,1), B(2,2,0,1),
	/* 2530 */ B(1,1,0,2), B(2,1,0,2), B(1,2,0,2), B(2,2,0,2),
	/* 2534 */ B(1,1,1,0), B(2,1,1,0), B(1,2,1,0), B(2,2,1,0),
	/* 2538 */ B(1,1,2,0), B(2,1,2,0), B(1,2,2,0), B(2,2,2,0),
	/* 253C */ B(1,1,1,1), B(2,1,1,1), B(1,2,1,1), B(2,2,1,1),
	/* 2540 */ B(1,1,2,1), B(1,1,1,2), B(1,1,2,2), B(2,1,2,1),
	/* 2544 */ B(1,2,2,1), B(2,1,1,2), B(1,2,1,2), B(2,2,2,1),
	/* 2548 */ B(2,2,1,2), B(2,1,2,2), B(1,2,2,2), B(2,2,2,2),
	/* 254C */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
	/* 2550 */ B(3,3,0,0), B(0,0,3,3), B(0,3,0,1), B(0,1,0,3),
	/* 2554 */ B(0,3,0,3), B(3,0,0,1), B(1,0,0,3), B(3,0,0,3),
	/* 2558 */ B(0,3,1,0), B(0,1,3,0), B(0,3,3,0), B(3,0,1,0),
	/* 255C */ B(1,0,3,0), B(3,0,3,0), B(0,3,1,1), B(0,1,3,3),
	/* 2560 */ B(0,3,3,3), B(3,0,1,1), B(1,0,3,3), B(3,0,3,3),
	/* 2564 */ B(3,3,0,1), B(1,1,0,3), B(3,3,0,3), B(3,3,1,0),
	/* 2568 */ B(1,1,3,0), B(3,3,3,0), B(3,3,1,1), B(1,1,3,3),
	/* 256C */ B(3,3,3,3), B(0,1,0,1), B(1,0,0,1), B(1,0,1,0),
	/* 2570 */ B(0,1,1,0), 0, 0, 0,
	/* 2574 */ B(1,0,0,0), B(0,0,1,0), B(0,1,0,0), B(0,0,0,1),
	/* 2578 */ B(2,0,0,0), B(0,0,2,0), B(0,2,0,0), B(0,0,0,2),
	/* 257C */ B(1,2,0,0), B(0,0,1,2), B(2,1,0,0), B(0,0,2,1),
};

static void hseg(int x0, int x1, int cy, int wgt, int t)
{
	if (wgt == 3) {
		rect(x0, cy - t / 2 - t, x1 - x0, t);
		rect(x0, cy - t / 2 + t, x1 - x0, t);
	} else if (wgt) {
		int th = wgt == 2 ? 2 * t : t;
		rect(x0, cy - th / 2, x1 - x0, th);
	}
}

static void vseg(int y0, int y1, int cx, int wgt, int t)
{
	if (wgt == 3) {
		rect(cx - t / 2 - t, y0, t, y1 - y0);
		rect(cx - t / 2 + t, y0, t, y1 - y0);
	} else if (wgt) {
		int th = wgt == 2 ? 2 * t : t;
		rect(cx - th / 2, y0, th, y1 - y0);
	}
}

/* Box drawing and block elements are drawn as rectangles so they join up
 * across lines regardless of the (Khmer-enlarged) line height. */
static void draw_box(uint32_t cp, int x, int y, int w, int h, uint32_t rgb)
{
	set_color(rgb, 255);
	if (cp < 0x2580) {
		uint8_t b = box_table[cp - 0x2500];
		if (!b)
			return;
		int L = b & 3, R = b >> 2 & 3, U = b >> 4 & 3, D = b >> 6 & 3;
		int t = SDL_max(1, fm.thick);
		int cx = x + w / 2, cy = y + h / 2;
		int thick[4] = { 0, t, 2 * t, 3 * t };
		int hv = thick[SDL_max(U, D)] / 2 + 1, hh = thick[SDL_max(L, R)] / 2 + 1;
		if (!U && !D) hv = 0;
		if (!L && !R) hh = 0;
		hseg(x, cx + hv, cy, L, t);
		hseg(cx - hv, x + w, cy, R, t);
		vseg(y, cy + hh, cx, U, t);
		vseg(cy - hh, y + h, cx, D, t);
		return;
	}
	static const uint8_t quads[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
	if (cp == 0x2580) {
		rect(x, y, w, h / 2);
	} else if (cp <= 0x2588) {
		int hh = h * (int)(cp - 0x2580) / 8;
		rect(x, y + h - hh, w, hh);
	} else if (cp <= 0x258f) {
		rect(x, y, w * (int)(0x2590 - cp) / 8, h);
	} else if (cp == 0x2590) {
		rect(x + w / 2, y, w - w / 2, h);
	} else if (cp <= 0x2593) {
		set_color(rgb, (Uint8)(64 * (cp - 0x2590)));
		rect(x, y, w, h);
	} else if (cp == 0x2594) {
		rect(x, y, w, SDL_max(1, h / 8));
	} else if (cp == 0x2595) {
		rect(x + w - SDL_max(1, w / 8), y, SDL_max(1, w / 8), h);
	} else {
		int q = quads[cp - 0x2596], hw = w / 2, hh = h / 2;
		if (q & 1) rect(x, y, hw, hh);
		if (q & 2) rect(x + hw, y, w - hw, hh);
		if (q & 4) rect(x, y + hh, hw, h - hh);
		if (q & 8) rect(x + hw, y + hh, w - hw, h - hh);
	}
}

static void draw_glyph(int font, uint32_t gid, bool bold, float x, float baseline,
                       uint32_t rgb)
{
	const Glyph *g = font_glyph(G.ren, font, gid, bold);
	if (!g->tex)
		return;
	float s = g->scale;
	SDL_FRect r = {
		roundf(x + g->left * s), roundf(baseline - g->top * s), g->w * s, g->h * s
	};
	if (g->color)
		SDL_SetTextureColorMod(g->tex, 255, 255, 255);
	else
		SDL_SetTextureColorMod(g->tex, rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff);
	SDL_RenderTexture(G.ren, g->tex, NULL, &r);
}

static uint32_t resolve(uint32_t col, bool fg, uint16_t attr)
{
	switch (COL_TYPE(col)) {
	case 1: {
		unsigned i = col & 0xff;
		if (fg && (attr & ATTR_BOLD) && i < 8)
			i += 8;
		return term_palette[i];
	}
	case 2: return col & 0xffffff;
	default: return fg ? term_default_fg : term_default_bg;
	}
}

static uint32_t blend(uint32_t a, uint32_t b, float t)
{
	uint32_t r = 0;
	for (int s = 0; s <= 16; s += 8) {
		float ca = (float)(a >> s & 0xff), cb = (float)(b >> s & 0xff);
		r |= (uint32_t)lroundf(ca + (cb - ca) * t) << s;
	}
	return r;
}

static bool sel_contains(int r, int c)
{
	if (!G.has_sel)
		return false;
	int r0 = G.sel_r0, c0 = G.sel_c0, r1 = G.sel_r1, c1 = G.sel_c1;
	if (r0 > r1 || (r0 == r1 && c0 > c1)) {
		int tr = r0, tc = c0;
		r0 = r1; c0 = c1; r1 = tr; c1 = tc;
	}
	if (r < r0 || r > r1)
		return false;
	if (r == r0 && c < c0)
		return false;
	if (r == r1 && c > c1)
		return false;
	return true;
}

static void render_row(int row, const Line *l, int y)
{
	Term *t = G.t;
	int cols = t->cols;
	int *X = &G.rowx[row * (cols + 1)];
	layout_row(l, cols, X);

	uint32_t fgc[cols], bgc[cols];
	for (int c = 0; c < cols; c++) {
		const Cell *cell = cell_at(l, c);
		uint16_t a = cell->attr;
		uint32_t fg = resolve(cell->fg, true, a), bg = resolve(cell->bg, false, a);
		if (a & ATTR_REVERSE) {
			uint32_t tmp = fg; fg = bg; bg = tmp;
		}
		if (a & ATTR_FAINT)
			fg = blend(fg, bg, 0.45f);
		if (a & ATTR_INVISIBLE)
			fg = bg;
		if (sel_contains(row, c)) {
			uint32_t tmp = fg; fg = bg; bg = tmp;
		}
		fgc[c] = fg;
		bgc[c] = bg;
	}

	/* Cursor extent: the whole visual cluster containing the cursor cell. */
	bool cursor = G.scroll == 0 && t->m_cursor_visible && row == t->cy;
	int lo = 0, hi = 0;
	if (cursor) {
		int c = t->cx;
		lo = c;
		while (lo > 0 && X[lo - 1] == X[c])
			lo--;
		hi = c + 1;
		while (hi < cols && X[hi] == X[c])
			hi++;
		if ((cell_at(l, c)->attr & ATTR_WIDE) && hi == c + 1 && c + 2 <= cols)
			hi = c + 2;
		G.cur_x = G.pad + X[lo];
		G.cur_y = y;
	}
	/* a blank cell may have absorbed slack; keep the cursor one cell wide */
	int cur_w = cursor ? X[hi] - X[lo] : 0;
	if (cursor && hi - lo == 1 && is_blank(cell_at(l, lo)))
		cur_w = SDL_min(cur_w, fm.cw);
	bool block = cursor && G.focused && t->cursor_style <= 2 && !G.preedit[0];
	if (block) {
		for (int c = lo; c < hi; c++) {
			uint32_t tmp = fgc[c]; fgc[c] = bgc[c]; bgc[c] = tmp;
		}
	}

	int ox = G.pad;
	for (int c = 0; c < cols; c++) {
		if (block && c >= lo && c < hi)
			continue;
		if (bgc[c] != term_default_bg)
			fill(ox + X[c], y, X[c + 1] - X[c], fm.ch, bgc[c]);
	}
	if (block) {
		/* fgc/bgc are swapped inside the cursor, so fgc is the cell's bg */
		if (fgc[lo] != term_default_bg)
			fill(ox + X[lo], y, X[hi] - X[lo], fm.ch, fgc[lo]);
		fill(ox + X[lo], y, cur_w, fm.ch, bgc[lo]);
	}

	if (cursor && !block) {
		int cx0 = ox + X[lo], cw = cur_w;
		uint32_t cc = term_default_fg;
		if (G.preedit[0])
			;
		else if (!G.focused) {
			set_color(cc, 255);
			SDL_FRect r = { (float)cx0 + 0.5f, (float)y + 0.5f, (float)cw - 1, (float)fm.ch - 1 };
			SDL_RenderRect(G.ren, &r);
		} else if (t->cursor_style <= 4) {
			fill(cx0, y + fm.ch - 2 * fm.thick, cw, 2 * fm.thick, cc);
		} else {
			fill(cx0, y, SDL_max(2, fm.thick * 2), fm.ch, cc);
		}
	}

	float baseline = (float)(y + fm.ascent);
	for (int i = 0; i < G.nitems; i++) {
		DItem *it = &G.items[i];
		uint32_t fg = fgc[it->col];
		if (it->font < 0) {
			int x0 = ox + X[it->col];
			int x1 = ox + (it->col + it->w <= cols ? X[it->col + it->w] : X[cols]);
			draw_box(it->gid, x0, y, x1 - x0, fm.ch, fg);
			continue;
		}
		float x = ox + it->x;
		if (it->w && it->font != 0) {
			/* centre fallback glyphs in their cells */
			const Glyph *g = font_glyph(G.ren, it->font, it->gid, it->bold);
			float room = (float)(it->w * fm.cw) - g->adv;
			if (room > 0)
				x += roundf(room / 2);
		}
		draw_glyph(it->font, it->gid, it->bold, x, baseline - it->yoff, fg);
	}

	for (int c = 0; c < cols; c++) {
		uint16_t a = cell_at(l, c)->attr;
		int x0 = ox + X[c], w = X[c + 1] - X[c];
		if (a & ATTR_UNDERLINE)
			fill(x0, y + fm.ascent + fm.uline, w, fm.thick, fgc[c]);
		if (a & ATTR_STRIKE)
			fill(x0, y + fm.ascent * 2 / 3, w, fm.thick, fgc[c]);
	}
}

/* Decode UTF-8 into codepoints; returns count. */
static int utf8_decode(const char *s, uint32_t *out, int max)
{
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && n < max) {
		uint32_t c = *p++;
		int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
		if (extra)
			c &= 0x3f >> extra;
		while (extra-- && (*p & 0xc0) == 0x80)
			c = c << 6 | (*p++ & 0x3f);
		out[n++] = c;
	}
	return n;
}

static void render_preedit(void)
{
	uint32_t cps[256], cls[256];
	ShapedGlyph sg[512];
	int n = utf8_decode(G.preedit, cps, 256);
	if (!n)
		return;
	int fnt = 0;
	for (int i = 0; i < n; i++)
		if (cps[i] > 0x7f) {
			fnt = font_for_cp(cps[i]);
			break;
		}
	for (int i = 0; i < n; i++)
		cls[i] = (uint32_t)i;
	int ng = font_shape(fnt, cps, cls, n, sg, 512);
	float w = 0;
	for (int i = 0; i < ng; i++)
		w += sg[i].x_adv;
	int x = G.cur_x, y = G.cur_y;
	fill(x, y, (int)ceilf(w), fm.ch, blend(term_default_bg, term_default_fg, 0.15f));
	float pen = (float)x;
	for (int i = 0; i < ng; i++) {
		draw_glyph(fnt, sg[i].gid, false, pen + sg[i].x_off,
		           (float)(y + fm.ascent) - sg[i].y_off, term_default_fg);
		pen += sg[i].x_adv;
	}
	fill(x, y + fm.ascent + fm.uline, (int)ceilf(w), fm.thick, term_default_fg);
}

static void ensure_buffers(void)
{
	Term *t = G.t;
	int need = t->rows * (t->cols + 1);
	if (need > G.rowx_cap) {
		G.rowx = realloc(G.rowx, (size_t)need * sizeof(int));
		G.rowx_cap = need;
	}
	if (t->cols != G.rowx_cols || t->rows != G.rowx_rows) {
		/* plain grid until the next layout pass fills in real positions */
		for (int r = 0; r < t->rows; r++)
			for (int c = 0; c <= t->cols; c++)
				G.rowx[r * (t->cols + 1) + c] = c * fm.cw;
		G.rowx_cols = t->cols;
		G.rowx_rows = t->rows;
	}
	int sc = t->cols * (MAXCOMB + 1) * 2 + 16;
	if (sc > G.scratch_cap) {
		G.cps = realloc(G.cps, (size_t)sc * sizeof(uint32_t));
		G.cls = realloc(G.cls, (size_t)sc * sizeof(uint32_t));
		G.sg = realloc(G.sg, (size_t)sc * sizeof(ShapedGlyph));
		G.scratch_cap = sc;
	}
}

static void render(void)
{
	ensure_buffers();
	set_color(term_default_bg, 255);
	SDL_RenderClear(G.ren);
	G.cur_x = -1;
	for (int r = 0; r < G.t->rows; r++)
		render_row(r, term_view_line(G.t, r, G.scroll), G.pad + r * fm.ch);
	if (G.preedit[0] && G.cur_x >= 0)
		render_preedit();

	if (G.cur_x >= 0) {
		SDL_Rect ir = { (int)(G.cur_x / G.density), (int)(G.cur_y / G.density),
		                (int)(fm.cw / G.density), (int)(fm.ch / G.density) };
		if (memcmp(&ir, &G.ime_rect, sizeof ir)) {
			G.ime_rect = ir;
			SDL_SetTextInputArea(G.win, &ir, 0);
		}
	}

	if (G.dump_path && SDL_GetTicks() >= G.dump_at) {
		SDL_Surface *s = SDL_RenderReadPixels(G.ren, NULL);
		if (s) {
			SDL_SavePNG(s, G.dump_path);
			SDL_DestroySurface(s);
		}
		G.running = false;
	}
	SDL_RenderPresent(G.ren);
}

/* ---- frame pacing ----
 * Programs often emit one screen update in several writes (vim flushes every
 * ~2KB). Drawing between them shows half-finished frames, which flickers.
 * Wait until output goes quiet briefly, but never longer than a frame; while
 * a program holds synchronized output (mode 2026), wait for it to finish. */

#define QUIET_NS  (3 * SDL_NS_PER_MS)
#define MAXLAT_NS (16 * SDL_NS_PER_MS)
#define SYNC_NS   (150 * SDL_NS_PER_MS)

/* ns until the next frame should be drawn: 0 = now, -1 = nothing to draw */
static Sint64 render_delay(Uint64 now)
{
	if (!G.dirty)
		return -1;
	if (!G.pty_pending)
		return 0;
	Uint64 due;
	if (G.t->m_sync) {
		due = G.sync_since + SYNC_NS;
	} else {
		due = G.last_data + QUIET_NS;
		if (G.first_data + MAXLAT_NS < due)
			due = G.first_data + MAXLAT_NS;
	}
	return now >= due ? 0 : (Sint64)(due - now);
}

/* ---- resize ---- */

static void on_resize(void)
{
	int w, h;
	SDL_GetCurrentRenderOutputSize(G.ren, &w, &h);
	int cols = SDL_max(1, (w - 2 * G.pad) / fm.cw);
	int rows = SDL_max(1, (h - 2 * G.pad) / fm.ch);
	if (cols != G.t->cols || rows != G.t->rows) {
		term_resize(G.t, cols, rows);
		struct winsize ws = { (unsigned short)rows, (unsigned short)cols,
		                      (unsigned short)w, (unsigned short)h };
		ioctl(G.fd, TIOCSWINSZ, &ws);
		G.has_sel = G.selecting = false;
		G.scroll = SDL_min(G.scroll, G.t->sb_count);
	}
	ensure_buffers();
	G.dirty = true;
}

/* ---- font zoom ---- */

static bool load_fonts(float size)
{
	return font_init(G.mono, G.khmer, size * G.ui_scale * 96.0f / 72.0f, G.kscale);
}

/* Change the font size, keeping the window size; the grid reflows to fit. */
static void set_font_size(float size)
{
	size = SDL_clamp(size, 4.0f, 96.0f);
	if (size == G.size)
		return;
	font_done();
	if (!load_fonts(size)) {
		font_done();
		if (!load_fonts(G.size)) {
			fprintf(stderr, "tnelterm: could not reload fonts\n");
			exit(1);
		}
		return;
	}
	G.size = size;
	G.rowx_cols = G.rowx_rows = -1; /* cell width changed: reset positions */
	on_resize();
}

/* ---- selection / clipboard ---- */

static void put_utf8(char **buf, size_t *len, size_t *cap, uint32_t c)
{
	if (*len + 5 > *cap) {
		*cap = *cap ? *cap * 2 : 256;
		*buf = realloc(*buf, *cap);
	}
	char *p = *buf + *len;
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
	*len = (size_t)(p - *buf);
}

static char *selection_text(void)
{
	if (!G.has_sel)
		return NULL;
	int r0 = G.sel_r0, c0 = G.sel_c0, r1 = G.sel_r1, c1 = G.sel_c1;
	if (r0 > r1 || (r0 == r1 && c0 > c1)) {
		int tr = r0, tc = c0;
		r0 = r1; c0 = c1; r1 = tr; c1 = tc;
	}
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (int r = r0; r <= r1; r++) {
		const Line *l = term_view_line(G.t, r, G.scroll);
		int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : G.t->cols - 1;
		size_t line_start = len;
		size_t content_end = len;
		for (int c = a; c <= b; c++) {
			const Cell *cell = cell_at(l, c);
			if (cell->attr & ATTR_WDUMMY)
				continue;
			put_utf8(&buf, &len, &cap, cell->cp ? cell->cp : ' ');
			for (int j = 0; j < cell->ncomb; j++)
				put_utf8(&buf, &len, &cap, cell->comb[j]);
			if (cell->cp && cell->cp != ' ')
				content_end = len;
		}
		bool wrapped = l && l->wrapped && b == G.t->cols - 1;
		if (!wrapped)
			len = content_end > line_start ? content_end : line_start;
		if (r != r1 && !wrapped)
			put_utf8(&buf, &len, &cap, '\n');
	}
	put_utf8(&buf, &len, &cap, 0);
	return buf;
}

static void paste(const char *text)
{
	if (!text || !*text)
		return;
	size_t n = strlen(text);
	char *buf = malloc(n + 1);
	size_t j = 0;
	for (size_t i = 0; i < n; i++) {
		char c = text[i];
		if (c == '\r' && text[i + 1] == '\n')
			continue;
		if (c == '\n')
			c = '\r';
		if (G.t->m_bracketed_paste && c == 0x1b)
			continue;
		buf[j++] = c;
	}
	if (G.t->m_bracketed_paste)
		send_str("\x1b[200~");
	pty_write(NULL, buf, j);
	if (G.t->m_bracketed_paste)
		send_str("\x1b[201~");
	free(buf);
	G.scroll = 0;
	G.dirty = true;
}

/* ---- mouse ---- */

static void mouse_cell(float px, float py, int *row, int *col)
{
	int r = (int)floorf((py - G.pad) / fm.ch);
	r = SDL_clamp(r, 0, G.t->rows - 1);
	int *X = &G.rowx[r * (G.t->cols + 1)];
	float x = px - G.pad;
	int c = 0;
	while (c < G.t->cols - 1 && X[c + 1] <= x)
		c++;
	*row = r;
	*col = c;
}

static void mouse_report(int btn, bool press, bool motion, int row, int col)
{
	SDL_Keymod mod = SDL_GetModState();
	int b = btn;
	if (motion) b += 32;
	if (mod & SDL_KMOD_SHIFT) b += 4;
	if (mod & SDL_KMOD_ALT) b += 8;
	if (mod & SDL_KMOD_CTRL) b += 16;
	char buf[64];
	if (G.t->mouse_sgr) {
		snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", b, col + 1, row + 1, press ? 'M' : 'm');
	} else {
		if (!press)
			b = (b & ~3) | 3;
		if (col + 33 > 255 || row + 33 > 255)
			return;
		snprintf(buf, sizeof buf, "\x1b[M%c%c%c", 32 + b, 33 + col, 33 + row);
	}
	pty_write(NULL, buf, strlen(buf));
}

static bool mouse_reporting(void)
{
	return G.t->mouse_mode && !(SDL_GetModState() & SDL_KMOD_SHIFT);
}

static int sdl_button(int b)
{
	return b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_MIDDLE ? 1 : b == SDL_BUTTON_RIGHT ? 2 : -1;
}

static void on_mouse(SDL_Event *ev)
{
	SDL_ConvertEventToRenderCoordinates(G.ren, ev);
	int row, col;
	switch (ev->type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		mouse_cell(ev->button.x, ev->button.y, &row, &col);
		int b = sdl_button(ev->button.button);
		if (mouse_reporting() && b >= 0) {
			G.mouse_btn = b;
			G.mouse_row = row;
			G.mouse_col = col;
			mouse_report(b, true, false, row, col);
		} else if (ev->button.button == SDL_BUTTON_LEFT) {
			G.selecting = true;
			G.has_sel = false;
			G.sel_r0 = G.sel_r1 = row;
			G.sel_c0 = G.sel_c1 = col;
			if (ev->button.clicks >= 2) { /* select word */
				const Line *l = term_view_line(G.t, row, G.scroll);
				int a = col, z = col;
				while (a > 0 && !is_blank(cell_at(l, a - 1))) a--;
				while (z < G.t->cols - 1 && !is_blank(cell_at(l, z + 1))) z++;
				G.sel_c0 = a;
				G.sel_c1 = z;
				G.has_sel = true;
			}
			G.dirty = true;
		} else if (ev->button.button == SDL_BUTTON_MIDDLE) {
			char *s = SDL_GetPrimarySelectionText();
			paste(s);
			SDL_free(s);
		}
		break;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		mouse_cell(ev->button.x, ev->button.y, &row, &col);
		int b = sdl_button(ev->button.button);
		if (G.t->mouse_mode && G.mouse_btn >= 0 && b >= 0 && !G.selecting) {
			mouse_report(b, false, false, row, col);
			G.mouse_btn = -1;
		} else if (ev->button.button == SDL_BUTTON_LEFT && G.selecting) {
			G.selecting = false;
			if (G.has_sel) {
				char *s = selection_text();
				if (s)
					SDL_SetPrimarySelectionText(s);
				free(s);
			}
		}
		break;
	}
	case SDL_EVENT_MOUSE_MOTION:
		mouse_cell(ev->motion.x, ev->motion.y, &row, &col);
		if (G.selecting) {
			if (row != G.sel_r1 || col != G.sel_c1 || !G.has_sel) {
				G.sel_r1 = row;
				G.sel_c1 = col;
				G.has_sel = true;
				G.dirty = true;
			}
		} else if (G.t->mouse_mode && (row != G.mouse_row || col != G.mouse_col)) {
			G.mouse_row = row;
			G.mouse_col = col;
			if (G.mouse_btn >= 0 && G.t->mouse_mode >= 1002)
				mouse_report(G.mouse_btn, true, true, row, col);
			else if (G.mouse_btn < 0 && G.t->mouse_mode == 1003)
				mouse_report(3, true, true, row, col);
		}
		break;
	case SDL_EVENT_MOUSE_WHEEL: {
		float dy = ev->wheel.y;
		if (dy == 0)
			break;
		if (mouse_reporting()) {
			mouse_cell(ev->wheel.mouse_x, ev->wheel.mouse_y, &row, &col);
			mouse_report(dy > 0 ? 64 : 65, true, false, row, col);
		} else if (G.t->altscreen) {
			const char *seq = dy > 0 ? (G.t->m_appcursor ? "\x1bOA" : "\x1b[A")
			                         : (G.t->m_appcursor ? "\x1bOB" : "\x1b[B");
			for (int i = 0; i < 3; i++)
				pty_write(NULL, seq, 3);
		} else {
			G.scroll = SDL_clamp(G.scroll + (dy > 0 ? 3 : -3), 0, G.t->sb_count);
			G.has_sel = false;
			G.dirty = true;
		}
		break;
	}
	}
}

/* ---- keyboard ---- */

static void on_key(const SDL_KeyboardEvent *k)
{
	SDL_Keymod mod = k->mod;
	bool ctrl = mod & SDL_KMOD_CTRL, shift = mod & SDL_KMOD_SHIFT;
	bool alt = mod & SDL_KMOD_LALT;
	SDL_Keycode key = k->key;
	char buf[32];

	if (ctrl && shift) {
		if (key == SDLK_C) {
			char *s = selection_text();
			if (s)
				SDL_SetClipboardText(s);
			free(s);
			return;
		}
		if (key == SDLK_V) {
			char *s = SDL_GetClipboardText();
			paste(s);
			SDL_free(s);
			return;
		}
	}
	if (ctrl && !shift && !alt) {
		/* Ctrl+Shift+- still sends ^_ (readline undo) */
		if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) {
			set_font_size(G.size + 1);
			return;
		}
		if (key == SDLK_MINUS || key == SDLK_KP_MINUS) {
			set_font_size(G.size - 1);
			return;
		}
		if (key == SDLK_0 || key == SDLK_KP_0) {
			set_font_size(G.base_size);
			return;
		}
	}
	if (shift && !ctrl && !G.t->altscreen) {
		int page = SDL_max(1, G.t->rows - 1);
		if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
			G.scroll += key == SDLK_PAGEUP ? page : -page;
			G.scroll = SDL_clamp(G.scroll, 0, G.t->sb_count);
			G.dirty = true;
			return;
		}
	}
	if (shift && key == SDLK_INSERT) {
		char *s = SDL_GetPrimarySelectionText();
		paste(s);
		SDL_free(s);
		return;
	}

	int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
	const char *esc = alt ? "\x1b" : "";
	char letter = 0;
	int tilde = 0;
	switch (key) {
	case SDLK_RETURN: case SDLK_KP_ENTER:
		snprintf(buf, sizeof buf, "%s\r", esc); send_str(buf); return;
	case SDLK_BACKSPACE:
		snprintf(buf, sizeof buf, "%s%s", esc, ctrl ? "\x08" : "\x7f"); send_str(buf); return;
	case SDLK_TAB:
		send_str(shift ? "\x1b[Z" : alt ? "\x1b\t" : "\t"); return;
	case SDLK_ESCAPE:
		send_str("\x1b"); return;
	case SDLK_UP: letter = 'A'; break;
	case SDLK_DOWN: letter = 'B'; break;
	case SDLK_RIGHT: letter = 'C'; break;
	case SDLK_LEFT: letter = 'D'; break;
	case SDLK_HOME: letter = 'H'; break;
	case SDLK_END: letter = 'F'; break;
	case SDLK_INSERT: tilde = 2; break;
	case SDLK_DELETE: tilde = 3; break;
	case SDLK_PAGEUP: tilde = 5; break;
	case SDLK_PAGEDOWN: tilde = 6; break;
	case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: {
		char l = (char)('P' + (key - SDLK_F1));
		if (m > 1) snprintf(buf, sizeof buf, "\x1b[1;%d%c", m, l);
		else snprintf(buf, sizeof buf, "\x1bO%c", l);
		send_str(buf);
		return;
	}
	case SDLK_F5: tilde = 15; break;
	case SDLK_F6: tilde = 17; break;
	case SDLK_F7: tilde = 18; break;
	case SDLK_F8: tilde = 19; break;
	case SDLK_F9: tilde = 20; break;
	case SDLK_F10: tilde = 21; break;
	case SDLK_F11: tilde = 23; break;
	case SDLK_F12: tilde = 24; break;
	default: break;
	}
	if (letter) {
		if (m > 1) snprintf(buf, sizeof buf, "\x1b[1;%d%c", m, letter);
		else if (G.t->m_appcursor) snprintf(buf, sizeof buf, "\x1bO%c", letter);
		else snprintf(buf, sizeof buf, "\x1b[%c", letter);
		send_str(buf);
		return;
	}
	if (tilde) {
		if (m > 1) snprintf(buf, sizeof buf, "\x1b[%d;%d~", tilde, m);
		else snprintf(buf, sizeof buf, "\x1b[%d~", tilde);
		send_str(buf);
		return;
	}

	/* Ctrl combos. Alt+printable is handled via text input (ESC prefix). */
	if (!ctrl)
		return;
	uint32_t c = (uint32_t)key;
	if (c >= 128 || c == 0) {
		/* non-Latin layout: fall back to the physical key */
		SDL_Scancode sc = k->scancode;
		if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
			c = 'a' + (uint32_t)(sc - SDL_SCANCODE_A);
		else
			return;
	}
	int out;
	if (c >= 'a' && c <= 'z') out = (int)(c & 0x1f);
	else if (c == ' ' || c == '@' || c == '2') out = 0;
	else if (c == '[' || c == '3') out = 27;
	else if (c == '\\' || c == '4') out = 28;
	else if (c == ']' || c == '5') out = 29;
	else if (c == '^' || c == '6') out = 30;
	else if (c == '_' || c == '-' || c == '/' || c == '7') out = 31;
	else if (c == '8') out = 127;
	else return;
	char s[3] = { 0 };
	int n = 0;
	if (alt) s[n++] = 0x1b;
	s[n++] = (char)out;
	pty_write(NULL, s, (size_t)n);
	if (G.scroll) {
		G.scroll = 0;
		G.dirty = true;
	}
}

/* SDL's X11 backend, when it has no input context (e.g. XMODIFIERS names an
 * IM that isn't running), treats XLookupString output as Latin-1 even though
 * Xlib returns UTF-8 in a UTF-8 locale, so non-Latin keys arrive
 * double-encoded.  Undo that: if every codepoint is <= U+00FF and those bytes
 * form valid multi-byte UTF-8, use the bytes directly. */
static void fix_double_utf8(char *text)
{
	unsigned char bytes[64];
	int n = 0;
	bool high = false;
	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		uint32_t c;
		if (*p < 0x80) c = *p++;
		else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
			c = (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f);
			p += 2;
		} else return;
		if (c > 0xff || n == (int)sizeof bytes)
			return;
		high |= c >= 0x80;
		bytes[n++] = (unsigned char)c;
	}
	if (!high)
		return;
	for (int i = 0; i < n;) {
		int len = bytes[i] < 0x80 ? 1 : (bytes[i] & 0xe0) == 0xc0 ? 2 :
		          (bytes[i] & 0xf0) == 0xe0 ? 3 : (bytes[i] & 0xf8) == 0xf0 ? 4 : 0;
		if (!len || i + len > n || (len == 2 && bytes[i] < 0xc2))
			return;
		for (int j = 1; j < len; j++)
			if ((bytes[i + j] & 0xc0) != 0x80)
				return;
		i += len;
	}
	memcpy(text, bytes, (size_t)n);
	text[n] = 0;
}

static void on_text(const char *in)
{
	char text[256];
	snprintf(text, sizeof text, "%s", in);
	if (G.x11)
		fix_double_utf8(text);
	SDL_Keymod mod = SDL_GetModState();
	if (mod & SDL_KMOD_CTRL)
		return;
	if (mod & SDL_KMOD_LALT)
		send_str("\x1b");
	send_str(text);
}

/* ---- main ---- */

static void usage(void)
{
	fprintf(stderr,
	        "usage: tnelterm [-f font] [-k khmer-font] [-K scale] [-s size] [-e command args...]\n"
	        "  -f  monospace font pattern (default: Terminus,monospace)\n"
	        "  -k  Khmer font family (default: Noto Sans Khmer)\n"
	        "  -K  Khmer size relative to the main font (default: auto-match)\n"
	        "  -s  font size in points (default: 12)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	const char *mono = "Terminus,monospace", *khmer = "Noto Sans Khmer";
	float size = 12, kscale = 0;
	char **cmd = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) {
			cmd = &argv[i + 1];
			break;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			mono = argv[++i];
		} else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
			khmer = argv[++i];
		} else if (!strcmp(argv[i], "-K") && i + 1 < argc) {
			kscale = strtof(argv[++i], NULL);
			if (kscale <= 0) usage();
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			size = strtof(argv[++i], NULL);
			if (size <= 0) usage();
		} else {
			usage();
		}
	}

	/* We only speak UTF-8; make sure the child does too. */
	const char *loc = setlocale(LC_CTYPE, "");
	if (!loc || (!strstr(loc, "UTF-8") && !strstr(loc, "utf8"))) {
		setlocale(LC_CTYPE, "C.UTF-8");
		unsetenv("LC_ALL");
		setenv("LC_CTYPE", "C.UTF-8", 1);
	}
	signal(SIGPIPE, SIG_IGN);

	SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
	SDL_SetHint(SDL_HINT_APP_ID, "tnelterm");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	G.win = SDL_CreateWindow("tnelterm", 800, 500,
	                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
	if (!G.win) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	G.ren = SDL_CreateRenderer(G.win, NULL);
	if (!G.ren) {
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		return 1;
	}
	G.x11 = !strcmp(SDL_GetCurrentVideoDriver(), "x11");
	SDL_SetRenderVSync(G.ren, 1);
	SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);

	G.density = SDL_GetWindowPixelDensity(G.win);
	if (G.density <= 0)
		G.density = 1;
	float scale = SDL_GetWindowDisplayScale(G.win);
	if (scale <= 0)
		scale = 1;
	G.mono = mono;
	G.khmer = khmer;
	G.kscale = kscale;
	G.ui_scale = scale;
	G.size = G.base_size = size;
	if (!load_fonts(size))
		return 1;
	G.pad = (int)lroundf(4 * scale);

	int cols = 80, rows = 24;
	SDL_SetWindowSize(G.win, (int)((cols * fm.cw + 2 * G.pad) / G.density),
	                  (int)((rows * fm.ch + 2 * G.pad) / G.density));

	G.t = term_new(cols, rows, SCROLLBACK, pty_write, NULL);
	G.mouse_btn = -1;
	ensure_buffers();
	spawn(cmd, cols, rows);

	G.dump_path = getenv("TNELTERM_DUMP");
	if (G.dump_path) {
		const char *ms = getenv("TNELTERM_DUMP_MS");
		G.dump_at = SDL_GetTicks() + (Uint64)(ms ? atoi(ms) : 1000);
	}

	G.pty_event = SDL_RegisterEvents(1);
	G.sem = SDL_CreateSemaphore(0);
	SDL_DetachThread(SDL_CreateThread(reader_thread, "pty", NULL));

	SDL_ShowWindow(G.win);
	SDL_StartTextInput(G.win);
	G.focused = true;
	G.running = true;
	G.dirty = true;
	on_resize();

	while (G.running) {
		SDL_Event ev;
		Sint64 delay = render_delay(SDL_GetTicksNS());
		bool got;
		if (delay == 0)
			got = SDL_PollEvent(&ev);
		else if (delay > 0)
			got = SDL_WaitEventTimeout(&ev, (Sint32)((delay + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS));
		else if (G.dump_path)
			got = SDL_WaitEventTimeout(&ev, 50);
		else
			got = SDL_WaitEvent(&ev);
		while (got) {
			if (ev.type == G.pty_event) {
				read_pty();
			} else {
				switch (ev.type) {
				case SDL_EVENT_QUIT:
					G.running = false;
					break;
				case SDL_EVENT_KEY_DOWN:
					on_key(&ev.key);
					break;
				case SDL_EVENT_TEXT_INPUT:
					G.preedit[0] = 0;
					on_text(ev.text.text);
					G.dirty = true;
					break;
				case SDL_EVENT_TEXT_EDITING:
					snprintf(G.preedit, sizeof G.preedit, "%s", ev.edit.text ? ev.edit.text : "");
					G.dirty = true;
					break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
				case SDL_EVENT_MOUSE_MOTION:
				case SDL_EVENT_MOUSE_WHEEL:
					on_mouse(&ev);
					break;
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					on_resize();
					break;
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
				case SDL_EVENT_WINDOW_FOCUS_LOST:
					G.focused = ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
					G.dirty = true;
					break;
				case SDL_EVENT_WINDOW_EXPOSED:
					G.dirty = true;
					break;
				default:
					break;
				}
			}
			got = SDL_PollEvent(&ev);
		}
		if ((G.dirty && render_delay(SDL_GetTicksNS()) == 0) || G.dump_path) {
			G.dirty = false;
			G.pty_pending = false;
			render();
		}
	}

	kill(G.pid, SIGHUP);
	SDL_Quit();
	return 0;
}
