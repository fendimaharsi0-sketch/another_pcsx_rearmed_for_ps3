/* Game list, settings and the in-game menu, drawn on the overlay. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include "common.h"
#include "core_hooks.h"
#include "cover.h"
#include "cheatdb.h"
#include "hwr/hwr.h"

#define MAX_GAMES 2000
static char *games[MAX_GAMES];
static int ngames, scanned;
static unsigned char own_cfg[MAX_GAMES];    /* the game has configs/<game>.ini */

static void check_own_settings(void)
{
    int i;
    for (i = 0; i < ngames; i++) {
        char key[256], *dot;
        const char *b = strrchr(games[i], '/');
        snprintf(key, sizeof key, "%s", b ? b + 1 : games[i]);
        if ((dot = strrchr(key, '.'))) *dot = 0;
        own_cfg[i] = (unsigned char)config_game_exists(key);
    }
}

static const char *scan_dirs[] = {
    APP_DIR "/roms", "/dev_hdd0/PSXISO", "/dev_hdd0/ROMS/PSX", "/dev_hdd0/ROMS/PS1", "/dev_hdd0/roms/psx",
    "/dev_usb000/PSXISO", "/dev_usb000/ROMS/PSX", "/dev_usb000/roms/psx",
    "/dev_usb001/PSXISO", "/dev_usb001/ROMS/PSX", "/dev_usb001/roms/psx", NULL };

static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

static int is_ext(const char *n, const char *e) { return !strcasecmp(ext_of(n), e); }

/* names listed in the .m3u playlists of a directory (those discs are reached
   through the playlist, so they are not listed separately) */
#define MAX_LISTED 64
static char listed[MAX_LISTED][256];
static int nlisted;

static void read_m3u(const char *dir, const char *name)
{
    char path[1300], line[300];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "r"))) return;
    while (nlisted < MAX_LISTED && fgets(line, sizeof line, f)) {
        char *e = line + strlen(line), *b;
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        b = strrchr(line, '/');
        snprintf(listed[nlisted++], sizeof listed[0], "%s", b ? b + 1 : line);
    }
    fclose(f);
}

static int is_listed(const char *name)
{
    int i;
    for (i = 0; i < nlisted; i++) if (!strcasecmp(listed[i], name)) return 1;
    return 0;
}

#define MAX_DIR_ENTRIES 4096

/* one directory: read it once, then decide what to show */
static void scan(const char *dir, int depth)
{
    static const char *exts[] = { "cue", "chd", "pbp", "m3u", "ccd", "iso", "img", "bin", "exe", NULL };
    char **names = NULL;
    unsigned char *isdir = NULL;
    int n = 0, i, has_sheet = 0;
    DIR *d;
    struct dirent *de;

    app_poll_system();
    WHERE("menu: scanning folders");
    if (app_exit_requested || !(d = opendir(dir))) return;
    names = malloc(MAX_DIR_ENTRIES * sizeof *names);
    isdir = malloc(MAX_DIR_ENTRIES);
    if (!names || !isdir) { closedir(d); free(names); free(isdir); return; }
    while ((de = readdir(d)) && n < MAX_DIR_ENTRIES) {
        if (de->d_name[0] == '.') continue;
        names[n] = strdup(de->d_name);
        if (!names[n]) break;
        isdir[n] = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {          /* filesystem without d_type: ask */
            char path[1300]; struct stat st;
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            isdir[n] = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        }
        n++;
    }
    closedir(d);

    nlisted = 0;
    for (i = 0; i < n; i++) {
        if (isdir[i]) continue;
        if (is_ext(names[i], "cue") || is_ext(names[i], "ccd")) has_sheet = 1;
        if (is_ext(names[i], "m3u")) read_m3u(dir, names[i]);
    }
    for (i = 0; i < n && ngames < MAX_GAMES; i++) {
        int k;
        if (isdir[i]) continue;
        for (k = 0; exts[k]; k++) if (is_ext(names[i], exts[k])) break;
        if (!exts[k]) continue;
        /* track files described by a .cue/.ccd next to them */
        if ((is_ext(names[i], "bin") || is_ext(names[i], "img")) && has_sheet) continue;
        if (is_listed(names[i])) continue;
        {
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            games[ngames++] = strdup(path);
        }
    }
    if (depth < 2)
        for (i = 0; i < n; i++)
            if (isdir[i]) {
                char path[1300];
                snprintf(path, sizeof path, "%s/%s", dir, names[i]);
                scan(path, depth + 1);
            }
    for (i = 0; i < n; i++) free(names[i]);
    free(names); free(isdir);
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int cmp_games(const void *a, const void *b) { return strcasecmp(base_name(*(char *const *)a), base_name(*(char *const *)b)); }

static void cover_release(void);   /* defined with the cover panel, below */

static void rescan(void)
{
    int i;
    for (i = 0; i < ngames; i++) free(games[i]);
    ngames = 0;
    for (i = 0; scan_dirs[i]; i++) scan(scan_dirs[i], 0);
    qsort(games, ngames, sizeof *games, cmp_games);
    scanned = 1;
    /* A rescan is also when a refreshed mygames.xml should take effect:
       drop the parsed XML and the cached cover, so the next frame's
       cover lookup re-reads the XML even if the selection did not move. */
    cover_xml_invalidate();
    cover_release();
    logf_("menu: %d games found\n", ngames);
}

/* ---------------- drawing helpers ---------------- */
#define C_TEXT   0xffe8e8e8
#define C_DIM    0xff9098a8
#define C_ACCENT 0xffffd24a
#define C_PANEL  0xe0141a26
#define PANEL_R  16   /* rounded panel corner radius, px at 720p (cover + quick menu) */
#define C_SEL    0xff2c3a58

static const char *menu_name = "game list";

static void frame_begin(int over_game)
{
    video_begin();
    WHERE("menu: system callbacks");
    app_poll_system();
    WHERE("menu: reading pads");
    input_poll();
    if (input_pressed()) {
        char b[96];
        logf_("menu (%s): pressed %s\n", menu_name, input_names(input_pressed(), b, sizeof b));
    }
    WHERE("menu: drawing");
    if (over_game) {
        /* the game picture behind the menu: no smoothing passes (SPU smoothing
           re-run for every menu frame held the menu at 20 fps) */
        video_redraw_quick(1);
        hooks_redraw_last_frame();
        video_redraw_quick(0);
    }
    else video_clear(0xff0c1018);
    video_overlay_clear();
}

static void frame_end(void)
{
    video_draw_overlay();
    video_present();
}

/* text stays inside this margin (many TVs cut off the edges of the picture) */
#define MARGIN 64

static void header(const char *title, const char *sub)
{
    ovl_fill(0, 0, OVL_W, 64, 0xff182238);
    ovl_text(MARGIN, 17, title, C_ACCENT);
    if (sub) ovl_text(OVL_W - MARGIN - ovl_text_width(sub), 17, sub, C_DIM);
}

/* App-title header (game list + disc interstitial only): the picked style —
   a yellow accent bar + white title, instead of the plain yellow title
   header() draws. Scoped to APP_TITLE on purpose: settings and tool
   screens keep header() and their own titles. Bar 4 px, title-height 30,
   12 px gap, title at MARGIN + 16 — same margins as header(), so CRT
   overscan treats it exactly like the old title. */
static void header_app(const char *sub)
{
    ovl_fill(0, 0, OVL_W, 64, 0xff182238);
    ovl_fill(MARGIN, 17, 4, 30, C_ACCENT);
    ovl_text(MARGIN + 16, 17, APP_TITLE, C_TEXT);
    if (sub) ovl_text(OVL_W - MARGIN - ovl_text_width(sub), 17, sub, C_DIM);
}

static int draw_prompt(int x, int y, const char *s, uint32_t color, uint32_t bg);
static int prompt_width(const char *s);

static void footer(const char *s)
{
    ovl_fill(0, OVL_H - 52, OVL_W, 52, 0xff182238);
    draw_prompt(MARGIN, OVL_H - 41, s, C_DIM, 0xff182238);
}

/* two-line footer (the game list) */
static void footer2(const char *a, const char *b)
{
    ovl_fill(0, OVL_H - 84, OVL_W, 84, 0xff182238);
    draw_prompt(MARGIN, OVL_H - 76, a, C_DIM, 0xff182238);
    draw_prompt(MARGIN, OVL_H - 44, b, C_DIM, 0xff182238);
}

/* Footer prompts with PS button glyphs (monochrome: glyphs use the text
   colour). Face buttons are 24 px glyphs in a 30 px text cell (y + 3
   centres them); shoulders/select/start are chips (pills with a text
   label, 30 px tall since the label text itself is 30 px). Only whole
   words are replaced: "X", "Circle", "Triangle", "Square", "L1", "R1",
   "SELECT", "START" — D-pad ("Up/Down") stays text. A button is followed
   by a 6 px gap; the single space after a button name is consumed, since
   the gap replaces it. Footer strings write shoulders as two words
   ("L1 R1") and the combo spaced ("SELECT + START") so each gets its
   own chip. bg is the solid background behind a chip (the footer band
   for footer/footer2); callers whose strings hold no chip words may
   pass 0. */
#define BTN_ADV 30   /* glyph 24 + gap 6 */
/* Chip ids (menu.c-local, above the GLYPH_* range). */
#define CHIP_BASE 100
#define CHIP_L1 100
#define CHIP_R1 101
#define CHIP_SELECT 102
#define CHIP_START 103
static const char *chip_label(int btn)
{
    switch (btn) {
    case CHIP_L1: return "L1";
    case CHIP_R1: return "R1";
    case CHIP_SELECT: return "SELECT";
    case CHIP_START: return "START";
    default: return "";
    }
}
static int prompt_button(const char *s, int *btn)
{
    static const struct { const char *name; int btn; } b[] = {
        { "Triangle", GLYPH_TRIANGLE }, { "Circle", GLYPH_CIRCLE },
        { "Square", GLYPH_SQUARE }, { "X", GLYPH_CROSS },
        { "L1", CHIP_L1 }, { "R1", CHIP_R1 },
        { "SELECT", CHIP_SELECT }, { "START", CHIP_START },
    };
    unsigned i;
    for (i = 0; i < sizeof b / sizeof b[0]; i++) {
        size_t n = strlen(b[i].name);
        if (!strncmp(s, b[i].name, n) && (s[n] == ' ' || s[n] == 0)) { *btn = b[i].btn; return (int)n; }
    }
    return 0;
}

/* Chip advance: pill width (label + 16 padding) + 6 gap. */
static int chip_adv(const char *label) { return ovl_text_width(label) + 16 + 6; }

/* Chip (pill) label: outline pill in the text colour over bg, text
   centred. 30 px tall (the text cell — taller than the 24 px face
   glyphs, since the label text itself is 30 px). Returns the advance. */
static int draw_chip(int x, int y, const char *label, uint32_t color, uint32_t bg)
{
    int w = ovl_text_width(label) + 16;
    ovl_round_fill(x, y, w, 30, 15, color);
    ovl_round_fill(x + 2, y + 2, w - 4, 26, 13, bg);
    ovl_text(x + 8, y, label, color);
    return w + 6;
}

static int draw_prompt(int x, int y, const char *s, uint32_t color, uint32_t bg)
{
    int i = 0, seg = 0;
    char buf[256];
    while (s[i]) {
        int btn, n = 0;
        if ((i == 0 || s[i - 1] == ' ') && (n = prompt_button(s + i, &btn)) > 0) {
            if (i > seg) {
                size_t len = (size_t)(i - seg);
                if (len >= sizeof buf) len = sizeof buf - 1;
                memcpy(buf, s + seg, len); buf[len] = 0;
                x = ovl_text(x, y, buf, color);
            }
            if (btn >= CHIP_BASE) x += draw_chip(x, y, chip_label(btn), color, bg);
            else { ovl_button(x, y + 3, btn, color); x += BTN_ADV; }
            i += n;
            if (s[i] == ' ') i++;
            seg = i;
        } else i++;
    }
    if (i > seg) {
        size_t len = (size_t)(i - seg);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, s + seg, len); buf[len] = 0;
        x = ovl_text(x, y, buf, color);
    }
    return x;
}

static int prompt_width(const char *s)
{
    int w = 0, i = 0, cw = ovl_text_width("W");
    while (s[i]) {
        int btn, n = 0;
        if ((i == 0 || s[i - 1] == ' ') && (n = prompt_button(s + i, &btn)) > 0) {
            w += btn >= CHIP_BASE ? chip_adv(chip_label(btn)) : BTN_ADV;
            i += n;
            if (s[i] == ' ') i++;
        } else { w += cw; i++; }
    }
    return w;
}



/* text cut with "..." so it ends before the right margin */
static void text_fit(int x, int y, const char *s, uint32_t argb)
{
    char b[256];
    int max = (OVL_W - MARGIN - x) / ovl_text_width("W");
    if ((int)strlen(s) <= max || max < 4) { ovl_text(x, y, s, argb); return; }
    snprintf(b, sizeof b, "%.*s...", max - 3, s);
    ovl_text(x, y, b, argb);
}

/* ---------------- settings ---------------- */
static const char *filter_names[] = { "Sharp (nearest)", "Smooth (bilinear)", "FXAA (RSX)", "Smoothing on SPUs" };
static const char *aspect_names[] = { "4:3", "Stretch to screen", "14:9", "Zoom 14:9", "Zoom 16:9" };
/* menu order of the ASPECT_* values */
static const int aspect_order[] = { ASPECT_4_3, ASPECT_14_9, ASPECT_ZOOM_14_9, ASPECT_ZOOM, ASPECT_FULL };

typedef struct { const char *label; const char *help; } item_t;
static const item_t settings_items[] = {
    { "CPU", "Dynarec is much faster. 2 threads is experimental and can freeze some games." },
    { "Renderer", "RSX draws 3D at higher resolution on the PS3 GPU (experimental)." },
    { "Texture filter", "Nearest / Bilinear (manual, all modes) / Bilinear (Performance: hardware 15-bit, manual CLUT). RSX only, upscaled pass only. Native VRAM stays nearest." },
    { "Sprite filter", "Texture filter for sprites (textured GP0 sprites and axis-aligned textured polygons). Nearest keeps small sprites, particles and HUD icons crisp. Same modes as Texture filter. RSX only, upscaled pass only." },
    { "Picture filter", "How the picture is scaled to your TV." },
    { "Aspect ratio", "Zoom keeps the PS1 shape and crops the top and bottom." },
    { "Widescreen (GTE Hack 16:9)", "GTE widescreen: 3D vertices projected by the GTE are compressed 3/4, then stretched back by Aspect = Stretch to screen on a 16:9 TV, widening the field of view. Pair with Stretch; at 4:3 it only looks squashed. Sprites, HUD and 2D backgrounds are not compressed, so they stretch with the display. Takes effect immediately, no game restart." },
    { "Screen size", "Press X to shrink or stretch the picture to fit your TV." },
    { "Frame skip", "Auto skips drawing frames in heavy scenes. Software renderer only." },
    { "Dithering", "PS1 dithering (software renderer and RSX)." },
    { "Show FPS", "" },
    { "GPU thread", "Software renderer on the PS3's second CPU thread: faster." },
    { "Left stick as D-pad", "The left stick also presses the D-pad (for digital-pad games)." },
    { "Right stick as buttons", "Right stick up/down/left/right press Triangle/X/Square/Circle." },
    { "Vibration", "Controller vibration in games that use it (DualShock 3)." },
    { "FMV decoding", "SPUs: the Cell's SPUs decode the videos while the PPU keeps emulating." },
    { "PS1 CPU speed", "Faster: smoother games that slowed down on a PS1. Some may glitch." },
    { "Sound when slow", "Slow down: sound slows with the game instead of crackling." },
    { "Skip intro videos", "Skips videos in a game's first 3 minutes (presses Start/X; fast-forwards the rest)." },
    { "PAL games at 60 Hz", "European games run at 60 Hz: 20% faster. From the next game start." },
    { "PS1 logo at start", "Shows the PlayStation logo at game start. Needs a BIOS in USRDIR/system." },
    { "Speed limit", "Normal: catches up after a hitch. Strict: never faster. Off: no limit." },
    { "VRAM check (debug)", "Debug: software renderer draws too. Pause menu > Dump VRAM." },
    { "Debug log", "Debug: writes log files (USRDIR/log.txt, USRDIR/logs) and hang reports." },
    { "Memory card 1", "X opens the card picker: Auto = this game's own card (saves/SCUS-12345_1.mcd), then the imported cards, then None. The other slot cannot pick the same imported file. Next launch only; Reset alone does not apply it." },
    { "Memory card 2", "X opens the card picker: Auto = this game's own card (saves/SCUS-12345_2.mcd), then the imported cards, then None. The other slot cannot pick the same imported file. Default is None. Next launch only." },
    { "Import PS3 VMC", "Press X to copy a PS3 virtual card from savedata/vmc/ into the imported library (saves/imported/). It does not change the active card or mode: pick it in Memory card 1 or 2 to play with it. One-way: saves made here never appear in XMB. Game list only, never in a game." },
    { "Analog joystick", "DualShock analog sticks on, or a digital pad. When on, Left stick as D-pad and Right stick as buttons are bypassed (the stick and D-pad stay independent)." },
    { "Turbo CD", "Faster CD loads: instant seeks + quicker reads. Auto-off during XA audio and FMV." },
    { "Enable cheats", "Master switch for cheats. The Cheats menu in the pause menu only appears when this is on, and no cheat is applied while it is off. Cheats write to game memory every frame and can crash games or corrupt saves." },
};
#define NSET (int)(sizeof settings_items / sizeof settings_items[0])
#define SET_ROW 27   /* row height (30-pixel font) */
#define SET_VISIBLE 17   /* rows shown above the help line; the list scrolls past them */

/* ---------------- settings pages ----------------
   The 30 settings are grouped into 5 pages, flipped with L1/R1 (top row).
   set_page_items[p] lists the GLOBAL settings_items indexes on page p, in
   display order; set_page_counts[p] is its length. Every item must appear
   exactly once: the typedef below fails the build if the counts stop adding
   up to NSET, and the host test checks uniqueness and the grouping itself. */
#define NPAGES 5
static const char *set_page_titles[NPAGES] = {
    "Performance", "Picture", "Media & Audio", "Controls", "System & Saves"
};
static const int set_page_items[NPAGES][9] = {
    { 0, 16, 21, 8, 11, 28 },                /* Performance: CPU, PS1 CPU speed, Speed limit, Frame skip, GPU thread, Turbo CD */
    { 1, 2, 3, 4, 5, 6, 7, 9, 10 },          /* Picture: Renderer, Texture filter, Sprite filter, Picture filter, Aspect, Widescreen, Screen size, Dithering, Show FPS */
    { 15, 18, 17 },                          /* Media & Audio: FMV decoding, Skip intro videos, Sound when slow */
    { 12, 13, 14, 27 },                      /* Controls: Left stick as D-pad, Right stick as buttons, Vibration, Analog joystick */
    { 19, 20, 22, 23, 24, 25, 26, 29 },      /* System & Saves: PAL 60 Hz, PS1 logo, VRAM check, Debug log, Memory card 1, Memory card 2, Import PS3 VMC, Enable cheats */
};
static const int set_page_counts[NPAGES] = { 6, 9, 3, 4, 8 };
typedef char set_pages_cover_all[(6 + 9 + 3 + 4 + 8 == NSET) ? 1 : -1];
/* per-page cursor memory: the row each page was left on (reset on panel open) */
static int set_page_row[NPAGES];
static int set_page_of(int g)
{
    int p, r;
    for (p = 0; p < NPAGES; p++)
        for (r = 0; r < set_page_counts[p]; r++)
            if (set_page_items[p][r] == g) return p;
    return 0;
}
static int set_row_of(int g)
{
    int p, r;
    for (p = 0; p < NPAGES; p++)
        for (r = 0; r < set_page_counts[p]; r++)
            if (set_page_items[p][r] == g) return r;
    return 0;
}

static void setting_value(int i, char *b, size_t n)
{
    switch (i) {
    case 0: snprintf(b, n, "%s", !cfg.dynarec ? "Interpreter" : cfg.dynarec_thread ? "Dynarec, 2 threads" : "Dynarec, 1 thread"); break;
    case 1:
        if (cfg.gpu_scale) {
            /* in a game: the resolution it is drawn at now (its picture size times
               the scale); in the game list only the scale (games differ) */
            int w, h, sx = cfg.gpu_scale > 4 ? 4 : cfg.gpu_scale;
            float sy = cfg.gpu_scale > 4 ? 4.5f : (float)cfg.gpu_scale;
            const char *name = cfg.gpu_scale > 4 ? "RSX 4.5x" : cfg.gpu_scale == 4 ? "RSX 4x" : cfg.gpu_scale == 3 ? "RSX 3x" :
                               cfg.gpu_scale == 2 ? "RSX 2x" : "RSX 1x";
            hwr_shown_size(&w, &h);
            {   /* drawn at less when the screen cannot show more (480-line pictures) */
                int ex; float ey;
                hooks_gpu_scale(&ex, &ey);
                if (ex > 0) { sx = ex; sy = ey; }
            }
            if (w > 0 && h > 0) snprintf(b, n, "%s (%dx%d)", name, w * sx, (int)(h * sy + 0.5f));
            else snprintf(b, n, "%s", name);
        } else snprintf(b, n, "Software (native)");
        break;
    case 2: snprintf(b, n, "%s", cfg.texfilter == 2 ? "Bilinear (Performance)" : cfg.texfilter ? "Bilinear" : "Nearest"); break;
    case 3: snprintf(b, n, "%s", cfg.sprite_texfilter == 2 ? "Bilinear (Performance)" : cfg.sprite_texfilter ? "Bilinear" : "Nearest"); break;
    case 4: snprintf(b, n, "%s", filter_names[cfg.filter]); break;
    case 5: snprintf(b, n, "%s", aspect_names[cfg.aspect]); break;
    case 6: snprintf(b, n, "%s", cfg.widescreen ? "On (16:9)" : "Off"); break;
    case 7: snprintf(b, n, "%d%% x %d%%", cfg.screen_w, cfg.screen_h); break;
    case 8: snprintf(b, n, "%s", cfg.frameskip ? "Auto" : "Off"); break;
    case 9: snprintf(b, n, "%s", cfg.dithering ? "On" : "Off"); break;
    case 10: snprintf(b, n, "%s", cfg.show_fps ? "On" : "Off"); break;
    case 11: snprintf(b, n, "%s", cfg.gpu_thread ? "On" : "Off"); break;
    case 12: snprintf(b, n, "%s", cfg.stick_dpad ? "On" : "Off"); break;
    case 13: snprintf(b, n, "%s", cfg.stick_buttons ? "On" : "Off"); break;
    case 14: snprintf(b, n, "%s", cfg.vibration ? "On" : "Off"); break;
    case 15: snprintf(b, n, "%s", cfg.spu_mdec ? "SPUs" : "PPU"); break;
    case 16: snprintf(b, n, "%s", cfg.cpu_clock == 2 ? "+75%" : cfg.cpu_clock == 1 ? "+50%" : "Normal"); break;
    case 17: snprintf(b, n, "%s", cfg.audio_stretch ? "Slow down" : "Keep pitch"); break;
    case 18: snprintf(b, n, "%s", cfg.skip_intro ? "On" : "Off"); break;
    case 19: snprintf(b, n, "%s", cfg.pal60 ? "On" : "Off"); break;
    case 20: snprintf(b, n, "%s", !cfg.boot_logo ? "Off" : hooks_bios_present() ? "On" : "On (no BIOS found)"); break;
    case 21: snprintf(b, n, "%s", cfg.speed_limit == 1 ? "Strict" : cfg.speed_limit == 2 ? "Off" : "Normal"); break;
    case 22: snprintf(b, n, "%s", cfg.vram_check ? "On" : "Off"); break;
    case 23: snprintf(b, n, "%s", cfg.debug_log ? "On" : "Off"); break;
    case 24: snprintf(b, n, "%s", cfg.mc1_mode == 1 ? (cfg.mc1_shared[0] ? cfg.mc1_shared : "Shared (default)") : cfg.mc1_mode == 2 ? "None" : "Auto (per-game)"); break;
    case 25: snprintf(b, n, "%s", cfg.mc2_mode == 1 ? (cfg.mc2_shared[0] ? cfg.mc2_shared : "Shared (default)") : cfg.mc2_mode == 2 ? "None" : "Auto (per-game)"); break;
    case 26: snprintf(b, n, "%s", cfg.vmc_source[0] ? cfg.vmc_source : "Press X to import"); break;
    case 27: snprintf(b, n, "%s", cfg.analog ? "On (DualShock)" : "Off (digital pad)"); break;
    case 28: snprintf(b, n, "%s", cfg.turbo_cd ? "On" : "Off"); break;
    case 29: snprintf(b, n, "%s", cfg.cheat_enable ? "On" : "Off"); break;
    }
}

static void setting_value(int i, char *b, size_t n);
static void setting_change_(int i, int dir, int in_game);
static void screen_size_page(int in_game);
static void vmc_import_page(void);
static void vmc_slot_picker(int slot);
static void setting_change(int i, int dir, int in_game)
{
    char before[64], after[64];
    setting_value(i, before, sizeof before);
    setting_change_(i, dir, in_game);
    setting_value(i, after, sizeof after);
    logf_("settings: %s: %s -> %s%s\n", settings_items[i].label, before, after, in_game ? " (in game)" : "");
}

static void setting_change_(int i, int dir, int in_game)
{
    switch (i) {
    case 0:                                             /* engine: live on resume via update_variables; thread mode: next launch */
        {   /* Dynarec, 2 threads (Lightrec compiles on the PPU's second thread) -> 1 thread -> Interpreter */
            int k = !cfg.dynarec ? 2 : cfg.dynarec_thread ? 0 : 1;
            k = (k + (dir > 0 ? 1 : 2)) % 3;
            cfg.dynarec = k != 2;
            if (k != 2) cfg.dynarec_thread = k == 0;
        }
        break;
    case 1: {
        /* Software (native) -> RSX 1x (native, drawn by the RSX) -> 2x -> 3x -> 4x -> 4.5x */
        static const int order[] = { 0, 1, 2, 3, 4, 5 };
        int k = 0;
        while (k < 5 && order[k] != cfg.gpu_scale) k++;
        k = (k + (dir > 0 ? 1 : 5)) % 6;
        cfg.gpu_scale = order[k];
        break;
    }
    case 2: cfg.texfilter = (cfg.texfilter + (dir > 0 ? 1 : 2)) % 3; break;
    case 3: cfg.sprite_texfilter = (cfg.sprite_texfilter + (dir > 0 ? 1 : 2)) % 3; break;
    case 4: cfg.filter = (cfg.filter + (dir > 0 ? 1 : FILTER_COUNT - 1)) % FILTER_COUNT; break;
    case 5: {
        int k = 0, n = (int)(sizeof aspect_order / sizeof aspect_order[0]);
        while (k < n - 1 && aspect_order[k] != cfg.aspect) k++;
        cfg.aspect = aspect_order[(k + (dir > 0 ? 1 : n - 1)) % n];
        break;
    }
    case 6: cfg.widescreen ^= 1; gte_widescreen = cfg.widescreen; break;
    case 7: screen_size_page(in_game); break;
    case 8: cfg.frameskip ^= 1; break;
    case 9: cfg.dithering ^= 1; break;
    case 10: cfg.show_fps ^= 1; break;
    case 11: cfg.gpu_thread ^= 1; break;
    case 12: cfg.stick_dpad ^= 1; break;
    case 13: cfg.stick_buttons ^= 1; break;
    case 14: cfg.vibration ^= 1; break;
    case 15: cfg.spu_mdec ^= 1; break;
    case 16: cfg.cpu_clock = (cfg.cpu_clock + (dir > 0 ? 1 : 2)) % 3; break;
    case 17: cfg.audio_stretch ^= 1; break;
    case 18: cfg.skip_intro ^= 1; break;
    case 19: cfg.pal60 ^= 1; break;
    case 20: cfg.boot_logo ^= 1; break;
    case 21: cfg.speed_limit = (cfg.speed_limit + (dir > 0 ? 1 : 2)) % 3; break;
    case 22: cfg.vram_check ^= 1; break;
    case 23: cfg.debug_log ^= 1; break;
    case 24: vmc_slot_picker(1); break;   /* any key opens the picker; it only writes settings, so it is safe in a game too (launch-only, like the old mode toggle) */
    case 25: vmc_slot_picker(2); break;
    case 26: if (!in_game) vmc_import_page(); break;   /* import writes a card file: game list only, never in a game */
    case 27: cfg.analog ^= 1; break;
    case 28: cfg.turbo_cd ^= 1; break;
    case 29: cfg.cheat_enable ^= 1;
        /* The gate cuts application, not just menu visibility: turning it
           off mid-game resets every fed code at once, turning it on
           re-reads the state file (manual codes do not fire: there was no
           trigger, the same as at game start). From the game list there is
           no engine to touch; the value is saved on panel exit. */
        if (in_game) {
            if (cfg.cheat_enable) cheat_apply_saved(0);
            else cheat_reset_all();
        }
        break;
    }
}

/* Screen size: the whole picture (game and menus, see video.c screen_area) is
   shrunk or stretched until the frame drawn at the edges of the overlay just
   fits the TV. Changes show at once; X or O keeps them, Square resets. */
static void screen_size_page(int in_game)
{
    logf_("menu: screen size\n");
    while (!app_exit_requested) {
        uint32_t p;
        char b[64];
        int i, cx = OVL_W / 2, y = OVL_H / 2 - 105;
        frame_begin(in_game);            /* in game: the game shows behind */
        p = input_pressed();
        {   /* a held direction repeats after a moment */
            static int held_frames;
            uint32_t h = (input_pad(0)->buttons | input_pad(1)->buttons) & (PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN);
            held_frames = h ? held_frames + 1 : 0;
            if (held_frames > 20 && held_frames % 3 == 0) p |= h;
        }
        if ((p & PAD_LEFT) && cfg.screen_w > SCREEN_MIN) cfg.screen_w--;
        if ((p & PAD_RIGHT) && cfg.screen_w < SCREEN_MAX) cfg.screen_w++;
        if ((p & PAD_DOWN) && cfg.screen_h > SCREEN_MIN) cfg.screen_h--;
        if ((p & PAD_UP) && cfg.screen_h < SCREEN_MAX) cfg.screen_h++;
        if (p & PAD_SQUARE) cfg.screen_w = cfg.screen_h = 100;
        if (p & (PAD_CROSS | PAD_CIRCLE | PAD_TRIANGLE)) { config_save(); video_borders_dirty(); return; }
        /* the frame: the overlay's edges, with corner marks and edge arrows */
        ovl_fill(0, 0, OVL_W, 4, 0xffffffff); ovl_fill(0, OVL_H - 4, OVL_W, 4, 0xffffffff);
        ovl_fill(0, 0, 4, OVL_H, 0xffffffff); ovl_fill(OVL_W - 4, 0, 4, OVL_H, 0xffffffff);
        ovl_fill(4, 4, 60, 8, C_ACCENT); ovl_fill(4, 4, 8, 60, C_ACCENT);
        ovl_fill(OVL_W - 64, 4, 60, 8, C_ACCENT); ovl_fill(OVL_W - 12, 4, 8, 60, C_ACCENT);
        ovl_fill(4, OVL_H - 12, 60, 8, C_ACCENT); ovl_fill(4, OVL_H - 64, 8, 60, C_ACCENT);
        ovl_fill(OVL_W - 64, OVL_H - 12, 60, 8, C_ACCENT); ovl_fill(OVL_W - 12, OVL_H - 64, 8, 60, C_ACCENT);
        for (i = 0; i < 12; i++) {       /* arrows pointing at the middle of each edge */
            ovl_fill(cx - i, 8 + i, 2 * i + 1, 1, C_ACCENT);
            ovl_fill(cx - i, OVL_H - 9 - i, 2 * i + 1, 1, C_ACCENT);
            ovl_fill(8 + i, OVL_H / 2 - i, 1, 2 * i + 1, C_ACCENT);
            ovl_fill(OVL_W - 9 - i, OVL_H / 2 - i, 1, 2 * i + 1, C_ACCENT);
        }
        ovl_fill(cx - 420, y - 24, 840, 278, C_PANEL);
        ovl_text(cx - ovl_text_width("Screen size") / 2, y, "Screen size", C_ACCENT);
        snprintf(b, sizeof b, "Width %d%%    Height %d%%", cfg.screen_w, cfg.screen_h);
        ovl_text(cx - ovl_text_width(b) / 2, y + 45, b, C_TEXT);
        ovl_text(cx - ovl_text_width("Adjust until all four edges of the") / 2, y + 95, "Adjust until all four edges of the", C_DIM);
        ovl_text(cx - ovl_text_width("frame are just visible on your TV.") / 2, y + 125, "frame are just visible on your TV.", C_DIM);
        ovl_text(cx - ovl_text_width("Left/Right width   Up/Down height") / 2, y + 170, "Left/Right width   Up/Down height", C_TEXT);
        draw_prompt(cx - prompt_width("Square reset   X done") / 2, y + 200, "Square reset   X done", C_TEXT, 0);
        frame_end();
    }
}

/* ---- PS3 VMC import + shared-card library (core-direct, no copy) ----
 * A PS1 card is only 128 KB (15 blocks), so a PS3 user usually has MANY .VM1
 * files, not one: the pickers below sort them and scroll (L1/R1 page), they
 * are not fixed short lists. The source in savedata/vmc/ is opened read-only
 * ("rb") and never written: XMB's cache is never invalidated by us.
 * Import copies a VM1 into the library (saves/imported/<name>.mcd) and
 * changes nothing else: not the active card, not the mode. Each slot's
 * picker (Memory card 1 / 2) then chooses that slot's card: Auto first
 * (the game's own serial card), then the imported files, then None, plus
 * the legacy default shared card (pcsx-card<slot>.mcd) when such a file
 * exists and is valid. The choice is stored as the slot's mode + library
 * file name (mc1_mode/mc1_shared, mc2_mode/mc2_shared); the core reads
 * the chosen file directly at the next launch (frontend/libretro.c
 * load_memcards, pcsx_rearmed_memcard<slot>_shared variables), with no
 * copy and no write-back step, so switching cards can never lose saves.
 * With no card chosen, a Shared slot falls back to its default
 * pcsx-card<slot>.mcd, exactly as before this feature. One-way: saves made
 * here go to the library file, never back to the XMB VM1. The slot picker
 * only writes settings, so unlike import it is safe in a game too (the
 * choice still applies on the next launch only, and in-game it edits the
 * game's own settings when Own settings is on, like every other setting).
 * Same-file protection: the other slot's chosen imported file is shown
 * dimmed and refused with a message, so two slots can never share one
 * library file (Auto uses per-slot serial files, so Auto+Auto is fine). */
#define VMC_DIR "/dev_hdd0/savedata/vmc"
#define LIB_DIR APP_DIR "/saves/imported"
#define VMC_SIZE 131072
#define MAX_VMC 256
static char vmc_names[MAX_VMC][256];
static int nvmc;

static int vmc_name_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* Sorted scan of one directory for one extension, into vmc_names (the two
   pickers are modal, so sharing the buffer is safe). */
static int vmc_scan_dir(const char *dir, const char *ext)
{
    DIR *d;
    struct dirent *e;
    int total = 0;
    nvmc = 0;
    if (!(d = opendir(dir))) return 0;
    while ((e = readdir(d)) != NULL) {
        if (!is_ext(e->d_name, ext)) continue;
        total++;
        if (nvmc < MAX_VMC) snprintf(vmc_names[nvmc++], sizeof vmc_names[0], "%s", e->d_name);
    }
    closedir(d);
    if (nvmc > 1) qsort(vmc_names, (size_t)nvmc, sizeof vmc_names[0], vmc_name_cmp);
    if (total > nvmc) logf_("vmc: %d cards in %s, showing %d\n", total, dir, nvmc);
    return nvmc;
}

static int vmc_scan(void) { return vmc_scan_dir(VMC_DIR, "VM1"); }
static int lib_scan(void) { return vmc_scan_dir(LIB_DIR, "MCD"); }

static int vmc_valid(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char magic[2];
    struct stat st;
    if (!f) return 0;
    if (stat(path, &st) != 0 || st.st_size != VMC_SIZE) { fclose(f); return 0; }
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 'M' || magic[1] != 'C') { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    unsigned char buf[8192];
    size_t n;
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return 0; }
    if (ferror(in)) { fclose(in); fclose(out); return 0; }
    fclose(in);
    return fclose(out) == 0;
}

static void vmc_message(const char *page, const char *title, const char *a, const char *b)
{
    while (!app_exit_requested) {
        uint32_t p;
        frame_begin(0);
        p = input_pressed();
        header(page, NULL);
        text_fit(MARGIN, 110, title, C_ACCENT);
        if (a) text_fit(MARGIN, 170, a, C_TEXT);
        if (b) text_fit(MARGIN, 210, b, C_DIM);
        footer("X OK");
        frame_end();
        if (p & (PAD_CROSS | PAD_CIRCLE)) return;
    }
}

static void vmc_import_page(void)
{
    int sel = 0, top = 0, rows = 17, i;
    char src[1300], dst[1300], bak[1400];
    logf_("menu: import PS3 VMC\n");
    if (vmc_scan() == 0) {
        vmc_message("Import PS3 VMC", "No PS3 VMC found", "Create a card in XMB Memory Card Utility first,", "then come back here (savedata/vmc/*.VM1).");
        return;
    }
    while (!app_exit_requested) {   /* picker: sorted, scrolls, L1/R1 page */
        uint32_t p;
        char sub[64];
        frame_begin(0);
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + nvmc - 1) % nvmc;
        if (p & PAD_DOWN) sel = (sel + 1) % nvmc;
        if (p & PAD_L1) sel = sel > rows ? sel - rows : 0;
        if (p & PAD_R1) sel = sel + rows < nvmc ? sel + rows : nvmc - 1;
        if (p & PAD_CIRCLE) return;
        if (p & PAD_CROSS) break;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        snprintf(sub, sizeof sub, "%d cards", nvmc);
        header("Import PS3 VMC", sub);
        for (i = top; i < nvmc && i < top + rows; i++) {
            int y = 90 + (i - top) * SET_ROW;
            if (i == sel) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
            text_fit(MARGIN, y, vmc_names[i], i == sel ? C_ACCENT : C_TEXT);
        }
        footer("Up/Down select    L1 R1 page    X import    Circle back");
        frame_end();
    }
    if (app_exit_requested) return;
    if (app_exit_requested) return;
    /* Library name: the VM1 base name with .mcd instead of .VM1. */
    {
        char libname[256], bakmsg[300];
        size_t ln = strlen(vmc_names[sel]);
        if (ln >= 4 && !strcasecmp(vmc_names[sel] + ln - 4, ".VM1")) {
            if (ln - 4 == 0) {
                vmc_message("Import PS3 VMC", "Import failed", "That file has no card name.", "Nothing was changed.");
                return;
            }
            snprintf(libname, sizeof libname, "%.*s.mcd", (int)(ln - 4), vmc_names[sel]);
        } else
            snprintf(libname, sizeof libname, "%s.mcd", vmc_names[sel]);
        /* The core's card path is 256 bytes: a library name that does not
           fit in saves/imported/ would be read back truncated, so refuse
           it here rather than importing a card the core cannot address. */
        if (strlen(LIB_DIR "/") + strlen(libname) + 1 > 256) {
            vmc_message("Import PS3 VMC", "Import failed", "That card name is too long for the core's path limit.", "Rename it in XMB first, then import again.");
            return;
        }
        snprintf(src, sizeof src, "%s/%s", VMC_DIR, vmc_names[sel]);
        snprintf(dst, sizeof dst, "%s/%s", LIB_DIR, libname);
        snprintf(bakmsg, sizeof bakmsg, "Existing library copy, if any, is backed up as %s.bak.", libname);
        while (!app_exit_requested) {   /* confirm */
            uint32_t q;
            char msg[300];
            frame_begin(0);
            q = input_pressed();
            header("Import PS3 VMC", NULL);
            snprintf(msg, sizeof msg, "Copy %s to the imported library?", vmc_names[sel]);
            text_fit(MARGIN, 110, msg, C_ACCENT);
            text_fit(MARGIN, 170, bakmsg, C_TEXT);
            ovl_text(MARGIN, 210, "Import does not change the active card or mode.", C_TEXT);
            ovl_text(MARGIN, 250, "Pick it in Memory card 1 or 2 to play with it.", C_DIM);
            footer("X import    Circle back");
            frame_end();
            if (q & PAD_CIRCLE) return;
            if (q & PAD_CROSS) break;
        }
        if (app_exit_requested) return;
        if (!vmc_valid(src)) {
            logf_("vmc: import refused, %s is not a 128 KB MC card\n", src);
            vmc_message("Import PS3 VMC", "Import failed", "That file is not a 128 KB PS1 memory card.", "Nothing was changed.");
            return;
        }
        mkdir(LIB_DIR, 0777);
        snprintf(bak, sizeof bak, "%s.bak", dst);
        {   struct stat st;
            if (stat(dst, &st) == 0 && !copy_file(dst, bak)) {
                logf_("vmc: import aborted, could not back up %s\n", dst);
                vmc_message("Import PS3 VMC", "Import failed", "Could not back up the existing library copy.", "Nothing was changed.");
                return;
            }
            if (stat(dst, &st) == 0) logf_("vmc: backed up %s to %s\n", dst, bak);
        }
        if (!copy_file(src, dst) || !vmc_valid(dst)) {
            struct stat st;
            if (stat(bak, &st) == 0) copy_file(bak, dst);
            else remove(dst);
            logf_("vmc: import failed copying %s\n", src);
            vmc_message("Import PS3 VMC", "Import failed", "Copy failed; the old library copy was restored.", NULL);
            return;
        }
        snprintf(cfg.vmc_source, sizeof cfg.vmc_source, "%s", vmc_names[sel]);
        config_save();
        logf_("vmc: imported %s to %s (library; active card and mode unchanged)\n", src, dst);
        vmc_message("Import PS3 VMC", "Import succeeded", vmc_names[sel], "It is now in the library. Choose it in Memory card 1 or 2 to play with it.");
    }
}

static void vmc_slot_picker(int slot)
{
    /* Entries: 0 = Auto, 1..n = imported files, n+1 = None, and n+2 =
       the legacy default shared card, only when such a file exists and
       is a valid card (so pre-library shared cards stay reachable). */
    int *mode = slot == 1 ? &cfg.mc1_mode : &cfg.mc2_mode;
    char *shared = slot == 1 ? cfg.mc1_shared : cfg.mc2_shared;
    const char *other_shared = slot == 1 ? cfg.mc2_shared : cfg.mc1_shared;
    int other_mode = slot == 1 ? cfg.mc2_mode : cfg.mc1_mode;
    int sel = 0, top = 0, rows = 17, i, n, total, has_default;
    char title[32], defpath[1300], deftext[64], path[1300];
    snprintf(title, sizeof title, "Memory card %d", slot);
    snprintf(defpath, sizeof defpath, APP_DIR "/saves/pcsx-card%d.mcd", slot);
    snprintf(deftext, sizeof deftext, "Default shared card (pcsx-card%d.mcd)", slot);
    logf_("menu: memory card %d picker\n", slot);
    n = lib_scan();
    has_default = vmc_valid(defpath);
    total = n + 2 + (has_default ? 1 : 0);
    /* Start on the current choice (a dangling file choice, whose file is
       gone from the library, starts on None; Circle leaves it unchanged). */
    if (*mode == 0) sel = 0;
    else if (*mode == 2) sel = n + 1;
    else if (*mode == 1 && shared[0]) {
        sel = n + 1;
        for (i = 0; i < n; i++) if (!strcmp(vmc_names[i], shared)) sel = i + 1;
    } else if (*mode == 1) sel = has_default ? n + 2 : n + 1;
    while (!app_exit_requested) {
        uint32_t p;
        char sub[64];
        frame_begin(0);
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + total - 1) % total;
        if (p & PAD_DOWN) sel = (sel + 1) % total;
        if (p & PAD_L1) sel = sel > rows ? sel - rows : 0;
        if (p & PAD_R1) sel = sel + rows < total ? sel + rows : total - 1;
        if (p & PAD_CIRCLE) return;
        if (p & PAD_CROSS) break;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        snprintf(sub, sizeof sub, "%d imported cards", n);
        header(title, sub);
        for (i = top; i < total && i < top + rows; i++) {
            int y = 90 + (i - top) * SET_ROW;
            const char *row = NULL;
            uint32_t col = C_TEXT;
            if (i == sel) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
            if (i == 0) row = "Auto (per-game)";
            else if (i <= n) {
                row = vmc_names[i - 1];
                if (other_mode == 1 && other_shared[0] && !strcmp(row, other_shared)) col = C_DIM;
            }
            else if (i == n + 1) row = "None (no card)";
            else row = deftext;
            text_fit(MARGIN, y, row, i == sel ? C_ACCENT : col);
        }
        footer("Up/Down select    L1 R1 page    X choose    Circle back");
        frame_end();
    }
    if (app_exit_requested) return;
    if (sel == 0) {
        *mode = 0; shared[0] = 0;
        config_save();
        logf_("vmc: memory card %d set to Auto\n", slot);
        vmc_message(title, "Memory card set to Auto", "This game's own serial card.", "Next launch only.");
        return;
    }
    if (sel == n + 1) {
        *mode = 2; shared[0] = 0;
        config_save();
        logf_("vmc: memory card %d set to None\n", slot);
        vmc_message(title, "Memory card set to None", "No card in this slot.", "Next launch only.");
        return;
    }
    if (has_default && sel == n + 2) {
        *mode = 1; shared[0] = 0;
        config_save();
        logf_("vmc: memory card %d set to default %s\n", slot, defpath);
        vmc_message(title, "Memory card set to Default", deftext, "Next launch only.");
        return;
    }
    /* An imported file: refuse it when the other slot already uses it. */
    if (other_mode == 1 && other_shared[0] && !strcmp(vmc_names[sel - 1], other_shared)) {
        char msg[300];
        snprintf(msg, sizeof msg, "That card is already used by slot %d.", slot == 1 ? 2 : 1);
        vmc_message(title, "Cannot pick that card", msg, "Pick a different card.");
        return;
    }
    snprintf(path, sizeof path, "%s/%s", LIB_DIR, vmc_names[sel - 1]);
    if (!vmc_valid(path)) {
        logf_("vmc: slot %d choice refused, %s is not a 128 KB MC card\n", slot, path);
        vmc_message(title, "Cannot use that card", "That file is not a 128 KB PS1 memory card.", "Nothing was changed.");
        return;
    }
    *mode = 1;
    snprintf(shared, 256, "%s", vmc_names[sel - 1]);
    config_save();
    logf_("vmc: memory card %d set to %s\n", slot, path);
    vmc_message(title, "Memory card set", vmc_names[sel - 1], "Next launch only.");
}

static void draw_settings(int sel, int y0, int in_game, int highlight, const char *help)
{
    /* sel is a GLOBAL settings_items index; the page it sits on is derived,
       so the caller keeps passing the same sel it already tracks. highlight
       is 0 while the selection sits on an apply row below the page (the
       page row the user came from must not look selected as well). help
       overrides the help line when non-NULL: the apply rows' explanations,
       shown only while the cursor sits on them, like the other rows' help. */
    int pg = set_page_of(sel), rows = set_page_counts[pg], i;
    {   /* page navigation row, above the items: L1/R1 flank the page title.
           Text only: the overlay font is ASCII 32-126, so no triangle glyphs
           (the chat mockup's arrows do not exist on the PS3). One text row
           tall, inset to the header's margins (MARGIN) so CRT overscan,
           which eats the screen edges, does not crop it. */
        const char *t = set_page_titles[pg];
        int y = y0 - 34;
        ovl_text(MARGIN, y, "< L1", C_ACCENT);
        ovl_text((OVL_W - ovl_text_width(t)) / 2, y, t, C_ACCENT);
        ovl_text(OVL_W - MARGIN - ovl_text_width("R1 >"), y, "R1 >", C_ACCENT);
    }
    for (i = 0; i < rows; i++) {
        int g = set_page_items[pg][i];
        char v[64];
        int y = y0 + i * SET_ROW;
        if (g == sel && highlight) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
        ovl_text(50, y, settings_items[g].label, g == sel && highlight ? C_ACCENT : C_TEXT);
        setting_value(g, v, sizeof v);
        ovl_text(520, y, "<", C_DIM);
        ovl_text(550, y, v, C_TEXT);
        ovl_text(560 + ovl_text_width(v), y, ">", C_DIM);
    }
    /* No scrolling anymore: the longest page has 9 rows, SET_VISIBLE is 17,
       so the old scroll triangles are gone (they would have lied: on a page
       the row below the last one is not another setting). */
    ovl_text(50, y0 + rows * SET_ROW + 12, help ? help : settings_items[sel].help, C_DIM);
    if (sel == 0 && highlight) ovl_text(50, y0 + rows * SET_ROW + 40, in_game ? "(engine switches live on resume)"
                                                              : "Interpreter if a game misbehaves.", C_DIM);
}

extern void game_settings_changed(void);   /* main.c: re-apply cfg to the running game */

/* ---------------- shared settings panel ----------------
 * One panel for both entry points: Triangle on the home game list and
 * Settings in the pause quick menu. title_key is the ROM file name without
 * extension for a scanned game row / the running game, or NULL for the
 * disc entry, Boot BIOS, an empty list, or the in-game BIOS (global only).
 * in_game keeps its old meaning: in a game, every change applies live
 * (game_settings_changed) and the engine switch runs on close; at home,
 * changes only land in the files when applied or on back.
 * Load rule: if the title has its own configs/<key>.ini, its per-game keys
 * overlay cfg on entry; otherwise cfg stays as the global values. The rows
 * and pages are identical either way. The header names the title when one
 * is selected ("Settings: <key>", display-truncated), else "Settings for
 * all games".
 * Apply rows (panel-level, not per-page, fixed below the page rows):
 * "Apply to this title only" over "Apply to global setting". Down from a
 * page's last row reaches the first apply row, Down again the second; Up
 * reverses; the column wraps (first page row <-> last apply row); L1/R1
 * flips return to the page's remembered row. X applies and the panel stays
 * open with a confirmation. With no title the title button is hidden.
 * Back (Circle, and Triangle at home) AUTO-SAVES and closes, never
 * discards: the target is picked at back time from whether the title's
 * .ini exists then (title-apply split if it does, global-apply if not).
 * A back-save never creates a .ini that did not exist, and never deletes
 * one. After a home back-save cfg is the global baseline again and the
 * list's own-settings tags refresh; after an in-game back-save cfg stays
 * as the saved effective values (no revert: in-game edits were live).
 * CPU switch (in game only): if cfg.dynarec at close differs from the
 * entry value, the live-switch-on-resume logic runs as before, with the
 * message returned through close_msg for the quick menu to show. */
static void settings_panel(const char *title_key, int in_game, char *close_msg, size_t close_msg_n)
{
    char panel_key[256];
    const char *old_name = menu_name;
    const char *panel_msg = NULL;
    settings_t baseline;
    int has_title = title_key && title_key[0];
    int has_personal, sel = 0, apply = -1, i, cpu_entry;
    if (has_title) snprintf(panel_key, sizeof panel_key, "%s", title_key);
    else panel_key[0] = 0;
    logf_("menu: settings%s%s\n", has_title ? ": " : "", panel_key);
    menu_name = "settings";
    for (i = 0; i < NPAGES; i++) set_page_row[i] = 0;   /* fresh panel: page 0, row 0 */
    /* baseline = the settings for all games at entry (home: entry cfg, which
       is global; in game: the launch snapshot, not the effective values). */
    if (in_game) config_panel_global(&baseline);
    else baseline = cfg;
    has_personal = has_title ? config_panel_overlay(panel_key, &cfg) : 0;
    if (has_personal) logf_("settings: this title's own settings from configs/%s.ini\n", panel_key);
    cpu_entry = cfg.dynarec;   /* in game: the effective engine at entry, as before */
    while (!app_exit_requested) {
        uint32_t p;
        char pg_ind[16];
        int applied = 0, n_apply = has_title ? 2 : 1;
        frame_begin(0);
        p = input_pressed();
        if (apply >= 0) {
            /* On an apply row: sel still holds the page row we came from,
               so L1/R1 flips from that page, and Up/Down walk the column.
               Up from the first apply row goes to the LAST page row
               explicitly (mirroring Down's wrap from the last apply row
               to the first page row): returning to sel here would strand
               Up on the first row after a Down wrap reset sel to it. */
            if (p & PAD_UP) { if (apply == 0) { int pg = set_page_of(sel);
                                  apply = -1; sel = set_page_items[pg][set_page_counts[pg] - 1]; set_page_row[pg] = set_page_counts[pg] - 1; }
                              else apply--; }
            if (p & PAD_DOWN) { if (apply == n_apply - 1) { int pg = set_page_of(sel);
                                  apply = -1; sel = set_page_items[pg][0]; set_page_row[pg] = 0; }
                                else apply++; }
            if (p & (PAD_L1 | PAD_R1)) { int pg = set_page_of(sel);
                                         apply = -1; set_page_row[pg] = set_row_of(sel);
                                         pg = (pg + (p & PAD_L1 ? NPAGES - 1 : 1)) % NPAGES;
                                         sel = set_page_items[pg][set_page_row[pg]]; }
            if (p & PAD_CROSS) {
                if (has_title && apply == 0) {
                    if (config_panel_apply_title(panel_key, &cfg, &baseline, in_game)) {
                        has_personal = 1;
                        panel_msg = "Saved to this title only";
                    } else panel_msg = "Save failed";
                } else {
                    panel_msg = config_panel_apply_global(&cfg, &baseline, in_game)
                              ? "Saved to global settings" : "Save failed";
                }
                applied = 1;
            }
            /* Left/Right do nothing on apply rows. */
        } else {
            /* Up/Down move within the current page, except at the edges,
               where the column continues onto the apply rows (and wraps). */
            if (p & PAD_UP) { int pg = set_page_of(sel), rw = set_row_of(sel);
                              if (rw == 0) apply = n_apply - 1;
                              else { rw--; sel = set_page_items[pg][rw]; set_page_row[pg] = rw; } }
            if (p & PAD_DOWN) { int pg = set_page_of(sel), rw = set_row_of(sel);
                                if (rw == set_page_counts[pg] - 1) apply = 0;
                                else { rw++; sel = set_page_items[pg][rw]; set_page_row[pg] = rw; } }
            if (p & (PAD_L1 | PAD_R1)) { int pg = set_page_of(sel);
                                         set_page_row[pg] = set_row_of(sel);
                                         pg = (pg + (p & PAD_L1 ? NPAGES - 1 : 1)) % NPAGES;
                                         sel = set_page_items[pg][set_page_row[pg]]; }
            if (p & (PAD_LEFT | PAD_RIGHT | PAD_CROSS)) {
                setting_change(sel, p & PAD_LEFT ? -1 : 1, in_game);
                if (in_game) game_settings_changed();
            }
        }
        if (p & (PAD_CIRCLE | PAD_TRIANGLE)) {
            /* Back auto-saves (never discards). Target by the file's
               existence NOW, not at entry: a title-apply during this panel
               may have created the file since. */
            int ok = (has_title && config_game_exists(panel_key))
                   ? config_panel_apply_title(panel_key, &cfg, &baseline, in_game)
                   : config_panel_apply_global(&cfg, &baseline, in_game);
            if (!ok) logf_("menu: settings back-save failed\n");
            if (!in_game) {
                cfg = baseline;   /* the list operates on the settings for all games */
                check_own_settings();   /* a newly created .ini shows its tag at once */
                logf_("menu: back to game list\n");
            } else if (cfg.dynarec != cpu_entry) {
                /* Live engine switch on resume, as before: the next
                   retro_run re-reads pcsx_rearmed_drc (option_value answers
                   from cfg.dynarec) and upstream update_variables swaps the
                   engine (Notify BEFORE_SAVE -> Shutdown -> Init -> Notify
                   AFTER_LOAD). Only the Interpreter -> Dynarec direction
                   syncs Lightrec's thread-mode global first: set_threaded
                   only touches the global when not on Dynarec (no restart,
                   no live state), so the fresh Lightrec starts in the picked
                   thread mode. From Dynarec, do NOT call it: it would
                   restart Lightrec without a register sync. Thread-mode-only
                   changes stay next-launch. */
                if (!cpu_entry && cfg.dynarec) {
                    extern void lightrec_plugin_set_threaded(int on) __attribute__((weak));
                    if (lightrec_plugin_set_threaded) lightrec_plugin_set_threaded(cfg.dynarec_thread);
                }
                if (close_msg && close_msg_n > 0) snprintf(close_msg, close_msg_n, "CPU switches live on resume");
            } else if (close_msg && close_msg_n > 0) close_msg[0] = 0;
            menu_name = old_name;
            return;
        }
        if (!applied && p) panel_msg = NULL;   /* a fresh input clears a stale confirmation */
        if (in_game) ovl_fill(0, 0, OVL_W, OVL_H, C_PANEL);   /* the old inline page filled the screen */
        snprintf(pg_ind, sizeof pg_ind, "%d / %d", set_page_of(sel) + 1, NPAGES);
        if (has_title) {
            char hdr[300];
            snprintf(hdr, sizeof hdr, "Settings: %.50s", panel_key);
            header(hdr, pg_ind);
        } else header("Settings for all games", pg_ind);
        {   /* The apply rows' explanations live on the help line, shown only
               while the cursor sits on them (like the page rows' help),
               not as permanent rows under the buttons. */
            const char *apply_help = NULL;
            if (apply >= 0)
                apply_help = (has_title && apply == 0) ? "Saves these values to this title only."
                                                      : "Overwrites global settings. This title's own settings are kept.";
            draw_settings(sel, 118, in_game, apply < 0, apply_help);
        }
        {
            /* Apply section, one 27 px row pitch like the page rows: status
               (title only), stacked buttons, then the back behaviour. The
               buttons' explanations are NOT permanent rows here: they show
               on the help line while the cursor sits on the button (see
               draw_settings). All lines fit the panel width
               (OVL_W - 2*MARGIN at FONT_W 16 = 72 chars). */
            int y = 440, k = 0;
            if (has_title) {
                ovl_text(50, y, has_personal ? "This title has its own settings."
                                             : "This title has no personal settings yet.", C_DIM);
                y += SET_ROW;
                if (apply == 0) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
                ovl_text(50, y, "Apply to this title only", apply == 0 ? C_ACCENT : C_TEXT);
                y += SET_ROW; k = 1;
            }
            if (apply == k) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
            ovl_text(50, y, "Apply to global setting", apply == k ? C_ACCENT : C_TEXT);
            y += SET_ROW;
            ovl_text(50, y, "Back saves to this title if it has its own settings, else global.", C_DIM);
            y += SET_ROW;
            if (panel_msg) ovl_text(50, y, panel_msg, C_ACCENT);
        }
        if (!in_game) {
            char b[200];
            snprintf(b, sizeof b, "BIOS: %s", hooks_bios_status());
            ovl_text(50, OVL_H - 82, b, C_DIM);
        }
        footer("Up/Down select    Left/Right change    X apply    Circle back");
        frame_end();
    }
    menu_name = old_name;
}

/* ---------------- game list ---------------- */
int disc_probe(char *label, size_t n);     /* disc.c */
void disc_identify_start(void);
int  disc_identify_result(char *title, size_t n);
const char *disc_identify_serial(void);   /* disc.c: serial from SYSTEM.CNF */
void disc_identify_wait(void);
static char disc_msg[64], disc_msg2[64];   /* one or two lines over the game list */
static int disc_msg_frames;
extern int disc_needs_cfw;               /* disc.c */

/* the PS1 disc in the console's drive: the volume name names its save states and
   own settings; NULL (and a message) if it cannot be read */
/* a black PS1 disc, 28 pixels across, at x, y (top left); bg: the row's colour */
static void disc_icon(int x, int y, uint32_t bg)
{
    int i, j;
    for (j = 0; j < 29; j++)
        for (i = 0; i < 29; i++) {
            int dx = i - 14, dy = j - 14, d = dx * dx + dy * dy;
            uint32_t c;
            if (d > 14 * 14) continue;
            c = d > 13 * 13 ? 0xff808080        /* rim */
              : d > 6 * 6 ? (dx + dy < -6 && d > 9 * 9 ? 0xff303038 : 0xff0c0c10)   /* black, a faint shine */
              : d > 3 * 3 ? 0xff5a5a5a          /* hub */
              : bg;                             /* hole */
            ovl_fill(x + i, y + j, 1, 1, c);
        }
}

static const char *start_disc(void)
{
    static char disc_path[64];
    char label[40];
    int k;
    frame_begin(0);                     /* the drive may take a few seconds to spin up */
    header_app(NULL);
    ovl_text(MARGIN, 110, "Reading the disc...", C_TEXT);
    frame_end();
    disc_identify_wait();               /* the game list's check must not read at the same time */
    if (disc_probe(label, sizeof label)) {
        for (k = 0; label[k]; k++)
            if (label[k] == '/' || label[k] == '.' || label[k] == ':' || label[k] == '\\') label[k] = '_';
        snprintf(disc_path, sizeof disc_path, "cdrom://%s", label);
        logf_("menu: starting the disc (%s)\n", disc_path);
        return disc_path;
    }
    if (disc_needs_cfw) {
        snprintf(disc_msg, sizeof disc_msg, "Playing from the disc needs custom firmware (CFW).");
        snprintf(disc_msg2, sizeof disc_msg2, "On HEN, copy the disc to an image on a PC.");
        disc_msg_frames = 360;
    } else {
        snprintf(disc_msg, sizeof disc_msg, "Disc read failed (with Debug log on, log.txt says why).");
        disc_msg2[0] = 0;
        disc_msg_frames = 240;
    }
    return NULL;
}

/* ---- copying the disc in the drive to the HDD (disc.c) ---- */
struct disc_copy;
struct disc_copy *disc_copy_begin(const char *dir, const char *name, char *err, size_t errn);
int disc_copy_step(struct disc_copy *c, unsigned ms, uint32_t *done, uint32_t *total);
const char *disc_copy_error(struct disc_copy *c);
const char *disc_copy_cue(struct disc_copy *c);
void disc_copy_end(struct disc_copy *c);
#define COPY_DIR "/dev_hdd0/PSXISO"     /* one of the folders the game list scans */

/* a message until X or O is pressed */
static void copy_message(const char *title, const char *a, const char *b, const char *c)
{
    while (!app_exit_requested) {
        uint32_t p;
        frame_begin(0);
        p = input_pressed();
        header("Copy disc to HDD", NULL);
        text_fit(MARGIN, 110, title, C_ACCENT);
        if (a) text_fit(MARGIN, 170, a, C_TEXT);
        if (b) text_fit(MARGIN, 210, b, C_TEXT);
        if (c) text_fit(MARGIN, 250, c, C_DIM);
        footer("X OK");
        frame_end();
        if (p & (PAD_CROSS | PAD_CIRCLE)) return;
    }
}

/* the disc's game name as a file name: no characters the file systems refuse */
static void copy_file_name(const char *title, char *out, size_t n)
{
    size_t j = 0;
    for (; *title && j + 1 < n; title++) {
        char ch = *title;
        if ((unsigned char)ch < 32 || strchr("\\/*?\"<>|", ch)) continue;
        if (ch == ':') ch = '-';
        if (ch == ' ' && (j == 0 || out[j - 1] == ' ')) continue;
        out[j++] = ch;
    }
    while (j && (out[j - 1] == ' ' || out[j - 1] == '.')) j--;
    out[j] = 0;
    if (!j) snprintf(out, n, "PS1 disc");
}

/* asks, then copies with a progress bar; 1 and cue = the new .cue if it was copied */
static int copy_disc(const char *title, char *cue, size_t cue_n)
{
    char name[128], dest[256], err[640], line[128];
    struct disc_copy *c;
    uint32_t done = 0, total = 0;
    uint64_t t0, cancel_at = 0;
    int r = 1;
    copy_file_name(title, name, sizeof name);
    snprintf(dest, sizeof dest, COPY_DIR "/%s.cue", name);
    for (;;) {                          /* confirm */
        uint32_t p;
        if (app_exit_requested) return 0;
        frame_begin(0);
        p = input_pressed();
        header("Copy disc to HDD", NULL);
        text_fit(MARGIN, 110, title, C_ACCENT);
        ovl_text(MARGIN, 170, "Copies the whole disc to the HDD as", C_TEXT);
        text_fit(MARGIN + 32, 210, dest, C_TEXT);
        ovl_text(MARGIN, 270, "The copy is in the game list afterwards and loads", C_DIM);
        ovl_text(MARGIN, 306, "from the HDD: no pauses while the drive seeks.", C_DIM);
        ovl_text(MARGIN, 342, "Takes a few minutes and up to 700 MB of space.", C_DIM);
        footer("X copy    Circle back");
        frame_end();
        if (p & PAD_CIRCLE) return 0;
        if (p & PAD_CROSS) break;
    }
    frame_begin(0);                     /* the drive may take a few seconds to spin up */
    header("Copy disc to HDD", NULL);
    text_fit(MARGIN, 110, title, C_ACCENT);
    ovl_text(MARGIN, 170, "Reading the disc...", C_TEXT);
    frame_end();
    if (!(c = disc_copy_begin(COPY_DIR, name, err, sizeof err))) {
        if (disc_needs_cfw)
            copy_message(title, "Copying the disc needs custom firmware (CFW).",
                         "On HEN, copy the disc to an image on a PC.", NULL);
        else
            copy_message(title, "The copy could not start:", err, NULL);
        return 0;
    }
    t0 = sysGetSystemTime();
    while (r == 1) {
        uint32_t p;
        uint64_t now;
        double secs, mbs;
        if (app_exit_requested) break;
        frame_begin(0);
        p = input_pressed();
        r = disc_copy_step(c, 150, &done, &total);   /* most of the time reading, the screen about 6 times a second */
        now = sysGetSystemTime();
        if (p & PAD_CIRCLE) {
            if (cancel_at && now - cancel_at < 3000000) break;
            cancel_at = now;
        }
        header("Copy disc to HDD", NULL);
        text_fit(MARGIN, 110, title, C_ACCENT);
        text_fit(MARGIN, 170, dest, C_DIM);
        {   /* the bar */
            int w = OVL_W - 2 * MARGIN, f = total ? (int)((uint64_t)w * done / total) : 0;
            ovl_fill(MARGIN, 240, w, 36, C_SEL);
            ovl_fill(MARGIN, 240, f, 36, C_ACCENT);
        }
        secs = (now - t0) / 1e6;
        mbs = secs > 0.5 ? done * 2352.0 / 1048576.0 / secs : 0.0;
        if (mbs > 0.01 && total > done) {
            int left = (int)((total - done) * 2352.0 / 1048576.0 / mbs);
            snprintf(line, sizeof line, "%u of %u MB   %.1f MB/s   about %d:%02d left",
                     (unsigned)(done * 2352ull >> 20), (unsigned)(total * 2352ull >> 20), mbs, left / 60, left % 60);
        } else
            snprintf(line, sizeof line, "%u of %u MB", (unsigned)(done * 2352ull >> 20), (unsigned)(total * 2352ull >> 20));
        ovl_text(MARGIN, 300, line, C_TEXT);
        footer(cancel_at && now - cancel_at < 3000000 ? "Press Circle again to stop the copy" : "Circle stop");
        frame_end();
    }
    if (r == 0) {
        snprintf(cue, cue_n, "%s", disc_copy_cue(c));
        disc_copy_end(c);
        copy_message(title, "Copied to the HDD:", dest, "It is in the game list now.");
        return 1;
    }
    if (r < 0) snprintf(err, sizeof err, "%s", disc_copy_error(c));
    disc_copy_end(c);                   /* (removes the unfinished file) */
    if (r < 0) copy_message(title, "The copy failed:", err, "The unfinished file was removed.");
    else if (!app_exit_requested) copy_message(title, "Copy stopped.", "The unfinished file was removed.", NULL);
    return 0;
}

/* ---- game list cover panel (wide list + square cover, agreed proto) ----
   List right edge 884, panel 920..1240, cover 290x290 at (935, 110).
   No bundled image asset: the cover is the user's webMAN pack file at run
   time, the placeholder is just panel + text. Only the selected game's
   cover is kept (cover_key); it is reloaded when the selection changes
   and freed when the list is left, so the menu never holds a whole list
   of images on an old machine. When there are no game files the panel is
   skipped, so the empty-list help stays exactly as before. */
#define LIST_TAG_R 884
#define PANEL_X 920
#define PANEL_W 320
#define COVER_X 935
/* Spine case (picked style): case 292x276 at (COVER_X,110) — spine 24 +
   gap 2 + art 264 + right frame 2 wide; art 6px below the case top and
   6px above its bottom. The art shrinks 290 -> 264 (near 1:1 for 268px
   pack covers). */
#define CASE_W 292
#define CASE_H 276
#define SPINE_W 24
#define CASE_ART 264
/* Disc placeholder (picked: tray + shadow + NO COVER inside the disc):
   tray 246x246 centred in the art rect, disc r 107 centred (1093,248),
   hub r 15 + hole r 5, label below the hub. */
#define TRAY_S 246
#define TRAY_X (COVER_X + SPINE_W + 2 + (CASE_ART - TRAY_S) / 2)
#define TRAY_Y (110 + 6 + (CASE_ART - TRAY_S) / 2)
#define DISC_R 107
#define DISC_CX (COVER_X + SPINE_W + 2 + CASE_ART / 2)
#define DISC_CY (110 + 6 + CASE_ART / 2)

static char cover_key[1300];
static cover_img_t cover_img;

static void cover_release(void)
{
    cover_free(&cover_img);
    cover_key[0] = 0;
}

static void cover_update(const char *key, const char *game_path, int is_disc, const char *disc_serial)
{
    if (!strcmp(key, cover_key)) return;
    cover_free(&cover_img);
    snprintf(cover_key, sizeof cover_key, "%s", key);
    if (cover_load(game_path, is_disc, disc_serial, &cover_img))
        logf_("menu: cover found for %s (%dx%d)\n", key, cover_img.w, cover_img.h);
    else
        logf_("menu: no cover for %s\n", key);
}

/* text_fit, but ending at a right edge other than the TV margin (the list
   now ends at 884, the panel's text at 1225) */
static void text_fit_r(int x, int y, const char *s, uint32_t argb, int right)
{
    char b[256];
    int max = (right - x) / ovl_text_width("W");
    if ((int)strlen(s) <= max || max < 4) { ovl_text(x, y, s, argb); return; }
    snprintf(b, sizeof b, "%.*s...", max - 3, s);
    ovl_text(x, y, b, argb);
}

/* Diagonal shine band across a rect (the clear-plastic reflection over the
   cover art): 3 strips per row for soft edges, clipped to the rect.
   Slope 9/20 (~24 deg, like the picked mockup); centre strip 30% white,
   side strips 15%. Art only, not the spine (as mocked). */
static void cover_shine(int x, int y, int w, int h)
{
    int j;
    if (w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        /* long intermediates: int math drew -Wstrict-overflow on the
           clamp-then-compare below; for int inputs every value here is
           bounded by ~4e9, far inside long range, so no wrap is possible
           and the warning is benign (GCC 7 heuristic on this pattern). */
        long cx = (long)x + w / 3 + (long)j * 9 / 20;
        long lo, hi;
        lo = cx - 28; if (lo < x) lo = x;
        hi = cx - 14; if (hi > (long)x + w) hi = (long)x + w;
        if (hi > lo) ovl_fill((int)lo, (int)((long)y + j), (int)(hi - lo), 1, 0x26ffffffu);
        lo = cx - 14; if (lo < x) lo = x;
        hi = cx + 14; if (hi > (long)x + w) hi = (long)x + w;
        if (hi > lo) ovl_fill((int)lo, (int)((long)y + j), (int)(hi - lo), 1, 0x4dffffffu);
        lo = cx + 14; if (lo < x) lo = x;
        hi = cx + 28; if (hi > (long)x + w) hi = (long)x + w;
        if (hi > lo) ovl_fill((int)lo, (int)((long)y + j), (int)(hi - lo), 1, 0x26ffffffu);
    }
}

/* Integer square root (Newton, no libm — same method as ovl_round_fill's
   helper in draw.c, duplicated here so the disc code stays menu.c-local). */
static int cover_isqrt(int v)
{
    int r, t;
    if (v <= 0) return 0;
    r = v;
    while ((t = (r + v / r) / 2) < r) r = t;
    return r;
}

/* Filled disc via one span per row (ovl_circle is outline-only). */
static void cover_disc_fill(int cx, int cy, int r, uint32_t argb)
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        int dx = cover_isqrt(r * r - dy * dy);
        ovl_fill(cx - dx, cy + dy, 2 * dx + 1, 1, argb);
    }
}

/* The plastic shine band clipped to a disc: same 3-strip band as
   cover_shine (same slope, same art-rect band), intersected per row
   with the disc span. Same benign GCC 7 -Wstrict-overflow note as
   cover_shine (clamp-then-compare pattern; values bounded, long math). */
static void cover_disc_shine(int cx, int cy, int r, int bx, int by, int bw)
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        int dx = cover_isqrt(r * r - dy * dy);
        int dlo = cx - dx, dhi = cx + dx;
        long bc = (long)bx + bw / 3 + (long)(y - by) * 9 / 20;
        long lo, hi;
        lo = bc - 28; if (lo < dlo) lo = dlo;
        hi = bc - 14; if (hi > dhi) hi = dhi;
        if (hi > lo) ovl_fill((int)lo, y, (int)(hi - lo), 1, 0x26ffffffu);
        lo = bc - 14; if (lo < dlo) lo = dlo;
        hi = bc + 14; if (hi > dhi) hi = dhi;
        if (hi > lo) ovl_fill((int)lo, y, (int)(hi - lo), 1, 0x4dffffffu);
        lo = bc + 14; if (lo < dlo) lo = dlo;
        hi = bc + 28; if (hi > dhi) hi = dhi;
        if (hi > lo) ovl_fill((int)lo, y, (int)(hi - lo), 1, 0x26ffffffu);
    }
}

static void draw_cover_panel(const char *title, const char *line2)
{
    ovl_round_fill(PANEL_X, 90, PANEL_W, 530, PANEL_R, C_PANEL);
    if (cover_img.px) {
        /* Spine case: drop shadow, black case base, spine grooves, art
           inset right of the spine, plastic shine over the art. */
        ovl_fill(COVER_X + 4, 110 + 6, CASE_W, CASE_H, 0x73000000u);
        ovl_fill(COVER_X, 110, CASE_W, CASE_H, 0xff000000u);
        ovl_fill(COVER_X + 6, 110, 2, CASE_H, 0xff222222u);
        ovl_fill(COVER_X + 16, 110, 2, CASE_H, 0xff222222u);
        ovl_blit(COVER_X + SPINE_W + 2, 110 + 6, CASE_ART, CASE_ART,
                 cover_img.px, cover_img.w, cover_img.h);
        cover_shine(COVER_X + SPINE_W + 2, 110 + 6, CASE_ART, CASE_ART);
        text_fit_r(COVER_X, 420, title, C_ACCENT, PANEL_X + PANEL_W - 15);
        if (line2[0]) text_fit_r(COVER_X, 460, line2, C_TEXT, PANEL_X + PANEL_W - 15);
    } else {
        /* Disc placeholder (picked: tray + shadow + NO COVER inside the
           disc): the spine case frames it like art (same 4 case fills,
           deliberately duplicated so the tested with-cover branch stays
           untouched); the label is drawn AFTER the shine so it stays
           legible over the reflection. */
        ovl_fill(COVER_X + 4, 110 + 6, CASE_W, CASE_H, 0x73000000u);
        ovl_fill(COVER_X, 110, CASE_W, CASE_H, 0xff000000u);
        ovl_fill(COVER_X + 6, 110, 2, CASE_H, 0xff222222u);
        ovl_fill(COVER_X + 16, 110, 2, CASE_H, 0xff222222u);
        ovl_round_fill(TRAY_X, TRAY_Y, TRAY_S, TRAY_S, 12, 0xff1a1a1au);
        cover_disc_fill(DISC_CX + 6, DISC_CY + 8, DISC_R, 0x99000000u);
        cover_disc_fill(DISC_CX, DISC_CY, DISC_R, 0xff050505u);
        ovl_circle(DISC_CX, DISC_CY, DISC_R, 0xff333333u);
        cover_disc_fill(DISC_CX, DISC_CY, 15, 0xff1a1a1au);
        ovl_circle(DISC_CX, DISC_CY, 15, 0xff888888u);
        cover_disc_fill(DISC_CX, DISC_CY, 5, 0xff000000u);
        cover_disc_shine(DISC_CX, DISC_CY, DISC_R,
                         COVER_X + SPINE_W + 2, 110 + 6, CASE_ART);
        ovl_text(DISC_CX - ovl_text_width("NO COVER") / 2, DISC_CY + 34,
                 "NO COVER", C_DIM);
        text_fit_r(COVER_X, 420, title, C_ACCENT, PANEL_X + PANEL_W - 15);
        if (line2[0]) text_fit_r(COVER_X, 460, line2, C_TEXT, PANEL_X + PANEL_W - 15);
    }
}

/* Yes/No confirm for deleting a title's personal settings. Same layout and
   buttons as the VMC-import and copy-disc confirms (X confirms, Circle
   goes back): the page name in the header bar, the question in accent,
   the details below, footer "X delete    Circle back". 1 = confirmed
   (X), 0 = back (Circle) or exit. (No generic confirm helper exists in
   this file — those confirms are inline loops — so this factors the
   identical pattern rather than duplicating the loop a third time.) */
static int confirm_delete_personal(const char *title)
{
    char q[300];
    while (!app_exit_requested) {
        uint32_t p;
        frame_begin(0);
        p = input_pressed();
        header("Personal settings", NULL);
        ovl_text(MARGIN, 110, "Delete personal settings?", C_ACCENT);
        snprintf(q, sizeof q, "Delete personal settings for %s?", title);
        text_fit(MARGIN, 170, q, C_TEXT);
        ovl_text(MARGIN, 210, "This cannot be undone.", C_DIM);
        footer("X delete    Circle back");
        frame_end();
        if (p & PAD_CIRCLE) return 0;
        if (p & PAD_CROSS) return 1;
    }
    return 0;
}

/* list entry 0 is the disc in the drive, entry 1 is Boot BIOS, entries 2.. the game files */
const char *menu_run(void)
{
    static int sel = -1, top;
    const int rows = 12;
    if (!scanned) rescan();
    check_own_settings();               /* may have changed in the game just played */
    if (sel < 0 || sel > ngames + 1) sel = ngames ? 2 : 0;   /* the first game file, as before */
    disc_identify_start();              /* which game is in the drive (in the background) */
    while (!app_exit_requested) {
        uint32_t p;
        int i, n = ngames + 2, y0 = 90;
        char sub[64];
        frame_begin(0);
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + n - 1) % n;
        if (p & PAD_DOWN) sel = (sel + 1) % n;
        if (p & (PAD_L1 | PAD_R1)) {
            /* Letter jump (game list only; the settings pages keep L1/R1
               as page flips, and the card pickers keep them as page
               scrolls): R1 moves to the first game of the next
               first-letter group, L1 to the first game of the previous
               group, wrapping around the list. Groups are the shown file
               name's first character (case-insensitive); the list is
               sorted by that name, so groups appear in order and a letter
               with no games is simply skipped. From the disc/BIOS rows
               (no letter) R1 goes to the first game, L1 to the last. */
            if (ngames) {
                int dir = (p & PAD_R1) ? 1 : -1, gi = sel >= 2 ? sel - 2 : -1, j, tgt = -1;
                if (gi < 0) {
                    tgt = dir > 0 ? 0 : ngames - 1;
                } else {
                    int cur = toupper((unsigned char)base_name(games[gi])[0]);
                    if (dir > 0) {
                        for (j = 1; j <= ngames && tgt < 0; j++) {
                            int k = (gi + j) % ngames;
                            if (toupper((unsigned char)base_name(games[k])[0]) != cur) tgt = k;
                        }
                    } else {
                        int prev = -1;
                        for (j = 1; j <= ngames && prev < 0; j++) {
                            int k = (gi + ngames - j) % ngames;
                            if (toupper((unsigned char)base_name(games[k])[0]) != cur) prev = k;
                        }
                        if (prev >= 0) {
                            /* rewind to the first game of that group */
                            int g = toupper((unsigned char)base_name(games[prev])[0]), k;
                            tgt = prev;
                            for (k = 0; k < ngames; k++) {
                                int b = (tgt + ngames - 1) % ngames;
                                if (toupper((unsigned char)base_name(games[b])[0]) != g) break;
                                tgt = b;
                            }
                        }
                    }
                }
                if (tgt >= 0) sel = tgt + 2;
            }
        }
        if (p & PAD_CROSS) {
            if (sel == 0) { const char *d = start_disc(); if (d) { cover_release(); return d; } continue; }
            if (sel == 1) {   /* Boot BIOS: the PS1 BIOS menu, no disc */
                if (!hooks_bios_present()) {
                    vmc_message("Boot BIOS", "No real BIOS found", "Put scph5501.bin in " APP_DIR "/system/", "The built-in HLE BIOS has no menu.");
                    continue;
                }
                logf_("menu: starting BIOS menu\n");
                cover_release();
                return "bios:";   /* sentinel: run_game + the core boot the BIOS with no disc */
            }
            logf_("menu: starting %s\n", games[sel - 2]);
            cover_release();
            return games[sel - 2];
        }
        if ((p & PAD_CIRCLE) && sel == 0) {   /* copy the disc to the HDD */
            char t[128], cue[512];
            if (disc_identify_result(t, sizeof t) == 1 && copy_disc(t[0] ? t : "PS1 disc", cue, sizeof cue)) {
                rescan(); check_own_settings();
                for (i = 0; i < ngames; i++) if (!strcmp(games[i], cue)) sel = i + 2;
            }
            continue;
        }
        if (p & PAD_TRIANGLE) {
            /* A scanned game row opens the shared panel for that title (its
               own file overlays the global values if it exists); the disc
               entry, Boot BIOS, and an empty list open it global-only. */
            char tkey[256]; const char *tk = NULL;
            if (sel >= 2 && sel - 2 < ngames) {
                const char *b = strrchr(games[sel - 2], '/');
                snprintf(tkey, sizeof tkey, "%s", b ? b + 1 : games[sel - 2]);
                { char *dot = strrchr(tkey, '.'); if (dot) *dot = 0; }
                tk = tkey;
            }
            settings_panel(tk, 0, NULL, 0);
            check_own_settings();   /* a newly created .ini shows its tag at once */
            continue;
        }
        if (p & PAD_SQUARE) { rescan(); check_own_settings(); sel = ngames ? 2 : 0; top = 0; disc_identify_start(); continue; }
        if ((p & PAD_R3) && sel >= 2 && sel - 2 < ngames && own_cfg[sel - 2]) {
            /* R3 deletes this title's personal settings, after a confirm.
               Silent no-op otherwise (disc/BIOS row, or no personal file). */
            char key[256]; const char *b = strrchr(games[sel - 2], '/');
            snprintf(key, sizeof key, "%s", b ? b + 1 : games[sel - 2]);
            { char *dot = strrchr(key, '.'); if (dot) *dot = 0; }
            if (confirm_delete_personal(key)) {
                if (config_game_delete(key)) {
                    logf_("menu: deleted personal settings for %s\n", key);
                    check_own_settings();   /* the tag disappears at once */
                    snprintf(disc_msg, sizeof disc_msg, "Personal settings deleted.");
                } else {
                    logf_("menu: could not delete personal settings for %s\n", key);
                    snprintf(disc_msg, sizeof disc_msg, "Could not delete personal settings.");
                }
                disc_msg2[0] = 0;
                disc_msg_frames = 240;
            }
            continue;
        }
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        snprintf(sub, sizeof sub, "%d game%s", ngames, ngames == 1 ? "" : "s");
        header_app(sub);
        for (i = top; i < n && i < top + rows; i++) {
            int y = y0 + (i - top) * 44;
            if (i == sel) ovl_fill(30, y - 6, 870, 42, C_SEL);
            if (i == 0) {               /* the disc in the drive: icon, its game, "from disc" */
                char t[128], nm[160];
                int r = disc_identify_result(t, sizeof t), max;
                const char *tag = r == 1 ? "from disc" : "";
                disc_icon(MARGIN, y - 1, i == sel ? C_SEL : 0xff000000);
                if (r < 0) snprintf(nm, sizeof nm, "Checking the disc drive...");
                else if (r == 0) snprintf(nm, sizeof nm, "No PS1 disc in the drive");
                else if (r == 2) snprintf(nm, sizeof nm, "Disc in the drive (could not read it)");
                else snprintf(nm, sizeof nm, "%s", t[0] ? t : "PS1 disc");
                max = (LIST_TAG_R - ovl_text_width(tag) - ovl_text_width(" ") - (MARGIN + 44)) / ovl_text_width("W");
                if ((int)strlen(nm) > max && max > 3) { nm[max - 3] = 0; strcat(nm, "..."); }
                ovl_text(MARGIN + 44, y, nm, r == 1 ? (i == sel ? C_ACCENT : C_TEXT) : C_DIM);
                if (tag[0]) ovl_text(LIST_TAG_R - ovl_text_width(tag), y, tag, C_DIM);
            } else if (i == 1) {   /* Boot BIOS, below the disc entry */
                const char *tag = "memory card manager";
                ovl_text(MARGIN, y, "Boot BIOS", i == sel ? C_ACCENT : C_TEXT);
                ovl_text(LIST_TAG_R - ovl_text_width(tag), y, tag, C_DIM);
            } else if (own_cfg[i - 2]) {
                ovl_text(LIST_TAG_R - ovl_text_width("own settings"), y, "own settings", C_DIM);
                {   /* the name ends before the tag */
                    char nm[256];
                    int max = (LIST_TAG_R - ovl_text_width("own settings ") - MARGIN) / ovl_text_width("W");
                    const char *s = base_name(games[i - 2]);
                    if ((int)strlen(s) > max && max > 3) { snprintf(nm, sizeof nm, "%.*s...", max - 3, s); s = nm; }
                    ovl_text(MARGIN, y, s, i == sel ? C_ACCENT : C_TEXT);
                }
            } else text_fit_r(MARGIN, y, base_name(games[i - 2]), i == sel ? C_ACCENT : C_TEXT, LIST_TAG_R);
        }
        if (ngames) {                  /* the selected game's cover, right of the list */
            char title[256], line2[128];
            if (sel == 0) {
                char t[128];
                int r = disc_identify_result(t, sizeof t);
                char key[1300];
                snprintf(key, sizeof key, "disc:%s", disc_identify_serial());
                cover_update(key, NULL, 1, disc_identify_serial());
                snprintf(title, sizeof title, "%s", r == 1 ? (t[0] ? t : "PS1 disc") : "PS1 disc");
                snprintf(line2, sizeof line2, "%s", r == 1 ? "from disc" : "");
            } else if (sel == 1) {
                cover_release();   /* no cover for BIOS: the placeholder panel */
                snprintf(title, sizeof title, "Boot BIOS");
                snprintf(line2, sizeof line2, "memory card manager");
            } else {
                const char *s = base_name(games[sel - 2]), *e = ext_of(s);
                char ext[16];
                int k;
                cover_update(games[sel - 2], games[sel - 2], 0, NULL);
                snprintf(title, sizeof title, "%s", s);
                {   /* panel title without the file extension ("Tekken 3", not "Tekken 3.cue") */
                    char *dot = strrchr(title, '.');
                    if (dot && dot != title) *dot = 0;
                }
                for (k = 0; e[k] && k < 15; k++) ext[k] = (char)toupper((unsigned char)e[k]);
                ext[k] = 0;
                if (ext[0] && own_cfg[sel - 2]) snprintf(line2, sizeof line2, "%s   own settings", ext);
                else if (own_cfg[sel - 2]) snprintf(line2, sizeof line2, "own settings");
                else snprintf(line2, sizeof line2, "%s", ext);
            }
            draw_cover_panel(title, line2);
        }
        if (!ngames) {                  /* below the disc entry */
            ovl_text(MARGIN, 214, "No game files found.", C_TEXT);
            ovl_text(MARGIN, 264, "Put .cue/.bin, .chd, .pbp, .iso or .m3u files in one of:", C_DIM);
            for (i = 0; scan_dirs[i] && i < 6; i++) ovl_text(MARGIN + 32, 304 + i * 36, scan_dirs[i], C_TEXT);
            ovl_text(MARGIN, 534, "BIOS (optional, better compatibility):", C_DIM);
            ovl_text(MARGIN + 32, 570, APP_DIR "/system/scph5501.bin", C_TEXT);
        }
        if (disc_msg_frames > 0) {
            disc_msg_frames--;
            if (disc_msg2[0]) {
                ovl_fill(30, OVL_H - 176, OVL_W - 60, 80, C_SEL);
                ovl_text(MARGIN, OVL_H - 170, disc_msg, C_ACCENT);
                ovl_text(MARGIN, OVL_H - 134, disc_msg2, C_TEXT);
            } else {
                ovl_fill(30, OVL_H - 140, OVL_W - 60, 44, C_SEL);
                ovl_text(MARGIN, OVL_H - 134, disc_msg, C_ACCENT);
            }
        }
        {   /* R3 hint only when the selected title has personal settings */
            char foot[256];
            if (sel >= 2 && sel - 2 < ngames && own_cfg[sel - 2]) {
                snprintf(foot, sizeof foot, "X play    Triangle settings    Square rescan    L1 R1 letter    R3 delete personal settings");
                if (prompt_width(foot) > OVL_W - 2 * MARGIN)
                    snprintf(foot, sizeof foot, "X play    Triangle settings    Square rescan    L1 R1 letter    R3 delete settings");
            } else
                snprintf(foot, sizeof foot, "X play    Triangle settings    Square rescan    L1 R1 letter");
            footer2(sel == 0 && disc_identify_result(NULL, 0) == 1
                        ? "X play   Circle copy to HDD   Triangle settings   Square rescan"
                        : sel == 1
                        ? "X boot BIOS    Triangle settings    Square rescan    L1 R1 letter"
                        : foot,
                    "In game: SELECT + START opens the menu (save states, settings)");
        }
        frame_end();
    }
    cover_release();
    return NULL;
}

/* ---------------- in-game menu ---------------- */
extern int game_save_state(int slot);
extern int game_load_state(int slot);
extern const char *game_state_info(int slot, char *out, size_t n);
extern int game_dump_vram(void);
extern void game_reset(void);
extern void game_settings_changed(void);

/* ---------------- cheats: full-screen selection ----------------
 * The pause quick menu is only the entry point: this screen lists one
 * game's codes (the bundled database file for its serial), paged like the
 * game list, so even a thousand-entry list (FFIX) fits. Checkboxes are
 * ASCII "[X]"/"[ ]": the overlay font has no glyphs.
 * Toggles only change the pending selection. Triangle saves: the state
 * file is written and the engine is fed (manual codes fire once, on this
 * save). Circle discards everything pending: the game was never touched,
 * so there is nothing to undo. Unsupported codes show their reason and
 * cannot be toggled; the state file keeps names, never indices. */
#define CHT_ROWS 12
static int cheat_screen(void)
{
    char dbpath[1200], stpath[1200], serial[32];
    cht_db db;
    cht_state *st = NULL;
    int nst = 0, n, i, sel = 0, top = 0, saved = 0;
    int *enabled, *opt_idx;
    uint32_t *opt_val;
    const char *msg = NULL;
    static int held_frames;

    cht_dashed_serial(hooks_game_serial(), serial, sizeof serial);
    if (!serial[0]) {
        vmc_message("Cheats", "No disc serial", "Cheats need a disc serial to look up.", NULL);
        return 0;
    }
    snprintf(dbpath, sizeof dbpath, APP_DIR "/cheats/%s.cht", serial);
    snprintf(stpath, sizeof stpath, APP_DIR "/configs/%s.cheats", serial);
    if (cht_db_load(dbpath, &db) != 0) {
        char a[128];
        snprintf(a, sizeof a, "No cheat database file for %s.", serial);
        vmc_message("Cheats", "No cheats for this game", a, "The bundled database has no file for this serial.");
        return 0;
    }
    cht_state_load(stpath, &st, &nst);   /* missing file: nothing enabled */
    n = db.n_codes;
    enabled = (int *)calloc(n > 0 ? (size_t)n : 1, sizeof(int));
    opt_idx = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    opt_val = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));
    if (!enabled || !opt_idx || !opt_val) {
        free(enabled); free(opt_idx); free(opt_val);
        cht_state_free(st, nst);
        cht_db_free(&db);
        return 0;
    }
    for (i = 0; i < n; i++) {
        cht_code *c = &db.codes[i];
        int j, found = -1;
        opt_idx[i] = 0;
        if (c->is_range) opt_val[i] = c->range_min;
        else if (c->n_opts > 0) opt_val[i] = c->opt_vals[0];
        else opt_val[i] = 0;
        if (!c->supported) continue;   /* a stale state entry for an unsupported code is pruned on save */
        for (j = 0; j < nst; j++)
            if (!strcmp(st[j].name, c->name)) { found = j; break; }
        if (found < 0) continue;
        enabled[i] = 1;
        if (!st[found].has_value) continue;
        if (c->is_range) {
            opt_val[i] = st[found].value;
            if (opt_val[i] < c->range_min) opt_val[i] = c->range_min;
            if (opt_val[i] > c->range_max) opt_val[i] = c->range_max;
        } else if (c->n_opts > 0) {
            int k;
            for (k = 0; k < c->n_opts; k++)
                if (c->opt_vals[k] == st[found].value) { opt_idx[i] = k; break; }
            opt_val[i] = c->opt_vals[opt_idx[i]];
        }
    }
    cht_state_free(st, nst);

    logf_("menu: cheat screen (%s, %d codes)\n", serial, n);
    while (!app_exit_requested) {
        uint32_t p;
        int r;
        frame_begin(0);
        p = input_pressed();
        {   /* a held direction repeats after a moment (same as the VMC picker) */
            uint32_t h = (input_pad(0)->buttons | input_pad(1)->buttons) & (PAD_UP | PAD_DOWN);
            held_frames = h ? held_frames + 1 : 0;
            if (held_frames > 20 && held_frames % 3 == 0) p |= h;
        }
        if (n > 0) {
            if (p & PAD_UP) sel = (sel + n - 1) % n;
            if (p & PAD_DOWN) sel = (sel + 1) % n;
            if (p & PAD_L1) sel = sel > CHT_ROWS ? sel - CHT_ROWS : 0;
            if (p & PAD_R1) sel = sel + CHT_ROWS < n ? sel + CHT_ROWS : n - 1;
            if (sel < top) top = sel;
            if (sel >= top + CHT_ROWS) top = sel - CHT_ROWS + 1;
        }
        if ((p & (PAD_LEFT | PAD_RIGHT)) && n > 0) {
            cht_code *c = &db.codes[sel];
            int dir = (p & PAD_LEFT) ? -1 : 1;
            if (c->supported && c->has_options) {
                if (c->is_range) {
                    int v = (int)opt_val[sel] + dir;
                    if (v < (int)c->range_min) v = (int)c->range_min;
                    if (v > (int)c->range_max) v = (int)c->range_max;
                    opt_val[sel] = (uint32_t)v;
                } else if (c->n_opts > 0) {
                    opt_idx[sel] = (opt_idx[sel] + dir + c->n_opts) % c->n_opts;
                    opt_val[sel] = c->opt_vals[opt_idx[sel]];
                }
                msg = NULL;
            }
        }
        if ((p & PAD_CROSS) && n > 0) {
            if (!db.codes[sel].supported) msg = "This code is not supported by the PS1 cheat engine";
            else { enabled[sel] ^= 1; msg = NULL; }
        }
        if (p & PAD_CIRCLE) break;
        if (p & PAD_TRIANGLE) {
            /* The configs dir exists once any game has its own settings;
               a cheat-only game may not have it yet. */
            mkdir(APP_DIR "/configs", 0777);
            if (cht_state_save(stpath, db.codes, enabled, opt_val, n) != 0) {
                msg = "Could not write the cheat state file";
            } else {
                cheat_apply_saved(1);   /* this save is the manual codes' trigger */
                saved = 1;
                break;
            }
        }
        header("Cheats", serial);
        if (n == 0) {
            ovl_text(MARGIN, 90, "No cheats in this file.", C_DIM);
        }
        for (r = 0; r < CHT_ROWS; r++) {
            int idx = top + r, y = 90 + r * 44;
            cht_code *c;
            char line[300], opt[128];
            uint32_t col;
            int optw = 0;
            if (idx >= n) break;
            c = &db.codes[idx];
            if (idx == sel) ovl_fill(30, y - 6, OVL_W - 60, 42, C_SEL);
            col = !c->supported ? C_DIM : idx == sel ? C_ACCENT : C_TEXT;
            ovl_text(MARGIN, y, enabled[idx] ? "[X]" : "[ ]", col);
            if (c->has_options && c->supported) {
                if (c->is_range) snprintf(opt, sizeof opt, "%u", opt_val[idx]);
                else if (c->n_opts > 0) snprintf(opt, sizeof opt, "%s", c->opt_names[opt_idx[idx]]);
                else opt[0] = 0;
                optw = ovl_text_width(opt);
                ovl_text(OVL_W - MARGIN - optw, y, opt, C_DIM);
            }
            if (!c->supported) snprintf(line, sizeof line, "%s (unsupported)", c->name);
            else if (c->manual) snprintf(line, sizeof line, "%s (manual)", c->name);
            else snprintf(line, sizeof line, "%s", c->name);
            text_fit_r(MARGIN + 72, y, line, col, OVL_W - MARGIN - optw - (optw ? 24 : 0));
        }
        if (msg) ovl_text(MARGIN, 618, msg, C_ACCENT);
        footer("X toggle   L1 R1 page   Left/Right option   Triangle save   Circle back");
        frame_end();
    }
    free(enabled); free(opt_idx); free(opt_val);
    cht_db_free(&db);
    return saved;
}

#define DSWAP_ROWS 12

/* One picker row: either a disc of the loaded playlist (index into the
   core's image list) or a disc image file from the scanned game list. */
typedef struct {
    int is_playlist;
    unsigned index;      /* playlist rows only */
    const char *path;    /* file rows only: pointer into games[] (untouched while in-game) */
    char label[256];
} dswap_entry;

/* Full-screen disc picker, opened from the pause menu. Returns 1 when a
   swap ran, with the swapped disc's label in out_label; 0 on back or
   when nothing could be swapped. The pause menu is the gateway only:
   the swap itself runs here, and the game resumes from the pause menu. */
static int disc_swap_screen(char *out_label, size_t out_len)
{
    dswap_entry *list;
    int n = 0, sel = 0, top = 0, swapped = 0, i;
    unsigned num = 0, cur = 0;
    char curpath[1200], curlabel[256];
    const char *msg = NULL;
    static int held_frames;

    if (!disk_iface_available()) {
        vmc_message("Swap disc", "Disk control not available", "The core did not offer its disk-control interface.", NULL);
        return 0;
    }
    num = disk_iface_num_images();
    cur = disk_iface_current_index();
    curpath[0] = 0;
    disk_iface_path(cur, curpath, sizeof curpath);
    if (disk_iface_label(cur, curlabel, sizeof curlabel) != 0)
        snprintf(curlabel, sizeof curlabel, "%s", app_game_name());

    /* Count first, then fill: one malloc, no realloc failure paths. */
    if (num > 1) n = (int)num;
    for (i = 0; i < ngames; i++) {
        const char *p = games[i];
        if (!is_ext(p, "cue") && !is_ext(p, "chd") && !is_ext(p, "pbp") &&
            !is_ext(p, "ccd") && !is_ext(p, "iso") && !is_ext(p, "img") &&
            !is_ext(p, "bin"))
            continue;
        if (curpath[0] && !strcmp(p, curpath)) continue;
        n++;
    }
    if (n == 0) {
        vmc_message("Swap disc", "No discs to swap to", "No other disc image was found in the scanned folders.", NULL);
        return 0;
    }
    list = (dswap_entry *)malloc(sizeof *list * (size_t)n);
    if (!list) return 0;
    n = 0;
    /* Section 1: the loaded playlist's discs, only when there is more
       than one (a single image has nothing to swap to within itself). */
    if (num > 1) {
        for (i = 0; (unsigned)i < num; i++) {
            dswap_entry *e = &list[n++];
            e->is_playlist = 1;
            e->index = (unsigned)i;
            e->path = NULL;
            if (disk_iface_label((unsigned)i, e->label, sizeof e->label) != 0)
                snprintf(e->label, sizeof e->label, "Disc %u", (unsigned)i + 1);
        }
    }
    /* Section 2: every scanned disc image. NOT locked to the playlist:
       this is what a GameShark-style boot disc swaps to, and it also
       covers swapping a single-disc game to any other image. Playlist
       files (.m3u) and executables (.exe) are not disc targets; the
       disc already in the drive is excluded (it is section 1's
       "(current)" row, or the game itself for a single image). */
    for (i = 0; i < ngames; i++) {
        dswap_entry *e;
        const char *p = games[i];
        if (!is_ext(p, "cue") && !is_ext(p, "chd") && !is_ext(p, "pbp") &&
            !is_ext(p, "ccd") && !is_ext(p, "iso") && !is_ext(p, "img") &&
            !is_ext(p, "bin"))
            continue;
        if (curpath[0] && !strcmp(p, curpath)) continue;
        e = &list[n++];
        e->is_playlist = 0;
        e->index = 0;
        e->path = p;
        snprintf(e->label, sizeof e->label, "%s", base_name(p));
    }
    logf_("menu: swap disc screen (%d entries, %u playlist discs)\n", n, num);
    while (!app_exit_requested) {
        uint32_t p;
        int r;
        frame_begin(0);
        p = input_pressed();
        {   /* a held direction repeats after a moment (same as the VMC picker) */
            uint32_t h = (input_pad(0)->buttons | input_pad(1)->buttons) & (PAD_UP | PAD_DOWN);
            held_frames = h ? held_frames + 1 : 0;
            if (held_frames > 20 && held_frames % 3 == 0) p |= h;
        }
        if (p & PAD_UP) sel = (sel + n - 1) % n;
        if (p & PAD_DOWN) sel = (sel + 1) % n;
        if (p & PAD_L1) sel = sel > DSWAP_ROWS ? sel - DSWAP_ROWS : 0;
        if (p & PAD_R1) sel = sel + DSWAP_ROWS < n ? sel + DSWAP_ROWS : n - 1;
        if (sel < top) top = sel;
        if (sel >= top + DSWAP_ROWS) top = sel - DSWAP_ROWS + 1;
        if (p & PAD_CIRCLE) break;
        if (p & PAD_CROSS) {
            dswap_entry *e = &list[sel];
            int ok;
            if (e->is_playlist && e->index == cur) {
                msg = "That disc is already in the drive";
            } else {
                /* The drive now holds the new disc; the picker closes
                   and the game resumes from the pause menu. */
                ok = e->is_playlist ? disk_iface_swap_to_index(e->index)
                                    : disk_iface_swap_to_file(e->path);
                if (ok == 0) {
                    if (out_label && out_len) snprintf(out_label, out_len, "%s", e->label);
                    swapped = 1;
                    break;
                }
                msg = "Swap failed";
            }
        }
        header("Swap disc", curlabel);
        for (r = 0; r < DSWAP_ROWS; r++) {
            int idx = top + r, y = 90 + r * 44;
            dswap_entry *e;
            char line[300];
            uint32_t col;
            if (idx >= n) break;
            e = &list[idx];
            if (idx == sel) ovl_fill(30, y - 6, OVL_W - 60, 42, C_SEL);
            col = (e->is_playlist && e->index == cur) ? C_DIM : idx == sel ? C_ACCENT : C_TEXT;
            if (e->is_playlist) {
                if (e->index == cur) snprintf(line, sizeof line, "Disc %u: %s (current)", e->index + 1, e->label);
                else snprintf(line, sizeof line, "Disc %u: %s", e->index + 1, e->label);
            } else snprintf(line, sizeof line, "%s", e->label);
            text_fit_r(MARGIN, y, line, col, OVL_W - MARGIN);
        }
        if (msg) ovl_text(MARGIN, 618, msg, C_ACCENT);
        footer("X swap   L1 R1 page   Circle back");
        frame_end();
    }
    free(list);
    return swapped;
}

int menu_ingame(void)
{
    static const char *items[] = { "Resume", "Save state", "Load state", "Swap disc", "Reset", "Joystick mode", "Settings", "Cheats",
                                   "Quit to game list", "Dump VRAM (debug)" };
    /* Action ids are the items[] indexes. Swap disc sits above Reset and
       is shown for any running disc (not the BIOS): it is NOT gated on
       the playlist size, so a GameShark-style boot disc (a single image)
       can swap to any scanned image. The Cheats row only exists when
       the global gate is on (and the running game has a serial); the Dump
       row only when VRAM check is on. qitems maps display rows to action
       ids, rebuilt every frame so a gate toggle in the shared settings
       panel takes effect when returning here. */
    int qitems[10], N = 0;
    static int slot;             /* kept while the app runs */
    int sel = 0;
    const char *msg = NULL;
    char info[40] = "";
    char close_msg[64] = "";
    logf_("menu: in-game menu opened\n");
    menu_name = "in-game";
    game_state_info(slot, info, sizeof info);
    while (!app_exit_requested) {
        uint32_t p;
        int i;
        N = 0;
        qitems[N++] = 0; qitems[N++] = 1; qitems[N++] = 2;
        if (!app_game_is_bios()) qitems[N++] = 3;
        qitems[N++] = 4; qitems[N++] = 5; qitems[N++] = 6;
        if (cfg.cheat_enable && !app_game_is_bios() && hooks_game_serial()[0]) qitems[N++] = 7;
        qitems[N++] = 8;
        if (cfg.vram_check) qitems[N++] = 9;
        if (sel >= N) sel = N - 1;
        frame_begin(1);          /* the quick menu always draws over the game */
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + N - 1) % N;
        if (p & PAD_DOWN) sel = (sel + 1) % N;
        if ((sel == 1 || sel == 2) && (p & (PAD_LEFT | PAD_RIGHT))) {
            slot = (slot + (p & PAD_LEFT ? 9 : 1)) % 10;
            game_state_info(slot, info, sizeof info);
            msg = NULL;
        }
        if (p & PAD_CIRCLE) { menu_name = "game list"; logf_("menu: resume\n"); return 0; }
        if (p & PAD_CROSS) {
            switch (qitems[sel]) {
            case 0: menu_name = "game list"; logf_("menu: resume\n"); return 0;
            case 1:
                msg = game_save_state(slot) ? "State saved" : "Saving failed";
                game_state_info(slot, info, sizeof info);
                break;
            case 2: if (game_load_state(slot)) return 0; msg = "This slot is empty"; break;
            case 3: {
                static char sm[280];
                char lbl[256];
                if (disc_swap_screen(lbl, sizeof lbl)) {
                    snprintf(sm, sizeof sm, "Swapped to %s", lbl);
                    msg = sm;
                } else msg = NULL;
                break;
            }
            case 4: game_reset(); return 0;
            case 5:
                cfg.analog ^= 1;
                game_settings_changed();   /* applies the port type immediately */
                config_save();             /* persist now: per-game if the title has its own settings, else global */
                msg = cfg.analog ? "Joystick mode: analog" : "Joystick mode: digital";
                break;
            /* finish this frame (the paused menu) first: starting another one without
               presenting this one made the screen blink */
            case 6: {
                /* The shared panel, for the running game (BIOS: global only).
                   Its close message, if any, is the CPU live-switch notice. */
                const char *k = app_game_is_bios() ? NULL : app_game_name();
                close_msg[0] = 0;
                settings_panel(k, 1, close_msg, sizeof close_msg);
                if (close_msg[0]) msg = close_msg;
                break;
            }
            case 7: {
                static char cm[64];
                if (cheat_screen()) {
                    snprintf(cm, sizeof cm, "Cheats saved (%d on)", cheat_enabled_count());
                    msg = cm;
                } else msg = NULL;
                break;
            }
            case 8: menu_name = "game list"; logf_("menu: quit to game list\n"); return 1;
            case 9: {
                static char dm[64];
                int n = game_dump_vram();
                if (n) snprintf(dm, sizeof dm, "Dump %d written to USRDIR/dumps", n);
                msg = n ? dm : "Dump failed (see the log)";
                break;
            }
            }
        }
        ovl_round_fill(290, 110, 700, 130 + N * 46 + (msg ? 50 : 0), PANEL_R, C_PANEL);
        ovl_text(320, 132, "Paused", C_ACCENT);
        for (i = 0; i < N; i++) {
            int y = 180 + i * 46;
            int a = qitems[i];
            char line[80];
            if (a == 1 || a == 2) snprintf(line, sizeof line, "%s   < slot %d >", items[a], slot);
            else if (a == 5) snprintf(line, sizeof line, "%s: %s", items[a], cfg.analog ? "analog" : "digital");
            else if (a == 7) {
                int n = cheat_enabled_count();
                if (n > 0) snprintf(line, sizeof line, "%s: %d on", items[a], n);
                else snprintf(line, sizeof line, "%s", items[a]);
            }
            else snprintf(line, sizeof line, "%s", items[a]);   /* Quit, Dump: plain labels */
            if (i == sel) ovl_fill(305, y - 7, 670, 42, C_SEL);
            ovl_text(330, y, line, i == sel ? C_ACCENT : C_TEXT);
        }
        {
            char l2[80];
            snprintf(l2, sizeof l2, "Slot %d: %s", slot, info);
            ovl_text(330, 180 + N * 46 + 6, l2, C_DIM);
        }
        if (msg) ovl_text(330, 180 + N * 46 + 56, msg, C_ACCENT);
        frame_end();
    }
    return 1;
}
