#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* Glyphs live in shared atlas textures so a whole frame can be drawn with a
 * few batched geometry calls instead of one texture switch per glyph. */
typedef struct {
	SDL_Texture *tex; /* atlas page; NULL for empty glyphs */
	float u0, v0, u1, v1; /* texture coordinates in tex */
	int w, h, left, top;
	float adv;   /* pixels, already scaled */
	float scale; /* draw scale for bitmap (emoji) fonts, else 1 */
	bool color;
} Glyph;

typedef struct {
	uint32_t gid, cluster;
	float x_adv, x_off, y_off;
} ShapedGlyph;

typedef struct {
	int cw;     /* cell width */
	int ch;     /* line height (fits Khmer stacks) */
	int ascent; /* baseline offset from top of line */
	int uline;  /* underline offset below baseline */
	int thick;  /* line thickness */
} FontMetrics;

extern FontMetrics fm;

/* kscale: Khmer size relative to px; <= 0 picks one to match the mono font.
 * spacing: extra pixels between lines (may be negative). */
bool font_init(const char *mono, const char *khmer, float px, float kscale, int spacing);
/* Free all fonts and cached glyph textures so font_init can run again. */
void font_done(void);
int font_for_cp(uint32_t cp);
uint32_t font_glyph_index(int font, uint32_t cp);
/* Shape n codepoints with per-codepoint cluster ids. Returns glyph count. */
int font_shape(int font, const uint32_t *cps, const uint32_t *clusters, int n,
               ShapedGlyph *out, int max);
const Glyph *font_glyph(SDL_Renderer *ren, int font, uint32_t gid, bool bold);
/* An opaque white texel in an atlas page, for drawing solid rectangles in
 * the same batches as glyphs. */
SDL_Texture *font_white(SDL_Renderer *ren, float *u, float *v);
