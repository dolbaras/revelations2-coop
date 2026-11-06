/*
 * Stage 8: the "Co-Op" entry in the game's menus, see menu.h and docs/lobby.md.
 *
 * What is patched (build 3658995 only, every site is checked byte for byte first):
 *   1. page 2 (campaign menu) of the common-menu page tables, row id 0x14: label "Co-Op Play" (the game's own string, all 12 languages) instead of
 *      "Invalid Message", help text the game's "Play the game in 2-player mode with a friend."
 *   2. the call at 0x44687D in cDemoCampaignMenu_Build that hides that row: a stub shows it (main campaign only)
 *   3. vtable of cDemoSessionSelectMenu (the dormant menu id 0x18, which "selecting the row" already opens): [5] Update runs our lobby, [0] destructor
 *      tells the lobby the screen is gone
 *   4. pause menu: after cPauseTopProcess_SetRowVisibility the row "Leave" (id 0x3B) is shown and the local split screen rows are hidden while a net
 *      session runs; the select handler of that row shows the game's own "Quit session?" popup, whose confirmation is the stock quit-to-title flow
 * The lobby itself is drawn by overlay.c in the Present hook, driven by the lobby model (lobby.c). While it is open the game's own common menu is hidden
 * and the pad system's "input disabled" byte holds the stock menus still.
 */
#include <winsock2.h>
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lobby.h"
#include "menu.h"
#include "overlay.h"
#include "r2.h"

#define THISCALL __attribute__((thiscall))
#define MLOG(...) r2_log("menu: " __VA_ARGS__)

/* ------------------------------------------------------------------ game addresses (build 3658995) */
enum {
    A_SAREA = 0x15DE720,          /* cell: sArea */
    A_GAMEMAN = 0x157AE00,        /* cell: GameMan */
    A_STORY = 0x157D120,          /* cell: story block */
    A_SGUI = 0x15DE768,           /* cell: sGUI (message tables, language) */
    A_BIOPAD = 0x15DE5DC,         /* cell: sBioPad */
    A_GETDEMOAREA = 0x44D620,
    A_MSG_GETSTRING = 0x954B40,
    A_COMMON_SETITEM = 0x8B16E0,
    A_COMMON_RESUME = 0x8B1CD0,
    A_SET_EPISODE = 0x425A00,
    A_SET_DIFFICULTY = 0x4259B0,
    A_SET_CONTINUE = 0x439D70,
    A_SET_GAMETYPE = 0x887AF0,
    A_GOTO_EPISODE_START = 0x4260A0,
    A_SESSION_BASEUPDATE = 0x811920,
    A_SESSION_UPDATE = 0x448140,
    A_SESSION_DTOR = 0x445250,
    A_VT_SESSION = 0x1255A9C,     /* vtable of cDemoSessionSelectMenu */
    A_ROW_COOP = 0x1393DDC,       /* page 2 row id 0x14 */
    A_CALL_HIDE_COOP = 0x44687D,  /* cDemoCampaignMenu_Build: call SetItemState(0x14, 2) */
    A_PAUSE_VIS = 0x8B9A60,
    A_PAUSE_SELECT = 0x8B85D0,
    A_POPUP_SETMSG = 0x436CE0
};
#define MENU_ID_LOBBY 0x18
#define MENU_ID_CAMPAIGN 0xA

typedef void *(THISCALL *thiscall_p_t)(void *);
typedef void (THISCALL *setitem_t)(void *, int, int);

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r2_safe_read((void *)a, &v, 4);
    return v;
}
static int wr_data(unsigned a, const void *p, unsigned n)
{
    DWORD old;
    if (!VirtualProtect((void *)a, n, PAGE_READWRITE, &old)) return 0;
    memcpy((void *)a, p, n);
    VirtualProtect((void *)a, n, old, &old);
    return 1;
}
static void wr32(unsigned a, unsigned v) { wr_data(a, &v, 4); }

/* ------------------------------------------------------------------ state */
static int g_installed, g_enabled = 1, g_hide_cm = 0, g_gate = 1;   /* hide_cm: calling the common menu's hide toggle (vtable +0xE8) even once, with 0, leaves its list, header and backdrop invisible (hardware, 2026-11-03): off */
static unsigned g_hooks;                          /* bit mask of what was installed (menu status) */
static lobby_t *g_lobby;
static lobby_settings g_set;
static char g_ini_path[MAX_PATH];
static void *g_obj;                               /* the cDemoSessionSelectMenu object that runs the lobby */
static void *g_closed;                            /* the object the lobby was closed on (it is destroyed on the next menu update; never reopen on it) */
static unsigned g_last_update;                    /* GetTickCount of the last lobby update */
static unsigned g_open_tick, g_ov_frames_open;
static unsigned g_wd_frames, g_wd_tick;           /* render watchdog: the Present counter and when it last moved */
static int g_armed, g_gate_on;
static unsigned char g_gate_prev;
static int g_cm_hidden;
static int g_leave_msg;                           /* the pause select handler of "Leave" is running: its popup says "Quit session?" */
static unsigned g_out_frames;
static int g_autoopen = 1;
static unsigned g_start_count;
static CRITICAL_SECTION g_lock;                   /* scene snapshot, note queue */
static ov_scene g_scene;
static int g_scene_valid;
static unsigned g_scene_tick;
static struct { int what, pid, a, b; char text[160]; } g_notes[8];
static unsigned g_note_w, g_note_r;

/* ------------------------------------------------------------------ settings file (the game's config folder, not the install) */
static void ini_path(void)
{
    if (r2_env("REV2_COOP_INI", g_ini_path, sizeof g_ini_path)) return;
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base - 80) {
        g_ini_path[0] = 0;
        return;
    }
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\CAPCOM", base);
    CreateDirectoryA(dir, NULL);   /* (both folders exist once the game ran; creating them is harmless otherwise) */
    snprintf(dir, sizeof dir, "%s\\CAPCOM\\RESIDENT EVIL REVELATIONS2", base);
    CreateDirectoryA(dir, NULL);
    snprintf(g_ini_path, sizeof g_ini_path, "%s\\rev2_coop.ini", dir);
}

static const char *const char_words[4] = {"claire", "moira", "barry", "natalia"};

static void settings_defaults(lobby_settings *s)
{
    memset(s, 0, sizeof *s);
    s->character = CP_CHAR_CLAIRE, s->port = 28410, s->transport = 0, s->episode = 1, s->difficulty = 1;
}

static void settings_load(void)
{
    settings_defaults(&g_set);
    FILE *f = g_ini_path[0] ? fopen(g_ini_path, "rb") : NULL;
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        while (*v == ' ') v++;
        if (!_stricmp(k, "name")) snprintf(g_set.name, sizeof g_set.name, "%s", v);
        else if (!_stricmp(k, "character")) {
            int c = cp_char_parse(v);
            if (c >= 0 && c <= 3) g_set.character = c;
        } else if (!_stricmp(k, "ip")) snprintf(g_set.ip, sizeof g_set.ip, "%s", v);
        else if (!_stricmp(k, "port")) g_set.port = atoi(v);
        else if (!_stricmp(k, "transport")) g_set.transport = !_stricmp(v, "direct") || !strcmp(v, "1");
        else if (!_stricmp(k, "episode")) g_set.episode = atoi(v);
        else if (!_stricmp(k, "difficulty")) g_set.difficulty = atoi(v);
        else if (!_stricmp(k, "solo")) g_set.solo = atoi(v) != 0;
        else if (!_stricmp(k, "autoopen")) g_autoopen = atoi(v) != 0;
        else if (!_stricmp(k, "hide_menu")) g_hide_cm = atoi(v) != 0;
        else if (!_stricmp(k, "input_gate")) g_gate = atoi(v) != 0;
        else if (!_stricmp(k, "enabled")) g_enabled = atoi(v) != 0;
    }
    fclose(f);
}

static void settings_save(const lobby_settings *s)
{
    g_set = *s;
    if (!g_ini_path[0]) return;
    FILE *f = fopen(g_ini_path, "wb");
    if (!f) {
        MLOG("cannot write %s", g_ini_path);
        return;
    }
    fprintf(f, "; Resident Evil Revelations 2 co-op mod: lobby settings (written by the mod; edit while the game is closed)\r\n");
    fprintf(f, "name=%s\r\ncharacter=%s\r\nip=%s\r\nport=%d\r\ntransport=%s\r\nepisode=%d\r\ndifficulty=%d\r\n", s->name, char_words[s->character & 3], s->ip, s->port,
            s->transport ? "direct" : "steam", s->episode, s->difficulty);
    fprintf(f, "; switches: enabled=1 (0 removes the Co-Op entry), autoopen=1 (open the lobby when a friend's invitation joins), hide_menu=1, input_gate=1, solo=0 (1: the host may start alone)\r\n");
    fprintf(f, "enabled=%d\r\nautoopen=%d\r\nhide_menu=%d\r\ninput_gate=%d\r\nsolo=%d\r\n", g_enabled, g_autoopen, g_hide_cm, g_gate, s->solo);
    fclose(f);
}

/* ------------------------------------------------------------------ game helpers */
static unsigned gm(void) { return rd32(A_GAMEMAN); }
static int menu_id(void) { unsigned g = gm(); return g ? (int)rd32(g + 0x20) : -1; }
static void set_menu_id(int id)
{
    unsigned g = gm();
    if (!g) return;
    wr32(g + 0x20, (unsigned)id);
    wr32(g + 0x24, 0xFFFFFFFFu);
}

static unsigned demo_area(void)
{
    if (!rd32(A_SAREA)) return 0;
    return ((unsigned(__cdecl *)(void))A_GETDEMOAREA)();
}
static unsigned common_menu(void)
{
    unsigned a = demo_area();
    return a ? rd32(a + 0x6C) : 0;
}

static void cm_set_hidden(int hide)
{
    unsigned cm = common_menu();
    if (!cm) return;
    unsigned vt = rd32(cm);
    unsigned fn = vt ? rd32(vt + 0xE8) : 0;
    if (fn < 0x400000 || fn > 0x1400000) return;
    ((void(THISCALL *)(void *, int))fn)((void *)cm, hide);
    g_cm_hidden = hide;
}

static void cm_resume(void)
{
    unsigned cm = common_menu();
    if (!cm) return;
    wr32(cm + 0x2C4, 0x68);   /* "nothing selected": a confirm the hidden list saw while the lobby was open must not reach the campaign menu (the game's own handlers reset it the same way) */
    ((void(THISCALL *)(void *))A_COMMON_RESUME)((void *)cm);
}

/* sBioPad+0xBC0: "input disabled" for every reader of the pad system (including the keyboard-as-pad of the menus) */
static void gate_set(int on)
{
    unsigned bp = rd32(A_BIOPAD);
    if (!bp) return;
    if (on && !g_gate_on) {
        unsigned char v = 0;
        r2_safe_read((void *)(bp + 0xBC0), &v, 1);
        g_gate_prev = v;
        unsigned char one = 1;
        wr_data(bp + 0xBC0, &one, 1);
        g_gate_on = 1;
    } else if (!on && g_gate_on) {
        wr_data(bp + 0xBC0, &g_gate_prev, 1);
        g_gate_on = 0;
    }
}

static int g_lang_override = -1;   /* test console `menu lang <0-11>`: the lobby's own texts and its font class in that language (lobby_lang_names order); the game's own strings stay in the loaded language */
static int game_lang(void)
{
    static const int game_id_of[12] = {1, 2, 5, 4, 3, 10, 11, 9, 0, 6, 8, 7};   /* inverse of lobby_lang_from_game */
    if (g_lang_override >= 0 && g_lang_override < 12) return game_id_of[g_lang_override];
    unsigned s = rd32(A_SGUI);
    return s ? (int)rd32(s + 0x28) : 1;
}

/* ------------------------------------------------------------------ texts */
static void strip_markup(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '<') {
            char *e = strchr(r, '>');
            if (e) {
                r = e;
                continue;
            }
        }
        if (*r == '\r') continue;
        *w++ = *r == '\n' ? ' ' : *r;
    }
    *w = 0;
    while (w > s && w[-1] == ' ') *--w = 0;
}

static const char *game_text(int table, int id)
{
    static char buf[8][400];
    static unsigned n;
    unsigned sg = rd32(A_SGUI);
    if (!sg) return NULL;
    const char *p = ((const char *(THISCALL *)(void *, int, int))A_MSG_GETSTRING)((void *)sg, table, id);
    if (!p || (unsigned)p < 0x10000) return NULL;
    char *dst = buf[n++ & 7];
    if (!r2_safe_read(p, dst, 399)) {   /* (near the end of a page: byte by byte) */
        int i = 0;
        for (; i < 399 && r2_safe_read(p + i, &dst[i], 1) && dst[i]; i++) {}
        dst[i] = 0;
    }
    dst[399] = 0;
    strip_markup(dst);
    {   /* (a leading button icon tag leaves a space behind) */
        char *t = dst;
        while (*t == ' ') t++;
        if (t != dst) memmove(dst, t, strlen(t) + 1);
    }
    if (!dst[0] || !strncmp(dst, "Invalid Message", 15) || strstr(dst, "_comment") || strstr(dst, "_guide_")) return NULL;
    return dst;
}

/* stock strings are looked up once per language (the view asks for dozens of them every frame) */
static struct {
    int lang, state;   /* state 0 unknown, 1 found, 2 not found (retried after a while) */
    unsigned tick;
    char text[400];
} g_tcache[LT_COUNT];

static const char *env_text(void *u, int id)
{
    (void)u;
    int glang = game_lang();
    int lang = lobby_lang_from_game(glang);
    if (id < 0 || id >= LT_COUNT) return "";
    if (lobby_stock_ids[id].table < 0) return lobby_own_text(id, lang);
    unsigned now = GetTickCount();
    if (g_tcache[id].lang != glang + 1 || (g_tcache[id].state == 2 && now - g_tcache[id].tick > 4000)) g_tcache[id].state = 0;
    if (g_tcache[id].state == 0) {
        const char *s = game_text(lobby_stock_ids[id].table, lobby_stock_ids[id].id);
        if (s && lang >= 8) {   /* Japanese / Korean / Chinese: some stock entries are English in the game's own table (Difficulty, Start Game ...): our word when we have one */
            const char *own = lobby_own_text(id, lang);
            int ascii = 1;
            for (const char *c = s; *c; c++)
                if ((unsigned char)*c >= 0x80) ascii = 0;
            if (ascii && strcmp(own, lobby_own_text(id, 0))) s = own;
        }
        g_tcache[id].lang = glang + 1, g_tcache[id].tick = now;
        if (s) g_tcache[id].state = 1, snprintf(g_tcache[id].text, sizeof g_tcache[id].text, "%s", s);
        else g_tcache[id].state = 2;
    }
    return g_tcache[id].state == 1 ? g_tcache[id].text : lobby_own_text(id, lang);   /* (a table that is not loaded or an "Invalid Message" gap: our word in that language, English where we have none) */
}

/* ------------------------------------------------------------------ session environment of the lobby */
static int env_command(void *u, const char *line, char *out, int size)
{
    (void)u;
    return coop_net_command(line, out, size);
}
static cs_t *env_session(void *u)
{
    (void)u;
    return coop_net_session();
}
static uint32_t env_now(void *u)
{
    (void)u;
    return GetTickCount();
}
static void env_save(void *u, const lobby_settings *s)
{
    (void)u;
    settings_save(s);
}

/* one worker thread for the blocking console lines (Steam lobby search, hosting / joining through Steam) */
static struct {
    HANDLE ev;
    volatile LONG state;   /* 0 idle, 1 requested, 2 running, 3 finished */
    char line[200];
    char out[6000];
} g_job;

static DWORD WINAPI job_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_job.ev, INFINITE);
        if (g_job.state != 1) continue;
        InterlockedExchange(&g_job.state, 2);
        g_job.out[0] = 0;
        coop_net_command(g_job.line, g_job.out, sizeof g_job.out);
        InterlockedExchange(&g_job.state, 3);
    }
    return 0;
}

static int env_async_start(void *u, const char *line)
{
    (void)u;
    if (g_job.state == 1 || g_job.state == 2) return 0;
    if (!g_job.ev) {
        g_job.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, job_thread, NULL, 0, NULL);
    }
    snprintf(g_job.line, sizeof g_job.line, "%s", line);
    InterlockedExchange(&g_job.state, 1);
    SetEvent(g_job.ev);
    MLOG("job: %s", line);
    return 1;
}
static int env_async_poll(void *u, char *out, int size)
{
    (void)u;
    if (g_job.state != 3) return 0;
    snprintf(out, (size_t)size, "%s", g_job.out);
    InterlockedExchange(&g_job.state, 0);
    return 1;
}

static int env_clipboard(void *u, char *out, int size)
{
    (void)u;
    int ok = 0;
    if (OpenClipboard(NULL)) {
        HANDLE h = GetClipboardData(CF_TEXT);
        const char *p = h ? (const char *)GlobalLock(h) : NULL;
        if (p) {
            snprintf(out, (size_t)size, "%s", p);
            ok = out[0] != 0;
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    return ok;
}

/* the IPv4 address of this PC on the way to the Internet / LAN: connecting a UDP socket sends nothing but makes Windows pick the outgoing interface (a host-only or VPN adapter
 * that happens to come first in the host name's address list is thereby avoided); the host name's list is the fallback */
static int env_local_address(void *u, char *out, int size)
{
    (void)u;
    out[0] = 0;
    SOCKET so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (so != INVALID_SOCKET) {
        struct sockaddr_in to, me;
        int len = sizeof me;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET, to.sin_port = htons(53), to.sin_addr.s_addr = htonl(0x08080808u);
        if (connect(so, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(so, (struct sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr != 0) {
            unsigned char *b = (unsigned char *)&me.sin_addr.s_addr;
            snprintf(out, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
        closesocket(so);
    }
    if (out[0]) return 1;
    char host[128];
    if (gethostname(host, sizeof host) != 0) return 0;
    struct hostent *he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET) return 0;
    for (char **p = he->h_addr_list; p && *p; p++) {
        unsigned char *b = (unsigned char *)*p;
        int priv = b[0] == 10 || (b[0] == 192 && b[1] == 168) || (b[0] == 172 && b[1] >= 16 && b[1] <= 31);
        if (priv || !out[0]) snprintf(out, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (priv) break;
    }
    return out[0] != 0;
}

/* ------------------------------------------------------------------ input: keyboard (real or injected), pad (what the game's pad scan sees) */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_SPACE, K_F, K_ESC, K_BACK, K_V, K_CTRL, K_SHIFT, K_NKEYS };
static const int k_vk[K_NKEYS] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE, 'F', VK_ESCAPE, VK_BACK, 'V', VK_CONTROL, VK_SHIFT};
static unsigned char k_prev[K_NKEYS];
static unsigned k_next[K_NKEYS];
static unsigned char c_prev[256];     /* typed characters: by VK */
static unsigned c_next[256];
static unsigned short p_prev;
static unsigned p_next[6];
static int m_hover = -2, m_prev_btn, m_vx, m_vy;   /* mouse: last row under the pointer, left button, pointer in virtual coordinates */
static unsigned m_last_move;

static int in_foreground(void)
{
    HWND w = testmode_find_window();
    return w && GetForegroundWindow() == w;
}

static int vk_down(int vk, int fg)
{
    if (inject_vk_down(vk)) return 1;
    if (vk == VK_CONTROL && (inject_vk_down(VK_LCONTROL) || inject_vk_down(VK_RCONTROL))) return 1;
    if (vk == VK_SHIFT && (inject_vk_down(VK_LSHIFT) || inject_vk_down(VK_RSHIFT))) return 1;
    return fg && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int edge_or_repeat(unsigned char *prev, unsigned *next, int down, unsigned now, int repeat)
{
    int fire = 0;
    if (down && !*prev) {
        fire = 1;
        *next = now + 420;
    } else if (down && repeat && (int)(now - *next) >= 0) {
        fire = 1;
        *next = now + 75;
    }
    *prev = (unsigned char)down;
    return fire;
}

/* character a key stands for in the address field; 0 = none */
static int vk_char(int vk, int shift)
{
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
    if (vk >= 'A' && vk <= 'Z') return 'a' + (vk - 'A');
    if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) return shift && vk == VK_OEM_PERIOD ? ':' : '.';
    if (vk == VK_OEM_1) return ':';
    if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) return shift ? '_' : '-';
    return 0;
}

static int poll_input(lobby_event *ev, int max, int text_mode)
{
    int n = 0;
    unsigned now = GetTickCount();
    int fg = in_foreground();
    int down[K_NKEYS];
    for (int i = 0; i < K_NKEYS; i++) down[i] = vk_down(k_vk[i], fg);
    unsigned short pb = 0;
    short lx = 0, ly = 0;
    testmode_pad_snapshot(&pb, &lx, &ly);
    int any = 0;
    for (int i = 0; i < K_NKEYS; i++) any |= i != K_CTRL && i != K_SHIFT && down[i];
    if (pb || ly > 22000 || ly < -22000 || lx > 22000 || lx < -22000) any = 1;
    if (!g_armed) {   /* the key that opened the screen must be released first */
        if (!any) g_armed = 1;
        memcpy(k_prev, down, sizeof k_prev);
        p_prev = pb;
        return 0;
    }
#define PUSH(k) do { if (n < max) ev[n].kind = (k), ev[n].ch = 0, ev[n].row = 0, n++; } while (0)
    if (edge_or_repeat(&k_prev[K_UP], &k_next[K_UP], down[K_UP], now, 1)) PUSH(LE_UP);
    if (edge_or_repeat(&k_prev[K_DOWN], &k_next[K_DOWN], down[K_DOWN], now, 1)) PUSH(LE_DOWN);
    if (edge_or_repeat(&k_prev[K_LEFT], &k_next[K_LEFT], down[K_LEFT], now, 1)) PUSH(LE_LEFT);
    if (edge_or_repeat(&k_prev[K_RIGHT], &k_next[K_RIGHT], down[K_RIGHT], now, 1)) PUSH(LE_RIGHT);
    if (edge_or_repeat(&k_prev[K_ENTER], &k_next[K_ENTER], down[K_ENTER], now, 0)) PUSH(LE_OK);
    if (edge_or_repeat(&k_prev[K_SPACE], &k_next[K_SPACE], down[K_SPACE] && !text_mode, now, 0)) PUSH(LE_OK);
    if (edge_or_repeat(&k_prev[K_F], &k_next[K_F], down[K_F] && !text_mode, now, 0)) PUSH(LE_OK);
    if (edge_or_repeat(&k_prev[K_ESC], &k_next[K_ESC], down[K_ESC], now, 0)) PUSH(LE_BACK);
    if (edge_or_repeat(&k_prev[K_BACK], &k_next[K_BACK], down[K_BACK], now, 1)) PUSH(LE_BACKSPACE);
    /* typed characters: the key states are tracked in every mode (a key held while the screen switches to the address field must not count as a new press), the characters
     * are only passed on in text mode */
    {
        int paste = edge_or_repeat(&k_prev[K_V], &k_next[K_V], down[K_V] && down[K_CTRL], now, 0);
        if (text_mode && paste) PUSH(LE_PASTE);
        static const unsigned char vks[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
                                            VK_OEM_PERIOD, VK_DECIMAL, VK_OEM_1, VK_OEM_MINUS, VK_SUBTRACT, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q',
                                            'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};
        for (unsigned i = 0; i < sizeof vks; i++) {
            int vk = vks[i], d = !down[K_CTRL] && vk_down(vk, fg);
            if (edge_or_repeat(&c_prev[vk], &c_next[vk], d, now, 1) && text_mode) {
                int c = vk_char(vk, down[K_SHIFT]);
                if (c && n < max) ev[n].kind = LE_CHAR, ev[n].ch = c, n++;
            }
        }
    }
    /* pad: d-pad / left stick move, A or Start confirms, B goes back */
    int pup = (pb & 1) || ly > 22000, pdn = (pb & 2) || ly < -22000, plf = (pb & 4) || lx < -22000, prt = (pb & 8) || lx > 22000;
    static unsigned char ps[6];
    if (edge_or_repeat(&ps[0], &p_next[0], pup, now, 1)) PUSH(LE_UP);
    if (edge_or_repeat(&ps[1], &p_next[1], pdn, now, 1)) PUSH(LE_DOWN);
    if (edge_or_repeat(&ps[2], &p_next[2], plf, now, 1)) PUSH(LE_LEFT);
    if (edge_or_repeat(&ps[3], &p_next[3], prt, now, 1)) PUSH(LE_RIGHT);
    if (edge_or_repeat(&ps[4], &p_next[4], (pb & 0x1000) || (pb & 0x10), now, 0)) PUSH(LE_OK);
    if (edge_or_repeat(&ps[5], &p_next[5], (pb & 0x2000) != 0, now, 0)) PUSH(LE_BACK);
    p_prev = pb;
    /* mouse (only while the game window is in front: the real cursor and the left button) */
    if (fg) {
        HWND w = testmode_find_window();
        POINT pt;
        RECT rc;
        if (GetCursorPos(&pt) && ScreenToClient(w, &pt) && GetClientRect(w, &rc)) {
            static POINT last_pt = {-1, -1};
            int row = overlay_row_at_client(pt.x, pt.y, rc.right, rc.bottom, &m_vx, &m_vy);
            if (pt.x != last_pt.x || pt.y != last_pt.y) m_last_move = now, last_pt = pt;
            int btn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (row != m_hover && now - m_last_move < 200) {
                m_hover = row;
                if (row >= 0 && n < max) ev[n].kind = LE_HOVER, ev[n].ch = 0, ev[n].row = row, n++;
            }
            if (btn && !m_prev_btn && row >= 0 && n < max) ev[n].kind = LE_CLICK, ev[n].ch = 0, ev[n].row = row, n++;
            m_prev_btn = btn;
        }
    }
    return n;
#undef PUSH
}

/* ------------------------------------------------------------------ the scene for the overlay */
static void publish_scene(int visible)
{
    /* the view is built outside g_lock: it takes the session lock, and the session calls menu_on_note (g_lock) while it holds that one */
    static ov_scene tmp;
    if (visible && g_lobby) {
        lobby_view_get(g_lobby, &tmp.v);
        tmp.lang = lobby_lang_from_game(game_lang());
        tmp.cursor_on = GetTickCount() - m_last_move < 3000 && in_foreground();
        tmp.cursor_x = m_vx, tmp.cursor_y = m_vy;
    } else
        visible = 0;
    EnterCriticalSection(&g_lock);
    if (visible) {
        g_scene = tmp;
        g_scene_valid = 1;
        g_scene_tick = GetTickCount();
    } else
        g_scene_valid = 0;
    LeaveCriticalSection(&g_lock);
}

/* 6i: the banner strip (the barrier waits for the other PC) */
static int g_banner_kind;
static unsigned g_banner_until;
static char g_banner_who[48];
void menu_banner(int kind, const char *who, unsigned ms)
{
    if (!g_installed) return;
    EnterCriticalSection(&g_lock);
    g_banner_kind = kind;
    g_banner_until = GetTickCount() + ms;
    snprintf(g_banner_who, sizeof g_banner_who, "%s", who && who[0] ? who : "...");
    LeaveCriticalSection(&g_lock);
}

void menu_present(void *dev)
{
    if (!g_installed) return;
    ov_scene sc;
    EnterCriticalSection(&g_lock);
    int ok = g_scene_valid && GetTickCount() - g_scene_tick < 500;
    if (ok) sc = g_scene;
    int bk = g_banner_kind;
    char who[48];
    snprintf(who, sizeof who, "%s", g_banner_who);
    if (bk && (int)(g_banner_until - GetTickCount()) <= 0) bk = g_banner_kind = 0;
    LeaveCriticalSection(&g_lock);
    if (!ok) {
        if (bk == MB_WAIT) {   /* no lobby on the screen: only the strip */
            int lang = lobby_lang_from_game(game_lang());
            char line[200];
            snprintf(line, sizeof line, lobby_own_text(LT_WAIT_LOAD, lang), who);
            overlay_banner(dev, line, lang);
        }
        return;
    }
    sc.tick_ms = GetTickCount();
    overlay_draw(dev, &sc);
}

void menu_before_reset(void)
{
    if (g_installed) overlay_release();
}

/* ------------------------------------------------------------------ session notes (any thread) */
void menu_on_note(const cs_note *n)
{
    if (!g_installed) return;
    EnterCriticalSection(&g_lock);
    unsigned i = g_note_w++ & 7;
    g_notes[i].what = n->what, g_notes[i].pid = n->pid, g_notes[i].a = n->a, g_notes[i].b = n->b;
    snprintf(g_notes[i].text, sizeof g_notes[i].text, "%s", n->text ? n->text : "");
    if (g_note_w - g_note_r > 8) g_note_r = g_note_w - 8;
    LeaveCriticalSection(&g_lock);
}

static void drain_notes(void)
{
    for (;;) {
        int what, pid, a, b;
        char text[160];
        EnterCriticalSection(&g_lock);
        int have = g_note_r != g_note_w;
        if (have) {
            unsigned i = g_note_r++ & 7;
            what = g_notes[i].what, pid = g_notes[i].pid, a = g_notes[i].a, b = g_notes[i].b;
            snprintf(text, sizeof text, "%s", g_notes[i].text);
        }
        LeaveCriticalSection(&g_lock);
        if (!have) break;
        if (g_lobby) lobby_note(g_lobby, what, pid, a, b, text);
    }
}

/* ------------------------------------------------------------------ the stock start sequence of a new campaign game */
static int g_start_cont;   /* 6h, console only (`menu start <ep> <part> <diff> 1`): the stock Continue (the checkpoint of the save) instead of a new game */
static void game_start(int episode, int part, int difficulty)
{
    unsigned g = gm(), st = rd32(A_STORY), area = demo_area();
    if (!g || !st || !area) {
        MLOG("start: game not ready (gm %08x story %08x aDemo %08x)", g, st, area);
        return;
    }
    MLOG("start: episode %d part %d difficulty %d", episode, part, difficulty);
    ((void(THISCALL *)(void *, int))A_SET_GAMETYPE)((void *)g, 0);
    unsigned char zero = 0;
    wr_data(st + 0x15C485, &zero, 1);
    ((void(THISCALL *)(void *, int))A_SET_EPISODE)((void *)st, episode < 1 ? 1 : episode > 4 ? 4 : episode);
    ((void(THISCALL *)(void *, int))A_SET_DIFFICULTY)((void *)st, difficulty < 0 ? 0 : difficulty > 3 ? 3 : difficulty);
    wr32(st + 0x15C4A8, (unsigned)(part ? 1 : 0));
    ((void(THISCALL *)(void *, int))A_SET_CONTINUE)((void *)st, g_start_cont ? 1 : 0);
    ((void(THISCALL *)(void *))A_GOTO_EPISODE_START)((void *)area);
    g_start_count++;
}

/* ------------------------------------------------------------------ lobby life cycle */
static lobby_t *lobby_get(void)
{
    if (g_lobby) return g_lobby;
    lobby_env e;
    memset(&e, 0, sizeof e);
    e.command = env_command, e.session = env_session, e.now = env_now, e.text = env_text, e.async_start = env_async_start, e.async_poll = env_async_poll,
    e.save = env_save, e.clipboard = env_clipboard, e.local_address = env_local_address;
    g_lobby = lobby_new(&e);
    if (g_lobby) lobby_set_settings(g_lobby, &g_set);
    return g_lobby;
}

static void lobby_begin(void *self)
{
    lobby_t *l = lobby_get();
    if (!l) return;
    g_obj = self;
    g_open_tick = GetTickCount();
    g_ov_frames_open = overlay_frames();
    g_armed = 0;
    memset(k_prev, 0, sizeof k_prev), memset(c_prev, 0, sizeof c_prev);
    lobby_set_settings(l, &g_set);
    lobby_open(l);
    if (g_hide_cm) cm_set_hidden(1);
    if (g_gate) gate_set(1);
    MLOG("lobby opened (object %p, common menu hidden %d, input gate %d)", self, g_hide_cm, g_gate);
}

/* restore: 1 = give the stock menu back (the player went back), 0 = the game is starting */
static void lobby_end(int restore)
{
    if (!g_obj && !g_gate_on && !g_cm_hidden) return;
    if (g_obj) g_closed = g_obj;
    g_obj = NULL;
    if (g_lobby) lobby_close(g_lobby);
    gate_set(0);
    if (restore) {
        if (g_cm_hidden) cm_set_hidden(0);
        cm_resume();
    }
    g_cm_hidden = 0;
    if (restore) publish_scene(0);   /* (a start leaves the last scene up: it ages out by itself while the game loads) */
    MLOG("lobby closed (restore %d)", restore);
}

static void THISCALL h_session_update(void *self)
{
    ((void(THISCALL *)(void *))A_SESSION_BASEUPDATE)(self);
    if (!g_enabled || !self || self == g_closed) return;
    if (self != g_obj) lobby_begin(self);
    if (!g_lobby) return;
    g_last_update = GetTickCount();
    drain_notes();
    lobby_event ev[16];
    lobby_view v;
    lobby_view_get(g_lobby, &v);
    int n = poll_input(ev, 16, v.input_on);
    for (int i = 0; i < n; i++) lobby_event_in(g_lobby, &ev[i]);
    lobby_update(g_lobby);
    int req = lobby_take_request(g_lobby);
    if (req == LR_CLOSE) {
        set_menu_id(MENU_ID_CAMPAIGN);
        lobby_end(1);
        return;
    }
    if (req == LR_START) {
        int ep, part, df, ch, gt;
        lobby_start_params(g_lobby, &ep, &part, &df, &ch, &gt);
        game_start(ep, part, df);
        publish_scene(1);   /* "Starting the game..." stays up until the menu object goes away */
        lobby_end(0);
        return;
    }
    publish_scene(1);
}

static void *THISCALL h_session_dtor(void *self, int flag)
{
    int id = menu_id();
    if (self == g_obj) lobby_end(id < 0xD || id > 0x17);   /* (a close through the stock flow: the menu id changed under us; a start keeps the list hidden) */
    if (self == g_closed) g_closed = NULL;
    return ((void *(THISCALL *)(void *, int))A_SESSION_DTOR)(self, flag);
}

/* ------------------------------------------------------------------ hooks into the game's code */
static void *make_tramp(unsigned target, const unsigned char *expect, int n)
{
    unsigned char cur[16];
    if (n < 5 || n > 12 || !r2_safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        MLOG("bytes at %08x differ from the supported build, hook skipped", target);
        return NULL;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    return t;
}

static int patch_jmp(unsigned target, int n, void *hook)
{
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    unsigned char *p = (unsigned char *)target;
    for (int i = 5; i < n; i++) p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    return 1;
}

static int hook_entry(unsigned target, const unsigned char *expect, int n, void *hook, void **orig)
{
    void *t = make_tramp(target, expect, n);
    if (!t) return 0;
    *orig = t;
    return patch_jmp(target, n, hook);
}

/* (2) the campaign page hides the row 0x14 by SetItemState(0x14, 2): show it for the main campaign */
static void THISCALL h_campaign_setitem(void *cm, int id, int state)
{
    if (id == 0x14 && g_enabled && gm() && rd32(gm() + 0x5C) == 0) state = 0;
    ((setitem_t)A_COMMON_SETITEM)(cm, id, state);
}

/* (4) pause menu */
static void (THISCALL *orig_pause_vis)(void *);
static void (THISCALL *orig_pause_select)(void *);
static void (THISCALL *orig_popup_setmsg)(void *, int, int);

static int net_game_running(void)
{
    cs_t *s = coop_net_session_peek();
    return s && lobby_session_running(s);
}

static void THISCALL h_pause_vis(void *self)
{
    orig_pause_vis(self);
    if (!g_enabled || !net_game_running()) return;
    unsigned proc = (unsigned)self, a = rd32(proc + 8), cm = a ? rd32(a + 0x30) : 0;
    if (!cm) return;
    ((setitem_t)A_COMMON_SETITEM)((void *)cm, 0x36, 2);   /* the local split screen join / end rows would break the session */
    ((setitem_t)A_COMMON_SETITEM)((void *)cm, 0x37, 2);
    ((setitem_t)A_COMMON_SETITEM)((void *)cm, 0x3B, 0);   /* "Leave" */
}

static void THISCALL h_pause_select(void *self)
{
    unsigned proc = (unsigned)self, a = rd32(proc + 8), cm = a ? rd32(a + 0x30) : 0;
    int leave = g_enabled && cm && rd32(cm + 0x2C4) == 0x3B && net_game_running();
    if (leave) {
        g_leave_msg = 1;
        MLOG("pause menu: Leave session selected");
    }
    orig_pause_select(self);
    g_leave_msg = 0;
}

static void THISCALL h_popup_setmsg(void *p, int table, int id)
{
    if (g_leave_msg && table == 58 && id == 0xD) id = 116;   /* "Quit session?" instead of "If you quit any unsaved progress will be lost." */
    orig_popup_setmsg(p, table, id);
}

void menu_install(void)
{
    if (g_installed) return;
    if (!r2_exe_ok() || !r2_text_decrypted()) return;
    char v[16];
    InitializeCriticalSection(&g_lock);
    ini_path();
    settings_load();
    if (r2_env("REV2_MENU", v, sizeof v) && v[0] == '0') g_enabled = 0;
    if (r2_env("REV2_MENU_AUTOOPEN", v, sizeof v)) g_autoopen = v[0] == '1';
    else if (r2_env("REV2_CONSOLE", v, sizeof v)) g_autoopen = 0;   /* test runs drive the menus with keys: no surprise screens */
    if (r2_env("REV2_MENU_HIDECM", v, sizeof v)) g_hide_cm = v[0] == '1';
    if (r2_env("REV2_MENU_GATE", v, sizeof v)) g_gate = v[0] == '1';
    if (r2_env("REV2_OVERLAY_MODE", v, sizeof v)) overlay_set_mode(atoi(v));
    if (r2_env("REV2_OVERLAY_TRACE", v, sizeof v)) overlay_set_trace(atoi(v));
    g_installed = 1;
    MLOG("settings %s: name '%s' character %s transport %s port %d; entry %s, autoopen %d", g_ini_path[0] ? g_ini_path : "(no path)", g_set.name, char_words[g_set.character & 3],
         g_set.transport ? "direct" : "steam", g_set.port, g_enabled ? "on" : "OFF", g_autoopen);
    if (!g_enabled) return;

    /* 1. the row of the campaign page */
    unsigned rows = rd32(0x13947C8 + 2 * 0x14);
    if (rows == 0x1393D70 && rd32(A_ROW_COOP) == 0x14) {
        unsigned row[9];
        r2_safe_read((void *)A_ROW_COOP, row, sizeof row);
        row[1] = 3, row[2] = 120;                       /* label: m03_sys_title_topmenu #120 "Co-Op Play" */
        row[4] = 3, row[5] = 126, row[6] = 3, row[7] = 126;   /* help (normal and greyed): #126 "Play the game in 2-player mode with a friend." */
        if (wr_data(A_ROW_COOP, row, sizeof row)) g_hooks |= 1;
    } else
        MLOG("campaign page rows differ (rows %08x id %08x): entry not patched", rows, rd32(A_ROW_COOP));

    /* 2. the call that hides the row */
    {
        unsigned char c[5];
        if (r2_safe_read((void *)A_CALL_HIDE_COOP, c, 5) && c[0] == 0xE8 && (unsigned)(A_CALL_HIDE_COOP + 5 + *(int *)(c + 1)) == A_COMMON_SETITEM) {
            DWORD old;
            if (VirtualProtect((void *)A_CALL_HIDE_COOP, 5, PAGE_EXECUTE_READWRITE, &old)) {
                *(int *)(A_CALL_HIDE_COOP + 1) = (int)((unsigned)h_campaign_setitem - (A_CALL_HIDE_COOP + 5));
                VirtualProtect((void *)A_CALL_HIDE_COOP, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void *)A_CALL_HIDE_COOP, 5);
                g_hooks |= 2;
            }
        } else
            MLOG("the call at %08x is not the expected SetItemState: row stays hidden", A_CALL_HIDE_COOP);
    }

    /* 3. vtable of the dormant session menu */
    if (rd32(A_VT_SESSION + 0x14) == A_SESSION_UPDATE && rd32(A_VT_SESSION) == A_SESSION_DTOR) {
        wr32(A_VT_SESSION + 0x14, (unsigned)h_session_update);
        wr32(A_VT_SESSION, (unsigned)h_session_dtor);
        g_hooks |= 4;
    } else
        MLOG("vtable of cDemoSessionSelectMenu differs: lobby not hooked");

    /* 4. pause menu */
    static const unsigned char b_vis[] = {0x83, 0xEC, 0x7C, 0x53, 0x55};
    static const unsigned char b_sel[] = {0x53, 0x56, 0x57, 0x8B, 0xF9};
    static const unsigned char b_msg[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x81, 0xD0, 0x02, 0x00, 0x00};
    void *o;
    if (hook_entry(A_PAUSE_VIS, b_vis, 5, (void *)h_pause_vis, &o)) orig_pause_vis = o, g_hooks |= 8;
    if (hook_entry(A_PAUSE_SELECT, b_sel, 5, (void *)h_pause_select, &o)) orig_pause_select = o, g_hooks |= 16;
    if (hook_entry(A_POPUP_SETMSG, b_msg, 10, (void *)h_popup_setmsg, &o)) orig_popup_setmsg = o, g_hooks |= 32;
    MLOG("installed: campaign row %s, hide-call %s, lobby object %s, pause rows %s, pause select %s, popup text %s", g_hooks & 1 ? "ok" : "NO", g_hooks & 2 ? "ok" : "NO",
         g_hooks & 4 ? "ok" : "NO", g_hooks & 8 ? "ok" : "NO", g_hooks & 16 ? "ok" : "NO", g_hooks & 32 ? "ok" : "NO");
}

/* ------------------------------------------------------------------ per pass of the game loop */
void menu_frame(void)
{
    if (!g_installed || !g_enabled) return;
    unsigned now = GetTickCount();
    /* the lobby object vanished without telling us (a hook failed to see the destruction): give the stock menu its input back */
    if ((g_obj || g_gate_on) && now - g_last_update > 1500) {
        MLOG("lobby: no update for %u ms, closing", now - g_last_update);
        lobby_end(1);
    }
    /* diagnostics: the picture stopped while the lobby is open (the wedge seen on the first hardware run) */
    if (g_obj) {
        unsigned f = d3d_frames();
        if (f != g_wd_frames) g_wd_frames = f, g_wd_tick = now;
        else if (g_wd_tick && now - g_wd_tick > 3000) {
            unsigned app = rd32(0x15DF9CC);
            unsigned char a20 = 0, a2551 = 0, a2657 = 0, a2660 = 0;
            r2_safe_read((void *)(app + 0x20), &a20, 1), r2_safe_read((void *)(app + 0x2551), &a2551, 1);
            r2_safe_read((void *)(app + 0x2657), &a2657, 1), r2_safe_read((void *)(app + 0x2660), &a2660, 1);
            MLOG("RENDER STALLED for %u ms with the lobby open (Present count %u): app %08x flags +20=%u +2551=%u +2657=%u +2660=%u, foreground %d, hide %d gate %d", now - g_wd_tick, f, app, a20, a2551,
                 a2657, a2660, in_foreground(), g_cm_hidden, g_gate_on);
            g_wd_tick = now + 600000;   /* once */
        }
    } else
        g_wd_tick = 0;
    /* the overlay never drew (no d3dx9_43.dll, a failing font): the player would face a blank screen, so go back to the stock menu */
    if (g_obj && now - g_open_tick > 2500 && overlay_frames() == g_ov_frames_open) {
        MLOG("lobby: the overlay drew nothing in 2.5 s (%s): back to the campaign menu", overlay_available() ? "device problem" : "d3dx9_43.dll missing");
        set_menu_id(MENU_ID_CAMPAIGN);
        lobby_end(1);
    }
    cs_t *s = coop_net_session_peek();
    if (!s) return;
    int active = cs_active(s);
    unsigned area = active || g_obj ? demo_area() : 0;
    /* a running net game and the title menus are on screen again: the player left the game (pause menu > Leave / Exit): the session ends with it */
    if (active && area && !g_obj && lobby_session_running(s)) {
        int id = menu_id();
        if (id == 8 || id == 9 || id == MENU_ID_CAMPAIGN || id == 0xC) {
            if (++g_out_frames > 90) {
                char out[100];
                MLOG("back at the menus with a running net game (menu %d): leaving the session", id);
                coop_net_command("stop", out, sizeof out);
                g_out_frames = 0;
            }
        } else
            g_out_frames = 0;
    } else
        g_out_frames = 0;
    /* an invitation / "Join game" click joined a lobby while the player sits in the menus: open the co-op screen */
    if (g_autoopen && active && area && !g_obj && !cs_is_host(s) && cs_phase(s) == CP_PHASE_LOBBY && cs_pid(s) >= 0) {
        unsigned a = area;
        int id = menu_id();
        if ((id == 8 || id == 9 || id == MENU_ID_CAMPAIGN) && (rd32(a + 4) & 0xFF) == 4) {
            static unsigned last_try;
            if (now - last_try > 3000) {
                last_try = now;
                MLOG("a session was joined while in the menus (menu %d): opening the co-op screen", id);
                set_menu_id(MENU_ID_LOBBY);
            }
        }
    }
}

/* ------------------------------------------------------------------ console */
static void dump_view(char *out, int size)
{
    if (!g_lobby) {
        snprintf(out, (size_t)size, "no lobby object yet");
        return;
    }
    lobby_view v;
    lobby_view_get(g_lobby, &v);
    int o = snprintf(out, (size_t)size, "screen %d \"%s\" %s\n", v.screen, v.title, v.subtitle);
    if (v.prompt[0]) o += snprintf(out + o, (size_t)size - (size_t)o, "prompt: %s\n", v.prompt);
    if (v.input_on) o += snprintf(out + o, (size_t)size - (size_t)o, "input: [%s]\n", v.input);
    for (int i = 0; i < v.nrows && o < size - 200; i++)
        o += snprintf(out + o, (size_t)size - (size_t)o, "%c %s%s%s%s\n", i == v.sel ? '>' : ' ', v.rows[i].kind == LV_GREY ? "(grey) " : v.rows[i].kind == LV_INFO ? "(info) " : "", v.rows[i].label,
                      v.rows[i].value[0] ? "  :  " : "", v.rows[i].value);
    for (int i = 0; i < v.nplayers && o < size - 200; i++)
        o += snprintf(out + o, (size_t)size - (size_t)o, "player %s%s %s %s ping %d%s\n", v.players[i].name, v.players[i].me ? " (me)" : "", v.players[i].chr, v.players[i].ready ? "ready" : "not ready",
                      v.players[i].ping, v.players[i].host ? " host" : "");
    if (v.status[0]) o += snprintf(out + o, (size_t)size - (size_t)o, "status[%d]: %s\n", v.status_kind, v.status);
    if (v.note[0] && o < size - 300) o += snprintf(out + o, (size_t)size - (size_t)o, "note: %s\n", v.note);
}

void menu_command(const char *arg, char *out, int size)
{
    if (!lt_on_game_thread()) {   /* everything here touches the lobby or the game: run it on the game thread */
        char c[400];
        snprintf(c, sizeof c, "menu %s", arg);
        lt_post(c, out, size);
        return;
    }
    char w[32] = "", rest[300] = "";
    sscanf(arg, "%31s %299[^\r\n]", w, rest);
    if (!w[0] || !strcmp(w, "status")) {
        char ov[160];
        overlay_stats(ov, sizeof ov);
        unsigned g = gm();
        snprintf(out, (size_t)size,
                 "menu: %s, hooks %02x (1 row, 2 hide-call, 4 lobby object, 8 pause rows, 16 pause select, 32 popup text), lobby %s, scene %d, menu id %d, aDemo %08x, common menu %08x (hidden %d), gate %d (BC0=%d), "
                 "armed %d, starts %u, autoopen %d\nsettings file %s\n%s",
                 g_enabled ? "on" : "OFF", g_hooks, g_obj ? "OPEN" : "closed", g_scene_valid, g ? (int)rd32(g + 0x20) : -1, demo_area(), common_menu(), g_cm_hidden, g_gate_on,
                 rd32(A_BIOPAD) ? (int)(rd32(rd32(A_BIOPAD) + 0xBC0) & 0xFF) : -1, g_armed, g_start_count, g_autoopen, g_ini_path, ov);
    } else if (!strcmp(w, "open")) {
        if (!demo_area()) snprintf(out, (size_t)size, "not in the menus (no aDemo)");
        else {
            set_menu_id(MENU_ID_LOBBY);
            snprintf(out, (size_t)size, "menu id set to 0x18; the lobby opens with the next menu update");
        }
    } else if (!strcmp(w, "close")) {
        set_menu_id(MENU_ID_CAMPAIGN);
        lobby_end(1);
        snprintf(out, (size_t)size, "back to the campaign menu");
    } else if (!strcmp(w, "view")) {
        dump_view(out, size);
    } else if (!strcmp(w, "ev")) {   /* menu ev up|down|left|right|ok|back|bs|paste [row] */
        char k[16] = "";
        int row = 0;
        sscanf(rest, "%15s %d", k, &row);
        lobby_event e = {LE_NONE, 0, row};
        e.kind = !strcmp(k, "up") ? LE_UP : !strcmp(k, "down") ? LE_DOWN : !strcmp(k, "left") ? LE_LEFT : !strcmp(k, "right") ? LE_RIGHT : !strcmp(k, "ok") ? LE_OK
               : !strcmp(k, "back") ? LE_BACK : !strcmp(k, "bs") ? LE_BACKSPACE : !strcmp(k, "paste") ? LE_PASTE : !strcmp(k, "click") ? LE_CLICK : LE_NONE;
        if (!g_lobby || !g_obj) snprintf(out, (size_t)size, "the lobby is not open");
        else if (e.kind == LE_NONE) snprintf(out, (size_t)size, "usage: menu ev up|down|left|right|ok|back|bs|paste|click <row>");
        else {
            lobby_event_in(g_lobby, &e);
            dump_view(out, size);
        }
    } else if (!strcmp(w, "text")) {   /* menu text <string>: typed into the address field */
        if (!g_lobby || !g_obj) snprintf(out, (size_t)size, "the lobby is not open");
        else {
            for (const char *p = rest; *p; p++) {
                lobby_event e = {LE_CHAR, *p, 0};
                lobby_event_in(g_lobby, &e);
            }
            dump_view(out, size);
        }
    } else if (!strcmp(w, "start")) {   /* menu start <episode> <part> <difficulty>: the stock start sequence, no lobby */
        int ep = 1, part = 0, df = 1, cont = 0;
        sscanf(rest, "%d %d %d %d", &ep, &part, &df, &cont);
        if (!demo_area()) snprintf(out, (size_t)size, "not in the menus (no aDemo)");
        else {
            g_start_cont = cont != 0;
            game_start(ep, part, df);
            g_start_cont = 0;
            snprintf(out, (size_t)size, "start sequence done: menu id now 0x%x", menu_id());
        }
    } else if (!strcmp(w, "set")) {   /* menu set name=<n> | ip=<a> | character=<c> | solo=0|1 */
        lobby_settings s = g_set;
        char *eq = strchr(rest, '=');
        if (!eq) snprintf(out, (size_t)size, "usage: menu set name=|ip=|port=|character=|solo=|episode=|difficulty=<value>");
        else {
            *eq = 0;
            const char *val = eq + 1;
            if (!strcmp(rest, "name")) snprintf(s.name, sizeof s.name, "%s", val);
            else if (!strcmp(rest, "ip")) snprintf(s.ip, sizeof s.ip, "%s", val);
            else if (!strcmp(rest, "port")) s.port = atoi(val);
            else if (!strcmp(rest, "character")) s.character = cp_char_parse(val) <= 3 ? cp_char_parse(val) : s.character;
            else if (!strcmp(rest, "solo")) s.solo = atoi(val) != 0;
            else if (!strcmp(rest, "episode")) s.episode = atoi(val);
            else if (!strcmp(rest, "difficulty")) s.difficulty = atoi(val);
            settings_save(&s);
            if (g_lobby) lobby_set_settings(g_lobby, &s);
            snprintf(out, (size_t)size, "saved: name '%s' ip '%s' port %d character %s solo %d episode %d difficulty %d", s.name, s.ip, s.port, char_words[s.character & 3], s.solo, s.episode, s.difficulty);
        }
    } else if (!strcmp(w, "autoopen")) {
        g_autoopen = atoi(rest) != 0;
        snprintf(out, (size_t)size, "autoopen = %d", g_autoopen);
    } else if (!strcmp(w, "gate") || !strcmp(w, "hidecm") || !strcmp(w, "enabled")) {   /* runtime switches */
        int on = atoi(rest);
        if (!strcmp(w, "gate")) g_gate = on, gate_set(on && g_obj ? 1 : 0);
        else if (!strcmp(w, "hidecm")) {
            g_hide_cm = on;
            if (g_obj) cm_set_hidden(on);   /* (never called while the lobby is closed: see g_hide_cm) */
        }
        else g_enabled = on;
        snprintf(out, (size_t)size, "%s = %d", w, on);
    } else if (!strcmp(w, "ov")) {   /* menu ov <0|1|2> [trace frames]: overlay diagnostics (0 nothing, 1 rectangles only, 2 all) */
        int mode = 2, tr = 0;
        sscanf(rest, "%d %d", &mode, &tr);
        overlay_set_mode(mode);
        overlay_set_trace(tr);
        snprintf(out, (size_t)size, "overlay mode %d, trace %d frames", mode, tr);
    } else if (!strcmp(w, "pad")) {
        unsigned short b;
        short lx, ly;
        int any = testmode_pad_snapshot(&b, &lx, &ly);
        snprintf(out, (size_t)size, "pad snapshot: %s buttons %04x stick %d,%d; foreground %d; up %d down %d enter %d esc %d", any ? "pad" : "no pad", b, lx, ly, in_foreground(), inject_vk_down(VK_UP),
                 inject_vk_down(VK_DOWN), inject_vk_down(VK_RETURN), inject_vk_down(VK_ESCAPE));
    } else if (!strcmp(w, "lang")) {   /* menu lang <0-11|off>: test only, see g_lang_override */
        g_lang_override = !strcmp(rest, "off") || !rest[0] ? -1 : atoi(rest);
        snprintf(out, (size_t)size, "lobby language %s (%d)", g_lang_override < 0 ? "= the game's" : lobby_lang_names[g_lang_override % 12], g_lang_override);
    } else if (!strcmp(w, "text0")) {   /* menu text0 <table> <id>: a string of the game in the current language */
        int t = 0, id = 0;
        sscanf(rest, "%d %d", &t, &id);
        const char *s = game_text(t, id);
        snprintf(out, (size_t)size, "language %d: %s", game_lang(), s ? s : "(none)");
    } else {
        snprintf(out, (size_t)size, "menu: status | open | close | view | ev <k> | text <s> | start <ep> <part> <diff> | set k=v | gate 0|1 | hidecm 0|1 | enabled 0|1 | autoopen 0|1 | ov <mode> | pad | text0 <table> <id> | lang <0-11|off>");
    }
}
