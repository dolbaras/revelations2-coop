/* coopsave_logic.h: the pure part of the co-op save (0.2.1, native/coopsave.c), so tests/test_coopsave.c can check it without a game.
 *
 * How Rev2 keeps its campaign save (Ghidra, notes\coopsave.md section 1): only through ISteamRemoteStorage of the game's Steam context (FileExists, GetFileSize,
 * FileRead, FileWrite, FileDelete on "DATA0.DAT"); Steam stores the file in userdata\<account>\287290\remote\ and Steam Cloud carries it. The co-op save:
 *   - ROUTING: while a co-op game runs, every call of those five on the save name is served by the mod from its own file (outside Steam's folders), every
 *     other call passes through; with co-op off everything passes through. So the solo DATA0.DAT is never opened by a co-op game and Steam never sees the
 *     co-op save (cps_route below).
 *   - RECORD: the checkpoint summary the lobby needs before a game is loaded, and what the host sends so that the guest follows the host's checkpoint (like the
 *     stock online path FUN_00976F60 / FUN_00976A20: mTempData + the flag block at cGameInfo+0x123740): built from the campaign slot at every co-op save write.
 * Slot layout (relative to a game data slot of 0x1A610 bytes = the "current" block cGameInfo+0x10F030 the stock Continue copies it to):
 *   +0x1C mTempData (0x84 bytes; +0x24 difficulty, +0x28 episode, +0x2C (cleared by the copy), +0x30 mStageNo, +0x34 mStartPosIdx, +0x38 mSceNo),
 *   +0x9DC4 EpisodeFlags (16 words), +0x14710 the flag block (0x59C4 bytes: 20 per-stage scenario flag slots {id, 16 words} and more). */
#ifndef COOPSAVE_LOGIC_H
#define COOPSAVE_LOGIC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CPS_SAVE_NAME "DATA0.DAT"
#define CPS_SLOT_BYTES 0x1A610u
#define CPS_T_OFF 0x1Cu
#define CPS_T_LEN 0x84u
#define CPS_EPF_OFF 0x9DC4u
#define CPS_EPF_LEN 0x40u
#define CPS_FL_OFF 0x14710u
#define CPS_FL_LEN 0x59C4u
#define CPS_WORDS ((CPS_FL_LEN + CPS_EPF_LEN) / 4u)   /* the flag block's words, then the 16 EpisodeFlags words */
/* mTempData fields (offsets inside the slot) */
#define CPS_S_DIFF 0x24u
#define CPS_S_EPISODE 0x28u
#define CPS_S_2C 0x2Cu
#define CPS_S_STAGE 0x30u
#define CPS_S_STARTPOS 0x34u
#define CPS_S_SCENE 0x38u

/* ---- routing: which calls the mod serves ---- */
static inline int cps_is_save_name(const char *name)
{
    if (!name) return 0;
    const char *a = name, *b = CPS_SAVE_NAME;
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}
/* 1 = the call goes to the co-op file, 0 = to Steam (the game's own save). co_op = a co-op game runs (cps state ON). CPS_MODEL_BEFORE: 0.2.0 = always Steam. */
static inline int cps_route(int co_op, const char *name)
{
#ifdef CPS_MODEL_BEFORE
    (void)co_op, (void)name;
    return 0;
#else
    return co_op && cps_is_save_name(name);
#endif
}


/* ---- the record ---- */
#define CPS_REC_MAGIC 0x53433252u   /* 'R' '2' 'C' 'S' */
#define CPS_REC_VERSION 1u
typedef struct {
    uint32_t magic, version;
    uint8_t temp[CPS_T_LEN];
    uint8_t fl[CPS_FL_LEN];
    uint8_t epf[CPS_EPF_LEN];
    uint32_t crc;            /* crc32 of temp, fl, epf */
} cps_rec;

static inline uint32_t cps_crc32(uint32_t crc, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
/* the co-op files are named after a hash of the Steam account (rev2_coop_save_<hash>.dat / .rec in the game's config folder): two accounts of one Windows
 * user keep two co-op saves, and the account number itself is not in a file name */
static inline uint32_t cps_account_hash(uint32_t account) { return cps_crc32(0x52324353u, &account, 4); }
static inline uint32_t cps_rec_crc(const cps_rec *r)
{
    uint32_t c = cps_crc32(0, r->temp, sizeof r->temp);
    c = cps_crc32(c, r->fl, sizeof r->fl);
    return cps_crc32(c, r->epf, sizeof r->epf);
}
static inline uint32_t cps_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void cps_wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
/* a field of mTempData by its slot offset (CPS_S_*) */
static inline uint32_t cps_rec_field(const cps_rec *r, unsigned slot_off) { return cps_rd32(r->temp + (slot_off - CPS_T_OFF)); }
static inline void cps_rec_set_field(cps_rec *r, unsigned slot_off, uint32_t v) { cps_wr32(r->temp + (slot_off - CPS_T_OFF), v); }

static inline void cps_rec_from_slot(cps_rec *r, const uint8_t *slot)
{
    r->magic = CPS_REC_MAGIC, r->version = CPS_REC_VERSION;
    memcpy(r->temp, slot + CPS_T_OFF, CPS_T_LEN);
    memcpy(r->fl, slot + CPS_FL_OFF, CPS_FL_LEN);
    memcpy(r->epf, slot + CPS_EPF_OFF, CPS_EPF_LEN);
    r->crc = cps_rec_crc(r);
}
static inline int cps_rec_valid(const cps_rec *r) { return r->magic == CPS_REC_MAGIC && r->version == CPS_REC_VERSION && r->crc == cps_rec_crc(r); }
/* the half of a stage (Story_GetPartOfArea 0x984740: -1 below 100, else stage % 100 > 49) and its episode (stage / 100) */
static inline int cps_stage_part(int stage) { return stage < 100 ? -1 : (stage % 100 > 49); }
static inline int cps_stage_episode(int stage) { return stage < 100 ? 0 : stage / 100; }
/* the checkpoint a record holds is one the co-op lobby can continue: Episode 1 (the 0.2.x rule), a story stage of that episode */
static inline int cps_rec_continuable(const cps_rec *r, int *part, int *difficulty, int *stage, int *startpos)
{
    if (!cps_rec_valid(r)) return 0;
    int st = (int)cps_rec_field(r, CPS_S_STAGE), ep = (int)cps_rec_field(r, CPS_S_EPISODE), d = (int)cps_rec_field(r, CPS_S_DIFF);
    if (ep != 1 || cps_stage_episode(st) != 1 || d < 0 || d > 3) return 0;
    if (part) *part = cps_stage_part(st);
    if (difficulty) *difficulty = d;
    if (stage) *stage = st;
    if (startpos) *startpos = (int)cps_rec_field(r, CPS_S_STARTPOS);
    return 1;
}
/* the guest follows the host's checkpoint: what the stock online apply FUN_00976A20 (mode 0) writes, plus the difficulty and the EpisodeFlags */
static inline void cps_rec_apply_slot(const cps_rec *r, uint8_t *slot)
{
    static const unsigned f[] = {CPS_S_DIFF, CPS_S_EPISODE, CPS_S_2C, CPS_S_STAGE, CPS_S_STARTPOS, CPS_S_SCENE};
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) memcpy(slot + f[i], r->temp + (f[i] - CPS_T_OFF), 4);
    memcpy(slot + CPS_FL_OFF, r->fl, CPS_FL_LEN);
    memcpy(slot + CPS_EPF_OFF, r->epf, CPS_EPF_LEN);
}

/* ---- the record on the wire (session events kind CPS_EV, reliable + ordered, sent by the host just before START) ----
 * type 0 HEAD  {u8 0, u8 ver, u16 pairs, u32 crc (cps_wire_crc), u32 diff, u32 episode, u32 +0x2C, u32 stage, u32 startpos, u32 scene}  (32 bytes)
 * type 1 WORDS {u8 1, u8 n, u16 first pair number} + n x {u16 word index, u32 value}  (n <= 15: 94 bytes; only words that are not 0)
 * type 2 END   {u8 2, u8 ver, u16 pairs, u32 crc}
 * The receiver starts from zeros, so the words that are 0 need no event; crc = cps_wire_crc of the rebuilt record. */
#define CPS_EV_HEAD 0
#define CPS_EV_WORDS 1
#define CPS_EV_END 2
#define CPS_EV_PAIRS 15
static inline uint32_t cps_word(const cps_rec *r, unsigned i) { return i < CPS_FL_LEN / 4 ? cps_rd32(r->fl + 4 * i) : cps_rd32(r->epf + 4 * (i - CPS_FL_LEN / 4)); }
static inline void cps_set_word(cps_rec *r, unsigned i, uint32_t v)
{
    if (i < CPS_FL_LEN / 4) cps_wr32(r->fl + 4 * i, v);
    else if (i < CPS_WORDS) cps_wr32(r->epf + 4 * (i - CPS_FL_LEN / 4), v);
}
/* what the wire carries is checked as a whole: the six fields the guest applies, the flag block, the EpisodeFlags */
static inline uint32_t cps_wire_crc(const cps_rec *r)
{
    static const unsigned f[] = {CPS_S_DIFF, CPS_S_EPISODE, CPS_S_2C, CPS_S_STAGE, CPS_S_STARTPOS, CPS_S_SCENE};
    uint32_t c = 0;
    for (unsigned i = 0; i < 6; i++) c = cps_crc32(c, r->temp + (f[i] - CPS_T_OFF), 4);
    c = cps_crc32(c, r->fl, sizeof r->fl);
    return cps_crc32(c, r->epf, sizeof r->epf);
}
static inline unsigned cps_rec_pairs(const cps_rec *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CPS_WORDS; i++) n += cps_word(r, i) != 0;
    return n;
}
/* the events of a record, in order: calls emit(user, body, len) for each; returns the number of events (0 = an invalid record, nothing sent) */
typedef int (*cps_emit_fn)(void *user, const uint8_t *body, unsigned len);
static inline int cps_wire_send(const cps_rec *r, cps_emit_fn emit, void *user)
{
    if (!cps_rec_valid(r)) return 0;
    static const unsigned f[] = {CPS_S_DIFF, CPS_S_EPISODE, CPS_S_2C, CPS_S_STAGE, CPS_S_STARTPOS, CPS_S_SCENE};
    uint8_t b[96];
    unsigned pairs = cps_rec_pairs(r), ev = 0;
    memset(b, 0, sizeof b);
    uint32_t wcrc = cps_wire_crc(r);
    b[0] = CPS_EV_HEAD, b[1] = CPS_REC_VERSION, b[2] = (uint8_t)pairs, b[3] = (uint8_t)(pairs >> 8);
    cps_wr32(b + 4, wcrc);
    for (unsigned i = 0; i < 6; i++) cps_wr32(b + 8 + 4 * i, cps_rec_field(r, f[i]));
    if (!emit(user, b, 32)) return 0;
    ev++;
    unsigned done = 0, n = 0;
    uint8_t *p = b + 4;
    for (unsigned i = 0; i <= CPS_WORDS; i++) {
        uint32_t v = i < CPS_WORDS ? cps_word(r, i) : 0;
        if (i < CPS_WORDS && v) {
            if (n == 0) memset(b, 0, sizeof b), b[0] = CPS_EV_WORDS, b[2] = (uint8_t)done, b[3] = (uint8_t)(done >> 8), p = b + 4;
            p[0] = (uint8_t)i, p[1] = (uint8_t)(i >> 8), cps_wr32(p + 2, v), p += 6, n++;
        }
        if (n && (n == CPS_EV_PAIRS || i == CPS_WORDS)) {
            b[1] = (uint8_t)n;
            if (!emit(user, b, 4 + 6 * n)) return 0;
            done += n, n = 0, ev++;
        }
    }
    memset(b, 0, sizeof b);
    b[0] = CPS_EV_END, b[1] = CPS_REC_VERSION, b[2] = (uint8_t)pairs, b[3] = (uint8_t)(pairs >> 8);
    cps_wr32(b + 4, wcrc);
    if (!emit(user, b, 8)) return 0;
    return (int)ev + 1;
}
typedef struct {
    cps_rec rec;
    unsigned pairs_expected, pairs_got;
    uint32_t crc;
    int have_head, complete, bad;
} cps_rx;
static inline void cps_rx_reset(cps_rx *x) { memset(x, 0, sizeof *x); }
/* feed one event body; returns 1 when the record just became complete (crc checked) */
static inline int cps_rx_feed(cps_rx *x, const uint8_t *b, unsigned len)
{
    static const unsigned f[] = {CPS_S_DIFF, CPS_S_EPISODE, CPS_S_2C, CPS_S_STAGE, CPS_S_STARTPOS, CPS_S_SCENE};
    if (len < 4) return 0;
    if (b[0] == CPS_EV_HEAD) {
        if (len < 32 || b[1] != CPS_REC_VERSION) { x->bad++; return 0; }
        cps_rx_reset(x);
        x->have_head = 1, x->pairs_expected = (unsigned)b[2] | (unsigned)b[3] << 8, x->crc = cps_rd32(b + 4);
        x->rec.magic = CPS_REC_MAGIC, x->rec.version = CPS_REC_VERSION;
        for (unsigned i = 0; i < 6; i++) cps_rec_set_field(&x->rec, f[i], cps_rd32(b + 8 + 4 * i));
        return 0;
    }
    if (!x->have_head || x->complete) { x->bad++; return 0; }
    if (b[0] == CPS_EV_WORDS) {
        unsigned n = b[1], first = (unsigned)b[2] | (unsigned)b[3] << 8;
        if (len < 4 + 6 * n || first != x->pairs_got) { x->bad++; return 0; }
        for (unsigned k = 0; k < n; k++) {
            const uint8_t *p = b + 4 + 6 * k;
            unsigned i = (unsigned)p[0] | (unsigned)p[1] << 8;
            if (i >= CPS_WORDS) { x->bad++; return 0; }
            cps_set_word(&x->rec, i, cps_rd32(p + 2));
        }
        x->pairs_got += n;
        return 0;
    }
    if (b[0] == CPS_EV_END) {
        if (len < 8) { x->bad++; return 0; }
        unsigned pairs = (unsigned)b[2] | (unsigned)b[3] << 8;
        if (pairs != x->pairs_expected || x->pairs_got != pairs || cps_rd32(b + 4) != x->crc || cps_wire_crc(&x->rec) != x->crc) { x->bad++; return 0; }
        x->rec.crc = cps_rec_crc(&x->rec);   /* the received record is a valid record of its own (the bytes of mTempData it does not carry are 0) */
        x->complete = 1;
        return 1;
    }
    x->bad++;
    return 0;
}

/* ---- the session flag and the players' save flags (coop_proto.h CP_SESS_CONT, CP_PF_SAVE0 / CP_PF_SAVE1) ----
 * the flags a player advertises: its co-op save can be continued in that half */
static inline unsigned cps_save_pflags(const cps_rec *r, unsigned save0_bit, unsigned save1_bit)
{
    int part = -1;
    if (!r || !cps_rec_continuable(r, &part, NULL, NULL, NULL)) return 0;
    return part == 1 ? save1_bit : part == 0 ? save0_bit : 0;
}

/* ---- the co-op state machine (coopsave.c drives it from the game thread) ----
 * OFF -> (a co-op game starts: the save manager idle) ON: solo image kept, routing on, co-op save loaded -> (the game reached a level) ON + level_seen ->
 * (the title menus for 1.5 s, the save manager idle, a level was seen or the session is gone) OFF: solo image back, routing off. */
enum { CPS_OFF = 0, CPS_ON = 1 };
typedef struct {
    int state, level_seen, menu_frames;
    unsigned enter_tick;
} cps_sm;
/* one game-thread pass while ON: returns 1 when the co-op game is over and the solo save comes back now */
static inline int cps_leave_due(cps_sm *m, int in_level, int title_menus, int save_idle, int session_running, unsigned now)
{
    if (m->state != CPS_ON) return 0;
    if (in_level) m->level_seen = 1, m->menu_frames = 0;
    else if (title_menus && save_idle) m->menu_frames++;
    else m->menu_frames = 0;
    if (m->menu_frames < 90) return 0;
    /* a start that never reached a level (the partner's game never came, an error): give the solo save back once the session is gone or after 60 s */
    return m->level_seen || !session_running || now - m->enter_tick > 60000u;
}

#endif
