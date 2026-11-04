/* glue_logic.h: the pure decisions of the co-op glue (coop_game.inc, world.inc) that must not depend on a running game, so tests	est_glue.c can replay them.
 * Stage 6f (2026-11-04), found by the retry hang of the pair run s6e2.
 *
 * The retry hang: after a retry the game plays the opening scene of Claire's half again ("WakeUp"); on the client the human stood in the scripted action INT_MIN for 8 minutes, the loading icon
 * stayed up, SF[1] never rose, and 48 s later the "coop flag is off for 8 s" watchdog sent the glue back into `setup`. THE CAUSE (proved in the first 6f run: `net w guard 0` on the client let the scene
 * finish in 12 s): the client refuses every enemy creation its own story asks for, and the scene's script WAITS for the enemy (100,116,0) and asks again every frame (glue_demand_check below).
 * My first guess, a type-2 puppet during the scene, was wrong (the scene hung with the partner as an AI partner too); the scene gate (glue_gate_*, `net gate 1`, REV2_GATE=1) stays as an option, off by default.
 * Rules that are right anyway: the watchdog and the role set-up never act while the local human is in a scripted action (watchdog) or a scene plays (join). */
#ifndef GLUE_LOGIC_H
#define GLUE_LOGIC_H

#define GLUE_SCRIPTED_ACT 0x80000000u   /* player action id while a scene / hold owns the body (the "INT_MIN" of the logs) */
#define GLUE_GATE_SETTLE_MS 8000        /* after the barrier opened: no puppet before this much time (scenes start a few seconds after a load) */
#define GLUE_GATE_CONTROL_MS 2500       /* ... and the local human has to be under control for this long without a break */
#define GLUE_GATE_MAX_MS 900000         /* never wait longer than 15 minutes: open anyway (a story part where the human never gets control) */
#define GLUE_COOP_LOST_MS 8000          /* the game's coop flag has to stay off this long (human in control) before the roles are set up again */

typedef struct {
    unsigned act;        /* player+0x1A54 of the local human */
    int hp;              /* player+0x1A08 */
    unsigned view_mode;  /* gm+0x8F4: 2 / 3 are the engine's cutscene view modes */
    int demo_running;    /* sDemo reports a running demo (state 1 / 2) */
} glue_local_t;

/* The local human owns its body: no scene, no hold, no cutscene view. A dead human counts as controlled (the death screen waits for the player, not for a script). */
static inline int glue_in_control(const glue_local_t *l)
{
    if (l->hp <= 0) return 1;
    if (l->view_mode == 2 || l->view_mode == 3) return 0;
    if (l->act == GLUE_SCRIPTED_ACT) return 0;
    if (l->demo_running) return 0;
    return 1;
}

/* May the remote slot become the type-2 puppet? since_ms = time since the barrier opened, control_ms = how long the local human has been in control without a break. */
static inline int glue_gate_open(unsigned since_ms, unsigned control_ms)
{
    if (since_ms >= GLUE_GATE_MAX_MS) return 1;
    return since_ms >= GLUE_GATE_SETTLE_MS && control_ms >= GLUE_GATE_CONTROL_MS;
}

/* The gate tracker: call glue_gate_init at the barrier and glue_gate_update every pass; it returns 1 the moment the gate opens. *since_out / *held_out are for the log. */
typedef struct {
    unsigned start;       /* tick of the barrier */
    unsigned ctrl_since;  /* tick since the local human has been in control without a break (0 = not in control) */
} glue_gate_t;

static inline void glue_gate_init(glue_gate_t *g, unsigned now)
{
    g->start = now, g->ctrl_since = 0;
}

static inline int glue_gate_update(glue_gate_t *g, unsigned now, const glue_local_t *l, unsigned *since_out, unsigned *held_out)
{
    if (!glue_in_control(l)) g->ctrl_since = 0;
    else if (!g->ctrl_since) g->ctrl_since = now ? now : 1;
    unsigned since = now - g->start, held = g->ctrl_since ? now - g->ctrl_since : 0;
    if (since_out) *since_out = since;
    if (held_out) *held_out = held;
    return glue_gate_open(since, held);
}

/* Does the "coop flag is off" watchdog start / keep counting? (the caller resets its timer when this returns 0) */
static inline int glue_coop_lost_counts(int coop_flag, unsigned view_mode, int in_episode, int demo_recent, int gated, const glue_local_t *l)
{
    if (coop_flag == 1 || !in_episode) return 0;
    if (view_mode == 2 || view_mode == 3) return 0;
    if (demo_recent || gated) return 0;
    if (!glue_in_control(l)) return 0;
    return 1;
}

/* A scene plays: the engine's cutscene view or an sDemo request. (A human in the scripted action INT_MIN alone is NOT a scene for this purpose: the AI partner of Claire's half, Moira, stands
 * in INT_MIN from the start, before the join turns her into a human; found in the first run of 6f, where the guard on that blocked the host's join for 2 minutes.) */
static inline int glue_scene_running(const glue_local_t *l)
{
    return l->view_mode == 2 || l->view_mode == 3 || l->demo_running;
}

/* May the role set-up ask the game for a second human now (`join 1`)? Not while a scene plays: the join request reaches the aGame state machine in the middle of it. */
static inline int glue_may_join(const glue_local_t *l) { return !glue_scene_running(l); }


/* Stage 6f, the REAL cause of the retry hang: a client refuses every enemy creation its own story asks for (the host's SPAWN is supposed to create it). The opening scene of Claire's half
 * asks for the enemy (100,116,0) a second after it starts and the story script WAITS for it, asking again every frame (50 Hz); the host's story never reached that node, so no SPAWN
 * came, the scene never ended, the human stood in the scripted action for ever and the loading icon stayed up (proved: `net w guard 0` on the client let the scene finish in 12 s).
 * A story that asks for the same layout again and again within a second is waiting for it: from the GLUE_DEMAND_N-th request on the client creates it itself (a placed enemy is found by
 * its layout id when the host's records / SPAWN arrive, so nothing is duplicated). */
#define GLUE_DEMAND_N 8
#define GLUE_DEMAND_WINDOW_MS 1000
#define GLUE_DEMAND_SLOTS 8
typedef struct {
    int a;
    unsigned bc, first_ms, n, allowed;
} glue_demand_t;

/* returns 1 when this request must be allowed (the story is waiting for it) */
static inline int glue_demand_check(glue_demand_t *tab, int a, unsigned bc, unsigned now)
{
    int idx = -1;
    for (int i = 0; i < GLUE_DEMAND_SLOTS; i++)
        if (tab[i].n && tab[i].a == a && tab[i].bc == bc) {
            idx = i;
            break;
        }
    if (idx < 0) {   /* a free slot, else the entry that was started longest ago */
        int pick = 0;
        for (int i = 0; i < GLUE_DEMAND_SLOTS; i++) {
            if (!tab[i].n) {
                pick = i;
                break;
            }
            if ((unsigned)(now - tab[i].first_ms) > (unsigned)(now - tab[pick].first_ms)) pick = i;
        }
        idx = pick;
        tab[idx].a = a, tab[idx].bc = bc, tab[idx].first_ms = now, tab[idx].n = 0, tab[idx].allowed = 0;
    }
    glue_demand_t *d = &tab[idx];
    if ((unsigned)(now - d->first_ms) > GLUE_DEMAND_WINDOW_MS) d->first_ms = now, d->n = 0;
    d->n++;
    if (d->n >= GLUE_DEMAND_N) {
        d->allowed++;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * Stage 6g, task b: the engine's OWN game-over state instead of guesses from hp / action id.
 *
 * Static finding (Ghidra, 2026-11-04): aGame (vtable of sArea_GetGameArea) runs a state byte at aGame+4; its jump table (0x42B1C4) sends state 0x0B to FUN_004330E0, the game-over state: sub state 5 creates
 * the death screen (uGUIDead, the "You died" screen). The state is entered through FUN_00428670 (called from the main gameplay state 3, Coop_UpdateMain 0x434320, when FUN_00426E00 says so).
 * FUN_00426E00 is the engine's per-pass check: for every active human slot that is not yet in the dead mask sGameMan+0x878 it asks
 *      hp < 1  ||  player+0x78E8 (ctrl) bit 1  ||  player+0x37B8 (pstate) bit 14 (0x4000)  ||  player+0x7904 bit 28
 * and then marks the slot (FUN_00885F30(slot, 10.0f): mask bit + timer at sGameMan+0x87C+4*slot) and enters the game-over state. The shapes seen in 6f all fit: a fall into the void leaves hp > 0 with ctrl
 * 00001002 (bit 1); a held human falling leaves hp 96, act INT_MIN, pstate 4000 (bit 14) - neither has hp 0, and the old rule (hp <= 0 or act 118 with pstate 80000000) missed the second one.
 * A death is therefore: the engine's mask bit for the slot, OR the game-over state itself, OR the engine's predicate (the first frame, before the mask is written). The old shapes stay as extra evidence. */
#define GLUE_PSTATE_DEAD 0x4000u         /* player+0x37B8 bit 14 */
#define GLUE_CTRL_DEAD 2u                /* player+0x78E8 bit 1 */
#define GLUE_F7904_DEAD 0x10000000u      /* player+0x7904 bit 28 */
#define GLUE_AGAME_GAMEOVER 0x0Bu        /* aGame+4 */
#define GLUE_AGAME_PLAY 0x03u            /* aGame+4: main gameplay (Coop_UpdateMain); 9 = scenes (FUN_00434E10), 0x0B = game over, 0x0C = leaving after it */

typedef struct {
    int hp;                 /* player+0x1A08 (full 32 bit, signed) */
    unsigned action;        /* player+0x1A54 */
    unsigned pstate;        /* player+0x37B8 */
    unsigned ctrl;          /* player+0x78E8 */
    unsigned f7904;         /* player+0x7904 */
    int mask_bit;           /* (sGameMan+0x878 >> slot) & 1 */
    unsigned agame_state;   /* aGame+4 (low byte) */
} glue_dead_t;

enum { GLUE_DR_HP = 1, GLUE_DR_OLD_VOID = 2, GLUE_DR_ENGINE_PRED = 4, GLUE_DR_MASK = 8, GLUE_DR_SCREEN = 16 };

static inline unsigned glue_dead_reasons(const glue_dead_t *d)
{
    unsigned r = 0;
    if (d->hp < 1) r |= GLUE_DR_HP;
    if (d->action == 118 && d->pstate == 0x80000000u) r |= GLUE_DR_OLD_VOID;
    /* the engine evaluates the predicate only in its main gameplay state (aGame state 3); scenes run in state 9 and may show other words in those fields */
    if ((d->agame_state & 0xFF) == GLUE_AGAME_PLAY && ((d->ctrl & GLUE_CTRL_DEAD) || (d->pstate & GLUE_PSTATE_DEAD) || (d->f7904 & GLUE_F7904_DEAD))) r |= GLUE_DR_ENGINE_PRED;
    if (d->mask_bit) r |= GLUE_DR_MASK;
    if ((d->agame_state & 0xFF) == GLUE_AGAME_GAMEOVER) r |= GLUE_DR_SCREEN;
    return r;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * Stage 6g, task a: a human that an enemy holds must not stay held for ever.
 *
 * The enemy's grab action (cEm1100ActGrap*, Afflicted) and the human's own cEm1100ActGrapPL (0x5985C0) are a pair: the human's action keeps the hold while the enemy's action id (enemy+0x1A54)
 * equals the id that started the grab (0x4D..0x54) or is one of 0x4F, 0x50, 0x53, 0x54; the moment the enemy leaves that set the human's action releases itself (ChangeAction 0xB9, "return 4").
 * So a hold that lasts after the holder left the chain is a wedge: 6f3 saw the host's enemy stuck in action 77 for 3 minutes (also dead, hp 0) and the victim held on both PCs.
 * Two causes are covered here: (1) a REPLICA that is told 77 again every time its own chain moved on (77 <-> 79 cycling, the client's human held 9.9 s against 5.1 s on the host): glue_replica_force_ok keeps
 * the replica from restarting a chain it is inside of; (2) any wedge: the victim's own PC frees its human when the holder is gone / dead / out of the chain for GLUE_HOLD_ORPHAN_MS, and in any case
 * after GLUE_HOLD_MAX_MS (a stock hold ends by itself after about 5 s). */
#define GLUE_HOLD_FLAG 0x2000u          /* player+0x7900 bit 13: up on a human an enemy holds, never in a scene (6f2 traces) */
#define GLUE_HOLD_ORPHAN_MS 1500        /* the holder is known and dead / out of the chain */
#define GLUE_HOLD_LOST_MS 4000          /* the holder cannot be found at all (erased after its death, or a class whose catch id is not the Afflicted's) */
#define GLUE_HOLD_MAX_MS 15000
#define GLUE_HOLD_MAX_OTHER_MS 90000    /* holders of classes other than the Afflicted (bosses, scripted holds) */
#define GLUE_HOLD_RETRY_MS 2000         /* between two release attempts on the same hold */

static inline int glue_is_held(unsigned action, unsigned f7900) { return action == GLUE_SCRIPTED_ACT && (f7900 & GLUE_HOLD_FLAG) != 0; }

/* the action ids of the Afflicted's grab chain (cEm1100ActGrap, start ids 0x4D..0x54, the chain's follow-ups up to 0x58) */
static inline int glue_grab_family(unsigned act) { return act >= 0x4D && act <= 0x58; }

/* May a host record's action be forced on a replica? Not when the replica is already inside the grab chain (its own action walks it): re-forcing 77 every time the replica's chain moved to 79 restarted the
 * grab over and over (6f3). Leaving the chain (the host's action is outside it) is always forced. */
static inline int glue_replica_force_ok(unsigned host_act, unsigned replica_act)
{
    if (host_act == replica_act) return 0;
    if (glue_grab_family(host_act) && glue_grab_family(replica_act)) return 0;
    return 1;
}

typedef struct {
    int held;           /* glue_is_held() of the local human */
    int holder_found;   /* an enemy whose catch id (+0x73E8) equals the victim's (+0x3788) */
    int holder_hp;      /* its hp */
    int holder_in_chain;/* its action is in the chain the victim's action waits for (1 when the class is not known) */
    int holder_seen;    /* a holder was found at some time during THIS hold (without it the hold is a scene / script, not an enemy: 6g hardware, the opening scene of a retry has act INT_MIN + flags 0x2000 and no holder at all) */
    unsigned max_ms;    /* the cap for this holder (0 = GLUE_HOLD_MAX_MS; longer for classes whose scripted holds are not known: a boss may hold for a scene) */
} glue_hold_in_t;

typedef struct {
    unsigned held_since;    /* tick the hold began (0 = not held) */
    unsigned orphan_since;  /* tick since the holder is not a living enemy in the chain (0 = it is) */
    unsigned last_release;  /* tick of the last release attempt */
    unsigned releases;
} glue_hold_t;

enum { GLUE_HOLD_NONE = 0, GLUE_HOLD_REL_ORPHAN = 1, GLUE_HOLD_REL_MAX = 2 };

/* every pass; returns GLUE_HOLD_REL_* when the victim's action must be ended now */
static inline int glue_hold_step(glue_hold_t *h, unsigned now, const glue_hold_in_t *in)
{
    if (!in->held) {
        h->held_since = h->orphan_since = h->last_release = 0;
        return GLUE_HOLD_NONE;
    }
    if (!h->held_since) h->held_since = now ? now : 1;
    int ok = !in->holder_seen || (in->holder_found && in->holder_hp > 0 && in->holder_in_chain);   /* never saw a holder: not an enemy hold, only the long cap applies */
    if (ok) h->orphan_since = 0;
    else if (!h->orphan_since) h->orphan_since = now ? now : 1;
    int retry_ok = !h->last_release || (unsigned)(now - h->last_release) >= GLUE_HOLD_RETRY_MS;
    if (!retry_ok) return GLUE_HOLD_NONE;
    if (h->orphan_since && (unsigned)(now - h->orphan_since) >= (in->holder_found ? GLUE_HOLD_ORPHAN_MS : GLUE_HOLD_LOST_MS)) {
        h->last_release = now ? now : 1, h->releases++;
        return GLUE_HOLD_REL_ORPHAN;
    }
    if ((unsigned)(now - h->held_since) >= (!in->holder_seen ? GLUE_HOLD_MAX_OTHER_MS : in->max_ms ? in->max_ms : GLUE_HOLD_MAX_MS)) {
        h->last_release = now ? now : 1, h->releases++;
        return GLUE_HOLD_REL_MAX;
    }
    return GLUE_HOLD_NONE;
}

#endif
