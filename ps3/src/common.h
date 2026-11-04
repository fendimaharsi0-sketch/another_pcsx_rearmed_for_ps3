/* PS3 front-end for the PCSX-ReARMed libretro core: shared declarations. */
#ifndef PS1PS3_COMMON_H
#define PS1PS3_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <ppu-types.h>

#define APP_ID    "PCSR00001"
#define APP_TITLE "PCSX-ReARMed for PS3"
#define APP_VERSION "0.1 (build 72)"   /* logs only; not shown in the game list (version lives on the release page) */
#ifndef APP_DIR   /* overridable for the host UI tests */
#define APP_DIR   "/dev_hdd0/game/" APP_ID "/USRDIR"
#endif

/* ---- log.c ---- */
void log_open(void);
void log_ready(void);                    /* settings read: files follow the Debug log setting */
void log_game_begin(const char *name);   /* per-game log file until log_game_end() */
void log_game_end(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* what the main thread is doing, reported by the watchdog when frames stop */
extern volatile const char *app_where;
#define WHERE(s) (app_where = (s))
extern volatile const char *app_sub;   /* finer step inside it (renderer, SPU code) */
#define SUBWHERE(s) (app_sub = (s))
/* hang reports: a game is running; calls of the core's callbacks (video, audio,
   input, environment), so the watchdog sees whether the core still progresses */
extern volatile int app_in_game;
extern volatile unsigned app_cb_counts[4];
void log_sync(void);   /* log lines on the disk now (before risky steps) */
void watchdog_start(void);
void watchdog_frame(void);      /* called once per presented frame */

/* ---- config.c ---- */
enum { FILTER_SHARP = 0, FILTER_SMOOTH, FILTER_FXAA, FILTER_FXAA_SPU, FILTER_COUNT };
/* picture shape on the TV; the zoom modes keep the 4:3 proportions and crop the
   top and bottom instead (values are stored in settings.txt: append only) */
enum { ASPECT_4_3 = 0, ASPECT_FULL, ASPECT_14_9, ASPECT_ZOOM_14_9, ASPECT_ZOOM, ASPECT_COUNT };
typedef struct {
    int dynarec;        /* 1 = Lightrec, 0 = interpreter */
    int dynarec_thread; /* Lightrec compiles new code on the PPU's second thread */
    int gpu_scale;      /* 0 = software renderer, 2..4 = RSX renderer at that scale, 5 = 4x wide, 4.5x tall */
    int filter;         /* FILTER_* */
    int texfilter;      /* RSX texture filter: 0 = nearest (default), 1 = bilinear (upscaled pass only), 2 = bilinear (performance: hardware 15-bit, manual CLUT) */
    int sprite_texfilter; /* RSX sprite filter: same 0/1/2 values as texfilter, default nearest; applies to textured GP0 sprites (do_rect) and to textured polygons whose UV mapping is axis-aligned (U only with X, V only with Y, like DuckStation); other polygons keep texfilter */
    int aspect;         /* ASPECT_* */
    int widescreen;     /* GTE widescreen 16:9: 0 off (default), 1 on — RTPS/RTPT scale the projected X offset 3/4 (see gte_widescreen); pair with Aspect = Stretch to screen on a 16:9 TV */
    int frameskip;      /* 0 off, 1 auto */
    int dithering;
    int show_fps;
    int gpu_thread;     /* software renderer on the second PPU thread */
    int stick_dpad;     /* left stick also presses the D-pad */
    int stick_buttons;  /* right stick presses Triangle / X / Square / Circle */
    int analog;         /* DualShock analog sticks on (else a digital pad) */
    int vram_check;     /* debug: software renderer draws too, for VRAM dumps */
    int spu_mdec;       /* FMV (MDEC) decoding on the SPUs */
    int audio_stretch;  /* sound slows down with the game instead of crackling */
    int cpu_clock;      /* PS1 CPU speed: 0 normal, 1 +50%, 2 +75% */
    int debug_log;      /* extra logging: core debug messages, emulator state in hang and 10 s reports */
    int skip_intro;     /* fast-forward FMVs that start in a game's first 3 minutes */
    int vibration;      /* DualShock vibration */
    int boot_logo;      /* the BIOS shows the PlayStation logo at game start (with a real BIOS) */
    int pal60;          /* European (PAL) games run with NTSC timing, at 60 Hz (from the game start) */
    int speed_limit;    /* 0 normal (catches up after a hitch), 1 strict (never faster), 2 off (as fast as possible) */
    int screen_w;       /* screen adjustment: width and height of the whole */
    int screen_h;       /* picture (game and menus) in % of the TV signal */
    int mc1_mode;       /* memory card 1: 0 = Auto (the game's own serial card), 1 = Shared (chosen imported card, or the default shared card), 2 = None */
    int mc2_mode;       /* memory card 2: same values, default None (slot 2 was always off) */
    char vmc_source[128]; /* base name of the PS3 VMC last imported into the library (global only, display only) */
    char mc1_shared[256]; /* library card file name for slot 1 Shared (saves/imported/<name>), global only; empty = default pcsx-card1.mcd */
    char mc2_shared[256]; /* same for slot 2 Shared (default pcsx-card2.mcd) */
    char last_dir[512];
    int turbo_cd;       /* upstream TurboCD: instant CD seeks + faster reads; auto-off during XA audio and FMV */
    int cheat_enable;   /* global-only master gate for cheats (default off): the Cheats menu in the pause menu only appears when this is on, and no cheat is applied while it is off */
    int pgxp_enable;    /* global-only master gate for PGXP slice 1 (default off): the sub-options below only take effect when this is on */
    int pgxp_texture;   /* PGXP texture correction: perspective-correct UV in the RSX renderer (default on, per game) */
    int pgxp_culling;   /* PGXP culling correction: NCLIP recomputed from precise GTE positions (default on, per game) */
} settings_t;
extern settings_t cfg;
extern int gte_widescreen;   /* core, libpcsxcore/gte.c: 1 = GTE widescreen 16:9, mirrored from cfg.widescreen */
extern int gte_pgxp;   /* core, libpcsxcore/pgxp.c: bitmask (1 = texture, 2 = culling), mirrored from cfg by config_mirror_gte_pgxp() */
extern int pgxp_track;   /* T1: 1 when the PGXP master switch is on, mirrored from cfg */
void config_mirror_gte_pgxp(void);   /* recompute gte_pgxp from cfg.pgxp_enable/texture/culling */
void lightrec_pgxp_gate(int on);   /* JIT gate, libpcsxcore/lightrec/plugin.c: on a master change, re-point cop2_notify in the state's copied ops + flush all blocks (skipped when Lightrec is not running) */
void pgxp_next_frame(void);   /* core, libpcsxcore/pgxp.c: invalidate the GTE vertex cache (new frame) */
void config_load(void);
void config_save(void);
/* per-game settings: configs/<game>.ini (game = ROM file name without extension) */
void config_game_begin(const char *game);   /* at game start: its file, if any, applies */
void config_game_end(void);                 /* back to the settings for all games */
int  config_game_active(void);              /* the running game uses its own settings */
void config_game_enable(int on);            /* in game: give it its own settings / remove them */
int  config_game_exists(const char *game);
#define SCREEN_MIN 80
#define SCREEN_MAX 130

/* ---- video.c ---- */
int  video_init(void);
void video_shutdown(void);
unsigned video_width(void);
unsigned video_height(void);
/* buffer the core may render the next frame into (mapped for the RSX) */
void *video_frame_buffer(size_t *pitch);
/* begins a display frame: waits until the back buffer may be drawn again */
void video_begin(void);
void video_clear(uint32_t argb);
/* draws a software frame (XRGB8888 in main memory) to the 4:3 or full area */
void video_draw_frame(const void *pix, unsigned w, unsigned h, size_t pitch);
void video_clear_frame_buffer(void);
/* draws a texture already in RSX local memory (used by the GPU renderer) */
void video_draw_rsx_texture(uint32_t offset, unsigned tex_w, unsigned tex_h, size_t pitch,
                            float u0, float v0, float u1, float v1);
/* PS1 dither pattern on the RSX renderer's picture: native_w/h = PS1 pixels
   across the whole texture passed to video_draw_rsx_texture */
void video_set_dither(int on, float native_w, float native_h);
void video_redraw_quick(int on);   /* menus: redraw the game picture without the smoothing passes */
/* CPU-drawn overlay (menu, messages): 1280x720 ARGB, alpha blended on top */
uint32_t *video_overlay(void);
void video_overlay_clear(void);
void video_overlay_clear_rows(int y0, int y1);
unsigned video_overlay_generation(void);
void video_draw_overlay(void);
/* draws the overlay, flushing CPU caches only for rows y0..y1 (the rest unchanged) */
void video_draw_overlay_rows(int y0, int y1);
int  video_overlay_border(void);   /* black border left of the game picture, overlay pixels */
void video_borders_dirty(void);    /* clear the borders again (something was drawn there) */
void video_present(void);
void video_flush_cpu_writes(const void *p, size_t n);
/* RSX state shared with the GPU renderer backend */
struct _gcmCtxData;
extern struct _gcmCtxData *rsx_ctx;
void video_reset_state(void);

/* ---- draw.c (overlay text and boxes) ---- */
#define OVL_W 1280
#define OVL_H 720
void ovl_fill(int x, int y, int w, int h, uint32_t argb);
int  ovl_text(int x, int y, const char *s, uint32_t argb);   /* returns end x */
int  ovl_text_width(const char *s);
/* An RGBA image scaled to w x h at x, y (nearest sampling, alpha blended);
   used for the game list's cover panel. No image asset is bundled: the
   image comes from the user's cover pack at run time. */
void ovl_blit(int x, int y, int w, int h, const unsigned char *px, int sw, int sh);
/* PS face buttons, drawn procedurally at run time (no asset file, pkg +0):
   a 24x24 outline glyph, 2 px stroke (1 px flickers at 480i on a CRT).
   Monochrome: every button uses the caller's colour, so glyphs match the
   text they sit next to. */
#define GLYPH_CROSS 0
#define GLYPH_CIRCLE 1
#define GLYPH_TRIANGLE 2
#define GLYPH_SQUARE 3
void ovl_line(int x0, int y0, int x1, int y1, uint32_t argb);   /* 2 px thick */
void ovl_circle(int cx, int cy, int r, uint32_t argb);         /* 2 px outline */
void ovl_button(int x, int y, int btn, uint32_t argb);         /* 24x24 glyph */
void ovl_round_fill(int x, int y, int w, int h, int r, uint32_t argb);   /* filled rounded rect */

/* ---- audio.c ---- */
int  audio_init(void);
void audio_shutdown(void);
void audio_push(const int16_t *stereo, size_t frames);
void audio_set_input_rate(double hz);
void audio_pause(int paused);
void audio_set_stretch(int on);          /* sound slows down with the game */
unsigned audio_fill(void);   /* buffered frames, for diagnostics */
unsigned audio_underruns(void); /* blocks played empty since the last call */
unsigned audio_underruns_total(void); /* blocks played empty since the start */
extern unsigned video_vp_epoch;   /* counts vertex program loads by video.c */
void hooks_gpu_scale(int *sx, float *sy);   /* scale the RSX renderer draws at now (0: off) */

/* ---- input.c ---- */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_CROSS = 1 << 4, PAD_CIRCLE = 1 << 5, PAD_SQUARE = 1 << 6, PAD_TRIANGLE = 1 << 7,
    PAD_L1 = 1 << 8, PAD_R1 = 1 << 9, PAD_L2 = 1 << 10, PAD_R2 = 1 << 11,
    PAD_L3 = 1 << 12, PAD_R3 = 1 << 13, PAD_START = 1 << 14, PAD_SELECT = 1 << 15,
};
typedef struct { uint32_t buttons; uint8_t lx, ly, rx, ry; int connected; } pad_state_t;
void input_init(void);
void input_poll(void);
void input_rumble(int port, int large, unsigned strength);   /* from the core (0-65535) */
void input_rumble_stop(void);
const pad_state_t *input_pad(int port);
uint32_t input_pressed(void);   /* newly pressed buttons on any pad (menu navigation) */
const char *input_names(uint32_t buttons, char *out, size_t n);

/* ---- menu.c ---- */
/* runs the game list / settings; returns the path of the game to start, or NULL to quit */
const char *menu_run(void);
/* in-game menu (SELECT+START); returns 1 to return to the game list */
int menu_ingame(void);

/* ---- main.c ---- */
extern volatile int app_exit_requested;
void log_memory(const char *when);
void app_poll_system(void);
const char *app_game_name(void);   /* ROM file name without extension ("BIOS" for the BIOS menu) */
int app_game_is_bios(void);        /* the running "game" is the BIOS menu, not a disc/image */
/* ---- cheats (main.c): feeding the core's dormant cheat engine ---- */
void cheat_apply_saved(int include_manual);  /* reset + feed enabled codes from the state file; gate, BIOS and serial checked inside */
void cheat_reset_all(void);                  /* clear all fed codes + pending manual disables */
void cheat_post_frame(void);                 /* disable manual codes after their first emulated frame */
int cheat_enabled_count(void);               /* codes currently fed to the engine */
/* Disk control (stored from env_cb, ext preferred, base as fallback):
   the Swap disc picker reads the playlist through these and swaps with
   one eject -> change -> close cycle per call. */
int disk_iface_available(void);              /* the core delivered an interface */
unsigned disk_iface_num_images(void);        /* discs in the loaded playlist (1 for a single image) */
unsigned disk_iface_current_index(void);     /* index of the disc in the drive */
int disk_iface_label(unsigned i, char *buf, size_t len);  /* 0 ok, -1 unavailable */
int disk_iface_path(unsigned i, char *buf, size_t len);   /* 0 ok, -1 unavailable */
int disk_iface_swap_to_index(unsigned i);    /* swap to a playlist disc; 0 ok */
int disk_iface_swap_to_file(const char *path);  /* replace the drive's disc with an image file; 0 ok */

/* spu_mdec.c: FMV decoding on the SPUs */
int  spu_mdec_init(int nspu);              /* starts the SPU threads + self-test; 1 = usable */
void spu_mdec_enable(int on);              /* installs / removes the core hooks */
void spu_mdec_shutdown(void);
int  spu_mdec_active(void);
void spu_mdec_stats(unsigned *streams, unsigned *mbs, unsigned *misses, double *wait_ms);   /* and resets */
int  spu_aa_available(void);               /* SPU anti-aliasing of the displayed picture */
int  spu_aa_start(uint64_t in_ea, uint64_t out_ea, uint64_t label_ea, uint32_t label_value,
                  unsigned w, unsigned h, unsigned pitch);
int  spu_aa_wait(unsigned max_us);         /* 1 done, 0 not yet, -1 an SPU gave up */
void spu_aa_stats(unsigned *frames, unsigned *late, double *wait_ms);   /* and resets */

#endif
