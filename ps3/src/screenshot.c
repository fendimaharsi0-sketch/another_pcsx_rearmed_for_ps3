/* Minimal PNG writer for the in-game screenshot (pause menu).
 * See screenshot.h. One IDAT, zlib level 6, CRC32 via zlib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#include "screenshot.h"

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* One chunk: length + type + data + CRC32(type + data). Returns 0 or -1. */
static int put_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    uLong crc;
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len) {
        if (fwrite(data, 1, len, f) != len) return -1;
        crc = crc32(crc, data, len);
    }
    put_be32(hdr, (uint32_t)crc);
    if (fwrite(hdr, 1, 4, f) != 4) return -1;
    return 0;
}

int screenshot_write_png(const char *path, const uint32_t *pix, unsigned w, unsigned h)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw = NULL, *cmp = NULL;
    uLongf raw_len, cmp_len;
    FILE *f = NULL;
    unsigned y, x;
    int rc = -1;

    if (!path || !pix || w == 0 || h == 0) return -1;
    /* guard against absurd sizes (a corrupt width/height must not malloc forever) */
    if (w > 8192 || h > 8192) return -1;

    raw_len = (uLongf)h * (1u + (uLongf)w * 3u);
    raw = (unsigned char *)malloc(raw_len ? raw_len : 1);
    if (!raw) return -1;
    /* filter 0, RGB per pixel, rows top-first */
    for (y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (1u + (size_t)w * 3u);
        const uint32_t *src = pix + (size_t)y * w;
        row[0] = 0;
        for (x = 0; x < w; x++) {
            uint32_t v = src[x];
            row[1 + x * 3 + 0] = (unsigned char)((v >> 16) & 0xff);   /* R */
            row[1 + x * 3 + 1] = (unsigned char)((v >> 8) & 0xff);    /* G */
            row[1 + x * 3 + 2] = (unsigned char)(v & 0xff);           /* B */
        }
    }
    cmp_len = compressBound(raw_len);
    cmp = (unsigned char *)malloc(cmp_len ? cmp_len : 1);
    if (!cmp) goto out;
    if (compress2(cmp, &cmp_len, raw, raw_len, 6) != Z_OK) goto out;

    f = fopen(path, "wb");
    if (!f) goto out;
    if (fwrite(sig, 1, 8, f) != 8) goto out;
    put_be32(ihdr + 0, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* color type: truecolour RGB */
    ihdr[10] = 0;   /* compression method */
    ihdr[11] = 0;   /* filter method */
    ihdr[12] = 0;   /* interlace: none */
    if (put_chunk(f, "IHDR", ihdr, 13) != 0) goto out;
    if (put_chunk(f, "IDAT", cmp, (uint32_t)cmp_len) != 0) goto out;
    if (put_chunk(f, "IEND", NULL, 0) != 0) goto out;
    rc = 0;
out:
    if (f) fclose(f);
    free(cmp);
    free(raw);
    return rc;
}
