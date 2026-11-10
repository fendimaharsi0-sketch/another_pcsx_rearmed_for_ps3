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
int hooks_bios_present(void);             /* a BIOS file is in system/ (else the HLE BIOS) */

/* GPU renderer */
void hooks_frame_from_gpu(unsigned w, unsigned h);   /* in main.c */
void hooks_draw_gpu_frame(void);
void hooks_log_gpu_stats(void);
const char *hooks_gpu_notice(void);       /* shown on screen at game start, or NULL */
int  hooks_dump_vram(const char *name);
void hooks_sync_vram_for_state(void);       /* core VRAM up to date before a savestate */   /* VRAM pictures into USRDIR/dumps */     /* hwr/hwr_rsx.c, every 10 s */
void hooks_redraw_last_frame(void);

#endif
