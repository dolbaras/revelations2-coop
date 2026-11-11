/* aim.inc: LAB ONLY (test instances, console). `net aim ...`: the shot vector of THIS PC's human points at a chosen target (an enemy joint or a world point).
 * Why and how: aim_logic.h. Nothing is patched until a console `net aim at|pos|watch` runs; after that the call site 0x6D43D3 goes through h_aim_shot, which only
 * calls the stock FUN_006EF600 while the aid is off (and for every other character). Included at the end of coop3.c after ach.inc (rd32/rdf/wr/player_obj,
 * w_find_active / layout_of / SENEMY_CELL from world.inc). Notes: notes\aim.md. */
#include "aim_logic.h"
#define AIM_LOG "aim: "

typedef void(__attribute__((thiscall)) *aim_shot_t)(void *player, void *weapon, float *start, float *dir, int flag);

enum { AIM_OFF = 0, AIM_ENEMY = 1, AIM_POS = 2 };
static struct {
    int installed, mode, watch, joint, n_log;   /* joint: -1 = the highest joint ("top"), -2 = no joint (enemy position + dy), else the joint id */
    int a; unsigned b, c;
    float pos[3], dy;
    unsigned en, shots, changed, my_calls, other_calls, last_shot_t;
    float last_start[3], last_old[3], last_new[3], last_tgt[3], last_turn, last_dist;
    int hp_seen, hp_at_shot;
} g_aim = {.joint = -1, .dy = -6.0f};

/* the world position of joint id j of a model (0 when it has none) */
static int aim_joint_pos(unsigned obj, int j, float out[3])
{
    unsigned map = rd32(obj + AIM_JOINT_MAP), arr = rd32(obj + AIM_JOINT_ARR);
    if (!map || !arr || j < 0 || j > 255) return 0;
    unsigned char idx = 0xFF;
    if (!r2_safe_read((void *)(map + (unsigned)j), &idx, 1) || idx == 0xFF) return 0;
    float p[3];
    if (!r2_safe_read((void *)(arr + idx * AIM_JOINT_SIZE + AIM_JOINT_POS), p, sizeof p)) return 0;
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
    return 1;
}

/* the highest joint of a model: its id (or -1) and position */
static int aim_top_joint(unsigned obj, float out[3])
{
    float pos[256][3];
    int ids[256], n = 0;
    for (int j = 0; j < 256; j++)
        if (aim_joint_pos(obj, j, pos[n])) ids[n++] = j;
    int k = aim_pick_top((const float(*)[3])pos, n);
    if (k < 0) return -1;
    out[0] = pos[k][0], out[1] = pos[k][1], out[2] = pos[k][2];
    return ids[k];
}

/* the current target point; 0 = none */
static int aim_target(float t[3])
{
    if (g_aim.mode == AIM_POS) {
        t[0] = g_aim.pos[0], t[1] = g_aim.pos[1], t[2] = g_aim.pos[2];
        return 1;
    }
    if (g_aim.mode != AIM_ENEMY || !g_aim.en) return 0;
    unsigned en = g_aim.en;
    if (g_aim.joint >= 0 && aim_joint_pos(en, g_aim.joint, t)) return 1;
    if (g_aim.joint == -1 && aim_top_joint(en, t) >= 0) { t[1] += g_aim.dy; return 1; }
    t[0] = rdf(en + 0x40), t[1] = rdf(en + 0x44) + (g_aim.joint == -2 ? g_aim.dy : 100.0f), t[2] = rdf(en + 0x48);
    return isfinite(t[0]) && isfinite(t[1]) && isfinite(t[2]);
}

static __attribute__((thiscall)) void h_aim_shot(void *player, void *weapon, float *start, float *dir, int flag)
{
    ((aim_shot_t)AIM_FN)(player, weapon, start, dir, flag);
    unsigned me = player_obj(g_my_slot >= 0 ? g_my_slot : 0);
    if (!me || (unsigned)player != me) { g_aim.other_calls++; return; }
    g_aim.my_calls++;
    if (g_aim.mode == AIM_OFF) return;
    float t[3];
    if (!aim_target(t)) return;
    float old[3] = {dir[0], dir[1], dir[2]}, turn = 0;
    int ch = 0;
    if (g_aim.watch) {
        float v[3] = {t[0] - start[0], t[1] - start[1], t[2] - start[2]};
        float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), l = sqrtf(old[0] * old[0] + old[1] * old[1] + old[2] * old[2]);
        if (d > 1 && l > 1e-6f) { float c = (v[0] * old[0] + v[1] * old[1] + v[2] * old[2]) / (d * l); turn = acosf(c > 1 ? 1 : c < -1 ? -1 : c) * 57.2957795f; }
    } else {
        ch = aim_redirect(start, dir, t, &turn);
    }
    g_aim.shots++;
    if (ch) g_aim.changed++;
    g_aim.last_shot_t = GetTickCount();
    g_aim.hp_at_shot = g_aim.en ? (int)rd32(g_aim.en + 0x1A08) : 0;
    for (int i = 0; i < 3; i++) g_aim.last_start[i] = start[i], g_aim.last_old[i] = old[i], g_aim.last_new[i] = dir[i], g_aim.last_tgt[i] = t[i];
    g_aim.last_turn = turn;
    g_aim.last_dist = sqrtf((t[0] - start[0]) * (t[0] - start[0]) + (t[1] - start[1]) * (t[1] - start[1]) + (t[2] - start[2]) * (t[2] - start[2]));
    if (g_aim.n_log++ < 60)
        r2_log(AIM_LOG "shot %u of my human %08x (weapon %08x, owner %08x, flag %d): start (%.0f,%.0f,%.0f), vector (%.2f,%.2f,%.2f) |%.0f| %s (%.2f,%.2f,%.2f), %.1f deg %s, target (%.0f,%.0f,%.0f) %.0f units away, target hp %d",
               g_aim.shots, me, (unsigned)weapon, weapon ? rd32((unsigned)weapon + 0x19C0) : 0, flag & 0xFF, start[0], start[1], start[2], old[0], old[1], old[2], sqrtf(old[0] * old[0] + old[1] * old[1] + old[2] * old[2]),
               ch ? "->" : "kept", dir[0], dir[1], dir[2], turn, g_aim.watch ? "off the target (watch only)" : "turned", t[0], t[1], t[2], g_aim.last_dist, g_aim.hp_at_shot);
}

static int aim_install(void)
{
    if (g_aim.installed) return 1;
    /* movaps [esp+0xF0],xmm0 ; call FUN_006EF600 ; mov ecx,[ebx+0x20] */
    static const unsigned char want[16] = {0x0F, 0x29, 0x84, 0x24, 0xF0, 0x00, 0x00, 0x00, 0xE8, 0x28, 0xB2, 0x01, 0x00, 0x8B, 0x4B, 0x20};
    unsigned char c[16];
    if (!r2_safe_read((void *)(AIM_SITE - 8), c, sizeof c) || memcmp(c, want, sizeof c) != 0 || (unsigned)(AIM_SITE + 5 + *(int *)(c + 9)) != AIM_FN) {
        r2_log(AIM_LOG "the bytes at %08x are not the expected ones, NOT installed", AIM_SITE);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect((void *)AIM_SITE, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int *)(AIM_SITE + 1) = (int)((unsigned)h_aim_shot - (AIM_SITE + 5));
    VirtualProtect((void *)AIM_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)AIM_SITE, 5);
    g_aim.installed = 1;
    r2_log(AIM_LOG "shot call at %08x now goes through the lab aim (stock FUN_006EF600 first; only this PC's human is changed while `net aim` is on)", AIM_SITE);
    return 1;
}

/* ---- lab: `net aim cam <a> <b> <c> [joint <id>|base] [dy <u>] [right <u>] [eye <u>] | cam off`: every frame the camera of THIS PC's human is turned so that
 * its crosshair sits on the enemy's joint (default joint 0 = the chest) - a handgun shoots at the crosshair's aim point (aim_logic.h). Writes uCameraCtrl
 * (player+0x4780) +0x23A8..+0x23BC. Fire with `net ach fight <a> <b> <c> <ms> <range> 1 0` (LT held). Game thread, test instances only. */
static struct { int on, a, joint, n_log; unsigned b, c, frames; float dy, right, eye, yaw, pitch; } g_ac = {.right = AIM_CAM_RIGHT, .eye = AIM_CAM_EYE};
static void aim_cam_retarget(int a, unsigned b, unsigned c) { if (g_ac.on) g_ac.a = a, g_ac.b = b, g_ac.c = c; }
static void ac_tick(void)
{
    if (!g_ac.on) return;
    unsigned me = player_obj(g_my_slot >= 0 ? g_my_slot : 0), en = w_find_active(SENEMY_CELL, g_ac.a, g_ac.b, g_ac.c);
    if (!me || !en) return;
    unsigned ctrl = rd32(me + 0x4780);
    if (!ctrl) return;
    float t[3], h[3] = {rdf(me + 0x40), rdf(me + 0x44), rdf(me + 0x48)};
    if (g_ac.joint < 0 || !aim_joint_pos(en, g_ac.joint, t)) t[0] = rdf(en + 0x40), t[1] = rdf(en + 0x44) + 100.0f, t[2] = rdf(en + 0x48);
    t[1] += g_ac.dy;
    float yaw, pitch;
    if (!aim_cam_solve(h, t, g_ac.right, g_ac.eye, AIM_CAM_PITCH_RAD, &yaw, &pitch)) return;
    wr(ctrl + 0x23AC, &yaw, 4), wr(ctrl + 0x23B4, &yaw, 4), wr(ctrl + 0x23B8, &yaw, 4);
    wr(ctrl + 0x23A8, &pitch, 4), wr(ctrl + 0x23B0, &pitch, 4), wr(ctrl + 0x23BC, &pitch, 4);
    g_ac.yaw = yaw, g_ac.pitch = pitch;
    if ((g_ac.frames++ % 120) == 0 && g_ac.n_log++ < 40)
        r2_log(AIM_LOG "cam (%d,%u,%u) %08x: target (%.0f,%.0f,%.0f) from (%.0f,%.0f,%.0f): camera yaw %.3f pitch %.3f", g_ac.a, g_ac.b, g_ac.c, en, t[0], t[1], t[2], h[0], h[1], h[2], yaw, pitch);
}

/* ---- lab: `net aim grap <a> <b> <c> [auto <hexbuttons>|auto game|auto game+stock|auto stock] | grap off`: the physical-attack ("grapple") offer of one enemy for THIS
 * PC's human, sampled every 6 frames and logged on change (grapple_logic.h has the stock rule): the enemy's action and motion (+0x4A4), +0x3010 (6 while staggered),
 * +0x37B8 bit 30 (alerted), the layer-0 motion flags 12/13 (+0x5B0), the offer entries +0x7570/+0x7574 (stagger) and +0x7578/+0x757C (surprise), the stock checks
 * FUN_00607040 (stagger offer), FUN_00607A50 (surprise offer) and the class check vf 0x434 called for my human's player index (+0x790C, what the entry evaluator passes), my
 * action and my grapple target +0x791C. auto: the first time the stagger check passes (once per offer) the offer is taken:
 *   <hex>       the pad of my human presses those XInput buttons for 250 ms (as before);
 *   game        the button the game itself wants for this entry (combat2): FUN_0040AD80(entry, index) -> the sPad bit (table 0x124F020) -> the XInput pad (FUN_00C11E20);
 *   game+stock  as game; when my action is not a grapple 700 ms later, the entry's stock press handler FUN_0040ACB0(entry, index) is called (= what the button does);
 *   stock       the stock press handler at once (no pad).
 * The press handler only runs the entry's action (LAB_00603D80 -> FUN_00714970, the stock player rule FUN_00703300 again). Game thread, test instances only. */
enum { GW_NONE = 0, GW_BUTTONS, GW_GAME, GW_GAME_STOCK, GW_STOCK };
static struct { int on, a, n_log, pressing, mode, fb_armed; unsigned b, c, en, btn, t_press, frames, s[16], offers, presses, stock_presses, entry, idx; gp_xpad_t x; } g_gw;
static void gw_pad(unsigned btn)
{
    ach_gamepad_t g;
    memset(&g, 0, sizeof g);
    g.wButtons = (WORD)btn;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, btn != 0, btn ? &g : NULL);
    else testmode_pad_set(0, btn != 0, btn ? &g : NULL);
}
static void gw_xpad(const gp_xpad_t *x)
{
    ach_gamepad_t g;
    memset(&g, 0, sizeof g);
    int on = x && (x->buttons || x->lt || x->rt);
    if (on) g.wButtons = x->buttons, g.bLeftTrigger = x->lt, g.bRightTrigger = x->rt;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, on, on ? &g : NULL);
    else testmode_pad_set(0, on, on ? &g : NULL);
}
static int gw_is_grapple(unsigned act) { return act >= GP_ACT_GRAPPLE_FIRST && act <= GP_ACT_GRAPPLE_LAST; }
static void gw_stock_press(unsigned me, const char *why)
{
    if (!g_gw.entry) return;
    unsigned a[2] = {g_gw.entry, g_gw.idx};
    do_call(2, 0x40ACB0, a, 2);
    g_gw.stock_presses++;
    r2_log(AIM_LOG "grap: %s: the entry's stock press handler FUN_0040ACB0(%08x, %u) called: my action now %u, target-is-it %u", why, g_gw.entry, g_gw.idx, rd32(me + 0x1A54),
           rd32(me + 0x791C) == g_gw.en);
}
static void gw_tick(void)
{
    if (!g_gw.on) return;
    unsigned now = GetTickCount();
    int me_slot = g_my_slot >= 0 ? g_my_slot : 0;
    unsigned me = player_obj(me_slot);
    if (g_gw.pressing && (int)(now - g_gw.t_press) >= 250) {
        if (g_gw.mode == GW_BUTTONS) gw_pad(0); else gw_xpad(NULL);
        g_gw.pressing = 0;
    }
    if (g_gw.fb_armed && me && (int)(now - g_gw.t_press) >= 700) {
        g_gw.fb_armed = 0;
        unsigned act = rd32(me + 0x1A54);
        if (!gw_is_grapple(act)) gw_stock_press(me, "the game button did not start a grapple");
        else r2_log(AIM_LOG "grap: the game button started grapple action %u (no stock press needed)", act);
    }
    if (++g_gw.frames % 6) return;
    unsigned en = w_find_active(SENEMY_CELL, g_gw.a, g_gw.b, g_gw.c);
    if (en != g_gw.en) {
        r2_log(AIM_LOG "grap: enemy (%d,%u,%u) is now %08x", g_gw.a, g_gw.b, g_gw.c, en);
        g_gw.en = en;
    }
    if (!en || !me) return;
    unsigned role = rd32(me + 0x790C) & 0xFF, idx = role < 8 ? role : (unsigned)me_slot, vt = rd32(en), cls_fn = vt ? rd32(vt + 0x434) : 0;   /* the evaluator FUN_0040B1xx passes the entry's +0x1C = the player's +0x790C (FUN_006FCDB0), not +0x3788 */
    unsigned ca[3] = {en, idx, 0}, cc[3] = {en, idx, 0}, cv[4] = {en, en, idx, 0};
    unsigned chk_a = do_call(0, 0x607040, ca, 3) & 0xFF, chk_c = do_call(0, 0x607A50, cc, 3) & 0xFF;
    unsigned cls = cls_fn > 0x400000 && cls_fn < 0x1400000 ? do_call(2, cls_fn, cv, 4) & 0xFF : 0xEE;
    float dx = rdf(en + 0x40) - rdf(me + 0x40), dz = rdf(en + 0x48) - rdf(me + 0x48);
    unsigned short mot = 0;
    r2_safe_read((void *)(en + 0x4A4), &mot, 2);
    unsigned v[16] = {rd32(en + 0x1A54), rd32(en + 0x3010), (rd32(en + 0x37B8) >> 30) & 1, rd32(en + 0x5B0) & 0x3000, rd32(en + 0x7570) != 0, rd32(en + 0x7574) != 0,
                      rd32(en + 0x7578) != 0, rd32(en + 0x757C) != 0, chk_a, chk_c, cls, rd32(me + 0x1A54), rd32(me + 0x791C) == en, (unsigned)(int)rd32(en + 0x1A08), mot, 0};
    if (memcmp(v, g_gw.s, sizeof v) && g_gw.n_log < 600) {
        g_gw.n_log++;
        r2_log(AIM_LOG "grap (%d,%u,%u) %08x: act %u motion %03x 3010 %u alerted %u flags5B0 %04x entries stagger %u/%u surprise %u/%u | checks for my human (index %u = +0x790C): stagger %u surprise %u class %u"
                       " | my act %u target-is-it %u | hp %d dist %.0f",
               g_gw.a, g_gw.b, g_gw.c, en, v[0], v[14], v[1], v[2], v[3], v[4], v[5], v[6], v[7], idx, v[8], v[9], v[10], v[11], v[12], (int)v[13], sqrtf(dx * dx + dz * dz));
        memcpy(g_gw.s, v, sizeof v);
    }
    static unsigned last_a;
    if (chk_a == 1 && last_a != 1) {
        g_gw.offers++;
        unsigned entry = idx < 2 ? rd32(en + 0x7570 + 4 * idx) : 0, kind13 = (rd32(en + 0x5B0) >> 13) & 1;
        g_gw.entry = entry, g_gw.idx = idx;
        if (g_gw.mode != GW_NONE && !g_gw.pressing) {
            if (g_gw.mode == GW_BUTTONS) {
                gw_pad(g_gw.btn), g_gw.pressing = 1, g_gw.t_press = now, g_gw.presses++;
                r2_log(AIM_LOG "grap: the stagger offer is up (kind %u): auto press %04x (offer %u)", kind13, g_gw.btn, g_gw.offers);
            } else if (g_gw.mode == GW_STOCK) {
                r2_log(AIM_LOG "grap: the stagger offer is up (kind %u, entry %08x, offer %u)", kind13, entry, g_gw.offers);
                gw_stock_press(me, "auto stock");
            } else {
                unsigned ka[2] = {entry, idx}, bkind = entry ? do_call(2, 0x40AD80, ka, 2) : 0, bit = gp_spad_bit_for_kind(bkind);
                int ok = gp_xinput_for_spad(bit, &g_gw.x);
                r2_log(AIM_LOG "grap: the stagger offer is up (kind %u, entry %08x, offer %u): the game's button for it: kind %u -> sPad bit %05x -> XInput buttons %04x LT %u RT %u%s", kind13, entry, g_gw.offers,
                       bkind, bit, g_gw.x.buttons, g_gw.x.lt, g_gw.x.rt, ok ? "" : " (none known)");
                if (ok) gw_xpad(&g_gw.x), g_gw.pressing = 1, g_gw.t_press = now, g_gw.presses++;
                if (g_gw.mode == GW_GAME_STOCK) g_gw.fb_armed = 1, g_gw.t_press = now;
            }
        }
    }
    last_a = chk_a;
}

/* ---- lab: `net aim assist <victim slot 0|1> gun|knife [ms] | assist watch | assist off`: freeing a held human by hitting its holder (combat2, notes\combat2.md section 4).
 * watch: every hold / release of either human on THIS PC is logged (action INT_MIN with player+0x7900 bit 13 = held, glue_logic.h), with the holder (catch id: victim+0x3788 ==
 * enemy+0x73E8), its action and hp. gun|knife (implies watch): the first time the victim is held, THIS PC's human attacks the holder for <ms> (default 6000) with the lab's own
 * tools: gun = `net aim cam <holder> joint 4` + `net ach fight <holder> <ms> 600 1 0` (LT held, RT presses), knife = `net ach fight <holder> <ms> 150 0 1` (approach, RT).
 * When the victim is free the attack stops. The blows themselves are the game's (and MHIT on a client). A console round trip is too slow for a ~5 s hold. Game thread, lab only. */
static struct { int on, watch, vslot, gun, started, prev[2], la; unsigned ms, t_start, lb, lc, holder, t_held[2], n_log; } g_as;
static unsigned as_holder(unsigned victim, int *la, unsigned *lb, unsigned *lc)
{
    unsigned tag = rd32(victim + 0x3788), list[48], n = w_live_enemies(list, 48);
    if (!tag) return 0;
    for (unsigned i = 0; i < n; i++)
        if (rd32(list[i] + 0x73E8) == tag) {
            layout_t l;
            layout_of(list[i], &l);
            *la = l.a, *lb = l.b, *lc = l.c;
            return list[i];
        }
    return 0;
}
static void as_tick(void)
{
    if (!g_as.watch) return;
    unsigned now = GetTickCount();
    for (int sl = 0; sl < 2; sl++) {
        unsigned p = player_obj(sl);
        if (!p) continue;
        int held = glue_is_held(rd32(p + 0x1A54), rd32(p + 0x7900));
        if (held == g_as.prev[sl]) continue;
        g_as.prev[sl] = held;
        int la = 0; unsigned lb = 0, lc = 0, h = as_holder(p, &la, &lb, &lc);
        if (held) g_as.t_held[sl] = now;
        if (g_as.n_log++ < 200)
            r2_log(AIM_LOG "assist: slot %d (%s, %08x) is %s%s: holder (%d,%u,%u) %08x act %d hp %d, catch id %08x, my slot %d", sl, sl == g_my_slot ? "my human" : "the partner", p,
                   held ? "HELD" : "FREE", held ? "" : (g_as.t_held[sl] ? " again" : ""), la, lb, lc, h, h ? (int)rd32(h + 0x1A54) : -1, h ? (int)rd32(h + 0x1A08) : 0, rd32(p + 0x3788), g_my_slot);
        if (!held && g_as.t_held[sl] && g_as.n_log < 200)
            r2_log(AIM_LOG "assist: slot %d was held for %u ms", sl, now - g_as.t_held[sl]);
    }
    if (!g_as.on) return;
    unsigned v = player_obj(g_as.vslot);
    if (!v) return;
    int held = glue_is_held(rd32(v + 0x1A54), rd32(v + 0x7900));
    char cmd[120], out[300];
    if (!g_as.started && held) {
        int la = 0; unsigned lb = 0, lc = 0, h = as_holder(v, &la, &lb, &lc);
        if (!h) return;
        g_as.started = 1, g_as.t_start = now, g_as.holder = h, g_as.la = la, g_as.lb = lb, g_as.lc = lc;
        if (g_as.gun) {
            snprintf(cmd, sizeof cmd, "cam %d %u %u joint 4", la, lb, lc);
            aim_command(cmd, out, sizeof out);
            snprintf(cmd, sizeof cmd, "fight %d %u %u %u 600 1 0", la, lb, lc, g_as.ms);
        } else {
            snprintf(cmd, sizeof cmd, "fight %d %u %u %u 150 0 1", la, lb, lc, g_as.ms);
        }
        ach_command(cmd, out, sizeof out);
        r2_log(AIM_LOG "assist: slot %d is held by (%d,%u,%u) %08x (act %u hp %d): my human attacks it with the %s for %u ms (%s)", g_as.vslot, la, lb, lc, h, rd32(h + 0x1A54), (int)rd32(h + 0x1A08),
               g_as.gun ? "gun" : "knife", g_as.ms, out);
        return;
    }
    if (g_as.started && (!held || (int)(now - g_as.t_start) > (int)g_as.ms + 2000)) {
        unsigned h = g_as.holder;
        ach_command("fight stop", out, sizeof out);
        if (g_as.gun) aim_command("cam off", out, sizeof out);
        r2_log(AIM_LOG "assist: slot %d %s %u ms after my attack began; holder (%d,%u,%u) act %d hp %d", g_as.vslot, held ? "is STILL held" : "is FREE", now - g_as.t_start, g_as.la, g_as.lb, g_as.lc,
               h ? (int)rd32(h + 0x1A54) : -1, h ? (int)rd32(h + 0x1A08) : 0);
        g_as.on = 0, g_as.started = 0;
    }
}

/* ---- lab: `net aim press <manager cell hex> <a> <b> <c> <entry offset hex> [game|stock]`: take the cActionButton entry an object keeps at +<offset> (a brick uSmInteractive:
 * sSetModel 1581920, +43D0, registered by 0x7D4E50 with +0x2C 0x25) for THIS PC's human: game = press the button the game wants for it (FUN_0040AD80 -> sPad bit -> XInput, 250 ms),
 * stock = call the entry's press handler FUN_0040ACB0(entry, index). 700 ms later my action and the object in my hand (+0x7380, its class) are logged. Lab only (combat2: Natalia's brick). */
static struct { int on, logged; unsigned t, entry, idx; gp_xpad_t x; } g_pr;
static void pr_tick(void)
{
    if (!g_pr.on) return;
    unsigned now = GetTickCount(), me = player_obj(g_my_slot >= 0 ? g_my_slot : 0);
    if (!g_pr.logged && (int)(now - g_pr.t) >= 250) gw_xpad(NULL), g_pr.logged = 1;
    if ((int)(now - g_pr.t) < 700 || !me) return;
    unsigned hand = rd32(me + 0x7380);
    char cn[100] = "-";
    if (hand) w_class(hand, cn, sizeof cn);
    r2_log(AIM_LOG "press: 700 ms after the press of entry %08x: my action %d, in my hand %08x (%s), owner +19C0 %08x", g_pr.entry, (int)rd32(me + 0x1A54), hand, cn, hand ? rd32(hand + 0x19C0) : 0);
    g_pr.on = 0;
}
static void pr_command(const char *rest, char *out, int size)
{
    unsigned cell = 0, b = 0, c = 0, ofs = 0;
    int a = 0;
    char how[12] = "game";
    if (sscanf(rest, "%x %d %u %u %x %11s", &cell, &a, &b, &c, &ofs, how) < 5) {
        snprintf(out, size, "usage: net aim press <manager cell hex> <a> <b> <c> <entry offset hex> [game|stock]");
        return;
    }
    unsigned obj = w_find_active(cell, a, b, c), me = player_obj(g_my_slot >= 0 ? g_my_slot : 0);
    if (!obj || !me) { snprintf(out, size, "no object (%d,%u,%u) in %x or no human", a, b, c, cell); return; }
    unsigned entry = rd32(obj + ofs), role = rd32(me + 0x790C) & 0xFF, idx = role < 8 ? role : (unsigned)(g_my_slot >= 0 ? g_my_slot : 0);
    if (!entry) { snprintf(out, size, "object %08x has no entry at +%x", obj, ofs); return; }
    unsigned char ev = 0;
    r2_safe_read((void *)(entry + 5), &ev, 1);
    g_pr.entry = entry, g_pr.idx = idx, g_pr.t = GetTickCount(), g_pr.on = 1, g_pr.logged = 0;
    if (!strcmp(how, "stock")) {
        unsigned args[2] = {entry, idx};
        do_call(2, 0x40ACB0, args, 2);
        g_pr.logged = 1;
        snprintf(out, size, "press: object %08x entry %08x (evaluated %u) for index %u: stock press handler called, my action now %d", obj, entry, ev, idx, (int)rd32(me + 0x1A54));
    } else {
        unsigned ka[2] = {entry, idx}, kind = do_call(2, 0x40AD80, ka, 2), bit = gp_spad_bit_for_kind(kind);
        int ok = gp_xinput_for_spad(bit, &g_pr.x);
        if (ok) gw_xpad(&g_pr.x); else g_pr.logged = 1;
        snprintf(out, size, "press: object %08x entry %08x (evaluated %u) for index %u: button kind %u -> sPad %05x -> XInput %04x LT %u RT %u%s", obj, entry, ev, idx, kind, bit, g_pr.x.buttons, g_pr.x.lt, g_pr.x.rt,
                 ok ? " pressed for 250 ms" : " (no button known: nothing pressed)");
    }
    r2_log(AIM_LOG "%s", out);
}

/* per frame (coop_game_frame): resolve the target enemy, log its hp after my shots */
static void aim_frame(void)
{
    gw_tick();
    ac_tick();
    as_tick();
    pr_tick();
    if (g_aim.mode != AIM_ENEMY) return;
    unsigned en = w_find_active(SENEMY_CELL, g_aim.a, g_aim.b, g_aim.c);
    if (en != g_aim.en) {
        r2_log(AIM_LOG "target (%d,%u,%u) is now %08x", g_aim.a, g_aim.b, g_aim.c, en);
        g_aim.en = en, g_aim.hp_seen = en ? (int)rd32(en + 0x1A08) : 0;
    }
    if (!en) return;
    int hp = (int)rd32(en + 0x1A08);
    if (hp != g_aim.hp_seen) {
        unsigned now = GetTickCount();
        r2_log(AIM_LOG "target (%d,%u,%u) hp %d -> %d, %u ms after my shot %u (action %u)", g_aim.a, g_aim.b, g_aim.c, g_aim.hp_seen, hp, g_aim.last_shot_t ? now - g_aim.last_shot_t : 0, g_aim.shots,
               rd32(en + 0x1A54));
        g_aim.hp_seen = hp;
    }
}

static void aim_status(char *out, int size)
{
    snprintf(out, size,
             "aim %s%s (call %s): target %s (%d,%u,%u) obj %08x joint %d dy %.0f pos (%.0f,%.0f,%.0f); my shots %u (calls %u, changed %u, other characters %u); last: start (%.0f,%.0f,%.0f) "
             "vector (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f), %.1f deg, target (%.0f,%.0f,%.0f) at %.0f",
             g_aim.mode == AIM_OFF ? "off" : g_aim.mode == AIM_ENEMY ? "on (enemy)" : "on (point)", g_aim.watch ? " watch-only" : "", g_aim.installed ? "patched" : "stock",
             g_aim.mode == AIM_ENEMY ? "enemy" : "-", g_aim.a, g_aim.b, g_aim.c, g_aim.en, g_aim.joint, g_aim.dy, g_aim.pos[0], g_aim.pos[1], g_aim.pos[2], g_aim.shots, g_aim.my_calls,
             g_aim.changed, g_aim.other_calls, g_aim.last_start[0], g_aim.last_start[1], g_aim.last_start[2], g_aim.last_old[0], g_aim.last_old[1], g_aim.last_old[2], g_aim.last_new[0],
             g_aim.last_new[1], g_aim.last_new[2], g_aim.last_turn, g_aim.last_tgt[0], g_aim.last_tgt[1], g_aim.last_tgt[2], g_aim.last_dist);
}

/* net aim at <a> <b> <c> [top|joint <id>|base] [dy <units>] | pos <x> <y> <z> | watch <a> <b> <c> [..] | joints <a> <b> <c> | off | status  (game thread) */
static void aim_command(const char *arg, char *out, int size)
{
    char sub[16] = "";
    sscanf(arg, "%15s", sub);
    const char *rest = arg + strlen(sub);
    while (*rest == ' ') rest++;
    if (!sub[0] || !strcmp(sub, "status")) { aim_status(out, size); return; }
    if (!strcmp(sub, "off")) {
        g_aim.mode = AIM_OFF, g_aim.watch = 0, g_aim.en = 0;
        snprintf(out, size, "aim off (call site stays %s; it calls the stock function only)", g_aim.installed ? "patched" : "stock");
        return;
    }
    if (!strcmp(sub, "joints")) {
        int a = 0; unsigned b = 0, c = 0;
        if (sscanf(rest, "%d %u %u", &a, &b, &c) < 3) { snprintf(out, size, "usage: net aim joints <a> <b> <c>"); return; }
        unsigned en = w_find_active(SENEMY_CELL, a, b, c);
        if (!en) { snprintf(out, size, "no active enemy (%d,%u,%u)", a, b, c); return; }
        int o = snprintf(out, size, "enemy %08x pos (%.0f,%.0f,%.0f) hp %d act %u joints:", en, rdf(en + 0x40), rdf(en + 0x44), rdf(en + 0x48), (int)rd32(en + 0x1A08), rd32(en + 0x1A54));
        for (int j = 0; j < 256 && o < size - 40; j++) {
            float p[3];
            if (aim_joint_pos(en, j, p)) o += snprintf(out + o, size - o, " %d:(%.0f,%.0f,%.0f)", j, p[0], p[1], p[2]);
        }
        float tp[3];
        int top = aim_top_joint(en, tp);
        if (o < size - 60) snprintf(out + o, size - o, "; top joint %d at y %.0f", top, top >= 0 ? tp[1] : 0.0f);
        r2_log(AIM_LOG "%s", out);
        return;
    }
    if (!strcmp(sub, "cam")) {
        if (!strncmp(rest, "off", 3)) {
            g_ac.on = 0;
            snprintf(out, size, "aim cam off (last yaw %.3f pitch %.3f)", g_ac.yaw, g_ac.pitch);
            return;
        }
        int a = 0, n = 0; unsigned b = 0, c = 0;
        if (sscanf(rest, "%d %u %u%n", &a, &b, &c, &n) < 3) { snprintf(out, size, "usage: net aim cam <a> <b> <c> [joint <id>|base] [dy <u>] [right <u>] [eye <u>] | cam off"); return; }
        const char *q = rest + n;
        int joint = 0; float dy = 0, right = AIM_CAM_RIGHT, eye = AIM_CAM_EYE;
        char w[16]; int k;
        while (sscanf(q, "%15s%n", w, &k) == 1) {
            q += k;
            if (!strcmp(w, "base")) joint = -1;
            else if (!strcmp(w, "joint") && sscanf(q, "%d%n", &joint, &k) == 1) q += k;
            else if (!strcmp(w, "dy") && sscanf(q, "%f%n", &dy, &k) == 1) q += k;
            else if (!strcmp(w, "right") && sscanf(q, "%f%n", &right, &k) == 1) q += k;
            else if (!strcmp(w, "eye") && sscanf(q, "%f%n", &eye, &k) == 1) q += k;
        }
        g_ac.on = 1, g_ac.a = a, g_ac.b = b, g_ac.c = c, g_ac.joint = joint, g_ac.dy = dy, g_ac.right = right, g_ac.eye = eye, g_ac.frames = 0, g_ac.n_log = 0;
        snprintf(out, size, "aim cam (%d,%u,%u): enemy %08x, joint %d dy %.0f, camera right %.1f eye %.0f", a, b, c, w_find_active(SENEMY_CELL, a, b, c), joint, dy, right, eye);
        r2_log(AIM_LOG "%s", out);
        return;
    }
    if (!strcmp(sub, "press")) { pr_command(rest, out, size); return; }
    if (!strcmp(sub, "assist")) {
        char how[16] = "";
        int vs = -1; unsigned ms = 6000;
        if (!strncmp(rest, "off", 3)) {
            if (g_as.started) { char o2[200]; ach_command("fight stop", o2, sizeof o2); if (g_as.gun) aim_command("cam off", o2, sizeof o2); }
            g_as.on = g_as.watch = g_as.started = 0;
            snprintf(out, size, "assist off");
            return;
        }
        if (!strncmp(rest, "watch", 5)) {
            g_as.watch = 1, g_as.prev[0] = g_as.prev[1] = 0;
            snprintf(out, size, "assist watch on (holds / releases of both humans on this PC are logged)");
            r2_log(AIM_LOG "%s", out);
            return;
        }
        if (sscanf(rest, "%d %15s %u", &vs, how, &ms) < 2 || vs < 0 || vs > 1 || (strcmp(how, "gun") && strcmp(how, "knife"))) {
            snprintf(out, size, "usage: net aim assist <victim slot 0|1> gun|knife [ms] | assist watch | assist off");
            return;
        }
        g_as.on = 1, g_as.watch = 1, g_as.vslot = vs, g_as.gun = !strcmp(how, "gun"), g_as.ms = ms ? ms : 6000, g_as.started = 0;
        snprintf(out, size, "assist armed: when slot %d is held, my human (slot %d) attacks the holder with the %s for %u ms", vs, g_my_slot, how, g_as.ms);
        r2_log(AIM_LOG "%s", out);
        return;
    }
    if (!strcmp(sub, "grap")) {
        int a = 0; unsigned b = 0, c = 0, btn = 0;
        char w[16] = "", how[24] = "";
        if (!strncmp(rest, "off", 3)) {
            if (g_gw.pressing) { if (g_gw.mode == GW_BUTTONS) gw_pad(0); else gw_xpad(NULL); }
            g_gw.on = 0, g_gw.pressing = 0;
            snprintf(out, size, "grap watch off (offers %u, presses %u, stock presses %u)", g_gw.offers, g_gw.presses, g_gw.stock_presses);
            return;
        }
        int n = sscanf(rest, "%d %u %u %15s %23s", &a, &b, &c, w, how);
        if (n < 3) { snprintf(out, size, "usage: net aim grap <a> <b> <c> [auto <hexbuttons>|auto game|auto game+stock|auto stock] | grap off"); return; }
        int mode = GW_NONE;
        if (n >= 5 && !strcmp(w, "auto")) {
            if (!strcmp(how, "game")) mode = GW_GAME;
            else if (!strcmp(how, "game+stock")) mode = GW_GAME_STOCK;
            else if (!strcmp(how, "stock")) mode = GW_STOCK;
            else if (sscanf(how, "%x", &btn) == 1 && btn) mode = GW_BUTTONS;
        }
        memset(&g_gw, 0, sizeof g_gw);
        g_gw.on = 1, g_gw.a = a, g_gw.b = b, g_gw.c = c, g_gw.btn = btn, g_gw.mode = mode;
        static const char *const mn[] = {"", " with auto press (buttons)", " with auto press (the game's button)", " with auto press (the game's button, stock press fallback)", " with the stock press handler"};
        snprintf(out, size, "grap watch (%d,%u,%u) on%s", a, b, c, mn[mode]);
        r2_log(AIM_LOG "%s (buttons %04x)", out, btn);
        return;
    }
    if (!strcmp(sub, "pos")) {
        float x, y, z;
        if (sscanf(rest, "%f %f %f", &x, &y, &z) < 3) { snprintf(out, size, "usage: net aim pos <x> <y> <z>"); return; }
        if (!aim_install()) { snprintf(out, size, "aim: NOT installed (bytes differ, see the log)"); return; }
        g_aim.mode = AIM_POS, g_aim.watch = 0, g_aim.pos[0] = x, g_aim.pos[1] = y, g_aim.pos[2] = z, g_aim.n_log = 0;
        snprintf(out, size, "aim: my shots go to (%.0f,%.0f,%.0f)", x, y, z);
        return;
    }
    if (!strcmp(sub, "at") || !strcmp(sub, "watch")) {
        int a = 0, n = 0; unsigned b = 0, c = 0;
        if (sscanf(rest, "%d %u %u%n", &a, &b, &c, &n) < 3) { snprintf(out, size, "usage: net aim at|watch <a> <b> <c> [top|base|joint <id>] [dy <units>]"); return; }
        const char *p = rest + n;
        int joint = -1; float dy = -6.0f;
        char w[16]; int k;
        while (sscanf(p, "%15s%n", w, &k) == 1) {
            p += k;
            if (!strcmp(w, "top")) joint = -1;
            else if (!strcmp(w, "base")) joint = -2, dy = 100.0f;
            else if (!strcmp(w, "joint") && sscanf(p, "%d%n", &joint, &k) == 1) p += k;
            else if (!strcmp(w, "dy") && sscanf(p, "%f%n", &dy, &k) == 1) p += k;
        }
        if (!aim_install()) { snprintf(out, size, "aim: NOT installed (bytes differ, see the log)"); return; }
        g_aim.mode = AIM_ENEMY, g_aim.watch = !strcmp(sub, "watch"), g_aim.a = a, g_aim.b = b, g_aim.c = c, g_aim.joint = joint, g_aim.dy = dy, g_aim.n_log = 0;
        g_aim.en = w_find_active(SENEMY_CELL, a, b, c), g_aim.hp_seen = g_aim.en ? (int)rd32(g_aim.en + 0x1A08) : 0;
        float t[3] = {0, 0, 0};
        int have = aim_target(t);
        snprintf(out, size, "aim %s (%d,%u,%u): enemy %08x hp %d, joint %d dy %.0f, target point %s(%.0f,%.0f,%.0f)", g_aim.watch ? "watch" : "at", a, b, c, g_aim.en, g_aim.hp_seen, joint, dy,
                 have ? "" : "NONE ", t[0], t[1], t[2]);
        r2_log(AIM_LOG "%s", out);
        return;
    }
    snprintf(out, size, "usage: net aim at|watch <a> <b> <c> [top|base|joint <id>] [dy <u>] | pos x y z | joints a b c | cam a b c [joint <id>|base] [dy|right|eye <u>] | cam off | grap a b c [auto <hex>|game|game+stock|stock] | grap off | assist <slot> gun|knife [ms] | assist watch|off | off | status");
}
