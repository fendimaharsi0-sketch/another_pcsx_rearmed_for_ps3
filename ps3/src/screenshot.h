/* Minimal PNG writer for the in-game screenshot (pause menu).
 *
 * No PS3 headers: only stdio/stdlib/string/stdint + zlib, so the encoder
 * is host-testable (see the host test in the verification notes).
 * Format: 8-bit RGB, color type 2, filter 0 (one filter byte 0 per
 * scanline), IHDR + a single IDAT + IEND. Rows are top-first, matching
 * the display buffer's scanout order.
 *
 * Pixels are XRGB8888 u32 0x00RRGGBB (the fork's display format: A8R8G8B8
 * textures, FXAA self-test in video.c extracts channels as
 * (v >> (16 - 8*k)) & 255 for R,G,B). R=(v>>16)&0xff, G=(v>>8)&0xff,
 * B=v&0xff — do NOT treat the bytes as B,G,R,X, that swaps R and B.
 */
#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdint.h>

/* Write path as a PNG of w x h pixels from pix (row-major, w*h u32,
   row 0 = top). Returns 0 on success, -1 on any failure (open, malloc,
   compress, write). The file is left closed either way; a failed write
   may leave a partial file behind (the caller logs and the user can
   delete it; screenshots are one-shot, not retried). */
int screenshot_write_png(const char *path, const uint32_t *pix, unsigned w, unsigned h);

#endif /* SCREENSHOT_H */
