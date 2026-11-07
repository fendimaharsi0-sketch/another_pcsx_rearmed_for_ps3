/* Minimal PNG writer for the in-game screenshot (pause menu).
 * See screenshot.h. One IDAT with a stored (uncompressed) DEFLATE stream:
 * deliberately NO compression-library dependency at all — the stock
 * PSL1GHT repo ships no portlibs/compression library, so a compressed
 * writer cannot build in CI (run #37 died on a missing header). Stored
 * blocks make the build identical on every machine, at the cost of a
 * bigger file (~6.2 MB @1080p instead of ~1-2 MB). CRC32 and Adler-32
 * are implemented locally.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "screenshot.h"

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put_le16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* PNG CRC32 (IEEE, reflected, polynomial 0xEDB88320). Table generated once
 * at first use. Calling convention matches the usual running-CRC form:
 * crc = png_crc32(crc, buf, len), starting from 0. */
static uint32_t crc_table[256];
static int crc_table_ready;

static void make_crc_table(void)
{
    unsigned n, k;
    for (n = 0; n < 256; n++) {
        uint32_t c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

static uint32_t png_crc32(uint32_t crc, const unsigned char *buf, size_t len)
{
    size_t i;
    if (!crc_table_ready) make_crc_table();
    crc ^= 0xffffffffu;
    for (i = 0; i < len; i++)
        crc = crc_table[(crc ^ buf[i]) & 0xffu] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

/* Adler-32 of the raw data (the DEFLATE stream trailer). */
static uint32_t adler32_of(const unsigned char *data, size_t len)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* One chunk: length + type + data + CRC32(type + data). Returns 0 or -1. */
static int put_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    uint32_t crc;
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    crc = png_crc32(0, (const unsigned char *)type, 4);
    if (len) {
        if (fwrite(data, 1, len, f) != len) return -1;
        crc = png_crc32(crc, data, len);
    }
    put_be32(hdr, crc);
    if (fwrite(hdr, 1, 4, f) != 4) return -1;
    return 0;
}

/* Write the IDAT chunk: DEFLATE stream header 0x78 0x01, then DEFLATE stored blocks
 * (max 65535 bytes each: 1 header byte, LEN + NLEN little-endian, raw
 * bytes; header 0x00 non-final, 0x01 final), then Adler-32 big-endian.
 * Streams the raw data (no second buffer); the chunk CRC is accumulated
 * incrementally as the data is written. Returns 0 or -1. */
static int put_idat(FILE *f, const unsigned char *raw, size_t raw_len)
{
    static const unsigned char zhdr[2] = { 0x78, 0x01 };
    unsigned char hdr[8];
    uint32_t crc;
    size_t nblocks, idat_len, off, i;

    if (raw_len == 0) return -1;
    nblocks = (raw_len + 65534u) / 65535u;
    idat_len = 2u + 4u + raw_len + 5u * nblocks;
    put_be32(hdr, (uint32_t)idat_len);
    memcpy(hdr + 4, "IDAT", 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    crc = png_crc32(0, (const unsigned char *)"IDAT", 4);
    if (fwrite(zhdr, 1, 2, f) != 2) return -1;
    crc = png_crc32(crc, zhdr, 2);
    off = 0;
    for (i = 0; i < nblocks; i++) {
        size_t chunk = raw_len - off;
        unsigned char bh[5];
        if (chunk > 65535u) chunk = 65535u;
        bh[0] = (unsigned char)(i == nblocks - 1 ? 1 : 0);  /* BFINAL, BTYPE 00 */
        put_le16(bh + 1, (uint32_t)chunk);                   /* LEN */
        put_le16(bh + 3, (uint32_t)(0xffffu - (uint32_t)chunk));  /* NLEN */
        if (fwrite(bh, 1, 5, f) != 5) return -1;
        crc = png_crc32(crc, bh, 5);
        if (fwrite(raw + off, 1, chunk, f) != chunk) return -1;
        crc = png_crc32(crc, raw + off, chunk);
        off += chunk;
    }
    put_be32(hdr, adler32_of(raw, raw_len));
    if (fwrite(hdr, 1, 4, f) != 4) return -1;
    crc = png_crc32(crc, hdr, 4);
    put_be32(hdr, crc);
    if (fwrite(hdr, 1, 4, f) != 4) return -1;
    return 0;
}

int screenshot_write_png(const char *path, const uint32_t *pix, unsigned w, unsigned h)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw = NULL;
    size_t raw_len;
    FILE *f = NULL;
    unsigned y, x;
    int rc = -1;

    if (!path || !pix || w == 0 || h == 0) return -1;
    /* guard against absurd sizes (a corrupt width/height must not malloc forever) */
    if (w > 8192 || h > 8192) return -1;

    raw_len = (size_t)h * (1u + (size_t)w * 3u);
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
    if (put_idat(f, raw, raw_len) != 0) goto out;
    if (put_chunk(f, "IEND", NULL, 0) != 0) goto out;
    rc = 0;
out:
    if (f) fclose(f);
    free(raw);
    return rc;
}
