/* downed.inc: engine glue of the downed / revive rule (RE5 / RE6 "agony"). Included at the end of coop3.c after world.inc (same translation unit: rd32/rdf/wr/do_call/player_obj, g_cs, g_my_slot,
 * g_rem_slot, g_state, w_slot_of_player, o_psub come from there). The decisions live in downed_logic.h (tests/test_downed.c); this file reads the engine, feeds the logic and executes its outputs.
 * Design, wire events, the engine facts and the live-check list: docs/downed-revive.md (engine evidence: the private notes\downed-revive.md).
 *
 * STATE: written offline, NEVER RUN. Everything that touches the game is marked `TODO live check`; every decision is logged with the prefix "downed: ". The rule is OFF unless the host's session flag
 * (lobby row "Downed / revive") or `net downed on` / REV2_DOWNED=1 switches it on.
 *
 * Engine facts used (all Ghidra, rerev2.exe build 3658995):
 *   player+0x1A08 / +0x1A0C hp / max hp (int), +0x1A54 current action id, +0x790C role (0 Claire/Barry, 1 Moira/Natalia), +0x7920 kind (0 local human, 1 AI, 2 remote puppet)
 *   action ids (call sites, class names inferred): 0x79 pl_act_dying_start, 0x7A / 0x7B pl_act_wait_rescue, 0x7C pl_act_rescue (the rescuer), 0x7D pl_act_be_rescued, 0xB9 release
 *   player+0x7BD8 = who rescues me, +0x7BE8 = whom I rescue, +0x7BEC = rescue state of the target (0 none, 1 begun, 2 success, 3 cancelled)
 *   FUN_0070CCC0(player) completes a rescue (needs act 0x7D and +0x7BD8 != 0; hp := max hp for the support role, else g_sPlayer+0xCEC; get-up motion)
 *   player HP subtract = 0x715020 (already detoured by world.inc as h_psub): the damage gate lives there (downed_hit)
 */
#include "downed_logic.h"

#define DOWNED_LOG "downed: "
enum { EV_D_DOWN = CP_EV_USER + 0x60, EV_D_DEAD, EV_D_BEGIN, EV_D_KEEPALIVE, EV_D_CANCEL, EV_D_DONE };   /* body: u8 sender slot, u8 target slot */

#define ACT_DYING_START 0x79u
#define ACT_WAIT_RESCUE_A 0x7Au
#define ACT_WAIT_RESCUE_B 0x7Bu
#define ACT_RESCUE 0x7Cu
#define ACT_BE_RESCUED 0x7Du

static dl_t g_dl;
static int g_dl_ready;                 /* g_dl initialised for this session */
static int g_dl_mode = -1;             /* -1 follow the session flag (and REV2_DOWNED), 0 force off, 1 force on */
static int g_dl_on;                    /* the rule is active this pass (read by the damage gate) */
static int g_dl_hold_mode = 2;         /* 0 engine's own rescue action only (observe act 0x7C), 1 mod-driven hold only, 2 both (default: the engine's if it starts, else the mod's) */
static float g_dl_range = 170.0f;      /* mod-driven hold: distance (xz) to the lying partner. TODO live check: the stock HelpPos distance */
static int g_dl_revive_pct = 35;       /* hp after a revive, percent of the maximum, at least what the engine restores */
static uint32_t g_dl_bleed_override = 0xFFFFFFFFu, g_dl_revive_override = 0;   /* console overrides of the session's preset / the mod-driven hold time */
static unsigned g_dl_epoch_key = 0xFFFFFFFFu;
static unsigned g_dl_down_at_tick, g_dl_act_fix_t, g_dl_pup_fix_t, g_dl_banner_t, g_dl_getup_t, g_dl_hit_log;
static int g_dl_getup_pending;
static unsigned g_dl_getup_rescuer;
static int g_dl_keep_log;   /* lines of the main-role dying keep (log limit) */
static int g_dl_engine_hold;           /* my human is in the engine's rescue action on the partner's puppet */
static unsigned g_dl_hits_absorbed, g_dl_hits_down, g_dl_log_n;
static int g_dl_engine_rescuer_seen;   /* the engine started pl_act_rescue on my human at least once (for the mod-driven fallback's decision) */
static int g_dl_kbfix;                 /* the keyboard rescue fix at 0x40F02B: 1 on, 0 off, -1 unexpected bytes (dl_kbfix_apply) */
static unsigned g_dl_hold_log_t;       /* last "rescue input" log line of the mod-driven hold */

static void downed_log_state(const char *why)
{
    char b[420];
    dl_status(&g_dl, b, sizeof b);
    r2_log(DOWNED_LOG "%s | %s", why, b);
}

/* ---------------------------------------------------------------- engine access */
static int dl_is_down_act(unsigned a) { return dl_act_lying_family(a); }   /* 0x79 0x7A 0x7B 0x7D (downed_logic.h) */

static void dl_change_action(unsigned p, unsigned id)
{
    unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
    if (fn > 0x400000 && fn < 0x1400000) {
        unsigned args[6] = {p, id, 0, 0, 0xFFFFFFFFu, 0};
        do_call(2, fn, args, 6);
    }
}

static void dl_set_hp(unsigned p, int hp)
{
    unsigned v = (unsigned)hp;
    wr(p + 0x1A08, &v, 4);
}

static unsigned dl_dead_reasons(unsigned p)
{
    unsigned gm0 = rd32(0x157AE00), ag = game_area();
    glue_dead_t dd;
    dd.hp = (int)rd32(p + 0x1A08), dd.action = rd32(p + 0x1A54), dd.pstate = rd32(p + 0x37B8), dd.ctrl = rd32(p + 0x78E8), dd.f7904 = rd32(p + 0x7904);
    dd.mask_bit = gm0 && g_my_slot >= 0 && g_my_slot < 8 ? (int)((rd32(gm0 + 0x878) >> g_my_slot) & 1) : 0;
    dd.agame_state = ag ? rd32(ag + 4) & 0xFF : 0;
    return glue_dead_reasons(&dd);
}

static float dl_dist_xz(unsigned a, unsigned b)
{
    float dx = rdf(a + 0x40) - rdf(b + 0x40), dz = rdf(a + 0x48) - rdf(b + 0x48);
    return sqrtf(dx * dx + dz * dz);
}

/* the rescue input of my human (downed_logic.h "the rescue input"): the byte sActionButton+0x19FC+role that the first version read is written by the engine ONLY while
 * its own rescue action (0x7C) runs, so the mod-driven hold never saw a key (run C, 2026-11-06). Read the input the way the engine's hold entry does: the rebindable
 * "Rescue" key (keyboard action 0x1E, Tab in the owner's config), the slot's pad button of entry button 0x27 (Y/Triangle, or B/Circle for button types 1..4), and the
 * engine's byte. Engine calls: GAME THREAD only (downed_frame, the console commands post themselves). */
typedef struct {
    int ok;                 /* the code / tables are those of build 3658995: the key and pad parts were read */
    int engine_byte, engine_raw, key_held, kb_mode, btn_id, pad_num, gui_ok;
    unsigned role, keyact;
    uint32_t mask, pad_on;
} dl_input;

static int dl_input_code_ok(void)
{
    static int checked, ok;
    if (checked) return ok;
    uint8_t kq[11] = {0}, bt[12] = {0};
    uint32_t masks[5] = {0};
    int rd = r2_safe_read((void *)DL_IN_KEYQ_FN, kq, sizeof kq) && r2_safe_read((void *)DL_IN_BTN_FN, bt, sizeof bt) && r2_safe_read((void *)DL_IN_BTN_MASKS, masks, sizeof masks);
    if (!rd || !r2_text_decrypted()) return 0;   /* not yet readable (stub): ask again later */
    checked = 1, ok = dl_in_code_ok(kq, bt, masks, rd32(DL_IN_KEYACT_CELL));
    r2_log(DOWNED_LOG "rescue input reader: %s (key query %02x %02x %02x, button %02x %02x %02x, masks %x %x, key action %x)", ok ? "code of build 3658995, on" : "UNEXPECTED code, only the engine byte is read",
           kq[0], kq[1], kq[2], bt[0], bt[1], bt[2], masks[2], masks[4], rd32(DL_IN_KEYACT_CELL));
    return ok;
}

static void dl_read_input(int slot, unsigned p, dl_input *in)
{
    unsigned ab = rd32(DL_IN_AB_CELL), sb = rd32(DL_IN_KBMODE_CELL), gui = rd32(DL_IN_GUI_CELL);
    unsigned char b = 0;
    memset(in, 0, sizeof *in);
    in->kb_mode = sb ? (int)rd32(sb + DL_IN_KBMODE_OFF) : -1, in->btn_id = -1, in->pad_num = -1, in->gui_ok = -1;
    if (gui && r2_safe_read((void *)(gui + DL_IN_GUI_OK), &b, 1)) in->gui_ok = b;
    in->role = p ? rd32(p + 0x790C) & 0xFF : 0xFF;
    if (ab && in->role < 8 && r2_safe_read((void *)(ab + DL_IN_AB_HOLD + in->role), &b, 1)) in->engine_raw = b;
    in->engine_byte = dl_in_engine_byte(in->engine_raw, p ? rd32(p + 0x1A54) : 0);   /* stale outside 0x7C (run H 2026-11-07: still 1 after the rescue) */
    if (!dl_input_code_ok()) return;
    in->ok = 1;
    unsigned cfg = rd32(DL_IN_KEYCFG_CELL);
    in->keyact = rd32(DL_IN_KEYACT_CELL);
    if (cfg && in->keyact == DL_IN_KEYACT) {
        unsigned a[4] = {cfg, 1, in->keyact, 2};
        in->key_held = (do_call(2, DL_IN_KEYQ_FN, a, 4) & 0xFF) != 0;
    }
    if (slot >= 0 && slot < 8) {
        static uint32_t entry[16];   /* FUN_0040AD80 reads only +0x2C of its "entry" */
        entry[0x2C / 4] = DL_IN_RESCUE_BTN;
        unsigned a[2] = {(unsigned)(uintptr_t)entry, (unsigned)slot};
        in->btn_id = (int)do_call(2, DL_IN_BTN_FN, a, 2);
        if (in->btn_id >= 0 && in->btn_id < 0x29) in->mask = rd32(DL_IN_BTN_MASKS + 4u * (unsigned)in->btn_id);
        if (ab) in->pad_on = rd32(ab + DL_IN_AB_CONT(slot) + DL_IN_AB_PAD_ON), in->pad_num = (int)rd32(ab + DL_IN_AB_CONT(slot) + DL_IN_AB_PAD_NUM);
    }
}

static int dl_hold_button(int slot, unsigned p)
{
    dl_input in;
    dl_read_input(slot, p, &in);
    return dl_in_held(in.engine_byte, in.key_held, in.pad_on, in.mask);
}

static int dl_input_text(int slot, unsigned p, char *out, int size)
{
    dl_input in;
    dl_read_input(slot, p, &in);
    return snprintf(out, size, "input slot %d role %u: HOLD %d | engine byte sAB+19FC+role %d (raw %d) | rescue key (action %x) %d, input mode %d (1 = keyboard) | pad %d button id %d mask %04x (%s, XInput %04x) on %08x -> %d | gui actions %d | reader %s | kbfix %d",
                    slot, in.role, dl_in_held(in.engine_byte, in.key_held, in.pad_on, in.mask), in.engine_byte, in.engine_raw, in.keyact, in.key_held, in.kb_mode, in.pad_num, in.btn_id, in.mask,
                    dl_in_pad_name(in.mask), dl_in_xinput_of(in.mask), in.pad_on, (in.pad_on & in.mask) != 0, in.gui_ok, in.ok ? "on" : "OFF (unexpected code)", g_dl_kbfix);
}

/* the stock keyboard bug (downed_logic.h): FUN_0040EDC0 writes the keyboard's hold into sAB+0x19FC+0 whatever the slot, the rescue reads +role: a role-1 human on the
 * keyboard (Natalia raising Barry) is cancelled in the first frame of 0x7C. Patched to the slot's own byte (16 bytes in place, checked) while the rule is on; for slot 0
 * (single player, the stock case) the patched code writes the same byte as before. GAME THREAD (the code is not running while our frame hook runs). */
static int dl_kbfix_apply(int on)
{
    uint8_t cur[16];
    if (!r2_text_decrypted() || !r2_safe_read((void *)DL_KBFIX_AT, cur, 16)) return 0;
    int st = dl_kbfix_state(cur);
    if (st == 0) {
        if (g_dl_kbfix != -1) r2_log(DOWNED_LOG "keyboard rescue fix: bytes at %08x unexpected (%02x %02x %02x ...): left alone", DL_KBFIX_AT, cur[0], cur[1], cur[2]);
        g_dl_kbfix = -1;
        return 0;
    }
    if ((st == 2) == (on != 0)) {
        g_dl_kbfix = on ? 1 : 0;
        return 1;
    }
    DWORD old;
    if (!VirtualProtect((void *)DL_KBFIX_AT, 16, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)DL_KBFIX_AT, on ? dl_kbfix_new : dl_kbfix_orig, 16);
    VirtualProtect((void *)DL_KBFIX_AT, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)DL_KBFIX_AT, 16);
    g_dl_kbfix = on ? 1 : 0;
    r2_log(DOWNED_LOG "keyboard rescue fix %s (FUN_0040EDC0 case 0xD writes sAB+0x19FC+slot)", on ? "ON" : "off");
    return 1;
}

/* ---------------------------------------------------------------- the damage gate (called by h_psub / the PDMG replay): returns 1 = skip the engine's subtraction */
static int downed_hit(unsigned p, int *amount)
{
    if (!g_dl_on || !g_dl_ready || *amount <= 0) return 0;
    if (p != player_obj(g_my_slot) || rd32(p + 0x7920) != 0) return 0;   /* only my own (type 0) human: the puppet's hp is the owner's */
    int hp = (int)rd32(p + 0x1A08);
    if (hp <= 0) return 0;
    unsigned now = GetTickCount();
    if (dl_state(&g_dl, g_my_slot) == DL_DOWN) {   /* lying: the hit does nothing (hp stays 1) */
        g_dl_hits_down++;
        if (g_dl_hit_log++ < 6) r2_log(DOWNED_LOG "hit of %d on my lying character absorbed (hp %d)", *amount, hp);
        return 1;
    }
    if (hp - *amount > 0) return 0;
    int would = *amount;
    if (!dl_local_lethal(&g_dl, now)) {
        r2_log(DOWNED_LOG "lethal hit of %d at hp %d on my character: nobody is up to raise me, the death goes through (caller exe+%x)", would, hp, (unsigned)__builtin_return_address(0) - 0x400000);
        return 0;
    }
    *amount = hp - 1;   /* leave exactly 1 hp */
    g_dl_hits_absorbed++;
    r2_log(DOWNED_LOG "LETHAL hit of %d at hp %d on my character absorbed: hp 1, DOWN (caller exe+%x)", would, hp, (unsigned)__builtin_return_address(0) - 0x400000);
    return *amount == 0;
}

/* ---------------------------------------------------------------- events */
static void downed_send(int kind, int target)
{
    if (!g_cs || !cs_active(g_cs)) return;
    unsigned char b[2] = {(unsigned char)g_my_slot, (unsigned char)(target < 0 ? 255 : target)};
    unsigned k = kind == DL_O_SEND_DOWN ? EV_D_DOWN : kind == DL_O_SEND_DEAD ? EV_D_DEAD : kind == DL_O_SEND_BEGIN ? EV_D_BEGIN : kind == DL_O_SEND_KEEPALIVE ? EV_D_KEEPALIVE : kind == DL_O_SEND_CANCEL ? EV_D_CANCEL : EV_D_DONE;
    cs_send_event(g_cs, k, b, 2);
}

static void downed_on_event(const cp_event *e)
{
    if (!g_dl_on || !g_dl_ready || e->len < 2) return;
    if (e->epoch != (uint8_t)cs_epoch(g_cs)) {   /* an event of another level instance (a retry in between) */
        r2_log(DOWNED_LOG "event %u of epoch %u dropped (mine %u)", e->kind, e->epoch, (unsigned)(uint8_t)cs_epoch(g_cs));
        return;
    }
    int from = e->body[0], target = e->body[1] == 255 ? -1 : e->body[1];
    int k = e->kind == EV_D_DOWN ? DL_E_DOWN : e->kind == EV_D_DEAD ? DL_E_DEAD : e->kind == EV_D_BEGIN ? DL_E_BEGIN : e->kind == EV_D_KEEPALIVE ? DL_E_KEEPALIVE : e->kind == EV_D_CANCEL ? DL_E_CANCEL : DL_E_DONE;
    if (from < 0 || from >= DL_MAX || from == g_my_slot) return;
    if (k != DL_E_KEEPALIVE && g_dl_log_n++ < 200) r2_log(DOWNED_LOG "event from slot %d: %s target %d", from, k == DL_E_DOWN ? "DOWN" : k == DL_E_DEAD ? "DEAD" : k == DL_E_BEGIN ? "BEGIN" : k == DL_E_CANCEL ? "CANCEL" : "DONE", target);
    dl_remote_event(&g_dl, from, k, target, GetTickCount());
}

/* the partner's STATE sample (coop_game_frame, every pass that has one) */
static void downed_on_sample(const cp_state *o)
{
    if (!g_dl_on || !g_dl_ready || g_rem_slot < 0) return;
    dl_remote_sample(&g_dl, g_rem_slot, (o->flags & CP_SF_DOWN) != 0, (o->flags & CP_SF_DEAD) != 0, GetTickCount());
}

/* sample_local asks: is my character DOWN (the STATE flag) */
static int downed_my_flag(void) { return g_dl_on && g_dl_ready && dl_state(&g_dl, g_my_slot) == DL_DOWN; }

/* ---------------------------------------------------------------- executing the logic's outputs */
static void downed_exec(unsigned p, unsigned rp, unsigned now)
{
    dl_out o;
    while (dl_take(&g_dl, &o)) {
        switch (o.kind) {
        case DL_O_GO_DOWN:
            if ((int)rd32(p + 0x1A08) != 1) dl_set_hp(p, 1);
            if (!dl_is_down_act(rd32(p + 0x1A54))) dl_change_action(p, ACT_DYING_START);   /* TODO live check: the chain 0x79 -> 0x7A for the MAIN role (stock only does it for role 1) */
            g_dl_down_at_tick = now;
            r2_log(DOWNED_LOG "GO DOWN: hp 1, action %u -> 0x79 (dying_start), pstate %08x ctrl %08x role %u", rd32(p + 0x1A54), rd32(p + 0x37B8), rd32(p + 0x78E8), rd32(p + 0x790C));
            break;
        case DL_O_DIE:
            dl_set_hp(p, 0);
            r2_log(DOWNED_LOG "DIE: my hp := 0 (game over follows; partner coupling does the rest)");
            break;
        case DL_O_GET_UP:
            g_dl_getup_pending = 1, g_dl_getup_t = now, g_dl_getup_rescuer = rp;
            r2_log(DOWNED_LOG "GET UP requested (by slot %d)", o.arg);
            break;
        case DL_O_REVIVE_PLAY:   /* TODO live check: be_rescued (0x7D) on my lying human, rescuer pointer set */
            wr(p + 0x7BD8, &rp, 4);
            dl_change_action(p, ACT_BE_RESCUED);
            r2_log(DOWNED_LOG "being raised by slot %d: action 0x7D", o.arg);
            break;
        case DL_O_REVIVE_STOP: {
            unsigned z = 0;
            wr(p + 0x7BD8, &z, 4), wr(p + 0x7BEC, &z, 4);
            if (rd32(p + 0x1A54) == ACT_BE_RESCUED) dl_change_action(p, ACT_WAIT_RESCUE_A);
            r2_log(DOWNED_LOG "the revive stopped: back to lying (0x7A)");
            break;
        }
        case DL_O_PUPPET_DOWN:
        case DL_O_PUPPET_UP:
            break;   /* handled by the maintain loop (downed_puppet) so a missed call is repeated */
        case DL_O_SEND_DOWN:
        case DL_O_SEND_DEAD:
        case DL_O_SEND_BEGIN:
        case DL_O_SEND_KEEPALIVE:
        case DL_O_SEND_CANCEL:
        case DL_O_SEND_DONE:
            downed_send(o.kind, o.arg);
            break;
        case DL_O_GAME_OVER:
            r2_log(DOWNED_LOG "GAME OVER decided (%s)", o.arg ? "a human is dead" : "nobody is up");
            break;
        case DL_O_HOLD_RESET:
            break;
        default:
            break;
        }
    }
}

/* the get-up: the engine's own completion once the human is in be_rescued (0x7D) with a rescuer; a fallback after a second */
static void downed_getup_step(unsigned p, unsigned now)
{
    if (!g_dl_getup_pending) return;
    unsigned act = rd32(p + 0x1A54);
    int mx = (int)rd32(p + 0x1A0C);
    if (act != ACT_BE_RESCUED && (int)(now - g_dl_getup_t) < 1000) {
        wr(p + 0x7BD8, &g_dl_getup_rescuer, 4);
        dl_change_action(p, ACT_BE_RESCUED);
        return;
    }
    if (act == ACT_BE_RESCUED) {
        unsigned w0 = rd32(p + 0x7BD8), w = dl_getup_rescuer_word(act, w0, g_dl_getup_rescuer);   /* the completion needs +0x7BD8 != 0: a CANCEL + BEGIN in one pass let the engine clear it */
        if (w != w0) {
            wr(p + 0x7BD8, &w, 4);
            r2_log(DOWNED_LOG "the rescuer word +7BD8 was 0 at the completion: %08x written back", w);
        }
        unsigned a1[1] = {p};
        do_call(2, 0x70CCC0, a1, 1);   /* TODO live check: FUN_0070CCC0 (thiscall, ecx = player): hp restore + get-up motion */
        r2_log(DOWNED_LOG "engine completion called: hp %d act %u", (int)rd32(p + 0x1A08), rd32(p + 0x1A54));
    } else {
        dl_change_action(p, 0);   /* fallback: stand up idle */
        r2_log(DOWNED_LOG "get-up fallback (the be_rescued action did not take): idle, hp %d", (int)rd32(p + 0x1A08));
    }
    int want = mx * g_dl_revive_pct / 100;
    if (want < 2) want = 2;
    if ((int)rd32(p + 0x1A08) < want) dl_set_hp(p, want < mx ? want : mx);
    unsigned z = 0;
    wr(p + 0x7BD8, &z, 4), wr(p + 0x7BEC, &z, 4);
    g_dl_getup_pending = 0;
    downed_log_state("got up");
}

/* ---------------------------------------------------------------- the puppet of the lying partner: lying (so the stock rescue can see it) and up again */
static void downed_puppet(unsigned rp, unsigned now)
{
    if (!rp || (int)(now - g_dl_pup_fix_t) < 400) return;
    int st = dl_state(&g_dl, g_rem_slot);
    unsigned act = rd32(rp + 0x1A54);
    if (st == DL_DOWN && !dl_is_down_act(act) && act != ACT_RESCUE) {
        g_dl_pup_fix_t = now;
        dl_change_action(rp, ACT_WAIT_RESCUE_A);   /* TODO live check: a type-2 puppet accepts 0x7A (the stock net handler refuses type 2 only for the be_rescued transition) */
        r2_log(DOWNED_LOG "puppet of slot %d shown lying (act %u -> 0x7A)", g_rem_slot, act);
    } else if (st != DL_DOWN && (act == ACT_WAIT_RESCUE_A || act == ACT_WAIT_RESCUE_B || act == ACT_BE_RESCUED || act == ACT_DYING_START)) {
        g_dl_pup_fix_t = now;
        dl_change_action(rp, 0);
        unsigned z = 0;
        wr(rp + 0x7BD8, &z, 4), wr(rp + 0x7BEC, &z, 4);
        r2_log(DOWNED_LOG "puppet of slot %d stands up again (act %u -> 0)", g_rem_slot, act);
    }
}

/* ---------------------------------------------------------------- my hold on the partner: the engine's own rescue action (observed) or the mod's (button + range) */
static void downed_hold(unsigned p, unsigned rp, unsigned now)
{
    int rem = g_rem_slot;
    unsigned act = rd32(p + 0x1A54);
    int rem_down = dl_state(&g_dl, rem) == DL_DOWN;
    if (g_dl_hold_mode != 1) {   /* observe the engine's pl_act_rescue (0x7C) on my human */
        if (act == ACT_RESCUE && rd32(p + 0x7BE8) == rp && rem_down) {
            if (!g_dl_engine_hold) {
                g_dl_engine_hold = 1, g_dl_engine_rescuer_seen = 1;
                dl_local_revive_begin(&g_dl, rem, now);
                r2_log(DOWNED_LOG "engine rescue began on the puppet of slot %d", rem);
            }
            dl_local_revive_keep(&g_dl, now);
        } else if (g_dl_engine_hold) {
            unsigned st = rd32(rp + 0x7BEC);
            g_dl_engine_hold = 0;
            if (st == 2) {
                dl_local_revive_done(&g_dl, now);
                r2_log(DOWNED_LOG "engine rescue SUCCEEDED (target state 2)");
            } else {
                dl_local_revive_cancel(&g_dl);
                r2_log(DOWNED_LOG "engine rescue ended without success (target state %u)", st);
            }
            unsigned z = 0;
            wr(rp + 0x7BEC, &z, 4), wr(rp + 0x7BD8, &z, 4), wr(p + 0x7BE8, &z, 4);
        }
    }
    if (g_dl_hold_mode != 0 && !g_dl_engine_hold && act != ACT_RESCUE) {   /* the mod's own hold: the rescue button (dl_hold_button: "Rescue" key / pad Y) held next to him (TODO live check: g_dl_range 170 vs the stock prompt's 180) */
        int holding = dl_hold_button(g_my_slot, p) && dl_state(&g_dl, g_my_slot) == DL_ALIVE;
        int in_range = rem_down && dl_dist_xz(p, rp) <= g_dl_range;
        static int was_holding = -1;
        if (rem_down && holding != was_holding && (int)(now - g_dl_hold_log_t) > 250) {   /* the rescue input as the mod sees it, at each change while the partner lies */
            char b[400];
            g_dl_hold_log_t = now, was_holding = holding;
            dl_input_text(g_my_slot, p, b, sizeof b);
            r2_log(DOWNED_LOG "mod hold input %s, distance %.0f (range %.0f) | %s", holding ? "DOWN" : "up", dl_dist_xz(p, rp), g_dl_range, b);
        }
        dl_local_hold(&g_dl, rem, holding, in_range, now);
    }
}

/* ---------------------------------------------------------------- live probe (test instances only): what the action ids 0x76..0x80 are
 * Armed by `net downed probe on`, which only works when the instance has REV2_DOWNED_PROBE=1 (default off; a player's game never sets it). It runs every frame from coop_game_frame,
 * with or without a net session (run A2 had none and the probe, then called only from downed_frame inside a session, logged nothing). Per slot 0 and 1 (local human, AI partner
 * or remote puppet alike):
 *   - "downed: probe act slot S ...: OLD -> NEW": every change of +0x1A54 (dl_watch_step: bounded per second and per arming, chain ids 0x76..0x80 never suppressed),
 *   - "downed: probe word slot S +XXXX: OLD -> NEW": changes of the rescue fields +7BD8 / +7BE8 / +7BEC, the kind +7920 and the engine's dying flag gm+0x8A0+slot,
 *   - once per slot and id: the first time that slot has that id in +0x1A54, one line "downed: probe id ..." with
 *   - the DTI class of the object at player+0x1A38 (the action machine the notes name; read like door.inc reads the set models' manager),
 *   - the class of the instance behind +0x1A38+0x30 (door.inc's layout for set models: UNVERIFIED for players),
 *   - the engine's numbers: sPlayer +0xCE4 (rescue hold, s), +0xCEC (revive hp), +0xCF0 (grace, s), +0x404 (low-hp threshold), and the absolute DAT_0124AD94 (UNVERIFIED: its type and meaning;
 *     printed as a dword and as a float).
 * Offsets come from the decompiled code (notes section 3); the values are what the game holds at that moment. The name per id is INFERRED (dl_probe_inferred_name). Reads only. */
static int g_dl_probe_on;              /* armed by `net downed probe on` (and REV2_DOWNED_PROBE=1 in the environment) */
static uint32_t g_dl_probe_mask[DL_WATCH_SLOTS];   /* per slot: ids already logged, bit (id - 0x76) */
static dl_watch_t g_dl_watch;          /* the change watch of +0x1A54 */
#define DL_PW_N 5
static const unsigned g_dl_pw_off[DL_PW_N] = {0x7BD8, 0x7BE8, 0x7BEC, 0x7920, 0};   /* 0 = the engine's dying flag gm+0x8A0+slot (byte) */
static uint32_t g_dl_pw[DL_WATCH_SLOTS][DL_PW_N], g_dl_pw_lines;
static int g_dl_pw_have[DL_WATCH_SLOTS];

static void downed_probe_rearm(void)
{
    memset(&g_dl_watch, 0, sizeof g_dl_watch);
    memset(g_dl_probe_mask, 0, sizeof g_dl_probe_mask);
    memset(g_dl_pw_have, 0, sizeof g_dl_pw_have);
    g_dl_pw_lines = 0;
}

static unsigned downed_dying_flag(int slot)
{
    unsigned gm = rd32(0x157AE00);
    unsigned char fl = 0;
    if (gm && slot >= 0 && slot < 2) r2_safe_read((void *)(gm + 0x8A0 + (unsigned)slot), &fl, 1);
    return fl;
}

static void downed_probe_line(unsigned p, int slot, unsigned id)
{
    static char mgr[4096], cur_cls[4096];   /* classes_name does not bound the chain: the buffers are big */
    unsigned cur = rd32(p + 0x1A38 + 0x30), sp = rd32(0x1567EAC);
    classes_name(p + 0x1A38, mgr, sizeof mgr - 1);
    if (cur > 0x10000 && cur < 0x7FFF0000u) classes_name(cur, cur_cls, sizeof cur_cls - 1);
    else snprintf(cur_cls, sizeof cur_cls, "no instance (%08x)", cur);
    if (sp) {
        r2_log(DOWNED_LOG "probe id 0x%X (%u, %s) first on slot %d: role %u kind %u hp %d/%d pstate %08x | action machine +1A38: %s | current instance (+1A38+30 = %08x): %s | "
               "sPlayer %08x: +CE4 %.3f s, +CEC %d, +CF0 %.3f s, +404 %.3f (as int %d) | DAT_0124AD94 (UNVERIFIED type) dword %08x float %.6f",
               id, id, dl_probe_inferred_name(id), slot, rd32(p + 0x790C) & 0xFF, rd32(p + 0x7920), (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A0C), rd32(p + 0x37B8), mgr, cur, cur_cls,
               sp, rdf(sp + 0xCE4), (int)rd32(sp + 0xCEC), rdf(sp + 0xCF0), rdf(sp + 0x404), (int)rdf(sp + 0x404), rd32(0x124AD94), rdf(0x124AD94));
    } else {
        r2_log(DOWNED_LOG "probe id 0x%X (%u, %s) first on slot %d: role %u kind %u hp %d/%d | action machine +1A38: %s | current instance: %s | no sPlayer (UNVERIFIED read of the values)",
               id, id, dl_probe_inferred_name(id), slot, rd32(p + 0x790C) & 0xFF, rd32(p + 0x7920), (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A0C), mgr, cur_cls);
    }
}

static void downed_probe(void)
{
    if (!g_dl_probe_on) return;
    unsigned now = GetTickCount(), gm = rd32(0x157AE00);
    for (int slot = 0; slot < DL_WATCH_SLOTS; slot++) {
        unsigned p = player_obj(slot), id = p ? rd32(p + 0x1A54) : 0;
        uint32_t prev, dropped;
        int r = dl_watch_step(&g_dl_watch, slot, p, id, now, &prev, &dropped);
        if (r == DL_W_GONE) {
            r2_log(DOWNED_LOG "probe act slot %d: object gone (last act %u)", slot, prev);
            g_dl_pw_have[slot] = 0;
        } else if (r == DL_W_NEW_OBJ || r == DL_W_CHANGE) {
            char was[16], dn[48] = "";
            if (prev == 0xFFFFFFFFu) snprintf(was, sizeof was, "-");
            else snprintf(was, sizeof was, "%u", prev);
            if (dropped) snprintf(dn, sizeof dn, " | %u changes not logged before this one", dropped);
            r2_log(DOWNED_LOG "probe act slot %d%s obj %08x kind %u role %u: %s -> %u (0x%X%s%s) hp %d/%d pstate %08x ctrl %08x f7904 %08x rescuer %08x target %08x state %u dying %u (%.1f s)%s",
                   slot, r == DL_W_NEW_OBJ ? " NEW" : "", p, rd32(p + 0x7920), rd32(p + 0x790C) & 0xFF, was, id, id, dl_probe_in_chain(id) ? " " : "", dl_probe_in_chain(id) ? dl_probe_inferred_name(id) : "",
                   (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A0C), rd32(p + 0x37B8), rd32(p + 0x78E8), rd32(p + 0x7904), rd32(p + 0x7BD8), rd32(p + 0x7BE8), rd32(p + 0x7BEC),
                   downed_dying_flag(slot), gm ? rdf(gm + 0x8A8 + 4 * (unsigned)slot) : 0.0f, dn);
        } else if (r == DL_W_CAP) {
            r2_log(DOWNED_LOG "probe: %u change lines this arming: from now on only the ids 0x76..0x80 are logged (`net downed probe reset` re-arms)", DL_WATCH_CAP);
        }
        if (p && dl_probe_first_seen(&g_dl_probe_mask[slot], id)) downed_probe_line(p, slot, id);
        /* the rescue words, the kind and the dying flag: rare changes, logged each time (2000 lines per arming at most) */
        if (!p) continue;
        for (int k = 0; k < DL_PW_N; k++) {
            uint32_t v = g_dl_pw_off[k] ? rd32(p + g_dl_pw_off[k]) : downed_dying_flag(slot);
            if (g_dl_pw_have[slot] && v != g_dl_pw[slot][k] && g_dl_pw_lines < 2000) {
                g_dl_pw_lines++;
                if (g_dl_pw_off[k]) r2_log(DOWNED_LOG "probe word slot %d +%X: %08x -> %08x (act %u hp %d)", slot, g_dl_pw_off[k], g_dl_pw[slot][k], v, id, (int)rd32(p + 0x1A08));
                else r2_log(DOWNED_LOG "probe word slot %d dying flag gm+%X: %u -> %u (act %u hp %d, timer %.1f s)", slot, 0x8A0 + slot, g_dl_pw[slot][k], v, id, (int)rd32(p + 0x1A08), gm ? rdf(gm + 0x8A8 + 4 * (unsigned)slot) : 0.0f);
            }
            g_dl_pw[slot][k] = v;
        }
        g_dl_pw_have[slot] = 1;
    }
}

/* net downed slots: the raw words of slots 0 and 1 in one answer (no net session needed: the lab scripts' stage snapshots; one console call instead of 30) */
static int downed_slots_text(char *out, int size)
{
    unsigned sp = rd32(0x1567EAC), gm = rd32(0x157AE00);
    int o = snprintf(out, size, "slots: sPlayer %08x gm %08x gm+904 %08x DAT_0124AD94 %08x (float %.4f) probe %s masks %03x %03x lines %u changes %u", sp, gm, gm ? rd32(gm + 0x904) : 0, rd32(0x124AD94), rdf(0x124AD94),
                     g_dl_probe_on ? "armed" : "off", g_dl_probe_mask[0], g_dl_probe_mask[1], g_dl_watch.lines, g_dl_watch.changes);
    for (int s = 0; s < 2 && o < size - 1; s++) {
        unsigned p = player_obj(s);
        if (!p) {
            o += snprintf(out + o, size - o, "\n  slot %d: none", s);
            continue;
        }
        o += snprintf(out + o, size - o, "\n  slot %d %08x: hp %d/%d act %u kind %u role %u pstate %08x ctrl %08x f78f8 %08x f7900 %08x f7904 %08x rescuer %08x target %08x state %u f7bf0 %08x f1a68 %08x pos (%.1f,%.1f,%.1f) dying %u (%.1f s)",
                      s, p, (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A0C), rd32(p + 0x1A54), rd32(p + 0x7920), rd32(p + 0x790C) & 0xFF, rd32(p + 0x37B8), rd32(p + 0x78E8), rd32(p + 0x78F8), rd32(p + 0x7900),
                      rd32(p + 0x7904), rd32(p + 0x7BD8), rd32(p + 0x7BE8), rd32(p + 0x7BEC), rd32(p + 0x7BF0), rd32(p + 0x1A68), rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), downed_dying_flag(s),
                      gm ? rdf(gm + 0x8A8 + 4 * (unsigned)s) : 0.0f);
    }
    return o;
}

/* net downed pkt <state 0..3> <sender slot> <target slot>: hands the engine's Raid rescue handler FUN_009DF820 a synthesized packet (TEST instances only: REV2_DOWNED_PKT=1, default
 * off). Checks first: the code bytes of the handler, its jump table and its `ret 8` are those of build 3658995 (the stub has decrypted .text), both slots hold valid objects, the slots
 * differ. Calls it as stdcall(seat, packet) on the game thread with seat = gm+0x904 + 1 (any value but gm+0x904 passes the "my own packet" test). Prints the fields before and after
 * and what the static reading expects (dl_pkt_expect). Single-player use: Barry (0) raises the lying AI Natalia (1): `pkt 0 0 1` (begin -> 0x7D), then `pkt 2 0 1` (success). */
static void downed_pkt_command(const char *a, char *out, int size)
{
    char env[16];
    if (!(r2_env("REV2_DOWNED_PKT", env, sizeof env) && atoi(env) != 0)) {
        snprintf(out, size, "pkt: not allowed here: start a TEST instance with REV2_DOWNED_PKT=1 (a player's game never sets it)");
        return;
    }
    int st = -1, from = -1, to = -1;
    if (sscanf(a, "%d %d %d", &st, &from, &to) != 3 || !dl_pkt_args_ok(st, from, to)) {
        snprintf(out, size, "usage: net downed pkt <state 0 begin|1 rejected|2 success|3 cancel> <sender slot 0|1> <target slot 0|1> (different slots)");
        return;
    }
    if (!lt_on_game_thread()) {
        char c[100];
        snprintf(c, sizeof c, "net downed pkt %d %d %d", st, from, to);
        lt_post(c, out, size);
        return;
    }
    uint8_t entry[15] = {0}, ret[3] = {0};
    uint32_t table[4] = {0};
    if (!r2_safe_read((void *)DL_PKT_FN, entry, sizeof entry) || !r2_safe_read((void *)DL_PKT_TABLE, table, sizeof table) || !r2_safe_read((void *)DL_PKT_RET, ret, sizeof ret) || !dl_pkt_code_ok(entry, table, ret)) {
        snprintf(out, size, "pkt: REFUSED: the code at %08x (entry %02x %02x %02x %02x %02x, table %08x) is not the handler of build 3658995", DL_PKT_FN, entry[0], entry[1], entry[2], entry[3], entry[4], table[0]);
        return;
    }
    unsigned gm = rd32(0x157AE00), ps = player_obj(from), pt = player_obj(to);
    if (!gm || !ps || !pt) {
        snprintf(out, size, "pkt: REFUSED: no game manager or no player in slot %d / %d (gm %08x, %08x, %08x)", from, to, gm, ps, pt);
        return;
    }
    uint8_t pkt[8];
    dl_pkt_build(pkt, (unsigned)from, (unsigned)to, (unsigned)st);
    unsigned seat = rd32(gm + 0x904) + 1u;
    int o = snprintf(out, size, "pkt state %d sender slot %d target slot %d, packet %02x %02x %02x %02x %02x %02x %02x %02x, seat %08x (gm+904 %08x)\n  expect: %s\n  before: target act %u kind %u hp %d/%d rescuer %08x state %u | sender act %u kind %u target %08x",
                     st, from, to, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], seat, rd32(gm + 0x904), dl_pkt_expect((unsigned)st), rd32(pt + 0x1A54), rd32(pt + 0x7920), (int)rd32(pt + 0x1A08),
                     (int)rd32(pt + 0x1A0C), rd32(pt + 0x7BD8), rd32(pt + 0x7BEC), rd32(ps + 0x1A54), rd32(ps + 0x7920), rd32(ps + 0x7BE8));
    unsigned args[2] = {seat, (unsigned)(uintptr_t)pkt};
    do_call(1, DL_PKT_FN, args, 2);   /* stdcall, 2 arguments, callee cleans (ret 8) */
    o += snprintf(out + o, size - o, "\n  after:  target act %u kind %u hp %d/%d rescuer %08x state %u | sender act %u kind %u target %08x", rd32(pt + 0x1A54), rd32(pt + 0x7920), (int)rd32(pt + 0x1A08), (int)rd32(pt + 0x1A0C),
                  rd32(pt + 0x7BD8), rd32(pt + 0x7BEC), rd32(ps + 0x1A54), rd32(ps + 0x7920), rd32(ps + 0x7BE8));
    r2_log(DOWNED_LOG "pkt test: %s", out);
}

/* ---------------------------------------------------------------- the pass */
static void downed_apply_mode(void)
{
    int want = g_dl_mode;
    cp_session ss;
    memset(&ss, 0, sizeof ss);
    if (want < 0) {
        char env[16];
        if (g_cs && cs_active(g_cs)) cs_session_get(g_cs, &ss);
        want = (ss.flags & CP_SESS_DOWNED) != 0;
        if (!want && r2_env("REV2_DOWNED", env, sizeof env)) want = atoi(env) != 0;
    }
    if (want != g_dl_on) {
        g_dl_on = want;
        r2_log(DOWNED_LOG "the rule is now %s (session flags %02x, mode %d)", want ? "ON" : "off", ss.flags, g_dl_mode);
        if (want) {
            if (!g_w_installed) w_install();   /* the damage gate rides on the world module's player SubHP detour */
            if (!g_w_installed) r2_log(DOWNED_LOG "WARNING: the world hooks are not installed: no damage gate, the rule only follows the engine's own wounded chain");
            dl_kbfix_apply(1);                 /* a role-1 human on the keyboard can hold the stock rescue (left on: for slot 0 it writes the same byte as the stock code) */
            dl_input_code_ok();                /* one log line: the rescue input reader is on or not */
        }
        g_dl_ready = 0;
    }
}

static void downed_frame(unsigned now)
{
    downed_apply_mode();   /* (the probe runs from coop_game_frame, session or not) */
    if (!g_dl_on || !g_cs || !cs_active(g_cs) || g_my_slot < 0 || g_rem_slot < 0 || g_my_slot > 1 || g_rem_slot > 1) return;
    unsigned p = player_obj(g_my_slot), rp = player_obj(g_rem_slot);
    if (!p || !rp || p == rp) return;
    cp_session ss;
    cs_session_get(g_cs, &ss);
    dl_cfg c;
    dl_cfg_from_flags(&c, ss.flags | CP_SESS_DOWNED);
    c.enabled = 1;
    if (g_dl_bleed_override != 0xFFFFFFFFu) c.bleed_ms = g_dl_bleed_override;
    if (g_dl_revive_override) c.revive_ms = g_dl_revive_override;
    unsigned key = cs_epoch(g_cs) + 1000u * g_loads_seen;
    if (!g_dl_ready) {
        dl_init(&g_dl, &c, g_my_slot, 2, now);
        g_dl_ready = 1, g_dl_epoch_key = key;
        g_dl_getup_pending = 0, g_dl_engine_hold = 0;
        downed_log_state("started");
    } else {
        g_dl.cfg = c;
        if (key != g_dl_epoch_key) {   /* a level load / retry: everybody is up again */
            g_dl_epoch_key = key;
            dl_reset(&g_dl, now);
            g_dl_getup_pending = 0, g_dl_engine_hold = 0;
            downed_log_state("level load: reset");
        }
    }
    int rem_connected = cs_remote_pid(g_cs) >= 0;
    dl_set_present(&g_dl, g_rem_slot, rem_connected, now);

    /* 1. follow the engine for my own human */
    int hp = (int)rd32(p + 0x1A08);
    unsigned act = rd32(p + 0x1A54);
    int st = dl_state(&g_dl, g_my_slot);
    dl_follow_in fi = {act, hp, dl_dead_reasons(p), g_dl_getup_pending, now, g_dl_getup_t, g_dl_down_at_tick, g_dl_act_fix_t};
    switch (dl_follow(&g_dl, &fi)) {   /* downed_logic.h: the decisions (the DONE / be_rescued order of run C 2026-11-07 is a test case there) */
    case DL_F_DEAD:
        dl_local_dead(&g_dl, now);
        downed_log_state(st == DL_DOWN ? "my lying character died (engine)" : "my character is dead (engine)");
        break;
    case DL_F_ENGINE_DOWN:   /* the engine's own wounded chain (support role, a scripted dying_start); never 0x7D, never right after a revive */
        if (dl_local_lethal(&g_dl, now)) r2_log(DOWNED_LOG "the engine put my character into its wounded chain (act %u, hp %d): DOWN", act, hp);
        else {
            r2_log(DOWNED_LOG "the engine put my character into its wounded chain (act %u) but nobody is up: dying for real", act);
            dl_set_hp(p, 0);
            dl_local_dead(&g_dl, now);
        }
        break;
    case DL_F_ENGINE_UP:
        dl_local_up(&g_dl, now);
        downed_log_state("the engine stood my character up (its own rescue / a heal)");
        break;
    case DL_F_BACK_TO_CHAIN:   /* something pulled my lying human out of the chain: put it back */
        g_dl_act_fix_t = now;
        dl_change_action(p, ACT_WAIT_RESCUE_A);
        if (hp != 1) dl_set_hp(p, 1);
        r2_log(DOWNED_LOG "my lying character left the wounded chain (act %u): back to 0x7A", act);
        break;
    case DL_F_STUCK_UP:   /* up for the rule but still in wait_rescue / be_rescued with hp > 1 (a completion that did not take, run C 2026-11-07 06:53): stand up */
        g_dl_act_fix_t = now;
        dl_change_action(p, 0);
        r2_log(DOWNED_LOG "my character is up for the rule but still lying (act %u, hp %d): stood up (idle)", act, hp);
        break;
    default:
        break;
    }

    /* 2. time, then the outputs, the hold, the get-up, the puppet */
    dl_tick(&g_dl, now);
    downed_exec(p, rp, now);
    downed_hold(p, rp, now);
    downed_exec(p, rp, now);
    downed_getup_step(p, now);
    {   /* the MAIN role's stock dying gauge / timer while the rule says DOWN (downed_logic.h DL_GO_DOWN_MS: run C 2026-11-07 08:22, Barry died 67 ms after entering 0x7A) */
        int ng = 0;
        float nt = 0;
        int g0 = (int)rd32(p + 0x6F54);
        float t0 = rdf(p + 0x6F1C);
        int mk = dl_main_dying_keep((int)(rd32(p + 0x790C) & 0xFF), dl_state(&g_dl, g_my_slot) == DL_DOWN, rd32(p + 0x1A54), g0, t0, &ng, &nt);
        if (mk & 1) wr(p + 0x6F54, &ng, 4);
        if (mk & 2) wr(p + 0x6F1C, &nt, 4);
        if (mk && g_dl_keep_log < 12) g_dl_keep_log++, r2_log(DOWNED_LOG "main role lying: stock dying gauge +6F54 %d -> %d, timer +6F1C %.2f -> %.2f (g_sPlayer+0x54C %.2f)", g0, (mk & 1) ? ng : g0, t0, (mk & 2) ? nt : t0, rdf(rd32(0x1567EAC) + 0x54C));
    }
    downed_puppet(rp, now);

    /* 3. the banner strips (2 Hz) */
    if ((int)(now - g_dl_banner_t) > 500) {
        g_dl_banner_t = now;
        char sec[16];
        if (dl_state(&g_dl, g_my_slot) == DL_DOWN) {
            int32_t l = dl_bleed_left(&g_dl, g_my_slot);
            snprintf(sec, sizeof sec, "%d", l == 0x7FFFFFFF ? 99 : l / 1000);
            menu_banner(MB_DOWN_SELF, sec, 900);
        } else if (dl_state(&g_dl, g_rem_slot) == DL_DOWN && dl_state(&g_dl, g_my_slot) == DL_ALIVE) {
            int32_t l = dl_bleed_left(&g_dl, g_rem_slot);
            snprintf(sec, sizeof sec, "%d", l == 0x7FFFFFFF ? 99 : l / 1000);
            menu_banner(MB_DOWN_PARTNER, sec, 900);
        }
    }
}

/* ---------------------------------------------------------------- console: net downed ... */
static void downed_command(const char *rest, char *out, int size)
{
    unsigned p = player_obj(g_my_slot), rp = player_obj(g_rem_slot);
    if (!strncmp(rest, "on", 2) && (rest[2] == 0 || rest[2] == ' ')) g_dl_mode = 1;
    else if (!strncmp(rest, "off", 3)) g_dl_mode = 0;
    else if (!strncmp(rest, "auto", 4)) g_dl_mode = -1;
    else if (!strncmp(rest, "bleed ", 6)) g_dl_bleed_override = (uint32_t)atoi(rest + 6) * 1000u;   /* seconds, 0 = never */
    else if (!strncmp(rest, "bleed", 5)) g_dl_bleed_override = 0xFFFFFFFFu;
    else if (!strncmp(rest, "hold ", 5)) g_dl_revive_override = (uint32_t)atoi(rest + 5);   /* ms of the mod-driven hold */
    else if (!strncmp(rest, "range ", 6)) g_dl_range = (float)atof(rest + 6);
    else if (!strncmp(rest, "pct ", 4)) g_dl_revive_pct = atoi(rest + 4) < 1 ? 1 : atoi(rest + 4) > 100 ? 100 : atoi(rest + 4);
    else if (!strncmp(rest, "mode ", 5)) g_dl_hold_mode = !strncmp(rest + 5, "engine", 6) ? 0 : !strncmp(rest + 5, "mod", 3) ? 1 : 2;
    else if (!strncmp(rest, "hit ", 4)) {   /* net downed hit <amount>: the damage path of an enemy hit on my character (goes through the detoured player SubHP) */
        if (!lt_on_game_thread()) {
            char c[100];
            snprintf(c, sizeof c, "net downed %s", rest);
            lt_post(c, out, size);
            return;
        }
        int amt = atoi(rest + 4);
        if (p && amt > 0) {
            unsigned vt = rd32(p), fn = rd32(vt + 0xB0), a2[2] = {p, (unsigned)amt};
            if (fn > 0x400000 && fn < 0x1400000) do_call(2, fn, a2, 2);
        }
    } else if (!strncmp(rest, "probe", 5)) {   /* net downed probe [on|off|reset]: the live probe of the action ids 0x76..0x80 (test instances only, see downed_probe) */
        char env[16];
        const char *a = rest + 5;
        while (*a == ' ') a++;
        if (!(r2_env("REV2_DOWNED_PROBE", env, sizeof env) && atoi(env) != 0)) {
            snprintf(out, size, "probe: off, and not allowed here: start a TEST instance with REV2_DOWNED_PROBE=1 (a player's game never sets it)");
            return;
        }
        if (!strncmp(a, "on", 2)) {
            if (!g_dl_probe_on) downed_probe_rearm();   /* a fresh arming: baseline lines for both slots at the next frame */
            g_dl_probe_on = 1;
        } else if (!strncmp(a, "off", 3)) g_dl_probe_on = 0;
        else if (!strncmp(a, "reset", 5)) downed_probe_rearm();
        snprintf(out, size, "probe: %s, ids logged mask slot 0 %03x slot 1 %03x (bit 0 = 0x76 ... bit 10 = 0x80), change lines %u of %u (changes %u); see the log lines \"downed: probe\"", g_dl_probe_on ? "armed" : "off",
                 g_dl_probe_mask[0], g_dl_probe_mask[1], g_dl_watch.lines, DL_WATCH_CAP, g_dl_watch.changes);
        return;
    } else if (!strncmp(rest, "slots", 5)) {   /* net downed slots: raw words of both slots (read only, any thread) */
        downed_slots_text(out, size);
        return;
    } else if (!strncmp(rest, "pkt", 3)) {   /* net downed pkt <state> <sender> <target>: FUN_009DF820 with a synthesized packet (REV2_DOWNED_PKT=1 only) */
        downed_pkt_command(rest + 3, out, size);
        return;
    } else if (!strncmp(rest, "input", 5) || !strncmp(rest, "kbfix", 5)) {   /* net downed input [slot]: the rescue input of a slot (no session needed; default my slot, else 0) |
                                                                                 net downed kbfix on|off: the keyboard rescue fix (on by itself when the rule turns on) */
        if (!lt_on_game_thread()) {
            char c[100];
            snprintf(c, sizeof c, "net downed %s", rest);
            lt_post(c, out, size);
            return;
        }
        if (!strncmp(rest, "kbfix", 5)) {
            const char *a = rest + 5;
            while (*a == ' ') a++;
            if (!strncmp(a, "on", 2)) dl_kbfix_apply(1);
            else if (!strncmp(a, "off", 3)) dl_kbfix_apply(0);
            uint8_t cur[16] = {0};
            r2_safe_read((void *)DL_KBFIX_AT, cur, 16);
            snprintf(out, size, "kbfix %d (code at %08x: %s)", g_dl_kbfix, DL_KBFIX_AT, dl_kbfix_state(cur) == 2 ? "patched" : dl_kbfix_state(cur) == 1 ? "stock" : "unexpected");
            return;
        }
        int slot = g_my_slot >= 0 ? g_my_slot : 0;
        if (rest[5] == ' ') slot = atoi(rest + 6);
        unsigned q = player_obj(slot), sp = rd32(0x1567EAC);
        int o = dl_input_text(slot, q, out, size);
        if (q && sp && o < size) o += snprintf(out + o, size - o, "\n  slot %d: act %u hp %d kind %u prompt target(+7BE8) %08x | sPlayer prompt range +CF4 %.1f angle +CF8 %.1f hold +CE4 %.2f s", slot, rd32(q + 0x1A54), (int)rd32(q + 0x1A08),
                                       rd32(q + 0x7920), rd32(q + 0x7BE8), rdf(sp + 0xCF4), rdf(sp + 0xCF8), rdf(sp + 0xCE4));
        return;
    } else if (!strncmp(rest, "engine", 6)) {   /* the engine's numbers for the live checks (section 3 of docs/downed-revive.md) */
        if (!lt_on_game_thread()) {   /* the rescue input line calls engine functions */
            lt_post("net downed engine", out, size);
            return;
        }
        unsigned sp = rd32(0x1567EAC), gm = rd32(0x157AE00);
        int o = snprintf(out, size, "engine: sPlayer %08x: +0xCE4 hold %.3f s, +0xCEC revive hp %d, +0xCF0 grace %.3f, +0x404 low-hp %.1f; gm dying flags", sp, sp ? rdf(sp + 0xCE4) : 0.0f, sp ? (int)rd32(sp + 0xCEC) : 0, sp ? rdf(sp + 0xCF0) : 0.0f, sp ? rdf(sp + 0x404) : 0.0f);
        for (int i = 0; i < 2 && gm; i++) {
            unsigned char fl = 0;
            r2_safe_read((void *)(gm + 0x8A0 + i), &fl, 1);
            o += snprintf(out + o, size - o, " [slot %d: %u, %.1f s]", i, fl, rdf(gm + 0x8A8 + 4 * i));
        }
        for (int i = 0; i < 2; i++) {
            unsigned q = i ? rp : p;
            if (q) o += snprintf(out + o, size - o, "\n  %s slot %d: hp %d/%d act %u kind %u role %u pstate %08x ctrl %08x f78f8 %08x f7900 %08x f7904 %08x rescuer(+7BD8) %08x target(+7BE8) %08x state(+7BEC) %u", i ? "partner" : "me", i ? g_rem_slot : g_my_slot, (int)rd32(q + 0x1A08), (int)rd32(q + 0x1A0C), rd32(q + 0x1A54), rd32(q + 0x7920), rd32(q + 0x790C), rd32(q + 0x37B8), rd32(q + 0x78E8), rd32(q + 0x78F8), rd32(q + 0x7900), rd32(q + 0x7904), rd32(q + 0x7BD8), rd32(q + 0x7BE8), rd32(q + 0x7BEC));
        }
        if (p) {
            o += snprintf(out + o, size - o, "\n  hold button of my role: %d, distance to the partner %.0f\n  ", dl_hold_button(g_my_slot, p), rp ? dl_dist_xz(p, rp) : -1.0f);
            if (o < size) dl_input_text(g_my_slot, p, out + o, size - o);
        }
        return;
    }
    int o = snprintf(out, size, "downed rule: %s (mode %d), hold mode %d, range %.0f, revive hp %d%%, bleed override %d, hold override %u\n", g_dl_on ? "ON" : "off", g_dl_mode, g_dl_hold_mode, g_dl_range, g_dl_revive_pct,
                     g_dl_bleed_override == 0xFFFFFFFFu ? -1 : (int)(g_dl_bleed_override / 1000), g_dl_revive_override);
    if (g_dl_ready) o += dl_status(&g_dl, out + o, size - o);
    snprintf(out + o, size - o, "\nhits absorbed %u (on a lying human %u), engine rescue seen %d", g_dl_hits_absorbed, g_dl_hits_down, g_dl_engine_rescuer_seen);
}
