/* Theme UI: menu shape, cover style and colour preset (menu.c only).
 * The palette table is the single source of truth: menu.c reads every
 * menu colour through theme_pal(), so a preset change repaints the whole
 * menu on the next frame with no call-site edits. Values are the v2
 * mockup's: 4 dark presets, then Light, retro Grey and warm-LCD mono.
 * panel keeps the old 0xe0 alpha (translucent over the game/clear colour);
 * everything else is opaque. */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

enum {
    THEME_SHAPE_MIXED = 0,  /* rounded panels/chips, square rows (the old look) */
    THEME_SHAPE_ROUND,      /* rows rounded too */
    THEME_SHAPE_SQUARE,     /* everything square */
    THEME_SHAPE_COUNT
};

enum {
    THEME_COVER_GLOSSY = 0, /* plastic shine over the art (the old look) */
    THEME_COVER_MATTE,      /* no shine */
    THEME_COVER_CORNERS,    /* accent corner brackets around the case */
    THEME_COVER_FRAMED,     /* black frame around the art, with shine */
    THEME_COVER_FRAME_MATTE,/* black frame around the art, no shine */
    THEME_COVER_SHINE,      /* bare art with shine, no case at all */
    THEME_COVER_PLAIN,      /* bare art, no case, no shine */
    THEME_COVER_COUNT
};

enum {
    THEME_COLOR_YELLOW = 0, /* the old look */
    THEME_COLOR_BLUE,
    THEME_COLOR_GREEN,
    THEME_COLOR_RED,
    THEME_COLOR_LIGHT,
    THEME_COLOR_GRAY,
    THEME_COLOR_LCD,        /* warm monochrome, like an old LCD */
    THEME_COLOR_COUNT
};

typedef struct {
    uint32_t text, dim, accent, panel, sel, header, clear;
} theme_pal_t;

/* text, dim, accent, panel, sel, header, clear */
static const theme_pal_t theme_pals[THEME_COLOR_COUNT] = {
    { 0xffe8e8e8u, 0xff9098a8u, 0xffffd24au, 0xe0141a26u, 0xff2c3a58u, 0xff182238u, 0xff0c1018u }, /* Yellow */
    { 0xffe8e8e8u, 0xff9098a8u, 0xff6ab8ffu, 0xe0101a28u, 0xff1e3a5cu, 0xff0f2038u, 0xff0c1018u }, /* Blue */
    { 0xffe8e8e8u, 0xff9098a8u, 0xff7de38bu, 0xe0101a14u, 0xff1e4030u, 0xff0f2418u, 0xff0c1018u }, /* Green */
    { 0xffe8e8e8u, 0xff9098a8u, 0xffff7a6bu, 0xe01a1010u, 0xff4a2320u, 0xff2a1210u, 0xff0c1018u }, /* Red */
    { 0xff20242au, 0xff6a7078u, 0xff9a6b00u, 0xe0ffffffu, 0xffc2d2e2u, 0xffc9d1d8u, 0xffdfe3e6u }, /* Light */
    { 0xff1c1c1cu, 0xff55595eu, 0xff2f4d7du, 0xe0cfd3d8u, 0xffa8adb2u, 0xff9aa0a6u, 0xffb8bcc0u }, /* Gray */
    { 0xff2e2410u, 0xff6b5a30u, 0xff7a5c14u, 0xe0e6d9acu, 0xffc2ab72u, 0xffc9b478u, 0xffd8c690u }, /* LCD */
};

static const char *theme_shape_names[THEME_SHAPE_COUNT] = {
    "Mixed", "Round", "Square"
};
static const char *theme_cover_names[THEME_COVER_COUNT] = {
    "Glossy", "Matte", "Corners", "Framed", "Frame Matte", "Shine", "Plain"
};
static const char *theme_color_names[THEME_COLOR_COUNT] = {
    "Yellow", "Blue", "Green", "Red", "Light", "Gray", "LCD"
};

#endif /* THEME_H */
