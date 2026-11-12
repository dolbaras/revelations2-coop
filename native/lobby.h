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
    LT_REF_PROTO, LT_REF_MODVER, LT_REF_MODVER_OLD, LT_PARTNER_SILENT,   /* 0.2.0 resilience: lobby_refusal_text / lobby_silent_text below */
    LT_WAITING, LT_PARTNER_JOINED, LT_ENTER_ADDR, LT_CONNECTING, LT_NO_FRIENDS, LT_REFRESH, LT_NAME, LT_PING, LT_HINT, LT_HINT_IP,
    LT_YOUR_ADDR, LT_STEAM_HINT, LT_STEAM_NA, LT_NEED_READY, LT_NOT_READY, LT_SEARCHING, LT_CHARACTER, LT_STARTING, LT_YOU, LT_HOST_TAG,
    LT_PARTNER_READY, LT_LEAVE_HINT, LT_SAVE_WARN,
    LT_WAIT_LOAD,   /* stage 6i: banner "Waiting for %s to finish loading..." (one %s = the player's name) */
    LT_LEG_OK,      /* legend word of the confirm key at the bottom of the lobby (the back key uses the stock "Back") */
    LT_DOWNED,      /* host row "Downed / revive" (value Yes / No) */
    LT_DOWN_SELF,   /* banner while my character lies: one %s = seconds left */
    LT_DOWN_PARTNER,/* banner while my partner lies: one %s = seconds left */
    /* ---- 0.2.0 lobby (lobby-must) ---- */
    /* stock strings (the game's own, all 12 languages); the K_FORMAT ones carry the game's <USER> / <SWAP> tag, which menu.c turns into one %s */
    LT_REMOVED,           /* m19 #39 "You have been removed from the session by the host." */
    LT_REF_BUILD,         /* m19 #57 "You have a different version of the game and could not join this session." */
    LT_KICK_ASK,          /* m19 #45 "Do you want to kick <USER>?"            (format: the partner's name) */
    LT_PARTNER_LEFT_ROOM, /* m09 #143 "<SWAP> has left the session."           (format) */
    LT_PARTNER_LOST_ROOM, /* m09 #142 "<SWAP> has disconnected."               (format) */
    LT_SESSION_OVER,      /* m40 #76 "Session has been concluded." (the host closed the room) */
    /* own strings */
    LT_REF_FULL, LT_REF_STARTED, LT_REF_NOANSWER, LT_REF_BAD,              /* refusal reasons (lobby_refusal_text) */
    LT_PARTNER_LEFT, LT_PARTNER_LOST,                                       /* in-game strip on the host: one %s = the partner's name */
    LT_HOST_ENDED, LT_HOST_LOST,                                            /* in-game strip on the guest, then back to the title */
    LT_EP1_END,                                                             /* the end of Episode 1 in co-op */
    LT_CONNECTING_TO, LT_AWAITING,                                          /* guest: one %s = the address or the friend */
    LT_HOST_WAIT_READY, LT_HOST_CAN_START, LT_GUEST_PICK, LT_GUEST_WAIT_HOST,   /* room status lines (one %s = the other player, LT_GUEST_PICK none) */
    LT_KICK,                                                                /* host row "Remove partner" */
    LT_HOST_NET_HINT,                                                       /* room info for the System Link host: one %s = the UDP port */
    LT_END_COOP, LT_END_COOP_HELP,                                          /* the host's pause menu row and its help line (drawn by the GAME's GUI font) */
    LT_REF_GONE, LT_REF_DENIED, LT_REF_NOLOBBY, LT_REF_NOTCOOP, LT_REF_STEAM_NOANSWER,   /* lang: the reason of a failed Steam join (lobby_backend_reason) */
    LT_MS,                                                                  /* lang: the room table's ping, one %s = milliseconds ("%s ms", "%s мс") */
    /* ---- 0.2.1 coopsave ---- */
    LT_CONTINUE,    /* stock m03 #87 "Continue": the host's row (value Yes / No) */
    LT_CONT_NEED,   /* own: Continue needs a co-op save of this half on both PCs */
    /* ---- 0.3.0 all-episodes ---- */
    LT_EP_STRUGGLE, LT_EP_LITTLE,   /* stock m03 #74 / #75: the main menu's names of the bonus episodes "The Struggle" / "Little Miss" (the Episode row's values) */
    LT_EVGENY, LT_DARK_NATALIA,     /* stock m01 #93 / #101: the bonus episodes' second figure (slot 1 of The Struggle / of Little Miss) */
    LT_COUNT
};
/* the own text for a backend's English failure reason (coop_steam.c "Steam: ..."; CS_N_REFUSED a = 0), -1 when none fits (the caller then shows no reason) */
int lobby_backend_reason(const char *why);
/* texts that are printf formats with exactly the %s listed above (the stock ones after menu.c turned <USER> / <SWAP> into %s) */
int lobby_text_args(int text);
/* an own or stock-shaped format with up to two values, cut to size like lobby_refusal_text (any other % of the table is not allowed: tests/test_resil.c) */
const char *lobby_format(const char *fmt, const char *a, const char *b, char *out, size_t size);
/* the in-game strip for a session note (0.2.0 lobby-must): the LT_ text, or -1 when the note needs none. is_host: the note came to the host (PEER_LEAVE) or to a guest
 * (LOST). Host: PEER_LEAVE b = CS_GONE_EP_END -> LT_EP1_END; a = 1 (lost) or b = CS_GONE_SILENT -> LT_PARTNER_LOST; otherwise LT_PARTNER_LEFT (also the host's own
 * End Co-Op, b = CS_GONE_REMOVED). Guest: LOST a = 1 b = CP_LEAVE_EP_END -> LT_EP1_END; a = 1 -> LT_HOST_ENDED; otherwise LT_HOST_LOST. */
int lobby_loss_text(int what, int a, int b, int is_host);
/* the strip / status text of a loss in a language: name = the other player (shown as "<player_word> 1" / "... 2" when it is a default name; player_word NULL =
 * the English fallback of the stock "Player") */
const char *lobby_loss_line(int text, const char *name, const char *player_word, int lang, char *out, size_t size);
/* the name shown for a player: the default wire names "Player 1" / "Player 2" read "<stock Player> 1" / "... 2" in the player's language (player_word = LT_PLAYER) */
const char *lobby_display_name(const char *wire_name, const char *player_word, char *out, size_t size);
/* stock string location (message table index of Msg_GetString, message id); table -1 = own string only */
typedef struct { int table, id; } lobby_stock;
extern const lobby_stock lobby_stock_ids[LT_COUNT];
/* own string (UTF-8) for a language: lang 0..11 in the order of lobby_lang_names (0 = English); falls back to English */
const char *lobby_own_text(int text, int lang);
/* the reason of a refusal in the player's language: code = cp_refuse_code or CS_REF_NOANSWER (CS_N_REFUSED a), host_mod = the host's mod version ("" = an older
 * host that sends none), own_mod = this game's; returns out, or NULL for a code without an own text (0 = a backend's own reason, e.g. Steam: menu.c then asks
 * lobby_backend_reason and never shows the session's English text). BUILD and REMOVED are stock strings: here their English fallback (menu.c shows the game's own words). Never longer than size - 1 bytes and never cut
 * inside a UTF-8 character. */
const char *lobby_refusal_text(int code, const char *host_mod, const char *own_mod, int lang, char *out, size_t size);
/* the strip while the partner's game sends nothing (CS_N_SILENT): "No answer from <name> for <seconds> s ..." (same size rule) */
const char *lobby_silent_text(const char *name, int seconds, int lang, char *out, size_t size);
int lobby_lang_from_game(int game_lang_index);   /* the game's language index ([sGUI]+0x28: 0 jpn, 1 eng, 2 fre, 3 spa, 4 ger, 5 ita, 6 kor, 7 chT, 8 chS, 9/21 ptB, 10 rus, 11 pol) -> 0..11 */
extern const char *const lobby_lang_names[12];

/* ---- episodes (0.3.0 all-episodes): the lobby's episode ids are the game's own: 1..4 of the story (game type 0), 5 = The Struggle (type 3), 9 = Little Miss (type 4) ---- */
enum { LOBBY_EP_STRUGGLE = 5, LOBBY_EP_LITTLE = 9, LOBBY_GT_STORY = 0, LOBBY_GT_STRUGGLE = 3, LOBBY_GT_LITTLE = 4 };
int lobby_episode_valid(int episode);          /* 1..4, 5, 9 */
int lobby_episode_game_type(int episode);      /* the GameMan type that starts it: 0, 3 or 4 (0 for an id that is not valid) */
int lobby_episode_is_bonus(int episode);       /* 5 or 9: one part, the two figures are slot 0 / slot 1 of the episode, not the four characters of the story */
int lobby_episode_step(int episode, int dir);  /* the next id of the row: 1 2 3 4 5 9 (wraps) */
/* Barry's half of Episodes 2-4 cannot be started by the stock new-game call (static read of Game_PrepareEpisodeStart 0x4240A0 and the live runs allepL2: a new game of an episode
 * other than 1 goes through the results area 9000 and Game_AdvanceToNextPart, which turns part 1 into the next episode; the Continue branch shows the clear results first). The lobby
 * therefore starts PART 0 of the episode and the host hops into part 1 once the stage is in play (the lab warp with `half`, carried to the partner by the retry coupling). This returns
 * the first stage of part 1 (250 / 351 / 451) for such a start, else 0. */
int lobby_second_half_stage(int episode, int part, int game_type);
/* the start position index of that hop (Episode 2: 0, Episodes 3 and 4: 1 = after the part opening: position 0 of 351 / 451 shows no player objects, live catalog 2L.1) */
int lobby_second_half_startpos(int episode);
/* the name of the character in a slot of a game type: the story's four names by CP_CHAR_*, in the bonus episodes the slot's unit (type 3: Moira / Evgeny, type 4: Natalia / Dark Natalia) */
int lobby_char_text(int game_type, int character);   /* an LT_ id (LT_CLAIRE ..) or -1 for CP_CHAR_NONE */

/* ---- environment supplied by the host application ---- */
typedef struct lobby_settings {
    char name[CP_NAME_MAX + 1];   /* player name ("" = the session default) */
    int character;                /* CP_CHAR_* wanted when hosting or joining */
    char ip[64];                  /* last direct address */
    int port;                     /* host port for direct sessions */
    int transport;                /* 0 Steam friends, 1 direct IP: last way used */
    int episode;                  /* host default: 1..4, or a bonus episode (5 The Struggle, 9 Little Miss) */
    int difficulty;               /* 0..3, host default */
    int solo;                     /* dev switch: the host may start alone (`start force`) */
    int sflags;                   /* session flags byte the host publishes (CP_SESS_*: the downed / revive rule and its bleed-out preset) */
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
    int (*coop_save)(void *user, int *episode, int *part, int *difficulty);   /* 0.2.1: this PC's co-op save can continue a checkpoint (1) of that room episode (1-4, 5, 9: 0.3.1; Episode 1 only before) and half; NULL / 0 = none */
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
    char chr[48];   /* (0.3.0: "Темная Наталья" is 28 bytes; the overlay shrinks a long name to its column) */
    int ready, host, me, ping;   /* ping -1 unknown */
} lobby_vplayer;

enum { LS_TOP = 0, LS_HOST_WAY, LS_JOIN_WAY, LS_JOIN_IP, LS_FRIENDS, LS_CONNECTING, LS_ROOM, LS_LEAVE_ASK, LS_KICK_ASK };
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
    char note[260];          /* small grey note under the status line (the save slot warning; on the leave question: the session ends for both) */
    char info[1024];          /* room: a wrapped block under the player table (Episode 1 only, or the experimental line + the Barry-half note; the System Link host's port forwarding hint) */
    char col_name[32], col_char[32], col_ping[16], t_ready[32], t_not_ready[32], t_host[24];   /* table captions */
    char t_ms[24];           /* the ping format of the table (LT_MS: one %s) */
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
