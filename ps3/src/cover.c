/* Cover art for the game list: lookup + decode. See cover.h for the design
   (user's webMAN pack only, one cover cached, small caps for an old machine). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "cover.h"

/* stb_image, JPG/PNG only: the other formats are stripped at compile time
   (they would only add code to the package), as is FILE loading (we read
   the file ourselves so its size can be capped before decoding). */
#define STBI_NO_STDIO
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define COVER_MAX_FILE (512 * 1024)
#define COVER_MAX_DIM 512

void cover_free(cover_img_t *c)
{
    if (!c) return;
    free(c->px);
    c->px = NULL;
    c->w = c->h = 0;
}

/* Read a whole file, capped, and decode it to RGBA. 1 on success, 0 otherwise. */
static int try_file(const char *path, cover_img_t *out)
{
    FILE *f;
    long sz;
    unsigned char *buf = NULL, *px;
    int w = 0, h = 0, n = 0;
    if (!(f = fopen(path, "rb"))) return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > COVER_MAX_FILE) { fclose(f); return 0; }
    buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);
    /* Dimensions first: a huge image is refused before it can allocate. */
    if (!stbi_info_from_memory(buf, (int)sz, &w, &h, &n) || w <= 0 || h <= 0 ||
        w > COVER_MAX_DIM || h > COVER_MAX_DIM) {
        free(buf);
        return 0;
    }
    px = stbi_load_from_memory(buf, (int)sz, &w, &h, &n, 4);
    free(buf);
    if (!px) return 0;
    out->px = px;
    out->w = w;
    out->h = h;
    return 1;
}

/* A serial as the pack names it: "SLUS_123.45" (with the underscore and dot).
   in is a serial in any of the usual spellings (SLUS12345, SLUS-12345,
   SLUS_123.45); out is the pack spelling, or "" when in is not a serial. */
static void pack_serial(const char *in, char *out, size_t n)
{
    char d[6] = { 0 };
    int k = 0, i = 0;
    out[0] = 0;
    if (!in) return;
    while (in[i] && k < 4 && isalpha((unsigned char)in[i])) i++;
    if (i != 4) return;
    if (in[i] == '-' || in[i] == '_') i++;
    while (in[i] && k < 5) {
        if (in[i] == '.' && k == 3) { i++; continue; }
        if (!isdigit((unsigned char)in[i])) return;
        d[k++] = in[i++];
    }
    if (k != 5) return;
    snprintf(out, n, "%.4s_%.3s.%.2s", in, d, d + 3);
    /* upper-case the letters (the pack's names are upper-case) */
    for (i = 0; out[i] && i < 4; i++) out[i] = (char)toupper((unsigned char)out[i]);
}

/* A serial hiding in a file name: "[SLUS-12345]", "SLUS_123.45",
   "SLUS12345" anywhere in the base name. "" when there is none. */
static void serial_in_name(const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/'), *p;
    char cand[16];
    out[0] = 0;
    if (!b) b = path; else b++;
    for (p = b; *p; p++) {
        int i;
        if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1]) ||
            !isalpha((unsigned char)p[2]) || !isalpha((unsigned char)p[3]))
            continue;
        for (i = 0; i < 15 && p[i] && p[i] != '/' ; i++) cand[i] = p[i];
        cand[i < 15 ? i : 15] = 0;
        pack_serial(cand, out, n);
        if (out[0]) return;
    }
}

static int try_pack(const char *serial, cover_img_t *out)
{
    static const char *dirs[] = {
        "/dev_hdd0/game/BLES80608/USRDIR/covers_retro/psx/",
        "/dev_usb000/game/BLES80608/USRDIR/covers_retro/psx/",
        "/dev_usb000/covers_retro/psx/",
        "/dev_usb001/game/BLES80608/USRDIR/covers_retro/psx/",
        "/dev_usb001/covers_retro/psx/",
        NULL };
    static const char *exts[] = { "JPG", "PNG", "jpg", "png", NULL };
    int i, j;
    char path[512];
    if (!serial[0]) return 0;
    for (i = 0; dirs[i]; i++)
        for (j = 0; exts[j]; j++) {
            snprintf(path, sizeof path, "%s%s_COV.%s", dirs[i], serial, exts[j]);
            if (try_file(path, out)) return 1;
        }
    return 0;
}

/* A cover named after the game file itself, next to it (webMAN's cover
   rule for ISO files): "Game.cue" -> "Game.JPG"/"Game.PNG". */
static int try_same_name(const char *game_path, cover_img_t *out)
{
    static const char *exts[] = { "JPG", "PNG", "jpg", "png", NULL };
    const char *dot;
    char base[1200];
    int j;
    size_t len;
    if (!game_path) return 0;
    len = strlen(game_path);
    if (len >= sizeof base) return 0;
    snprintf(base, sizeof base, "%s", game_path);
    dot = strrchr(base, '.');
    if (dot && dot > strrchr(base, '/')) * (char *)dot = 0;
    for (j = 0; exts[j]; j++) {
        char path[1210];
        snprintf(path, sizeof path, "%s.%s", base, exts[j]);
        if (try_file(path, out)) return 1;
    }
    return 0;
}

int cover_load(const char *game_path, int is_disc, const char *disc_serial, cover_img_t *out)
{
    char serial[16], from_name[16];
    out->px = NULL;
    out->w = out->h = 0;
    if (is_disc) {
        pack_serial(disc_serial ? disc_serial : "", serial, sizeof serial);
        return try_pack(serial, out);
    }
    /* Files: the pack by the serial in the file name, then a cover
       named after the file. Reading SYSTEM.CNF out of the disc image
       itself (for a serial no file name carries) is not in this version:
       plain ISO/BIN could, CHD/PBP could not, so it would be uneven. */
    serial_in_name(game_path, from_name, sizeof from_name);
    if (try_pack(from_name, out)) return 1;
    return try_same_name(game_path, out);
}
