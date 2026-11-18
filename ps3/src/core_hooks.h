/* Glue between the front-end and things only the core knows about
 * (implemented in hooks.c). */
#ifndef PS1PS3_CORE_HOOKS_H
#define PS1PS3_CORE_HOOKS_H

void hooks_init(void);
void hooks_game_started(void);
void hooks_game_stopping(void);
void hooks_apply_gpu_settings(void);
const char *hooks_game_serial(void);
const char *hooks_bios_status(void);
int hooks_ps3_bios(void);                 /* the PS3's own PS1 BIOS is there (PS3_BIOS) */
int hooks_bios_auto_flash(void);          /* Auto: nothing in system/, the PS3's own is used */
int hooks_bios_menu_ok(void);             /* a real BIOS is in system/ (the PS3's own has no menu) */
int hooks_bios_present(void);             /* a real BIOS or the PS3's own is there (else the HLE BIOS) */

/* GPU renderer */
void hooks_frame_from_gpu(unsigned w, unsigned h);   /* in main.c */
void hooks_draw_gpu_frame(void);
void hooks_log_gpu_stats(void);
const char *hooks_gpu_notice(void);       /* shown on screen at game start, or NULL */
int  hooks_dump_vram(const char *name);
void hooks_sync_vram_for_state(void);       /* core VRAM up to date before a savestate */   /* VRAM pictures into USRDIR/dumps */     /* hwr/hwr_rsx.c, every 10 s */
void hooks_redraw_last_frame(void);

/* ---- SPU polygon renderer (spu_poly.c) ----
   The software renderer records the primitives it is asked to draw and the
   Cell's SPUs rasterize them into VRAM. hooks_* below pick between this and
   the RSX renderer. */
int  spr_init(int want);                  /* start the SPU workers (0 = not wanted yet) */
void spr_shutdown(void);
int  spr_ready(void);                     /* workers up */
int  spr_active(void);                    /* the capture is on */
int  spr_spu_count(void);                 /* workers running, for the stats log */
int  spr_max_spu(void);                   /* the machine's SPU budget (6) */
int  spr_set_workers(int want);           /* rebuild with another count (off only) */
int  spr_start(void);
void spr_stop(void);
void spr_sync(void);                      /* draw everything recorded and wait */
void spr_stats(unsigned *jobs, double *prims, double *wait_ms, unsigned *max_spu_ms,
               unsigned *waits, unsigned *sprites, unsigned *tiles);
void spr_cap_set(int on);                 /* in plugins/dfxvideo/soft.c */
extern volatile int spr_busy;

#endif
