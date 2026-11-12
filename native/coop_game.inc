/* coop_game.inc: stage 4 glue between the session layer (coop_session.c) and the game. Included at the end of coop3.c (same translation
 * unit: it uses rd32/rdf/wr/do_call/player_obj/game_area/s3_run/vg_slot from there and runs on the game thread from frame_callbacks).
 *
 * Roles (notes\stage3.md): the character IS the player slot (Claire/Barry = slot 0 = pad 0, Moira/Natalia = slot 1 = pad 1).
 * On every PC the local character is a human slot (type 0) shown full screen, the other one is a remote slot (type 2) that is
 * moved with the game's own remote-player teleport from the interpolated network state. Type-2 slots stall level loads, so the
 * puppet is turned into an AI partner (type 1) for the length of a local load and back afterwards.
 *
 * Console (all `net ...`, see coop_net_command): host/join/status/char/ready/start/stop/session/event/say (coop_session.c) and
 *   glue | setup | tp <slot> x y z [yawrad] | anim <0|1|all> | pads <both|1> | trace <0|1> | auto <0|1> | simload [ms] | name <text>
 * Environment (tests): REV2_NET=host[:port][:char] | join:<ip[:port]>[:char] , REV2_NET_AUTO=1 , REV2_NAME , REV2_NET_INTERP=<ms>.
 */
#include "coop_session.h"
#include "menu.h"
#include "glue_logic.h"
#include "grapple_logic.h"   /* combat2: the puppet's default animation mask (GP_PUPPET_ANIM_W0/W1) */
#include "pad_logic.h"       /* flashlive R2-6: pad_seat_writes / pad_cpad_fix (the seats after a load) */

#define NETLOG_PREFIX "net: "

static cs_t *g_cs;
static CRITICAL_SECTION g_nlock;
static int g_nlock_init;
typedef struct { int what, pid, a, b; char text[100]; } gnote_t;
static gnote_t g_notes[64];
static unsigned g_note_w, g_note_r;
static unsigned g_wait_notes;       /* 6i: 'still waiting for the other PC to load' notes seen */
static cp_event g_evq[32];          /* world events (stage 6): copied out of the notification, handled on the game thread (world.inc) */
static unsigned g_evq_w, g_evq_r;
static void w_on_event(const cp_event *e);
static void w_tick(void);
static void w_command(const char *rest, char *out, int size);
static void bot_command(const char *rest, char *out, int size);
static void w_apply_equipment(unsigned p, int slot, const cp_state *o);
static void w_level_reset(void);
static int w_demo_recent(void);
static int w_demo_running(void);
static int scen_get(int idx);   /* scen.inc */
static void scen_tick(void);   /* stage 6e (scen.inc): scenario flag watch / sync, runs every frame, session or not */
static int w_install(void);
static void w_puppet_catchup(int remote_area);   /* 6m (scen.inc): the partner's puppet at a stage change that the other PC began */
static void session_defaults(void);   /* 6i (world.inc): what the lab switched on by hand is the default of a real session */
static int g_defaults_off;            /* REV2_WORLD_DEFAULTS=0 / `net w defaults 0`: a lab run keeps its own switches */
static void ach_frame(void);        /* stage 8b (ach.inc): hooks of the achievement gate, installed once a session is active */
static void aim_frame(void);        /* LAB (aim.inc): `net aim` target tracking; nothing unless the console turned it on */
static void aim_command(const char *arg, char *out, int size);
static void ach_on_event(const cp_event *e);
static void ach_command(const char *rest, char *out, int size);
static void downed_frame(unsigned now);   /* downed.inc: the downed / revive rule (off unless the session flag / `net downed on`) */
static void downed_probe(void);           /* downed.inc: the live probe (test instances, armed by `net downed probe on`): every frame, session or not */
static void downed_on_event(const cp_event *e);
static void downed_on_sample(const cp_state *o);
static int downed_my_flag(void);
static int g_dl_on;                      /* the rule is active this pass (downed.inc); read by the damage gate in world.inc */
static int downed_hit(unsigned p, int *amount);
static void downed_command(const char *rest, char *out, int size);
static int g_w_on, g_w_env, g_w_replica_ai;   /* g_w_replica_ai: 1 = replicas on a client keep their own AI (world.inc) */
static void lm_glue_init(int no_rejoin);   /* lobbymust.inc (0.2.0): a lost partner / host, End Co-Op, the end of Episode 1 */
static void lm_note(const gnote_t *n);
static void lm_frame(void);
static int lm_command(const char *cmd, const char *rest, char *out, int size);

enum { G_OFF = 0, G_LOBBY, G_SETUP, G_WAITGO, G_RUN, G_LOAD };
static const char *const g_state_names[] = {"off", "lobby", "setup", "wait-go", "running", "loading"};
static int g_state;
static int g_my_slot = -1, g_rem_slot = -1, g_rem_pid = -1;
static int g_anim = 1, g_pads_both = 1, g_auto, g_trace;
static int g_hold_act;   /* `net w holdact <id>` (stage 6e, research): the action id the puppet plays while its owner is HELD by an enemy (CP_SF_HELD); 0 = none (the puppet stays as it is) */
static unsigned g_hold_applied;
/* Stage 6f, grab (enemy holds a human): the enemy's grab action (cEm1100ActGrapPL, 0x5985C0) ends the hold when the HELD player's own input state says "break free": a flag byte at player+0x4A6 bit 2
 * (FUN_004C3830(player, 0): player+0x4A0+idx*0x140, byte +6 bit 2 = the stick shake) or bit 31 of the word at player+0x5B0 (FUN_00AD9B60(player, 0x1F, 0, 0)). A type-2 puppet has no input, so the enemy that holds
 * it on THIS PC (the host's real AI, the partner's human is the puppet) would hold it for ever. When the owner's HELD flag falls (the human broke free on its own PC), the puppet gets those two bits for a while:
 * the enemy's own code then releases it. `net w qte 0|1` switches it; counters in `net w qte`. Research, untested on hardware. */
static int g_qte_fwd = 1, g_rem_held_prev;
static unsigned g_qte_force_until, g_qte_releases, g_qte_writes;
static unsigned g_pup_orphan_since, g_pup_orphan_last, g_pup_frees;   /* 6g: puppet held while its owner is free */
static unsigned g_pup_script_since;   /* 6f: tick since the puppet has stood in the scripted action INT_MIN (0 = it does not) */
static unsigned g_anim_bits[8] = {PA_ANIM_W0, PA_ANIM_W1, PA_ANIM_W2, PA_ANIM_W3, PA_ANIM_W4, PA_ANIM_W5, PA_ANIM_W6, PA_ANIM_W7};   /* combat3: pupanim_logic.h (knife, throws, dodge, crouch, flashlight, reload, heal, hit reactions); stage 6b: 256-bit mask (bit n = id n), `net anim add <id> ...` / `net anim list`; 6c adds 13, 17, 21 (aim, seen with LT on Barry) and 22 (fire, RT while aiming): the puppet shows aiming and firing (unverified); combat2 adds the grapple ids 43..54 (the partner's physical attack / follow-up, grapple_logic.h) */
static unsigned g_anim_mask = 0x7C;  /* action ids the puppet may be switched to with ChangeAction: bit n = id n. 2 idle and 3 walk/run are seen; 4-6 are the next ids of the same family (FUN_00723630 puts ids 1-3 in the on-foot
                                      * class and 4-6 in the crouched one; the Raid net code replays all of them on its remote players) - unverified, they do not occur in the first rooms of Episode 1 */
/* combat3 (pupanim_logic.h pa_decide): an action the puppet drops at once on this PC is not switched again until its owner's action changes; `net anim guard 0|1`, REV2_PUPGUARD=0 */
static int g_pa_on = 1;
static pa_guard_t g_pa;
static unsigned g_pa_pup, g_pa_logs;
static unsigned char g_pa_shown[256];   /* ids the mirror has switched the puppet to (first time logged) */
static unsigned g_t_state, g_t_join, g_t_spoof;   /* GetTickCount of state entry / last join request / pad spoof */
static unsigned g_obj0, g_obj1, g_area_id, g_stable;
static unsigned g_simload_until;
static unsigned g_coop_lost_since;   /* GetTickCount since the game's coop flag has been 0 while running (0 = it is 1) */
static unsigned g_resetups;
static int g_gate_on = 0;                    /* stage 6f scene gate (glue_logic.h): after every barrier the remote slot stays the AI partner until the local human has been under control; OFF by default (the hang it was written for had another cause, see world.inc h_create); `net gate 0|1|open`, REV2_GATE=1 */
static int g_scene_gate;                     /* 1 = the gate is closed (remote slot = type 1) */
static glue_gate_t g_gate;
static unsigned g_gate_log_t, g_gate_opened, g_gate_last_wait_ms, g_gate_forced;
static unsigned g_applied, g_sampled_none, g_local_sent, g_loads_seen;
static float g_last_local[3];
static int g_have_last_local;
static char g_name[CP_NAME_MAX + 1];

static void net_log_sink(const char *line) { r2_log(NETLOG_PREFIX "%s", line); }

/* ---------------------------------------------------------------- real input (stage 5, task 1)
 * The game reads the keyboard and the mouse for a human player only while the story block's "input mode" word (+0x15C4BC, 1 = keyboard+mouse) is 1. Its per-frame
 * updater FUN_009885d0 (called from sBioMain_vf6) clears the word at once while the coop flag gm+0x8F0 is 1 (split screen = pads only), which is why a keyboard
 * stopped working as soon as the second human joined. The glue patches that one test away (the JZ at 0x98861C, 6 bytes, becomes NOPs; the bytes are checked) while a net session runs; the
 * updater then switches to 1 by itself on any key / mouse movement, as in single player. Every PC has exactly ONE type-0 human (the other character is a type-2 puppet), so
 * the word that is global in the game selects the local human's device. Real XInput pads are passed through to the local slot by testmode.c (pad_human); a pad that is
 * being used sets the word back to 0 here. REV2_INPUT=auto|pad|kbm (default auto) or `net input`. */
#define KBM_GUARD 0x98861Cu
static int g_kbm_patched, g_input_pref = 2;   /* 0 pad only, 1 keyboard+mouse only, 2 auto */
static unsigned g_input_writes;

static int kbm_patch(int on)
{
    static const unsigned char orig[6] = {0x0F, 0x84, 0x85, 0x00, 0x00, 0x00}, nop[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    unsigned char cur[6];
    if (!r2_safe_read((void *)KBM_GUARD, cur, 6)) return 0;
    if (!memcmp(cur, on ? nop : orig, 6)) {
        g_kbm_patched = on;
        return 1;
    }
    if (memcmp(cur, on ? orig : nop, 6)) {
        r2_log(NETLOG_PREFIX "keyboard patch: bytes at %08x are %02x %02x %02x %02x %02x %02x, not the expected ones; left alone", KBM_GUARD, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect((void *)KBM_GUARD, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)KBM_GUARD, on ? nop : orig, 6);
    VirtualProtect((void *)KBM_GUARD, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)KBM_GUARD, 6);
    g_kbm_patched = on;
    r2_log(NETLOG_PREFIX "keyboard/mouse allowed in split-screen mode: %s", on ? "patch on" : "patch off");
    return 1;
}

static unsigned story_block(void);
static void input_pass(void)
{
    unsigned sb = story_block();
    if (!sb || !g_kbm_patched) return;
    unsigned now = GetTickCount(), act = testmode_pad_activity();
    int want = g_input_pref == 0 ? 0 : g_input_pref == 1 ? 1 : (act && (int)(now - act) < 2500) ? 0 : -1;
    if (want >= 0 && (int)rd32(sb + 0x15C4BC) != want) {
        unsigned w = (unsigned)want;
        wr(sb + 0x15C4BC, &w, 4), wr(sb + 0x15C4C0, &w, 4);
        g_input_writes++;
    }
}

static void note_cb(void *user, const cs_note *n)
{
    (void)user;
    menu_on_note(n);   /* the lobby screen (stage 8) */
    if (n->what == CS_N_EVENT && n->ev && n->ev->kind == COOPSAVE_EV_KIND) {   /* 0.2.1: the host's checkpoint record (before START of a Continue) */
        if (g_cs && !cs_is_host(g_cs)) coopsave_on_event(n->ev->body, n->ev->len);
        return;
    }
    if (!g_nlock_init) return;
    EnterCriticalSection(&g_nlock);
    if (n->what == CS_N_EVENT && n->ev && n->ev->kind >= CP_EV_USER + 0x20) {
        g_evq[g_evq_w++ % 32] = *n->ev;
        if (g_evq_w - g_evq_r > 32) g_evq_r = g_evq_w - 32;
        LeaveCriticalSection(&g_nlock);
        return;
    }
    gnote_t *g = &g_notes[g_note_w++ % 64];
    g->what = n->what, g->pid = n->pid, g->a = n->a, g->b = n->b;
    snprintf(g->text, sizeof g->text, "%s", n->text ? n->text : "");
    if (g_note_w - g_note_r > 64) g_note_r = g_note_w - 64;
    LeaveCriticalSection(&g_nlock);
}

static cs_t *net_session(void)
{
    if (g_cs) return g_cs;
    if (!g_nlock_init) InitializeCriticalSection(&g_nlock), g_nlock_init = 1;
    cn_set_log(net_log_sink);
    cs_config c;
    memset(&c, 0, sizeof c);
    char env[64] = "", sha[80];
    if (!g_name[0]) {   /* the lab's REV2_NAME, else the Steam persona name of the lobby settings; none: "Player 1" / "Player 2" (never the Windows user or computer name) */
        if (r2_env("REV2_NAME", env, sizeof env)) snprintf(g_name, sizeof g_name, "%s", env);
        else snprintf(g_name, sizeof g_name, "%s", menu_player_name());
    }
    c.name = g_name;
    c.want_char = CP_CHAR_NONE;
    snprintf(sha, sizeof sha, "%s", r2_exe_sha());
    for (int i = 0; i < 8 && strlen(sha) >= (size_t)(2 * i + 2); i++) {
        char b[3] = {sha[2 * i], sha[2 * i + 1], 0};
        c.exe_tag[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    c.mod_version = R2_VERSION;
    c.on_note = note_cb;
    if (r2_env("REV2_NET_INTERP", env, sizeof env)) c.interp_delay_ms = atoi(env);
    if (r2_env("REV2_GATE", env, sizeof env)) g_gate_on = atoi(env) != 0;   /* 6f: REV2_GATE=1 switches the scene gate on */
    c.no_rejoin = !(r2_env("REV2_REJOIN", env, sizeof env) && env[0] == '1');   /* 0.2.0: nobody joins a running game; the lab keeps the rejoin with REV2_REJOIN=1 */
    lm_glue_init(c.no_rejoin);
    g_cs = cs_new(&c);
    return g_cs;
}

/* ---------------------------------------------------------------- game queries and actions */
static unsigned story_block(void) { return rd32(0x157D120); }
static unsigned story_area_id(void) { unsigned sb = story_block(); return sb ? rd32(sb + 0x140380) : 0; }

/* Both characters exist in an episode area (not in menus / while the area is torn down). */
static int game_in_episode(void) { return game_area() != 0 && player_obj(0) != 0 && player_obj(1) != 0; }

static float player_yaw(unsigned p) { return 2.0f * atan2f(rdf(p + 0x54), rdf(p + 0x5C)); }

/* GameMan_SetSlotType (the manager keeps the type across level loads); the player object is refreshed when it exists */
static void set_slot_type(int slot, int type)
{
    unsigned gm = rd32(0x157AE00);
    if (!gm) return;
    unsigned a[3] = {gm, (unsigned)slot, (unsigned)type};
    do_call(2, 0x887F50, a, 3);
    if (player_obj(slot)) {
        unsigned b[2] = {rd32(0x1567EAC), (unsigned)slot};
        do_call(2, 0x6E6110, b, 2);
    }
}

/* sBioMain+0x28626: the game is paused (pause menu open); found by comparing the flag words of sBioMain with and without the menu */
static int game_paused(void)
{
    unsigned bm = rd32(0x15DE6F8);
    unsigned char b = 0;
    return bm && r2_safe_read((void *)(bm + 0x28626), &b, 1) && b;
}

/* ---- equipment (read-only calls into the game): the weapon slot in the hand and the item ids in the weapon slots ---- */
static unsigned item_id_at(int slot, unsigned idx)
{
    unsigned si = rd32(0x157D3F4);
    if (!si || idx >= 0x22) return 0;
    unsigned arr = rd32(si + 0x418), cnt = rd32(si + 0x40C);
    if (!arr || (unsigned)slot >= cnt) return 0;
    unsigned pi = rd32(arr + 4 * (unsigned)slot);
    if (!pi) return 0;
    unsigned e = rd32(pi + 0xC + 4 * idx);
    return e ? rd32(e + 4) : 0;
}

static unsigned equipped_item(unsigned p, int slot_type)
{
    unsigned a[2] = {p, (unsigned)slot_type};
    unsigned idx = do_call(2, 0x6FB270, a, 2);   /* FUN_006fb270(slot type 1..3): inventory index of the item in that weapon slot, -1 none */
    return idx == 0xFFFFFFFFu ? 0 : item_id_at((int)(rd32(p + 0x790C) & 0xFF), idx);
}

static int g_action_seen[256];
static int sample_local(cp_state *st)
{
    unsigned p = player_obj(g_my_slot);
    if (!p) return 0;
    memset(st, 0, sizeof *st);
    st->pos[0] = rdf(p + 0x40), st->pos[1] = rdf(p + 0x44), st->pos[2] = rdf(p + 0x48);
    st->yaw = player_yaw(p);
    st->action = rd32(p + 0x1A54);
    st->hp = (int16_t)(int)rd32(p + 0x1A08), st->hpmax = (int16_t)(int)rd32(p + 0x1A0C);
    st->aim = rdf(p + 0x7950);
    st->aimv[0] = rdf(p + 0x7960), st->aimv[1] = rdf(p + 0x7964), st->aimv[2] = rdf(p + 0x7968);
    unsigned a1[1] = {p};
    st->weapon = (uint8_t)do_call(3, 0x6FB060, a1, 1);   /* FUN_006fb060 (fastcall): weapon slot in the hand */
    for (int i = 0; i < 3; i++) st->item[i] = (uint16_t)equipped_item(p, i + 1);
    st->pstate = rd32(p + 0x37B8), st->ctrl = rd32(p + 0x78E8);
    float mv = rdf(p + 0x792C);
    st->move = (uint8_t)(mv <= 0 ? 0 : mv >= 1 ? 255 : (int)(mv * 255.0f));
    st->elev = (int16_t)(int)(rdf(p + 0x7954) * 100.0f);
    {   /* 6g: the engine's own game-over state (glue_logic.h): the dead mask of sGameMan, the game-over state of aGame, the engine's per-pass predicate. 6f guessed from hp / action id and missed a held human's fall into the void (hp 96, act INT_MIN, pstate 4000) */
        unsigned gm0 = rd32(0x157AE00), ag = game_area();
        glue_dead_t dd;
        dd.hp = (int)rd32(p + 0x1A08), dd.action = st->action, dd.pstate = st->pstate, dd.ctrl = st->ctrl, dd.f7904 = rd32(p + 0x7904);
        dd.mask_bit = gm0 && g_my_slot >= 0 && g_my_slot < 8 ? (int)((rd32(gm0 + 0x878) >> g_my_slot) & 1) : 0;
        dd.agame_state = ag ? rd32(ag + 4) & 0xFF : 0;
        unsigned why = glue_dead_reasons(&dd);
        static unsigned last_why;
        if (why) st->flags |= CP_SF_DEAD;
        else if (downed_my_flag()) st->flags |= CP_SF_DOWN;   /* stage 9: lying and bleeding, the partner may raise me */
        if (why != last_why) {
            r2_log(NETLOG_PREFIX "dead state of my character %s: reasons %x [hp %d, engine predicate %d, dead mask %d, game-over screen state %d, old void shape %d] hp %d act %d pstate %08x ctrl %08x f7904 %08x aGame state %u", why ? "ON" : "off", why, (why & GLUE_DR_HP) != 0,
                   (why & GLUE_DR_ENGINE_PRED) != 0, (why & GLUE_DR_MASK) != 0, (why & GLUE_DR_SCREEN) != 0, (why & GLUE_DR_OLD_VOID) != 0, dd.hp, (int)dd.action, dd.pstate, dd.ctrl, dd.f7904, dd.agame_state);
            last_why = why;
        }
    }
    if (st->ctrl & 4) st->flags |= CP_SF_AIMING;
    if (game_paused()) st->flags |= CP_SF_PAUSED;
    {
        unsigned gmm = rd32(0x157AE00), vm = gmm ? rd32(gmm + 0x8F4) : 0;
        static int last_vm_held;
        if (vm == 2 || vm == 3) {   /* the engine's cutscene view modes; an enemy's hold camera (the QTE close-up) uses them too: that human is HELD, not in a cutscene, and its partner must keep applying its body (6d: the puppet froze and its HP never updated) */
            int held = st->action == 0x80000000u && !w_demo_recent();
            if (held) st->flags |= CP_SF_HELD;
            else st->flags |= CP_SF_CUTSCENE;
            if (held != last_vm_held) r2_log(NETLOG_PREFIX "view mode %u with action INT_MIN and no scene running: %s (pstate %08x ctrl %08x)", vm, held ? "held by an enemy, sent as HELD" : "cutscene", rd32(p + 0x37B8), rd32(p + 0x78E8));
            last_vm_held = held;
        } else if (st->action == 0x80000000u && !w_demo_recent()) {
            st->flags |= CP_SF_HELD;   /* no hold camera (a held human can also be seen in the normal view): the action INT_MIN alone */
        }
    }
    if (st->action < 256 && !g_action_seen[st->action]) {   /* learning table: every action id this PC's human produced, once, with what was going on */
        g_action_seen[st->action] = 1;
        r2_log(NETLOG_PREFIX "action id %u first seen on my character: pstate %08x ctrl %08x cmd %08x flags7900 %08x hp %d", st->action, st->pstate, st->ctrl, rd32(p + 0x78F8), rd32(p + 0x7900), st->hp);
    }
    if (g_have_last_local) {
        float dx = st->pos[0] - g_last_local[0], dy = st->pos[1] - g_last_local[1], dz = st->pos[2] - g_last_local[2];
        if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) st->flags |= CP_SF_TELEPORT;
    }
    g_last_local[0] = st->pos[0], g_last_local[1] = st->pos[1], g_last_local[2] = st->pos[2], g_have_last_local = 1;
    return 1;
}

/* ---- the remote character ----
 * drive 0: the game's remote teleport every pass (stage 4: the body jumps from sample to sample, root motion of the walk action and collision fight it).
 * drive 1 (default): the game's own remote-player movement, as the Raid net code uses it: write the target (+0x39A0.., yaw +0x39C4) and let the engine move the
 * body toward it over N frames (FUN_00ADCA10 position, FUN_00ADC8C0 yaw, FUN_007129E0 locomotion blend), the walk/idle action plays by itself. Far targets,
 * teleport samples and big height steps still use the hard teleport. */
static int g_drive = 0, g_drive_frames = 3, g_apply_hp = 1;
static int g_die_with = 1, g_auto_retry = 1;       /* a dead partner ends the run for both; a partner that retries takes this PC's death screen along */
static unsigned g_retry_until, g_retry_last, g_remote_dead_epoch, g_died_with;
static float g_drive_tele = 600.0f;
static unsigned g_drive_snaps, g_drive_engine;
static double g_perr_sum, g_perr_max, g_step_sum, g_step_sq, g_step_max;
static unsigned g_perr_n, g_step_n;
static float g_pup_pos[3], g_pup_tgt[3];   /* the puppet's position at the start of the previous pass / the target written then */
static int g_pup_have;
static FILE *g_drift;                       /* `net drift <file>`: O rows = my character, P rows = the puppet as shown; offline: tools\drift.py joins both PCs' files */
static unsigned g_drift_rows, g_drift_flush, g_pup_prev_t;

static void float_arg(unsigned *dst, float f) { memcpy(dst, &f, 4); }

static void apply_remote(const cp_state *o)
{
    unsigned p = player_obj(g_rem_slot);
    if (!p || p == player_obj(g_my_slot)) return;   /* the local human is only ever moved by its own input, never by network data */
    float x = o->pos[0], y = o->pos[1], z = o->pos[2], yaw = o->yaw;
    float cx = rdf(p + 0x40), cy = rdf(p + 0x44), cz = rdf(p + 0x48);
    float dx = x - cx, dz = z - cz, dist = sqrtf(dx * dx + dz * dz);
    if (g_pup_have) {   /* how far the engine got toward the previous target, and how evenly the body moved from pass to pass */
        double ex = (double)(cx - g_pup_tgt[0]), ez = (double)(cz - g_pup_tgt[2]), sx = (double)(cx - g_pup_pos[0]), sz = (double)(cz - g_pup_pos[2]);
        double te = sqrt(ex * ex + ez * ez), st = sqrt(sx * sx + sz * sz);
        g_perr_sum += te, g_perr_n++;
        if (te > g_perr_max) g_perr_max = te;
        g_step_sum += st, g_step_sq += st * st, g_step_n++;
        if (st > g_step_max) g_step_max = st;
    }
    g_pup_pos[0] = cx, g_pup_pos[1] = cy, g_pup_pos[2] = cz;
    if (g_drift && g_pup_have) {   /* the position the previous target resulted in, labelled with the sender-clock time of that target */
        fprintf(g_drift, "P,%u,%.2f,%.2f,%.2f,%.4f,%u\n", g_pup_prev_t, cx, cy, cz, player_yaw(p), rd32(p + 0x1A54));
        g_drift_rows++;
    }
    g_pup_prev_t = o->t_ms;
    int tele = g_drive == 0 || (o->flags & CP_SF_TELEPORT) || dist > g_drive_tele || fabsf(y - cy) > 200.0f || !g_pup_have;
    wr(p + 0x39A0, &x, 4), wr(p + 0x39A4, &y, 4), wr(p + 0x39A8, &z, 4), wr(p + 0x39C4, &yaw, 4);
    if (tele) {
        unsigned a1[1] = {p};
        do_call(3, 0x710FD0, a1, 1);   /* the game's own remote-player teleport: target fields -> body, yaw, model */
        wr(p + 0x39A0, &x, 4), wr(p + 0x39A4, &y, 4), wr(p + 0x39A8, &z, 4);
        g_drive_snaps++;
    } else {
        unsigned a[2] = {p, (unsigned)g_drive_frames};
        do_call(2, 0xADCA10, a, 2);   /* position over N frames */
        do_call(2, 0xADC8C0, a, 2);   /* yaw over N frames */
        unsigned m[2] = {p, 0};
        float_arg(&m[1], (float)o->move / 255.0f);
        do_call(2, 0x7129E0, m, 2);   /* locomotion blend (+0x792C, smoothed into +0x7928) */
        g_drive_engine++;
    }
    g_pup_tgt[0] = x, g_pup_tgt[1] = y, g_pup_tgt[2] = z, g_pup_have = 1;
    if (g_apply_hp && o->hpmax > 0 && o->hp > 0) {   /* the owner's health is the truth: local enemies must not wear the puppet down, the HUD / AI see the real value */
        unsigned hp = (unsigned)o->hp, hm = (unsigned)o->hpmax;
        wr(p + 0x1A08, &hp, 4), wr(p + 0x1A0C, &hm, 4);
    }
    float aim = o->aim;
    wr(p + 0x7950, &aim, 4);
    wr(p + 0x7960, o->aimv, 12);
    unsigned pup_cur = rd32(p + 0x1A54);
    int pup_scripted = 0;   /* 6f: the puppet stands in a scripted action on THIS PC (a door's Sync event, an enemy's grab, a scene) while its owner is in one too (HELD flag): forcing the owner's idle action over it
                             * ended the door's Sync event after 30 ms and the door fell back (first 6f run). A scripted puppet whose owner is NOT scripted is let go after 1.5 s (the owner broke free / the event is over). */
    if (pup_cur == 0x80000000u) {
        unsigned tnow = GetTickCount();
        if (!g_pup_script_since) g_pup_script_since = tnow ? tnow : 1;
        if ((o->flags & CP_SF_HELD) || tnow - g_pup_script_since < 1500) pup_scripted = 1;
        static unsigned stuck_log;
        if (!(o->flags & (CP_SF_HELD | CP_SF_CUTSCENE | CP_SF_LOADING)) && tnow - g_pup_script_since > 8000 && tnow - stuck_log > 15000) {   /* 6g (d): the puppet stays in the scripted action while its owner plays normally (129 s after a retry in 6f2): say what it looks like */
            stuck_log = tnow;
            r2_log(NETLOG_PREFIX "puppet (slot %d) has been in the scripted action for %u ms while its owner is in action %u (flags %x): puppet pstate %08x ctrl %08x flags7900 %08x hp %d, demo recent %d, scene gate %d, view mode %u", g_rem_slot,
                   tnow - g_pup_script_since, o->action, o->flags, rd32(p + 0x37B8), rd32(p + 0x78E8), rd32(p + 0x7900), (int)rd32(p + 0x1A08), w_demo_recent(), g_scene_gate, rd32(rd32(0x157AE00) + 0x8F4));
        }
    } else
        g_pup_script_since = 0;
    if (g_anim && !pup_scripted) {
        unsigned act = o->action, tnow = GetTickCount();
        if (p != g_pa_pup) pa_guard_reset(&g_pa), g_pa_pup = p;   /* a new puppet object (level, roles): a fresh guard */
        int d = g_pa_on ? pa_decide(&g_pa, g_anim_bits, act, pup_cur, tnow) : (act != pup_cur && gp_puppet_anim_allowed(g_anim_bits, act));
        if (d == 2 && g_pa_logs++ < 40)
            r2_log(NETLOG_PREFIX "puppet (slot %d) left action %u (%s) %u ms after the switch (now %u): not switched again until its owner's action changes", g_rem_slot, g_pa.last_refused, pa_family(g_pa.last_refused),
                   g_pa.last_refused_ms, pup_cur);
        if (d == 1) {
            unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
            if (fn > 0x400000 && fn < 0x1400000) {
                unsigned args[6] = {p, act, 1, 0, 0xFFFFFFFFu, 0};
                do_call(2, fn, args, 6);
                if (act < 256 && !g_pa_shown[act]) {
                    g_pa_shown[act] = 1;
                    r2_log(NETLOG_PREFIX "puppet (slot %d) shows its owner's action %u (%s) for the first time (it was in %u; now %u)", g_rem_slot, act, pa_family(act), pup_cur, rd32(p + 0x1A54));
                }
            }
        }
    }
    if (g_anim && (o->flags & CP_SF_HELD) && g_hold_act > 0 && rd32(p + 0x1A54) != (unsigned)g_hold_act) {   /* the owner is held by an enemy: its action id is INT_MIN (nothing to replay), show a hold pose */
        unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
        if (fn > 0x400000 && fn < 0x1400000) {
            unsigned args[6] = {p, (unsigned)g_hold_act, 1, 0, 0xFFFFFFFFu, 0};
            do_call(2, fn, args, 6);
            g_hold_applied++;
        }
    }
    w_apply_equipment(p, g_rem_slot, o);   /* stage 6b: the weapon slots / the hand of the owner (world.inc) */
    g_applied++;
}

/* the owner of the puppet broke free (its HELD flag fell): feed the "break free" input bits to the puppet for ~1.5 s, as long as it is still in the scripted hold on this PC */
static void qte_puppet_step(const cp_state *o, unsigned now)
{
    unsigned p = player_obj(g_rem_slot);
    if (!g_qte_fwd || !p) return;
    int held_now = (o->flags & CP_SF_HELD) != 0;
    int grabbed = (rd32(p + 0x7900) & 0x2000) != 0;   /* flags7900 bit 13 is up on a human the enemy holds (trace of 6f2: 0x2000 in every hold, on real humans and puppets, never in a scene) */
    if (g_rem_held_prev && !held_now && grabbed && rd32(p + 0x1A54) == 0x80000000u) {
        g_qte_force_until = now + 1500, g_qte_releases++;
        r2_log(NETLOG_PREFIX "partner broke free: feeding the break-free input to my copy of it (action %d, pstate %08x)", (int)rd32(p + 0x1A54), rd32(p + 0x37B8));
    }
    g_rem_held_prev = held_now;
    if (g_qte_force_until && (int)(now - g_qte_force_until) < 0 && grabbed && rd32(p + 0x1A54) == 0x80000000u) {
        unsigned char b = 0;
        r2_safe_read((void *)(p + 0x4A6), &b, 1);
        b |= 4;
        wr(p + 0x4A6, &b, 1);
        unsigned w = rd32(p + 0x5B0) | 0x80000000u;
        wr(p + 0x5B0, &w, 4);
        g_qte_writes++;
    } else if (g_qte_force_until && (int)(now - g_qte_force_until) >= 0)
        g_qte_force_until = 0;
    /* 6g: the owner is free (its HELD flag is down) but the puppet is still in the hold on this PC after 3 s (the holder is wedged, or the bits did not take): end its action the way the stock grab action
     * does when the holder is gone (ChangeAction 0xB9), again every 2 s. 6f3: a host enemy stuck in action 77 held the partner's puppet for 3 minutes. */
    if (!held_now && grabbed && rd32(p + 0x1A54) == 0x80000000u) {
        if (!g_pup_orphan_since) g_pup_orphan_since = now ? now : 1;
        else if (now - g_pup_orphan_since >= 3000 && (!g_pup_orphan_last || now - g_pup_orphan_last >= 2000)) {
            unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
            g_pup_orphan_last = now ? now : 1, g_pup_frees++;
            r2_log(NETLOG_PREFIX "partner is free but my copy of it is still held after %u ms: ending its grab action (attempt %u, 0xB9)", now - g_pup_orphan_since, g_pup_frees);
            if (fn > 0x400000 && fn < 0x1400000) {
                unsigned args[6] = {p, 0xB9, 0, 0, 0xFFFFFFFFu, 1};
                do_call(2, fn, args, 6);
            }
        }
    } else
        g_pup_orphan_since = g_pup_orphan_last = 0;
}

/* ---- stage 6g, task a / c: holds of the LOCAL human (glue_logic.h). A human an enemy holds (act INT_MIN + flags7900 bit 13) is freed by its own PC when the holder is gone / dead / out of the chain (1.5 s) or after 15 s;
 * every hold is logged (start, the holder, the engine's break-free input bits as they rise, the end) so the shake tempo that breaks a stock grab can be read from the log. `net hold [0|1]`. */
static glue_hold_t g_hold;
static int g_hold_seen;
static int g_hold_guard = 1, g_hold_was, g_hold_prev_shake, g_hold_prev_b31, g_hold_test_orphan;   /* g_hold_test_orphan: `net hold orphan 1` makes the watcher believe the holder is missing (proves its release live without a wedge) */
static unsigned g_hold_n, g_hold_t0, g_hold_rel_orphan, g_hold_rel_max, g_hold_feed_until, g_hold_feed_writes, g_hold_longest, g_hold_freed_by_input, g_hold_tag;
static int w_holder_info(unsigned tag, glue_hold_in_t *in, unsigned *act_out, char *cls, int cls_size);
static int g_hold_shake_half, g_hold_shake_user, g_hold_shake_dir, g_hold_shake_on;   /* `net hold shake <half ms> [delay ms] [user]`: an in-process stick shake that starts <delay> after a hold begins and stops when it ends (a console round trip to the other PC is 3 s) */
static unsigned g_hold_shake_delay = 800, g_hold_shake_t;

static void hold_watch(unsigned now)
{
    unsigned p = player_obj(g_my_slot);
    if (!p) {
        g_hold_was = 0;
        return;
    }
    glue_hold_in_t in;
    memset(&in, 0, sizeof in);
    unsigned act = rd32(p + 0x1A54), f79 = rd32(p + 0x7900), hact = 0;
    char cls[64];
    cls[0] = 0;
    in.held = glue_is_held(act, f79);
    unsigned char b4a6 = 0;
    r2_safe_read((void *)(p + 0x4A6), &b4a6, 1);
    int shake = (b4a6 & 4) != 0, b31 = (int)(rd32(p + 0x5B0) >> 31);
    if (in.held) w_holder_info(rd32(p + 0x3788), &in, &hact, cls, sizeof cls);
    if (in.held && in.holder_found) g_hold_seen = 1;   /* 6g: only a hold that had a holder at some time is an enemy hold (the opening scene of a retry looks like a hold and has none) */
    in.holder_seen = g_hold_seen;
    if (in.held && g_hold_test_orphan) in.holder_found = 0;
    if (in.held && !g_hold_was) {
        g_hold_seen = 0, g_hold_t0 = now ? now : 1, g_hold_n++, g_hold_prev_shake = shake, g_hold_prev_b31 = b31, g_hold_tag = rd32(p + 0x3788);
        r2_log(NETLOG_PREFIX "hold %u start: my character is in the held state (act INT_MIN, flags7900 bit 13; a scripted scene looks the same until a holder is found) (catch id %08x, hp %d), holder %s: %s hp %d act %u, input bits at start: shake %d, 5B0.31 %d", g_hold_n, g_hold_tag, (int)rd32(p + 0x1A08), in.holder_found ? "found" : "not found yet", cls, in.holder_hp, hact, shake, b31);
    } else if (!in.held && g_hold_was) {
        unsigned dur = now - g_hold_t0;
        if (dur > g_hold_longest) g_hold_longest = dur;
        r2_log(NETLOG_PREFIX "hold %u end after %u ms: my action %d hp %d pstate %08x, last input bits: shake %d, 5B0.31 %d; releases by the watcher so far %u", g_hold_n, dur, (int)act, (int)rd32(p + 0x1A08), rd32(p + 0x37B8), shake, b31, g_hold.releases);
    } else if (in.held && (shake != g_hold_prev_shake || b31 != g_hold_prev_b31)) {
        r2_log(NETLOG_PREFIX "hold %u input: shake bit %d, 5B0.31 %d at +%u ms (holder act %u hp %d)", g_hold_n, shake, b31, now - g_hold_t0, hact, in.holder_hp);
        g_hold_prev_shake = shake, g_hold_prev_b31 = b31;
    }
    g_hold_was = in.held;
    if (g_hold_shake_half > 0 && in.held && (unsigned)(now - g_hold_t0) >= g_hold_shake_delay) {   /* test aid: a left-stick shake of the pretended pad while held */
        if (!g_hold_shake_on || (unsigned)(now - g_hold_shake_t) >= (unsigned)g_hold_shake_half) {
            unsigned char gp[12];
            memset(gp, 0, sizeof gp);
            short lx = (short)((g_hold_shake_dir = !g_hold_shake_dir) ? 30000 : -30000);
            memcpy(gp + 4, &lx, 2);
            testmode_pad_pretend(g_hold_shake_user, 1, gp);
            g_hold_shake_t = now, g_hold_shake_on = 1;
        }
    } else if (g_hold_shake_on) {
        testmode_pad_pretend(g_hold_shake_user, 0, NULL);
        g_hold_shake_on = 0;
    }
    if (g_hold_feed_until && in.held && (int)(now - g_hold_feed_until) < 0) {   /* second attempt: the engine's own break-free input for a while */
        unsigned char b = b4a6 | 4;
        wr(p + 0x4A6, &b, 1);
        unsigned w = rd32(p + 0x5B0) | 0x80000000u;
        wr(p + 0x5B0, &w, 4);
        g_hold_feed_writes++;
    } else if (g_hold_feed_until && (int)(now - g_hold_feed_until) >= 0)
        g_hold_feed_until = 0;
    int r = glue_hold_step(&g_hold, now, &in);
    if (r && g_hold_guard) {
        if (r == GLUE_HOLD_REL_ORPHAN) g_hold_rel_orphan++;
        else g_hold_rel_max++;
        int odd = (g_hold.releases & 1) != 0;
        r2_log(NETLOG_PREFIX "hold %u: %s (holder %s: %s hp %d act %u, held %u ms): attempt %u = %s", g_hold_n, r == GLUE_HOLD_REL_ORPHAN ? "the holder is gone / dead / out of the grab chain" : "the hold passed the cap", in.holder_found ? "found" : "NOT FOUND", cls,
               in.holder_hp, hact, now - g_hold_t0, g_hold.releases, odd ? "the stock release action 0xB9" : "the break-free input bits for 1 s");
        if (odd) {
            unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
            if (fn > 0x400000 && fn < 0x1400000) {
                unsigned args[6] = {p, 0xB9, 0, 0, 0xFFFFFFFFu, 1};   /* the human's grab action ends with exactly this call when the holder is gone (cEm1100ActGrapPL, 0x5985C0) */
                do_call(2, fn, args, 6);
            }
        } else
            g_hold_feed_until = now + 1000;
    }
}

/* The death screen is confirmed with the keyboard (mode word 1) and with A on the human's pad (mode word 0); both are sent, the one the game ignores does nothing. */
static void press_confirm(void)
{
    inject_key(0x21, 250);   /* DIK_F: "confirm" in the key config of the test copy (Decide / PickUp = F) */
    unsigned char gp[12];    /* an XINPUT_GAMEPAD: wButtons (word), two trigger bytes, four stick words */
    memset(gp, 0, sizeof gp);
    gp[1] = 0x10;            /* wButtons = 0x1000 = A */
    testmode_pad_hold(g_my_slot, gp, 250);
}

static void run_cmd(const char *cmd)
{
    char out[300];
    out[0] = 0;
    s3_run(cmd, out, sizeof out);
    if (g_trace) r2_log(NETLOG_PREFIX "cmd '%s' -> %.100s", cmd, out);
}

/* stage 6f: the local human's state for the glue's decisions (glue_logic.h) */
static void glue_local(glue_local_t *l)
{
    unsigned p = player_obj(g_my_slot), gm = rd32(0x157AE00);
    memset(l, 0, sizeof *l);
    l->hp = 1;
    if (p) l->act = rd32(p + 0x1A54), l->hp = (int)rd32(p + 0x1A08);
    l->view_mode = gm ? rd32(gm + 0x8F4) : 0;
    l->demo_running = w_demo_running();
}

/* The gate starts at the barrier (G_WAITGO -> G_RUN) and ends when glue_gate_open says so: only then the remote slot becomes the type-2 puppet. */
static void gate_start(unsigned now)
{
    g_scene_gate = 1, g_gate_log_t = now;
    glue_gate_init(&g_gate, now);
    r2_log(NETLOG_PREFIX "scene gate closed: the partner stays the game's AI partner until my human has been under control for %u ms (at least %u ms after the barrier)", GLUE_GATE_CONTROL_MS, GLUE_GATE_SETTLE_MS);
}

static void gate_step(unsigned now)
{
    glue_local_t l;
    glue_local(&l);
    unsigned since, held;
    if (glue_gate_update(&g_gate, now, &l, &since, &held)) {
        if (since >= GLUE_GATE_MAX_MS) g_gate_forced++;
        g_scene_gate = 0, g_gate_opened++, g_gate_last_wait_ms = since;
        g_pup_have = 0;   /* the first applied sample teleports the puppet */
        set_slot_type(g_rem_slot, 2);
        r2_log(NETLOG_PREFIX "scene gate open after %u ms (in control for %u ms): the partner is a remote human now", since, held);
    } else if (now - g_gate_log_t >= 5000) {
        g_gate_log_t = now;
        unsigned gm = rd32(0x157AE00), rp = player_obj(g_rem_slot);
        r2_log(NETLOG_PREFIX "scene gate waiting (%u ms): my act %d hp %d view mode %u demo %d, partner act %d, coop flag %u, SF[1] %d", since, (int)l.act, l.hp, l.view_mode, l.demo_running,
               rp ? (int)rd32(rp + 0x1A54) : -1, gm ? rd32(gm + 0x8F0) : 0, story_area_id() ? scen_get(1) : -1);
    }
}

/* R2-6 (pad_logic.h pad_seat_writes / pad_cpad_fix, noteslashlive.md section 1): every level load the game's spawn swapped the seats of a PC whose human is slot 1
 * (the glue's partner is type 1 at the load), so the human's cPad read logical pad 0, a neutral pad, and this PC's gamepad did nothing until the next load. Put the
 * seats back (slot k = seat k, GameMan_SetSlotSeat 0x8880E0 + sPlayer_RefreshSlotFromGameMan 0x6E6110) before the role set-up changes the slot types (a type change
 * makes the human's uPlIntel re-read its seat into the cPad), and in the running game twice a second; a pad that still reads another pad number gets its slot
 * (FUN_00B429B0 on the embedded cPad: its +0xC = n, button state cleared). The pad number is NOT the cPadEx's own +0xC (the game rewrites that every frame):
 * cPadEx +0x858 is an embedded cPad ("mPad", vtbl_cPad 0x13E6754; +0x85C enabled, +0x860 mode, +0x864 = the pad number; FUN_006EB460 writes +0x860 / +0x864),
 * pair run flashlive-a 22:42: Moira's +0x864 = 0 after one load, `w32` 1 into it -> the pretended pad on game user 1 moved her 317 units at once.
 * `net padseat 0|1` (default 1) is the lab switch; REV2_PADSEAT=0 starts with it off. */
#define PADSEAT_CPADEX_VT 0x13C9268u   /* vtbl_cPadEx */
#define PADSEAT_CPAD_VT 0x13E6754u     /* vtbl_cPad (the cPadEx's embedded pad at +0x858) */
static int g_padseat = 1, g_padseat_env;
static unsigned g_padseat_t, g_padseat_seat_fixes, g_padseat_cpad_fixes, g_padseat_logs;
static int g_padseat_last[2] = {-9, -9}, g_padseat_cpad_last = -9;

static int padseat_cpad_no(unsigned *cpad)   /* the local human's pad number (cPadEx +0x858 = the embedded cPad, its +0xC), or -1 */
{
    *cpad = 0;
    unsigned p = player_obj(g_my_slot), in = p ? rd32(p + 0x78E0) : 0, pd = in ? rd32(in + 0x4C0) : 0;
    if (!pd || rd32(pd) != PADSEAT_CPADEX_VT || rd32(pd + 0x858) != PADSEAT_CPAD_VT) return -1;
    *cpad = pd + 0x858;
    return (int)rd32(pd + 0x858 + 0xC);
}

static void padseat_apply(const char *when)
{
    if (!g_padseat_env) {
        char e[8];
        g_padseat_env = 1;
        if (r2_env("REV2_PADSEAT", e, sizeof e) && e[0] == '0') g_padseat = 0;
    }
    unsigned gm = rd32(0x157AE00);
    if (!gm || g_my_slot < 0 || g_my_slot > 1) return;
    int seat[2] = {(int)rd32(gm + 0x7D4), (int)rd32(gm + 0x7D8)};
    g_padseat_last[0] = seat[0], g_padseat_last[1] = seat[1];
    if (!g_padseat) return;
    int m = pad_seat_writes(seat);
    if (m) {
        for (int k = 0; k < 2; k++)
            if (m & (1 << k)) {
                unsigned a[3] = {gm, (unsigned)k, (unsigned)k};
                do_call(2, 0x8880E0, a, 3);   /* GameMan_SetSlotSeat(k, k) */
            }
        for (int k = 0; k < 2; k++)
            if (player_obj(k)) {
                unsigned b[2] = {rd32(0x1567EAC), (unsigned)k};
                do_call(2, 0x6E6110, b, 2);   /* sPlayer_RefreshSlotFromGameMan: +0x7920 type, +0x7924 seat */
            }
        g_padseat_seat_fixes++;
        if (g_padseat_logs < 30) {
            g_padseat_logs++;
            r2_log(NETLOG_PREFIX "pads: the seats were %d,%d %s (the game's spawn swaps them while slot 0 is the AI partner): set back to 0,1 (my human slot %d)", seat[0], seat[1], when,
                   g_my_slot);
        }
    }
    unsigned pd;
    int no = padseat_cpad_no(&pd);
    g_padseat_cpad_last = no;
    if (no < 0) return;
    int fix = pad_cpad_fix(g_my_slot, no);
    if (fix < 0) return;
    unsigned c[2] = {pd, (unsigned)fix};
    do_call(2, 0xB429B0, c, 2);   /* the embedded cPad: pad number +0xC (= cPadEx +0x864) = fix, its button state cleared */
    g_padseat_cpad_fixes++;
    g_padseat_cpad_last = (int)rd32(pd + 0xC);
    if (g_padseat_logs < 30) {
        g_padseat_logs++;
        r2_log(NETLOG_PREFIX "pads: my human (slot %d) read pad number %d %s: set to %d (cPad %08x; this PC's real pad is game user %d)", g_my_slot, no, when, g_padseat_cpad_last, pd,
               g_my_slot);
    }
}

static void padseat_tick(unsigned now)   /* G_RUN: twice a second */
{
    if (now - g_padseat_t < 500) return;
    g_padseat_t = now;
    padseat_apply("in the running game");
}

/* One step of the role set-up; returns 1 when both humans exist and the types / view are final. */
static int setup_roles_step(void)
{
    unsigned gm = rd32(0x157AE00);
    if (!gm || !game_in_episode()) return 0;
    unsigned now = GetTickCount();
    if (!g_t_spoof) {   /* the game only takes a pad into the game through its own scan (once a second) of "connected" XInput users */
        testmode_pad_keep(1, 1);
        testmode_pad_keep(0, g_pads_both);
        testmode_pad_human(g_my_slot);   /* this PC's real pad (if any) is the local character's pad, whichever slot that is */
        kbm_patch(1);
        g_t_spoof = now, g_t_join = now;
        return 0;
    }
    if (rd32(gm + 0x8F0) != 1) {   /* no second human yet: the game's own join (the pad must have been assigned by its scan) */
        glue_local_t l;
        glue_local(&l);
        if (now - g_t_spoof > 3500 && now - g_t_join > 3000 && glue_may_join(&l)) {   /* 6f: not while a scene owns the human (the join request reaches the aGame state machine in the middle of it) */
            run_cmd("join 1");
            g_t_join = now;
        }
        return 0;
    }
    char c[64];
    padseat_apply("after the load");   /* R2-6: before the types change (the human's uPlIntel re-reads its seat at a type change) */
    for (int slot = 0; slot < 2; slot++) set_slot_type(slot, slot == g_my_slot ? 0 : g_gate_on ? 1 : 2);   /* 6f: the partner is a puppet only once the scene gate has opened (gate_step) */
    snprintf(c, sizeof c, "vp 0 %d", g_my_slot);
    run_cmd(c);
    run_cmd("view 0 1");
    snprintf(c, sizeof c, "viewguard %d", g_my_slot);
    run_cmd(c);
    return 1;
}

/* the former puppet becomes the game's AI partner again; the view stays on the local player */
static void release_puppet(void)
{
    if (g_rem_slot >= 0 && rd32(0x157AE00)) set_slot_type(g_rem_slot, 1);
}

static void glue_set_state(int s)
{
    if (g_state != s) r2_log(NETLOG_PREFIX "glue %s -> %s", g_state_names[g_state], g_state_names[s]);
    if (s == G_LOAD || s == G_SETUP) w_level_reset();   /* object pointers and barrier counters of the old level instance are void */
    if (s != G_RUN) g_scene_gate = 0;   /* the gate belongs to one barrier; a new load / set-up starts its own */
    g_state = s;
    g_t_state = GetTickCount();
}

static void roles_from_session(void)
{
    int ch = cs_local_character(g_cs);
    g_my_slot = ch <= 3 ? CP_CHAR_SLOT(ch) : 0;
    g_rem_slot = 1 - g_my_slot;
    g_rem_pid = cs_remote_pid(g_cs);
}

/* Did the level instance change since the last pass (new player objects or a new area)? Logged when it does. */
static int game_tuple_changed(void)
{
    unsigned ar = game_area(), o0 = player_obj(0), o1 = player_obj(1), id = story_area_id();
    if (ar != 0 && o0 == g_obj0 && o1 == g_obj1 && id == g_area_id) {
        g_stable++;
        return 0;
    }
    int changed = o0 != g_obj0 || o1 != g_obj1 || id != g_area_id;
    if (changed || g_trace) {
        unsigned gm = rd32(0x157AE00);
        r2_log(NETLOG_PREFIX "game tuple: aGame %08x players %08x %08x area %u gm8f0=%u types %u,%u (was %08x %08x area %u)", ar, o0, o1, id, rd32(gm + 0x8F0), rd32(gm + 0x7B4), rd32(gm + 0x7B8), g_obj0,
               g_obj1, g_area_id);
    }
    g_obj0 = o0, g_obj1 = o1, g_area_id = id, g_stable = 0;
    return changed;
}

/* ---------------------------------------------------------------- per pass */
static void retry_on_event(const cp_event *e);   /* warp.inc: the retry coupling (the pause menu's Retry, the lab warp) */
static void sh_ok_on_event(const cp_event *e);   /* warp.inc: 0.3.1 R1, the partner is in Barry's half */
static void handle_notes(void)
{
    for (;;) {   /* world events first (they are cheap and independent of the lobby state) */
        cp_event ev;
        EnterCriticalSection(&g_nlock);
        int have = g_evq_r != g_evq_w;
        if (have) ev = g_evq[g_evq_r++ % 32];
        LeaveCriticalSection(&g_nlock);
        if (!have) break;
        if (ev.kind == CP_EV_USER + 0x70) { retry_on_event(&ev); continue; }   /* RETRY: in any glue state (see warp.inc) */
        if (ev.kind == CP_EV_USER + 0x71) { sh_ok_on_event(&ev); continue; }   /* HALF_OK (0.3.1 R1): in any glue state */
        if (g_state == G_RUN) {
            if (ev.kind >= CP_EV_USER + 0x60 && ev.kind <= CP_EV_USER + 0x6F) downed_on_event(&ev);   /* stage 9: downed / revive */
            else if (ev.kind >= CP_EV_USER + 0x50 && ev.kind <= CP_EV_USER + 0x5F) ach_on_event(&ev);   /* stage 8b: forwarded achievement causes */
            else w_on_event(&ev);
        }
    }
    for (;;) {
        gnote_t n;
        EnterCriticalSection(&g_nlock);
        int have = g_note_r != g_note_w;
        if (have) n = g_notes[g_note_r++ % 64];
        LeaveCriticalSection(&g_nlock);
        if (!have) break;
        lm_note(&n);   /* lobbymust.inc: the strips and the solo / quit steps of 0.2.0 (before the glue changes its state) */
        switch (n.what) {
        case CS_N_CONNECTED:
            roles_from_session();
            if (n.a == CP_PHASE_LOADING || n.a == CP_PHASE_PLAYING) {   /* back in a running game: set the roles up now, the host holds the barrier for us */
                session_defaults();
                r2_log(NETLOG_PREFIX "joined a running game as %s", cp_char_name((unsigned)cs_local_character(g_cs)));
                glue_set_state(G_SETUP);
            } else
                glue_set_state(G_LOBBY);
            break;
        case CS_N_REFUSED:
        case CS_N_LOST:
            r2_log(NETLOG_PREFIX "session ended: %s", n.text);
            if (g_state >= G_SETUP) release_puppet();
            glue_set_state(G_OFF);
            break;
        case CS_N_PEER_JOIN:
            if (g_state == G_OFF) glue_set_state(G_LOBBY);
            roles_from_session();
            break;
        case CS_N_PEER_LEAVE:
            r2_log(NETLOG_PREFIX "player %d left (%s)", n.pid, n.text);
            if (g_state >= G_SETUP && n.pid == g_rem_pid) release_puppet();
            break;
        case CS_N_START:
            session_defaults();
            roles_from_session();
            g_t_spoof = 0;
            r2_log(NETLOG_PREFIX "START: I am %s (slot %d), the other player is slot %d", cp_char_name((unsigned)cs_local_character(g_cs)), g_my_slot, g_rem_slot);
            glue_set_state(G_SETUP);
            break;
        case CS_N_GO:
            r2_log(NETLOG_PREFIX "barrier open (epoch %d)", n.a);
            menu_banner(MB_NONE, NULL, 0);
            g_rem_pid = cs_remote_pid(g_cs);
            break;   /* the state machine below reacts to the phase */
        case CS_N_LOAD_BEGIN:
            g_loads_seen++;
            r2_log(NETLOG_PREFIX "load begin by player %d (epoch %d, area %d)", n.pid, n.a, n.b);
            if (n.pid != cs_pid(g_cs) && g_state == G_RUN && game_in_episode()) {
                /* somebody else loads while this PC's game is not loading: it is "loaded" at once and waits for the barrier */
                w_puppet_catchup(n.b);
                cs_load_begin(g_cs, 0);
                cs_load_done(g_cs);
                glue_set_state(G_WAITGO);
                unsigned lp = player_obj(g_my_slot);
                if (g_auto_retry && lp && (int)rd32(lp + 0x1A08) <= 0) {   /* this PC shows the death screen: the partner chose retry, so do the same */
                    g_retry_until = GetTickCount() + 15000, g_retry_last = 0;
                    r2_log(NETLOG_PREFIX "partner reloads and my character is dead: confirming the retry on my death screen");
                }
            }
            break;
        case CS_N_NOTICE:
            r2_log(NETLOG_PREFIX "host notice: %s", n.text);
            break;
        case CS_N_WAIT:   /* 6i: this PC is playable and the barrier is closed: the log says who is awaited; the banner tells the player (menu.c) */
            g_wait_notes++;
            if (n.b) r2_log(NETLOG_PREFIX "WAITING for %s to finish loading: %d s so far (the barrier opens by itself at %d s)", n.text[0] ? n.text : "the other player", n.a, n.b);
            else r2_log(NETLOG_PREFIX "WAITING for %s to finish loading: %d s so far", n.text[0] ? n.text : "the other player", n.a);
            menu_banner(MB_WAIT, n.text, 40000);
            break;
        case CS_N_LOBBY:
            roles_from_session();
            break;
        default:
            break;
        }
    }
}

static void net_env_autostart(void);
int coop_net_command(const char *arg, char *out, int size);

/* 0.2.1: the host's START; for a Continue (CP_SESS_CONT) the checkpoint record of its co-op save goes out first (the guest follows it), and only when the
 * start can happen (everybody ready), so that a refused start sends nothing */
static int glue_start(int force, char *err, size_t esz)
{
    cp_session ss;
    cs_session_get(g_cs, &ss);
    if ((ss.flags & CP_SESS_CONT) && cs_is_host(g_cs) && cs_phase(g_cs) == CP_PHASE_LOBBY) {
        int n = 0, ready = 0;
        for (int i = 0; i < CP_MAX_PLAYERS; i++) {
            cs_player_info pi;
            if (cs_player(g_cs, i, &pi) && pi.connected) n++, ready += pi.ready || i == 0;
        }
        if (!force && (n < 2 || ready < n)) {
            snprintf(err, esz, n < 2 ? "no other player yet" : "not everybody is ready");
            return 0;
        }
        if (!coopsave_send_record(g_cs, err, (int)esz)) return 0;
    }
    return cs_start(g_cs, force, err, esz);
}

static void coop_game_frame(void)
{
    net_env_autostart();
    if (g_w_env && !g_w_on && w_install()) g_w_on = 1;
    scen_tick();
    downed_probe();   /* reads only; nothing happens unless a test instance armed it */
    if (!g_cs) return;
    cs_set_pflags(g_cs, coopsave_pflags());   /* 0.2.1: the co-op save bits this PC advertises (a guest's HELLO, the host's row) */
    lm_frame();   /* lobbymust.inc: the end of Episode 1, the host's solo step, the guest's way back to the title (also after its session ended) */
    ach_frame();
    aim_frame();
    cs_poll(g_cs);
    handle_notes();
    if (!cs_active(g_cs)) {
        if (g_state != G_OFF) {
            if (g_state >= G_SETUP) release_puppet();
            glue_set_state(G_OFF);
        }
        return;
    }
    if (g_state == G_OFF) return;
    unsigned now = GetTickCount();
    int tuple_changed = game_tuple_changed();
    input_pass();

    if (g_state == G_LOBBY) {
        if (g_auto) {
            cs_player_info pi;
            int me = cs_pid(g_cs);
            if (!cs_is_host(g_cs) && cs_player(g_cs, me, &pi) && !pi.ready && cs_phase(g_cs) == CP_PHASE_LOBBY) cs_ready(g_cs, 1);
            if (cs_is_host(g_cs) && cs_player_count(g_cs) >= 2 && cs_phase(g_cs) == CP_PHASE_LOBBY) {
                static unsigned last_try;
                char err[100];
                if (now - last_try > 1000) {
                    last_try = now;
                    glue_start(0, err, sizeof err);
                }
            }
        }
        return;
    }

    if (g_state == G_SETUP) {
        if (setup_roles_step()) {
            cs_load_done(g_cs);   /* the initial barrier: this PC is in the level with its roles in place */
            glue_set_state(G_WAITGO);
        }
        return;
    }

    if (g_state == G_WAITGO) {
        if (g_retry_until) {
            if ((int)(now - g_retry_until) >= 0) g_retry_until = 0;
            else if (!g_retry_last || now - g_retry_last > 2500) g_retry_last = now ? now : 1, press_confirm();
        }
        if (g_retry_until && (!game_in_episode() || tuple_changed)) g_retry_until = 0;   /* the local load has begun */
        if (!game_in_episode() || tuple_changed) {   /* this PC's own load started while it waited (retry, death screen): same as in the running state */
            r2_log(NETLOG_PREFIX "local load detected while waiting (aGame %08x players %08x %08x area %u)", game_area(), player_obj(0), player_obj(1), story_area_id());
            cs_load_begin(g_cs, story_area_id());
            set_slot_type(g_rem_slot, 1);
            glue_set_state(G_LOAD);
            return;
        }
        if (cs_phase(g_cs) == CP_PHASE_PLAYING) {
            g_have_last_local = 0;
            glue_set_state(G_RUN);
            if (g_gate_on) gate_start(now);   /* 6f: the partner stays the AI partner through the scene that follows a load (opening of Claire's half) */
            else set_slot_type(g_rem_slot, 2);   /* the puppet is a remote human again */
        }
        return;
    }

    if (g_state == G_LOAD) {
        /* local load in progress: wait for the new level instance to settle, redo the roles, report loaded */
        if (g_simload_until) {
            if ((int)(now - g_simload_until) < 0) return;
            g_simload_until = 0;
        } else if (!(game_in_episode() && g_stable > 15))
            return;
        if (setup_roles_step()) {
            cs_load_done(g_cs);
            glue_set_state(G_WAITGO);
        }
        return;
    }

    /* G_RUN: the game drops the second human by itself when the spoofed pad goes away or a section ends the split: bring it back */
    {
        unsigned gm = rd32(0x157AE00), mode = gm ? rd32(gm + 0x8F4) : 0;
        glue_local_t gl;
        glue_local(&gl);
        if (gm && glue_coop_lost_counts((int)rd32(gm + 0x8F0), mode, game_in_episode(), w_demo_recent(), g_scene_gate, &gl)) {
            if (!g_coop_lost_since) g_coop_lost_since = now ? now : 1;
            else if (now - g_coop_lost_since > 8000) {
                r2_log(NETLOG_PREFIX "the game's coop flag has been off for 8 s: setting the roles up again");
                g_coop_lost_since = 0;
                g_t_spoof = 0;   /* the pads may be the reason: spoof them again, then join */
                g_resetups++;
                glue_set_state(G_SETUP);
                return;
            }
        } else
            g_coop_lost_since = 0;
    }
    if (!game_in_episode() || tuple_changed) {
        r2_log(NETLOG_PREFIX "local load detected (aGame %08x players %08x %08x area %u)", game_area(), player_obj(0), player_obj(1), story_area_id());
        cs_load_begin(g_cs, story_area_id());
        set_slot_type(g_rem_slot, 1);   /* a type-2 slot would stall the load */
        glue_set_state(G_LOAD);
        return;
    }
    if (cs_phase(g_cs) == CP_PHASE_PLAYING) {
        cp_state st;
        if (g_scene_gate) gate_step(now);
        hold_watch(now);   /* 6g: holds of the local human (logged, freed when the holder is gone) */
        padseat_tick(now);   /* R2-6: the seats / the human's pad number (twice a second) */
        downed_frame(now);   /* stage 9: the downed / revive rule (nothing happens while it is off) */
        if (sample_local(&st)) {
            if (g_scene_gate) st.flags |= CP_SF_CUTSCENE;   /* 6f: until the gate opens the partner's copy of me is not driven from my samples either */
            if (cs_submit_state(g_cs, &st)) g_local_sent++;
            if (g_drift) {
                fprintf(g_drift, "O,%u,%.2f,%.2f,%.2f,%.4f,%u\n", cs_now(g_cs), st.pos[0], st.pos[1], st.pos[2], st.yaw, st.action);
                if (++g_drift_rows - g_drift_flush >= 120) g_drift_flush = g_drift_rows, fflush(g_drift);
            }
        }
        if (g_rem_pid >= 0) {
            cp_state o;
            int r = cs_remote_sample(g_cs, g_rem_pid, cs_now(g_cs), &o);
            if (r == CS_SAMPLE_NONE) g_sampled_none++;
            else if (r != CS_SAMPLE_STALE) {
                unsigned lp = player_obj(g_my_slot);
                if (g_die_with && (o.flags & CP_SF_DEAD) && lp && (int)rd32(lp + 0x1A08) > 0 && g_remote_dead_epoch != cs_epoch(g_cs) + 1000u * g_loads_seen) {
                    unsigned zero = 0;   /* the partner died: game over for both, as in the split-screen game on one PC; the retry of either one takes both back to the checkpoint */
                    g_remote_dead_epoch = cs_epoch(g_cs) + 1000u * g_loads_seen;
                    wr(lp + 0x1A08, &zero, 4);
                    g_died_with++;
                    r2_log(NETLOG_PREFIX "the partner is dead: my character dies with it (game over for both)");
                }
                downed_on_sample(&o);
                if (!g_scene_gate) qte_puppet_step(&o, now);
                if (!g_scene_gate && !(o.flags & (CP_SF_CUTSCENE | CP_SF_LOADING)) && !(o.flags & CP_SF_DEAD)) apply_remote(&o);   /* the puppet is held while the partner is in a cutscene, loading or dead */
            }
        } else
            g_rem_pid = cs_remote_pid(g_cs);
        w_tick();   /* stage 6: enemy records host -> client (world.inc) */
    } else if (cs_phase(g_cs) == CP_PHASE_LOADING) {
        glue_set_state(G_WAITGO);   /* the barrier closed for another player's load */
    }
}

/* the session object for the lobby screens (menu.c, stage 8) */
cs_t *coop_net_session(void) { return net_session(); }
cs_t *coop_net_session_peek(void) { return g_cs; }   /* the session object if it exists (never creates it) */

/* ---------------------------------------------------------------- console */
static void glue_status(char *out, int size)
{
    int o = snprintf(out, size, "glue: state %s, my slot %d (char %s), puppet slot %d pid %d, anim %d mask %x, pads %s, auto %d, local sent %u, remote applied %u (no sample %u), loads seen %u\n",
                     g_state_names[g_state], g_my_slot, g_cs ? cp_char_name((unsigned)cs_local_character(g_cs)) : "-", g_rem_slot, g_rem_pid, g_anim, g_anim_mask, g_pads_both ? "both" : "1", g_auto,
                     g_local_sent, g_applied, g_sampled_none, g_loads_seen);
    o += snprintf(out + o, size - o, "scene gate %s (%s, opened %u)\n", g_gate_on ? "on" : "off", g_scene_gate ? "CLOSED" : "open", g_gate_opened);
    o += snprintf(out + o, size - o, "re-setups %u, coop-off timer %u, input: pref %s, keyboard patch %d, mode word %u (%u writes), last real pad activity %u ms ago\n", g_resetups, g_coop_lost_since,
                  g_input_pref == 0 ? "pad" : g_input_pref == 1 ? "kbm" : "auto", g_kbm_patched, story_block() ? rd32(story_block() + 0x15C4BC) : 0, g_input_writes,
                  testmode_pad_activity() ? GetTickCount() - testmode_pad_activity() : 0);
    o += snprintf(out + o, size - o, "puppet drive %s (frames %d, snap beyond %.0f): engine moves %u, snaps %u; tracking error to the previous target mean %.2f max %.1f (%u passes); step per pass mean %.2f sd %.2f max %.1f (%u); drift log %s\n",
                  g_drive ? "engine" : "teleport", g_drive_frames, g_drive_tele, g_drive_engine, g_drive_snaps, g_perr_n ? g_perr_sum / g_perr_n : 0.0, g_perr_max, g_perr_n,
                  g_step_n ? g_step_sum / g_step_n : 0.0, g_step_n ? sqrt(fmax(0.0, g_step_sq / g_step_n - (g_step_sum / g_step_n) * (g_step_sum / g_step_n))) : 0.0, g_step_max, g_step_n,
                  g_drift ? "on" : "off");
    unsigned gm = rd32(0x157AE00);
    o += snprintf(out + o, size - o, "game: in episode %d, area id %u, coop flag %u mode %u vp %d,%d, types %u,%u\n", game_in_episode(), story_area_id(), rd32(gm + 0x8F0), rd32(gm + 0x8F4),
                  (int)rd32(gm + 0x8F8), (int)rd32(gm + 0x8FC), rd32(gm + 0x7B4), rd32(gm + 0x7B8));
    for (int i = 0; i < 2; i++) {
        unsigned p = player_obj(i);
        if (p) o += snprintf(out + o, size - o, " slot%d %08x pos=(%.1f,%.1f,%.1f) act=%d hp=%d type=%u\n", i, p, rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), (int)rd32(p + 0x1A54), (int)rd32(p + 0x1A08), rd32(p + 0x7920));
    }
    if (g_cs && g_rem_pid >= 0) {
        cp_state s;
        int r = cs_remote_sample(g_cs, g_rem_pid, cs_now(g_cs), &s);
        snprintf(out + o, size - o, " remote sample %d: pos=(%.1f,%.1f,%.1f) yaw %.2f act %u hp %d", r, s.pos[0], s.pos[1], s.pos[2], s.yaw, s.action, s.hp);
    }
}

/* Console entry (any thread) and the game thread. Returns 0 when it is not a co-op command (bare `net` = the stage 2b capture counters). */
int coop_net_command(const char *arg, char *out, int size)
{
    while (*arg == ' ') arg++;
    if (!*arg) return 0;
    char cmd[24] = "";
    sscanf(arg, "%23s", cmd);
    const char *rest = arg + strlen(cmd);
    while (*rest == ' ') rest++;
    if (!strcmp(cmd, "cap")) return 0;
    cs_t *s = net_session();
    if (!s) {
        snprintf(out, size, "net: cannot create the session object");
        return 1;
    }
    if (!strcmp(cmd, "glue")) {
        glue_status(out, size);
    } else if (!strcmp(cmd, "setup")) {   /* redo the role set-up now (game thread) */
        if (!lt_on_game_thread()) {
            lt_post("net setup", out, size);
            return 1;
        }
        roles_from_session();
        g_t_spoof = 0;
        glue_set_state(G_SETUP);
        snprintf(out, size, "setup restarted (my slot %d)", g_my_slot);
    } else if (!strcmp(cmd, "tp")) {   /* net tp <slot> x y z [yawrad]: move a character with the game's own teleport (game thread) */
        if (!lt_on_game_thread()) {
            char c[200];
            snprintf(c, sizeof c, "net tp %s", rest);
            lt_post(c, out, size);
            return 1;
        }
        int slot = 0;
        float x = 0, y = 0, z = 0, yaw = 0;
        unsigned p;
        if (sscanf(rest, "%d %f %f %f %f", &slot, &x, &y, &z, &yaw) < 4) snprintf(out, size, "usage: net tp <slot> x y z [yawrad]");
        else if (!(p = player_obj(slot))) snprintf(out, size, "no player in slot %d", slot);
        else {
            wr(p + 0x39A0, &x, 4), wr(p + 0x39A4, &y, 4), wr(p + 0x39A8, &z, 4), wr(p + 0x39C4, &yaw, 4);
            unsigned a1[1] = {p};
            do_call(3, 0x710FD0, a1, 1);
            snprintf(out, size, "slot %d moved to %.1f %.1f %.1f yaw %.2f", slot, x, y, z, yaw);
        }
    } else if (!strcmp(cmd, "drive")) {   /* net drive [0|1] [frames] [snap distance]: how the puppet is moved (see apply_remote); `net drive reset` clears the error statistics */
        if (!strncmp(rest, "reset", 5)) {
            g_perr_sum = g_perr_max = g_step_sum = g_step_sq = g_step_max = 0;
            g_perr_n = g_step_n = 0, g_drive_engine = g_drive_snaps = 0;
        } else if (rest[0]) {
            int d = g_drive, f = g_drive_frames;
            float t = g_drive_tele;
            sscanf(rest, "%d %d %f", &d, &f, &t);
            g_drive = d != 0, g_drive_frames = f < 2 ? 2 : f > 30 ? 30 : f, g_drive_tele = t < 50 ? 50 : t;
        }
        snprintf(out, size, "puppet drive %s, frames %d, snap beyond %.0f", g_drive ? "engine" : "teleport", g_drive_frames, g_drive_tele);
    } else if (!strcmp(cmd, "downed")) {   /* net downed [on|off|auto] [bleed <s>] [hold <ms>] [range <u>] [pct <n>] [mode engine|mod|both] [hit <n>] [engine] [slots] [probe on|off|reset] [pkt <state> <from> <to>] (test instances only): stage 9, docs/downed-revive.md */
        downed_command(rest, out, size);
    } else if (!strcmp(cmd, "dying")) {   /* net dying [die_with 0|1] [auto_retry 0|1]: partner death / retry coupling */
        int a = g_die_with, b = g_auto_retry;
        sscanf(rest, "%d %d", &a, &b);
        g_die_with = a != 0, g_auto_retry = b != 0;
        snprintf(out, size, "partner death ends my run: %d, retry follows the partner: %d (deaths taken over %u)", g_die_with, g_auto_retry, g_died_with);
    } else if (!strcmp(cmd, "hold")) {   /* net hold [0|1]: the watcher that frees my character from a hold whose holder is gone / dead / past the cap (hold_watch, 6g); counters */
        if (!strncmp(rest, "orphan", 6)) {
            g_hold_test_orphan = atoi(rest + 6) != 0;
            snprintf(out, size, "hold watcher test: the holder is %s", g_hold_test_orphan ? "TREATED AS MISSING (the watcher frees the human 4 s into a hold)" : "looked up normally");
            return 1;
        }
        if (!strncmp(rest, "shake", 5)) {
            int half = 0, user = 0;
            unsigned delay = 800;
            const char *v = rest + 5;
            while (*v == ' ') v++;
            if (!strncmp(v, "off", 3)) g_hold_shake_half = 0;
            else if (sscanf(v, "%d %u %d", &half, &delay, &user) >= 1) {
                g_hold_shake_half = half < 30 ? 30 : half > 1000 ? 1000 : half, g_hold_shake_delay = delay > 20000 ? 20000 : delay, g_hold_shake_user = user < 0 || user > 3 ? 0 : user;
            }
            snprintf(out, size, "hold shake: %s (half period %d ms, starts %u ms after a hold begins, user %d)", g_hold_shake_half ? "ARMED" : "off", g_hold_shake_half, g_hold_shake_delay, g_hold_shake_user);
            return 1;
        }
        if (rest[0]) g_hold_guard = atoi(rest) != 0;
        snprintf(out, size, "hold watcher %s: holds seen %u, longest %u ms, releases: orphan holder %u, cap %u (total attempts %u), break-free bits written %u, now held %d", g_hold_guard ? "on" : "off", g_hold_n, g_hold_longest, g_hold_rel_orphan, g_hold_rel_max, g_hold.releases,
                 g_hold_feed_writes, g_hold_was);
    } else if (!strcmp(cmd, "qte")) {   /* net qte [0|1]: forward the partner's break-free of a grab to my copy of it (see qte_puppet_step) */
        if (rest[0]) g_qte_fwd = atoi(rest) != 0;
        snprintf(out, size, "grab break-free forwarding %s (releases %u, input writes %u, partner held now %d, puppet freed by 0xB9 %u times)", g_qte_fwd ? "on" : "off", g_qte_releases, g_qte_writes, g_rem_held_prev, g_pup_frees);
    } else if (!strcmp(cmd, "gate")) {   /* net gate [0|1|open]: the scene gate of glue_logic.h (the partner stays the AI partner after a barrier until my human has been under control); `open` ends a closed gate now (game thread) */
        if (!strncmp(rest, "open", 4)) {
            if (!lt_on_game_thread()) {
                lt_post("net gate open", out, size);
                return 1;
            }
            if (g_scene_gate) {
                g_scene_gate = 0, g_gate_forced++, g_pup_have = 0;
                set_slot_type(g_rem_slot, 2);
                r2_log(NETLOG_PREFIX "scene gate opened by hand");
            }
        } else if (rest[0]) g_gate_on = atoi(rest) != 0;
        snprintf(out, size, "scene gate %s, %s (opened %u times, last wait %u ms, forced %u)", g_gate_on ? "on" : "off", g_scene_gate ? "CLOSED now" : "open", g_gate_opened, g_gate_last_wait_ms, g_gate_forced);
    } else if (!strcmp(cmd, "hp")) {   /* net hp 0|1: apply the owner's health to the puppet (default 1) */
        if (rest[0]) g_apply_hp = atoi(rest) != 0;
        snprintf(out, size, "puppet health sync %d", g_apply_hp);
    } else if (!strcmp(cmd, "drift")) {   /* net drift <file>|off: write O (my character) and P (the puppet) rows for offline comparison of the two PCs */
        if (g_drift) fclose(g_drift), g_drift = NULL;
        if (strcmp(rest, "off") && rest[0]) {
            g_drift = fopen(rest, "w");
            g_drift_rows = g_drift_flush = 0;
            if (g_drift) fprintf(g_drift, "# kind,t_ms(sender clock),x,y,z,yaw,action\n");
        }
        snprintf(out, size, g_drift ? "drift log on: %s" : "drift log off", rest);
    } else if (!strcmp(cmd, "anim")) {
        if (!strncmp(rest, "all", 3)) {
            g_anim = 1, g_anim_mask = 0xFFFFFFFFu;
            for (int i = 0; i < 8; i++) g_anim_bits[i] = 0xFFFFFFFFu;
        } else if (!strncmp(rest, "add", 3)) {   /* net anim add <id> [<id> ...]: let the puppet play these action ids (decimal, or 0x hex) */
            const char *q = rest + 3;
            char *end;
            for (unsigned long v = strtoul(q, &end, 0); end != q && v < 256; v = strtoul(q, &end, 0)) g_anim_bits[v >> 5] |= 1u << (v & 31), q = end;
            g_anim = 1;
        } else if (!strncmp(rest, "del", 3)) {
            const char *q = rest + 3;
            char *end;
            for (unsigned long v = strtoul(q, &end, 0); end != q && v < 256; v = strtoul(q, &end, 0)) g_anim_bits[v >> 5] &= ~(1u << (v & 31)), q = end;
        } else if (!strncmp(rest, "guard", 5)) {   /* net anim guard [0|1]: combat3, the refusal guard of the mirror (pupanim_logic.h) */
            if (rest[5]) g_pa_on = atoi(rest + 5) != 0;
            snprintf(out, size, "puppet replay guard %s: switches %u, again after a finished run %u, refusals %u, held passes %u, applied %u, held %u", g_pa_on ? "on" : "OFF", g_pa.n_apply, g_pa.n_reapply,
                     g_pa.n_refused, g_pa.n_held, g_pa.applied, g_pa.held);
            return 1;
        } else if (strncmp(rest, "list", 4)) {
            g_anim = atoi(rest), g_anim_mask = 0x62207C;
            gp_puppet_anim_defaults(g_anim_bits);
        }
        int ao = snprintf(out, size, "puppet animation %d (guard %s, refusals %u), ids:", g_anim, g_pa_on ? "on" : "OFF", g_pa.n_refused);
        for (int i = 0; i < 256 && ao < size - 8; i++)
            if (g_anim_bits[i >> 5] & (1u << (i & 31))) ao += snprintf(out + ao, size - ao, " %d", i);
    } else if (!strcmp(cmd, "w")) {   /* net w on|off|observe|guard|log|status|obj|spawn: stage 6 world sync (world.inc) */
        w_command(rest, out, size);
    } else if (!strcmp(cmd, "ach")) {   /* net ach status|totals|table|gate|fwd|...: stage 8b achievements (ach.inc); game thread */
        if (!lt_on_game_thread()) {
            char c[400];
            snprintf(c, sizeof c, "net ach %s", rest);
            lt_post(c, out, size);
            return 1;
        }
        ach_command(rest, out, size);
    } else if (!strcmp(cmd, "aim")) {   /* net aim at|watch|pos|joints|off|status: LAB aim aid for this PC's human's shots (aim.inc); game thread */
        if (!lt_on_game_thread()) {
            char c[400];
            snprintf(c, sizeof c, "net aim %s", rest);
            lt_post(c, out, size);
            return 1;
        }
        aim_command(rest, out, size);
    } else if (!strcmp(cmd, "bot")) {   /* net bot go|stop|status|...: stage 6c waypoint walker (bot.inc); game thread */
        if (!lt_on_game_thread()) {
            char c[400];
            snprintf(c, sizeof c, "net bot %s", rest);
            lt_post(c, out, size);
            return 1;
        }
        bot_command(rest, out, size);
    } else if (!strcmp(cmd, "kbm")) {   /* net kbm <0|1>: the keyboard/mouse patch by hand (game thread: it changes code the game thread executes) */
        if (!lt_on_game_thread()) {
            char c[200];
            snprintf(c, sizeof c, "net kbm %s", rest);
            lt_post(c, out, size);
            return 1;
        }
        int ok = kbm_patch(atoi(rest) != 0);
        snprintf(out, size, "keyboard patch %s (%d)", g_kbm_patched ? "on" : "off", ok);
    } else if (!strcmp(cmd, "input")) {   /* net input [auto|pad|kbm]: which device the local human uses (default auto) */
        if (rest[0]) g_input_pref = !strncmp(rest, "pad", 3) ? 0 : !strncmp(rest, "kbm", 3) || !strncmp(rest, "key", 3) ? 1 : 2;
        snprintf(out, size, "input preference %s", g_input_pref == 0 ? "pad" : g_input_pref == 1 ? "kbm" : "auto");
    } else if (!strcmp(cmd, "padseat")) {   /* net padseat [0|1]: R2-6, the seats after a load (default 1) */
        if (rest[0]) g_padseat = atoi(rest) != 0, g_padseat_env = 1;
        snprintf(out, size, "padseat %s: seat fixes %u, pad number fixes %u; last seats %d,%d, my human (slot %d) pad number %d", g_padseat ? "on" : "off", g_padseat_seat_fixes,
                 g_padseat_cpad_fixes, g_padseat_last[0], g_padseat_last[1], g_my_slot, g_padseat_cpad_last);
    } else if (!strcmp(cmd, "pads")) {
        g_pads_both = strncmp(rest, "1", 1) != 0;
        snprintf(out, size, "spoofed pads: %s", g_pads_both ? "0 and 1" : "1 only");
    } else if (!strcmp(cmd, "trace")) {
        g_trace = atoi(rest);
        snprintf(out, size, "net trace %d", g_trace);
    } else if (!strcmp(cmd, "auto")) {
        g_auto = rest[0] ? atoi(rest) : 1;
        snprintf(out, size, "auto %d: clients get ready by themselves, the host starts when all are ready", g_auto);
    } else if (!strcmp(cmd, "name")) {
        if (cs_active(s)) snprintf(out, size, "leave the session first (net stop)");
        else {
            snprintf(g_name, sizeof g_name, "%s", rest);
            cs_free(s);
            g_cs = NULL;
            snprintf(out, size, "name '%s'", g_name);
        }
    } else if (lm_command(cmd, rest, out, size)) {   /* lm | endcoop | solo | quittitle (lobbymust.inc) */
    } else if (!strcmp(cmd, "simload")) {   /* barrier test without a real load: this PC reports a load of `ms` milliseconds */
        unsigned ms = (unsigned)atoi(rest);
        if (!ms) ms = 3000;
        if (cs_phase(s) != CP_PHASE_PLAYING || g_state != G_RUN) snprintf(out, size, "only while running (glue state %s)", g_state_names[g_state]);
        else {
            cs_load_begin(s, story_area_id());
            g_simload_until = GetTickCount() + ms;
            if (lt_on_game_thread()) set_slot_type(g_rem_slot, 1);
            glue_set_state(G_LOAD);
            snprintf(out, size, "simulated load of %u ms started", ms);
        }
    } else if (!strcmp(cmd, "start")) {   /* 0.2.1: the session's start through glue_start (the Continue record first) */
        char err[160];
        if (glue_start(!strncmp(rest, "force", 5), err, sizeof err)) snprintf(out, size, "START sent");
        else snprintf(out, size, "not started: %s", err);
    } else {
        int handled = cs_command(s, arg, out, (size_t)size);
        if (!handled) {
            snprintf(out, size, "net: unknown command '%s' (host join status char ready start stop session event say glue setup tp anim pads trace auto name simload; plain `net` = capture counters)", cmd);
            return 1;
        }
        if (!strcmp(cmd, "host") && cs_is_host(s)) {
            cp_session ss;   /* the session settings come from the game the host is in */
            cs_session_get(s, &ss);
            unsigned sb = story_block();
            if (sb) {
                ss.episode = (uint8_t)rd32(sb + 0x140378), ss.part = (uint8_t)(rd32(sb + 0x15C4A8) & 1), ss.area_id = rd32(sb + 0x140380);
                cs_set_session(s, &ss);
            }
            glue_set_state(G_LOBBY);
            roles_from_session();
        }
    }
    return 1;
}

/* REV2_NET=host[:port][:char] | join:<ip[:port]>[:char] , REV2_NET_AUTO=1: start from the environment, once, at the first pass */
static void net_env_autostart(void)
{
    static int done;
    if (done) return;
    done = 1;
    char v[160], out[600], cmd[200];
    if (r2_env("REV2_NET_AUTO", v, sizeof v) && v[0] == '1') g_auto = 1;
    if (r2_env("REV2_WORLD", v, sizeof v) && v[0] == '1') g_w_env = 1;   /* stage 6: hooks + world sync from the start (tests) */
    if (r2_env("REV2_WORLD_DEFAULTS", v, sizeof v) && v[0] == '0') g_defaults_off = 1;
    if (r2_env("REV2_REPLICA_AI", v, sizeof v) && v[0] == '1') g_w_replica_ai = 1;   /* stage 6c: keep the replicas' own AI on a client */
    if (r2_env("REV2_PUPGUARD", v, sizeof v) && v[0] == '0') g_pa_on = 0;   /* combat3: the old replay rule (pupanim_logic.h) */
    if (r2_env("REV2_INPUT", v, sizeof v)) g_input_pref = !strncmp(v, "pad", 3) ? 0 : !strncmp(v, "kbm", 3) || !strncmp(v, "key", 3) ? 1 : 2;
    if (!r2_env("REV2_NET", v, sizeof v)) return;
    if (!strncmp(v, "host", 4)) {
        int port = 28410;
        char ch[16] = "";
        const char *p = v + 4;
        if (*p == ':') {
            port = atoi(p + 1);
            const char *c2 = strchr(p + 1, ':');
            if (c2) snprintf(ch, sizeof ch, "%s", c2 + 1);
        }
        if (ch[0]) {
            snprintf(cmd, sizeof cmd, "char %s", ch);
            coop_net_command(cmd, out, sizeof out);
        }
        snprintf(cmd, sizeof cmd, "host %d", port);
    } else if (!strncmp(v, "join:", 5)) {
        char addr[100] = "", ch[16] = "";
        const char *p = v + 5, *last = strrchr(p, ':');
        snprintf(addr, sizeof addr, "%s", p);
        if (last && last != p && cp_char_parse(last + 1) != CP_CHAR_NONE) {
            snprintf(ch, sizeof ch, "%s", last + 1);
            addr[last - p] = 0;
        }
        if (ch[0]) {
            snprintf(cmd, sizeof cmd, "char %s", ch);
            coop_net_command(cmd, out, sizeof out);
        }
        snprintf(cmd, sizeof cmd, "join %s", addr);
    } else
        return;
    coop_net_command(cmd, out, sizeof out);
    r2_log(NETLOG_PREFIX "REV2_NET '%s' -> %s", v, out);
}
