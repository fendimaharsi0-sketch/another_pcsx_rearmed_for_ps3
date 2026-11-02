/* PS3 front-end for the PCSX-ReARMed libretro core: start-up, the libretro
 * callbacks, and the frame loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <ppu-lv2.h>
#include "libretro.h"
#include "common.h"
#include "core_hooks.h"
#include "gpu_hooks.h"

/* lv2 gives the primary thread 64 KiB of stack whatever this asks for (see
   main() at the end: the emulator runs on its own thread with a large stack) */
SYS_PROCESS_PARAM(1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);

volatile int app_exit_requested;

/* sys_memory_get_user_memory_size (lv2 syscall 352) */
void log_memory(const char *when)
{
    struct { u32 total, avail; } mi __attribute__((aligned(8)));
    lv2syscall1(352, (u64)(uintptr_t)&mi);
    logf_("memory (%s): %u KiB total, %u KiB free\n", when, mi.total / 1024, mi.avail / 1024);
}
static int input_blocked;       /* ignore pads until all buttons are released (after menus) */
static char game_name[256];     /* ROM file name without extension, for save states */
static int game_is_disc;        /* the game runs from the disc in the console's drive */
const char *app_game_name(void) { return game_name; }

/* ---------------- system events (PS button menu, quit from the XMB) ---------------- */
static void sysutil_cb(u64 status, u64 param, void *user)
{
    (void)param; (void)user;
    if (status == SYSUTIL_EXIT_GAME) app_exit_requested = 1;
}

void app_poll_system(void) { sysUtilCheckCallback(); }

/* ---------------- libretro callbacks ---------------- */
static struct {
    const void *data; unsigned w, h; size_t pitch;
    int fresh;                  /* a new frame arrived during this retro_run */
    int gpu;                    /* the frame is in the GPU renderer's target */
} frame;
static double core_fps = 59.94, core_rate = 44100.0;
static int vars_dirty;
static retro_audio_buffer_status_callback_t audio_status_cb;

static void core_log(enum retro_log_level lvl, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (lvl == RETRO_LOG_DEBUG && !cfg.debug_log) return;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    /* our frame buffer pitch is fixed at 4096 bytes, so the core warns about it
       on every frame; that is expected and would flood the log */
    if (strstr(buf, "unusual pitch")) return;
    logf_("core: %s%s", buf, buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
}

extern int hwr_boot_hold;        /* hwr/hwr_rsx.c */
extern int CdromExeRead;         /* core, libpcsxcore/misc.c */
/* Slot 1 Shared card, as the core reads it: the library file name from
   settings, or "" when none is chosen (the core then falls back to the
   default shared card). Sanitized here AND in the core: a name from a
   hand-edited settings.txt must never escape saves/imported/ (no slash,
   no leading dot) nor overflow the core's 256-byte card path. */
static const char *shared_card_value_for(const char *s)
{
    if (!s[0] || s[0] == '.' || strchr(s, '/') || strchr(s, '\\')) return "";
    if (strlen(APP_DIR "/saves/imported/") + strlen(s) + 1 > 256) return "";
    return s;
}
extern unsigned int lightrec_poll_skips __attribute__((weak)), lightrec_poll_addr __attribute__((weak));   /* core, lightrec/plugin.c (dynarec builds) */
static unsigned boot_frames;
static const char *option_value(const char *key)
{
    static char fsk[8];
    if (!strcmp(key, "pcsx_rearmed_drc")) return cfg.dynarec ? "enabled" : "disabled";
    if (!strcmp(key, "pcsx_rearmed_drc_thread")) return "disabled";
    if (!strcmp(key, "pcsx_rearmed_bios")) return "auto";
    /* the logo needs a real BIOS (the HLE one cannot boot the disc itself) */
    if (!strcmp(key, "pcsx_rearmed_show_bios_bootlogo")) return cfg.boot_logo && hooks_bios_present() ? "enabled" : "disabled";
    if (!strcmp(key, "pcsx_rearmed_region")) return cfg.pal60 ? "NTSC" : "auto";   /* NTSC timing: 60 Hz */
    /* sectors read ahead on a thread: from the console's drive, and from disc images
       too (upstream's default is 12): read on the emulation thread, a seek on the
       hard drive (Silent Hill starting a voice line) held up the frame */
    if (!strcmp(key, "pcsx_rearmed_cd_readahead")) return game_is_disc ? "64" : "32";
    if (!strcmp(key, "pcsx_rearmed_cd_turbo")) return cfg.turbo_cd ? "enabled" : "disabled";   /* upstream TurboCD, already in the core: the core's update_variables reads this on vars_dirty, so it applies on resume in a game */
    if (!strcmp(key, "pcsx_rearmed_memcard1")) return cfg.mc1_mode == 1 ? "shared" : cfg.mc1_mode == 2 ? "none" : "serial";   /* serial: SCUS-12345_1.mcd in saves/; shared: the chosen library card (saves/imported/), or pcsx-card1.mcd when none is chosen */
    if (!strcmp(key, "pcsx_rearmed_memcard1_shared")) return shared_card_value_for(cfg.mc1_shared);
    if (!strcmp(key, "pcsx_rearmed_memcard2_shared")) return shared_card_value_for(cfg.mc2_shared);
    if (!strcmp(key, "pcsx_rearmed_memcard2")) return cfg.mc2_mode == 1 ? "shared" : cfg.mc2_mode == 2 ? "none" : "serial";   /* default None: slot 2 was always off */
    if (!strcmp(key, "pcsx_rearmed_rgb32_output")) return "enabled";  /* XRGB8888, what the RSX path takes */
    if (!strcmp(key, "pcsx_rearmed_dithering")) return cfg.dithering ? "enabled" : "disabled";
    /* skipped frames never reach the RSX renderer, which then misses drawing that
       later frames texture from: frame skip is for the software renderer only */
    if (!strcmp(key, "pcsx_rearmed_frameskip_type")) return cfg.frameskip && !cfg.gpu_scale ? "auto" : "disabled";
    if (!strcmp(key, "pcsx_rearmed_psxclock")) return cfg.cpu_clock == 2 ? "100" : cfg.cpu_clock == 1 ? "85" : "auto";
    if (!strcmp(key, "pcsx_rearmed_frameskip_threshold")) { snprintf(fsk, sizeof fsk, "33"); return fsk; }
    if (!strcmp(key, "pcsx_rearmed_display_fps_v2")) return "disabled";
    if (!strcmp(key, "pcsx_rearmed_spu_interpolation")) return "simple";
    if (!strcmp(key, "pcsx_rearmed_spu_reverb")) return "enabled";
    /* the GPU renderer's hooks run wherever the software renderer runs: keep it
       on this thread while the RSX renderer is on */
    if (!strcmp(key, "pcsx_rearmed_gpu_thread_rendering"))
        return cfg.gpu_thread && !cfg.gpu_scale ? "enabled" : "disabled";
    if (!strcmp(key, "pcsx_rearmed_show_overscan")) return "disabled";
    if (!strcmp(key, "pcsx_rearmed_screen_centering")) return "auto";
    if (!strcmp(key, "pcsx_rearmed_vibration")) return "enabled";   /* the Vibration setting: input.c */
    if (!strcmp(key, "pcsx_rearmed_analog_combo")) return "disabled";   /* no stick-click combo: L3+R3 is a real move in Ape Escape (crawl + hide) and the combo would eat it; the quick-menu Analog toggle is the only switch */
    return NULL;
}

static bool rumble_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    input_rumble((int)port, effect == RETRO_RUMBLE_STRONG, strength);
    return true;
}

static bool env_cb(unsigned cmd, void *data)
{
    app_cb_counts[3]++;
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = APP_DIR "/system";
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = APP_DIR "/saves";
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = option_value(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = vars_dirty;
        vars_dirty = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
        struct retro_framebuffer *fb = data;
        size_t pitch;
        if (fb->width > 1024 || fb->height > 512) return false;
        fb->data = video_frame_buffer(&pitch);
        fb->pitch = pitch;
        fb->format = RETRO_PIXEL_FORMAT_XRGB8888;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        const struct retro_audio_buffer_status_callback *c = data;
        audio_status_cb = c ? c->callback : NULL;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        core_fps = av->timing.fps; core_rate = av->timing.sample_rate;
        audio_set_input_rate(core_rate);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_cb;
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    app_cb_counts[0]++;
    if (!data) return;                      /* duplicate: keep showing the last frame */
    if (data == RETRO_HW_FRAME_BUFFER_VALID) return;
    frame.data = data; frame.w = w; frame.h = h; frame.pitch = pitch;
    frame.fresh = 1; frame.gpu = 0;
}

/* fast-forwarding an intro video: its sound is dropped */
static int audio_mute;
static size_t audio_batch_cb(const int16_t *d, size_t n) { app_cb_counts[1]++; if (!audio_mute) audio_push(d, n); return n; }
static void audio_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; if (!audio_mute) audio_push(s, 1); }

/* Skip intro videos: an FMV (MDEC output DMAs, counted by the core) that starts in
   the first 3 minutes of emulated time is skipped: once it has played for 2/3 of a
   second with frames still coming, Start and X are tapped in turn (pad 1, 3 frames
   pressed, 9 released), as a player skips it, twice each at most (a video playing
   behind a menu must not get endless presses); until it ends it is also
   fast-forwarded, for the videos a game does not let you skip. The taps stop as soon
   as no frame came for 6 emulated frames, so they do not reach the screen after the
   video. A still picture decoded with the MDEC (a menu background) is a single
   burst: no taps. */
extern unsigned int mdec_dma1_count;
static uint32_t skip_buttons;         /* pad 1 buttons pressed to skip an intro video */
void psx_debug_state(char *b, int n);     /* core, libpcsxcore/misc.c (debug log) */
#define INTRO_FRAMES (3 * 60 * 60)
static struct {
    unsigned last_count, quiet, runs; int active, skipping;
    unsigned tap, busy;           /* skipping: tap phase; frames with MDEC output in this video */
    /* the video being played, for its log line */
    unsigned v_runs, v_slow, v_mdec0, v_dry0; u64 v_us, v_max;
} fmv;

static void fmv_track(u64 us)        /* after each retro_run (us: its time, 0 = not timed) */
{
    fmv.runs++;
    if (fmv.active) {
        fmv.v_runs++; fmv.v_us += us;
        if (us > fmv.v_max) fmv.v_max = us;
        if (us > 20000) fmv.v_slow++;
    }
    if (mdec_dma1_count != fmv.last_count) { fmv.last_count = mdec_dma1_count; fmv.quiet = 0; fmv.busy++; }
    else if (fmv.quiet < 1000) fmv.quiet++;
    /* the buttons pressed for the next frame (see above) */
    skip_buttons = 0;
    if (fmv.skipping && fmv.v_runs >= 40 && fmv.busy >= 10 && fmv.quiet < 6 && fmv.tap < 48) {
        unsigned ph = fmv.tap++ % 24;
        skip_buttons = ph < 3 ? PAD_START : ph >= 12 && ph < 15 ? PAD_CROSS : 0;
    }
    /* still playing while frames keep coming within half a second (15 fps videos,
       loading pauses between parts) */
    if (fmv.quiet < 30) {
        if (!fmv.active && fmv.runs < INTRO_FRAMES && cfg.skip_intro) {
            fmv.skipping = 1;
            fmv.tap = 0;
            logf_("intro video: skipping (emulated time %.1f s)\n", fmv.runs / 60.0);
        }
        if (!fmv.active) {
            fmv.v_runs = fmv.v_slow = 0; fmv.v_us = fmv.v_max = 0; fmv.busy = 1;
            fmv.v_mdec0 = mdec_dma1_count; fmv.v_dry0 = audio_underruns_total();
            logf_("fmv: video started (emulated time %.1f s)\n", fmv.runs / 60.0);
        }
        fmv.active = 1;
    } else if (fmv.active) {
        double n = fmv.v_runs ? fmv.v_runs : 1;
        logf_("fmv: video ended: %u frames, MDEC reads %u, emulation avg %.2f ms, longest %.1f ms, "
              "%u frames over 20 ms, sound ran dry %u times\n", fmv.v_runs, mdec_dma1_count - fmv.v_mdec0,
              fmv.v_us / 1000.0 / n, fmv.v_max / 1000.0, fmv.v_slow, audio_underruns_total() - fmv.v_dry0);
        if (fmv.skipping) logf_("intro video: ended after %.1f s of video, %u button taps (emulated time %.1f s)\n",
                                fmv.v_runs / 60.0, fmv.tap >= 48 ? 4 : (fmv.tap + 11) / 12, fmv.runs / 60.0);
        fmv.active = fmv.skipping = 0;
    }
    if (!cfg.skip_intro) fmv.skipping = 0;
    if (!fmv.skipping) skip_buttons = 0;
}
static void input_poll_cb(void) { }

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    app_cb_counts[2]++;
    const pad_state_t *p;
    static const uint32_t map[16] = {
        [RETRO_DEVICE_ID_JOYPAD_B] = PAD_CROSS,    [RETRO_DEVICE_ID_JOYPAD_Y] = PAD_SQUARE,
        [RETRO_DEVICE_ID_JOYPAD_SELECT] = PAD_SELECT, [RETRO_DEVICE_ID_JOYPAD_START] = PAD_START,
        [RETRO_DEVICE_ID_JOYPAD_UP] = PAD_UP,      [RETRO_DEVICE_ID_JOYPAD_DOWN] = PAD_DOWN,
        [RETRO_DEVICE_ID_JOYPAD_LEFT] = PAD_LEFT,  [RETRO_DEVICE_ID_JOYPAD_RIGHT] = PAD_RIGHT,
        [RETRO_DEVICE_ID_JOYPAD_A] = PAD_CIRCLE,   [RETRO_DEVICE_ID_JOYPAD_X] = PAD_TRIANGLE,
        [RETRO_DEVICE_ID_JOYPAD_L] = PAD_L1,       [RETRO_DEVICE_ID_JOYPAD_R] = PAD_R1,
        [RETRO_DEVICE_ID_JOYPAD_L2] = PAD_L2,      [RETRO_DEVICE_ID_JOYPAD_R2] = PAD_R2,
        [RETRO_DEVICE_ID_JOYPAD_L3] = PAD_L3,      [RETRO_DEVICE_ID_JOYPAD_R3] = PAD_R3,
    };
    if (port > 1 || input_blocked) return 0;
    p = input_pad(port);
    if (port == 0 && skip_buttons) {      /* the intro-video skip presses these too */
        static pad_state_t q;
        q = *p; q.buttons |= skip_buttons; p = &q;
    }
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD:
    case RETRO_DEVICE_ANALOG:
        if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && index != RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            int v = (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
                ? (id == RETRO_DEVICE_ID_ANALOG_X ? p->lx : p->ly)
                : (id == RETRO_DEVICE_ID_ANALOG_X ? p->rx : p->ry);
            return (int16_t)((v - 128) * 256);
        }
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t m = 0; unsigned k;
            for (k = 0; k < 16; k++) if (p->buttons & map[k]) m |= 1 << k;
            return m;
        }
        return id < 16 && (p->buttons & map[id]) ? 1 : 0;
    }
    return 0;
}

/* called by the core's GPU hook when the GPU renderer presented the frame itself */
void hooks_frame_from_gpu(unsigned w, unsigned h)
{
    frame.w = w; frame.h = h; frame.fresh = 1; frame.gpu = 1;
}

/* ---------------- running a game ---------------- */
static void draw_current_frame(void)
{
    if (frame.gpu) hooks_draw_gpu_frame();
    else if (frame.data) video_draw_frame(frame.data, frame.w, frame.h, frame.pitch);
    else video_clear(0xff000000);
}

void hooks_redraw_last_frame(void) { draw_current_frame(); }

/* --- slow frames: what one retro_run spent its time on (Debug log only) --- */
extern uint64_t cdra_prof_ticks;
extern unsigned int cdra_prof_reads;
extern void lightrec_debug_counts(u32 *c, u64 *t) __attribute__((weak));
extern void hwr_rsx_sync_stats(unsigned *n, uint64_t *ticks);
extern unsigned long long jit_mem_ticks, jit_mem_bytes;
extern unsigned int jit_mem_calls;
struct frame_snap { u64 gpu, spu, cd; unsigned cd_n, sync_n; uint64_t sync; u32 lr[10]; u64 lt[12]; u64 jm_t, jm_b; unsigned jm_n; };
static void frame_snap(struct frame_snap *f)
{
    f->gpu = gpulib_prof_ticks[0]; f->spu = gpulib_prof_ticks[1];
    f->cd = cdra_prof_ticks; f->cd_n = cdra_prof_reads;
    hwr_rsx_sync_stats(&f->sync_n, &f->sync);
    memset(f->lr, 0, sizeof f->lr); memset(f->lt, 0, sizeof f->lt);
    if (lightrec_debug_counts) lightrec_debug_counts(f->lr, f->lt);
    f->jm_t = jit_mem_ticks; f->jm_b = jit_mem_bytes; f->jm_n = jit_mem_calls;
}
/* frames over 50 ms (a third of a second of 60 Hz output lost at 300 ms): at most
   20 lines per 10 seconds */
static void slow_frame_check(const struct frame_snap *a, u64 us)
{
    static u64 window; static int lines;
    struct frame_snap b;
    double tb = 1000.0 / (double)sysGetTimebaseFrequency(), gpu, spu, cd, sync;
    u64 now;
    if (us < 50000 || !cfg.debug_log) return;
    now = sysGetSystemTime();
    if (now - window > 10000000) { window = now; lines = 0; }
    if (++lines > 20) return;
    frame_snap(&b);
    gpu = (b.gpu - a->gpu) * tb; spu = (b.spu - a->spu) * tb; cd = (b.cd - a->cd) * tb;
    sync = b.sync >= a->sync ? (b.sync - a->sync) * tb : b.sync * tb;
    logf_("slow frame: %.0f ms: GPU %.1f ms (of that VRAM syncs %u, %.1f ms), sound %.1f ms, "
          "CD %u reads %.1f ms, rest (CPU emulation) %.1f ms; dynarec: %u blocks interpreted, "
          "%u compiled, %u new, %u found overwritten; on the emulation thread: finding blocks %.1f ms "
          "(of that translating new ones %.1f ms), interpreting %.1f ms, compile queue %.1f ms "
          "(adding %.1f ms: %u quick, %u locked; removing %.1f ms; cleanup %.1f ms); "
          "compiling (second thread) %.1f ms; slow lookups %u, entries repaired %u empty + %u stale\n",
          us / 1000.0, gpu, b.sync_n >= a->sync_n ? b.sync_n - a->sync_n : b.sync_n, sync, spu,
          b.cd_n - a->cd_n, cd, us / 1000.0 - gpu - spu - cd,
          b.lr[0] - a->lr[0], b.lr[1] - a->lr[1], b.lr[2] - a->lr[2], b.lr[3] - a->lr[3],
          (b.lt[0] - a->lt[0]) * tb, (b.lt[1] - a->lt[1]) * tb, (b.lt[2] - a->lt[2]) * tb,
          (b.lt[4] - a->lt[4]) * tb, (b.lt[5] - a->lt[5]) * tb, b.lr[4] - a->lr[4], b.lr[5] - a->lr[5],
          (b.lt[6] - a->lt[6]) * tb, (b.lt[7] - a->lt[7]) * tb, (b.lt[3] - a->lt[3]) * tb,
          b.lr[8] - a->lr[8], b.lr[6] - a->lr[6], b.lr[7] - a->lr[7]);
    {   /* the compile itself, by stage (second thread, or this one with 1 thread) */
        u32 nc = b.lr[1] - a->lr[1], ops = b.lr[9] - a->lr[9];
        double st[4], all = (b.lt[3] - a->lt[3]) * tb, rest = all;
        int k;
        for (k = 0; k < 4; k++) { st[k] = (b.lt[8 + k] - a->lt[8 + k]) * tb; rest -= st[k]; }
        logf_("slow frame, compiling: %u blocks, %.1f instructions each, %.2f ms per block; setup %.1f ms, "
              "building %.1f ms, optimizing %.1f ms, emitting %.1f ms, rest (code table, cleanup) %.1f ms; "
              "Lightning memory %.1f ms (%u calls, %.0f KB cleared)\n",
              nc, nc ? (double)ops / nc : 0.0, nc ? all / nc : 0.0, st[0], st[1], st[2], st[3], rest,
              (b.jm_t - a->jm_t) * tb, b.jm_n - a->jm_n, (b.jm_b - a->jm_b) / 1024.0);
    }
}

/* only touches a small box, so it costs next to nothing per frame */
/* fps: new pictures the game drew per second; speed: emulation speed (100% = the
   PS1's 50/60 frames per second; a game can draw fewer pictures at full speed) */
static void fps_overlay(double fps, double speed)
{
    static char last[48];
    static int last_border = -1;
    static unsigned last_gen;
    char b[48];
    snprintf(b, sizeof b, "%.1f fps  speed %.0f%%", fps, speed);
    int border = video_overlay_border();
    /* unchanged (the numbers change about once a second): the overlay keeps it, and
       leaving it alone spares waiting for the RSX before writing it */
    if (border == last_border && last_gen == video_overlay_generation() && !strcmp(b, last)) return;
    strcpy(last, b); last_border = border;
    video_overlay_clear_rows(8, 42);
    /* plain text with a shadow. Over the black border (not cleared every frame)
       the old digits are covered with opaque black, which looks like the border */
    if (border > 0) ovl_fill(0, 8, border, 34, 0xff000000);
    ovl_text(20, 12, b, 0xc0000000);
    ovl_text(18, 10, b, 0xffffffff);
    last_gen = video_overlay_generation();
}

/* named after the ROM file, which is unique per game (disc serials are missing
   for EXEs and some discs). Slot 0 keeps the original name (".state"), slots
   1..9 add the number. */
static void path_state(char *out, size_t n, int slot, const char *suffix)
{
    char num[12] = "";
    if (slot > 0) snprintf(num, sizeof num, "%d", slot);
    snprintf(out, n, APP_DIR "/states/%s.state%s%s", game_name[0] ? game_name : "unknown", num, suffix);
}

static void apply_spu_mdec(void);

/* The core's save and load run on their own thread with a 1 MiB stack (the main
   thread waits): they keep large buffers on the stack (SaveState alone about 60
   KiB, ndrc_freeze another 16 KiB), more than the stack lv2 gives the main thread
   unless the process parameter is honoured. A stack overflow there corrupted
   the heap and the save hung in its last step. */
static struct { void *buf; size_t sz; int load, ok; } state_job;

static void state_thread(void *arg)
{
    (void)arg;
    state_job.ok = state_job.load ? retro_unserialize(state_job.buf, state_job.sz)
                                  : retro_serialize(state_job.buf, state_job.sz);
    sysThreadExit(0);
}

static int run_core_state(void *buf, size_t sz, int load)
{
    sys_ppu_thread_t t;
    u64 ret;
    sys_ppu_thread_stack_t si;
    if (sysThreadGetStackInformation(&si) == 0)
        logf_("state: main thread stack %u KiB, running the core's %s on a 1024 KiB thread\n",
              (unsigned)(si.size / 1024), load ? "load" : "save");
    state_job.buf = buf; state_job.sz = sz; state_job.load = load; state_job.ok = 0;
    if (sysThreadCreate(&t, state_thread, NULL, 1000, 1024 * 1024, THREAD_JOINABLE, (char *)"state") != 0) {
        logf_("state: cannot create the thread, running it here\n");
        return load ? retro_unserialize(buf, sz) : retro_serialize(buf, sz);
    }
    sysThreadJoin(t, &ret);
    return state_job.ok;
}

int game_save_state(int slot)
{
    char path[600], tmp[600];
    size_t sz = retro_serialize_size();
    void *buf = sz ? malloc(sz) : NULL;
    FILE *f;
    int ok = 0;
    if (!buf) { logf_("save state: out of memory (%u bytes)\n", (unsigned)sz); return 0; }
    path_state(tmp, sizeof tmp, slot, ".tmp");
    path_state(path, sizeof path, slot, "");
    logf_("save state slot %d: saving (%u bytes)\n", slot, (unsigned)sz);
    log_sync();
    /* our own parts first, outside the core's save: VRAM read back from the RSX,
       FMV decoding on the SPUs paused (the core then calls neither) */
    WHERE("save state: reading VRAM back");
    hooks_sync_vram_for_state();
    WHERE("save state: pausing SPU FMV decoding");
    spu_mdec_enable(0);
    logf_("save state slot %d: core serialising\n", slot);
    log_sync();
    WHERE("save state: core serialising");
    ok = run_core_state(buf, sz, 0);
    apply_spu_mdec();
    if (ok) {
        ok = 0;
        WHERE("save state: writing the file");
        /* write a temporary file and rename it, so a failed write keeps the old state */
        if ((f = fopen(tmp, "wb"))) {
            ok = fwrite(buf, 1, sz, f) == sz;
            ok = (fclose(f) == 0) && ok;
            if (ok) { remove(path); ok = rename(tmp, path) == 0; }
            else remove(tmp);
        }
    }
    free(buf);
    logf_("save state slot %d: %s (%s)\n", slot, ok ? "saved" : "FAILED", path);
    log_sync();
    return ok;
}

int game_load_state(int slot)
{
    char path[600];
    size_t sz = retro_serialize_size();
    void *buf = sz ? malloc(sz) : NULL;
    FILE *f;
    int ok = 0;
    if (!buf) return 0;
    path_state(path, sizeof path, slot, "");
    logf_("load state slot %d: loading\n", slot);
    log_sync();
    if ((f = fopen(path, "rb"))) {
        WHERE("load state: reading the file");
        if (fread(buf, 1, sz, f) == sz) {
            spu_mdec_enable(0);
            logf_("load state slot %d: core unserialising\n", slot);
            log_sync();
            WHERE("load state: core unserialising");
            /* a state loaded during the BIOS logo is past it (the BIOS never reads
               the game's executable then): the chosen renderer starts first, so the
               state loads into it as into a running one (started after the load,
               part of the picture stayed black) */
            if (hwr_boot_hold) {
                hwr_boot_hold = 0;
                logf_("BIOS logo done (state loaded): renderer as set\n");
                hooks_apply_gpu_settings();
            }
            ok = run_core_state(buf, sz, 1);
            apply_spu_mdec();
            /* The core tells the renderer that all of VRAM changed during its GPU
               load step, before it reads the saved VRAM: the RSX renderer re-uploaded
               the old picture and kept it (broken graphics after loading a state
               from another moment). Tell it again now that VRAM is the loaded one. */
            if (ok && gpu_frontend_hooks.vram_written) {
                WHERE("load state: VRAM to the RSX");
                gpu_frontend_hooks.vram_written(0, 0, 1024, 512);
            }
        }
        fclose(f);
        logf_("load state slot %d: %s\n", slot, ok ? "loaded" : "FAILED (wrong size or version)");
    }
    free(buf);
    return ok;
}

/* "empty" or when the slot was saved */
const char *game_state_info(int slot, char *out, size_t n)
{
    char path[600];
    struct stat st;
    path_state(path, sizeof path, slot, "");
    if (stat(path, &st) != 0) snprintf(out, n, "empty");
    else {
        time_t t = st.st_mtime;
        struct tm *tm = localtime(&t);
        if (tm && tm->tm_year > 100) strftime(out, n, "%d %b %H:%M", tm);
        else snprintf(out, n, "saved");
    }
    return out;
}

void game_reset(void) { retro_reset(); }
int game_dump_vram(void) { return hooks_dump_vram(game_name[0] ? game_name : "unknown"); }
/* the SPU workers serve FMV decoding and the SPU smoothing filter */
static void apply_spu_mdec(void)
{
    int up = (cfg.spu_mdec || cfg.filter == FILTER_FXAA_SPU) && spu_mdec_init(5);
    spu_mdec_enable(up && cfg.spu_mdec);
}

/* DualShock if analog is on, else a digital pad; both ports together. */
static void apply_controller_type(void)
{
    unsigned dev = cfg.analog ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1) : RETRO_DEVICE_JOYPAD;
    retro_set_controller_port_device(0, dev);
    retro_set_controller_port_device(1, dev);
}

void game_settings_changed(void) { vars_dirty = 1; gte_widescreen = cfg.widescreen; hooks_apply_gpu_settings(); apply_spu_mdec(); audio_set_stretch(cfg.audio_stretch); apply_controller_type(); }

static void run_game(const char *path)
{
    struct retro_game_info gi = { path, NULL, 0, NULL };
    struct retro_system_av_info av;
    double acc = 0.0, disp_hz = 59.94, fps_shown = 0.0, speed_shown = 100.0;
    unsigned speed_runs = 0;                /* core frames run since the last FPS update */
    u64 pace_t = 0;                         /* time of the last pacing step (0: start over) */
    u64 last_flip, fps_t0;
    unsigned fps_frames = 0, emu_runs = 0;
    u64 emu_us = 0, emu_max = 0, prof0[2] = { 0, 0 };
    double tb_ms = 1000.0 / (double)sysGetTimebaseFrequency();
    int combo_held = 0, notice_frames = 0;

    int is_bios = !strcmp(path, "bios:");   /* game-list Boot BIOS entry: BIOS menu, no disc */
    game_is_disc = !is_bios && !strncmp(path, "cdrom:", 6);
    if (is_bios)
        snprintf(game_name, sizeof game_name, "BIOS");   /* no per-game file exists for it, so global settings stay */
    else {
        const char *b = strrchr(path, '/');
        char *dot;
        snprintf(game_name, sizeof game_name, "%s", b ? b + 1 : path);
        if ((dot = strrchr(game_name, '.'))) *dot = 0;
    }
    config_game_begin(game_name);       /* its own settings (configs/<game>.ini), if it has them */
    log_game_begin(game_name);          /* from here to "game closed": logs/<game>.txt (Debug log on) */
    {   /* debug log: dynarec blocks record which one runs (for hang reports) */
        extern int psx_debug_trace;
        psx_debug_trace = cfg.debug_log;
    }
    logf_("%s " APP_VERSION ", built " __DATE__ " " __TIME__ "\n", APP_TITLE);
    logf_("settings: cpu %s, renderer %d, filter %d, aspect %d, frameskip %d, dither %d, fps %d, gpu thread %d\n",
          cfg.dynarec ? "dynarec" : "interp", cfg.gpu_scale, cfg.filter, cfg.aspect, cfg.frameskip, cfg.dithering,
          cfg.show_fps, cfg.gpu_thread);
    logf_("loading %s (%s)\n", is_bios ? "BIOS menu" : path, !cfg.dynarec ? "interpreter" : cfg.dynarec_thread ? "dynarec, compiling on the second thread" : "dynarec");
    logf_("BIOS: %s%s\n", hooks_bios_status(), cfg.boot_logo ? (hooks_bios_present() ? ", PS1 logo on" : ", PS1 logo needs a BIOS") : "");
    {   /* Lightrec's compiler thread (core: restarts Lightrec if the setting changed) */
        extern void lightrec_plugin_set_threaded(int on) __attribute__((weak));
        if (lightrec_plugin_set_threaded) lightrec_plugin_set_threaded(cfg.dynarec_thread);
    }
    memset(&frame, 0, sizeof frame);
    video_clear_frame_buffer();
    video_overlay_clear();
    input_blocked = 1;
    video_begin(); video_clear(0xff000000); video_present();
    if (!retro_load_game(&gi)) {
        logf_("load failed\n");
        config_game_end();
        log_game_end();
        return;
    }
    apply_controller_type();   /* DualShock if analog on, else a digital pad */
    retro_get_system_av_info(&av);
    core_fps = av.timing.fps; core_rate = av.timing.sample_rate;
    audio_set_input_rate(core_rate);
    /* the BIOS logo is drawn by the software renderer; the chosen one takes over
       when the BIOS reads the game's executable (or after 30 s, if it never does) */
    hwr_boot_hold = cfg.gpu_scale && cfg.boot_logo && hooks_bios_present();
    boot_frames = 0;
    if (hwr_boot_hold) logf_("BIOS logo: software renderer until the game starts\n");
    hooks_game_started();
    apply_spu_mdec();
    audio_set_stretch(cfg.audio_stretch);
    log_memory("game started");
    logf_("running at %.3f fps, audio %.0f Hz\n", core_fps, core_rate);
    app_in_game = 1;
    memset(&fmv, 0, sizeof fmv); fmv.last_count = mdec_dma1_count; fmv.quiet = 1000;

    last_flip = fps_t0 = sysGetSystemTime();
    while (!app_exit_requested) {
        const pad_state_t *p0;
        int runs = 0;
        u64 now;

        app_poll_system();
        input_poll();
        p0 = input_pad(0);
        if (input_blocked && !p0->buttons && !input_pad(1)->buttons) input_blocked = 0;
        if (hwr_boot_hold && (CdromExeRead || ++boot_frames > 1800)) {
            hwr_boot_hold = 0;
            logf_("BIOS logo done (%s): renderer as set\n", CdromExeRead ? "game executable read" : "30 s");
            hooks_apply_gpu_settings();
        }

        /* SELECT+START opens the in-game menu (on release, so the game never sees both) */
        if ((p0->buttons & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START)) combo_held = 1;
        else if (combo_held && !(p0->buttons & (PAD_SELECT | PAD_START))) {
            combo_held = 0;
            audio_pause(1);
            input_rumble_stop();            /* no vibration in the menu */
            if (menu_ingame()) { audio_pause(0); break; }
            audio_pause(0);
            video_begin();                  /* the RSX is done with the menu's last frame */
            video_overlay_clear();          /* drop the menu from the overlay */
            input_blocked = 1;              /* the confirming X press must not reach the game */
            acc = 0.0; pace_t = 0;
        }

        /* run the core at its own rate (59.94 / 50 Hz) by the clock: as many
           frames as the time since the last pass asks for. (Counting display
           refreshes instead slowed the game down whenever a pass missed one,
           and the speed then seemed to jump back up.) */
        now = sysGetSystemTime();
        if (pace_t && now > pace_t) acc += (double)(now - pace_t) * core_fps / 1e6;
        else acc += core_fps / disp_hz;
        pace_t = now;
        if (acc > 3.0) acc = 1.0;           /* fell far behind (loading, a stall): do not try to catch up */
        /* strict: one frame per pass at most, so never faster than real time; at most
           one frame of time is kept (capping it at exactly one frame lost a whole frame
           whenever a pass came a little early: GT2 ran at 70%) */
        if (cfg.speed_limit == 1 && acc > 2.0) acc = 2.0;
        if (cfg.speed_limit == 2 && !fmv.skipping) {
            /* no limit: as many frames as fit in about one display frame, only
               the last one shown, no sound (it would pile up) */
            u64 t_start = sysGetSystemTime();
            audio_mute = 1;
            do {
                WHERE("emulating (retro_run, no speed limit)");
                retro_run();
                fmv_track(0);
                runs++; speed_runs++;
            } while (runs < 12 && sysGetSystemTime() - t_start < 14000 && !combo_held);
            audio_mute = 0;
            acc = 0.0;
        }
        if (fmv.skipping) {
            /* fast-forward: as many frames as fit in about one display frame, no
               sound, only the last one shown */
            u64 t_start = sysGetSystemTime();
            audio_mute = 1;
            do {
                WHERE("emulating (retro_run, skipping a video)");
                retro_run();
                fmv_track(0);
                runs++; speed_runs++;
            } while (fmv.skipping && runs < 12 && sysGetSystemTime() - t_start < 14000);
            audio_mute = 0;
            acc = 0.0;
        }
        while (acc >= 1.0 && runs < (cfg.speed_limit == 1 ? 1 : 2)) {
            if (audio_status_cb) {
                unsigned fill = audio_fill();
                audio_status_cb(true, fill * 100 / 8192, fill < 1024);
            }
            WHERE("emulating (retro_run)");
            if (!combo_held) {
                u64 t0 = sysGetSystemTime(), t;
                struct frame_snap s0;
                frame_snap(&s0);
                retro_run();
                t = sysGetSystemTime() - t0;
                slow_frame_check(&s0, t);
                fmv_track(t);
                speed_runs++;
                emu_us += t; emu_runs++;
                if (t > emu_max) emu_max = t;
            }
            acc -= 1.0; runs++;
        }

        /* the emulation above overlapped with the RSX finishing the previous frame;
           the next picture goes into a free display buffer (triple buffering: the
           wait for the previous flip, if any, comes when this one is queued) */
        video_begin();
        WHERE("drawing frame");
        draw_current_frame();
        if (frame.fresh) { frame.fresh = 0; fps_frames++; }
        if (cfg.show_fps) { fps_overlay(fps_shown, speed_shown); video_draw_overlay_rows(8, 42); }
        {   /* renderer notice: the first 6 seconds of a game */
            static const char *shown_notice;
            const char *n = notice_frames < 360 ? hooks_gpu_notice() : NULL;
            if (n != shown_notice) {
                video_overlay_clear_rows(660, 700);
                if (n) { ovl_text(42, 667, n, 0xc0000000); ovl_text(40, 665, n, 0xffffd060); }
                shown_notice = n;
                video_draw_overlay_rows(660, 700);
                video_borders_dirty();      /* the notice also covered the borders */
            } else if (n) video_draw_overlay_rows(660, 700);
            notice_frames++;
        }
        video_present();

        now = sysGetSystemTime();
        if (now > last_flip) {
            double hz = 1e6 / (double)(now - last_flip);
            if (hz > 45.0 && hz < 65.0) disp_hz = disp_hz * 0.98 + hz * 0.02;   /* measured refresh rate */
        }
        last_flip = now;
        if (now - fps_t0 >= 1000000) {
            static unsigned secs;
            fps_shown = fps_frames * 1e6 / (double)(now - fps_t0);
            speed_shown = speed_runs * 1e6 / (double)(now - fps_t0) / core_fps * 100.0;
            fps_frames = 0; speed_runs = 0; fps_t0 = now;
            if (++secs % 10 == 0) {
                double n = emu_runs ? emu_runs : 1;
                double gpu_ms = (gpulib_prof_ticks[0] - prof0[0]) * tb_ms / n, spu_ms = (gpulib_prof_ticks[1] - prof0[1]) * tb_ms / n;
                double emu_ms = emu_us / 1000.0 / n;
                logf_("game: %.1f emulated fps, speed %.0f%%, display %.2f Hz, emulation avg %.2f ms max %.2f ms per frame "
                      "(GPU %.2f, sound %.2f, CPU and rest %.2f), audio buffered %u, empty audio blocks %u, renderer %s%s\n",
                      fps_shown, speed_shown, disp_hz, emu_ms, emu_max / 1000.0, gpu_ms, spu_ms, emu_ms - gpu_ms - spu_ms,
                      audio_fill(), audio_underruns(), frame.gpu ? "RSX" : "software",
                      cfg.gpu_thread && !cfg.gpu_scale ? " + GPU thread" : "");
                emu_us = emu_max = 0; emu_runs = 0;
                prof0[0] = gpulib_prof_ticks[0]; prof0[1] = gpulib_prof_ticks[1];
                if (frame.gpu) hooks_log_gpu_stats();
                if (cfg.debug_log && lightrec_debug_counts) {   /* dynarec totals so far */
                    u32 c[10]; u64 t[12];
                    lightrec_debug_counts(c, t);
                    logf_("dynarec: %u blocks compiled, %u found, %u found overwritten; status polling skipped %u reads (last %08x)\n",
                          c[1], c[2], c[3], lightrec_poll_skips, lightrec_poll_addr);
                }
                if (cfg.debug_log) {
                    static char b[1400];
                    psx_debug_state(b, sizeof b);
                    logf_("core: %s\n", b);
                    logf_("core callbacks so far: video %u, audio %u, input %u, environment %u\n",
                          app_cb_counts[0], app_cb_counts[1], app_cb_counts[2], app_cb_counts[3]);
                }
                if (spu_mdec_active()) {
                    unsigned st, mbs, miss; double w;
                    spu_mdec_stats(&st, &mbs, &miss, &w);
                    if (st || miss)
                        logf_("fmv: %u frames decoded on the SPUs (%u macroblocks), PPU waited %.1f ms in total, %u reads left to the PPU\n",
                              st, mbs, w, miss);
                }
            }
        }
    }
    app_in_game = 0;
    hooks_game_stopping();
    spu_mdec_enable(0);
    retro_unload_game();
    input_rumble_stop();
    logf_("game closed\n");
    config_game_end();
    log_game_end();
}

static int app_main(void)
{
    mkdir(APP_DIR "/system", 0777); mkdir(APP_DIR "/saves", 0777); mkdir(APP_DIR "/saves/imported", 0777);
    mkdir(APP_DIR "/states", 0777); mkdir(APP_DIR "/roms", 0777); mkdir(APP_DIR "/logs", 0777);
    mkdir(APP_DIR "/dumps", 0777);   /* made here, long before a hang report may need it */
    log_open();
    logf_("%s " APP_VERSION ", built " __DATE__ " " __TIME__ "\n", APP_TITLE);
    watchdog_start();
    config_load();
    log_ready();                        /* log files only with Debug log on */
    logf_("settings: cpu %s, renderer %d, filter %d, aspect %d, frameskip %d, dither %d, fps %d\n",
          cfg.dynarec ? "dynarec" : "interp", cfg.gpu_scale, cfg.filter, cfg.aspect, cfg.frameskip, cfg.dithering, cfg.show_fps);
    log_memory("start");
    {
        sys_ppu_thread_stack_t si;
        if (sysThreadGetStackInformation(&si) == 0) logf_("emulator thread stack: %u KiB\n", (unsigned)(si.size / 1024));
    }
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
    if (!video_init()) { logf_("video init failed\n"); return 1; }
    if (!audio_init()) logf_("audio init failed, continuing without sound\n");
    input_init();

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
    hooks_init();

    while (!app_exit_requested) {
        const char *game = menu_run();
        if (!game) break;
        config_save();
        run_game(game);
    }
    config_save();
    spu_mdec_shutdown();
    retro_deinit();
    audio_shutdown();
    video_shutdown();
    logf_("exit\n");
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    return 0;
}

/* Everything runs on a thread with a large stack: lv2 gives the primary thread
   only 64 KiB whatever SYS_PROCESS_PARAM asks for, and the core needs more.
   Lightrec's interpreter (it runs each new block once before compiling it)
   calls one C function per MIPS opcode without returning in between, about
   130 bytes of stack per opcode: a long block (Terracon, after loading a
   level) ran off the end of the 64 KiB and the PPU thread stopped dead. */
#define APP_STACK (2 * 1024 * 1024)
static int app_ret;

static void app_thread(void *arg)
{
    (void)arg;
    app_ret = app_main();
    sysThreadExit(0);
}

int main(int argc, char **argv)
{
    sys_ppu_thread_t t;
    u64 ret;
    (void)argc; (void)argv;
    if (sysThreadCreate(&t, app_thread, NULL, 1000, APP_STACK, THREAD_JOINABLE, (char *)"pcsx main") != 0 &&
        sysThreadCreate(&t, app_thread, NULL, 1000, 1024 * 1024, THREAD_JOINABLE, (char *)"pcsx main") != 0)
        return app_main();   /* should not happen; run on the small stack */
    sysThreadJoin(t, &ret);
    return app_ret;
}
