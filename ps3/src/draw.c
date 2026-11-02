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
