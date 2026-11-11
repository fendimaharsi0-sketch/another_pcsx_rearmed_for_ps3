/* Front-end glue that needs to know about the core or the GPU renderer. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "common.h"
#include "core_hooks.h"

extern char CdromId[10];        /* libpcsxcore/misc.c: serial of the running disc */

const char *hooks_game_serial(void) { return CdromId; }

/* a BIOS the core will use: one of the names it tries first, else any file of
   512 KiB in system/ (the core takes those too, of any name) */
static int ps3_bios_ok(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_size == 512 * 1024 || st.st_size == 4 * 1024 * 1024);
}
int hooks_ps3_bios(void) { return ps3_bios_ok(PS3_BIOS); }

/* a real (external) BIOS in system/: the known names first, else any file
   of 512 KiB. The PS3's own is NOT one: it has no BIOS menu (no Sony logo,
   no memory card / CD player screen), so Boot BIOS must not count it. */
static int external_bios(char *name, size_t n)
{
    static const char *names[] = { "scph5500", "scph5501", "scph5502", "psxonpsp660", "scph101", "scph7001", "scph1001", NULL };
    int i;
    DIR *d;
    struct dirent *e;
    for (i = 0; names[i]; i++) {
        char p[256];
        struct stat st;
        snprintf(p, sizeof p, APP_DIR "/system/%s.bin", names[i]);
        if (stat(p, &st) == 0 && st.st_size == 512 * 1024) {
            if (name) snprintf(name, n, "%s.bin", names[i]);
            return 1;
        }
    }
    if ((d = opendir(APP_DIR "/system"))) {
        while ((e = readdir(d))) {
            char p[512];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, APP_DIR "/system/%s", e->d_name);
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 512 * 1024) {
                if (name) snprintf(name, n, "%.150s", e->d_name);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}

int hooks_bios_menu_ok(void) { return external_bios(NULL, 0); }

static int bios_from_flash;
int hooks_bios_auto_flash(void) { hooks_bios_status(); return bios_from_flash; }

const char *hooks_bios_status(void)
{
    static char b[200];
    char name[160];
    bios_from_flash = 0;
    /* Auto: a real BIOS in system/ first, then the PS3's own, then HLE.
       The PS3's own used to be first (it is on every PS3), but it is a
       stripped PS2-era ROM with no BIOS menu, so a real BIOS wins. */
    if (external_bios(name, sizeof name)) {
        snprintf(b, sizeof b, "%s found", name);
        return b;
    }
    if (ps3_bios_ok(PS3_BIOS)) {
        bios_from_flash = 1;
        return "the PS3's own (ps1_rom.bin) found";
    }
    return "none found, using the built-in HLE BIOS (put scph5501.bin in system/)";
}

int hooks_bios_present(void) { return strncmp(hooks_bios_status(), "none", 4) != 0; }

/* GPU renderer hooks: hwr/hwr_rsx.c */
