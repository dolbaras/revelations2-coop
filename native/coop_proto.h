/*
 * Wire protocol of the Revelations 2 co-op mod (see docs/protocol.md). Pure C, no Windows or game dependencies: the same
 * file is compiled into the dinput8 proxy, the headless bot and the unit tests.
 *
 * All integers are little endian, floats IEEE-754 binary32, strings are u8 length + bytes (UTF-8, at most 255).
 * Every packet starts with a one byte message type. A packet is one transport message (no framing of our own).
 * Forward rule: a decoder takes the fields it knows and ignores trailing bytes, so a newer sender may append fields to an
 * existing message without a version bump; changing or removing a field needs CP_PROTO_VERSION + 1.
 */
#ifndef COOP_PROTO_H
#define COOP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define CP_PROTO_VERSION 2u   /* layout of the messages below (2: STATE grew from 40 to 60 bytes, stage 5) */
#define CP_PROTO_MIN 2u       /* oldest version this build still talks (a version 1 STATE has another size: refused by the version gate) */
#define CP_MAGIC 0x50433252u  /* bytes 'R' '2' 'C' 'P' */
#define CP_MAX_PLAYERS 4
#define CP_NAME_MAX 24
#define CP_TEXT_MAX 120
#define CP_MOD_MAX 15
#define CP_EVENT_MAX 96
#define CP_WORLD_MAX 400       /* body of a WORLD message (stage 6: enemy / object records, unreliable) */
#define CP_MAX_PACKET 512     /* every message of this protocol fits (state 60, lobby ~ 4 * 32) */

/* transport channels: 0 = reliable + ordered, 1 = unreliable + sequenced (newest wins) */
enum { CP_CH_CONTROL = 0, CP_CH_STATE = 1, CP_CHANNELS = 2 };

enum cp_type {
    CP_HELLO = 0x01,        /* client -> host, first message after connect */
    CP_WELCOME = 0x02,      /* host -> client: accepted, your player id, session and lobby */
    CP_REFUSE = 0x03,       /* host -> client: not accepted (code + text), the host closes the connection afterwards */
    CP_SESSION = 0x04,      /* host -> all: session settings changed */
    CP_LOBBY_PICK = 0x05,   /* client -> host: I want this character */
    CP_LOBBY_READY = 0x06,  /* client -> host: ready flag */
    CP_LOBBY_STATE = 0x07,  /* host -> all: the whole lobby table (revision counted) */
    CP_START = 0x08,        /* host -> all: begin the shared game (session + final lobby) */
    CP_NOTICE = 0x09,       /* host -> one client: a request was not honoured (code + text) */
    CP_LOAD_BEGIN = 0x10,   /* any -> all (via host): I begin loading level instance <epoch> of area <id> */
    CP_LOAD_DONE = 0x11,    /* any -> host: loaded and ready */
    CP_GO = 0x12,           /* host -> all: everybody finished loading <epoch>, run */
    CP_EVENT = 0x20,        /* any -> all (via host): reliable game event (fire, hit, death, pickup, door ...) */
    CP_HEARTBEAT = 0x30,    /* any -> any, unreliable: liveness, RTT and clock offset */
    CP_LEAVE = 0x3F,        /* any -> any: I am leaving (reason) */
    CP_STATE = 0x50,        /* any -> all (via host), unreliable, 20 Hz: the state of my character */
    CP_WORLD = 0x51         /* any -> all (via host), unreliable, sequenced: batches of world records (enemies, objects), stage 6 */
};

enum cp_character { CP_CHAR_CLAIRE = 0, CP_CHAR_MOIRA = 1, CP_CHAR_BARRY = 2, CP_CHAR_NATALIA = 3, CP_CHAR_NONE = 255 };
#define CP_CHAR_SLOT(c) ((c) & 1)   /* the player slot the character occupies: Claire/Barry = 0, Moira/Natalia = 1 */

enum cp_refuse_code {
    CP_REF_VERSION = 1,     /* protocol versions do not overlap */
    CP_REF_BUILD = 2,       /* another game build / exe */
    CP_REF_FULL = 3,
    CP_REF_STARTED = 4,     /* the game already started and this is not a known player */
    CP_REF_BAD = 5,         /* malformed or unexpected first message */
    CP_REF_MODVER = 6,      /* another release version of the co-op mod (0.2.0 on: the host compares HELLO.mod with its own, bots excepted) */
    CP_REF_REMOVED = 7      /* 0.2.0: the host removed this player from the session (kick) and refuses its name until the session ends */
};
/* LEAVE.reason (0.2.0; older peers send 0 and ignore it): why a player or the host ends the link */
enum cp_leave_reason {
    CP_LEAVE_QUIT = 0,      /* the player quit (menu Leave, back at the title, window closed) */
    CP_LEAVE_REMOVED = 2,   /* host -> one client: removed from the session (Remove partner in the room) */
    CP_LEAVE_END_COOP = 3,  /* host -> one client: the host ended co-op play in the pause menu and plays on alone */
    CP_LEAVE_EP_END = 4     /* any: this PC reached the end of the co-op episode (0.2.0: Episode 1); the session is over for both */
};
enum cp_notice_code { CP_NOTE_SLOT_TAKEN = 1, CP_NOTE_NOT_READY = 2, CP_NOTE_REJECTED = 3, CP_NOTE_INFO = 4 };

/* cp_session.flags (host settings every player sees): bit 0 = the downed / revive rule is on, bits 1..2 = bleed-out preset (0 60 s, 1 30 s, 2 120 s, 3 never); see docs/downed-revive.md */
#define CP_SESS_DOWNED 1u
/* 0.2.1 coopsave: the host continues the checkpoint of its co-op save; the guest follows the host's checkpoint record (events 0x180, native/coopsave_logic.h) */
#define CP_SESS_CONT 8u

/* CP_PF_SAVE0 / CP_PF_SAVE1 (0.2.1 coopsave): this player's co-op save holds an Episode 1 checkpoint of Claire's / Barry's half (sent in HELLO, kept in the lobby table) */
enum cp_player_flags { CP_PF_HOST = 1, CP_PF_CONNECTED = 2, CP_PF_INGAME = 4, CP_PF_LOADED = 8, CP_PF_BOT = 16, CP_PF_SAVE0 = 32, CP_PF_SAVE1 = 64 };

/* PLAYER_STATE flags */
enum cp_state_flags {
    CP_SF_AIMING = 1, CP_SF_FIRING = 2, CP_SF_RELOADING = 4, CP_SF_CROUCH = 8, CP_SF_MELEE = 16, CP_SF_FLASHLIGHT = 32, CP_SF_DEAD = 64,
    CP_SF_DOWN = 128, CP_SF_LOADING = 256, CP_SF_CUTSCENE = 512, CP_SF_TELEPORT = 1024 /* do not interpolate to this sample */, CP_SF_PAUSED = 2048 /* the sender's pause menu is open */,
    CP_SF_HELD = 4096 /* the sender's human is held by an enemy (action INT_MIN, the hold camera, no scene running): not a cutscene, the body keeps being applied (stage 6e) */
};

/* event kinds (CP_EVENT); only the envelope is implemented, bodies are documented in docs/protocol.md */
enum cp_event_kind {
    CP_EV_NONE = 0, CP_EV_FIRE = 1, CP_EV_HIT = 2, CP_EV_DEATH = 3, CP_EV_PICKUP = 4, CP_EV_DOOR = 5, CP_EV_INTERACT = 6, CP_EV_ITEMUSE = 7,
    CP_EV_CHECKPOINT = 8, CP_EV_CUTSCENE = 9, CP_EV_STAT = 10, CP_EV_TEXT = 11, CP_EV_USER = 0x100
};

enum cp_phase { CP_PHASE_NONE = 0, CP_PHASE_LOBBY = 1, CP_PHASE_LOADING = 2, CP_PHASE_PLAYING = 3 };

typedef struct {
    uint32_t id;            /* session id chosen by the host */
    uint8_t game_type;      /* 0 story campaign (more later) */
    uint8_t episode;        /* 1..4 */
    uint8_t part;           /* 0 Claire/Moira half, 1 Barry/Natalia half */
    uint8_t difficulty;
    uint8_t max_players;    /* 2 for now */
    uint8_t flags;
    uint32_t area_id;       /* game area the host starts in (informational) */
    uint32_t seed;
} cp_session;

typedef struct {
    uint8_t pid;
    uint8_t character;      /* enum cp_character or CP_CHAR_NONE */
    uint8_t ready;
    uint8_t flags;          /* enum cp_player_flags */
    char name[CP_NAME_MAX + 1];
} cp_player;

typedef struct {
    uint32_t rev;           /* incremented by the host at every change */
    uint8_t count;
    cp_player p[CP_MAX_PLAYERS];
} cp_lobby;

typedef struct {
    uint16_t version, min_version;
    uint8_t exe_tag[8];     /* first 8 bytes of the game exe's SHA-256, zeros = unknown (bot) */
    uint8_t want_char;
    uint8_t flags;          /* CP_PF_BOT */
    char name[CP_NAME_MAX + 1];
    char mod[CP_MOD_MAX + 1];
} cp_hello;

typedef struct {
    uint8_t pid;
    uint16_t seq;           /* per sender, wraps */
    uint32_t t_ms;          /* sender's monotonic milliseconds */
    float pos[3];
    float yaw;              /* radians, -pi..pi */
    uint32_t action;        /* the game's action id (player +0x1A54); 0x80000000 = waiting is sent as 0xFFFF */
    int16_t hp, hpmax;
    float aim;              /* degrees (player +0x7950) */
    float aimv[3];          /* aim vector (player +0x7960), roughly unit length */
    uint8_t weapon;         /* which weapon slot is in the hand: 0 none, 1..3 weapon slots, 4 special (FUN_006fb060) */
    uint8_t epoch;          /* level instance counter, see the load barrier */
    uint16_t flags;         /* enum cp_state_flags */
    /* version 2 */
    uint32_t pstate;        /* the game's own player state flags (+0x37B8): replayed on the puppet */
    uint32_t ctrl;          /* the game's player control state flags (+0x78E8: bit 2 = aiming, ...) */
    uint16_t item[3];       /* item ids in the weapon slots 1..3 (0 = empty): inventory entry +4 of the entry FUN_006fb270(slot) names */
    uint8_t move;           /* locomotion intensity 0..255 (+0x792C: 0 idle, 255 full walk/run blend) */
    int16_t elev;           /* aim elevation, degrees * 100 (+0x7954) */
} cp_state;

typedef struct {
    uint8_t pid;
    uint16_t kind;          /* enum cp_event_kind or CP_EV_USER + n */
    uint16_t eseq;          /* per sender event counter */
    uint32_t t_ms;
    uint8_t epoch;
    uint8_t len;
    uint8_t body[CP_EVENT_MAX];
} cp_event;

typedef struct {
    uint8_t pid;
    uint8_t kind;           /* enum cp_world_kind */
    uint16_t seq;           /* per sender and kind-independent counter; a receiver keeps only newer ones */
    uint32_t t_ms;          /* sender's clock */
    uint8_t epoch;          /* level instance, a receiver drops other epochs */
    uint16_t len;
    uint8_t body[CP_WORLD_MAX];
} cp_world;

typedef struct {
    uint8_t pid;
    uint32_t t_ms;          /* sender's clock now */
    uint32_t echo_t_ms;     /* the t_ms of the last heartbeat received from the peer (0 = none yet) */
    uint32_t echo_dwell_ms; /* how long ago that one arrived */
    uint8_t phase;          /* enum cp_phase */
    uint8_t epoch;
    uint16_t state_seq;     /* last state sequence sent */
} cp_heartbeat;

typedef struct {
    uint8_t type;           /* enum cp_type */
    union {
        cp_hello hello;
        /* WELCOME / REFUSE end with an optional tail: `mod` = the host's mod version (0.2.0 on; an older host sends none: decoded as ""), flags CP_PF_BOT = the host is a bot */
        struct { uint16_t version; uint8_t pid, flags; uint32_t host_ms; uint8_t phase, epoch; cp_session session; cp_lobby lobby; char mod[CP_MOD_MAX + 1]; } welcome;
        struct { uint8_t code; char text[CP_TEXT_MAX + 1]; char mod[CP_MOD_MAX + 1]; } refuse;   /* REFUSE and NOTICE (`mod`: REFUSE only) */
        cp_session session;
        struct { uint8_t character; } pick;
        struct { uint8_t ready; } ready;
        cp_lobby lobby;
        struct { cp_session session; cp_lobby lobby; } start;
        struct { uint8_t pid, epoch; uint32_t area_id; } load;         /* LOAD_BEGIN (pid = who), LOAD_DONE / GO use epoch (+ pid) */
        cp_event event;
        cp_world world;
        cp_heartbeat hb;
        struct { uint8_t reason; } leave;
        cp_state state;
    } u;
} cp_msg;

/* Encode: returns the packet length, 0 when it does not fit or the type is unknown. Decode: 1 = ok, 0 = malformed (short, bad magic, bad count). */
size_t cp_encode(const cp_msg *m, uint8_t *buf, size_t cap);
int cp_decode(const uint8_t *buf, size_t len, cp_msg *m);

const char *cp_type_name(unsigned type);
const char *cp_char_name(unsigned c);
int cp_char_parse(const char *s);           /* claire|moira|barry|natalia or 0..3, CP_CHAR_NONE when unknown */
const char *cp_refuse_text(unsigned code);

/* Sequence numbers modulo 2^16 / 2^8 / 2^32 (newer-than tests that survive wrap-around). */
static inline int cp_seq16_newer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }
static inline int cp_seq8_newer(uint8_t a, uint8_t b) { return (int8_t)(uint8_t)(a - b) > 0; }

#endif
