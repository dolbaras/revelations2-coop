/*
 * Direct3D 9 hooks: device creation (adapter, windowed for test instances), Reset, Present, screenshots.
 *
 * The exe imports d3d9.dll!Direct3DCreate9 by name, so the hook goes through its import slot; everything after that is COM
 * vtables (IDirect3D9::CreateDevice slot 16, IDirect3DDevice9::Reset 16, Present 17). Presents are also the "the stub is long
 * done, the game renders" marker: the first one is logged with the .text state.
 *
 *   REV2_SHOTDIR=<dir>  where screenshots go (default: rev2_coop\shots next to the exe)
 *   REV2_SHOTS=s1,s2    also save a frame this many seconds after the device was created
 *   F11 (game window in front) or the console command "shot": save the next presented frame as shotNNNN_SSS.bmp
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r2.h"
#include "devreset.h"

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Direct3DCreate9_t real_Direct3DCreate9;
static CreateDevice_t real_CreateDevice;
static Present_t real_Present;
static Reset_t real_Reset;

/* test hook (console `devreset <n>`, test instances only): the next n Reset calls answer DEVICELOST without the device
   being lost, so the wait in hook_Reset can be run for real. Requested here, run by hook_Present on the render thread. */
static LONG devreset_inject, devreset_test_pending;
static D3DPRESENT_PARAMETERS dr_pp;   /* the last parameters of CreateDevice / Reset, for the test Reset */
static HRESULT do_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (devreset_inject > 0) {
        devreset_inject--;
        return (HRESULT)DEVRESET_LOST;
    }
    return real_Reset(dev, pp);
}
void d3d_devreset_request(int n) { InterlockedExchange(&devreset_test_pending, n); }

static char shot_dir[MAX_PATH];
static int shot_secs[32], shot_count, shot_next, shot_serial;
static DWORD t0;
static volatile LONG shot_asked;
static volatile unsigned frames;

unsigned d3d_frames(void) { return frames; }
void d3d_request_shot(void) { InterlockedExchange(&shot_asked, 1); }
/* `shot <n> <ms>` (stage 6f): n frames, one per `ms` milliseconds, taken inside the Present hook (a console round trip is about a second: too slow for a 0.7 s attack animation) */
static volatile LONG burst_left;
static volatile DWORD burst_ms, burst_next;
void d3d_request_burst(int n, int ms)
{
    burst_ms = (DWORD)(ms < 30 ? 30 : ms), burst_next = GetTickCount();
    InterlockedExchange(&burst_left, n < 0 ? 0 : n > 40 ? 40 : n);
}

static void save_frame(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        r2_log("shot: back buffer format %d not supported", d.Format);
        goto done;
    }
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr) && FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        /* (a multisampled back buffer: resolve through a plain render target) */
        IDirect3DSurface9 *rt = NULL;
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, rt, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        if (rt)
            IDirect3DSurface9_Release(rt);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        r2_log("shot: copy of the back buffer failed (hr=0x%08lx, %ux%u format %d multisample %d)", (unsigned long)hr, d.Width, d.Height,
               d.Format, d.MultiSampleType);
        goto done;
    }
    char path[MAX_PATH + 40];
    /* (the folders are made by the first screenshot, not at load: a player who never takes one gets no rev2_coop\ in the game folder) */
    snprintf(path, sizeof path, "%s", shot_dir);
    char *up = strrchr(path, '\\');
    if (up && up > path) {
        *up = 0;
        CreateDirectoryA(path, NULL);
    }
    CreateDirectoryA(shot_dir, NULL);
    snprintf(path, sizeof path, "%s\\shot%04d_%03lu.bmp", shot_dir, ++shot_serial, (GetTickCount() - t0) / 1000);
    FILE *f = fopen(path, "wb");
    if (f) {
        DWORD row = d.Width * 4, size = row * d.Height;
        BITMAPFILEHEADER fh = {0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih = {sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, size, 0, 0, 0, 0};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (UINT y = 0; y < d.Height; y++)
            fwrite((BYTE *)lr.pBits + y * lr.Pitch, row, 1, f);
        fclose(f);
        r2_log("shot: %s (%ux%u)", path, d.Width, d.Height);
    } else
        r2_log("shot: cannot write %s", path);
    IDirect3DSurface9_UnlockRect(sys);
done:
    if (sys)
        IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp);

/* The game creates its device with D3DCREATE_MULTITHREADED (flags=0x46 in every CreateDevice log line), so Present and Reset may come from different threads.
 * The overlay's resources are used in both (drawn in Present, released before Reset). ov_lock serialises the two: a Reset waits for an overlay frame that is
 * being drawn, a Present that arrives while a Reset (or its device-lost wait) runs on another thread skips the overlay for that one frame instead of waiting,
 * so no thread ever blocks in Present on this lock. Same thread: the lock is recursive. The log names both threads. */
static CRITICAL_SECTION ov_lock;
static volatile LONG ov_skips;
static volatile DWORD present_tid;

static HRESULT WINAPI hook_Present(IDirect3DDevice9 *dev, const RECT *s, const RECT *d, HWND w, const RGNDATA *r)
{
    unsigned n = ++frames;
    present_tid = GetCurrentThreadId();
    LONG tn = InterlockedExchange(&devreset_test_pending, 0);
    if (tn > 0) {   /* `devreset <n>` (test): a Reset on this (render) thread with n injected lost answers */
        D3DPRESENT_PARAMETERS tp = dr_pp;
        devreset_inject = tn;
        HRESULT th = hook_Reset(dev, &tp);
        r2_log("devreset test: %ld injected lost answers, Reset returned 0x%08lx, %ld injections left", (long)tn, (unsigned long)th, (long)devreset_inject);
        devreset_inject = 0;
    }
    if (TryEnterCriticalSection(&ov_lock)) {
        menu_present(dev);   /* the lobby overlay (stage 8), above the screenshot code below so that shots show it */
        LeaveCriticalSection(&ov_lock);
    } else
        InterlockedIncrement(&ov_skips);
    if (n == 1) {
        char st[160];
        r2_text_state(st, sizeof st);
        r2_log("first Present (the game renders) on thread %lu: %s", (unsigned long)GetCurrentThreadId(), st);
        r2_iat_check("first Present");
        lt_install();
        menu_install();
        {
            char tv[8];
            if (r2_env("REV2_TRACE", tv, sizeof tv) && tv[0] == '1')
                trace_install();
        }
    }
    /* F11: only while the game window is really in front (never reacts to keys meant for another window) */
    static int f11_was;
    HWND gw = testmode_window();
    int f11 = (GetAsyncKeyState(VK_F11) & 0x8000) && gw && GetForegroundWindow() == gw;
    if (f11 && !f11_was)
        shot_asked = 1;
    f11_was = f11;
    if (shot_asked && InterlockedExchange(&shot_asked, 0))
        save_frame(dev);
    if (burst_left > 0 && (int)(GetTickCount() - burst_next) >= 0) {   /* `shot <n> <ms>` */
        InterlockedDecrement(&burst_left);
        burst_next = GetTickCount() + burst_ms;
        save_frame(dev);
    }
    if (shot_next < shot_count && GetTickCount() - t0 >= (DWORD)shot_secs[shot_next] * 1000) {
        shot_next++;
        save_frame(dev);
    }
    return real_Present(dev, s, d, w, r);
}

/* device-lost recovery (devreset.h): the game itself shows a fatal error box at the first lost Reset, so the wait is here */
#define DEVRESET_BUDGET_MS 20000u
#define DEVRESET_POLL_MS   100u
struct devreset_ctx {
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS *pp;
};
static long devreset_coop(void *c) { return (long)IDirect3DDevice9_TestCooperativeLevel(((struct devreset_ctx *)c)->dev); }
static long devreset_reset(void *c)
{
    struct devreset_ctx *x = c;
    return (long)do_reset(x->dev, x->pp);
}
static void devreset_sleep(void *c, unsigned ms) { (void)c; Sleep(ms); }
static unsigned devreset_now(void *c) { (void)c; return (unsigned)GetTickCount(); }

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (testmode_force_windowed() && pp && !pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    EnterCriticalSection(&ov_lock);   /* waits for an overlay frame in progress on another thread; Presents during the Reset skip the overlay */
    menu_before_reset();
    if (pp)
        dr_pp = *pp;
    HRESULT hr = do_reset(dev, pp);
    r2_log("device Reset %ux%u windowed=%d -> hr=0x%08lx (thread %lu, Present thread %lu)", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
           (unsigned long)hr, (unsigned long)GetCurrentThreadId(), (unsigned long)present_tid);
    if (devreset_is_lost((long)hr)) {
        struct devreset_ctx x = {dev, pp};
        unsigned tries = 0;
        r2_log("device lost on Reset (hr=0x%08lx): waiting up to %u s for TestCooperativeLevel, then Reset again", (unsigned long)hr,
               DEVRESET_BUDGET_MS / 1000u);
        hr = (HRESULT)devreset_wait((long)hr, devreset_coop, devreset_reset, devreset_sleep, devreset_now, &x,
                                    DEVRESET_BUDGET_MS, DEVRESET_POLL_MS, &tries);
        r2_log("device Reset after the wait: %u more Reset call(s) -> hr=0x%08lx%s", tries, (unsigned long)hr,
               devreset_is_lost((long)hr) ? " (still lost: the game will show its error)"
               : FAILED(hr) ? " (TestCooperativeLevel or Reset gave a real error: the game will show its error)" : " (no longer lost: returned to the game)");
    }
    LONG sk = InterlockedExchange(&ov_skips, 0);
    LeaveCriticalSection(&ov_lock);
    if (sk) r2_log("device Reset: %ld Present call(s) on another thread skipped the overlay meanwhile", (long)sk);
    return hr;
}

static HRESULT WINAPI hook_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                        IDirect3DDevice9 **out)
{
    UINT use = (UINT)testmode_adapter_pick(d3d, focus, adapter);
    int was_full = pp && !pp->Windowed;
    if (testmode_force_windowed() && pp && !pp->Windowed) {   /* a test instance never goes exclusive full screen */
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    D3DADAPTER_IDENTIFIER9 id;
    memset(&id, 0, sizeof id);
    IDirect3D9_GetAdapterIdentifier(d3d, use, 0, &id);
    r2_log("CreateDevice asked adapter %u, using %u (%s, vendor 0x%04lx) %ux%u fmt=%d windowed=%d (was %s) buffers=%u msaa=%d flags=0x%lx", adapter, use,
           id.Description, id.VendorId, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0, pp ? pp->Windowed : -1,
           was_full ? "full screen" : "windowed", pp ? pp->BackBufferCount : 0, pp ? pp->MultiSampleType : 0, flags);
    HRESULT hr = real_CreateDevice(d3d, use, type, focus, flags, pp, out);
    r2_log("CreateDevice -> hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr) && use != adapter && !testmode_adapter_strict())
        hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        void *p;
        if (pp)
            dr_pp = *pp;
        t0 = GetTickCount();
        if ((p = r2_patch_vtable(*out, 16, (void *)hook_Reset)))
            real_Reset = (Reset_t)p;
        if ((p = r2_patch_vtable(*out, 17, (void *)hook_Present)))
            real_Present = (Present_t)p;
    }
    return hr;
}

static IDirect3D9 *WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
    r2_log("Direct3DCreate9(%u) -> %p, %u adapters", sdk, (void *)d3d, d3d ? IDirect3D9_GetAdapterCount(d3d) : 0);
    if (d3d) {
        void *p = r2_patch_vtable(d3d, 16, (void *)hook_CreateDevice);
        if (p)
            real_CreateDevice = (CreateDevice_t)p;
        for (UINT i = 0; i < IDirect3D9_GetAdapterCount(d3d) && i < 4; i++) {
            D3DADAPTER_IDENTIFIER9 id;
            memset(&id, 0, sizeof id);
            if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, i, 0, &id)))
                r2_log("  adapter %u: %s (vendor 0x%04lx)", i, id.Description, id.VendorId);
        }
    }
    return d3d;
}

void d3d_install(void)
{
    char shots[256];
    InitializeCriticalSection(&ov_lock);
    t0 = GetTickCount();
    if (!r2_env("REV2_SHOTDIR", shot_dir, sizeof shot_dir)) {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        char *slash = strrchr(exe, '\\');
        if (n && slash && slash - exe + 24 < MAX_PATH) {
            strcpy(slash + 1, "rev2_coop");
            snprintf(shot_dir, sizeof shot_dir, "%s\\shots", exe);
        }
    }
    if (r2_env("REV2_SHOTS", shots, sizeof shots))
        for (char *t = strtok(shots, ","); t && shot_count < 32; t = strtok(NULL, ","))
            shot_secs[shot_count++] = atoi(t);
    real_Direct3DCreate9 = (Direct3DCreate9_t)r2_patch_iat("d3d9.dll", "Direct3DCreate9", (void *)hook_Direct3DCreate9);
    r2_log("d3d: Direct3DCreate9 import %s, screenshots to %s", real_Direct3DCreate9 ? "hooked" : "NOT hooked", shot_dir);
}
