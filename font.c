/* Font discovery (fontconfig), rasterisation (FreeType), shaping (HarfBuzz). */
#define _XOPEN_SOURCE 700
#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	FT_Face face;
	hb_font_t *hb;
	char *path;
	int index;
	float scale; /* bitmap strike -> target size */
	bool bitmap;
} Font;

#define MAXFONTS 64

FontMetrics fm;

static FT_Library ftlib;
static Font fonts[MAXFONTS];
static int nfonts;
static int khmer_font = -1;
static float px_size;
static char *fallback_pat;

static int16_t bmp_cache[0x10000]; /* cp -> font+1, 0 = unknown */
static struct { uint32_t cp; int16_t f; } astral[1024];
static int nastral;

typedef struct {
	uint64_t key;
	bool used;
	Glyph g;
} GSlot;
static GSlot *gtab;
static size_t gcap, gcount;

static bool is_khmer(uint32_t cp)
{
	return (cp >= 0x1780 && cp <= 0x17ff) || (cp >= 0x19e0 && cp <= 0x19ff);
}

static int load_font(const char *path, int index)
{
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].index == index && !strcmp(fonts[i].path, path))
			return i;
	if (nfonts == MAXFONTS)
		return -1;
	FT_Face face;
	if (FT_New_Face(ftlib, path, index, &face))
		return -1;
	Font *f = &fonts[nfonts];
	memset(f, 0, sizeof *f);
	f->scale = 1;
	int load_flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
	if (FT_IS_SCALABLE(face)) {
		FT_Set_Char_Size(face, 0, (FT_F26Dot6)lroundf(px_size * 64), 72, 72);
	} else if (face->num_fixed_sizes > 0) {
		/* Bitmap fonts (Terminus, colour emoji) ship fixed-size strikes;
		 * pick the closest. Only colour emoji gets scaled at draw time:
		 * scaling a pixel font just blurs it. */
		int best = 0;
		for (int i = 1; i < face->num_fixed_sizes; i++) {
			FT_Pos d = labs(face->available_sizes[i].y_ppem - (FT_Pos)(px_size * 64));
			FT_Pos bd = labs(face->available_sizes[best].y_ppem - (FT_Pos)(px_size * 64));
			if (d < bd)
				best = i;
		}
		FT_Select_Size(face, best);
		if (FT_HAS_COLOR(face))
			f->scale = px_size / (face->available_sizes[best].y_ppem / 64.0f);
		f->bitmap = true;
		load_flags = FT_LOAD_DEFAULT | FT_LOAD_COLOR;
	} else {
		FT_Done_Face(face);
		return -1;
	}
	f->face = face;
	f->hb = hb_ft_font_create_referenced(face);
	hb_ft_font_set_load_flags(f->hb, load_flags);
	f->path = strdup(path);
	f->index = index;
	return nfonts++;
}

/* Load the best fontconfig match for `pat` that covers cp (if cp != 0). */
static int fc_load(const char *pat, uint32_t cp)
{
	FcPattern *p = FcNameParse((const FcChar8 *)pat);
	if (!p)
		return -1;
	FcCharSet *cs = NULL;
	if (cp) {
		cs = FcCharSetCreate();
		FcCharSetAddChar(cs, cp);
		FcPatternAddCharSet(p, FC_CHARSET, cs);
	}
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcResult res;
	FcFontSet *fs = FcFontSort(NULL, p, FcTrue, NULL, &res);
	int idx = -1;
	for (int i = 0; fs && i < fs->nfont && idx < 0; i++) {
		FcPattern *m = fs->fonts[i];
		if (cp) {
			FcCharSet *mcs;
			if (FcPatternGetCharSet(m, FC_CHARSET, 0, &mcs) != FcResultMatch ||
			    !FcCharSetHasChar(mcs, cp))
				continue;
		}
		FcChar8 *file;
		int index = 0;
		if (FcPatternGetString(m, FC_FILE, 0, &file) != FcResultMatch)
			continue;
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		idx = load_font((const char *)file, index);
		if (idx >= 0 && cp && !FT_Get_Char_Index(fonts[idx].face, cp))
			idx = -1;
	}
	if (fs)
		FcFontSetDestroy(fs);
	FcPatternDestroy(p);
	if (cs)
		FcCharSetDestroy(cs);
	return idx;
}

int font_for_cp(uint32_t cp)
{
	if (cp < 0x10000) {
		if (bmp_cache[cp])
			return bmp_cache[cp] - 1;
	} else {
		for (int i = 0; i < nastral; i++)
			if (astral[i].cp == cp)
				return astral[i].f;
	}
	int r = -1;
	if (is_khmer(cp) && khmer_font >= 0 && FT_Get_Char_Index(fonts[khmer_font].face, cp))
		r = khmer_font;
	for (int i = 0; r < 0 && i < nfonts; i++)
		if (FT_Get_Char_Index(fonts[i].face, cp))
			r = i;
	if (r < 0)
		r = fc_load(fallback_pat, cp);
	if (r < 0)
		r = 0;
	if (cp < 0x10000) {
		bmp_cache[cp] = (int16_t)(r + 1);
	} else if (nastral < (int)(sizeof astral / sizeof astral[0])) {
		astral[nastral].cp = cp;
		astral[nastral++].f = (int16_t)r;
	}
	return r;
}

uint32_t font_glyph_index(int font, uint32_t cp)
{
	return FT_Get_Char_Index(fonts[font].face, cp);
}

int font_shape(int font, const uint32_t *cps, const uint32_t *clusters, int n,
               ShapedGlyph *out, int max)
{
	static hb_buffer_t *buf;
	if (!buf)
		buf = hb_buffer_create();
	hb_buffer_clear_contents(buf);
	hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
	for (int i = 0; i < n; i++)
		hb_buffer_add(buf, cps[i], clusters[i]);
	hb_buffer_guess_segment_properties(buf);
	hb_shape(fonts[font].hb, buf, NULL, 0);

	unsigned len;
	hb_glyph_info_t *inf = hb_buffer_get_glyph_infos(buf, &len);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &len);
	float s = fonts[font].scale / 64.0f;
	int count = (int)len < max ? (int)len : max;
	for (int i = 0; i < count; i++) {
		out[i].gid = inf[i].codepoint;
		out[i].cluster = inf[i].cluster;
		out[i].x_adv = pos[i].x_advance * s;
		out[i].x_off = pos[i].x_offset * s;
		out[i].y_off = pos[i].y_offset * s;
	}
	return count;
}

/* ---- glyph cache ---- */

static void render_glyph(SDL_Renderer *ren, Font *f, uint32_t gid, bool bold, Glyph *g)
{
	memset(g, 0, sizeof *g);
	g->scale = f->scale;
	FT_Int32 flags = f->bitmap ? (FT_LOAD_DEFAULT | FT_LOAD_COLOR)
	                           : (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
	if (FT_Load_Glyph(f->face, gid, flags))
		return;
	FT_GlyphSlot s = f->face->glyph;
	if (bold && s->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_Outline_Embolden(&s->outline, (FT_Pos)(px_size * 64 / 24));
	if (s->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL))
		return;

	FT_Bitmap *b = &s->bitmap;
	/* bitmap fonts can't be emboldened as outlines: smear right by 1px */
	int smear = bold && s->format == FT_GLYPH_FORMAT_BITMAP &&
	            b->pixel_mode != FT_PIXEL_MODE_BGRA;
	g->adv = s->advance.x / 64.0f * f->scale;
	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->w = (int)b->width + smear;
	g->h = (int)b->rows;
	if (!b->width || !g->h)
		return;

	uint8_t *px = malloc((size_t)g->w * g->h * 4);
	int pitch = abs(b->pitch);
	for (int y = 0; y < g->h; y++) {
		const uint8_t *row = b->buffer + (size_t)y * pitch;
		uint8_t *o = px + (size_t)y * g->w * 4;
		for (int x = 0; x < g->w; x++, o += 4) {
			if (x >= (int)b->width) { /* smear column */
				memset(o, 0, 4);
				continue;
			}
			switch (b->pixel_mode) {
			case FT_PIXEL_MODE_GRAY:
				o[0] = o[1] = o[2] = 255;
				o[3] = row[x];
				break;
			case FT_PIXEL_MODE_MONO:
				o[0] = o[1] = o[2] = 255;
				o[3] = (row[x >> 3] >> (7 - (x & 7)) & 1) ? 255 : 0;
				break;
			case FT_PIXEL_MODE_BGRA:
				memcpy(o, row + x * 4, 4);
				g->color = true;
				break;
			default:
				memset(o, 0, 4);
				break;
			}
		}
		if (smear) {
			o = px + (size_t)y * g->w * 4;
			for (int x = g->w - 1; x > 0; x--)
				if (o[(x - 1) * 4 + 3] > o[x * 4 + 3])
					o[x * 4 + 3] = o[(x - 1) * 4 + 3];
		}
	}
	g->tex = SDL_CreateTexture(ren, g->color ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32,
	                           SDL_TEXTUREACCESS_STATIC, g->w, g->h);
	if (g->tex) {
		SDL_UpdateTexture(g->tex, NULL, px, g->w * 4);
		SDL_SetTextureBlendMode(g->tex, g->color ? SDL_BLENDMODE_BLEND_PREMULTIPLIED
		                                         : SDL_BLENDMODE_BLEND);
		if (!f->bitmap)
			SDL_SetTextureScaleMode(g->tex, SDL_SCALEMODE_NEAREST);
	}
	free(px);
}

static size_t ghash(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	return (size_t)k;
}

static void gtab_insert(GSlot *tab, size_t cap, uint64_t key, const Glyph *g)
{
	size_t i = ghash(key) & (cap - 1);
	while (tab[i].used)
		i = (i + 1) & (cap - 1);
	tab[i].used = true;
	tab[i].key = key;
	tab[i].g = *g;
}

const Glyph *font_glyph(SDL_Renderer *ren, int font, uint32_t gid, bool bold)
{
	uint64_t key = (uint64_t)font << 40 | (uint64_t)bold << 32 | gid;
	if (gcap) {
		size_t i = ghash(key) & (gcap - 1);
		while (gtab[i].used) {
			if (gtab[i].key == key)
				return &gtab[i].g;
			i = (i + 1) & (gcap - 1);
		}
	}
	if ((gcount + 1) * 10 > gcap * 7) {
		size_t ncap = gcap ? gcap * 2 : 1024;
		GSlot *nt = calloc(ncap, sizeof *nt);
		for (size_t i = 0; i < gcap; i++)
			if (gtab[i].used)
				gtab_insert(nt, ncap, gtab[i].key, &gtab[i].g);
		free(gtab);
		gtab = nt;
		gcap = ncap;
	}
	Glyph g;
	render_glyph(ren, &fonts[font], gid, bold, &g);
	gtab_insert(gtab, gcap, key, &g);
	gcount++;
	size_t i = ghash(key) & (gcap - 1);
	while (gtab[i].key != key || !gtab[i].used)
		i = (i + 1) & (gcap - 1);
	return &gtab[i].g;
}

/* ---- init ---- */

/* Measure the real vertical extent of tall Khmer stacks so line height can
 * accommodate them; declared font metrics are often way off. */
static void measure_ink(int font, const char *sample, float *top, float *bot)
{
	hb_buffer_t *b = hb_buffer_create();
	hb_buffer_add_utf8(b, sample, -1, 0, -1);
	hb_buffer_guess_segment_properties(b);
	hb_shape(fonts[font].hb, b, NULL, 0);
	unsigned len;
	hb_glyph_info_t *inf = hb_buffer_get_glyph_infos(b, &len);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, &len);
	for (unsigned i = 0; i < len; i++) {
		hb_glyph_extents_t e;
		if (!hb_font_get_glyph_extents(fonts[font].hb, inf[i].codepoint, &e))
			continue;
		float t = (pos[i].y_offset + e.y_bearing) / 64.0f;
		float bt = (pos[i].y_offset + e.y_bearing + e.height) / 64.0f;
		if (t > *top) *top = t;
		if (bt < *bot) *bot = bt;
	}
	hb_buffer_destroy(b);
}

/* Pixels the rasterised glyph for cp rises above the baseline, or 0. Works
 * for bitmap fonts too, where HarfBuzz extents only give the cell box. */
static int ink_top(int font, uint32_t cp)
{
	FT_Face f = fonts[font].face;
	FT_Int32 flags = FT_LOAD_RENDER | (fonts[font].bitmap ? 0 : FT_LOAD_TARGET_LIGHT);
	if (FT_Load_Char(f, cp, flags))
		return 0;
	FT_Bitmap *b = &f->glyph->bitmap;
	for (unsigned y = 0; y < b->rows; y++) {
		const uint8_t *row = b->buffer + (size_t)y * (unsigned)abs(b->pitch);
		for (unsigned x = 0; x < b->width; x++) {
			bool on = b->pixel_mode == FT_PIXEL_MODE_MONO
			          ? (row[x >> 3] >> (7 - (x & 7))) & 1 : row[x] > 64;
			if (on)
				return f->glyph->bitmap_top - (int)y;
		}
	}
	return 0;
}

/* Size Khmer so its consonant body sits a third of the way from the mono
 * font's x-height to its cap height: the proportion Noto Sans Khmer has
 * next to Noto Sans Mono, which reads as balanced. */
static void scale_khmer(float px, float kscale)
{
	float k = kscale;
	if (k <= 0) {
		int xh = ink_top(0, 'x'), cap = ink_top(0, 'H'), body = ink_top(khmer_font, 0x1780);
		if (xh <= 0 || cap <= 0 || body <= 0)
			return;
		k = (2.0f * xh + cap) / 3.0f / body;
		if (k < 0.7f) k = 0.7f;
		if (k > 1.3f) k = 1.3f;
	}
	Font *f = &fonts[khmer_font];
	if (fabsf(k - 1) < 0.01f || !FT_IS_SCALABLE(f->face))
		return;
	FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)lroundf(px * k * 64), 72, 72);
	hb_ft_font_changed(f->hb);
}

void font_done(void)
{
	for (size_t i = 0; i < gcap; i++)
		if (gtab[i].used && gtab[i].g.tex)
			SDL_DestroyTexture(gtab[i].g.tex);
	free(gtab);
	gtab = NULL;
	gcap = gcount = 0;
	for (int i = 0; i < nfonts; i++) {
		hb_font_destroy(fonts[i].hb); /* drops its reference to the face */
		FT_Done_Face(fonts[i].face);
		free(fonts[i].path);
	}
	nfonts = 0;
	khmer_font = -1;
	memset(bmp_cache, 0, sizeof bmp_cache);
	nastral = 0;
	free(fallback_pat);
	fallback_pat = NULL;
	FT_Done_FreeType(ftlib);
	ftlib = NULL;
}

bool font_init(const char *mono, const char *khmer, float px, float kscale)
{
	if (FT_Init_FreeType(&ftlib) || !FcInit())
		return false;
	px_size = px;
	fallback_pat = strdup(mono);

	/* pixelsize steers fontconfig to the right file for bitmap fonts like
	 * Terminus, which ship one file per size */
	char mpat[512];
	snprintf(mpat, sizeof mpat, "%s:pixelsize=%g", mono, (double)px);
	if (fc_load(mpat, 0) != 0) {
		fprintf(stderr, "tnelterm: no font found for '%s'\n", mono);
		return false;
	}
	char kpat[256];
	snprintf(kpat, sizeof kpat, "%s:lang=km", khmer);
	khmer_font = fc_load(kpat, 0x1780);
	if (khmer_font < 0)
		fprintf(stderr, "tnelterm: warning: no Khmer font found\n");

	FT_Face pf = fonts[0].face;
	float asc = pf->size->metrics.ascender / 64.0f;
	float desc = -pf->size->metrics.descender / 64.0f;
	FT_Load_Char(pf, 'M', FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
	fm.cw = (int)lroundf(pf->glyph->advance.x / 64.0f);
	if (fm.cw < 1)
		fm.cw = 1;

	if (khmer_font > 0) {
		scale_khmer(px, kscale);
		float top = 0, bot = 0;
		measure_ink(khmer_font, "ក្សៀ ស្ត្រី ញ៉ុំ ឡើង ផ្ទុំ ម្ល៉ឹង ឆ្ងាញ់ ទ្រព្យ", &top, &bot);
		if (top > asc) asc = top;
		if (-bot > desc) desc = -bot;
	}
	fm.ascent = (int)ceilf(asc);
	fm.ch = fm.ascent + (int)ceilf(desc);
	fm.thick = px / 14 > 1 ? (int)lroundf(px / 14) : 1;
	fm.uline = (int)lroundf(-pf->underline_position * pf->size->metrics.y_scale / 65536.0f / 64.0f);
	/* Sit underlines low enough to clear Khmer subscripts. */
	if (fm.uline < fm.ch - fm.ascent - 2 * fm.thick)
		fm.uline = fm.ch - fm.ascent - 2 * fm.thick;
	if (fm.uline < 1)
		fm.uline = 1;
	return true;
}
