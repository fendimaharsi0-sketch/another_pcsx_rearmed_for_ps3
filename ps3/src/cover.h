/* Cover art for the game list, read from the user's own webMAN MOD cover pack
   (never bundled in this package: the pack's art is not ours to ship).
   stb_image v2.28 (public domain, in stb_image.h) decodes JPG/PNG with no
   new library for the CI toolchain to build; formats it cannot read fall
   back to the placeholder panel, they never crash the menu.
   Small by design, for an old machine: only the selected game's cover is
   kept, never the whole list; a file over 512 KB or an image over 512x512
   is refused outright (the pack's covers are 268x268 or smaller, so this
   cap has headroom without letting a stray huge image eat PS3 memory). */
#ifndef COVER_H
#define COVER_H

typedef struct {
    unsigned char *px;      /* RGBA, w*h*4 bytes, or NULL when no cover */
    int w, h;
} cover_img_t;

/* Load the cover for one game list entry. is_disc: the disc in the drive
   (disc_serial is its serial, "" when unknown); otherwise game_path is the
   game file and its name is used for the lookup. Returns 1 with *out filled
   when a cover was decoded, 0 with px NULL when none was found or readable. */
int cover_load(const char *game_path, int is_disc, const char *disc_serial, cover_img_t *out);
void cover_free(cover_img_t *c);

#endif
