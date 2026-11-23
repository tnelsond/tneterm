#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	SDL_Texture *tex; /* NULL for empty glyphs */
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

/* kscale: Khmer size relative to px; <= 0 picks one to match the mono font */
bool font_init(const char *mono, const char *khmer, float px, float kscale);
/* Free all fonts and cached glyph textures so font_init can run again. */
void font_done(void);
int font_for_cp(uint32_t cp);
uint32_t font_glyph_index(int font, uint32_t cp);
/* Shape n codepoints with per-codepoint cluster ids. Returns glyph count. */
int font_shape(int font, const uint32_t *cps, const uint32_t *clusters, int n,
               ShapedGlyph *out, int max);
const Glyph *font_glyph(SDL_Renderer *ren, int font, uint32_t gid, bool bold);
