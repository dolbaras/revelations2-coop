/* pupanim_logic.h: which action ids of a partner's human the puppet replays (ChangeAction, coop_game.inc apply_remote) and when it stops replaying one, tested offline by
 * tests/test_combat3.c (notes\combat3.md).
 *
 * The player action table (static, the memory image, 2026-11-08): 0x1495960 = 242 pairs {action class DTI, parameter} (ids 0x00..0xF1); the parameter lands at action+0x10.
 * The audit of every id (class, parameter, what the human does, seen live or not, why it is or is not replayed) is notes\combat3.md section 2. Summary:
 *   0-1 wait, 2 walk_to_wait, 3 walk, 4 walk_squat, 5-12 elude (the dodge: 1 2 4 8 = direction, +0x10 variant), 13-16 / 17-20 weapon_ready_begin / _loop (param 0 = the
 *   main weapon, 0x10 = the slot-3 sub-weapon (a throwable, +0x73C0), 0x8020 / 0x8030 = the special item FUN_006FECF0 / slot 4: Moira's flashlight kind 0x8000), 21 _end,
 *   22-31 weapon_fire (0x10 fire / throw / flash from the aim, 0x20 melee = knife / crowbar, 0x120 / 0x420 melee variants, 0x200 throw of the hand weapon as a new object
 *   with owner +0x19C0, 0x800 special item state 6, 0x1000 quick throw of the slot-3 sub-weapon, 0x2000, 0x4000 the +0x7CFC record), 32-33 reload, 34-37 weapon_change,
 *   38 raid rocket launcher, 39 lighter, 40-41 recovery (heal), 42 inventory, 43-54 grapple (kick / follow-up, combat2), 55-85 move actions (fence, jump over, climb,
 *   fall, ladder, step up, brake, long jump), 86-89 squat (crouch), 90-99 gets_up, 100-107 crab walk, 108-118 die, 119-121 dying_start, 122-123 wait_rescue, 124 rescue,
 *   125 be_rescued, 126 put_sm_interactive, 127 pickup_item, 128 carried, 129 take_down, 130 call_fellow, 131-157 reactions (149-151 = the dying variants), 158-216 damage
 *   reactions, 217-224 gesture (the Raid communication emotes, FUN_00710780), 225 bottle_set_mine, 226-237 trials, 238 setEventAct, 239-241 test.
 * combat2 (b0938e8) replayed 2..6, 13, 17, 21, 22 and the grapple 43..54: the host never showed the client's knife (28), throws (14 / 18 aim, 22-27 / 31), the dodge (7-12),
 * crouching (86-89), the flashlight aim (15 / 16 / 19 / 20), reload (32 / 33), the heal (40 / 41) or any hit reaction of the client's human (158-216).
 * The puppet's own copy of a blow stays harmless: the host drops a pending DI whose attacker (or its owner +0x19C0) is the puppet (grapple_logic.h gp_puppet_blow_drop, P2);
 * a client never applies DIs on replicas (replica AI off).
 *
 * Replay rule (pa_decide): the puppet is switched to the owner's action when the id is in the mask and differs from the puppet's. A type-2 puppet refuses some actions on
 * this PC (no weapon in the hand, an input-gated update, a missing context): it drops them within a frame or two, and the plain rule switched it back every pass (30-60
 * ChangeAction calls a second, the start's side effects each time). New: an id the puppet left less than PA_MIN_RUN_MS after the switch is held (not switched again)
 * until the owner's action changes; an id the puppet played longer and finished while the owner still is in it is switched again (the owner's knife combo restarts the
 * same id). */
#ifndef PUPANIM_LOGIC_H
#define PUPANIM_LOGIC_H
#include <stdint.h>

#define PA_NONE 0xFFFFFFFFu
#define PA_MIN_RUN_MS 200u   /* a puppet that leaves an action sooner than this after the switch refused it (a real animation of the table runs >= 300 ms) */
#define PA_IS_LOCO(a) ((a) >= 2u && (a) <= 4u)   /* walk_to_wait, walk, walk_squat: never held */

/* the default mask, bit n of word n / 32 = action id n */
#ifdef PA_MODEL_BEFORE_C3
/* combat2 b0938e8 (rev2-int f0c66af): 2..6, 13, 17, 21, 22 and the grapple ids 43..54 */
#define PA_ANIM_W0 0x0062207Cu
#define PA_ANIM_W1 0x007FF800u
#define PA_ANIM_W2 0u
#define PA_ANIM_W3 0u
#define PA_ANIM_W4 0u
#define PA_ANIM_W5 0u
#define PA_ANIM_W6 0u
#define PA_ANIM_W7 0u
#else
#define PA_B(n) (1u << ((n) & 31u))
#define PA_RANGE(lo, hi) ((uint32_t)((((hi) & 31u) == 31u ? 0xFFFFFFFFu : ((1u << (((hi) & 31u) + 1u)) - 1u)) & ~((1u << ((lo) & 31u)) - 1u)))   /* lo..hi inside one word */
/* word 0 (0-31): 2 walk_to_wait, 3 walk, 4 walk_squat, 5-12 elude, 13-21 weapon_ready, 22-31 weapon_fire (0 / 1 wait: the puppet settles by itself) */
#define PA_ANIM_W0 (PA_RANGE(2, 31))
/* word 1 (32-63): 32-33 reload, 39 lighter, 40-41 recovery, 43-54 grapple (34-37 weapon_change: the equipment sync owns the hand; 38 Raid; 42 inventory UI; 55-63 move actions) */
#define PA_ANIM_W1 (PA_RANGE(32, 33) | PA_B(39) | PA_RANGE(40, 41) | PA_RANGE(43, 54))
/* word 2 (64-95): 86-89 squat, 90-95 gets_up (64-85: move / ladder actions need the owner's environment target) */
#define PA_ANIM_W2 (PA_RANGE(86, 95))
/* word 3 (96-127): 96-99 gets_up (100-107 crab walk: wall / gap context; 108-121 die / dying: the game-over coupling; 122-125 rescue chain: downed1; 126-127 world objects) */
#define PA_ANIM_W3 (PA_RANGE(96, 99))
/* word 4 (128-159): 131-148 and 152-157 reactions, 158-159 damage (128-130 carried / take_down / call_fellow: story; 149-151 the dying reactions: downed1) */
#define PA_ANIM_W4 (PA_RANGE(131, 148) | PA_RANGE(152, 159))
/* word 5 (160-191), word 6 (192-216): damage reactions (217-224 Raid emotes, 225 bottle mine, 226-241 trials / event / test: not the human's) */
#define PA_ANIM_W5 (PA_RANGE(160, 191))
#define PA_ANIM_W6 (PA_RANGE(192, 216))
#define PA_ANIM_W7 0u
#endif

static inline void pa_anim_defaults(uint32_t bits[8])
{
    bits[0] = PA_ANIM_W0, bits[1] = PA_ANIM_W1, bits[2] = PA_ANIM_W2, bits[3] = PA_ANIM_W3;
    bits[4] = PA_ANIM_W4, bits[5] = PA_ANIM_W5, bits[6] = PA_ANIM_W6, bits[7] = PA_ANIM_W7;
}
static inline int pa_anim_allowed(const uint32_t bits[8], uint32_t act)
{
    return act < 256u && (bits[act >> 5] & (1u << (act & 31u))) != 0;
}

/* a short family name for logs (the audit table of notes\combat3.md section 2) */
static inline const char *pa_family(uint32_t a)
{
    if (a <= 1) return "wait";
    if (a == 2) return "walk_to_wait";
    if (a == 3) return "walk";
    if (a == 4) return "walk_squat (crouched walk)";
    if (a <= 12) return "elude (dodge)";
    if (a <= 20) return a == 14 || a == 18 ? "weapon_ready (throwable aim)" : a == 15 || a == 16 || a == 19 || a == 20 ? "weapon_ready (special item: flashlight)" : "weapon_ready (aim)";
    if (a == 21) return "weapon_ready_end";
    if (a == 22) return "weapon_fire (fire / throw / flash from the aim)";
    if (a == 28 || a == 29 || a == 30) return "weapon_fire (melee: knife / crowbar)";
    if (a == 24 || a == 27) return "weapon_fire (quick throw of the sub-weapon)";
    if (a == 31) return "weapon_fire (throw of the hand weapon)";
    if (a <= 31) return "weapon_fire (variant)";
    if (a <= 33) return "weapon_reload";
    if (a <= 37) return "weapon_change";
    if (a == 38) return "raid_rocket_launcher";
    if (a == 39) return "lighter_fire";
    if (a <= 41) return "recovery (heal)";
    if (a == 42) return "inventory";
    if (a <= 54) return "grapple (physical attack)";
    if (a <= 68 || (a >= 82 && a <= 85)) return "move action (vault / climb / fall / step)";
    if (a <= 81) return "ladder";
    if (a <= 89) return "squat (crouch)";
    if (a <= 99) return "gets_up";
    if (a <= 107) return "crab walk";
    if (a <= 121) return "die / dying";
    if (a <= 125) return "rescue chain (downed)";
    if (a <= 127) return "pickup / put down";
    if (a <= 130) return "carried / take_down / call_fellow";
    if (a <= 157) return a >= 149 && a <= 151 ? "reaction (dying)" : "reaction";
    if (a <= 216) return "damage reaction";
    if (a <= 224) return "gesture (Raid emote)";
    if (a == 225) return "bottle_set_mine";
    if (a <= 241) return "trial / event / test";
    return "?";
}

/* the replay guard of one puppet (reset when the puppet object changes) */
typedef struct {
    uint32_t applied;      /* the id the mirror switched the puppet to last (PA_NONE: none) */
    uint32_t applied_t;    /* when */
    uint32_t held;         /* the id the puppet refused during the owner's current action (PA_NONE: none) */
    uint32_t owner_prev;   /* the owner's id on the previous pass */
    uint32_t n_apply, n_reapply, n_refused, n_held;
    uint32_t last_refused, last_refused_ms;   /* for the log */
} pa_guard_t;

static inline void pa_guard_reset(pa_guard_t *g)
{
    g->applied = g->held = g->owner_prev = PA_NONE;
    g->applied_t = 0;
    g->n_apply = g->n_reapply = g->n_refused = g->n_held = 0;
    g->last_refused = PA_NONE, g->last_refused_ms = 0;
}

/* one pass: own = the owner's action (the sample), pup = the puppet's action on this PC; returns 1 = ChangeAction(own) now; 2 = the puppet was just found refusing
 * `own` (no switch; the caller may log g->last_refused / last_refused_ms) */
static inline int pa_decide(pa_guard_t *g, const uint32_t bits[8], uint32_t own, uint32_t pup, uint32_t now)
{
#ifdef PA_MODEL_BEFORE_C3
    (void)now;
    if (own != pup && pa_anim_allowed(bits, own)) {   /* f0c66af: every pass the ids differ */
        g->applied = own, g->n_apply++;
        return 1;
    }
    return 0;
#else
    if (own != g->owner_prev) g->owner_prev = own, g->held = PA_NONE;   /* a new action of the owner: anything may be tried again */
    if (own == pup || !pa_anim_allowed(bits, own)) return 0;
    if (PA_IS_LOCO(own)) {   /* idle / walk / crouched walk: the puppet's own locomotion switches between them by itself (drive 1): the plain rule, as before */
        g->applied = own, g->applied_t = now ? now : 1, g->n_apply++;
        return 1;
    }
    if (g->held == own) {
        g->n_held++;
        return 0;
    }
    if (g->applied == own && g->applied_t) {   /* switched to it before during this action of the owner; the puppet is out of it again */
        uint32_t ran = now - g->applied_t;
        if (ran < PA_MIN_RUN_MS) {
            g->held = own, g->n_refused++, g->last_refused = own, g->last_refused_ms = ran;
            return 2;
        }
        g->applied_t = now ? now : 1, g->n_reapply++;   /* it played and ended, the owner goes on with it (a combo): again */
        return 1;
    }
    g->applied = own, g->applied_t = now ? now : 1, g->n_apply++;
    return 1;
#endif
}
#endif
