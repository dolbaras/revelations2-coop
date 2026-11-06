/*
 * Co-op lobby screens (stage 8): the model behind the "Co-Op" menu entry. Pure C, no Windows UI and no game dependencies besides the
 * session layer (coop_session.h): menu.c (game glue), overlay.c (Direct3D 9 drawing) and the offline test (tests/test_lobby.c) use the same code.
 *
 * The model is a small state machine of screens: top (Host / Join), the way to host or join (Steam friends / direct IP), IP input,
 * Steam friend list, connecting, and the room (character choice, episode, difficulty, ready, start). It never draws anything: lobby_view()
 * fills a flat description (title, rows, text field, player table, status line) which the overlay renders. Input arrives as events.
 * Everything that talks to the network goes through the console grammar of the glue (`host`, `join`, `char`, `ready`, `start`, `session`,
 * `stop`, `steamhost`, `steamjoin`, `steamlobbies`, ...), so the lobby and the test console drive the same code paths.
 */
#ifndef LOBBY_H
#define LOBBY_H

#include <stddef.h>
#include <stdint.h>

#include "coop_session.h"

/* ---- localised texts: stock game strings where the game has them in every language, own strings otherwise (lobby_text.c) ---- */
enum lobby_text {
    LT_COOP, LT_HOST, LT_JOIN, LT_FRIEND, LT_LINK, LT_BACK, LT_READY, LT_START, LT_DIFFICULTY, LT_EPISODE,
    LT_CLAIRE, LT_MOIRA, LT_BARRY, LT_NATALIA, LT_PLAYER, LT_PARTNER, LT_YES, LT_NO, LT_QUIT_SESSION, LT_LEAVE, LT_SAME_SEL,
    LT_FAIL_JOIN, LT_FAIL_CREATE, LT_CONN_LOST, LT_CASUAL, LT_NORMAL, LT_HARD, LT_VERY_HARD, LT_INVITE,
    /* own strings from here on */
    LT_WAITING, LT_PARTNER_JOINED, LT_ENTER_ADDR, LT_CONNECTING, LT_NO_FRIENDS, LT_REFRESH, LT_NAME, LT_PING, LT_HINT, LT_HINT_IP,
    LT_YOUR_ADDR, LT_STEAM_HINT, LT_STEAM_NA, LT_NEED_READY, LT_NOT_READY, LT_SEARCHING, LT_CHARACTER, LT_STARTING, LT_YOU, LT_HOST_TAG,
    LT_PARTNER_READY, LT_LEAVE_HINT, LT_SAVE_WARN,
    LT_WAIT_LOAD,   /* stage 6i: banner "Waiting for %s to finish loading..." (one %s = the player's name) */
    LT_LEG_OK,      /* legend word of the confirm key at the bottom of the lobby (the back key uses the stock "Back") */
    LT_COUNT
};
/* stock string location (message table index of Msg_GetString, message id); table -1 = own string only */
typedef struct { int table, id; } lobby_stock;
extern const lobby_stock lobby_stock_ids[LT_COUNT];
/* own string (UTF-8) for a language: lang 0..11 in the order of lobby_lang_names (0 = English); falls back to English */
const char *lobby_own_text(int text, int lang);
int lobby_lang_from_game(int game_lang_index);   /* the game's language index ([sGUI]+0x28: 0 jpn, 1 eng, 2 fre, 3 spa, 4 ger, 5 ita, 6 kor, 7 chT, 8 chS, 9/21 ptB, 10 rus, 11 pol) -> 0..11 */
extern const char *const lobby_lang_names[12];

/* ---- environment supplied by the host application ---- */
typedef struct lobby_settings {
    char name[CP_NAME_MAX + 1];   /* player name ("" = the session default) */
    int character;                /* CP_CHAR_* wanted when hosting or joining */
    char ip[64];                  /* last direct address */
    int port;                     /* host port for direct sessions */
    int transport;                /* 0 Steam friends, 1 direct IP: last way used */
    int episode;                  /* 1..4, host default */
    int difficulty;               /* 0..3, host default */
    int solo;                     /* dev switch: the host may start alone (`start force`) */
} lobby_settings;

typedef struct lobby_env {
    void *user;
    int (*command)(void *user, const char *line, char *out, int size);   /* the glue's `net ...` grammar without the word `net`; returns 1 when handled */
    cs_t *(*session)(void *user);                                        /* the session object (created on demand) */
    uint32_t (*now)(void *user);                                         /* milliseconds */
    const char *(*text)(void *user, int lobby_text_id);                  /* localised UTF-8 string, never NULL */
    int (*async_start)(void *user, const char *line);                    /* run a blocking console line elsewhere (Steam lobby search); 0 = refused */
    int (*async_poll)(void *user, char *out, int size);                  /* 1 = finished (out filled), 0 = still running or nothing started */
    void (*save)(void *user, const lobby_settings *s);                   /* settings changed: persist */
    int (*clipboard)(void *user, char *out, int size);                   /* text on the clipboard (Ctrl+V), 1 = got some */
    int (*local_address)(void *user, char *out, int size);               /* "192.168.1.5" or "" */
} lobby_env;

/* ---- input ---- */
enum { LE_NONE = 0, LE_UP, LE_DOWN, LE_LEFT, LE_RIGHT, LE_OK, LE_BACK, LE_CHAR, LE_BACKSPACE, LE_PASTE, LE_CLICK, LE_HOVER };
typedef struct { int kind; int ch; int row; } lobby_event;   /* LE_CHAR: ch = ASCII; LE_CLICK / LE_HOVER: row index */

/* ---- what to draw ---- */
enum { LV_NORMAL = 0, LV_GREY = 1, LV_INFO = 2 };
#define LOBBY_ROWS 14
typedef struct {
    char label[96];
    char value[96];          /* right hand column: "< Normal >", the partner who holds the character, ... */
    int kind;                /* LV_* */
    int action;              /* internal id; 0 for info rows */
} lobby_row;

typedef struct {
    char name[CP_NAME_MAX + 8];
    char chr[24];
    int ready, host, me, ping;   /* ping -1 unknown */
} lobby_vplayer;

enum { LS_TOP = 0, LS_HOST_WAY, LS_JOIN_WAY, LS_JOIN_IP, LS_FRIENDS, LS_CONNECTING, LS_ROOM, LS_LEAVE_ASK };
enum { LSK_INFO = 0, LSK_ERROR = 1, LSK_OK = 2 };

typedef struct {
    int screen;
    char title[96];
    char subtitle[160];      /* second line under the title (address, session summary) */
    lobby_row rows[LOBBY_ROWS];
    int nrows, sel;
    int input_on;            /* a text field is shown */
    char input[96];
    char prompt[160];
    lobby_vplayer players[CP_MAX_PLAYERS];
    int nplayers;
    char status[240];
    int status_kind;
    char hint[240];
    int busy;                /* show a spinner */
    char note[260];          /* small grey note under the status line (the save slot warning) */
    char col_name[32], col_char[32], col_ping[16], t_ready[32], t_not_ready[32], t_host[24];   /* table captions */
    char leg_ok[32], leg_back[32];   /* key legend words (confirm / back) */
} lobby_view;

/* ---- requests of the model to the application ---- */
enum { LR_NONE = 0, LR_CLOSE, LR_START };

typedef struct lobby lobby_t;
lobby_t *lobby_new(const lobby_env *env);
void lobby_free(lobby_t *l);
void lobby_set_settings(lobby_t *l, const lobby_settings *s);
void lobby_get_settings(lobby_t *l, lobby_settings *s);
void lobby_open(lobby_t *l);                                /* the screen opens: top menu, or the room when a session exists already */
void lobby_close(lobby_t *l);
void lobby_event_in(lobby_t *l, const lobby_event *e);
void lobby_update(lobby_t *l);                              /* once per frame while open: polls the session, advances screens */
void lobby_note(lobby_t *l, int what, int pid, int a, int b, const char *text);   /* a cs_note (thread safe is the caller's job) */
void lobby_view_get(lobby_t *l, lobby_view *v);
int lobby_take_request(lobby_t *l);                         /* LR_* once */
void lobby_start_params(lobby_t *l, int *episode, int *part, int *difficulty, int *character, int *game_type);
int lobby_leave_ask_active(lobby_t *l);

/* pause menu helper: is a session running that the pause menu should offer "Leave" for */
int lobby_session_running(cs_t *s);

#endif
