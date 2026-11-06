#ifndef R2_H
#define R2_H
#include <windows.h>

#define R2_VERSION "0.1.1"

/* The Steam build 3658995 exe (SHA-256 of rerev2.exe on disk). Any other exe: the proxy only forwards DirectInput8Create. */
#define R2_EXE_SHA256 "55ac7d2bd80704c6a8a1fa7bd2371726adbaa01ae0fda056198fb042404367f9"

/* proxy.c: log, environment, import-table and vtable patches, safe reads */
void r2_log(const char *fmt, ...);
int r2_env(const char *name, char *out, int size);
void *r2_patch_iat(const char *dll, const char *func, void *hook);   /* returns the previous pointer; remembered for r2_iat_check */
void *r2_patch_vtable(void *obj, int index, void *hook);
int r2_safe_read(const void *addr, void *out, unsigned size);        /* 1 = all bytes read (ReadProcessMemory on ourselves) */
int r2_exe_ok(void);                                                   /* 1 = the exe is the supported build */
const char *r2_exe_sha(void);
void r2_iat_check(const char *when);                                   /* logs and repairs patched import slots the stub/game changed */
/* How "unpacked" .text is: ratio of 0x00 and 0x8B bytes in a sample (code ~ 12% / 6%, an encrypted body ~ 0.4% each). */
void r2_text_state(char *out, int size);
int r2_text_decrypted(void);

/* testmode.c: window placement, background mode, adapter, input rules for test runs (REV2_* variables) */
void testmode_install(void);
void testmode_wrap_dinput(void *dinput);
int testmode_background(void);
int testmode_adapter_pick(void *d3d, HWND focus, unsigned asked);
int testmode_adapter_strict(void);
int testmode_force_windowed(void);          /* 1 = test instance: never exclusive full screen */
void testmode_window_size(int *w, int *h);  /* wanted client size, 0 when not set */
HWND testmode_window(void);
HWND testmode_find_window(void);   /* 6n: testmode_window() or, in a player's start (no test hook recorded one), the visible top-level window of this process */
void testmode_xinput_stats(char *out, int size);
void testmode_pad_set(int slot, int active, const void *xinput_gamepad);   /* synthetic pad for a slot (console "pad") */
void testmode_pad_hold(int slot, const void *xinput_gamepad, int ms);       /* ... released by itself after ms */
void testmode_pad_keep(int user, int on);                                    /* co-op (stage 5): this game user is always a connected pad (neutral unless it is the human's) */
void testmode_pad_human(int game_user);                                      /* game user that carries this PC's real pad (-1 off) */
void testmode_pad_pretend(int user, int active, const void *xinput_gamepad); /* test: pretend a real pad (console rpad) */
int testmode_pad_snapshot(unsigned short *buttons, short *lx, short *ly);   /* stage 8: what the game's pad scan would see on users 0..3 (lobby overlay input) */
DWORD testmode_pad_activity(void);                                           /* tick of the last non-neutral state of the human's real pad */
void testmode_dinput_stats(char *out, int size);

/* inject.c: synthetic keyboard / mouse for test runs */
int inject_key_lookup(const char *name, int *dik, int *vk);
void inject_key(int dik, int ms);
void inject_key_up(int dik);
void inject_mouse_move(int dx, int dy, int wheel);
void inject_mouse_button(int n, int ms);
void inject_mouse_button_up(int n);
void inject_cursor(int on, int x, int y);
int inject_cursor_pos(POINT *p);
int inject_active(void);
void inject_apply_keyboard(unsigned char *state);
void inject_apply_mouse(void *data, unsigned size);
int inject_apply_data(int dev_kind, void *buf, unsigned objsize, unsigned *count, unsigned room);
int inject_vk_down(int vk);
void inject_vk_from_dik(int dik, int *vk);

/* d3d.c: Direct3D 9 creation/device/Present hooks, screenshots (F11, console "shot", REV2_SHOTS) */
void d3d_install(void);
void d3d_request_shot(void);
void d3d_request_burst(int n, int ms);
void d3d_devreset_request(int n);   /* test: the next Present makes n Reset calls answer DEVICELOST (console `devreset <n>`) */
void console_shake_tick(void);
unsigned d3d_frames(void);

/* steam.c: timing of the game's SteamAPI_Init / SteamAPI_RunCallbacks calls */
void steam_install(void);
void steam_stats(char *out, int size);
int steam_ach_blocking(void);                       /* stage 8b: 1 = SetAchievement and the other writers are logged and NOT passed to Steam (REV2_ACHBLOCK, default with the test console) */
void steam_ach_status(char *out, int size);         /* console `net w ach`: block mode and the (would-be) unlock list */

/* diag.c: thread creation, long waits, window messages (start-up diagnostics, log only) */
void diag_install(void);
void diag_message(unsigned msg, WPARAM wp, LPARAM lp);
void diag_report(char *out, int size);
int r2_pass(void);                                                     /* REV2_PASS=1: pass-through, no Steam/pad/dinput/diag hooks */

/* trace.c: inline hooks into the game (log only) and calls of its own mode functions, supported build only */
void trace_install(void);
void trace_gm_state(char *out, int size);
void trace_call(const char *which, int a, int b, char *out, int size);

/* classes.c: class-name walker (vtable[4] -> DTI), object scan */
void classes_name(unsigned obj, char *out, int size);
void classes_dtiname(unsigned dti, char *out, int size);
void classes_scan(const char *file, const char *filter, unsigned lo, unsigned hi, char *out, int size);

/* net.c: REV2_NETCAP=<prefix>: read-only capture of Winsock, Steam interface calls and callbacks (stage 2b) */
void net_install(void);
int net_enabled(void);
void net_on_client(void *steam_client);
void net_on_iface(void *iface, const char *version);
void net_on_callback(int id, void *obj, const void *pv, int size, int fail, unsigned long long call);
void net_mark(const char *text);
void net_stats(char *out, int size);


/* coop3.c: logic-thread hook and stage 3 commands */
void lt_install(void);
void lt_post(const char *cmd, char *out, int size);
int lt_on_game_thread(void);
void s3_run(const char *cmd, char *out, int size);
void console_exec(const char *cmd, char *out, int size);

/* menu.c (stage 8): the "Co-Op" entry in the game's menus, lobby overlay, pause menu "Leave"; see menu.h */
void menu_install(void);                 /* once, after the first Present */
void menu_frame(void);                   /* every pass of the game loop, game thread */
void menu_present(void *d3d9_device);    /* Present hook: draws the lobby overlay while it is open */
void menu_before_reset(void);            /* Reset hook: releases the overlay's resources */
void menu_command(const char *arg, char *out, int size);   /* console `menu ...` */

/* console.c: UDP test console on 127.0.0.1:REV2_CONSOLE */
void console_install(void);

/* mute.c: REV2_MUTE=1 keeps this process's audio sessions muted */
void mute_install(void);

#endif
