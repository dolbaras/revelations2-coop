/*
 * Lobby overlay, see overlay.h. Layout is done in a virtual 1920x1080 space (16:9, centred on wider windows) and scaled to the back buffer.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dx9core.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "r2.h"

typedef HRESULT(WINAPI *CreateFontW_t)(LPDIRECT3DDEVICE9, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPD3DXFONT *);
typedef HRESULT(WINAPI *CreateSprite_t)(LPDIRECT3DDEVICE9, LPD3DXSPRITE *);

static HMODULE g_d3dx;
static CreateFontW_t pCreateFontW;
static CreateSprite_t pCreateSprite;
static int g_tried, g_ok;
static IDirect3DDevice9 *g_dev;
static ID3DXSprite *g_sprite;
static IDirect3DTexture9 *g_white;
static struct {
    int px, bold, cls;
    ID3DXFont *f;
} g_fonts[16];
static unsigned g_frames, g_fail_font, g_fail_sprite, g_fail_tex, g_bd_ok, g_bd_fail;
static float g_scale = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
static int g_bbw = 1920, g_bbh = 1080;
static int g_mode = 3, g_trace;
#define TRACE(...) do { if (g_trace > 0) r2_log("overlay: " __VA_ARGS__); } while (0)
void overlay_set_mode(int mode) { g_mode = mode; }
void overlay_set_trace(int frames) { g_trace = frames; }
static struct { int x, y, w, h; } g_hit[LOBBY_ROWS];
static int g_hit_n;

static int face_class(int lang) { return lang == 8 ? 1 : lang == 9 ? 2 : lang == 10 ? 3 : lang == 11 ? 4 : 0; }
static const wchar_t *face_name(int cls)
{
    switch (cls) {
    case 1: return L"Yu Gothic UI";
    case 2: return L"Malgun Gothic";
    case 3: return L"Microsoft YaHei UI";
    case 4: return L"Microsoft JhengHei UI";
    default: return L"Segoe UI";
    }
}

static void gdi_free(void);
static void banner_free(void);
static void rt_free(void);
void overlay_release(void)
{
    rt_free();
    gdi_free();
    banner_free();
    for (unsigned i = 0; i < sizeof g_fonts / sizeof g_fonts[0]; i++)
        if (g_fonts[i].f) ID3DXFont_Release(g_fonts[i].f), memset(&g_fonts[i], 0, sizeof g_fonts[i]);
    if (g_sprite) ID3DXSprite_Release(g_sprite), g_sprite = NULL;
    if (g_white) IDirect3DTexture9_Release(g_white), g_white = NULL;
    g_dev = NULL;
}

static int load_d3dx(void)
{
    if (g_tried) return g_ok;
    g_tried = 1;
    g_d3dx = LoadLibraryA("d3dx9_43.dll");
    if (g_d3dx) {
        pCreateFontW = (CreateFontW_t)GetProcAddress(g_d3dx, "D3DXCreateFontW");
        pCreateSprite = (CreateSprite_t)GetProcAddress(g_d3dx, "D3DXCreateSprite");
    }
    g_ok = pCreateFontW && pCreateSprite;
    r2_log("overlay: d3dx9_43.dll %s", g_ok ? "loaded" : "NOT available, no lobby overlay");
    return g_ok;
}

static int ensure_resources(IDirect3DDevice9 *dev)
{
    if (!load_d3dx()) return 0;
    if (g_dev && g_dev != dev) overlay_release();
    if (!g_sprite) {
        if (FAILED(pCreateSprite(dev, &g_sprite))) {
            g_fail_sprite++;
            return 0;
        }
        g_dev = dev;
    }
    if (!g_white) {
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_white, NULL))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DTexture9_LockRect(g_white, 0, &lr, NULL, 0))) {
                *(DWORD *)lr.pBits = 0xFFFFFFFF;
                IDirect3DTexture9_UnlockRect(g_white, 0);
            }
        } else {
            g_fail_tex++;
            g_white = NULL;
        }
    }
    return g_sprite != NULL && g_white != NULL;
}

static ID3DXFont *font_get(IDirect3DDevice9 *dev, int px, int bold, int cls)
{
    unsigned free_slot = (unsigned)-1;
    for (unsigned i = 0; i < sizeof g_fonts / sizeof g_fonts[0]; i++) {
        if (g_fonts[i].f && g_fonts[i].px == px && g_fonts[i].bold == bold && g_fonts[i].cls == cls) return g_fonts[i].f;
        if (!g_fonts[i].f && free_slot == (unsigned)-1) free_slot = i;
    }
    if (free_slot == (unsigned)-1) {   /* more sizes than slots: drop everything and start over */
        for (unsigned i = 0; i < sizeof g_fonts / sizeof g_fonts[0]; i++)
            if (g_fonts[i].f) ID3DXFont_Release(g_fonts[i].f), memset(&g_fonts[i], 0, sizeof g_fonts[i]);
        free_slot = 0;
    }
    ID3DXFont *f = NULL;
    if (FAILED(pCreateFontW(dev, px, 0, bold ? FW_BOLD : FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_name(cls), &f))) {
        g_fail_font++;
        return NULL;
    }
    g_fonts[free_slot].px = px, g_fonts[free_slot].bold = bold, g_fonts[free_slot].cls = cls, g_fonts[free_slot].f = f;
    return f;
}

/* ------------------------------------------------------------------ drawing helpers (virtual coordinates) */
static IDirect3DDevice9 *D;
static int g_cls;

/* ------------------------------------------------------------------ backend 3: GDI into a bitmap, copied to the back buffer with StretchRect
 * No sprite, no BeginScene, no render state touched: the screen is composed on the CPU (opaque) and blitted by the device. */
static struct {
    HDC dc;
    HBITMAP bmp;
    unsigned *px;
    int w, h, active;
    IDirect3DSurface9 *sys, *def;
    D3DFORMAT fmt;
    unsigned sig;
    int have_sig;
    unsigned *bg, *cap;     /* backdrop (the game frame blurred, darkened, tinted) and the raw capture, w * h */
    unsigned bg_stamp, last_tick;
    int bg_shots, bg_final;   /* bg_final: the last capture was a real picture (not a fade to black): no more shots */
    struct { int px, role, cls; HFONT f; } fonts[40];
} G;

static void gdi_free(void)
{
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
        if (G.fonts[i].f) DeleteObject(G.fonts[i].f);
    if (G.sys) IDirect3DSurface9_Release(G.sys);
    if (G.def) IDirect3DSurface9_Release(G.def);
    if (G.dc) DeleteDC(G.dc);
    if (G.bmp) DeleteObject(G.bmp);
    free(G.bg), free(G.cap);
    memset(&G, 0, sizeof G);
}

static void gdi_rect(float x, float y, float w, float h, DWORD argb)
{
    int x0 = (int)(g_ox + x * g_scale), y0 = (int)(g_oy + y * g_scale), x1 = (int)(g_ox + (x + w) * g_scale + 0.5f), y1 = (int)(g_oy + (y + h) * g_scale + 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.w) x1 = G.w;
    if (y1 > G.h) y1 = G.h;
    unsigned a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    for (int yy = y0; yy < y1; yy++) {
        unsigned *row = G.px + (size_t)yy * (size_t)G.w;
        for (int xx = x0; xx < x1; xx++) {
            if (a >= 255) row[xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            else {
                unsigned d = row[xx];
                unsigned dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                row[xx] = 0xFF000000u | (((r * a + dr * (255 - a)) / 255) << 16) | (((g * a + dg * (255 - a)) / 255) << 8) | ((b * a + db * (255 - a)) / 255);
            }
        }
    }
}

/* faces: Segoe UI (role 0 semibold: title, selected row, labels; role 1 regular) and the CJK system UI faces; the game's own face is a texture atlas that GDI cannot load */
static void fontspec(int cls, int role, const wchar_t **face, int *weight)
{
    if (cls == 0) *face = role == 0 ? L"Segoe UI Semibold" : L"Segoe UI", *weight = 0;
    else *face = face_name(cls), *weight = role == 0 ? FW_SEMIBOLD : FW_NORMAL;
}

static HFONT gdi_font(int px, int role, int cls)
{
    unsigned free_slot = (unsigned)-1;
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++) {
        if (G.fonts[i].f && G.fonts[i].px == px && G.fonts[i].role == role && G.fonts[i].cls == cls) return G.fonts[i].f;
        if (!G.fonts[i].f && free_slot == (unsigned)-1) free_slot = i;
    }
    if (free_slot == (unsigned)-1) {
        for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
            if (G.fonts[i].f) DeleteObject(G.fonts[i].f), G.fonts[i].f = NULL;
        free_slot = 0;
    }
    const wchar_t *face;
    int weight;
    fontspec(cls, role, &face, &weight);
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) {
        g_fail_font++;
        return NULL;
    }
    G.fonts[free_slot].px = px, G.fonts[free_slot].role = role, G.fonts[free_slot].cls = cls, G.fonts[free_slot].f = f;
    return f;
}

static void gdi_text(const wchar_t *ws, float x, float y, float w, float h, int px, int bold, DWORD color, DWORD fmt)
{
    HFONT f = gdi_font((int)(px * g_scale + 0.5f), bold, g_cls);
    if (!f) return;
    HGDIOBJ old = SelectObject(G.dc, f);
    SetBkMode(G.dc, TRANSPARENT);
    SetTextColor(G.dc, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
    RECT rc = {(LONG)(g_ox + x * g_scale), (LONG)(g_oy + y * g_scale), (LONG)(g_ox + (x + w) * g_scale), (LONG)(g_oy + (y + h) * g_scale)};
    DrawTextW(G.dc, ws, -1, &rc, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOCLIP | DT_NOPREFIX);
    SelectObject(G.dc, old);
}


static void rect(float x, float y, float w, float h, DWORD argb)
{
    if (G.active) {
        gdi_rect(x, y, w, h, argb);
        return;
    }
    D3DXMATRIX m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = w * g_scale, m.m[1][1] = h * g_scale, m.m[2][2] = 1.0f, m.m[3][3] = 1.0f;
    m.m[3][0] = g_ox + x * g_scale, m.m[3][1] = g_oy + y * g_scale;
    ID3DXSprite_SetTransform(g_sprite, &m);
    ID3DXSprite_Draw(g_sprite, (LPDIRECT3DTEXTURE9)g_white, NULL, NULL, NULL, argb);
}

static void frame(float x, float y, float w, float h, float t, DWORD argb)
{
    rect(x, y, w, t, argb), rect(x, y + h - t, w, t, argb), rect(x, y, t, h, argb), rect(x + w - t, y, t, h, argb);
}

/* text in a box; fmt = DT_LEFT / DT_CENTER / DT_RIGHT; the vertical position is centred */
static void text(const char *s, float x, float y, float w, float h, int px, int bold, DWORD color, DWORD fmt)
{
    if (!s || !s[0] || g_mode == 1) return;
    wchar_t ws[400];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 399);
    if (n <= 0) return;
    if (G.active) {
        gdi_text(ws, x, y, w, h, px, bold, color, fmt);
        return;
    }
    ID3DXFont *f = font_get(D, (int)(px * g_scale + 0.5f), bold, g_cls);
    if (!f) return;
    D3DXMATRIX id;
    memset(&id, 0, sizeof id);
    id.m[0][0] = id.m[1][1] = id.m[2][2] = id.m[3][3] = 1.0f;
    ID3DXSprite_SetTransform(g_sprite, &id);
    RECT rc = {(LONG)(g_ox + x * g_scale), (LONG)(g_oy + y * g_scale), (LONG)(g_ox + (x + w) * g_scale), (LONG)(g_oy + (y + h) * g_scale)};
    ID3DXFont_DrawTextW(f, g_sprite, ws, -1, &rc, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOCLIP, color);
}

#define COL_TEXT 0xFFE8ECEF
#define COL_GREY 0xFF6A7178
#define COL_INFO 0xFFB8C2CC
#define COL_ACCENT 0xFFC0392B
#define COL_SEL 0xCCA82020

static DWORD status_color(int kind) { return kind == LSK_ERROR ? 0xFFFF7566 : kind == LSK_OK ? 0xFF86E394 : 0xFFD6DDE3; }

/* rows of a list at (x, y); returns the y below the list */
static float draw_rows(const lobby_view *v, float x, float y, float w, float rh, float step, int px)
{
    g_hit_n = 0;
    for (int i = 0; i < v->nrows; i++) {
        const lobby_row *r = &v->rows[i];
        int sel = i == v->sel;
        if (sel) {
            rect(x, y, w, rh, COL_SEL);
            rect(x, y, 6, rh, 0xFFFF4A3A);
        } else
            rect(x, y, w, rh, 0x40000000);
        DWORD col = r->kind == LV_GREY ? COL_GREY : r->kind == LV_INFO ? COL_INFO : COL_TEXT;
        text(r->label, x + 28, y, w * 0.58f, rh, px, sel, col, DT_LEFT);
        if (r->value[0]) text(r->value, x + w * 0.4f, y, w * 0.6f - 24, rh, px - 4, 0, r->kind == LV_GREY ? COL_GREY : (sel ? 0xFFFFFFFF : COL_INFO), DT_RIGHT);
        if (g_hit_n < LOBBY_ROWS) g_hit[g_hit_n].x = (int)x, g_hit[g_hit_n].y = (int)y, g_hit[g_hit_n].w = (int)w, g_hit[g_hit_n].h = (int)rh, g_hit_n++;
        y += step;
    }
    return y;
}

static void draw_players(const lobby_view *v, float x, float y, float w)
{
    if (!v->nplayers) return;
    rect(x, y, w, 56 + 64 * (float)v->nplayers + 12, 0x90101418);
    frame(x, y, w, 56 + 64 * (float)v->nplayers + 12, 2, 0xFF3A424A);
    text(v->col_name, x + 24, y, 300, 56, 24, 0, COL_INFO, DT_LEFT);
    text(v->col_char, x + 330, y, 200, 56, 24, 0, COL_INFO, DT_LEFT);
    text(v->col_ping, x + w - 130, y, 106, 56, 24, 0, COL_INFO, DT_RIGHT);
    rect(x + 16, y + 54, w - 32, 2, 0xFF3A424A);
    y += 62;
    for (int i = 0; i < v->nplayers; i++) {
        const lobby_vplayer *p = &v->players[i];
        char nm[80], pg[24];
        snprintf(nm, sizeof nm, "%s%s", p->name, p->host ? "" : "");
        text(nm, x + 24, y, 290, 64, 32, p->me, p->me ? 0xFFFFFFFF : COL_TEXT, DT_LEFT);
        text(p->chr, x + 330, y + 2, 190, 36, 30, 0, COL_TEXT, DT_LEFT);
        if (p->host) text(v->t_host, x + 330, y + 36, 190, 26, 20, 0, COL_INFO, DT_LEFT);
        else text(p->ready ? v->t_ready : v->t_not_ready, x + 330, y + 36, 190, 26, 20, 0, p->ready ? 0xFF86E394 : 0xFFB8C2CC, DT_LEFT);
        if (p->ping >= 0) snprintf(pg, sizeof pg, "%d ms", p->ping);
        else snprintf(pg, sizeof pg, "%s", p->me ? "" : "-");
        text(pg, x + w - 150, y, 126, 64, 26, 0, COL_INFO, DT_RIGHT);
        y += 64;
    }
}

static void spinner_text(char *out, size_t n, const char *base, unsigned tick, int busy)
{
    static const char *const dots[] = {"", ".", "..", "..."};
    snprintf(out, n, "%s%s", base, busy ? dots[(tick / 350) % 4] : "");
}

static void draw_full(const ov_scene *sc)
{
    const lobby_view *v = &sc->v;
    rect(-400, -100, 2720, 1280, 0xE0090B0E);   /* covers the letterbox bars too */
    text(v->title, 160, 70, 1100, 90, 56, 1, 0xFFFFFFFF, DT_LEFT);
    rect(160, 164, 560, 4, COL_ACCENT);
    if (v->subtitle[0]) text(v->subtitle, 160, 178, 1640, 44, 26, 0, COL_INFO, DT_LEFT);
    float y = 250;
    if (v->input_on) {
        text(v->prompt, 160, y, 1200, 44, 28, 0, COL_INFO, DT_LEFT);
        rect(160, y + 52, 800, 68, 0xFF1B2025);
        frame(160, y + 52, 800, 68, 2, 0xFF5A646E);
        char in[120];
        snprintf(in, sizeof in, "%s%s", v->input, ((sc->tick_ms / 500) & 1) ? "_" : " ");
        text(in, 180, y + 52, 760, 68, 36, 0, 0xFFFFFFFF, DT_LEFT);
        y += 160;
    } else if (v->prompt[0]) {
        text(v->prompt, 160, y, 1200, 56, 36, 1, COL_TEXT, DT_LEFT);
        y += 90;
    }
    draw_rows(v, 160, y, 800, 60, 70, 34);
    if (v->nplayers) draw_players(v, 1040, 250, 720);
    char st[300];
    spinner_text(st, sizeof st, v->status, sc->tick_ms, v->busy);
    if (v->status[0] || v->busy) text(st, 160, 880, 1600, 60, 32, 0, status_color(v->status_kind), DT_LEFT);
    if (v->note[0]) text(v->note, 160, 940, 1640, 36, 22, 0, 0xFFC9A66B, DT_LEFT);
    text(v->hint, 160, 990, 1640, 40, 22, 0, 0xFF7E8890, DT_LEFT);
    if (sc->cursor_on) {   /* a small pointer while the mouse is in use (the game's own cursor is not drawn on this screen) */
        float cx = (float)sc->cursor_x, cy = (float)sc->cursor_y;
        rect(cx - 1, cy - 12, 3, 25, 0xFFFFFFFF), rect(cx - 12, cy - 1, 25, 3, 0xFFFFFFFF);
        rect(cx, cy - 11, 1, 23, 0xFFC0392B), rect(cx - 11, cy, 23, 1, 0xFFC0392B);
    }
}


/* ------------------------------------------------------------------ the styled screen (GDI backend)
 * Drawn in the look of the game's own menus (pause menu, options pages): the game picture blurred, darkened and tinted rust-orange, a wide letter-spaced capital title with an
 * orange glow, a left-hand list with an orange bar and glow on the selected row, thin orange frames with bright corner brackets, orange key caps in a legend band at the bottom.
 * Everything is composed on the CPU into G.px (opaque, copied by StretchRect). Fonts: Segoe UI (every Windows ships it), the CJK system UI faces. */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
enum { SA_LEFT = 0, SA_CENTER = 1, SA_RIGHT = 2 };
enum { SF_UP = 1, SF_WRAP = 2, SF_GLOW = 4 };

static void s_put(int x, int y, unsigned rgb, unsigned a)
{
    if (x < 0 || y < 0 || x >= G.w || y >= G.h || a == 0) return;
    unsigned *p = G.px + (size_t)y * (size_t)G.w + (size_t)x;
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (a >= 255) {
        *p = 0xFF000000u | rgb;
        return;
    }
    unsigned d = *p, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    *p = 0xFF000000u | (((r * a + dr * (255 - a)) / 255) << 16) | (((g * a + dg * (255 - a)) / 255) << 8) | ((b * a + db * (255 - a)) / 255);
}

/* filled rectangle; fade: 0 none, 1 alpha falls to 0 towards the right end, 2 towards both ends (horizontal), a = 0..255 */
static void s_rect(float x, float y, float w, float h, unsigned rgb, float a, int fade)
{
    int x0 = (int)(g_ox + x * g_scale), y0 = (int)(g_oy + y * g_scale), x1 = (int)(g_ox + (x + w) * g_scale + 0.5f), y1 = (int)(g_oy + (y + h) * g_scale + 0.5f);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float k = 1.0f;
            if (fade) {
                float t = (float)(xx - x0) / (float)(x1 - x0);
                k = fade == 1 ? 1.0f - t : 1.0f - fabsf(t * 2.0f - 1.0f);
                k = k * k * (3 - 2 * k);
            }
            s_put(xx, yy, rgb, (unsigned)(a * k));
        }
}

static void s_frame(float x, float y, float w, float h, float t, unsigned rgb, float a)
{
    s_rect(x, y, w, t, rgb, a, 0), s_rect(x, y + h - t, w, t, rgb, a, 0), s_rect(x, y, t, h, rgb, a, 0), s_rect(x + w - t, y, t, h, rgb, a, 0);
}

static void s_disc(float cx, float cy, float r, unsigned rgb, float amax)
{
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, dr = r * g_scale;
    for (int y = (int)(dcy - dr) - 1; y <= (int)(dcy + dr) + 1; y++)
        for (int x = (int)(dcx - dr) - 1; x <= (int)(dcx + dr) + 1; x++) {
            float d = sqrtf(((float)x + 0.5f - dcx) * ((float)x + 0.5f - dcx) + ((float)y + 0.5f - dcy) * ((float)y + 0.5f - dcy));
            float k = clampf(dr - d + 0.5f, 0, 1);
            if (k > 0) s_put(x, y, rgb, (unsigned)(amax * k));
        }
}

/* soft glow blob (elliptical, quadratic falloff) */
static void s_glow(float cx, float cy, float rx, float ry, unsigned rgb, float amax)
{
    int x0 = (int)(g_ox + (cx - rx) * g_scale), x1 = (int)(g_ox + (cx + rx) * g_scale) + 1, y0 = (int)(g_oy + (cy - ry) * g_scale), y1 = (int)(g_oy + (cy + ry) * g_scale) + 1;
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, drx = rx * g_scale, dry = ry * g_scale;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float nx = ((float)x - dcx) / drx, ny = ((float)y - dcy) / dry, d = sqrtf(nx * nx + ny * ny);
            if (d >= 1.0f) continue;
            float k = 1.0f - d;
            s_put(x, y, rgb, (unsigned)(amax * k * k));
        }
}

static void s_blur(unsigned char *buf, int w, int h, int r)
{
    if (r < 1) return;
    unsigned char *tmp = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            unsigned char *s = buf + (size_t)y * (size_t)w, *d = tmp + (size_t)y * (size_t)w;
            int sum = 0;
            for (int x = -r; x <= r; x++) sum += s[x < 0 ? 0 : x >= w ? w - 1 : x];
            for (int x = 0; x < w; x++) {
                d[x] = (unsigned char)(sum / (2 * r + 1));
                int a = x + r + 1, b = x - r;
                sum += s[a >= w ? w - 1 : a] - s[b < 0 ? 0 : b];
            }
        }
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int y = -r; y <= r; y++) sum += tmp[(size_t)(y < 0 ? 0 : y >= h ? h - 1 : y) * (size_t)w + (size_t)x];
            for (int y = 0; y < h; y++) {
                buf[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)(sum / (2 * r + 1));
                int a = y + r + 1, b = y - r;
                sum += tmp[(size_t)(a >= h ? h - 1 : a) * (size_t)w + (size_t)x] - tmp[(size_t)(b < 0 ? 0 : b) * (size_t)w + (size_t)x];
            }
        }
    }
    free(tmp);
}

static void s_glow_run(const wchar_t *ws, int n, HFONT f, int extra, int x0, int base, int width, unsigned rgb, float strength)
{
    TEXTMETRICW tm;
    SelectObject(G.dc, f);
    GetTextMetricsW(G.dc, &tm);
    int rad = tm.tmHeight / 5 > 2 ? tm.tmHeight / 5 : 2, pad = rad * 3;
    int gw = width + 2 * pad, gh = tm.tmHeight + 2 * pad;
    if (gw < 4 || gh < 4 || gw > 3000 || gh > 600) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = gw, bi.bmiHeader.biHeight = -gh, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    unsigned *px = NULL;
    HDC sdc = GetDC(NULL);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, sdc);
    unsigned char *m = (unsigned char *)malloc((size_t)gw * (size_t)gh);
    if (dc && bmp && px && m) {
        HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, f);
        memset(px, 0, (size_t)gw * (size_t)gh * 4);
        SetBkMode(dc, TRANSPARENT), SetTextColor(dc, RGB(255, 255, 255)), SetTextCharacterExtra(dc, extra), SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        ExtTextOutW(dc, pad, pad + tm.tmAscent, 0, NULL, ws, (UINT)n, NULL);
        GdiFlush();
        for (int i = 0; i < gw * gh; i++) m[i] = (unsigned char)(px[i] & 255);
        s_blur(m, gw, gh, rad);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++) {
                unsigned a = (unsigned)(m[y * gw + x] * strength * 2.2f);
                if (a > 255) a = 255;
                s_put(x0 - pad + x, base - tm.tmAscent - pad + y, rgb, a);
            }
        SelectObject(dc, of), SelectObject(dc, ob);
    }
    free(m);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
}

static float g_glow = 0.8f;

/* ------------------------------------------------------------------ the game's own material (gametex.c)
 * Read on first use from the installed game's archives (<exe folder>\nativePCNext\arc\DX9NEXT: Msg_<lang>.arc for the menu face sys_eng / sys_kor / sys_chS / sys_chT, Core.arc for the
 * title face "deco" (its orange glow is part of the texture), the key caps of "pad_win" and the rust frame "frame00_ID"), decoded on the CPU and cached. Nothing is shipped.
 * Any failure leaves the pieces missing and the GDI / imitation drawing in charge. */
#include "gametex.h"

static char g_rt_dir[520];
void overlay_set_game_dir(const char *dir) { snprintf(g_rt_dir, sizeof g_rt_dir, "%s", dir ? dir : ""); }

typedef struct { int x, y, w, h; } rt_rect;
enum { SPR_KEY_F, SPR_KEY_ESC, SPR_COUNT };
static struct {
    int tried, ok, sprites_ok, frame_ok;
    gt_font *font[5][2];   /* [language class][0 = menu face (labels / text), 1 = the same face] */
    gt_font *deco;         /* the title face (Latin / Cyrillic) */
    gt_img pad, frame;
    rt_rect spr[SPR_COUNT];
    unsigned stat_real, stat_gdi, stat_deco;
    float capmid[5];
    float decomid;
} RT;

static void rt_free(void)
{
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 2; r++)
            if (RT.font[c][r] && !(c == 1 && r == 0) && !(r == 1)) gt_font_free(RT.font[c][r]);   /* [1][*] shares [0][*]; role 1 shares role 0 */
    gt_font_free(RT.deco);
    gt_img_free(&RT.pad), gt_img_free(&RT.frame);
    memset(&RT, 0, sizeof RT);
}

static rt_rect rt_tighten(const gt_img *im, int x, int y, int w, int h)
{
    int x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
    for (int yy = y; yy < y + h && yy < im->h; yy++)
        for (int xx = x; xx < x + w && xx < im->w; xx++)
            if ((im->px[(size_t)yy * (size_t)im->w + (size_t)xx] >> 24) > 12) {
                if (xx < x0) x0 = xx;
                if (xx > x1) x1 = xx;
                if (yy < y0) y0 = yy;
                if (yy > y1) y1 = yy;
            }
    rt_rect r = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    if (x1 < 0) r.w = r.h = 0;
    return r;
}

static void rt_init(void)
{
    if (RT.tried) return;
    RT.tried = 1;
    char dir[520];
    if (g_rt_dir[0]) snprintf(dir, sizeof dir, "%s", g_rt_dir);
    else {
        char exe[520];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = n ? strrchr(exe, '\\') : NULL;
        if (!slash) return;
        *slash = 0;
        snprintf(dir, sizeof dir, "%s\\nativePCNext\\arc\\DX9NEXT", exe);
    }
    char arc[520];
    static const char *const msg[5] = {"Msg_eng.arc", "Msg_eng.arc", "Msg_kor.arc", "Msg_chS.arc", "Msg_chT.arc"};
    static const char *const face[5] = {"sys_eng", "sys_eng", "sys_kor", "sys_chS", "sys_chT"};
    for (int c = 0; c < 5; c++) {
        if (c == 1) {
            RT.font[1][0] = RT.font[0][0];   /* Japanese lives in sys_eng too */
            continue;
        }
        char gfd[64], pages[96];
        snprintf(arc, sizeof arc, "%s\\%s", dir, msg[c]);
        snprintf(gfd, sizeof gfd, "ui_N\\00_font\\%s", face[c]);
        snprintf(pages, sizeof pages, "ui_N\\00_font\\%s_%%02d_AM_NOMIP", face[c]);
        RT.font[c][0] = gt_font_open(arc, gfd, pages);
    }
    RT.ok = RT.font[0][0] != NULL;
    snprintf(arc, sizeof arc, "%s\\Core.arc", dir);
    RT.deco = gt_font_open(arc, "ui_N\\00_font\\deco", "ui_N\\00_font\\deco_%02d_ID");
    unsigned size = 0;
    unsigned char *t = gt_arc_read(arc, "ui_N\\00_font\\pad_win_00_ID_HQ", &size);
    if (t) {
        if (gt_tex_decode(t, size, &RT.pad) && RT.pad.w == 1024 && RT.pad.h == 1024) {
            RT.spr[SPR_KEY_F] = rt_tighten(&RT.pad, 884, 324, 72, 72);
            RT.spr[SPR_KEY_ESC] = rt_tighten(&RT.pad, 244, 564, 72, 72);
            RT.sprites_ok = RT.spr[SPR_KEY_F].w > 30 && RT.spr[SPR_KEY_ESC].w > 30;
        }
        free(t);
    }
    t = gt_arc_read(arc, "ui_N\\10_tex\\frame00_ID", &size);
    if (t) {
        RT.frame_ok = gt_tex_decode(t, size, &RT.frame) && RT.frame.w == 1280 && RT.frame.h == 720;
        free(t);
    }
    r2_log("overlay: game material: fonts %s, title face %s, key caps %s, rust frame %s (%s)", RT.ok ? "ok" : "missing", RT.deco ? "ok" : "missing", RT.sprites_ok ? "ok" : "missing", RT.frame_ok ? "ok" : "missing", dir);
}

/* ---------------------------------------------------------------- sampling */
/* mode 0: coverage = alpha; 1: coverage = red (the menu face is white on black, DXT1); returns 0..1; for mode 2 (the title face, colour baked in) *rgb gets the colour */
static float rt_sample(const gt_img *pg, int mode, float u, float v, float x0, float y0, float x1, float y1, unsigned *rgb)
{
    if (u < x0) u = x0;
    if (v < y0) v = y0;
    if (u > x1) u = x1;
    if (v > y1) v = y1;
    float fu = u - 0.5f, fv = v - 0.5f;
    int iu = (int)floorf(fu), iv = (int)floorf(fv);
    fu -= (float)iu, fv -= (float)iv;
    int iu0 = iu < 0 ? 0 : iu, iv0 = iv < 0 ? 0 : iv;
    int iu1 = iu + 1 >= pg->w ? pg->w - 1 : iu + 1, iv1 = iv + 1 >= pg->h ? pg->h - 1 : iv + 1;
    if (iu1 < iu0) iu1 = iu0;
    if (iv1 < iv0) iv1 = iv0;
    unsigned p00 = pg->px[(size_t)iv0 * (size_t)pg->w + (size_t)iu0], p10 = pg->px[(size_t)iv0 * (size_t)pg->w + (size_t)iu1];
    unsigned p01 = pg->px[(size_t)iv1 * (size_t)pg->w + (size_t)iu0], p11 = pg->px[(size_t)iv1 * (size_t)pg->w + (size_t)iu1];
    float w00 = (1 - fu) * (1 - fv), w10 = fu * (1 - fv), w01 = (1 - fu) * fv, w11 = fu * fv;
    if (mode == 2) {
        float a = ((float)(p00 >> 24) * w00 + (float)(p10 >> 24) * w10 + (float)(p01 >> 24) * w01 + (float)(p11 >> 24) * w11) / 255.0f;
        float r = ((float)((p00 >> 16) & 255) * w00 + (float)((p10 >> 16) & 255) * w10 + (float)((p01 >> 16) & 255) * w01 + (float)((p11 >> 16) & 255) * w11);
        float g = ((float)((p00 >> 8) & 255) * w00 + (float)((p10 >> 8) & 255) * w10 + (float)((p01 >> 8) & 255) * w01 + (float)((p11 >> 8) & 255) * w11);
        float b = ((float)(p00 & 255) * w00 + (float)(p10 & 255) * w10 + (float)(p01 & 255) * w01 + (float)(p11 & 255) * w11);
        if (rgb) *rgb = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
        return a;
    }
    int sh = mode == 1 ? 16 : 24;
    return (((float)((p00 >> sh) & 255)) * w00 + ((float)((p10 >> sh) & 255)) * w10 + ((float)((p01 >> sh) & 255)) * w01 + ((float)((p11 >> sh) & 255)) * w11) / 255.0f;
}

typedef struct { const gt_glyph *g; const gt_img *pg; int mode; } rt_g;

/* role 2 = the title face (falls back to the menu face), else the menu face of the language class */
static int rt_find(int cls, int role, unsigned cp, rt_g *out)
{
    if (cls == 1 && cp >= 0x3000) return 0;   /* Japanese kana / kanji: the atlas at menu size is too coarse in the preview, GDI keeps those strings until checked live */
    for (int k = 0; k < 2; k++) {
        gt_font *f = (role == 2 && k == 0) ? RT.deco : RT.font[cls][0];
        int mode = (role == 2 && k == 0) ? 2 : 1;
        if (!f) continue;
        const gt_glyph *g = gt_font_find(f, cp);
        if (!g) continue;
        if (g->w == 0) {
            if (cp == 0x20) {
                out->g = g, out->pg = NULL, out->mode = mode;
                return 1;
            }
            continue;
        }
        const gt_img *pg = gt_font_page(f, g->page);
        if (!pg || g->x + g->w > pg->w || g->y4 / 4 + g->h4 / 4 > pg->h) continue;
        out->g = g, out->pg = pg, out->mode = mode;
        return 1;
    }
    return 0;
}

/* a pixel size we are asked for (px) maps to this fraction of the atlas cell height */
#define RT_EM_OF_CELL 0.84f
static float rt_em(int cls, int role)
{
    gt_font *f = role == 2 && RT.deco ? RT.deco : RT.font[cls][0];
    int cell = gt_font_cell(f);
    return (cell > 0 ? (float)cell : 32.0f) * RT_EM_OF_CELL;
}

static float rt_capmid(int cls, int role)
{
    float *slot = role == 2 && RT.deco ? &RT.decomid : &RT.capmid[cls];
    if (*slot > 0) return *slot;
    float mid = 16.0f;
    gt_font *f = role == 2 && RT.deco ? RT.deco : RT.font[cls][0];
    const gt_glyph *g = gt_font_find(f, 'H');
    const gt_img *pg = g ? gt_font_page(f, g->page) : NULL;
    if (g && pg && g->y4 / 4 + g->h4 / 4 <= pg->h) {
        int top = -1, bot = -1, gy = g->y4 / 4, gh = g->h4 / 4, ch = (role == 2 && RT.deco) ? 24 : 16;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < g->w; x++)
                if (((pg->px[(size_t)(gy + y) * (size_t)pg->w + (size_t)(g->x + x)] >> ch) & 255) > 150) {
                    if (top < 0) top = y;
                    bot = y;
                    break;
                }
        if (top >= 0) mid = ((float)top + (float)bot + 1.0f) * 0.5f;
    }
    *slot = mid;
    return mid;
}

static float rt_measure(const wchar_t *ws, int n, int cls, int role, float s, float extra, rt_g *gl)
{
    float w = 0;
    for (int i = 0; i < n; i++) {
        if (!rt_find(cls, role, ws[i], &gl[i])) return -1;
        w += (float)gl[i].g->adv * s + extra;
    }
    return w - (n ? extra : 0);
}

static void rt_draw_run(const rt_g *gl, int n, float x, float ytop, float s, float extra, unsigned rgb, float amul, unsigned char *mask, int mw, int mh, int mox, int moy)
{
    for (int i = 0; i < n; i++) {
        if (gl[i].pg) {
            const gt_glyph *g = gl[i].g;
            float gw = (float)g->w * s, gh = (float)(g->h4 / 4) * s, gy0 = (float)(g->y4 / 4);
            int dx0 = (int)floorf(x), dy0 = (int)floorf(ytop), dx1 = (int)ceilf(x + gw), dy1 = (int)ceilf(ytop + gh);
            for (int dy = dy0; dy < dy1; dy++)
                for (int dx = dx0; dx < dx1; dx++) {
                    float u = (float)g->x + ((float)dx + 0.5f - x) / s, v = gy0 + ((float)dy + 0.5f - ytop) / s;
                    unsigned col = rgb;
                    float a = rt_sample(gl[i].pg, gl[i].mode, u, v, (float)g->x, gy0, (float)(g->x + g->w), gy0 + (float)(g->h4 / 4), &col) * amul;
                    if (a <= 0.01f) continue;
                    if (mask) {
                        int mx = dx - mox, my = dy - moy;
                        if (mx >= 0 && my >= 0 && mx < mw && my < mh) {
                            unsigned v8 = (unsigned)(a * 255.0f);
                            if (v8 > mask[my * mw + mx]) mask[my * mw + mx] = (unsigned char)v8;
                        }
                    } else
                        s_put(dx, dy, col & 0xFFFFFF, (unsigned)(a * 255.0f + 0.5f));
                }
        }
        x += (float)gl[i].g->adv * s + extra;
    }
}

static int rt_line(const wchar_t *ws, int n, float bx, float by, float bw, float bh, float pxd, int role, unsigned color, int align, float sp, int flags, float *width_out)
{
    if (!RT.ok || n <= 0 || n > 300) return 0;
    rt_g gl[300];
    int eff_role = role == 2 ? 2 : 1;
    float pxd0 = pxd;
    if (eff_role == 2) pxd *= 1.5f, sp *= 0.3f;   /* the title face is drawn larger and is wide by itself */
    float s = pxd / rt_em(g_cls, eff_role), extra = sp * pxd;
    float w = rt_measure(ws, n, g_cls, eff_role, s, extra, gl);
    if (w < 0 && eff_role == 2) {   /* a glyph the title face lacks: the menu face with our own glow */
        eff_role = 1, pxd = pxd0, sp /= 0.3f;
        s = pxd / rt_em(g_cls, 1), extra = sp * pxd;
        w = rt_measure(ws, n, g_cls, 1, s, extra, gl);
        flags |= SF_GLOW;
    }
    if (w < 0) return 0;
    if (width_out) {
        *width_out = w;
        return 1;
    }
    if (w > bw && bw > 0) {
        float k = bw / w;
        if (k < 0.6f) k = 0.6f;
        s *= k, extra *= k;
        w = rt_measure(ws, n, g_cls, eff_role, s, extra, gl);
    }
    float x = align == SA_CENTER ? bx + (bw - w) * 0.5f : align == SA_RIGHT ? bx + bw - w : bx;
    float ytop = by + bh * 0.5f - rt_capmid(g_cls, eff_role) * s;
    if (eff_role == 2) RT.stat_deco++;
    if (flags & SF_GLOW) {
        float cell = (float)gt_font_cell(RT.font[g_cls][0]) * s;
        int rad = (int)(pxd * 0.18f) > 2 ? (int)(pxd * 0.18f) : 2, pad = rad * 3;
        int mw = (int)w + 2 * pad + 2, mh = (int)cell + 2 * pad + 2;
        if (mw < 3000 && mh < 600) {
            unsigned char *m = (unsigned char *)calloc((size_t)mw * (size_t)mh, 1);
            if (m) {
                int mox = (int)floorf(x) - pad, moy = (int)floorf(ytop) - pad;
                rt_draw_run(gl, n, x, ytop, s, extra, 0xFFFFFF, 1.0f, m, mw, mh, mox, moy);
                s_blur(m, mw, mh, rad);
                for (int yy = 0; yy < mh; yy++)
                    for (int xx = 0; xx < mw; xx++) {
                        unsigned a = (unsigned)(m[yy * mw + xx] * g_glow * 2.2f);
                        if (a > 255) a = 255;
                        s_put(mox + xx, moy + yy, color & 0xFFFFFF, a);
                    }
                free(m);
            }
        }
    }
    rt_draw_run(gl, n, x, ytop, s, extra, color & 0xFFFFFF, 1.0f, NULL, 0, 0, 0, 0);
    return 1;
}

static int rt_is_break(wchar_t c) { return c == L' ' || c == 0x3000; }
static int rt_is_cjk(wchar_t c) { return c >= 0x2E80 && c <= 0xFFEF && c != 0xFF0C && c != 0x3001 && c != 0x3002; }

static int rt_wrap(const wchar_t *ws, int n, float bx, float by, float bw, float bh, float pxd, unsigned color, int align, float sp)
{
    if (!RT.ok || n <= 0 || n > 300) return 0;
    rt_g gl[300];
    float s = pxd / rt_em(g_cls, 1), minS = s * 0.6f;
    if (rt_measure(ws, n, g_cls, 1, s, 0, gl) < 0) return 0;
    for (int it = 0; it < 10; it++) {
        float extra = sp * s * rt_em(g_cls, 1), line_h = (float)gt_font_cell(RT.font[g_cls][0]) * s * 1.1f;
        int starts[64], ends[64], nl = 0, pos = 0;
        while (pos < n && nl < 64) {
            float w = 0;
            int last_ok = pos, i = pos;
            while (i < n) {
                float cw = (float)gl[i].g->adv * s + extra;
                if (w + cw > bw && i > pos) break;
                w += cw, i++;
                if (rt_is_break(ws[i - 1]) || (rt_is_cjk(ws[i - 1]) && i < n)) last_ok = i;
            }
            int e = i;
            if (i < n && last_ok > pos) e = last_ok;
            starts[nl] = pos, ends[nl] = e, nl++;
            pos = e;
            while (pos < n && ws[pos] == L' ') pos++;
        }
        if (((float)nl * line_h <= bh + 1.0f) || s <= minS) {
            for (int l = 0; l < nl; l++) {
                int a = starts[l], b = ends[l];
                while (b > a && ws[b - 1] == L' ') b--;
                float lw = 0;
                for (int i = a; i < b; i++) lw += (float)gl[i].g->adv * s + extra;
                lw -= b > a ? extra : 0;
                float x = align == SA_CENTER ? bx + (bw - lw) * 0.5f : align == SA_RIGHT ? bx + bw - lw : bx;
                rt_draw_run(gl + a, b - a, x, by + (float)l * line_h, s, extra, color & 0xFFFFFF, 1.0f, NULL, 0, 0, 0, 0);
            }
            return 1;
        }
        s *= 0.93f;
    }
    return 1;
}

/* a key cap of the pad_win atlas, stretched to (dx, dy, dw, dh) virtual units: the atlas stores the shape in alpha (the game colours it at run time), here tinted with rgb */
static void spr_key(int id, float dx, float dy, float dw, float dh, unsigned rgb, float gain)
{
    if (!RT.sprites_ok) return;
    const rt_rect *r = &RT.spr[id];
    float x0 = g_ox + dx * g_scale, y0 = g_oy + dy * g_scale, x1 = g_ox + (dx + dw) * g_scale, y1 = g_oy + (dy + dh) * g_scale;
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++)
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            float fx = ((float)x + 0.5f - x0) / (x1 - x0), fy = ((float)y + 0.5f - y0) / (y1 - y0);
            if (fx < 0 || fx > 1 || fy < 0 || fy > 1) continue;
            float a = rt_sample(&RT.pad, 0, (float)r->x + fx * (float)r->w, (float)r->y + fy * (float)r->h, (float)r->x, (float)r->y, (float)(r->x + r->w), (float)(r->y + r->h), NULL) * gain;
            if (a > 1.0f) a = 1.0f;
            if (a > 0.004f) s_put(x, y, rgb, (unsigned)(a * 255.0f));
        }
}

/* the rust / grunge frame of the game's menus baked into the backdrop buffer (it is made for 1280x720; stretched to the window); done once per capture */
static void rt_frame_into(unsigned *dst, int w, int h, float strength)
{
    if (!RT.frame_ok) return;
    for (int y = 0; y < h; y++) {
        float v = ((float)y + 0.5f) / (float)h * 720.0f;
        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) / (float)w * 1280.0f;
            unsigned col = 0;
            float a = rt_sample(&RT.frame, 2, u, v, 0, 0, 1280, 720, &col) * strength;
            if (a <= 0.004f) continue;
            unsigned d = dst[(size_t)y * (size_t)w + (size_t)x];
            float ia = 1.0f - a;
            unsigned r = (unsigned)(((float)((d >> 16) & 255)) * ia + (float)((col >> 16) & 255) * a), g = (unsigned)(((float)((d >> 8) & 255)) * ia + (float)((col >> 8) & 255) * a),
                     b = (unsigned)(((float)(d & 255)) * ia + (float)(col & 255) * a);
            dst[(size_t)y * (size_t)w + (size_t)x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* text in a box (virtual coordinates): one line aligned and shrunk to the box width, centred on the capital height; SF_WRAP: several lines from the top, shrunk to the box height.
 * sp = letter spacing in em (halved for CJK), SF_UP = capitals, SF_GLOW = soft glow in the text colour. color is 0xRRGGBB. */
static void stext(const char *s, float x, float y, float w, float h, int px, int role, unsigned color, int align, float sp, int flags)
{
    if (!s || !s[0]) return;
    wchar_t ws[700];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 699);
    if (n <= 1) return;
    n--;
    if (flags & SF_UP) CharUpperBuffW(ws, (DWORD)n);
    if (g_cls != 0) sp *= 0.5f;
    float bx = g_ox + x * g_scale, by = g_oy + y * g_scale, bw = w * g_scale, bh = h * g_scale;
    float pxd = (float)px * g_scale, pxmin = pxd * 0.6f;
    rt_init();
    if (RT.ok) {   /* the game's own face when every character is in it */
        int done = (flags & SF_WRAP) ? rt_wrap(ws, n, bx, by, bw, bh, pxd, color, align, sp) : rt_line(ws, n, bx, by, bw, bh, pxd, role, color, align, sp, flags, NULL);
        if (done) {
            RT.stat_real++;
            return;
        }
        RT.stat_gdi++;
    }
    if (role == 2) role = 0;
    SetBkMode(G.dc, TRANSPARENT);
    SetTextColor(G.dc, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
    if (flags & SF_WRAP) {
        HFONT f = NULL;
        RECT rc;
        SetTextAlign(G.dc, TA_LEFT | TA_TOP);
        for (int it = 0; it < 8; it++) {
            f = gdi_font((int)(pxd + 0.5f), role, g_cls);
            if (!f) return;
            SelectObject(G.dc, f);
            SetTextCharacterExtra(G.dc, (int)(sp * pxd));
            rc.left = 0, rc.top = 0, rc.right = (LONG)bw, rc.bottom = 0;
            DrawTextW(G.dc, ws, n, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
            if ((float)rc.bottom <= bh || pxd <= pxmin) break;
            pxd *= 0.93f;
        }
        rc.left = (LONG)bx, rc.top = (LONG)by, rc.right = (LONG)(bx + bw), rc.bottom = (LONG)(by + bh + 40);
        DrawTextW(G.dc, ws, n, &rc, DT_WORDBREAK | DT_NOPREFIX | (align == SA_CENTER ? DT_CENTER : align == SA_RIGHT ? DT_RIGHT : DT_LEFT));
        SetTextCharacterExtra(G.dc, 0);
        return;
    }
    HFONT f = NULL;
    SIZE sz = {0, 0};
    int extra = 0;
    for (int it = 0; it < 10; it++) {
        f = gdi_font((int)(pxd + 0.5f), role, g_cls);
        if (!f) return;
        SelectObject(G.dc, f);
        extra = (int)(sp * pxd + 0.5f);
        SetTextCharacterExtra(G.dc, extra);
        GetTextExtentPoint32W(G.dc, ws, n, &sz);
        if ((float)(sz.cx - extra) <= bw || pxd <= pxmin) break;
        pxd *= clampf(bw / (float)(sz.cx - extra), 0.6f, 0.97f);
    }
    int vis = sz.cx - extra;
    float tx = align == SA_CENTER ? bx + (bw - (float)vis) * 0.5f : align == SA_RIGHT ? bx + bw - (float)vis : bx;
    int base = (int)(by + bh * 0.5f + pxd * 0.36f + 0.5f);
    if (flags & SF_GLOW) s_glow_run(ws, n, f, extra, (int)tx, base, vis, color & 0xFFFFFF, g_glow);
    SelectObject(G.dc, f);
    SetTextCharacterExtra(G.dc, extra);
    SetTextAlign(G.dc, TA_LEFT | TA_BASELINE);
    ExtTextOutW(G.dc, (int)tx, base, 0, NULL, ws, (UINT)n, NULL);
    SetTextCharacterExtra(G.dc, 0);
}

static float stext_w(const char *s, int px, int role, float sp, int flags)
{
    wchar_t ws[300];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 299);
    if (n <= 1) return 0;
    n--;
    if (flags & SF_UP) CharUpperBuffW(ws, (DWORD)n);
    if (g_cls != 0) sp *= 0.5f;
    float pxd = (float)px * g_scale;
    rt_init();
    if (RT.ok) {
        float rw = 0;
        if (rt_line(ws, n, 0, 0, 0, 0, pxd, role, 0, 0, sp, 0, &rw)) return rw / g_scale;
    }
    if (role == 2) role = 0;
    HFONT f = gdi_font((int)(pxd + 0.5f), role, g_cls);
    if (!f) return 0;
    SelectObject(G.dc, f);
    int extra = (int)(sp * pxd + 0.5f);
    SetTextCharacterExtra(G.dc, extra);
    SIZE sz;
    GetTextExtentPoint32W(G.dc, ws, n, &sz);
    SetTextCharacterExtra(G.dc, 0);
    return (float)(sz.cx - extra) / g_scale;
}

/* palette: the orange of the game's pause menu and options pages (frames of the stage 1 / 6 screenshots) */
#define S_HI 0xF4902E        /* orange: chevrons, bars, key caps */
#define S_HOT 0xFFE2B8       /* selected text, title: warm white */
#define S_TEXT 0xD6CCB8      /* normal text: beige grey */
#define S_INFO 0xA89E8A
#define S_GREY 0x6C6458
#define S_LINE 0xC8691E

static unsigned s_status_rgb(int kind) { return kind == LSK_ERROR ? 0xFF8062 : kind == LSK_OK ? 0xA6E08C : 0xE4D8C0; }

/* the backdrop: the captured game frame, blurred, darkened and tinted; rust at the bottom, a cold tint at the top left, like the pause menu. src may be NULL (no capture) */
static void s_backdrop_build(const unsigned *src, int w, int h, unsigned *dst)
{
    const int f = 8;
    int sw = (w + f - 1) / f, sh = (h + f - 1) / f;
    float *sm = NULL, *tmp = NULL;
    if (src) {
        sm = (float *)malloc((size_t)sw * (size_t)sh * 3 * sizeof(float));
        tmp = (float *)malloc((size_t)sw * (size_t)sh * 3 * sizeof(float));
        if (!sm || !tmp) free(sm), free(tmp), sm = tmp = NULL;
    }
    if (sm) {
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                float a[3] = {0, 0, 0};
                int cnt = 0;
                for (int j = 0; j < f && y * f + j < h; j++)
                    for (int i = 0; i < f && x * f + i < w; i++) {
                        unsigned p = src[(size_t)(y * f + j) * (size_t)w + (size_t)(x * f + i)];
                        a[0] += (float)((p >> 16) & 255), a[1] += (float)((p >> 8) & 255), a[2] += (float)(p & 255), cnt++;
                    }
                for (int c = 0; c < 3; c++) sm[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = a[c] / (float)(cnt ? cnt : 1);
            }
        for (int pass = 0; pass < 3; pass++) {
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int c = 0; c < 3; c++) {
                        float s = 0;
                        for (int i = -2; i <= 2; i++) s += sm[((size_t)y * (size_t)sw + (size_t)(x + i < 0 ? 0 : x + i >= sw ? sw - 1 : x + i)) * 3 + (size_t)c];
                        tmp[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = s / 5;
                    }
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int c = 0; c < 3; c++) {
                        float s = 0;
                        for (int j = -2; j <= 2; j++) s += tmp[((size_t)(y + j < 0 ? 0 : y + j >= sh ? sh - 1 : y + j) * (size_t)sw + (size_t)x) * 3 + (size_t)c];
                        sm[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = s / 5;
                    }
        }
    }
    for (int y = 0; y < h; y++) {
        float ny = ((float)y + 0.5f) / (float)h, vy = ny * 2.0f - 1.0f;
        for (int x = 0; x < w; x++) {
            float nx = ((float)x + 0.5f) / (float)w, vx = nx * 2.0f - 1.0f;
            float r2 = vx * vx * 0.55f + vy * vy * 0.9f, vig = clampf(1.0f - r2 * 0.75f, 0, 1);
            float rust = clampf((ny - 0.62f) / 0.38f, 0, 1);   /* bottom of the screen */
            rust = rust * rust * (0.55f + 0.45f * clampf(1.0f - nx * 0.6f, 0, 1));
            float cold = clampf(1.0f - sqrtf((nx / 0.55f) * (nx / 0.55f) + (ny / 0.42f) * (ny / 0.42f)), 0, 1);   /* top left */
            float k = RT.frame_ok ? 0.0f : 1.0f;   /* the real frame brings the rust and the cold tint itself */
            float br = 6 + 12 * vig + k * (66 * rust + 8 * cold * cold), bg = 5 + 9 * vig + k * (26 * rust + 22 * cold * cold), bb = 5 + 8 * vig + k * (10 * rust + 54 * cold * cold);
            float sr = 0, sg = 0, sb = 0;
            if (sm) {
                float fx = ((float)x + 0.5f) / (float)f - 0.5f, fy = ((float)y + 0.5f) / (float)f - 0.5f;
                int ix = (int)floorf(fx), iy = (int)floorf(fy);
                float ax = fx - (float)ix, ay = fy - (float)iy;
                int x0 = ix < 0 ? 0 : ix >= sw ? sw - 1 : ix, x1 = ix + 1 < 0 ? 0 : ix + 1 >= sw ? sw - 1 : ix + 1, y0 = iy < 0 ? 0 : iy >= sh ? sh - 1 : iy, y1 = iy + 1 < 0 ? 0 : iy + 1 >= sh ? sh - 1 : iy + 1;
                const float *p00 = sm + ((size_t)y0 * (size_t)sw + (size_t)x0) * 3, *p10 = sm + ((size_t)y0 * (size_t)sw + (size_t)x1) * 3, *p01 = sm + ((size_t)y1 * (size_t)sw + (size_t)x0) * 3,
                            *p11 = sm + ((size_t)y1 * (size_t)sw + (size_t)x1) * 3;
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = (p00[k] * (1 - ax) + p10[k] * ax) * (1 - ay) + (p01[k] * (1 - ax) + p11[k] * ax) * ay;
                float l = 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2], wgt = 0.30f + 0.45f * vig;
                sr = (l * 1.0f * 0.55f + c[0] * 0.45f) * wgt, sg = (l * 0.78f * 0.55f + c[1] * 0.45f) * wgt, sb = (l * 0.58f * 0.55f + c[2] * 0.45f) * wgt;
            }
            float r = br + sr, g = bg + sg, b = bb + sb;
            dst[(size_t)y * (size_t)w + (size_t)x] = 0xFF000000u | ((unsigned)clampf(r, 0, 255) << 16) | ((unsigned)clampf(g, 0, 255) << 8) | (unsigned)clampf(b, 0, 255);
        }
    }
    free(sm), free(tmp);
}

/* thin frame with bright corner brackets, the ornament of the option pages */
static void s_corners(float x, float y, float w, float h, float len)
{
    s_frame(x, y, w, h, 1.4f, S_LINE, 110);
    float t = 3.0f;
    s_rect(x - 1, y - 1, len, t, S_HI, 240, 0), s_rect(x - 1, y - 1, t, len, S_HI, 240, 0);
    s_rect(x + w - len + 1, y - 1, len, t, S_HI, 240, 0), s_rect(x + w - t + 1, y - 1, t, len, S_HI, 240, 0);
    s_rect(x - 1, y + h - t + 1, len, t, S_HI, 240, 0), s_rect(x - 1, y + h - len + 1, t, len, S_HI, 240, 0);
    s_rect(x + w - len + 1, y + h - t + 1, len, t, S_HI, 240, 0), s_rect(x + w - t + 1, y + h - len + 1, t, len, S_HI, 240, 0);
}

/* orange key cap with a letter, like the game's legend icons; returns the width */
static float s_keycap(float x, float cy, const char *label)
{
    if (RT.sprites_ok && (!strcmp(label, "F") || !strcmp(label, "Esc"))) {   /* the game's own key cap, tinted orange */
        spr_key(!strcmp(label, "F") ? SPR_KEY_F : SPR_KEY_ESC, x, cy - 21, 42, 42, S_HI, 1.9f);
        return 42;
    }
    float h = 36, w = stext_w(label, 22, 0, 0.0f, 0) + 20;
    if (w < h) w = h;
    float y = cy - h / 2;
    int x0 = (int)(g_ox + x * g_scale), x1 = (int)(g_ox + (x + w) * g_scale), y0 = (int)(g_oy + y * g_scale), y1 = (int)(g_oy + (y + h) * g_scale);
    for (int yy = y0; yy < y1; yy++) {
        float t = (float)(yy - y0) / (float)(y1 - y0 > 0 ? y1 - y0 : 1);
        unsigned r = (unsigned)(206 - 62 * t), g = (unsigned)(98 - 46 * t), b = (unsigned)(34 - 14 * t);
        for (int xx = x0; xx < x1; xx++) {
            int edge = xx == x0 || xx == x1 - 1 || yy == y0 || yy == y1 - 1;
            s_put(xx, yy, edge ? 0xF0A860 : (r << 16) | (g << 8) | b, 255);
        }
    }
    stext(label, x, y, w, h, 22, 0, 0xFFF4E4, SA_CENTER, 0.0f, 0);
    return w;
}

static void s_legend(const lobby_view *v, float right, float cy)
{
    const char *ok = v->leg_ok[0] ? v->leg_ok : "OK", *bk = v->leg_back[0] ? v->leg_back : "Back";
    float wok = stext_w(ok, 26, 1, 0.0f, 0), wbk = stext_w(bk, 26, 1, 0.0f, 0);
    float kf = stext_w("F", 22, 0, 0, 0) + 20, ke = stext_w("Esc", 22, 0, 0, 0) + 20;
    if (kf < 36) kf = 36;
    if (ke < 36) ke = 36;
    if (RT.sprites_ok) kf = ke = 42;
    float total = kf + 14 + wok + 44 + ke + 14 + wbk;
    float x = right - total;
    x += s_keycap(x, cy, "F") + 14;
    stext(ok, x, cy - 20, wok + 6, 40, 26, 1, S_INFO, SA_LEFT, 0.0f, 0);
    x += wok + 44;
    x += s_keycap(x, cy, "Esc") + 14;
    stext(bk, x, cy - 20, wbk + 6, 40, 26, 1, S_INFO, SA_LEFT, 0.0f, 0);
}

static void s_value(const lobby_row *r, int sel, float right, float y, float rh, float wmax, int px)
{
    const char *v = r->value;
    int grey = r->kind == LV_GREY;
    unsigned col = grey ? S_GREY : sel ? 0xFFF0D8 : S_TEXT;
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '<' && v[n - 1] == '>') {
        char mid[96];
        size_t a = 1, b = n - 1;
        while (a < b && v[a] == ' ') a++;
        while (b > a && v[b - 1] == ' ') b--;
        size_t m = b - a < sizeof mid - 1 ? b - a : sizeof mid - 1;
        memcpy(mid, v + a, m), mid[m] = 0;
        unsigned mcol = sel ? S_HI : 0x9A6A3A;
        float mk = 34, tw = stext_w(mid, px, 1, 0.0f, 0);
        if (tw > wmax - 2 * mk) tw = wmax - 2 * mk;
        stext(">", right - mk, y, mk, rh, px + 2, 0, mcol, SA_RIGHT, 0, 0);
        stext(mid, right - mk - tw - 4, y, tw + 8, rh, px, 1, col, SA_RIGHT, 0.0f, 0);
        stext("<", right - mk - tw - 8 - mk, y, mk, rh, px + 2, 0, mcol, SA_LEFT, 0, 0);
    } else if (n)
        stext(v, right - wmax, y, wmax, rh, px, 1, col, SA_RIGHT, 0.0f, 0);
}

static void s_rows(const lobby_view *v, float x, float y0, float w, float ymax)
{
    g_hit_n = 0;
    float step = 70, rh = 58;
    if (v->nrows > 0 && y0 + step * (float)v->nrows > ymax) step = (ymax - y0) / (float)v->nrows, rh = step - 8;
    int px = (int)(rh * 0.68f);
    if (px > 34) px = 34;
    if (px < 22) px = 22;
    for (int i = 0; i < v->nrows; i++) {
        const lobby_row *r = &v->rows[i];
        int sel = i == v->sel;
        float y = y0 + step * (float)i;
        if (sel) {
            s_rect(x, y, w, rh, 0xE0701A, 120, 1);
            s_rect(x, y, w, 1.5f, S_HI, 150, 1);
            s_rect(x, y + rh - 1.5f, w, 1.5f, S_HI, 150, 1);
            s_rect(x + 8, y + rh * 0.5f - 18, 6, 36, S_HI, 255, 0);
        }
        unsigned col = r->kind == LV_GREY ? S_GREY : r->kind == LV_INFO ? S_INFO : sel ? S_HOT : S_TEXT;
        g_glow = 0.8f;
        stext(r->label, x + 34, y, w * 0.56f, rh, px, sel ? 0 : 1, col, SA_LEFT, 0.01f, sel ? SF_GLOW : 0);
        if (r->value[0]) s_value(r, sel, x + w - 26, y, rh, w * 0.46f, px - 3);
        if (g_hit_n < LOBBY_ROWS) g_hit[g_hit_n].x = (int)x, g_hit[g_hit_n].y = (int)y, g_hit[g_hit_n].w = (int)w, g_hit[g_hit_n].h = (int)rh, g_hit_n++;
    }
}

static void s_players(const lobby_view *v, float x, float y, float w)
{
    if (!v->nplayers) return;
    float h = 74 + 76 * (float)v->nplayers + 14;
    s_rect(x, y, w, h, 0x050303, 150, 0);
    s_corners(x, y, w, h, 28);
    stext(v->col_name, x + 28, y + 12, 300, 44, 22, 0, S_INFO, SA_LEFT, 0.10f, SF_UP);
    stext(v->col_char, x + 340, y + 12, 200, 44, 22, 0, S_INFO, SA_LEFT, 0.10f, SF_UP);
    stext(v->col_ping, x + w - 150, y + 12, 122, 44, 22, 0, S_INFO, SA_RIGHT, 0.10f, SF_UP);
    s_rect(x + 20, y + 62, w - 40, 1.5f, S_LINE, 150, 2);
    y += 74;
    for (int i = 0; i < v->nplayers; i++) {
        const lobby_vplayer *p = &v->players[i];
        char pg[24];
        stext(p->name, x + 28, y, 300, 76, 32, p->me ? 0 : 1, p->me ? S_HOT : S_TEXT, SA_LEFT, 0.0f, 0);
        stext(p->chr, x + 340, y + 4, 200, 40, 30, 1, S_TEXT, SA_LEFT, 0.0f, 0);
        if (p->host) stext(v->t_host, x + 340, y + 42, 200, 28, 20, 1, S_INFO, SA_LEFT, 0.04f, 0);
        else stext(p->ready ? v->t_ready : v->t_not_ready, x + 340, y + 42, 220, 28, 20, 1, p->ready ? 0xA6E08C : S_INFO, SA_LEFT, 0.0f, 0);
        if (p->ping >= 0) snprintf(pg, sizeof pg, "%d ms", p->ping);
        else snprintf(pg, sizeof pg, "%s", p->me ? "" : "-");
        stext(pg, x + w - 170, y, 142, 76, 26, 1, S_INFO, SA_RIGHT, 0.0f, 0);
        y += 76;
    }
}

static void draw_styled(const ov_scene *sc)
{
    const lobby_view *v = &sc->v;
    if (G.bg) memcpy(G.px, G.bg, (size_t)G.w * (size_t)G.h * 4);
    else
        for (size_t i = 0, n = (size_t)G.w * (size_t)G.h; i < n; i++) G.px[i] = 0xFF0A0706u;
    /* bottom band: help line / legend, with the thin orange line above it */
    s_rect(-400, 968, 2720, 140, 0x060403, 200, 0);
    s_rect(-400, 968, 2720, 1.6f, S_LINE, 200, 0);
    g_glow = 0.9f;
    stext(v->title, 165, 52, 1500, 100, 58, 2, S_HOT, SA_LEFT, 0.40f, SF_UP | SF_GLOW);
    float y = 250;
    if (v->subtitle[0]) stext(v->subtitle, 170, 160, 1500, 44, 28, 1, S_INFO, SA_LEFT, 0.02f, 0);
    if (v->input_on) {
        stext(v->prompt, 170, y, 1300, 44, 30, 1, S_TEXT, SA_LEFT, 0.0f, 0);
        s_rect(170, y + 54, 860, 74, 0x050303, 190, 0);
        s_corners(170, y + 54, 860, 74, 26);
        char in[120];
        snprintf(in, sizeof in, "%s%s", v->input, ((sc->tick_ms / 500) & 1) ? "_" : " ");
        stext(in, 196, y + 54, 810, 74, 38, 1, S_HOT, SA_LEFT, 0.02f, 0);
        y += 170;
    } else if (v->prompt[0]) {
        stext(v->prompt, 170, y, 1300, 56, 36, 0, S_HOT, SA_LEFT, 0.04f, 0);
        y += 96;
    }
    s_rows(v, 170, y, 860, 840);
    if (v->nplayers) s_players(v, 1160, 250, 660);
    char st[300];
    spinner_text(st, sizeof st, v->status, sc->tick_ms, v->busy);
    if (v->status[0] || v->busy) stext(st, 170, 860, 1580, 52, 32, 1, s_status_rgb(v->status_kind), SA_LEFT, 0.01f, 0);
    if (v->note[0]) stext(v->note, 170, 912, 1640, 40, 23, 1, 0xC2A070, SA_LEFT, 0.0f, 0);
    stext(v->hint, 170, 976, 1000, 90, 22, 1, S_INFO, SA_LEFT, 0.0f, SF_WRAP);
    s_legend(v, 1780, 1022);
    if (sc->cursor_on) {   /* a small pointer while the mouse is in use */
        float cx = (float)sc->cursor_x, cy = (float)sc->cursor_y;
        s_rect(cx - 1, cy - 12, 3, 25, 0xFFFFFF, 255, 0), s_rect(cx - 12, cy - 1, 25, 3, 0xFFFFFF, 255, 0);
        s_rect(cx, cy - 11, 1, 23, S_HI, 255, 0), s_rect(cx - 11, cy, 23, 1, S_HI, 255, 0);
    }
}

/* the finished game frame -> G.cap (32-bit formats only); returns 1 when it worked */
static int capture_frame(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    if (!G.sys || (G.fmt != D3DFMT_X8R8G8B8 && G.fmt != D3DFMT_A8R8G8B8)) return 0;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, G.sys))) return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(G.sys, &lr, NULL, D3DLOCK_READONLY))) return 0;
    if (!G.cap) G.cap = (unsigned *)malloc((size_t)G.w * (size_t)G.h * 4);
    int ok = G.cap != NULL;
    if (ok)
        for (int y = 0; y < G.h; y++) {
            const unsigned *s = (const unsigned *)((const unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch);
            unsigned *d = G.cap + (size_t)y * (size_t)G.w;
            for (int x = 0; x < G.w; x++) d[x] = s[x] | 0xFF000000u;
        }
    IDirect3DSurface9_UnlockRect(G.sys);
    return ok;
}

/* mean brightness (0..255) of a captured frame, sampled */
static int frame_mean(const unsigned *px, int w, int h)
{
    unsigned long sum = 0, n = 0;
    for (size_t i = 0, total = (size_t)w * (size_t)h; i < total; i += 17, n++) sum += ((px[i] >> 16) & 255) + ((px[i] >> 8) & 255) + (px[i] & 255);
    return n ? (int)(sum / (n * 3)) : 0;
}

/* the backdrop is taken from the game frame when the lobby opens; while that frame is black (a fade) it is taken again every 300 ms, up to 6 shots; the lobby is a still picture */
static void backdrop_update(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, int reopened, unsigned now)
{
    int need = 0;
    if (reopened || !G.bg) G.bg_stamp = now, G.bg_shots = 0, G.bg_final = 0, need = 1;
    else if (!G.bg_final && now - G.bg_stamp > 300u * (unsigned)G.bg_shots) need = 1;
    if (!need) return;
    if (!G.bg) G.bg = (unsigned *)malloc((size_t)G.w * (size_t)G.h * 4);
    if (!G.bg) return;
    rt_init();
    int cap = capture_frame(dev, bb);
    s_backdrop_build(cap ? G.cap : NULL, G.w, G.h, G.bg);
    rt_frame_into(G.bg, G.w, G.h, 0.9f);
    if (cap) g_bd_ok++;
    else g_bd_fail++;
    G.bg_shots++;
    G.bg_final = !cap || frame_mean(G.cap, G.w, G.h) >= 5 || G.bg_shots >= 6;
    r2_log("overlay: backdrop %s (shot %d)", cap ? "captured" : "fallback gradient", G.bg_shots);
}

/* one frame of backend 3: compose when the scene changed, then UpdateSurface + StretchRect */
static void gdi_frame(IDirect3DDevice9 *dev, const ov_scene *sc, UINT W, UINT H, D3DFORMAT fmt)
{
    if (G.w != (int)W || G.h != (int)H || G.fmt != fmt || !G.dc) {
        gdi_free();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = (LONG)W, bi.bmiHeader.biHeight = -(LONG)H, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        HDC sdc = GetDC(NULL);
        G.dc = CreateCompatibleDC(sdc);
        G.bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&G.px, NULL, 0);
        ReleaseDC(NULL, sdc);
        if (!G.dc || !G.bmp || !G.px) {
            g_fail_tex++;
            gdi_free();
            return;
        }
        SelectObject(G.dc, G.bmp);
        G.w = (int)W, G.h = (int)H, G.fmt = fmt, G.have_sig = 0;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, fmt, D3DPOOL_SYSTEMMEM, &G.sys, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, fmt, D3DPOOL_DEFAULT, &G.def, NULL))) {
            g_fail_tex++;
            r2_log("overlay: GDI backend: cannot create the staging surfaces (format %d)", (int)fmt);
            gdi_free();
            return;
        }
        r2_log("overlay: GDI backend ready (%ux%u, format %d)", W, H, (int)fmt);
    }
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    unsigned now = GetTickCount();
    int reopened = !G.last_tick || now - G.last_tick > 400;   /* the lobby was closed in between: take a new backdrop */
    G.last_tick = now ? now : 1;
    backdrop_update(dev, bb, reopened, now);
    /* the scene changes rarely: compose again only then (the blink and the dots tick every 350-500 ms) */
    unsigned sig = 2166136261u;
    const unsigned char *b = (const unsigned char *)&sc->v;
    for (size_t i = 0; i < sizeof sc->v; i++) sig = (sig ^ b[i]) * 16777619u;
    sig = (sig ^ (sc->tick_ms / 350)) * 16777619u, sig = (sig ^ ((sc->tick_ms / 500) & 1)) * 16777619u, sig = (sig ^ (unsigned)sc->lang) * 16777619u;
    sig = (sig ^ (unsigned)sc->cursor_x ^ ((unsigned)sc->cursor_y << 10) ^ (unsigned)sc->cursor_on) * 16777619u;
    sig = (sig ^ (unsigned)G.bg_shots) * 16777619u;
    if (!G.have_sig || sig != G.sig) {
        g_cls = face_class(sc->lang);
        draw_styled(sc);
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(G.sys, &lr, NULL, 0))) {
            for (int y = 0; y < G.h; y++) memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, G.px + (size_t)y * (size_t)G.w, (size_t)G.w * 4);
            IDirect3DSurface9_UnlockRect(G.sys);
            IDirect3DDevice9_UpdateSurface(dev, G.sys, NULL, G.def, NULL);
            G.sig = sig, G.have_sig = 1;
        }
    }
    HRESULT hr = IDirect3DDevice9_StretchRect(dev, G.def, NULL, bb, NULL, D3DTEXF_NONE);
    TRACE("StretchRect -> 0x%08lx", (unsigned long)hr);
    if (SUCCEEDED(hr)) g_frames++;
    IDirect3DSurface9_Release(bb);
}

/* offline (tests/overlay_preview_bmp.c): the lobby scene into a 32-bit top-down bitmap without any device; the picture set with overlay_set_preview_backdrop stands for the game frame */
static const unsigned *g_pv_px;
static int g_pv_w, g_pv_h;
void overlay_set_preview_backdrop(const unsigned *px, int w, int h) { g_pv_px = px, g_pv_w = w, g_pv_h = h; }
int overlay_render_to_bitmap(const ov_scene *sc, int w, int h, unsigned **pixels_out)
{
    gdi_free();
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(NULL);
    G.dc = CreateCompatibleDC(sdc);
    G.bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&G.px, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!G.dc || !G.bmp || !G.px) {
        gdi_free();
        return 0;
    }
    SelectObject(G.dc, G.bmp);
    G.w = w, G.h = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            G.px[(size_t)y * (size_t)w + (size_t)x] = g_pv_px && g_pv_w > 0 ? g_pv_px[(size_t)(y * g_pv_h / h) * (size_t)g_pv_w + (size_t)(x * g_pv_w / w)] | 0xFF000000u : 0xFF303840u;
    G.bg = (unsigned *)malloc((size_t)w * (size_t)h * 4);
    rt_init();
    if (G.bg) {
        s_backdrop_build(g_pv_px ? G.px : NULL, w, h, G.bg);
        rt_frame_into(G.bg, w, h, 0.9f);
    }
    float sx = (float)w / 1920.0f, sy = (float)h / 1080.0f;
    g_scale = sx < sy ? sx : sy, g_ox = ((float)w - 1920.0f * g_scale) * 0.5f, g_oy = ((float)h - 1080.0f * g_scale) * 0.5f;
    g_cls = face_class(sc->lang);
    draw_styled(sc);
    GdiFlush();
    *pixels_out = G.px;
    return 1;
}

void overlay_draw(void *vdev, const ov_scene *sc)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)vdev;
    if (!dev || !sc || g_mode == 0) return;
    TRACE("frame: ensure resources");
    if (g_mode != 3 && !ensure_resources(dev)) return;
    TRACE("resources ok (sprite %p, texture %p)", (void *)g_sprite, (void *)g_white);
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &desc);
    IDirect3DSurface9_Release(bb);
    g_bbw = (int)desc.Width, g_bbh = (int)desc.Height;
    float sx = (float)desc.Width / 1920.0f, sy = (float)desc.Height / 1080.0f;
    g_scale = sx < sy ? sx : sy;
    g_ox = ((float)desc.Width - 1920.0f * g_scale) * 0.5f;
    g_oy = ((float)desc.Height - 1080.0f * g_scale) * 0.5f;
    g_cls = face_class(sc->lang);
    D = dev;
    if (g_mode == 3) {
        TRACE("GDI frame");
        gdi_frame(dev, sc, desc.Width, desc.Height, desc.Format);
        if (g_trace > 0) g_trace--;
        return;
    }

    D3DVIEWPORT9 old, vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    IDirect3DDevice9_GetViewport(dev, &old);
    HRESULT hb = IDirect3DDevice9_BeginScene(dev);
    int in_scene = SUCCEEDED(hb);
    TRACE("BeginScene -> 0x%08lx", (unsigned long)hb);
    IDirect3DDevice9_SetViewport(dev, &vp);
    HRESULT hs = ID3DXSprite_Begin(g_sprite, D3DXSPRITE_ALPHABLEND);
    TRACE("Sprite Begin -> 0x%08lx", (unsigned long)hs);
    if (SUCCEEDED(hs)) {
        draw_full(sc);
        TRACE("drawn, calling Sprite End");
        HRESULT he = ID3DXSprite_End(g_sprite);
        TRACE("Sprite End -> 0x%08lx", (unsigned long)he);
        g_frames++;
    }
    IDirect3DDevice9_SetViewport(dev, &old);
    if (in_scene) {
        HRESULT hc = IDirect3DDevice9_EndScene(dev);
        TRACE("EndScene -> 0x%08lx", (unsigned long)hc);
    }
    if (g_trace > 0) g_trace--;
}

/* ------------------------------------------------------------------ stage 6i: the banner strip
 * Same technique as backend 3 (GDI bitmap -> UpdateSurface -> StretchRect, no BeginScene, no render state), but only for a strip across the top of the
 * back buffer, so the game stays visible below it. Own bitmap and surfaces (width x strip height); composed again only when the text or the size changes. */
static struct {
    HDC dc;
    HBITMAP bmp;
    unsigned *px;
    int w, h;
    IDirect3DSurface9 *sys, *def;
    D3DFORMAT fmt;
    char text[200];
    int lang, have;
} B;

static void banner_free(void)
{
    if (B.sys) IDirect3DSurface9_Release(B.sys);
    if (B.def) IDirect3DSurface9_Release(B.def);
    if (B.dc) DeleteDC(B.dc);
    if (B.bmp) DeleteObject(B.bmp);
    memset(&B, 0, sizeof B);
}

void overlay_banner(void *vdev, const char *utf8, int lang)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)vdev;
    if (!dev || !utf8 || !utf8[0] || g_mode != 3) return;
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &desc);
    int bw = (int)desc.Width, bh = (int)(desc.Height * 0.055f + 0.5f);
    if (bh < 36) bh = 36;
    if (bh > (int)desc.Height / 2) bh = (int)desc.Height / 2;
    if (B.w != bw || B.h != bh || B.fmt != desc.Format || !B.dc) {
        banner_free();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = bw, bi.bmiHeader.biHeight = -bh, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        HDC sdc = GetDC(NULL);
        B.dc = CreateCompatibleDC(sdc);
        B.bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&B.px, NULL, 0);
        ReleaseDC(NULL, sdc);
        if (!B.dc || !B.bmp || !B.px || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, (UINT)bw, (UINT)bh, desc.Format, D3DPOOL_SYSTEMMEM, &B.sys, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, (UINT)bw, (UINT)bh, desc.Format, D3DPOOL_DEFAULT, &B.def, NULL))) {
            g_fail_tex++;
            r2_log("overlay: banner: cannot create the strip (%dx%d, format %d)", bw, bh, (int)desc.Format);
            banner_free();
            IDirect3DSurface9_Release(bb);
            return;
        }
        SelectObject(B.dc, B.bmp);
        B.w = bw, B.h = bh, B.fmt = desc.Format, B.have = 0;
    }
    if (!B.have || B.lang != lang || strcmp(B.text, utf8) != 0) {
        for (size_t i = 0, n = (size_t)B.w * (size_t)B.h; i < n; i++) B.px[i] = 0xFF0C0807u;
        int stripe = bh / 12 > 3 ? bh / 12 : 3;
        for (int y = 0; y < B.h; y++)
            for (int x = 0; x < stripe; x++) B.px[(size_t)y * (size_t)B.w + (size_t)x] = 0xFFF4902Eu;
        for (int y = B.h - 2; y < B.h; y++)
            for (int x = 0; x < B.w; x++) B.px[(size_t)y * (size_t)B.w + (size_t)x] = 0xFFC8691Eu;
        wchar_t ws[400];
        if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws, 399) > 0) {
            HFONT f = gdi_font(bh * 5 / 10, 0, face_class(lang));
            if (f) {
                HGDIOBJ old = SelectObject(B.dc, f);
                SetBkMode(B.dc, TRANSPARENT);
                SetTextColor(B.dc, RGB(0xF2, 0xE4, 0xCC));
                RECT rc = {stripe + bh / 3, 0, B.w - bh / 3, B.h};
                DrawTextW(B.dc, ws, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(B.dc, old);
            }
        }
        for (int y = 0; y < B.h; y++)
            for (int x = 0; x < B.w; x++) B.px[(size_t)y * (size_t)B.w + (size_t)x] |= 0xFF000000u;   /* GDI text leaves the alpha byte at 0 */
        D3DLOCKED_RECT lr;
        B.have = 0;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(B.sys, &lr, NULL, 0))) {
            for (int y = 0; y < B.h; y++) memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, B.px + (size_t)y * (size_t)B.w, (size_t)B.w * 4);
            IDirect3DSurface9_UnlockRect(B.sys);
            if (SUCCEEDED(IDirect3DDevice9_UpdateSurface(dev, B.sys, NULL, B.def, NULL))) {
                B.have = 1, B.lang = lang;
                snprintf(B.text, sizeof B.text, "%s", utf8);
            }
        }
    }
    if (B.have) {
        RECT dst = {0, 0, bw, bh};
        IDirect3DDevice9_StretchRect(dev, B.def, NULL, bb, &dst, D3DTEXF_NONE);
    }
    IDirect3DSurface9_Release(bb);
}

int overlay_available(void) { return load_d3dx(); }
unsigned overlay_frames(void) { return g_frames; }

int overlay_row_at_client(int px, int py, int cw, int ch, int *vx, int *vy)
{
    if (cw <= 0 || ch <= 0 || g_scale <= 0.0f) return -1;
    float bx = (float)px * (float)g_bbw / (float)cw, by = (float)py * (float)g_bbh / (float)ch;
    int x = (int)((bx - g_ox) / g_scale), y = (int)((by - g_oy) / g_scale);
    if (vx) *vx = x;
    if (vy) *vy = y;
    for (int i = 0; i < g_hit_n; i++)
        if (x >= g_hit[i].x && x < g_hit[i].x + g_hit[i].w && y >= g_hit[i].y && y < g_hit[i].y + g_hit[i].h) return i;
    return -1;
}

void overlay_stats(char *out, int size)
{
    snprintf(out, (size_t)size, "overlay: d3dx %s, %u frames drawn, failures: font %u sprite %u texture %u, game material: fonts %s title %s keys %s frame %s (strings: %u real face, %u GDI), scale %.2f",
             g_ok ? "ok" : (g_tried ? "missing" : "not loaded yet"), g_frames, g_fail_font, g_fail_sprite, g_fail_tex, RT.ok ? "ok" : "no", RT.deco ? "ok" : "no", RT.sprites_ok ? "ok" : "no", RT.frame_ok ? "ok" : "no",
             RT.stat_real, RT.stat_gdi, g_scale);
}
