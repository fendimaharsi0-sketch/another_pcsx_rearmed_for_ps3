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

/* ---- webMAN mygames.xml, layer 3 ----
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
       (2) the pack by the serial in the file name; (3) webMAN's
       mygames.xml icon path. Reading SYSTEM.CNF out of the disc image
       itself is still not in this version: for webMAN users the XML
       already covers it, and for plain ISO/BIN it would only help
       users without webMAN — a future last layer, not this one. */
    if (try_same_name(game_path, out)) return 1;
    serial_in_name(game_path, from_name, sizeof from_name);
    if (try_pack(from_name, out)) return 1;
    return try_xml(game_path, out);
}
