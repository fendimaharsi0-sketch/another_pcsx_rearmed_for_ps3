/* Cover art for the game list: lookup + decode. See cover.h for the design
   (user's webMAN pack only, one cover cached, small caps for an old machine). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include "common.h"
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

#define COVER_MAX_FILE (1 * 1024 * 1024)
#define COVER_MAX_DIM 1024
/* The panel art box (menu.c CASE_ART): decoded art larger than this is
   fitted to it in try_file, so the panel never holds a full-size image. */
#define COVER_BOX 264

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
    /* Fit art larger than the panel box to it, by averaging the source
       pixels each output pixel covers (downscale only; smaller art is
       left alone and the panel upscales it with nearest). Averaging keeps
       thin lines from shimmering where nearest would drop them. */
    if (w > COVER_BOX || h > COVER_BOX) {
        float s = (float)COVER_BOX / w < (float)COVER_BOX / h ? (float)COVER_BOX / w : (float)COVER_BOX / h;
        int nw = (int)(w * s + 0.5f), nh = (int)(h * s + 0.5f), x, y;
        unsigned char *small;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        small = malloc((size_t)nw * nh * 4);
        if (!small) { stbi_image_free(px); return 0; }
        for (y = 0; y < nh; y++) {
            int y0 = (int)(y / s), y1 = (int)((y + 1) / s);
            if (y1 > h) y1 = h;
            if (y1 <= y0) y1 = y0 + 1;
            for (x = 0; x < nw; x++) {
                int x0 = (int)(x / s), x1 = (int)((x + 1) / s), i, j;
                unsigned r = 0, g = 0, b = 0, a = 0, k;
                unsigned char *d;
                if (x1 > w) x1 = w;
                if (x1 <= x0) x1 = x0 + 1;
                for (j = y0; j < y1; j++)
                    for (i = x0; i < x1; i++) {
                        const unsigned char *p = px + ((size_t)j * w + i) * 4;
                        r += p[0]; g += p[1]; b += p[2]; a += p[3];
                    }
                k = (unsigned)((y1 - y0) * (x1 - x0));
                d = small + ((size_t)y * nw + x) * 4;
                d[0] = (unsigned char)(r / k); d[1] = (unsigned char)(g / k);
                d[2] = (unsigned char)(b / k); d[3] = (unsigned char)(a / k);
            }
        }
        stbi_image_free(px);
        px = small;
        w = nw; h = nh;
    }
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

/* ---- the serial of a game file, read out of the image itself ----
   As the disc code writes it: SLUS00707 (letters and digits). From the
   file name when it has one ("Game [SLUS-00707].cue"), else from the
   image: SYSTEM.CNF's BOOT line in a .bin/.img/.iso (the first one a
   .cue names), DISC_ID in a .pbp's PARAM.SFO, the first disc of an .m3u.
   Not from a .chd (compressed). Reads are bounded (a few sectors, the
   SFO in 4 KB) and the caller runs this at most once per list entry. */

/* "SLUS-00707", "SLUS_007.07", "SLUS00707" at s: SLUS00707 in out (10 bytes), 1 */
static int serial_at(const char *s, char *out)
{
    int i, k = 0;
    for (i = 0; i < 4; i++) if (!isalpha((unsigned char)s[i])) return 0;
    if (i > 0 && isalpha((unsigned char)s[4])) return 0;
    for (i = 0; i < 4; i++) out[i] = (char)toupper((unsigned char)s[i]);
    s += 4;
    if (*s == '-' || *s == '_' || *s == ' ') s++;
    for (k = 0; k < 5; s++) {
        if (*s == '.' && k == 3) continue;
        if (!isdigit((unsigned char)*s)) return 0;
        out[4 + k++] = *s;
    }
    if (isdigit((unsigned char)*s)) return 0;
    out[9] = 0;
    return 1;
}

static int serial_in_name_bare(const char *path, char *out)
{
    const char *b = strrchr(path, '/'), *p;
    for (p = b ? b + 1 : path; *p; p++)
        if ((p == path || !isalpha((unsigned char)p[-1])) && serial_at(p, out)) return 1;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* a disc image file: 2352-byte raw sectors or 2048-byte ones */
typedef struct { FILE *f; int raw; } img_t;

/* the 2048 data bytes of sector lba into d */
static int img_read(img_t *m, uint32_t lba, uint8_t *d)
{
    uint8_t s[2352];
    if (m->raw) {
        if (fseek(m->f, (long)lba * 2352, SEEK_SET) || fread(s, 1, 2352, m->f) != 2352) return -1;
        memcpy(d, s + (s[15] == 1 ? 16 : 24), 2048);
    } else if (fseek(m->f, (long)lba * 2048, SEEK_SET) || fread(d, 1, 2048, m->f) != 2048) return -1;
    return 0;
}

/* SYSTEM.CNF's BOOT line (BOOT = cdrom:\SLUS_007.07;1), as disc.c reads it */
static int serial_in_image(const char *path, char *out)
{
    static const uint8_t sync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    uint8_t d[2049], s[16];
    img_t m;
    uint32_t root, size, lba, i, cnf = 0, cnf_size = 0;
    int ok = 0;
    if (!(m.f = fopen(path, "rb"))) return 0;
    m.raw = fread(s, 1, 16, m.f) == 16 && !memcmp(s, sync, 12);
    if (img_read(&m, 16, d) || memcmp(d + 1, "CD001", 5)) goto done;
    root = le32(d + 156 + 2); size = le32(d + 156 + 10);
    for (lba = 0; lba < (size + 2047) / 2048 && lba < 16 && !cnf; lba++) {
        if (img_read(&m, root + lba, d)) goto done;
        for (i = 0; i < 2048 && d[i] >= 34 && i + d[i] <= 2048; i += d[i]) {
            const uint8_t *r = d + i;
            if (r[32] >= 10 && !strncasecmp((const char *)r + 33, "SYSTEM.CNF", 10)) {
                cnf = le32(r + 2); cnf_size = le32(r + 10);
                break;
            }
        }
    }
    if (!cnf || !cnf_size || img_read(&m, cnf, d)) goto done;
    d[cnf_size < 2048 ? cnf_size : 2048] = 0;
    {
        char *b = (char *)d, *e;
        for (; *b && strncasecmp(b, "BOOT", 4); b++) ;
        if (!*b || !(b = strchr(b, '='))) goto done;
        for (e = b; *e && *e != ';' && *e != '\r' && *e != '\n'; e++) ;
        *e = 0;
        if ((e = strrchr(b, '\\')) || (e = strrchr(b, ':'))) b = e + 1;
        ok = serial_at(b, out);
    }
done:
    fclose(m.f);
    return ok;
}

/* DISC_ID in the PARAM.SFO of a PlayStation Store .pbp (SLUS00707) */
static int serial_in_pbp(const char *path, char *out)
{
    uint8_t h[40], sfo[4096];
    uint32_t off, n, keys, data, i;
    size_t got;
    int ok = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(h, 1, 40, f) != 40 || memcmp(h, "\0PBP", 4)) goto done;
    off = le32(h + 8);
    if (fseek(f, (long)off, SEEK_SET) || (got = fread(sfo, 1, sizeof sfo, f)) < 20 || memcmp(sfo, "\0PSF", 4)) goto done;
    keys = le32(sfo + 8); data = le32(sfo + 12); n = le32(sfo + 16);
    for (i = 0; i < n && 20 + i * 16 + 16 <= got; i++) {
        const uint8_t *e = sfo + 20 + i * 16;
        uint32_t k = keys + le16(e), v = data + le32(e + 12);
        if (k + 8 <= got && !memcmp(sfo + k, "DISC_ID", 8) && v + 10 <= got) {
            char t[16];
            memcpy(t, sfo + v, 9); t[9] = 0;
            ok = serial_at(t, out);
            break;
        }
    }
done:
    fclose(f);
    return ok;
}

/* the first FILE of a .cue (in the .cue's folder) or the first line of an .m3u */
static int first_listed(const char *path, int cue, char *out, size_t n)
{
    char line[1024], *b, *e;
    const char *slash = strrchr(path, '/');
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        for (b = line; *b == ' ' || *b == '\t'; b++) ;
        if (cue) {
            if (strncasecmp(b, "FILE", 4)) continue;
            b += 4;
            while (*b == ' ' || *b == '\t') b++;
            if (*b == '"') { b++; e = strchr(b, '"'); }
            else for (e = b; *e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n'; e++) ;
            if (!e) continue;
        } else {
            if (*b == '#' || *b == '\r' || *b == '\n' || !*b) continue;
            for (e = b + strlen(b); e > b && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '); e--) ;
        }
        *e = 0;
        fclose(f);
        if (*b == '/' || !slash) snprintf(out, n, "%s", b);
        else snprintf(out, n, "%.*s/%s", (int)(slash - path), path, b);
        return 1;
    }
    fclose(f);
    return 0;
}

static const char *ext_of(const char *p)
{
    const char *d = strrchr(p, '.'), *s = strrchr(p, '/');
    return d && (!s || d > s) ? d + 1 : "";
}

/* The serial of a game file, as the disc code writes it ("SLUS00707",
   out is 10 bytes). From the file name when it has one, else from the
   image (see above). 1 when found, 0 with out empty otherwise. */
int cover_serial(const char *path, char *out)
{
    char sub[1100];
    const char *x = ext_of(path);
    int ok;
    out[0] = 0;
    if (serial_in_name_bare(path, out)) return 1;
    if (!strcasecmp(x, "pbp")) ok = serial_in_pbp(path, out);
    else if (!strcasecmp(x, "cue")) ok = first_listed(path, 1, sub, sizeof sub) && serial_in_image(sub, out);
    else if (!strcasecmp(x, "m3u")) ok = first_listed(path, 0, sub, sizeof sub) && strcasecmp(ext_of(sub), "m3u") && cover_serial(sub, out);
    else if (!strcasecmp(x, "bin") || !strcasecmp(x, "img") || !strcasecmp(x, "iso")) ok = serial_in_image(path, out);
    else ok = 0;
    /* a failed serial_at may have left its four letters in out already;
       a failed lookup must leave out empty, as the callers rely on it */
    if (!ok) out[0] = 0;
    return ok;
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

/* A cover in the user's own covers folder (APP_DIR/covers/), by game
   file name: "Tekken 3.jpg" there beats the webMAN pack. */
static int try_covers_name(const char *game_path, cover_img_t *out)
{
    static const char *exts[] = { "JPG", "PNG", "jpg", "png", NULL };
    const char *base, *dot;
    char name[256], path[512];
    int j;
    size_t len;
    if (!game_path) return 0;
    base = strrchr(game_path, '/');
    base = base ? base + 1 : game_path;
    len = strlen(base);
    if (len >= sizeof name) return 0;
    snprintf(name, sizeof name, "%s", base);
    dot = strrchr(name, '.');
    if (dot) * (char *)dot = 0;
    for (j = 0; exts[j]; j++) {
        snprintf(path, sizeof path, APP_DIR "/covers/%s.%s", name, exts[j]);
        if (try_file(path, out)) return 1;
    }
    return 0;
}

/* A cover in APP_DIR/covers/ by serial, in the three spellings covers
   are named with. in is the pack spelling ("SLUS_123.45"). */
static int try_covers_serial(const char *serial, cover_img_t *out)
{
    static const char *exts[] = { "JPG", "PNG", "jpg", "png", NULL };
    const char *names[3];
    char bare[16], dash[16], path[512];
    int j, k;
    if (!serial[0] || strlen(serial) != 11) return 0;
    /* pack spelling LLLL_DDD.DD -> bare LLLLDDDDD, dash LLLL-DDDDD */
    snprintf(bare, sizeof bare, "%.4s%.3s%.2s", serial, serial + 5, serial + 9);
    snprintf(dash, sizeof dash, "%.4s-%.3s%.2s", serial, serial + 5, serial + 9);
    names[0] = serial; names[1] = dash; names[2] = bare;
    for (k = 0; k < 3; k++)
        for (j = 0; exts[j]; j++) {
            snprintf(path, sizeof path, APP_DIR "/covers/%s.%s", names[k], exts[j]);
            if (try_file(path, out)) return 1;
        }
    return 0;
}

/* ---- webMAN mygames.xml, layer 4 ----
   webMAN MOD's game list, at /dev_hdd0/xmlhost/game_plugin/mygames.xml
   (XMBML, ~3 MB cap), already did the hard work: its scanner found a
   serial for files whose names carry none, and picked an icon for them.
   So this layer does NOT re-derive a serial from the XML's title: it
   uses the entry's icon path directly. That matters because title and
   info can disagree (one game in a real list has [SLPS-91512] in its
   title and SLPM86201 in its info) — the icon webMAN chose is the cover
   webMAN chose, no second guess.
   Parsing is a plain string scan, no XML library: only the PSX segment
   (seg_wm_psx_items) is read when groups are on; with groups off the
   whole file is scanned but only entries whose mount path looks like a
   PSX folder are kept. The map is built once, lazily, on the first
   cover lookup after a rescan, and freed on invalidate; a missing file,
   an oversize file, or a stale list (a game added after webMAN's last
   Refresh XML) simply means this layer finds nothing. Generic webMAN
   icons (tmp/wm_icons) are not covers, so they are skipped, as are
   non-local paths (network, NTFS cache) and non-JPG/PNG icons.
   Matching: the mount action is URL-decoded ('+' -> space, %XX -> byte)
   and its mount prefix stripped, then the extension is stripped, so
   our list's "Game.cue" matches webMAN's "Game.bin". Full-path match
   first, then base-name match (case-insensitive both ways). */
#define XML_PATH "/dev_hdd0/xmlhost/game_plugin/mygames.xml"
#define XML_MAX_FILE (4 * 1024 * 1024)
#define XML_MAX_ENTRIES 2000

static char *xml_full[XML_MAX_ENTRIES];   /* decoded mount path, no extension */
static char *xml_base[XML_MAX_ENTRIES];   /* base name of xml_full[i] */
static char *xml_icon[XML_MAX_ENTRIES];
static int nxml, xml_state;               /* 0 = not tried, 1 = loaded, 2 = failed */

void cover_xml_invalidate(void)
{
    int i;
    for (i = 0; i < nxml; i++) { free(xml_full[i]); free(xml_base[i]); free(xml_icon[i]); }
    nxml = 0;
    xml_state = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In place: '+' -> ' ', %XX -> the byte. A trailing bare '%' is kept. */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        int h, l;
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && (h = hexval((unsigned char)r[1])) >= 0 &&
                 (l = hexval((unsigned char)r[2])) >= 0) {
            *w++ = (char)(h * 16 + l);
            r += 3;
        } else *w++ = *r++;
    }
    *w = 0;
}

static void strip_ext(char *s)
{
    char *slash = strrchr(s, '/'), *dot = strrchr(s, '.');
    if (dot && (!slash || dot > slash)) *dot = 0;
}

static const char *base_of(const char *s)
{
    const char *b = strrchr(s, '/');
    return b ? b + 1 : s;
}

/* Case-insensitive substring search (strcasestr is not portable here). */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (!nl) return 1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            if (!hay[i + j]) break;
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int is_local_icon(const char *p)
{
    return !strncmp(p, "/dev_hdd0/", 10) || !strncmp(p, "/dev_usb", 8);
}

static int has_image_ext(const char *p)
{
    const char *dot = strrchr(p, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png");
}

/* Copy a <P key="key"><>value</> field out of one <T>...</T> block.
   1 when found and non-empty, 0 otherwise. */
static int xml_field(const char *blk, const char *blk_end, const char *key,
                     char *out, size_t n)
{
    char tag[64];
    const char *f, *v, *e;
    snprintf(tag, sizeof tag, "<P key=\"%s\">", key);
    f = strstr(blk, tag);
    if (!f || f >= blk_end) return 0;
    v = strstr(f + strlen(tag), "<>");
    if (!v || v >= blk_end) return 0;
    v += 2;
    e = strstr(v, "</>");
    if (!e || e > blk_end) return 0;
    if ((size_t)(e - v) >= n || e == v) return 0;
    memcpy(out, v, (size_t)(e - v));
    out[e - v] = 0;
    return 1;
}

static void xml_load(void)
{
    FILE *f;
    long sz;
    char *buf = NULL, *seg, *seg_end, *pos;
    int psx_only;
    if (xml_state) return;
    xml_state = 2;
    if (!(f = fopen(XML_PATH, "rb"))) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > XML_MAX_FILE) { fclose(f); return; }
    buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return; }
    fclose(f);
    buf[sz] = 0;
    /* The segment definition, not the folder link that points at it
       (src="#seg_wm_psx_items" appears much earlier in the file). */
    seg = strstr(buf, "id=\"seg_wm_psx_items\"");
    psx_only = seg != NULL;
    if (!seg) seg = buf;
    seg_end = strstr(seg + 20, "<V id=");
    if (!seg_end) seg_end = buf + sz;
    pos = seg;
    while (nxml < XML_MAX_ENTRIES) {
        const char *t = strstr(pos, "<T ");
        const char *bend;
        char icon[600], act[1300], *full, *base;
        if (!t || t >= seg_end) break;
        bend = strstr(t, "</T>");
        if (!bend || bend > seg_end) break;
        pos = (char *)bend + 4;
        if (!xml_field(t, bend, "icon", icon, sizeof icon)) continue;
        if (!xml_field(t, bend, "module_action", act, sizeof act)) continue;
        if (strstr(icon, "wm_icons")) continue;
        if (!is_local_icon(icon) || !has_image_ext(icon)) continue;
        /* Mount action -> game path: strip the mount prefix, decode.
           ("/mount_ps3/" is 11 chars, "mount_ps3/" is 10 — counted, not
           guessed: an off-by-one here silently drops every XML entry.) */
        if (!strncmp(act, "/mount_ps3/", 11)) memmove(act, act + 11, strlen(act + 11) + 1);
        else if (!strncmp(act, "/mount.ps3/", 11)) memmove(act, act + 11, strlen(act + 11) + 1);
        else if (!strncmp(act, "mount_ps3/", 10)) memmove(act, act + 10, strlen(act + 10) + 1);
        /* Stripping the mount prefix leaves "dev_hdd0/..." with no
           leading slash; restore it so full-path matching works. */
        if (act[0] != '/') { memmove(act + 1, act, strlen(act) + 1); act[0] = '/'; }
        if (strncmp(act, "/dev_", 5)) continue;
        url_decode(act);
        if (!psx_only &&
            !contains_ci(act, "PSXISO") && !contains_ci(act, "PSXGAMES") &&
            !contains_ci(act, "/ROMS/PSX") && !contains_ci(act, "/PSX/")) continue;
        strip_ext(act);
        full = strdup(act);
        base = full ? strdup(base_of(full)) : NULL;
        if (!full || !base) { free(full); free(base); continue; }
        xml_full[nxml] = full;
        xml_base[nxml] = base;
        xml_icon[nxml] = strdup(icon);
        if (!xml_icon[nxml]) { free(full); free(base); continue; }
        nxml++;
    }
    free(buf);
    xml_state = 1;
}

static const char *xml_lookup(const char *game_path)
{
    char norm[1300];
    const char *b;
    int i;
    if (!game_path) return NULL;
    xml_load();
    if (!nxml || strlen(game_path) >= sizeof norm) return NULL;
    snprintf(norm, sizeof norm, "%s", game_path);
    strip_ext(norm);
    for (i = 0; i < nxml; i++)
        if (!strcasecmp(norm, xml_full[i])) return xml_icon[i];
    b = base_of(norm);
    for (i = 0; i < nxml; i++)
        if (!strcasecmp(b, xml_base[i])) return xml_icon[i];
    return NULL;
}

static int try_xml(const char *game_path, cover_img_t *out)
{
    const char *icon = xml_lookup(game_path);
    if (!icon) return 0;
    return try_file(icon, out);
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
    /* Files, in order: (1) a cover named after the file itself — the
       user's own custom cover, so it wins over the generic pack;
       (2) the user's own covers folder (APP_DIR/covers/), by game file
       name, then by serial; (3) the pack by the serial in the file
       name, or — when the name carries none — by the serial read out
       of the disc image itself (SYSTEM.CNF, .cue/.m3u/.pbp);
       (4) webMAN's mygames.xml icon path. */
    if (try_same_name(game_path, out)) return 1;
    if (try_covers_name(game_path, out)) return 1;
    serial_in_name(game_path, from_name, sizeof from_name);
    if (!from_name[0]) {
        /* No serial in the name: read it out of the image itself. */
        char bare[16];
        if (cover_serial(game_path, bare))
            pack_serial(bare, from_name, sizeof from_name);
    }
    if (try_covers_serial(from_name, out)) return 1;
    if (try_pack(from_name, out)) return 1;
    return try_xml(game_path, out);
}
