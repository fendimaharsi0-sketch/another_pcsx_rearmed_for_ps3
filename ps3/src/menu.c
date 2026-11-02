/* Game list, settings and the in-game menu, drawn on the overlay. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include "common.h"
#include "core_hooks.h"
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

static void rescan(void)
{
    int i;
    for (i = 0; i < ngames; i++) free(games[i]);
    ngames = 0;
    for (i = 0; scan_dirs[i]; i++) scan(scan_dirs[i], 0);
    qsort(games, ngames, sizeof *games, cmp_games);
    scanned = 1;
    logf_("menu: %d games found\n", ngames);
}

/* ---------------- drawing helpers ---------------- */
#define C_TEXT   0xffe8e8e8
#define C_DIM    0xff9098a8
#define C_ACCENT 0xffffd24a
#define C_PANEL  0xe0141a26
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

static void footer(const char *s)
{
    ovl_fill(0, OVL_H - 52, OVL_W, 52, 0xff182238);
    ovl_text(MARGIN, OVL_H - 41, s, C_DIM);
}

/* two-line footer (the game list) */
static void footer2(const char *a, const char *b)
{
    ovl_fill(0, OVL_H - 84, OVL_W, 84, 0xff182238);
    ovl_text(MARGIN, OVL_H - 76, a, C_DIM);
    ovl_text(MARGIN, OVL_H - 44, b, C_DIM);
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
    { "Picture filter", "How the picture is scaled to your TV." },
    { "Aspect ratio", "Zoom keeps the PS1 shape and crops the top and bottom." },
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
    { "Texture filter", "Bilinear filtering for 3D textures, RSX only, upscaled pass only. Native VRAM stays nearest." },
};
#define NSET (int)(sizeof settings_items / sizeof settings_items[0])
#define SET_ROW 27   /* row height (30-pixel font) */
#define SET_VISIBLE 17   /* rows shown above the help line; the list scrolls past them */

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
    case 2: snprintf(b, n, "%s", filter_names[cfg.filter]); break;
    case 3: snprintf(b, n, "%s", aspect_names[cfg.aspect]); break;
    case 4: snprintf(b, n, "%d%% x %d%%", cfg.screen_w, cfg.screen_h); break;
    case 5: snprintf(b, n, "%s", cfg.frameskip ? "Auto" : "Off"); break;
    case 6: snprintf(b, n, "%s", cfg.dithering ? "On" : "Off"); break;
    case 7: snprintf(b, n, "%s", cfg.show_fps ? "On" : "Off"); break;
    case 8: snprintf(b, n, "%s", cfg.gpu_thread ? "On" : "Off"); break;
    case 9: snprintf(b, n, "%s", cfg.stick_dpad ? "On" : "Off"); break;
    case 10: snprintf(b, n, "%s", cfg.stick_buttons ? "On" : "Off"); break;
    case 11: snprintf(b, n, "%s", cfg.vibration ? "On" : "Off"); break;
    case 12: snprintf(b, n, "%s", cfg.spu_mdec ? "SPUs" : "PPU"); break;
    case 13: snprintf(b, n, "%s", cfg.cpu_clock == 2 ? "+75%" : cfg.cpu_clock == 1 ? "+50%" : "Normal"); break;
    case 14: snprintf(b, n, "%s", cfg.audio_stretch ? "Slow down" : "Keep pitch"); break;
    case 15: snprintf(b, n, "%s", cfg.skip_intro ? "On" : "Off"); break;
    case 16: snprintf(b, n, "%s", cfg.pal60 ? "On" : "Off"); break;
    case 17: snprintf(b, n, "%s", !cfg.boot_logo ? "Off" : hooks_bios_present() ? "On" : "On (no BIOS found)"); break;
    case 18: snprintf(b, n, "%s", cfg.speed_limit == 1 ? "Strict" : cfg.speed_limit == 2 ? "Off" : "Normal"); break;
    case 19: snprintf(b, n, "%s", cfg.vram_check ? "On" : "Off"); break;
    case 20: snprintf(b, n, "%s", cfg.debug_log ? "On" : "Off"); break;
    case 21: snprintf(b, n, "%s", cfg.texfilter ? "Bilinear" : "Nearest"); break;
    }
}

static void setting_value(int i, char *b, size_t n);
static void setting_change_(int i, int dir, int in_game);
static void screen_size_page(int in_game);
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
    case 0:                                             /* needs a restart of the game */
        if (!in_game) {   /* Dynarec, 2 threads (Lightrec compiles on the PPU's second thread) -> 1 thread -> Interpreter */
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
    case 2: cfg.filter = (cfg.filter + (dir > 0 ? 1 : FILTER_COUNT - 1)) % FILTER_COUNT; break;
    case 3: {
        int k = 0, n = (int)(sizeof aspect_order / sizeof aspect_order[0]);
        while (k < n - 1 && aspect_order[k] != cfg.aspect) k++;
        cfg.aspect = aspect_order[(k + (dir > 0 ? 1 : n - 1)) % n];
        break;
    }
    case 4: screen_size_page(in_game); break;
    case 5: cfg.frameskip ^= 1; break;
    case 6: cfg.dithering ^= 1; break;
    case 7: cfg.show_fps ^= 1; break;
    case 8: cfg.gpu_thread ^= 1; break;
    case 9: cfg.stick_dpad ^= 1; break;
    case 10: cfg.stick_buttons ^= 1; break;
    case 11: cfg.vibration ^= 1; break;
    case 12: cfg.spu_mdec ^= 1; break;
    case 13: cfg.cpu_clock = (cfg.cpu_clock + (dir > 0 ? 1 : 2)) % 3; break;
    case 14: cfg.audio_stretch ^= 1; break;
    case 15: cfg.skip_intro ^= 1; break;
    case 16: cfg.pal60 ^= 1; break;
    case 17: cfg.boot_logo ^= 1; break;
    case 18: cfg.speed_limit = (cfg.speed_limit + (dir > 0 ? 1 : 2)) % 3; break;
    case 19: cfg.vram_check ^= 1; break;
    case 20: cfg.debug_log ^= 1; break;
    case 21: cfg.texfilter ^= 1; break;
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
        ovl_text(cx - ovl_text_width("Square reset   X done") / 2, y + 200, "Square reset   X done", C_TEXT);
        frame_end();
    }
}

static void draw_settings(int sel, int y0, int in_game)
{
    static int top;                      /* first row shown: the selection stays in view */
    int i, rows = NSET < SET_VISIBLE ? NSET : SET_VISIBLE;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top > NSET - rows) top = NSET - rows;
    if (top < 0) top = 0;
    for (i = top; i < top + rows; i++) {
        char v[64];
        int y = y0 + (i - top) * SET_ROW;
        if (i == sel) ovl_fill(30, y - 1, OVL_W - 60, SET_ROW, C_SEL);
        ovl_text(50, y, settings_items[i].label, i == sel ? C_ACCENT : C_TEXT);
        setting_value(i, v, sizeof v);
        ovl_text(520, y, "<", C_DIM);
        ovl_text(550, y, v, (in_game && i == 0) ? C_DIM : C_TEXT);
        ovl_text(560 + ovl_text_width(v), y, ">", C_DIM);
    }
    {   /* more rows above / below: a triangle at the right of the first / last row */
        int k, ax = OVL_W - 64;
        for (k = 0; k < 9; k++) {
            if (top > 0)                         /* pointing up, at the first row */
                ovl_fill(ax - k, y0 + 6 + k, 2 * k + 1, 1, C_ACCENT);
            if (top + rows < NSET)               /* pointing down, at the last row */
                ovl_fill(ax - k, y0 + (rows - 1) * SET_ROW + 20 - k, 2 * k + 1, 1, C_ACCENT);
        }
    }
    ovl_text(50, y0 + rows * SET_ROW + 12, settings_items[sel].help, C_DIM);
    if (sel == 0) ovl_text(50, y0 + rows * SET_ROW + 40, in_game ? "(change the CPU from the game list)"
                                                              : "Interpreter if a game misbehaves.", C_DIM);
}

static void settings_screen(void)
{
    int sel = 0;
    logf_("menu: settings\n");
    menu_name = "settings";
    while (!app_exit_requested) {
        uint32_t p;
        frame_begin(0);
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + NSET - 1) % NSET;
        if (p & PAD_DOWN) sel = (sel + 1) % NSET;
        if (p & (PAD_LEFT | PAD_CROSS)) setting_change(sel, p & PAD_LEFT ? -1 : 1, 0);
        if (p & PAD_RIGHT) setting_change(sel, 1, 0);
        if (p & (PAD_CIRCLE | PAD_TRIANGLE)) {
            config_save();
            logf_("menu: back to game list\n");
            menu_name = "game list";
            return;
        }
        header("Settings for all games", NULL);
        draw_settings(sel, 84, 0);
        {
            char b[200];
            snprintf(b, sizeof b, "BIOS: %s", hooks_bios_status());
            ovl_text(50, OVL_H - 82, b, C_DIM);
        }
        footer("Up/Down select    Left/Right change    Circle back");
        frame_end();
    }
}

/* ---------------- game list ---------------- */
int disc_probe(char *label, size_t n);     /* disc.c */
void disc_identify_start(void);
int  disc_identify_result(char *title, size_t n);
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
    header(APP_TITLE, NULL);
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

/* list entry 0 is the disc in the drive, entries 1.. the game files */
const char *menu_run(void)
{
    static int sel = -1, top;
    const int rows = 12;
    if (!scanned) rescan();
    check_own_settings();               /* may have changed in the game just played */
    if (sel < 0 || sel > ngames) sel = ngames ? 1 : 0;   /* the first game file, as before */
    disc_identify_start();              /* which game is in the drive (in the background) */
    while (!app_exit_requested) {
        uint32_t p;
        int i, n = ngames + 1, y0 = 90;
        char sub[64];
        frame_begin(0);
        p = input_pressed();
        if (p & PAD_UP) sel = (sel + n - 1) % n;
        if (p & PAD_DOWN) sel = (sel + 1) % n;
        if (p & PAD_L1) sel = sel > rows ? sel - rows : 0;
        if (p & PAD_R1) sel = sel + rows < n ? sel + rows : n - 1;
        if (p & PAD_CROSS) {
            if (sel == 0) { const char *d = start_disc(); if (d) return d; continue; }
            logf_("menu: starting %s\n", games[sel - 1]);
            return games[sel - 1];
        }
        if ((p & PAD_CIRCLE) && sel == 0) {   /* copy the disc to the HDD */
            char t[128], cue[512];
            if (disc_identify_result(t, sizeof t) == 1 && copy_disc(t[0] ? t : "PS1 disc", cue, sizeof cue)) {
                rescan(); check_own_settings();
                for (i = 0; i < ngames; i++) if (!strcmp(games[i], cue)) sel = i + 1;
            }
            continue;
        }
        if (p & PAD_TRIANGLE) { settings_screen(); continue; }
        if (p & PAD_SQUARE) { rescan(); check_own_settings(); sel = ngames ? 1 : 0; top = 0; disc_identify_start(); continue; }
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        snprintf(sub, sizeof sub, "%d game%s", ngames, ngames == 1 ? "" : "s");
        header(APP_TITLE, sub);
        ovl_text(MARGIN + ovl_text_width(APP_TITLE "  "), 17, "v" APP_VERSION, C_DIM);
        for (i = top; i < n && i < top + rows; i++) {
            int y = y0 + (i - top) * 44;
            if (i == sel) ovl_fill(30, y - 6, OVL_W - 60, 42, C_SEL);
            if (i == 0) {               /* the disc in the drive: icon, its game, "from disc" */
                char t[128], nm[160];
                int r = disc_identify_result(t, sizeof t), max;
                const char *tag = r == 1 ? "from disc" : "";
                disc_icon(MARGIN, y - 1, i == sel ? C_SEL : 0xff000000);
                if (r < 0) snprintf(nm, sizeof nm, "Checking the disc drive...");
                else if (r == 0) snprintf(nm, sizeof nm, "No PS1 disc in the drive");
                else if (r == 2) snprintf(nm, sizeof nm, "Disc in the drive (could not read it)");
                else snprintf(nm, sizeof nm, "%s", t[0] ? t : "PS1 disc");
                max = (OVL_W - 2 * MARGIN - 44 - ovl_text_width(tag) - ovl_text_width(" ")) / ovl_text_width("W");
                if ((int)strlen(nm) > max && max > 3) { nm[max - 3] = 0; strcat(nm, "..."); }
                ovl_text(MARGIN + 44, y, nm, r == 1 ? (i == sel ? C_ACCENT : C_TEXT) : C_DIM);
                if (tag[0]) ovl_text(OVL_W - MARGIN - ovl_text_width(tag), y, tag, C_DIM);
            } else if (own_cfg[i - 1]) {
                ovl_text(OVL_W - MARGIN - ovl_text_width("own settings"), y, "own settings", C_DIM);
                {   /* the name ends before the tag */
                    char nm[256];
                    int max = (OVL_W - 2 * MARGIN - ovl_text_width("own settings ")) / ovl_text_width("W");
                    const char *s = base_name(games[i - 1]);
                    if ((int)strlen(s) > max && max > 3) { snprintf(nm, sizeof nm, "%.*s...", max - 3, s); s = nm; }
                    ovl_text(MARGIN, y, s, i == sel ? C_ACCENT : C_TEXT);
                }
            } else text_fit(MARGIN, y, base_name(games[i - 1]), i == sel ? C_ACCENT : C_TEXT);
        }
        if (!ngames) {                  /* below the disc entry */
            ovl_text(MARGIN, 170, "No game files found.", C_TEXT);
            ovl_text(MARGIN, 220, "Put .cue/.bin, .chd, .pbp, .iso or .m3u files in one of:", C_DIM);
            for (i = 0; scan_dirs[i] && i < 6; i++) ovl_text(MARGIN + 32, 260 + i * 36, scan_dirs[i], C_TEXT);
            ovl_text(MARGIN, 490, "BIOS (optional, better compatibility):", C_DIM);
            ovl_text(MARGIN + 32, 526, APP_DIR "/system/scph5501.bin", C_TEXT);
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
        footer2(sel == 0 && disc_identify_result(NULL, 0) == 1
                    ? "X play   Circle copy to HDD   Triangle settings   Square rescan"
                    : "X play    Triangle settings    Square rescan    L1/R1 page",
                "In game: SELECT+START opens the menu (save states, settings)");
        frame_end();
    }
    return NULL;
}

/* ---------------- in-game menu ---------------- */
extern int game_save_state(int slot);
extern int game_load_state(int slot);
extern const char *game_state_info(int slot, char *out, size_t n);
extern int game_dump_vram(void);
extern void game_reset(void);
extern void game_settings_changed(void);

int menu_ingame(void)
{
    static const char *items[] = { "Resume", "Save state", "Load state", "Reset", "Settings", "Own settings for this game",
                                   "Quit to game list", "Dump VRAM (debug)" };
    int N = cfg.vram_check ? 8 : 7;
    static int slot;             /* kept while the app runs */
    int sel = 0, settings = 0, ssel = 0;
    const char *msg = NULL;
    char info[40] = "";
    logf_("menu: in-game menu opened\n");
    menu_name = "in-game";
    game_state_info(slot, info, sizeof info);
    while (!app_exit_requested) {
        uint32_t p;
        int i;
        frame_begin(!settings);          /* the settings page covers the whole screen */
        p = input_pressed();
        if (settings) {
            if (p & PAD_UP) ssel = (ssel + NSET - 1) % NSET;
            if (p & PAD_DOWN) ssel = (ssel + 1) % NSET;
            if (p & (PAD_LEFT | PAD_RIGHT | PAD_CROSS)) { setting_change(ssel, p & PAD_LEFT ? -1 : 1, 1); game_settings_changed(); }
            if (p & (PAD_CIRCLE | PAD_TRIANGLE)) { settings = 0; config_save(); }
            ovl_fill(0, 0, OVL_W, OVL_H, C_PANEL);
            header(config_game_active() ? "Settings for this game" : "Settings for all games", NULL);
            draw_settings(ssel, 84, 1);
            footer("Up/Down select    Left/Right change    Circle back");
            frame_end();
            continue;
        }
        if (p & PAD_UP) sel = (sel + N - 1) % N;
        if (p & PAD_DOWN) sel = (sel + 1) % N;
        if ((sel == 1 || sel == 2) && (p & (PAD_LEFT | PAD_RIGHT))) {
            slot = (slot + (p & PAD_LEFT ? 9 : 1)) % 10;
            game_state_info(slot, info, sizeof info);
            msg = NULL;
        }
        if (p & PAD_CIRCLE) { menu_name = "game list"; logf_("menu: resume\n"); return 0; }
        if (p & PAD_CROSS) {
            switch (sel) {
            case 0: menu_name = "game list"; logf_("menu: resume\n"); return 0;
            case 1:
                msg = game_save_state(slot) ? "State saved" : "Saving failed";
                game_state_info(slot, info, sizeof info);
                break;
            case 2: if (game_load_state(slot)) return 0; msg = "This slot is empty"; break;
            case 3: game_reset(); return 0;
            /* finish this frame (the paused menu) first: starting another one without
               presenting this one made the screen blink */
            case 4: settings = 1; ssel = 0; break;
            case 5:
                config_game_enable(!config_game_active());
                game_settings_changed();
                msg = config_game_active() ? "Changes now apply to this game only" : "This game uses the settings for all games";
                break;
            case 6: menu_name = "game list"; logf_("menu: quit to game list\n"); return 1;
            case 7: {
                static char dm[64];
                int n = game_dump_vram();
                if (n) snprintf(dm, sizeof dm, "Dump %d written to USRDIR/dumps", n);
                msg = n ? dm : "Dump failed (see the log)";
                break;
            }
            }
        }
        ovl_fill(290, 110, 700, 130 + N * 46 + (msg ? 50 : 0), C_PANEL);
        ovl_text(320, 132, "Paused", C_ACCENT);
        for (i = 0; i < N; i++) {
            int y = 180 + i * 46;
            char line[80];
            if (i == 1 || i == 2) snprintf(line, sizeof line, "%s   < slot %d >", items[i], slot);
            else if (i == 5) snprintf(line, sizeof line, "%s: %s", items[i], config_game_active() ? "On" : "Off");
            else snprintf(line, sizeof line, "%s", items[i]);
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
