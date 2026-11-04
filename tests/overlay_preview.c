/*
 * Renders the lobby overlay (native/overlay.c) into BMP files without the game: a hidden window, a Direct3D 9 device on the default adapter, a gradient as
 * the "game frame" and one screenshot per screen / language. 32-bit only (it loads the same d3dx9_43.dll the game uses).
 *   sh tests/build32.sh  (builds build/32/overlay_preview.exe)   build/32/overlay_preview.exe <outdir> [width height]
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/overlay.h"

void r2_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void save_bmp(IDirect3DDevice9 *dev, const char *path)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        printf("GetRenderTargetData failed\n");
        return;
    }
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    FILE *f = fopen(path, "wb");
    DWORD row = d.Width * 4, size = row * d.Height;
    BITMAPFILEHEADER fh = {0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, size, 0, 0, 0, 0};
    fwrite(&fh, sizeof fh, 1, f), fwrite(&ih, sizeof ih, 1, f);
    for (UINT y = 0; y < d.Height; y++) fwrite((BYTE *)lr.pBits + y * lr.Pitch, row, 1, f);
    fclose(f);
    IDirect3DSurface9_UnlockRect(sys);
    IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

static void row(lobby_view *v, const char *label, const char *value, int kind)
{
    lobby_row *r = &v->rows[v->nrows++];
    memset(r, 0, sizeof *r);
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->value, sizeof r->value, "%s", value);
    r->kind = kind, r->action = kind == LV_NORMAL;
}

static void common(lobby_view *v, const char *title)
{
    memset(v, 0, sizeof *v);
    snprintf(v->title, sizeof v->title, "%s", title);
    snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT, 0));
    snprintf(v->col_name, sizeof v->col_name, "Name");
    snprintf(v->col_char, sizeof v->col_char, "Character");
    snprintf(v->col_ping, sizeof v->col_ping, "Ping");
    snprintf(v->t_ready, sizeof v->t_ready, "Ready");
    snprintf(v->t_not_ready, sizeof v->t_not_ready, "Not ready");
    snprintf(v->t_host, sizeof v->t_host, "Host");
}

static void room(lobby_view *v)
{
    common(v, "Co-Op Play");
    snprintf(v->subtitle, sizeof v->subtitle, "Your address (give it to your friend): 192.168.1.5:28410");
    row(v, "Claire", "(you)", LV_NORMAL);
    row(v, "Moira", "BeBeBe", LV_GREY);
    row(v, "Barry", "", LV_NORMAL);
    row(v, "Natalia", "", LV_GREY);
    row(v, "Episode", "<  1  >", LV_NORMAL);
    row(v, "Difficulty", "<  Normal  >", LV_NORMAL);
    row(v, "Start Game", "", LV_GREY);
    row(v, "Leave", "", LV_NORMAL);
    v->sel = 0;
    v->nplayers = 2;
    snprintf(v->players[0].name, sizeof v->players[0].name, "YoungOn"), snprintf(v->players[0].chr, sizeof v->players[0].chr, "Claire");
    v->players[0].host = 1, v->players[0].me = 1, v->players[0].ping = -1;
    snprintf(v->players[1].name, sizeof v->players[1].name, "BeBeBe"), snprintf(v->players[1].chr, sizeof v->players[1].chr, "Moira");
    v->players[1].ready = 0, v->players[1].ping = 12;
    snprintf(v->status, sizeof v->status, "The game can start when your partner is ready.");
    snprintf(v->note, sizeof v->note, "%s", lobby_own_text(LT_SAVE_WARN, 0));
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    UINT W = argc > 3 ? (UINT)atoi(argv[2]) : 1920, H = argc > 3 ? (UINT)atoi(argv[3]) : 1080;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA, wc.hInstance = GetModuleHandleA(NULL), wc.lpszClassName = "ovprev";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("ovprev", "ovprev", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);   /* never shown */
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_COPY, pp.BackBufferFormat = D3DFMT_X8R8G8B8, pp.BackBufferWidth = W, pp.BackBufferHeight = H, pp.hDeviceWindow = w;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, (argc > 4 && argv[4][0] == 'g') ? (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE) : D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        printf("CreateDevice failed 0x%08lx\n", hr);
        return 1;
    }
    if (argc > 6) overlay_set_mode(atoi(argv[6]));
    struct { const char *name; int lang; int screen; } shots[] = {{"room-en", 0, 0}, {"top-en", 0, 1}, {"ip-en", 0, 2}, {"room-ru", 5, 0}, {"room-ja", 8, 0}, {"room-ko", 9, 0}, {"room-zhs", 10, 0}};
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        ov_scene sc;
        memset(&sc, 0, sizeof sc);
        sc.lang = shots[i].lang, sc.tick_ms = 600;
        lobby_view *v = &sc.v;
        if (shots[i].screen == 0) {
            room(v);
            if (shots[i].lang) {   /* the same room with the texts of that language */
                snprintf(v->status, sizeof v->status, "%s", lobby_own_text(LT_NEED_READY, shots[i].lang));
                snprintf(v->note, sizeof v->note, "%s", lobby_own_text(LT_SAVE_WARN, shots[i].lang));
                snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT, shots[i].lang));
                snprintf(v->subtitle, sizeof v->subtitle, "%s: 192.168.1.5:28410", lobby_own_text(LT_YOUR_ADDR, shots[i].lang));
                snprintf(v->col_name, sizeof v->col_name, "%s", lobby_own_text(LT_NAME, shots[i].lang));
                snprintf(v->col_char, sizeof v->col_char, "%s", lobby_own_text(LT_CHARACTER, shots[i].lang));
                snprintf(v->t_ready, sizeof v->t_ready, "%s", lobby_own_text(LT_PARTNER_READY, shots[i].lang));
                snprintf(v->t_not_ready, sizeof v->t_not_ready, "%s", lobby_own_text(LT_NOT_READY, shots[i].lang));
            }
        } else if (shots[i].screen == 1) {
            common(v, "Co-Op Play");
            row(v, "Find a Partner", "", LV_NORMAL), row(v, "Join a Partner", "", LV_NORMAL), row(v, "Back", "", LV_NORMAL);
            v->sel = 1;
            snprintf(v->status, sizeof v->status, "Failed to join session.  (no answer from the host)");
            v->status_kind = LSK_ERROR;
        } else if (shots[i].screen == 2) {
            common(v, "Join a Partner - System Link");
            snprintf(v->prompt, sizeof v->prompt, "%s", lobby_own_text(LT_ENTER_ADDR, 0));
            v->input_on = 1;
            snprintf(v->input, sizeof v->input, "192.168.1.145:28410");
            row(v, "Join a Partner", "", LV_NORMAL), row(v, "Back", "", LV_NORMAL);
            v->sel = 0;
            snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT_IP, 0));
            snprintf(v->status, sizeof v->status, "Connecting");
            v->busy = 1;
        }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF2A3038, 1.0f, 0);   /* the "game" */
        D3DRECT bar = {0, (LONG)H / 2, (LONG)W, (LONG)H};
        IDirect3DDevice9_Clear(dev, 1, &bar, D3DCLEAR_TARGET, 0xFF5A3A20, 1.0f, 0);
        overlay_draw(dev, &sc);
        char path[400];
        snprintf(path, sizeof path, "%s\\%s.bmp", outdir, shots[i].name);
        save_bmp(dev, path);
        char st[200];
        overlay_stats(st, sizeof st);
        printf("%s -> %s\n", st, path);
    }
    if (argc > 5) {   /* present loop: the game presents every frame; check that the overlay and Present keep going */
        ov_scene sc;
        memset(&sc, 0, sizeof sc);
        room(&sc.v);
        for (int i = 0, n = atoi(argv[5]); i < n; i++) {
            sc.tick_ms = (unsigned)i * 16;
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF2A3038, 1.0f, 0);
            overlay_draw(dev, &sc);
            HRESULT hp = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
            if (FAILED(hp)) { printf("Present %d failed 0x%08lx\n", i, hp); break; }
        }
        printf("present loop done\n");
    }
    overlay_release();
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
