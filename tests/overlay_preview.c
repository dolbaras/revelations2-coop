/*
 * Renders the lobby overlay (native/overlay.c) into BMP files without the game: a hidden window, a Direct3D 9 device on the default adapter, a gradient as
 * the "game frame" and one screenshot per screen / language.
 *   build/overlay_preview.exe <outdir> [width height [g [present_frames [mode [device_resets]]]]]
 *   device_resets = n: n rounds of draw + overlay_release + IDirect3DDevice9::Reset + draw (the game's Reset path with a real device; fails when the overlay
 *                   left a D3DPOOL_DEFAULT resource alive, because Reset then answers D3DERR_INVALIDCALL)
 *   build/overlay_preview.exe --reset-cycle [<game arc folder> [width height]]
 *                   NO device: the lobby is rendered by the GDI backend with the game's own material (nativePCNext\arc\DX9NEXT), everything is released as before
 *                   a device Reset (overlay_release) and rendered again, 3 rounds over the 6 face classes; the process heaps must stay valid and the GDI object
 *                   count must not grow. tests/build.sh runs this one. (Rev1 e55ea25: a double free of the Japanese face, which shares the pointers of class 0,
 *                   crashed Rev1's live game at the first Reset with 0xC0000374.)
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
    row(v, "Moira", "Boris6", LV_GREY);
    row(v, "Barry", "", LV_NORMAL);
    row(v, "Natalia", "", LV_GREY);
    row(v, "Episode", "<  1  >", LV_NORMAL);
    row(v, "Difficulty", "<  Normal  >", LV_NORMAL);
    row(v, "Start Game", "", LV_GREY);
    row(v, "Leave", "", LV_NORMAL);
    v->sel = 0;
    v->nplayers = 2;
    snprintf(v->players[0].name, sizeof v->players[0].name, "Alisa77"), snprintf(v->players[0].chr, sizeof v->players[0].chr, "Claire");
    v->players[0].host = 1, v->players[0].me = 1, v->players[0].ping = -1;
    snprintf(v->players[1].name, sizeof v->players[1].name, "Boris6"), snprintf(v->players[1].chr, sizeof v->players[1].chr, "Moira");
    v->players[1].ready = 0, v->players[1].ping = 12;
    snprintf(v->status, sizeof v->status, "The game can start when your partner is ready.");
    snprintf(v->note, sizeof v->note, "%s", lobby_own_text(LT_SAVE_WARN, 0));
}

/* the room with the own texts of one language (the face class follows the language) */
static void room_lang(lobby_view *v, int lang)
{
    room(v);
    if (!lang) return;
    snprintf(v->status, sizeof v->status, "%s", lobby_own_text(LT_NEED_READY, lang));
    snprintf(v->note, sizeof v->note, "%s", lobby_own_text(LT_SAVE_WARN, lang));
    snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT, lang));
    snprintf(v->subtitle, sizeof v->subtitle, "%s: 192.168.1.5:28410", lobby_own_text(LT_YOUR_ADDR, lang));
    snprintf(v->col_name, sizeof v->col_name, "%s", lobby_own_text(LT_NAME, lang));
    snprintf(v->col_char, sizeof v->col_char, "%s", lobby_own_text(LT_CHARACTER, lang));
    snprintf(v->t_ready, sizeof v->t_ready, "%s", lobby_own_text(LT_PARTNER_READY, lang));
    snprintf(v->t_not_ready, sizeof v->t_not_ready, "%s", lobby_own_text(LT_NOT_READY, lang));
}

static int heaps_valid(void)
{
    HANDLE hs[128];
    DWORD n = GetProcessHeaps(128, hs);
    for (DWORD i = 0; i < n && i < 128; i++)
        if (!HeapValidate(hs[i], 0, NULL)) return 0;
    return 1;
}

/* the device Reset path without a device, see the header */
static int reset_cycle(const char *arcdir, int w, int h)
{
    static const int langs[] = {0, 5, 8, 9, 10, 11};   /* en, ru, ja (face class 1 = the pointers of class 0), ko, zh-Hans, zh-Hant */
    const int rounds = 3;
    DWORD gdi[3] = {0};
    double first_ms[3] = {0};
    int material = arcdir && arcdir[0];
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    if (material) overlay_set_game_dir(arcdir);
    for (int round = 0; round < rounds; round++) {
        for (unsigned i = 0; i < sizeof langs / sizeof langs[0]; i++) {
            ov_scene sc;
            memset(&sc, 0, sizeof sc);
            sc.lang = langs[i], sc.tick_ms = 600u + 1000u * (unsigned)round;
            room_lang(&sc.v, langs[i]);
            unsigned *px = NULL;
            LARGE_INTEGER q0, q1;
            QueryPerformanceCounter(&q0);
            int ok = overlay_render_to_bitmap(&sc, w, h, &px);
            QueryPerformanceCounter(&q1);
            if (i == 0) first_ms[round] = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
            if (!ok || !px) {
                printf("FAIL: render %d (language %d) after release\n", round, langs[i]);
                return 1;
            }
            unsigned c0 = px[0], same = 1;
            for (int k = 0; k < w * h && same; k += 97) same = px[k] == c0;
            if (same) {
                printf("FAIL: round %d language %d: an empty picture\n", round, langs[i]);
                return 1;
            }
        }
        char st[400];
        overlay_stats(st, sizeof st);
        if (material && !strstr(st, "game material: fonts ok title ok")) {
            printf("FAIL: round %d: the game's material did not load again after the release (%s)\n", round, st);
            return 1;
        }
        overlay_release();   /* what menu_before_reset() does before IDirect3DDevice9::Reset */
        if (!heaps_valid()) {
            printf("FAIL: round %d: a process heap is corrupt after overlay_release\n", round);
            return 1;
        }
        gdi[round] = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        printf("round %d: %s; GDI objects after the release %lu; first frame after the release %.1f ms\n", round, st, (unsigned long)gdi[round], first_ms[round]);
    }
    if (gdi[rounds - 1] > gdi[0]) {
        printf("FAIL: GDI objects grow by %lu per release round (%lu -> %lu)\n", (unsigned long)(gdi[rounds - 1] - gdi[0]) / (unsigned long)(rounds - 1), (unsigned long)gdi[0],
               (unsigned long)gdi[rounds - 1]);
        return 1;
    }
    printf("reset cycle: 3 x release + render ok (%s, heaps valid, GDI objects %lu -> %lu)\n", material ? "the game's own material" : "no game folder: fallback faces", (unsigned long)gdi[0],
           (unsigned long)gdi[rounds - 1]);
    return 0;
}

/* --banner-fit: every in-game strip text (0.2.0) with a long name fits the one-line strip at 16:9, 16:10, 4:3 and 5:4 (the strip cuts with an ellipsis otherwise) */
static int banner_fit(void)
{
    static const int texts[] = {LT_PARTNER_LEFT, LT_PARTNER_LOST, LT_HOST_ENDED, LT_HOST_LOST, LT_EP1_END, LT_WAIT_LOAD, LT_PARTNER_SILENT, LT_DOWN_SELF, LT_DOWN_PARTNER};
    static const struct { int w, h; const char *name; } screens[] = {{1920, 1080, "16:9"}, {1920, 1200, "16:10"}, {1440, 1080, "4:3"}, {1280, 1024, "5:4"}};
    const char *name = "Maximilian Grossmann 12";
    int bad = 0, n = 0, worst = 0;
    for (int lang = 0; lang < 12; lang++)
        for (unsigned k = 0; k < sizeof texts / sizeof texts[0]; k++) {
            char line[240];
            if (texts[k] == LT_PARTNER_SILENT) lobby_silent_text(name, 599, lang, line, sizeof line);
            else if (texts[k] == LT_DOWN_SELF || texts[k] == LT_DOWN_PARTNER) lobby_format(lobby_own_text(texts[k], lang), "59", NULL, line, sizeof line);
            else lobby_loss_line(texts[k], name, "Player", lang, line, sizeof line);
            for (unsigned sc = 0; sc < sizeof screens / sizeof screens[0]; sc++) {
                int room = 0, w = overlay_banner_measure(line, lang, screens[sc].w, screens[sc].h, &room);
                n++;
                if (w > 0 && room > 0 && w * 100 / room > worst) worst = w * 100 / room;
                if (w < 0 || w > room) {
                    bad++;
                    printf("TOO WIDE (%s): %s text %d: %d > %d px: %s\n", screens[sc].name, lobby_lang_names[lang], texts[k], w, room, line);
                }
            }
        }
    printf("banner fit: %d strip x screen checks (16:9 16:10 4:3 5:4), %d too wide, widest %d %% of the room\n", n, bad, worst);
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--banner-fit")) return banner_fit();
    if (argc > 1 && !strcmp(argv[1], "--reset-cycle")) return reset_cycle(argc > 2 ? argv[2] : "", argc > 4 ? atoi(argv[3]) : 1280, argc > 4 ? atoi(argv[4]) : 720);
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
    D3DPRESENT_PARAMETERS pp0 = pp;
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
            room_lang(v, shots[i].lang);
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
    if (argc > 7) {   /* the game's device Reset path with a real device (not run by tests/build.sh: it needs a Direct3D 9 device) */
        int rounds = atoi(argv[7]);
        for (int r = 0; r < rounds; r++) {
            ov_scene sc;
            memset(&sc, 0, sizeof sc);
            sc.lang = (r & 1) ? 8 : 0, sc.tick_ms = 600;
            room_lang(&sc.v, sc.lang);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF2A3038, 1.0f, 0);
            overlay_draw(dev, &sc);
            overlay_banner(dev, "Waiting for Boris6 to finish loading...", sc.lang);
            IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
            overlay_release();   /* menu_before_reset() */
            D3DPRESENT_PARAMETERS rp = pp0;
            HRESULT hrr = IDirect3DDevice9_Reset(dev, &rp);
            if (FAILED(hrr)) {
                printf("FAIL: device Reset %d -> 0x%08lx (a D3DPOOL_DEFAULT resource of the overlay still alive?)\n", r, (unsigned long)hrr);
                return 1;
            }
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF2A3038, 1.0f, 0);
            overlay_draw(dev, &sc);
            overlay_banner(dev, "Waiting for Boris6 to finish loading...", sc.lang);
            if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) {
                printf("FAIL: Present after Reset %d\n", r);
                return 1;
            }
        }
        printf("device reset cycle: %d x draw + release + Reset + draw ok\n", rounds);
    }
    overlay_release();
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
