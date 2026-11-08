/* Settings, stored as key=value lines in the app folder: settings.txt for all
 * games, and configs/<game>.ini for a game that has its own settings (the same
 * keys; a key missing there comes from settings.txt). Either file can be edited
 * by hand: it is read when the app starts and when the game starts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include "common.h"

#define CFG_PATH APP_DIR "/settings.txt"
#define GAME_DIR APP_DIR "/configs"

settings_t cfg = {
    .dynarec = 1, .dynarec_thread = 0, .gpu_scale = 4, .gpu_res_target = 0, .filter = FILTER_SMOOTH, .texfilter = 0, .sprite_texfilter = 0, .aspect = ASPECT_4_3, .widescreen = 0,
    .frameskip = 0, .dithering = 0, .show_fps = 1, .gpu_thread = 1, .stick_dpad = 1, .stick_buttons = 0, .analog = 1, .vibration = 1, .spu_mdec = 1, .audio_stretch = 1, .cpu_clock = 0, .skip_intro = 0, .pal60 = 0, .boot_logo = 0, .speed_limit = 0, .debug_log = 0, .screen_w = 100, .screen_h = 100, .mc1_mode = 0, .mc2_mode = 2, .vmc_source = "", .mc1_shared = "", .mc2_shared = "", .last_dir = "", .cd_speed = 0, .cheat_enable = 0, .mc_swap = 0, .theme_shape = 0, .theme_cover = 0, .theme_color = 0, .color_brightness = 0, .color_contrast = 0, .color_saturation = 0, .color_temperature = 0, .color_gamma = 0, .pgxp = 0,
};

/* game: 1 = can differ per game; 0 = for all games only (the TV, the folder) */
#define F(x) offsetof(settings_t, x)
static const struct { const char *key; size_t off; int min, max, game; } ints[] = {
    { "dynarec", F(dynarec), 0, 1, 1 },
    { "dynarec_thread", F(dynarec_thread), 0, 1, 1 },
    { "gpu_scale", F(gpu_scale), 0, 10, 1 },
    { "gpu_res_target", F(gpu_res_target), 0, 1200, 1 },
    { "filter", F(filter), 0, FILTER_COUNT - 1, 1 },
    { "texfilter", F(texfilter), 0, 2, 1 },
    { "sprite_texfilter", F(sprite_texfilter), 0, 2, 1 },
    { "aspect", F(aspect), 0, ASPECT_COUNT - 1, 1 },
    { "widescreen", F(widescreen), 0, 1, 1 },
    { "frameskip", F(frameskip), 0, 1, 1 },
    { "dithering", F(dithering), 0, 1, 1 },
    { "show_fps", F(show_fps), 0, 1, 1 },
    { "gpu_thread", F(gpu_thread), 0, 1, 1 },
    { "stick_dpad", F(stick_dpad), 0, 1, 1 },
    { "stick_buttons", F(stick_buttons), 0, 1, 1 },
    { "vram_check", F(vram_check), 0, 1, 1 },
    { "spu_mdec", F(spu_mdec), 0, 1, 1 },
    { "audio_stretch", F(audio_stretch), 0, 1, 1 },
    { "cpu_clock", F(cpu_clock), 0, 2, 1 },
    { "skip_intro", F(skip_intro), 0, 1, 1 },
    { "pal60", F(pal60), 0, 1, 1 },
    { "vibration", F(vibration), 0, 1, 1 },
    { "boot_logo", F(boot_logo), 0, 1, 1 },
    { "speed_limit", F(speed_limit), 0, 2, 1 },
    { "debug_log", F(debug_log), 0, 1, 1 },
    { "screen_w", F(screen_w), SCREEN_MIN, SCREEN_MAX, 0 },
    { "screen_h", F(screen_h), SCREEN_MIN, SCREEN_MAX, 0 },
    { "mc1_mode", F(mc1_mode), 0, 2, 1 },
    { "mc2_mode", F(mc2_mode), 0, 2, 1 },
    { "analog", F(analog), 0, 1, 1 },
    { "cd_speed", F(cd_speed), 0, 2, 1 },
    { "cheat_enable", F(cheat_enable), 0, 1, 0 },
    { "mc_swap", F(mc_swap), 0, 1, 0 },
    { "theme_shape", F(theme_shape), 0, 2, 0 },
    { "theme_cover", F(theme_cover), 0, 3, 0 },
    { "theme_color", F(theme_color), 0, 6, 0 },
    { "color_brightness", F(color_brightness), -10, 10, 1 },
    { "color_contrast", F(color_contrast), -10, 10, 1 },
    { "color_saturation", F(color_saturation), -10, 10, 1 },
    { "color_temperature", F(color_temperature), -10, 10, 1 },
    { "color_gamma", F(color_gamma), -10, 10, 1 },
    { "pgxp", F(pgxp), 0, 2, 1 },
};
#define NINTS (sizeof ints / sizeof ints[0])
#define IV(s, i) (*(int *)((char *)(s) + ints[i].off))

/* bumped when defaults change in a way that should reach existing settings files */
#define CFG_VERSION 3

/* auto resolution target (gpu_res_target): 0 = Manual, 1 = the TV's height,
   else a target height. Anything else (a hand-edited file) is Manual. */
int cfg_auto_target_h(void)
{
    int t = cfg.gpu_res_target;
    if (t == 1) { int vh = (int)video_height(); return vh > 0 ? vh : 0; }
    if (t == 480 || t == 720 || t == 960 || t == 1080 || t == 1200) return t;
    return 0;
}

/* the RSX renderer should be on: a manual scale, or a resolvable auto target */
int cfg_rsx_on(void) { return cfg.gpu_scale != 0 || cfg_auto_target_h() != 0; }

/* reads key=value lines into *s; game: only the per-game keys. Returns the file's
   version (0: no file, 1: before versions were written) */
static int read_file(const char *path, settings_t *s, int game)
{
    char line[600];
    int version = 1;
    unsigned i;
    int legacy_turbo = -1, seen_cd_speed = 0;   /* legacy "turbo_cd" key, see below */
    int seen_scale = 0;   /* gpu_scale seen in this file (for the v3 re-encoding, below) */
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl, *k = line;
        if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        *eq++ = 0;
        if ((nl = strpbrk(eq, "\r\n"))) *nl = 0;
        while (*k == ' ' || *k == '\t') k++;
        { char *e = k + strlen(k); while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0; }
        while (*eq == ' ' || *eq == '\t') eq++;
        for (i = 0; i < NINTS; i++)
            if (!strcmp(k, ints[i].key) && (!game || ints[i].game)) {
                int v = atoi(eq);
                if (v >= ints[i].min && v <= ints[i].max) {
                    IV(s, i) = v;
                    if (!strcmp(k, "gpu_scale")) seen_scale = 1;
                }
                if (!strcmp(k, "cd_speed")) seen_cd_speed = 1;
            }
        /* legacy key: "turbo_cd" (0/1) was the old Turbo CD switch; 1 is the
           old Max, now cd_speed 2. Applied only when the file has no cd_speed
           of its own, so a file that already migrated never replays it; the
           key is dropped on the next write (it is no longer in ints[]). */
        if (!strcmp(k, "turbo_cd")) legacy_turbo = atoi(eq);
        if (!game && !strcmp(k, "last_dir")) snprintf(s->last_dir, sizeof s->last_dir, "%s", eq);
        if (!game && !strcmp(k, "vmc_source")) snprintf(s->vmc_source, sizeof s->vmc_source, "%s", eq);
        if (!game && !strcmp(k, "mc1_shared")) snprintf(s->mc1_shared, sizeof s->mc1_shared, "%s", eq);
        if (!game && !strcmp(k, "mc2_shared")) snprintf(s->mc2_shared, sizeof s->mc2_shared, "%s", eq);
        if (!strcmp(k, "version")) version = atoi(eq);
    }
    if (!seen_cd_speed && (legacy_turbo == 0 || legacy_turbo == 1))
        s->cd_speed = legacy_turbo ? 2 : 0;
    /* gpu_scale re-encoding (version 3): old files store whole steps 0..5
       (5 = 4x wide, 4.5x tall); new files store half-steps 0..10 (5 = 2.5x,
       9 = 4.5x). A file older than version 3 keeps its old meaning: migrate
       what it stored, before the value is ever used. Files the new code
       writes carry version 3, so they are never migrated twice. */
    if (version < 3 && seen_scale) {
        static const int old_to_new[] = { 0, 2, 4, 6, 8, 9 };
        int v = s->gpu_scale;
        if (v >= 0 && v <= 5) s->gpu_scale = old_to_new[v];
    }
    fclose(f);
    return version;
}

static int write_file(const char *path, const settings_t *s, int game)
{
    unsigned i;
    FILE *f = fopen(path, "w");
    if (!f) { logf_("settings: cannot write %s\n", path); return 0; }
    if (game) fprintf(f, "# settings for this game only (the others come from settings.txt)\n");
    fprintf(f, "version=%d\n", CFG_VERSION);
    for (i = 0; i < NINTS; i++)
        if (!game || ints[i].game) fprintf(f, "%s=%d\n", ints[i].key, IV(s, i));
    if (!game) fprintf(f, "vmc_source=%s\n", s->vmc_source);
    if (!game) fprintf(f, "mc1_shared=%s\n", s->mc1_shared);
    if (!game) fprintf(f, "mc2_shared=%s\n", s->mc2_shared);
    if (!game) fprintf(f, "last_dir=%s\n", s->last_dir);
    fclose(f);
    return 1;
}

void config_load(void)
{
    int v = read_file(CFG_PATH, &cfg, 0);
    if (v == 1) {
        /* new defaults: RSX 2x, smoothing on the SPUs, FPS shown, frame skip auto
           (software renderer only), dithering off */
        cfg.gpu_scale = 4; cfg.filter = FILTER_FXAA_SPU; cfg.show_fps = 1; cfg.frameskip = 1; cfg.dithering = 0;
        logf_("settings: updated to the new defaults (RSX 2x, SPU smoothing, FPS, frame skip auto, no dithering)\n");
    }
}

/* ---- per-game settings ---- */
static settings_t global_cfg;          /* the settings for all games while one has its own */
static int game_on, in_game;           /* game_on: the running game uses configs/<game>.ini */
static char game_path[700];

void config_game_path(const char *key, char *out, size_t n) { snprintf(out, n, GAME_DIR "/%s.ini", key); }

int config_game_exists(const char *key)
{
    char p[700];
    struct stat st;
    config_game_path(key, p, sizeof p);
    return stat(p, &st) == 0;
}

int config_game_active(void) { return game_on; }

int config_game_delete(const char *key)
{
    char p[700];
    if (!key || !key[0]) return 0;
    config_game_path(key, p, sizeof p);
    if (remove(p) != 0) return 0;
    logf_("settings: deleted %s\n", p);
    return 1;
}

void config_game_begin(const char *key)
{
    in_game = 1;
    global_cfg = cfg;
    config_game_path(key, game_path, sizeof game_path);
    game_on = read_file(game_path, &cfg, 1) > 0;
    if (game_on) logf_("settings: this game's own settings from %s\n", game_path);
    gte_widescreen = cfg.widescreen;   /* the game's own widescreen, or the global one */
    cdr_speed_up = cfg.cd_speed == 1 ? 2 : 1;   /* the game's own CD speed, or the global one */
}

void config_game_end(void)
{
    if (game_on) {
        int w = cfg.screen_w, h = cfg.screen_h;
        cfg = global_cfg;
        cfg.screen_w = w; cfg.screen_h = h;     /* for all games: kept */
    }
    game_on = in_game = 0;
    gte_widescreen = cfg.widescreen;   /* back to the settings for all games */
    cdr_speed_up = cfg.cd_speed == 1 ? 2 : 1;   /* back to the CD speed for all games */
}

void config_save(void)
{
    WHERE("saving settings");
    if (game_on) {
        /* the game's file gets the per-game keys; the keys for all games go to
           settings.txt, with the other values as they were for all games */
        unsigned i;
        for (i = 0; i < NINTS; i++) if (!ints[i].game) IV(&global_cfg, i) = IV(&cfg, i);
        snprintf(global_cfg.last_dir, sizeof global_cfg.last_dir, "%s", cfg.last_dir);
        snprintf(global_cfg.mc1_shared, sizeof global_cfg.mc1_shared, "%s", cfg.mc1_shared);
        snprintf(global_cfg.mc2_shared, sizeof global_cfg.mc2_shared, "%s", cfg.mc2_shared);
        write_file(game_path, &cfg, 1);
        write_file(CFG_PATH, &global_cfg, 0);
    } else write_file(CFG_PATH, &cfg, 0);
    logf_("settings saved%s (cpu %s, renderer %gx, auto %d, filter %d, aspect %d, frameskip %d, dither %d, fps %d, gpu thread %d)\n",
          game_on ? " for this game" : "", cfg.dynarec ? "dynarec" : "interp", (double)cfg.gpu_scale / 2.0, cfg.gpu_res_target, cfg.filter, cfg.aspect,
          cfg.frameskip, cfg.dithering, cfg.show_fps, cfg.gpu_thread);
}

/* ---- shared settings panel (menu.c): file helpers ----
   The panel edits cfg directly (in a game the changes apply live, as the old
   inline page did). These helpers only write files and never restore cfg or
   touch in_game. One exception, in config_panel_apply_title: a title-apply
   that successfully writes the .ini in a game sets game_on=1 (see below),
   so quit-time routing matches the file's existence. The panel keeps its own baseline (the
   settings for all games at entry, updated by every apply and by back-save),
   so back can save to the right target without disturbing launch/quit state.
   On any write failure the helper returns 0 and the baseline is left
   untouched, so the panel can report "Save failed" and the user can retry.
   A title-apply whose .ini write succeeded but whose settings.txt write
   failed leaves the new .ini in place; the next apply or back-save rewrites
   both files from the same cfg, so the partial state self-heals. */
int config_panel_overlay(const char *key, settings_t *s)
{
    char p[700];
    if (!key || !key[0] || !s) return 0;
    config_game_path(key, p, sizeof p);
    return read_file(p, s, 1) > 0;
}

void config_panel_global(settings_t *out) { if (out) *out = global_cfg; }

int config_panel_apply_title(const char *key, const settings_t *now, settings_t *baseline, int ingame)
{
    char p[700];
    settings_t merged;
    unsigned i;
    if (!key || !key[0] || !now || !baseline) return 0;
    mkdir(GAME_DIR, 0777);
    config_game_path(key, p, sizeof p);
    if (!write_file(p, now, 1)) return 0;   /* the title's file: per-game keys only */
    /* The title now HAS a personal file, so the running game uses it: set
       game_on, mirroring the deleted config_game_enable ON path (which did
       global_cfg = cfg; game_on = write_file(...)). The panel's title-apply
       already sets in-game global_cfg to the merged copy above, so no
       wholesale re-snapshot is needed here. Without this, a title that
       gains its .ini mid-session would quit with game_on=0, and the next
       config_save would write its per-game values to settings.txt (global
       pollution). Only on successful write: a failed write leaves game_on
       unchanged, so the flag never claims a file that does not exist.
       Home panels (ingame=0) never touch game_on: no game runs. */
    if (ingame) game_on = 1;
    /* settings.txt gets the settings for all games: the baseline's per-game
       keys, the panel's global-only keys and strings (the config_save split,
       plus vmc_source, which config_save omits because the import that sets
       it never runs in a game; copying it here is a no-op in the panel). */
    merged = *baseline;
    for (i = 0; i < NINTS; i++) if (!ints[i].game) IV(&merged, i) = IV(now, i);
    snprintf(merged.last_dir, sizeof merged.last_dir, "%s", now->last_dir);
    snprintf(merged.vmc_source, sizeof merged.vmc_source, "%s", now->vmc_source);
    snprintf(merged.mc1_shared, sizeof merged.mc1_shared, "%s", now->mc1_shared);
    snprintf(merged.mc2_shared, sizeof merged.mc2_shared, "%s", now->mc2_shared);
    if (!write_file(CFG_PATH, &merged, 0)) return 0;
    *baseline = merged;
    if (ingame) global_cfg = merged;
    logf_("settings saved for this title (%s)\n", key);
    return 1;
}

int config_panel_apply_global(const settings_t *now, settings_t *baseline, int ingame)
{
    if (!now || !baseline) return 0;
    if (!write_file(CFG_PATH, now, 0)) return 0;   /* the title's file is untouched */
    *baseline = *now;
    if (ingame) global_cfg = *now;
    logf_("settings saved for all games\n");
    return 1;
}
