#ifndef PAD_LOGIC_H
#define PAD_LOGIC_H
/* pad_logic.h: the pure part of the real-pad handling (6n, pad audit R2-1 / R2-2; tests/test_pad.c replays it without a game).
 *
 * Activity (R2-2): the old test "a button is down or a stick is beyond 9000" was a LEVEL: a drifting stick or a stuck button looked like a human using the pad for ever, kept the input mode on
 * "pad" and blocked the keyboard. Activity is now an EVENT: a button newly pressed, a trigger crossing its threshold, a stick leaving its dead zone, or a stick beyond the dead zone moving by more
 * than PAD_MOVE between two samples. The first sample of a pad is only a baseline (a pad that is plugged in with a stuck button or a drifting stick is not a human).
 *
 * Pick (R2-1): which connected XInput user is "the human's pad". The old code took the first connected user and pinned it, so an idle or duplicate pad with a lower index (a virtual pad of a
 * remapping tool) shadowed the real one. Now the pad that was used last wins: the chosen one stays while it is used or nothing else is; when it has been idle for PAD_IDLE_MS and another pad
 * shows activity, the choice moves; a pad that vanishes is replaced by the connected one with the newest activity (else the lowest index). */
#include <string.h>

#define PAD_DEAD 9000
#define PAD_MOVE 3000
#define PAD_TRIG 40
#define PAD_IDLE_MS 3000u

typedef struct { unsigned short buttons; unsigned char lt, rt; short lx, ly, rx, ry; int have; } pad_act_t;

static inline int pad_abs(int v) { return v < 0 ? -v : v; }

/* returns 1 when this sample is an activity event; the sample becomes the new baseline */
static inline int pad_act_update(pad_act_t *a, unsigned short buttons, unsigned char lt, unsigned char rt, short lx, short ly, short rx, short ry)
{
    int ev = 0;
    if (a->have) {
        short now_[4] = {lx, ly, rx, ry}, was[4] = {a->lx, a->ly, a->rx, a->ry};
        if (buttons & (unsigned short)~a->buttons) ev = 1;
        if ((lt > PAD_TRIG && a->lt <= PAD_TRIG) || (rt > PAD_TRIG && a->rt <= PAD_TRIG)) ev = 1;
        for (int i = 0; i < 4; i++)
            if (pad_abs(now_[i]) > PAD_DEAD && (pad_abs(now_[i] - was[i]) > PAD_MOVE || pad_abs(was[i]) <= PAD_DEAD)) ev = 1;
    }
    a->buttons = buttons, a->lt = lt, a->rt = rt, a->lx = lx, a->ly = ly, a->rx = rx, a->ry = ry, a->have = 1;
    return ev;
}

typedef struct { int cur; int has_ev[4]; unsigned last_ev[4]; } pad_pick_t;

static inline void pad_pick_init(pad_pick_t *p)
{
    memset(p, 0, sizeof *p);
    p->cur = -1;
}

static inline int pad_newer(unsigned a, unsigned b) { return (int)(a - b) > 0; }

/* one call per scan: connected[u] = the user answers, ev[u] = activity event in this sample. Returns the chosen user or -1. */
static inline int pad_pick_update(pad_pick_t *p, unsigned now, const int connected[4], const int ev[4])
{
    for (int u = 0; u < 4; u++)
        if (ev[u]) p->has_ev[u] = 1, p->last_ev[u] = now;
    if (p->cur >= 0 && !connected[p->cur]) p->cur = -1;
    if (p->cur < 0) {
        int best = -1;
        for (int u = 0; u < 4; u++) {
            if (!connected[u]) continue;
            if (best < 0 || (p->has_ev[u] && (!p->has_ev[best] || pad_newer(p->last_ev[u], p->last_ev[best])))) best = u;
        }
        p->cur = best;
        return p->cur;
    }
    int cur_idle = !p->has_ev[p->cur] || (int)(now - p->last_ev[p->cur]) >= (int)PAD_IDLE_MS;
    if (cur_idle)
        for (int u = 0; u < 4; u++)
            if (u != p->cur && connected[u] && p->has_ev[u] && (int)(now - p->last_ev[u]) < (int)PAD_IDLE_MS &&
                (!p->has_ev[p->cur] || pad_newer(p->last_ev[u], p->last_ev[p->cur]))) {
                p->cur = u;
                break;
            }
    return p->cur;
}

/* R2-6 (flashlive, 2026-11-08, notes\flashlive.md section 1): THE SEATS AFTER A LOAD. A player reads the logical pad cPad +0xC (its cPadEx: player +0x78E0 -> +0x4C0),
 * and that number is the player's seat (+0x7924, from GameMan +0x7D4 [slot]) read when its uPlIntel goes human (FUN_00A14BE0, again after every slot-type change).
 * The game's spawn of a level (sPlayer_SpawnAll 0x6E6490) swaps the two slots' types AND seats when slot 0 is the AI partner (type 1) and slot 1 a human (type 0),
 * and the glue puts the partner to type 1 at every load begin, so on a PC whose human is slot 1 (the client's Moira / Natalia, a host who plays them) every load
 * swapped the seats: the human got seat 0, read logical pad 0 = game user 0 (a neutral keep-alive pad), while this PC's real pad goes to game user = the slot:
 * the gamepad did nothing after the first load (a stage change, a retry, a checkpoint), the keyboard still worked, the next load swapped back.
 * pad_seat_writes: a bit per slot whose seat must be written back to its own number (the stock join's arrangement: slot 0 seat 0, slot 1 seat 1, which the
 * real-pad routing and the achievement logic assume); pad_cpad_fix: the pad number to write into the local human's cPad when it reads another one, else -1.
 * -DPAD_MODEL_BEFORE_R26 = rev2-int 6b40a90 (nothing written; tests/build.sh checks that test_pad fails with it). */
static inline int pad_seat_writes(const int seat[2])
{
#ifdef PAD_MODEL_BEFORE_R26
    (void)seat;
    return 0;
#else
    int m = 0;
    for (int k = 0; k < 2; k++)
        if (seat[k] != k) m |= 1 << k;
    return m;
#endif
}

static inline int pad_cpad_fix(int my_slot, int cpad_no)
{
#ifdef PAD_MODEL_BEFORE_R26
    (void)my_slot, (void)cpad_no;
    return -1;
#else
    if (my_slot < 0 || my_slot > 1) return -1;
    return cpad_no == my_slot ? -1 : my_slot;
#endif
}
#endif
