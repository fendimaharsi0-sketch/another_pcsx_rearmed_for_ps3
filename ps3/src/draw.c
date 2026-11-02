/* Text and boxes on the 1280x720 overlay (ARGB, alpha blended over the picture). */
#include <stdio.h>
#include "common.h"
#include "font_data.h"

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
    /* src colour with alpha a (0..255) over dst (premultiplied-free, keeps max alpha) */
    unsigned da = dst >> 24, oa = a > da ? a : da;
    unsigned r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * (255 - a)) / 255;
    unsigned g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * (255 - a)) / 255;
    unsigned b = ((src & 255) * a + (dst & 255) * (255 - a)) / 255;
    return oa << 24 | r << 16 | g << 8 | b;
}

void ovl_fill(int x, int y, int w, int h, uint32_t argb)
{
    uint32_t *o = video_overlay();
    unsigned a = argb >> 24;
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OVL_W) w = OVL_W - x;
    if (y + h > OVL_H) h = OVL_H - y;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            uint32_t *p = &o[(y + j) * OVL_W + x + i];
            *p = a == 255 ? argb : blend(*p, argb, a);
        }
}

int ovl_text_width(const char *s)
{
    int n = 0;
    for (; *s; s++) n++;
    return n * FONT_W;
}

void ovl_blit(int x, int y, int w, int h, const unsigned char *px, int sw, int sh)
{
    uint32_t *o = video_overlay();
    int i, j;
    if (!px || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OVL_W) w = OVL_W - x;
    if (y + h > OVL_H) h = OVL_H - y;
    for (j = 0; j < h; j++) {
        /* nearest sampling: the pack's covers are 268x268 or smaller and
           the panel is 290x290, so the scale is near 1:1 and no extra
           scaled buffer is needed (one less allocation on an old machine) */
        int sy = (int)((long)j * sh / h);
        for (i = 0; i < w; i++) {
            const unsigned char *s = px + ((size_t)sy * sw + (size_t)i * sw / w) * 4;
            unsigned a = s[3];
            uint32_t *p;
            if (!a) continue;
            p = &o[(y + j) * OVL_W + x + i];
            *p = a == 255 ? (0xff000000u | (unsigned)s[0] << 16 | (unsigned)s[1] << 8 | s[2])
                          : blend(*p, (unsigned)s[0] << 16 | (unsigned)s[1] << 8 | s[2], a);
        }
    }
}

void ovl_line(int x0, int y0, int x1, int y1, uint32_t argb)
{
    /* Bresenham, 2 px thick (a 2x2 block per step; ovl_fill clips). */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        ovl_fill(x0, y0, 2, 2, argb);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void ovl_circle(int cx, int cy, int r, uint32_t argb)
{
    /* Midpoint circle, 2 px outline. */
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        ovl_fill(cx + x, cy + y, 2, 2, argb); ovl_fill(cx - x, cy + y, 2, 2, argb);
        ovl_fill(cx + x, cy - y, 2, 2, argb); ovl_fill(cx - x, cy - y, 2, 2, argb);
        ovl_fill(cx + y, cy + x, 2, 2, argb); ovl_fill(cx - y, cy + x, 2, 2, argb);
        ovl_fill(cx + y, cy - x, 2, 2, argb); ovl_fill(cx - y, cy - x, 2, 2, argb);
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}

void ovl_button(int x, int y, int btn, uint32_t argb)
{
    /* 24x24 box, 2 px inset: the glyph spans x+2..x+21, y+2..y+21. */
    switch (btn) {
    case GLYPH_CROSS:
        ovl_line(x + 3, y + 3, x + 20, y + 20, argb);
        ovl_line(x + 20, y + 3, x + 3, y + 20, argb);
        break;
    case GLYPH_CIRCLE:
        ovl_circle(x + 11, y + 11, 9, argb);
        break;
    case GLYPH_TRIANGLE:
        ovl_line(x + 11, y + 2, x + 2, y + 21, argb);
        ovl_fill(x + 2, y + 21, 20, 2, argb);
        ovl_line(x + 21, y + 21, x + 11, y + 2, argb);
        break;
    case GLYPH_SQUARE:
        ovl_fill(x + 2, y + 2, 20, 2, argb);
        ovl_fill(x + 2, y + 20, 20, 2, argb);
        ovl_fill(x + 2, y + 2, 2, 20, argb);
        ovl_fill(x + 20, y + 2, 2, 20, argb);
        break;
    }
}

int ovl_text(int x, int y, const char *s, uint32_t argb)
{
    uint32_t *o = video_overlay();
    unsigned ca = argb >> 24;
#ifdef OVL_CHECK      /* host UI harness: text running into the right TV margin */
    if (x + ovl_text_width(s) > OVL_W - 50) fprintf(stderr, "OVL_CHECK: text too wide at x %d: %s\n", x, s);
#endif
    for (; *s; s++, x += FONT_W) {
        int c = (unsigned char)*s, r, k;
        const unsigned char *g;
        if (c < 32 || c > 126) c = '?';
        if (x + FONT_W > OVL_W) break;
        if (x < 0 || y < 0 || y + FONT_H > OVL_H) continue;
        g = font_data[c - 32];
        for (r = 0; r < FONT_H; r++)
            for (k = 0; k < FONT_W; k++) {
                unsigned v = g[(r * FONT_W + k) >> 1];
                unsigned a = (k & 1) ? (v & 15) : (v >> 4);
                if (a) {
                    uint32_t *p = &o[(y + r) * OVL_W + x + k];
                    *p = blend(*p, argb, a * 17 * ca / 255);
                }
            }
    }
    return x;
}
