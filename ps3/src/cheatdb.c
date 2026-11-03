/* See cheatdb.h. Pure C: no PS3 headers, so the host test compiles this
 * file directly. */
#include "cheatdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* The engine's opcode set, verified against libpcsxcore/cheat.h (the
 * research note said 17; the header defines 18). */
static const unsigned char classic_opcodes[] = {
    0x30, 0x80, 0x10, 0x11, 0x1F, 0x20, 0x21, 0x50, 0xC2,
    0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xE0, 0xE1, 0xE2, 0xE3
};

static int is_classic(unsigned top)
{
    size_t i;
    for (i = 0; i < sizeof classic_opcodes; i++)
        if (classic_opcodes[i] == top) return 1;
    return 0;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *xstrndup(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* In-place trim; returns the first non-space char. */
static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int is_hex_word(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || n > 8) return 0;
    for (i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* Decimal, 0x hex or 0b binary (DuckStation accepts all three). */
static int parse_num(const char *s, uint32_t *out)
{
    char *e;
    unsigned long v;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        const char *p = s + 2;
        v = 0;
        if (!*p) return 0;
        for (; *p; p++) {
            if (*p != '0' && *p != '1') return 0;
            v = (v << 1) | (unsigned)(*p - '0');
        }
        *out = (uint32_t)v;
        return 1;
    }
    v = strtoul(s, &e, 0);
    if (e == s || *e) return 0;
    *out = (uint32_t)v;
    return 1;
}

static void code_free_members(cht_code *c)
{
    int i;
    free(c->name);
    for (i = 0; i < c->n_opts; i++) free(c->opt_names[i]);
    free(c->opt_names);
    free(c->opt_vals);
    free(c->body);
    memset(c, 0, sizeof *c);
}

/* ---- body helpers ---- */

/* Appends one body line (without the newline) to the body buffer. */
static int body_append(const char *line, size_t n,
                       char **buf, size_t *len, size_t *cap)
{
    if (*len + n + 2 > *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        char *nb;
        while (nc < *len + n + 2) nc *= 2;
        nb = (char *)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, line, n);
    *len += n;
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
    return 1;
}

/* Scans the body for engine support. SLIDE/MEMCPY payload lines are skipped,
   not judged. Sets c->supported (0/1); the caller also folds in the meta
   flags. A body with no usable lines is unsupported: AddCheat would fail. */
static void body_check_support(cht_code *c)
{
    const char *p = c->body;
    int skip_next = 0, any = 0;
    c->supported = 1;
    if (!p || !*p) { c->supported = 0; return; }
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char w1[16];
        size_t w1n = 0, i = 0;
        unsigned long v;
        if (skip_next) { skip_next = 0; goto next; }
        while (i < n && p[i] != ' ' && p[i] != '\t' && w1n < sizeof w1 - 1) w1[w1n++] = p[i++];
        w1[w1n] = 0;
        if (!is_hex_word(w1, w1n)) { c->supported = 0; return; }  /* '?' in address */
        v = strtoul(w1, NULL, 16);
        any = 1;
        if (((v >> 24) & 0xFF) == 0x50 || ((v >> 24) & 0xFF) == 0xC2) skip_next = 1;
        else if (!is_classic((unsigned)((v >> 24) & 0xFF))) { c->supported = 0; return; }
    next:
        p = e ? e + 1 : p + n;
    }
    if (!any) c->supported = 0;
}

/* ---- parsing ---- */

typedef struct {
    cht_code cur;           /* code under construction */
    int have_cur;           /* a section header was seen */
    char *body;             /* growing body buffer for cur */
    size_t body_len, body_cap;
} parse_ctx;

static void ctx_free_cur(parse_ctx *x)
{
    code_free_members(&x->cur);
    free(x->body);
    x->body = NULL;
    x->body_len = x->body_cap = 0;
    x->have_cur = 0;
}

/* Moves cur into the list: ignored codes are dropped, same-name codes
   replace the earlier one (DuckStation semantics). Returns 0 on OOM. */
static int ctx_finish_code(parse_ctx *x, cht_db *db)
{
    int i;
    if (!x->have_cur || !x->cur.name) { ctx_free_cur(x); return 1; }
    x->cur.body = x->body;  /* hand the buffer over */
    x->body = NULL; x->body_len = x->body_cap = 0;
    if (x->cur.ignored) { code_free_members(&x->cur); return 1; }
    body_check_support(&x->cur);
    if (x->cur.type_assembly || x->cur.bad_meta) x->cur.supported = 0;
    for (i = 0; i < db->n_codes; i++) {
        if (!strcmp(db->codes[i].name, x->cur.name)) {
            code_free_members(&db->codes[i]);
            db->codes[i] = x->cur;
            memset(&x->cur, 0, sizeof x->cur);
            x->have_cur = 0;
            return 1;
        }
    }
    {
        cht_code *nc = (cht_code *)realloc(db->codes, sizeof(cht_code) * (size_t)(db->n_codes + 1));
        if (!nc) { code_free_members(&x->cur); return 0; }
        db->codes = nc;
        db->codes[db->n_codes++] = x->cur;
        memset(&x->cur, 0, sizeof x->cur);
        x->have_cur = 0;
    }
    return 1;
}

/* Handles one metadata line (key already split from value). */
static void parse_meta(parse_ctx *x, const char *key, const char *value)
{
    cht_code *c = &x->cur;
    if (!strcasecmp(key, "Type")) {
        if (!strcasecmp(value, "Assembly")) c->type_assembly = 1;
        else if (strcasecmp(value, "Gameshark")) c->bad_meta = 1;
    } else if (!strcasecmp(key, "Activation")) {
        if (!strcasecmp(value, "Manual")) c->manual = 1;
        else if (!strcasecmp(value, "StartFrame")) c->start_frame = 1;
        else if (!strcasecmp(value, "EndFrame")) { /* default, nothing to set */ }
        else c->bad_meta = 1;
    } else if (!strcasecmp(key, "Ignore")) {
        if (!strcasecmp(value, "true") || !strcmp(value, "1") || !strcasecmp(value, "yes"))
            c->ignored = 1;
    } else if (!strcasecmp(key, "Option")) {
        const char *colon = strrchr(value, ':');
        uint32_t v;
        char *nm;
        if (!colon) return;
        nm = xstrndup(value, (size_t)(colon - value));
        if (!nm) return;
        {
            char *t = trim(nm);
            char *nm2 = xstrdup(t);
            free(nm);
            if (!nm2) return;
            nm = nm2;
        }
        if (!parse_num(trim((char *)colon + 1), &v)) { free(nm); return; }
        {
            char **nn = (char **)realloc(c->opt_names, sizeof(char *) * (size_t)(c->n_opts + 1));
            uint32_t *nv = (uint32_t *)realloc(c->opt_vals, sizeof(uint32_t) * (size_t)(c->n_opts + 1));
            if (!nn || !nv) { free(nn); free(nv); free(nm); return; }
            c->opt_names = nn;
            c->opt_vals = nv;
            c->opt_names[c->n_opts] = nm;
            c->opt_vals[c->n_opts] = v;
            c->n_opts++;
            c->has_options = 1;
            c->is_range = 0;   /* an explicit list wins over a range */
        }
    } else if (!strcasecmp(key, "OptionRange")) {
        const char *colon = strchr(value, ':');
        uint32_t mn, mx;
        char *lo;
        if (!colon) return;
        lo = xstrndup(value, (size_t)(colon - value));
        if (!lo) return;
        if (!parse_num(trim(lo), &mn) || !parse_num(trim((char *)colon + 1), &mx)) {
            free(lo);
            return;
        }
        free(lo);
        if (c->n_opts == 0) {   /* a list, if any, wins (see above) */
            c->is_range = 1;
            c->range_min = mn;
            c->range_max = mx;
            c->has_options = 1;
        }
    }
    /* Author, Description and unknown keys: ignored. */
}

int cht_db_load(const char *path, cht_db *db)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;
    char *p;
    parse_ctx x;
    int ok = 1;

    memset(db, 0, sizeof *db);
    memset(&x, 0, sizeof x);
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -2; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = 0;
    fclose(f);

    p = buf;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = xstrndup(p, n);
        char *t;
        if (!line) { ok = 0; break; }
        t = trim(line);
        if (t[0] == '[') {
            /* Section header: the name runs to the last ']' on the line. */
            char *close = strrchr(t, ']');
            if (close && close > t + 1) {
                if (!ctx_finish_code(&x, db)) { free(line); ok = 0; break; }
                *close = 0;
                x.cur.name = xstrdup(trim(t + 1));
                if (!x.cur.name) { free(line); ok = 0; break; }
                x.have_cur = 1;
            }
            /* a malformed '[' line is skipped, like any other junk line */
        } else if (x.have_cur && t[0] && t[0] != ';' && t[0] != '#') {
            char *eq = strchr(t, '=');
            if (eq) {
                /* Metadata: an alphabetic key before '='. Anything else
                   with '=' is not a body line either, so it is skipped. */
                char *k = trim(xstrndup(t, (size_t)(eq - t)));
                int alpha = 0;
                size_t ki;
                if (k) {
                    alpha = k[0] != 0;
                    for (ki = 0; k[ki]; ki++)
                        if (!isalpha((unsigned char)k[ki])) alpha = 0;
                    if (alpha) parse_meta(&x, k, trim(eq + 1));
                    free(k);
                }
            } else {
                /* Body candidate: two whitespace-separated words. */
                char *s = t, *w1, *w2;
                size_t w1n, w2n;
                while (*s == ' ' || *s == '\t') s++;
                w1 = s;
                while (*s && *s != ' ' && *s != '\t') s++;
                w1n = (size_t)(s - w1);
                while (*s == ' ' || *s == '\t') s++;
                w2 = s;
                while (*s && *s != ' ' && *s != '\t') s++;
                w2n = (size_t)(s - w2);
                if (w1n > 0 && w2n > 0) {
                    size_t i;
                    int w1_hex = 1, w1_q = 0, w2_ok = 1;
                    for (i = 0; i < w1n; i++) {
                        if (w1[i] == '?') w1_q = 1;
                        else if (!isxdigit((unsigned char)w1[i])) w1_hex = 0;
                    }
                    if (w2n > 8) w2_ok = 0;
                    for (i = 0; i < w2n; i++)
                        if (!isxdigit((unsigned char)w2[i]) && w2[i] != '?') w2_ok = 0;
                    if (w1_q) x.cur.has_question = 1;   /* '?' in address: unsupported */
                    if ((w1_hex || w1_q) && w2_ok) {
                        /* Normalise the line to its two words: the filter
                           and the substitution only ever read those. */
                        char norm[32];
                        size_t nn = 0;
                        size_t j;
                        for (j = 0; j < w1n && nn < sizeof norm - 1; j++) norm[nn++] = w1[j];
                        norm[nn++] = ' ';
                        for (j = 0; j < w2n && nn < sizeof norm - 1; j++) norm[nn++] = w2[j];
                        norm[nn] = 0;
                        if (!body_append(norm, nn, &x.body, &x.body_len, &x.body_cap)) {
                            free(line);
                            ok = 0;
                            break;
                        }
                        if (w1_q) x.cur.supported = 0;  /* refined in body_check_support */
                        for (j = 0; j < w2n; j++)
                            if (w2[j] == '?') x.cur.has_question = 1;
                    }
                    /* otherwise: junk line, skipped DuckStation-style */
                }
            }
        }
        free(line);
        p = e ? e + 1 : p + n;
    }
    if (ok && !ctx_finish_code(&x, db)) ok = 0;
    if (!ok) {
        ctx_free_cur(&x);
        cht_db_free(db);
        free(buf);
        return -2;
    }
    ctx_free_cur(&x);
    free(buf);
    return 0;
}

void cht_db_free(cht_db *db)
{
    int i;
    for (i = 0; i < db->n_codes; i++) code_free_members(&db->codes[i]);
    free(db->codes);
    memset(db, 0, sizeof *db);
}

/* ---- substitution ---- */

char *cht_code_substitute(const cht_code *c, uint32_t opt_value)
{
    const char *p;
    char *out = NULL;
    size_t len = 0, cap = 0;
    if (!c->supported) return NULL;
    p = c->body;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[64];
        size_t i = 0;
        uint32_t addr, val;
        char w1[16], w2[16];
        size_t w1n = 0, w2n = 0;
        if (n >= sizeof line) return NULL;  /* cannot happen: lines are normalised */
        memcpy(line, p, n);
        line[n] = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && w1n < sizeof w1 - 1) w1[w1n++] = line[i++];
        w1[w1n] = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && w2n < sizeof w2 - 1) w2[w2n++] = line[i++];
        w2[w2n] = 0;
        addr = (uint32_t)strtoul(w1, NULL, 16);
        {
            /* Second word with '?'s zeroed, for the base value. */
            char base[16];
            size_t j;
            int qs = -1, qe = -1;
            for (j = 0; j < w2n; j++) {
                base[j] = w2[j] == '?' ? '0' : w2[j];
                if (w2[j] == '?') { if (qs < 0) qs = (int)j; qe = (int)j + 1; }
            }
            base[w2n] = 0;
            val = (uint32_t)strtoul(base, NULL, 16);
            if (qs >= 0) {
                /* One '?' field per line (DuckStation): bitpos counts from
                   the right end of the word as written. */
                int bitpos = (int)((w2n - (size_t)qe) * 4);
                int bitcount = (int)((size_t)(qe - qs) * 4);
                uint32_t mask = bitcount >= 32 ? 0xFFFFFFFFu : ((1u << bitcount) - 1u);
                val = (val & ~(mask << bitpos)) | ((opt_value & mask) << bitpos);
            }
        }
        {
            char tmp[32];
            int tl = snprintf(tmp, sizeof tmp, "%08X %08X\n", addr, val);
            if (tl < 0) { free(out); return NULL; }
            if (len + (size_t)tl + 1 > cap) {
                size_t nc = cap ? cap * 2 : 256;
                char *nb;
                while (nc < len + (size_t)tl + 1) nc *= 2;
                nb = (char *)realloc(out, nc);
                if (!nb) { free(out); return NULL; }
                out = nb;
                cap = nc;
            }
            memcpy(out + len, tmp, (size_t)tl);
            len += (size_t)tl;
            out[len] = 0;
        }
        p = e ? e + 1 : p + n;
    }
    if (!out) {
        out = xstrdup("");
        if (!out) return NULL;
    }
    return out;
}

void cht_dashed_serial(const char *serial, char *out, size_t n)
{
    size_t len;
    if (!serial) serial = "";
    len = strlen(serial);
    if (len > 4 && !strchr(serial, '-')) {
        /* First four chars, a dash, then the rest. */
        size_t a = 4 < n - 1 ? 4 : n - 1;
        size_t rest = len - 4;
        if (a + 1 + rest >= n) rest = n - 1 - a - 1;
        memcpy(out, serial, a);
        out[a] = '-';
        memcpy(out + a + 1, serial + 4, rest);
        out[a + 1 + rest] = 0;
    } else {
        snprintf(out, n, "%s", serial);
    }
}

/* ---- state file ---- */

int cht_state_load(const char *path, cht_state **out, int *n_out)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    cht_state *st = NULL;
    int n = 0;
    *out = NULL;
    *n_out = 0;
    if (!f) return 0;   /* no state file yet: nothing enabled, not an error */
    while (fgets(line, sizeof line, f)) {
        char *nl, *tab, *name;
        cht_state *ns;
        nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        ns = (cht_state *)realloc(st, sizeof(cht_state) * (size_t)(n + 1));
        if (!ns) { fclose(f); cht_state_free(st, n); return -1; }
        st = ns;
        tab = strchr(line, '\t');
        if (tab) *tab = 0;
        name = xstrdup(line);
        if (!name) { fclose(f); cht_state_free(st, n); return -1; }
        st[n].name = name;
        st[n].has_value = tab != NULL;
        st[n].value = tab ? (uint32_t)strtoul(tab + 1, NULL, 10) : 0;
        n++;
    }
    fclose(f);
    *out = st;
    *n_out = n;
    return 0;
}

void cht_state_free(cht_state *st, int n)
{
    int i;
    for (i = 0; i < n; i++) free(st[i].name);
    free(st);
}

int cht_state_save(const char *path, const cht_code *codes,
                   const int *enabled, const uint32_t *opt_values, int n)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return -1;
    fprintf(f, "# cheat state: one enabled code name per line, Name<TAB>value for option codes\n");
    for (i = 0; i < n; i++) {
        if (!enabled[i]) continue;
        if (codes[i].has_options) fprintf(f, "%s\t%u\n", codes[i].name, opt_values[i]);
        else fprintf(f, "%s\n", codes[i].name);
    }
    return fclose(f) == 0 ? 0 : -1;
}
