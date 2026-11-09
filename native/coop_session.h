/*
 * Co-op session layer: handshake with a version gate, lobby (character picks, ready), session settings, the level-change
 * barrier, 20 Hz player state with interpolation, events, heartbeat/RTT. Game independent: the proxy's glue (coop_game.inc)
 * and the headless bot / unit tests use the same code. Wire format: coop_proto.h, docs/protocol.md.
 *
 * Threading: every function takes the session lock (recursive), so the console thread may call host/join/pick/... while the
 * game thread calls cs_poll / cs_submit_state / cs_remote_sample. Notifications are delivered from cs_poll (and from the
 * calls above when they change local state) with the lock held; they must not block.
 */
#ifndef COOP_SESSION_H
#define COOP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "coop_net.h"
#include "coop_proto.h"

typedef struct cs_t cs_t;

enum cs_notify {
    CS_N_CONNECTED = 1,    /* client: the host accepted us (pid = our pid) */
    CS_N_REFUSED,          /* client: refused or no answer (text, a = cp_refuse_code, CS_REF_NOANSWER or 0 (a backend's own reason, e.g. Steam), peer_mod = the host's mod
                              version when it sent one); a = CP_REF_MODVER also when this client left a host whose WELCOME names another mod version ("" = a host older
                              than that field); a = CP_REF_STARTED also when a no_rejoin client was welcomed into a running game */
    CS_N_LOST,             /* the connection to the host is gone / session ended (text; a = 1 the host left on purpose (b = its LEAVE reason, enum cp_leave_reason),
                              2 the host was silent for the limit, 0 transport) */
    CS_N_PEER_JOIN,        /* pid joined the lobby / reconnected */
    CS_N_PEER_LEAVE,       /* pid left or was lost (a = 1 when lost without goodbye; b = 1 given up after the silence limit, 2 its old link was replaced by a new
                              connection of the same player, 3 removed by this host (cs_kick), 4 it left because its game reached the end of the episode) */
    CS_N_LOBBY,            /* lobby table changed */
    CS_N_SESSION,          /* session settings changed */
    CS_N_START,            /* the host started the game: characters are final */
    CS_N_LOAD_BEGIN,       /* a level load started somewhere (pid = who, a = epoch, b = area id): flip puppets */
    CS_N_GO,               /* barrier open: every player finished loading (a = epoch) */
    CS_N_EVENT,            /* ev = a game event from another player */
    CS_N_NOTICE,           /* host notice for us (text) */
    CS_N_WAIT,             /* stage 6i: this machine has finished loading and still waits for the barrier (a = seconds waited, b = the barrier limit in seconds (host) or 0, pid = who is awaited, text = his name) */
    CS_N_SILENT            /* nothing at all has come from pid for a while (its game paused / in the background, a long load or a stall; the link itself is up):
                              a = seconds of silence (first after 8 s, then every 10 s), b = the limit in seconds after which the session gives up on it, text = name;
                              a = 0: pid is heard again (b = how long it was silent) */
};

/* CS_N_REFUSED a when the host never answered (direct address: nobody listens / the connect timed out / no WELCOME in time); not a wire code */
#define CS_REF_NOANSWER 100
/* CS_N_PEER_LEAVE b */
enum { CS_GONE_LEFT = 0, CS_GONE_SILENT = 1, CS_GONE_REPLACED = 2, CS_GONE_REMOVED = 3, CS_GONE_EP_END = 4 };

typedef struct {
    int what, pid, a, b;
    const char *text;
    const cp_event *ev;
    const char *peer_mod;  /* CS_N_REFUSED: the host's mod version ("" unknown); NULL otherwise */
} cs_note;

typedef struct {
    const char *name;                 /* player name shown in the lobby ("" = "Player 1" when hosting, "Player 2" when joining) */
    int want_char;                    /* CP_CHAR_* or CP_CHAR_NONE */
    uint8_t exe_tag[8];               /* first 8 bytes of the exe SHA-256; zeros = skip the build check (bot) */
    const char *mod_version;
    int is_bot;
    uint32_t (*clock)(void *);        /* NULL = real monotonic clock (tests pass a virtual one) */
    void *clock_user;
    void (*on_note)(void *user, const cs_note *n);
    void *user;
    int interp_delay_ms;              /* 0 = default 100 */
    int no_rejoin;                    /* 0.2.0 product rule (1): nobody joins after the start, not even a known name (no seat take-over, REFUSE(STARTED)); a client that
                                         is welcomed into a running game leaves at once. 0 = the lab's rejoin (a known name gets its seat back) */
} cs_config;

typedef struct {
    uint32_t tx_state, rx_state, rx_dup, rx_old, rx_stale_epoch, rx_gap;   /* summed over remote players; rx_gap = states lost on the way */
    uint32_t tx_event, rx_event, rx_bad, hb_rx;
    /* (steamgap) rx_expired: states that arrived while this game's loop was away and were dropped from the inbox (only the newest are kept): never part of rx_gap.
     * away_n / away_max_ms: how often this game's loop did not poll the session for >= 3 s (a level load, a lost display device, a pause) and the longest time */
    uint32_t rx_expired, away_n, away_max_ms;
} cs_stats;

typedef struct {
    int pid, used, connected, peer;
    char name[CP_NAME_MAX + 1];
    uint8_t character, ready, flags, loaded;
    int rtt_ms;                       /* -1 unknown */
    uint32_t rx, dup, old, stale, gap;
    uint32_t expired;                 /* (steamgap) see cs_stats.rx_expired */
} cs_player_info;

/* result of cs_remote_sample */
enum { CS_SAMPLE_NONE = 0, CS_SAMPLE_INTERP = 1, CS_SAMPLE_EXTRAP = 2, CS_SAMPLE_STALE = 3 };

cs_t *cs_new(const cs_config *cfg);
void cs_free(cs_t *s);

int cs_host(cs_t *s, const char *transport, int port, int threaded, char *err, size_t esz);
int cs_join(cs_t *s, const char *transport, const char *target, int threaded, char *err, size_t esz);
void cs_leave(cs_t *s, int reason);                    /* LEAVE + close the transport; the session object stays usable (reason: enum cp_leave_reason) */
/* host only: end the link of one player with LEAVE(reason) (CP_LEAVE_REMOVED in the room: the name is refused with REFUSE(REMOVED) until the session ends;
 * CP_LEAVE_END_COOP after the start: the host plays on alone). PEER_LEAVE b = CS_GONE_REMOVED follows at once. 1 = done */
int cs_kick(cs_t *s, int pid, int reason);
int cs_join_stage(cs_t *s);                            /* client: 0 not joining, 1 the transport connects, 2 HELLO sent (awaiting the host's reply) */

void cs_poll(cs_t *s);                                 /* drain the transport, run timers (heartbeat, deadlines, barrier timeout) */

/* lobby (the host applies at once, clients ask the host and see the answer in the next LOBBY_STATE) */
int cs_pick(cs_t *s, int character);
int cs_ready(cs_t *s, int ready);
int cs_set_session(cs_t *s, const cp_session *ss);     /* host only */
int cs_start(cs_t *s, int force, char *err, size_t esz);   /* host only: needs everybody ready unless force */

/* load barrier: call load_begin when this machine starts a level load (or at once after START), load_done when it is playable again */
void cs_load_begin(cs_t *s, uint32_t area_id);
void cs_load_done(cs_t *s);

/* player state: the caller submits its character's state as often as it likes; at most one packet per 45 ms goes out (a TELEPORT flag
 * sends at once). Returns 1 when a packet was sent. Nothing is sent outside PLAYING. */
int cs_submit_state(cs_t *s, const cp_state *st);
/* interpolated state of another player, shown ~100 ms in the past; now_ms on the session clock (cs_now) */
int cs_remote_sample(cs_t *s, int pid, uint32_t now_ms, cp_state *out);

int cs_send_event(cs_t *s, unsigned kind, const void *body, unsigned len);   /* reliable, to everybody */

/* stage 6 world records: unreliable + sequenced (newest wins), only while PLAYING, body <= CP_WORLD_MAX. The receiver polls its inbox from the game thread (a ring of the newest 64). */
int cs_send_world(cs_t *s, unsigned kind, const void *body, unsigned len);
int cs_recv_world(cs_t *s, cp_world *out);                  /* 1 = one message taken */
void cs_world_stats(cs_t *s, uint32_t *tx, uint32_t *rx, uint32_t *drop);

/* information */
int cs_active(cs_t *s);                                /* a session exists (host or joined) */
int cs_is_host(cs_t *s);
int cs_pid(cs_t *s);
int cs_phase(cs_t *s);                                 /* enum cp_phase */
unsigned cs_epoch(cs_t *s);
uint32_t cs_now(cs_t *s);                              /* the session clock (milliseconds) */
int cs_local_character(cs_t *s);
int cs_player_count(cs_t *s);
int cs_player(cs_t *s, int pid, cs_player_info *out);  /* 1 = pid used */
int cs_remote_pid(cs_t *s);                            /* the first other connected player or -1 (two-player convenience) */
void cs_session_get(cs_t *s, cp_session *out);
void cs_get_stats(cs_t *s, cs_stats *out);
cn_t *cs_transport(cs_t *s);
void cs_status(cs_t *s, char *out, size_t size);

/* generic console commands (host, join, status, char, ready, start, stop, session, event, say); returns 0 when the command is not ours */
int cs_command(cs_t *s, const char *line, char *out, size_t size);

#endif
