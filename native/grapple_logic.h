/* grapple_logic.h: the pure part of the client's physical attack ("grapple": kick / finisher) on a staggered enemy replica (0.2.0 acceptance item, notes\aim.md
 * section 4), tested offline by tests/test_grapple.c.
 *
 * How the stock game offers a physical attack (static, Ghidra + the memory image, 2026-11-08):
 *   - The ENEMY offers it, not the player: its stagger start (cEm1100ActGroggy vf6 0x5A2A50, and ~20 other reaction starts) calls FUN_00603E10, which registers one
 *     interaction entry per player index 0..1 (FUN_0040FCD0, handles at enemy+0x7570/+0x7574) with the check FUN_00607040 and the action LAB_00603D80. A second kind
 *     (FUN_0061AA10, from the class init uEm1100_vf32, handles +0x7578/+0x757C) has the check FUN_00607A50 / action LAB_0061A990 (the surprise attack on an enemy that has
 *     not noticed anyone). The action calls FUN_00714970(player, enemy, kind): player+0x791C = enemy, ChangeAction 0x2B/0x2C/0x30 (cPlAction_pl_act_grapple).
 *   - The check FUN_00607040 (cdecl enemy, player index, 0): class byte 3, state 1/2, enemy+0x6F4C bit 18 (set by the class init), hp > 0, not dead (vf 0x37C), the
 *     player within enemy+0x7590 (250 units) and in the same zone (+0x3F54/+0x3F58), the class check vf 0x434, a free line of sight, FUN_00703300 (the player may grapple).
 *   - The class check of the Em1100 family (uEm1100_vf269 0x5D5DB0), Episode area:
 *       alerted (vf 0x36C = enemy+0x37B8 bit 30):      1 when the motion flags of layer 0 (enemy+0x5B0, FUN_00AD9B60) have bit 12 or 13 (the stagger window);
 *       not alerted:                                 1 when the action is outside 0x62..0x63 and 0x77..0xEA and the player is within 135 degrees in front;
 *       always 0 when the Intel (+0x6F10) is missing or its +0x500 is 9.
 *     The stagger actions 0x97 / 0x98 are inside 0x77..0xEA: a staggered enemy offers the attack ONLY while it is alerted.
 *   - Bit 30 of +0x37B8 is set by FUN_00616E80(1), called from the Intel (uEmIntel_vf22 / vf64, FUN_00601040 ...) when the enemy notices a human. A client skips the
 *     Intel update of every replica (world.inc, "replica AI off"), so a replica never gets the bit (live, melee run M1: host +0x37B8 48000000, replica 08000000):
 *     its stagger never offers the attack, and the surprise attack (not alerted) is offered on enemies the host's game knows to be alerted.
 * The fix (`net w alert`, default on): the host's enemy record already carries +0x37B8 (world.inc w_send_enemies); the client copies its bit 30 into the replica,
 * nothing else of that word.
 * Second gap (pair run m1, 2026-11-08 04:33-04:36, live): with the bit mirrored the client's class check still said 0 where the host's said 1 (same action, alerted,
 * flags 0x2000). uEm1100_vf269 also needs `bVar7`: Area_IsEpisodeArea(-1) (returned 0 on BOTH copies at stage 151) or else enemy+0x6F8B == 0. +0x6F8B is the
 * enemy's 'something touched / hit me, react' byte: set by the hit handler FUN_0061B050 and by FUN_00605CF0 (from the enemy's own update uEnemy_vf37, after a
 * player's body contact), consumed and cleared by the Intel (FUN_00601040 -> rPosAdjust_vf16(0)). A replica runs its own update but no Intel: once set it
 * stays 1 (live: client +0x6F88 dword 01000001, host 00000001). A console write of 0 turned the client's class check to 1 at once (04:36:11.923).
 * Fix 2 (same switch): the host's record carries its +0x6F8B in byte 31 (bit 0, was 0); the client copies it into the replica. */
#ifndef GRAPPLE_LOGIC_H
#define GRAPPLE_LOGIC_H
#include <stdint.h>

#define GP_ALERT_BIT 0x40000000u        /* enemy+0x37B8 bit 30: alerted (FUN_00616E80(1)); vf 0x36C (0x4C3DF0) reads it */
#define GP_ACT_STAGGER_A 0x97u          /* cEm1100ActGroggy (vtable 0x128B914): motions 0x446/0x447 */
#define GP_ACT_STAGGER_B 0x98u          /* motions 0x452/0x453 */
#define GP_MOTION_WINDOW ((1u << 12) | (1u << 13))   /* layer-0 motion flags (+0x5B0) that uEm1100_vf269 tests through FUN_00AD9B60(0xC / 0xD) */
#define GP_NOTICE_OFS 0x6F8Bu         /* enemy + this = the 'react to a touch / hit' byte (FUN_00605CF0 / FUN_0061B050 set, the Intel clears) */
#define GP_REC_NOTICE 0x01u           /* enemy record byte 31 (host -> client): bit 0 = the host's +0x6F8B != 0 */

/* the client: the replica's +0x37B8 after a host record (host_ps = the record's +0x37B8) */
static inline uint32_t gp_alert_mirror(uint32_t replica_ps, uint32_t host_ps)
{
#ifdef GP_MODEL_BEFORE_FIX
    (void)host_ps;
    return replica_ps;   /* rev2-int 713f816 / aim 3a4aac4: the record's +0x37B8 was sent but never applied */
#else
    return (replica_ps & ~GP_ALERT_BIT) | (host_ps & GP_ALERT_BIT);
#endif
}

/* the host: byte 31 of the enemy record from its +0x6F8B */
static inline uint8_t gp_rec_flags(uint8_t notice)
{
    return notice ? (uint8_t)GP_REC_NOTICE : (uint8_t)0;
}

/* the client: the replica's +0x6F8B after a host record (rec_flags = the record's byte 31) */
static inline uint8_t gp_notice_mirror(uint8_t replica_notice, uint8_t rec_flags)
{
#if defined(GP_MODEL_BEFORE_FIX) || defined(GP_MODEL_BEFORE_NOTICE)
    (void)rec_flags;
    return replica_notice;   /* 3d85f44 and before: the replica kept whatever its own update set (and nobody cleared it) */
#else
    return (rec_flags & GP_REC_NOTICE) ? 1 : 0;
#endif
}

/* the state the class check reads */
typedef struct {
    uint32_t ps;            /* +0x37B8 */
    uint32_t action;        /* +0x1A54 */
    uint32_t motion_flags;  /* +0x5B0 (layer 0) */
    int has_intel;          /* +0x6F10 != 0 */
    uint32_t intel_500;     /* Intel +0x500 (6 while staggered) */
    int player_in_front;    /* FUN_00B516B0(enemy, player, 135 degrees) */
    uint8_t notice;         /* +0x6F8B */
    int episode;            /* Area_IsEpisodeArea(-1) as the class check sees it (0 at stage 151 on both copies, live m1) */
} gp_enemy_t;

/* uEm1100_vf269 (0x5D5DB0) without its online-only side effect (FUN_00887850 = Raid online) */
static inline int gp_class_check(const gp_enemy_t *e)
{
    if (!e->has_intel) return 0;
    int r = 0;
    int may = e->episode ? 1 : e->notice == 0;   /* bVar7 of the decompile: both paths need it */
    if (!may) {
        r = 0;
    } else if ((e->ps & GP_ALERT_BIT) == 0) {
        if ((e->action < 0x77u || e->action > 0xEAu) && (e->action < 0x62u || e->action > 0x63u) && e->player_in_front) r = 1;
    } else if (e->motion_flags & GP_MOTION_WINDOW) {
        r = 1;
    }
    if (e->intel_500 == 9) r = 0;
    return r;
}

/* the stagger window of an alerted enemy: the case the stock game offers to a human standing next to a staggered enemy */
static inline int gp_stagger_offer(const gp_enemy_t *e)
{
    return (e->action == GP_ACT_STAGGER_A || e->action == GP_ACT_STAGGER_B) && gp_class_check(e);
}

/* the surprise attack (FUN_00607A50 path): offered while the class check passes on an enemy that is NOT alerted */
static inline int gp_surprise_offer(const gp_enemy_t *e)
{
    return (e->ps & GP_ALERT_BIT) == 0 && gp_class_check(e);
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * combat2 (2026-11-08, notes\combat2.md, static + motion data):
 *  - Which kind the offer has: FUN_00607040 / LAB_00603D80 take kind = layer-0 motion flag bit 13 ? 1 : 0; FUN_00714970 starts 0x2B (kind 0, the physical attack; 0x2C with
 *    the Charge Attack skill 0x100), 0x2F (kind 1, the follow-up), 0x30 (2), 0x32 (3), 0x33 (4). The stock player rule FUN_00703300 refuses kind 1 for Barry / Claire (class
 *    0x15 / 0x16) in the campaign unless the skill "Follow-Up" (id 0x300, FUN_0097D600) is equipped.
 *  - The flags come from the motion's LMT event group 0. On uEm1120 (stage 151) only motion 0x414 (cEm1100ActDamage action 0x83 = the head stagger) carries bit 12:
 *    frames 0-389. Bit 13 (kind 1): the dying flinch 0x42E (act 0x95), the crawl / down motions.
 *  - The button: the entry's +0x2C 0x23 -> FUN_0040AD80 kind 0x29 (pad, control type 0) / 0x21 (pad, other) / 0x1C (keyboard); sPad bit = table 0x124F020[kind].
 *  - The puppet (coop_game.inc) plays a partner's action only when its id is in the animation mask: the grapple ids 0x2B..0x36 were not, so the host never showed the
 *    client's kick (fix: GP_PUPPET_ANIM_W1). The puppet's own copy of a blow (its kick / shot collision on the host's enemy) must never count there: the real blow comes as
 *    MHIT. The stock rule FUN_0060DFD0 refuses a type-2 attacker only while enemy+0x6FB8 is set (set by the layout / checkpoint spawns, not by every creator), so the host
 *    drops such a pending DI itself (gp_puppet_blow_drop) before the stock update reads it. */
#define GP_ACT_GRAPPLE_FIRST 0x2Bu   /* cPlAction_pl_act_grapple ids 0x2B..0x36 (player action table 0x1495960) */
#define GP_ACT_GRAPPLE_LAST 0x36u
#define GP_ACT_KICK 0x2Bu            /* kind 0 */
#define GP_ACT_FOLLOWUP 0x2Fu        /* kind 1 */
#define GP_MOTION_HEAD_STAGGER 0x414u /* em1120DmA: event group 0 bit 12 at frames 0-389 (the only kind-0 window of uEm1120) */
#define GP_ACT_HEAD_STAGGER 0x83u
#define GP_SKILL_FOLLOWUP 0x300u     /* PlayerSkillCampaignTable row 0x300 = m37_sys_skill 9 "Follow-Up" */
#define GP_SKILL_BYTE_OFS 0x797Du    /* story block + this = the equipped byte of skill index 1 (0x300): [story+0x20] + 0x7955 + 1 * 8 */

/* the puppet's animation mask (bit n of word n / 32 = action id n): before combat2 only word 0 (0x62207C: idle 2, walk 3, 4..6, aim 13 / 17 / 21, fire 22) */
#define GP_PUPPET_ANIM_W0 0x0062207Cu
#ifdef GP_MODEL_BEFORE_PUPPET
#define GP_PUPPET_ANIM_W1 0x00000000u
#else
#define GP_PUPPET_ANIM_W1 ((((1u << (GP_ACT_GRAPPLE_LAST - GP_ACT_GRAPPLE_FIRST + 1)) - 1u)) << (GP_ACT_GRAPPLE_FIRST - 32u))   /* ids 43..54 = word 1 bits 11..22 = 0x007FF800 */
#endif
/* combat3: the product mask is pupanim_logic.h's (the knife, throws, dodge, crouch, flashlight, reload, heal, hit reactions on top of these); the GP_PUPPET_ANIM_W0 / W1
 * words stay the combat2 statement and its model before the puppet fix */
#include "pupanim_logic.h"
static inline void gp_puppet_anim_defaults(uint32_t bits[8])
{
#ifdef GP_MODEL_BEFORE_PUPPET
    for (int i = 0; i < 8; i++) bits[i] = 0;
    bits[0] = GP_PUPPET_ANIM_W0, bits[1] = GP_PUPPET_ANIM_W1;
#else
    pa_anim_defaults(bits);
#endif
}
static inline int gp_puppet_anim_allowed(const uint32_t bits[8], uint32_t act)
{
    return act < 256u && (bits[act >> 5] & (1u << (act & 31u))) != 0;
}

/* the host, before its enemy's stock update: drop a pending DI (valid, attacker, the attacker's owner +0x19C0) that the partner's puppet made itself */
static inline int gp_puppet_blow_drop(int valid, uint32_t attacker, uint32_t attacker_owner, uint32_t puppet, int mailbox_lab)
{
#ifdef GP_MODEL_BEFORE_PUPPET
    (void)valid, (void)attacker, (void)attacker_owner, (void)puppet, (void)mailbox_lab;
    return 0;   /* 5a0af8d: the stock FUN_0060DFD0 alone decided (refused only with enemy+0x6FB8 set) */
#else
    if (!valid || !puppet || mailbox_lab) return 0;   /* `net w mhit mailbox 1` (lab) posts MHIT blows with the puppet as attacker on purpose */
    return attacker == puppet || (attacker && attacker_owner == puppet);
#endif
}

/* the stock FUN_00703300 kind rule for a class, as far as the skill gate and the weapon kind go (the state checks of the player are left out) */
static inline int gp_player_kind_ok(uint32_t cls, int kind, int episode_area, int followup_equipped, int has_wpn, uint32_t wpn_kind, int player_type)
{
    if ((cls == 0x16u || cls == 0x15u) && !episode_area && kind == 1 && !followup_equipped) return 0;
    if (cls == 0x18u) return kind == 3;   /* Natalia: kind 1 needs a held uWpSmInteractive (brick) and FUN_007030A0; kind 3 always (left out here: kind 1) */
    if (kind == 3) return 0;
    if ((kind == 1 || kind == 2) && (!has_wpn || wpn_kind == 1u)) return 0;
    if (player_type == 1 && kind == 2) return 0;
    return 1;
}

/* LAB (net aim grap auto): the physical attack's button. kind = FUN_0040AD80(entry, player) -> the sPad bit (table 0x124F020) -> the XInput pad that makes it
 * (FUN_00C11E20: Y->0x1000, B->0x2000, A->0x4000, X->0x8000, LB 0x100, RB 0x200, LT->0x400, RT->0x800). 0 = unknown. */
static inline uint32_t gp_spad_bit_for_kind(uint32_t kind)
{
    static const uint32_t t[42] = {0, 0x8000, 0x1000, 0x4000, 0x2000, 0x400, 0x800, 0x100, 0x200, 0x6000, 0xC000, 0, 0, 0xC00, 0x300, 0x8000, 0, 0x4000, 0x2000, 0xF0000, 0xF0000,
                                   0xA0000, 0x200, 0, 0x10000, 0x40000, 0x80000, 0x20000, 0x8000, 0x1000, 0x4000, 0x2000, 0x400, 0x800, 0, 0, 0xF0000, 0, 0x4000, 0x1000, 0x100, 0x200};
    return kind < 42u ? t[kind] : 0;
}
typedef struct { uint16_t buttons; uint8_t lt, rt; } gp_xpad_t;
static inline int gp_xinput_for_spad(uint32_t bit, gp_xpad_t *x)
{
    x->buttons = 0, x->lt = x->rt = 0;
    if (bit & 0x1000u) x->buttons |= 0x8000u;
    if (bit & 0x2000u) x->buttons |= 0x2000u;
    if (bit & 0x4000u) x->buttons |= 0x1000u;
    if (bit & 0x8000u) x->buttons |= 0x4000u;
    if (bit & 0x100u) x->buttons |= 0x100u;
    if (bit & 0x200u) x->buttons |= 0x200u;
    if (bit & 0x400u) x->lt = 255;
    if (bit & 0x800u) x->rt = 255;
    return x->buttons || x->lt || x->rt;
}
#endif
