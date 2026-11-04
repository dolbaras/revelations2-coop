/*
 * Lobby overlay, see overlay.h. Layout is done in a virtual 1920x1080 space (16:9, centred on wider windows) and scaled to the back buffer.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dx9core.h>
#include <stdio.h>
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
static unsigned g_frames, g_fail_font, g_fail_sprite, g_fail_tex;
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
void overlay_release(void)
{
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
    struct { int px, bold, cls; HFONT f; } fonts[16];
} G;

static void gdi_free(void)
{
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
        if (G.fonts[i].f) DeleteObject(G.fonts[i].f);
    if (G.sys) IDirect3DSurface9_Release(G.sys);
    if (G.def) IDirect3DSurface9_Release(G.def);
    if (G.dc) DeleteDC(G.dc);
    if (G.bmp) DeleteObject(G.bmp);
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

static HFONT gdi_font(int px, int bold, int cls)
{
    unsigned free_slot = (unsigned)-1;
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++) {
        if (G.fonts[i].f && G.fonts[i].px == px && G.fonts[i].bold == bold && G.fonts[i].cls == cls) return G.fonts[i].f;
        if (!G.fonts[i].f && free_slot == (unsigned)-1) free_slot = i;
    }
    if (free_slot == (unsigned)-1) {
        for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
            if (G.fonts[i].f) DeleteObject(G.fonts[i].f), G.fonts[i].f = NULL;
        free_slot = 0;
    }
    HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_name(cls));
    if (!f) {
        g_fail_font++;
        return NULL;
    }
    G.fonts[free_slot].px = px, G.fonts[free_slot].bold = bold, G.fonts[free_slot].cls = cls, G.fonts[free_slot].f = f;
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
    /* the scene changes rarely: compose again only then (the blink and the dots tick every 350-500 ms) */
    unsigned sig = 2166136261u;
    const unsigned char *b = (const unsigned char *)&sc->v;
    for (size_t i = 0; i < sizeof sc->v; i++) sig = (sig ^ b[i]) * 16777619u;
    sig = (sig ^ (sc->tick_ms / 350)) * 16777619u, sig = (sig ^ ((sc->tick_ms / 500) & 1)) * 16777619u, sig = (sig ^ (unsigned)sc->lang) * 16777619u;
    sig = (sig ^ (unsigned)sc->cursor_x ^ ((unsigned)sc->cursor_y << 10) ^ (unsigned)sc->cursor_on) * 16777619u;
    if (!G.have_sig || sig != G.sig) {
        G.active = 1;
        for (size_t i = 0, n = (size_t)G.w * (size_t)G.h; i < n; i++) G.px[i] = 0xFF090B0Eu;   /* opaque, the letterbox bars too: the finished frame is replaced, not blended */
        draw_full(sc);
        G.active = 0;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(G.sys, &lr, NULL, 0))) {
            for (int y = 0; y < G.h; y++) memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, G.px + (size_t)y * (size_t)G.w, (size_t)G.w * 4);
            IDirect3DSurface9_UnlockRect(G.sys);
            IDirect3DDevice9_UpdateSurface(dev, G.sys, NULL, G.def, NULL);
            G.sig = sig, G.have_sig = 1;
        }
    }
    IDirect3DSurface9 *bb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        HRESULT hr = IDirect3DDevice9_StretchRect(dev, G.def, NULL, bb, NULL, D3DTEXF_NONE);
        TRACE("StretchRect -> 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr)) g_frames++;
        IDirect3DSurface9_Release(bb);
    }
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
        for (size_t i = 0, n = (size_t)B.w * (size_t)B.h; i < n; i++) B.px[i] = 0xFF14181Cu;
        int stripe = bh / 12 > 3 ? bh / 12 : 3;
        for (int y = 0; y < B.h; y++)
            for (int x = 0; x < stripe; x++) B.px[(size_t)y * (size_t)B.w + (size_t)x] = 0xFFC0392Bu;
        for (int y = B.h - 2; y < B.h; y++)
            for (int x = 0; x < B.w; x++) B.px[(size_t)y * (size_t)B.w + (size_t)x] = 0xFF3A424Au;
        wchar_t ws[400];
        if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws, 399) > 0) {
            HFONT f = gdi_font(bh * 5 / 10, 0, face_class(lang));
            if (f) {
                HGDIOBJ old = SelectObject(B.dc, f);
                SetBkMode(B.dc, TRANSPARENT);
                SetTextColor(B.dc, RGB(0xE8, 0xEC, 0xEF));
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
    snprintf(out, (size_t)size, "overlay: d3dx %s, %u frames drawn, failures: font %u sprite %u texture %u, scale %.2f", g_ok ? "ok" : (g_tried ? "missing" : "not loaded yet"), g_frames,
             g_fail_font, g_fail_sprite, g_fail_tex, g_scale);
}
