/*
 * Direct3D 9 overlay for the co-op lobby: draws a lobby_view (lobby.h) with ID3DXFont / ID3DXSprite (d3dx9_43.dll, which the game itself imports)
 * on top of the finished frame, from the Present hook. No dependence on the game's own GUI. Resources are created on first use and released
 * before every device Reset. The GDI backend (default) reads the finished frame back once when the lobby opens and uses it, blurred and tinted, as the backdrop.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include "lobby.h"

typedef struct {
    lobby_view v;
    int lang;            /* 0..11, order of lobby_lang_names (picks the font face) */
    unsigned tick_ms;    /* animation clock */
    int cursor_x, cursor_y, cursor_on;   /* mouse position in virtual 1920x1080 coordinates (for hover feedback) */
} ov_scene;

void overlay_draw(void *d3d9_device, const ov_scene *sc);   /* inside the Present hook; touches only the sprite/font state it restores itself */
void overlay_release(void);                                  /* before IDirect3DDevice9::Reset (and at shutdown) */
void overlay_banner(void *d3d9_device, const char *utf8_text, int lang);   /* stage 6i: an opaque one-line strip across the top of the back buffer (GDI backend only) */
/* mouse: maps a point of the game window's client area (client_w x client_h pixels) to the virtual 1920x1080 layout and returns the row of the last
 * drawn scene under it or -1; *vx / *vy get the virtual position (for the pointer the overlay draws) */
int overlay_row_at_client(int px, int py, int client_w, int client_h, int *vx, int *vy);
/* backends: mode 3 = GDI bitmap blitted with StretchRect (default, touches no render state), 2 = D3DX sprite and font, 1 = sprite rectangles only, 0 = draw nothing; trace = log the steps of the next frames */
void overlay_set_mode(int mode);
void overlay_set_trace(int frames);
int overlay_available(void);        /* d3dx9_43.dll and its two functions are there */
unsigned overlay_frames(void);       /* frames drawn so far (the lobby gives up when none appears) */
/* statistics for the console: frames drawn, resource creation failures */
void overlay_stats(char *out, int size);
/* offline: the lobby scene rendered by the GDI backend into a 32-bit top-down bitmap (no device); overlay_set_preview_backdrop sets the picture that stands for the game frame (tests/overlay_preview_bmp.c) */
void overlay_set_game_dir(const char *arc_dir);   /* folder with the game's Core.arc / Msg_<lang>.arc (default <exe folder>\\nativePCNext\\arc\\DX9NEXT); the offline preview sets it */
int overlay_render_to_bitmap(const ov_scene *sc, int w, int h, unsigned **pixels_out);
void overlay_set_preview_backdrop(const unsigned *px, int w, int h);

#endif
