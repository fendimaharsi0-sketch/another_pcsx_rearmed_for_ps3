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
    .dynarec = 1, .dynarec_thread = 0, .gpu_scale = 2, .filter = FILTER_FXAA_SPU, .texfilter = 0, .sprite_texfilter = 0, .aspect = ASPECT_4_3, .widescreen = 0,
    .frameskip = 1, .dithering = 0, .show_fps = 1, .gpu_thread = 1, .stick_dpad = 1, .stick_buttons = 0, .analog = 1, .vibration = 1, .spu_mdec = 1, .audio_stretch = 1, .cpu_clock = 0, .skip_intro = 0, .pal60 = 0, .boot_logo = 0, .speed_limit = 0, .debug_log = 0, .screen_w = 100, .screen_h = 100, .mc1_mode = 0, .mc2_mode = 2, .vmc_source = "", .mc1_shared = "", .mc2_shared = "", .last_dir = "", .turbo_cd = 0, .cheat_enable = 0,
};

/* game: 1 = can differ per game; 0 = for all games only (the TV, the folder) */
#define F(x) offsetof(settings_t, x)
static const struct { const char *key; size_t off; int min, max, game; } ints[] = {
    { "dynarec", F(dynarec), 0, 1, 1 },
    { "dynarec_thread", F(dynarec_thread), 0, 1, 1 },
    { "gpu_scale", F(gpu_scale), 0, 5, 1 },
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
    { "turbo_cd", F(turbo_cd), 0, 1, 1 },
    { "cheat_enable", F(cheat_enable), 0, 1, 0 },
};
#define NINTS (sizeof ints / sizeof ints[0])
#define IV(s, i) (*(int *)((char *)(s) + ints[i].off))

/* bumped when defaults change in a way that should reach existing settings files */
#define CFG_VERSION 2

/* reads key=value lines into *s; game: only the per-game keys. Returns the file's
   version (0: no file, 1: before versions were written) */
static int read_file(const char *path, settings_t *s, int game)
{
    char line[600];
    int version = 1;
    unsigned i;
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
                if (v >= ints[i].min && v <= ints[i].max) IV(s, i) = v;
            }
        if (!game && !strcmp(k, "last_dir")) snprintf(s->last_dir, sizeof s->last_dir, "%s", eq);
        if (!game && !strcmp(k, "vmc_source")) snprintf(s->vmc_source, sizeof s->vmc_source, "%s", eq);
        if (!game && !strcmp(k, "mc1_shared")) snprintf(s->mc1_shared, sizeof s->mc1_shared, "%s", eq);
        if (!game && !strcmp(k, "mc2_shared")) snprintf(s->mc2_shared, sizeof s->mc2_shared, "%s", eq);
        if (!strcmp(k, "version")) version = atoi(eq);
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
        cfg.gpu_scale = 2; cfg.filter = FILTER_FXAA_SPU; cfg.show_fps = 1; cfg.frameskip = 1; cfg.dithering = 0;
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

void config_game_begin(const char *key)
{
    in_game = 1;
    global_cfg = cfg;
    config_game_path(key, game_path, sizeof game_path);
    game_on = read_file(game_path, &cfg, 1) > 0;
    if (game_on) logf_("settings: this game's own settings from %s\n", game_path);
    gte_widescreen = cfg.widescreen;   /* the game's own widescreen, or the global one */
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
}

/* in a game: on = from now on this game has its own settings (the current ones);
   off = its file is removed and the settings for all games apply again */
void config_game_enable(int on)
{
    if (!in_game || on == game_on) return;
    if (on) {
        mkdir(GAME_DIR, 0777);
        global_cfg = cfg;
        game_on = write_file(game_path, &cfg, 1);
        if (game_on) logf_("settings: this game now has its own settings (%s)\n", game_path);
    } else {
        int w = cfg.screen_w, h = cfg.screen_h;
        remove(game_path);
        cfg = global_cfg;
        cfg.screen_w = w; cfg.screen_h = h;
        game_on = 0;
        logf_("settings: this game's own settings removed, the settings for all games apply\n");
    }
    gte_widescreen = cfg.widescreen;   /* per-game on/off switches the active widescreen */
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
    logf_("settings saved%s (cpu %s, renderer %d, filter %d, aspect %d, frameskip %d, dither %d, fps %d, gpu thread %d)\n",
          game_on ? " for this game" : "", cfg.dynarec ? "dynarec" : "interp", cfg.gpu_scale, cfg.filter, cfg.aspect,
          cfg.frameskip, cfg.dithering, cfg.show_fps, cfg.gpu_thread);
}
