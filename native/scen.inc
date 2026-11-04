/* scen.inc: stage 6e, scenario flags and door operations as the story's FSM uses them (research tools, no network traffic). Included from world.inc after door.inc.
 *
 * The story FSM (cFSMOrder, scripts in the stage archives) keeps its progress in "scenario flags" (SF): bit arrays per stage inside the story block, read by `FUN_00A90510(index)` (stdcall, ecx and the stage are
 * taken from the story block; an index below 0x200 means the current area) and written by `FUN_00A91AE0` (set) / `FUN_00A91A60` (clear), both thiscall on the story block. A condition node of an FSM reads
 * SF[n], a `Flag` node writes it, a trigger zone writes it when a human enters, the demo system writes some of them (demo started / playing / ended). The door operation of the FSM is `FUN_0086E0B0(door, param)`
 * (stdcall): the byte at param+5 (ActType) picks a door virtual: 0 unlock + close (action 2), 1 lock + close (action 2), 2 open (action 3), 3 set bit 0x10 at +0x4350, 6 clear it + action 1, 8 unlock + action 0x11,
 * 9 lock + action 0x11 (8 and 9 only when the state byte at +0x4354 is not 2 / 6), 10 action 0x12 (only when the state byte is not 1 / 5).
 * Console: `net w sf` (all set flags of the current area), `net w sf get <n>`, `net w sf set <n> <0|1>`, `net w sf watch 0|1|log N` (log every change, 10 Hz), `net w doors op <a> <b> <c> <acttype>` (door.inc calls scen_door_op),
 * `net w door <a> <b> <c>` (state of one set model: action ids, lock word, flags, state byte). */
#define SF_N 300
static int scen_door_op(unsigned door, int acttype);
static void bgl_tick(unsigned now);   /* 6j: loader mirror, below */
#include "bgl_logic.h"
static bgl_state_t g_bgl;
static int g_bgl_mirror = 1;
static unsigned g_bgl_tx, g_bgl_rx, g_bgl_calls, g_bgl_self_ok, g_bgl_writes, g_bgl_sent_t, g_bgl_scan_t, g_bgl_logs;
static bgl_t g_bgl_local[BGL_SLOTS], g_bgl_sent[BGL_SLOTS];
static int g_bgl_local_n, g_bgl_sent_n = -1, g_bgl_was_running;
typedef void(__attribute__((thiscall)) *bgl_unload_t)(unsigned self);
static bgl_unload_t o_bgl_unload;
/* stage change gate: the story stage changes when slot 0 stands on a floor of another stage and the old stage's blocks go away with the change; the human who is still on the old floor falls (s6j1/s6j2).
 * The hook on the area update (aEpisodeBase vf2, 0x462780) lets the change through only when every other human's floor already belongs to the new stage (or after GATE_MS): while it waits, the area's
 * own stage id (+0x30) is shown to the game as equal to slot 0's floor stage for the duration of the call. `net w bgl gate 0|1`. */
#define BGL_GATE_MS 25000
typedef void(__attribute__((thiscall)) *epi_upd_t)(unsigned self);
static epi_upd_t o_epi_upd;
static int g_stage_gate = 1;
static unsigned g_gate_holds, g_gate_since, g_gate_releases, g_gate_timeouts, g_gate_logs;
/* puppets in trigger zones (6j, proven by a control run on the pair: SF[18] stayed 0 on the PC where Claire is a puppet while the game's own shape test listed her inside four zones; writing 0 into her slot type
 * (+0x7920, 2 = remote human) for 3 s set SF[18] there): the action-button system (flag zones and FSM AreaHit zones) registers a human for a button in FUN_0040BFB0(viewer, button) and returns early for
 * slot type 2. The hook shows the puppet as type 0 for the duration of that one call. `net w bgl pupzones 0|1`. */
typedef void(__attribute__((thiscall)) *btn_cand_t)(unsigned self, unsigned button);
static btn_cand_t o_btn_cand;
static int g_pup_zones;   /* default OFF (6j): proven by hand, the first automatic version disturbed the co-op door (see notes); the scenario-zone-only version is not proven live. `net w bgl pupzones 1` */
static unsigned g_pup_flips, g_pup_logs;

static void bgl_read(unsigned bg, unsigned off, bgl_t *l)
{
    for (unsigned i = 0; i < BGL_SLOTS; i++) l[i].ep = (int)rd32(bg + off + 12 * i), l[i].stage = (int)rd32(bg + off + 12 * i + 4), l[i].block = (int)rd32(bg + off + 12 * i + 8);
}

static void bgl_fmt(const bgl_t *l, int n, char *s, int size)
{
    int o = 0;
    s[0] = 0;
    for (int i = 0; i < n && o + 24 < size; i++) o += snprintf(s + o, size - o, "%s[%d,%d,%d]", i ? " " : "", l[i].ep, l[i].stage, l[i].block);
    if (!n) snprintf(s, size, "-");
}

static unsigned char g_sf_last[SF_N];
static int g_sf_watch, g_sf_init, g_sf_log = 400;
static unsigned g_sf_changes, g_sf_t0, g_sf_scan_t, g_sf_area;

static int scen_get(int idx)
{
    unsigned a[1] = {(unsigned)idx};
    return (int)(do_call(1, 0xA90510, a, 1) & 0xFF);
}

static int scen_set(int idx, int v)
{
    unsigned sb = story_block();
    if (!sb) return 0;
    unsigned a[2] = {sb, (unsigned)idx};
    do_call(2, v ? 0xA91AE0 : 0xA91A60, a, 2);
    return 1;
}

/* scenario flag sync (research, `net w sf sync 0|1|2`): the story FSMs run on both PCs and mostly reach the same flags by themselves (same scripts, same inputs), but an input that only the OWNER's PC sees (its human enters a trigger zone,
 * a client-started scene) sets flags on that PC only. With sync 1 a flag SET on one PC (found by the 10 Hz scan below) is sent as SF {area, index, value} and set on the other PC through the game's own setter unless it is set there
 * already (the applied value becomes that PC's baseline, so nothing echoes); 2 sends clears too. Scans restart from a fresh baseline after a level (re)load (the quiet window of door.inc). */
static int g_sf_sync;
static unsigned g_sf_tx, g_sf_rx, g_sf_applied, g_sf_same, g_sf_wrong_area;

/* Claire's cell door (stage 6e finding). The scripts open the six cell doors at the start (`Door_man_jail2`: ActType 2) and, when the intro scene ends and SF[1] is set, `Door_Pl1_Start` LOCKS door (100,160,130) (ActType 1: lock bit +0x37B8 |= 1,
 * action 2). In the stock flow it then stays in action 2 (state byte 6) for good: no prompt, no way out; nothing in the 127 scripts of the stage unlocks it again (the unlock that the real game has was not found). A door only offers its prompt in
 * action 1 (the waiting action, flag +0x4350 bit 0), and a human opens it with the interact key (the prompt shows E on the keyboard, A on a pad): action 4 (OpenHand), the door slides away. The auto-open (action 3, ActType 2) does not remove
 * the collision of this door. So the helper does what the story misses: 6 s after SF[1] it unlocks the door (ActType 0, clears the lock bit) and switches it to action 1. `net w celldoor 0|1|2` (0 off, 1 only while a session runs = default, 2 always). */
static int g_celldoor = 1;
static unsigned g_celldoor_t, g_celldoor_since, g_celldoor_obj, g_celldoor_done_obj, g_celldoor_n;

static void scen_door_unlock_wait(unsigned so)
{
    scen_door_op(so, 0);
    unsigned fn = rd32(rd32(so) + 0xB8);
    if (fn > 0x400000 && fn < 0x1400000) {
        unsigned args[6] = {so, 1, 1, 0, 0xFFFFFFFFu, 0};
        do_call(2, fn, args, 6);
    }
}

static void scen_celldoor_tick(unsigned now, unsigned area)
{
    if (!g_celldoor || (g_celldoor == 1 && !w_session_running())) return;
    if (now - g_celldoor_t < 500) return;
    g_celldoor_t = now;
    if (area != 100 || !scen_get(1)) {
        g_celldoor_since = 0;
        return;
    }
    unsigned so = w_find_active(SSETMODEL_CELL, 100, 160, 130);
    if (!so || so == g_celldoor_done_obj) return;
    if ((rd32(so + 0x37B8) & 1) == 0 || (int)rd32(so + 0x1A54) != 2) return;   /* not locked and closed (the stock state after SF[1]) */
    if (so != g_celldoor_obj) g_celldoor_obj = so, g_celldoor_since = now;
    if (!g_celldoor_since) g_celldoor_since = now;
    if (now - g_celldoor_since < 6000) return;
    scen_door_unlock_wait(so);
    g_celldoor_done_obj = so;
    g_celldoor_n++;
    r2_log(W_LOG "cell door (100,160,130) %08x: locked and closed %u ms after SF[1] (the stock state): unlocked and put into the waiting action (the human opens it with the interact key)", so, now - g_celldoor_since);
}

static void scen_tick(void)
{
    if (!story_block()) return;
    bgl_tick(GetTickCount());   /* 6j: the loader mirror (list exchange) runs whenever a session does */
    unsigned area = story_area_id();
    if (area < 100 || area >= 3000 || !game_area()) return;   /* title / menus / loading: the story block holds no stage yet (the flag lookup would walk garbage) */
    unsigned now = GetTickCount();
    smtrace_tick();   /* door.inc: per-frame change log of one set model (net w smtrace) */
    scen_celldoor_tick(now, area);
    if (!g_sf_watch && !g_sf_sync) return;
    if (now - g_sf_scan_t < 100) return;
    g_sf_scan_t = now;
    unsigned char cur[SF_N];
    for (int i = 0; i < SF_N; i++) cur[i] = (unsigned char)scen_get(i);
    if (!g_sf_init || (int)(now - g_w_sm_quiet_until) < 0 || g_sf_area != story_area_id()) {   /* first scan, a level was (re)loaded, the area changed: new baseline, nothing is an event */
        memcpy(g_sf_last, cur, sizeof cur);
        g_sf_init = 1, g_sf_t0 = now, g_sf_area = story_area_id();
        return;
    }
    int sent = 0;
    for (int i = 0; i < SF_N; i++)
        if (cur[i] != g_sf_last[i]) {
            g_sf_changes++;
            if (g_sf_watch && (int)g_sf_changes <= g_sf_log) r2_log(W_LOG "SF[%d] %d -> %d (area %u, +%u ms since the watch started)", i, g_sf_last[i], cur[i], g_sf_area, now - g_sf_t0);
            if (g_sf_sync && w_session_running() && (cur[i] || g_sf_sync >= 2) && sent < 16) {
                unsigned char b[7];
                int area = (int)g_sf_area;
                memcpy(b, &area, 4), b[4] = (unsigned char)(i & 0xFF), b[5] = (unsigned char)(i >> 8), b[6] = cur[i];
                w_ev_send(EV_W_SF, b, 7);
                g_sf_tx++, sent++;
            }
            g_sf_last[i] = cur[i];
        }
}

static void scen_event(const cp_event *e)
{
    if (e->len < 7 || !g_sf_sync) return;
    int area;
    memcpy(&area, e->body, 4);
    unsigned idx = e->body[4] | ((unsigned)e->body[5] << 8), v = e->body[6] ? 1 : 0;
    g_sf_rx++;
    if ((unsigned)area != story_area_id() || idx >= SF_N) {
        g_sf_wrong_area++;
        return;
    }
    if (scen_get((int)idx) == (int)v) {
        g_sf_same++;
        if (g_sf_init) g_sf_last[idx] = (unsigned char)v;
        return;
    }
    scen_set((int)idx, (int)v);
    if (g_sf_init) g_sf_last[idx] = (unsigned char)v;   /* the applied value is this PC's baseline: no echo */
    g_sf_applied++;
    if (g_sf_applied < 100) r2_log(W_LOG "SF[%u] = %u applied from the other PC (area %d)", idx, v, area);
}

static void scen_command(const char *arg, char *out, int size)
{
    int o = 0, n = 0, v = 0;
    char w[16] = "";
    sscanf(arg, "%15s", w);
    if (!story_block() || story_area_id() < 100 || story_area_id() >= 3000) {
        snprintf(out, size, "no stage loaded (story area %u)", story_block() ? story_area_id() : 0);
        return;
    }
    if (!strcmp(w, "get") && sscanf(arg + 3, "%d", &n) == 1) {
        snprintf(out, size, "SF[%d] = %d (area %u)", n, scen_get(n), story_area_id());
        return;
    }
    if (!strcmp(w, "set") && sscanf(arg + 3, "%d %d", &n, &v) == 2) {
        if (!lt_on_game_thread()) {   /* the story block's critical section and the FSM read these on the game thread */
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w sf %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        int before = scen_get(n);
        scen_set(n, v);
        snprintf(out, size, "SF[%d] %d -> %d (area %u)", n, before, scen_get(n), story_area_id());
        return;
    }
    if (!strcmp(w, "celldoor")) {
        const char *p = arg + 8;
        while (*p == ' ') p++;
        if (*p) g_celldoor = atoi(p);
        snprintf(out, size, "cell door helper %d (0 off, 1 while a session runs, 2 always), applied %u times", g_celldoor, g_celldoor_n);
        return;
    }
    if (!strcmp(w, "sync")) {
        const char *p = arg + 4;
        while (*p == ' ') p++;
        if (*p) g_sf_sync = atoi(p), g_sf_init = 0;
        snprintf(out, size, "scenario flag sync %d (0 off, 1 sets, 2 sets and clears): sent %u, received %u (applied %u, already same %u, other area %u)", g_sf_sync, g_sf_tx, g_sf_rx, g_sf_applied, g_sf_same, g_sf_wrong_area);
        return;
    }
    if (!strcmp(w, "watch")) {
        const char *p = arg + 5;
        while (*p == ' ') p++;
        if (!strncmp(p, "log", 3)) g_sf_log = atoi(p + 3);
        else if (*p) g_sf_watch = atoi(p) != 0, g_sf_init = 0, g_sf_changes = 0;
        snprintf(out, size, "scenario flag watch %s, log limit %d, changes seen %u", g_sf_watch ? "ON" : "off", g_sf_log, g_sf_changes);
        return;
    }
    o += snprintf(out + o, size - o, "scenario flags set in area %u:", story_area_id());
    for (int i = 0; i < SF_N && o < size - 12; i++)
        if (scen_get(i)) o += snprintf(out + o, size - o, " %d", i);
}

/* the door operation of the FSM: ActType picks the virtual (see above); the same call the `Door` order makes for every target */
static int scen_door_op(unsigned door, int acttype)
{
    unsigned char param[16];
    memset(param, 0, sizeof param);
    param[5] = (unsigned char)acttype;
    unsigned a[2] = {door, (unsigned)(uintptr_t)param};
    do_call(1, 0x86E0B0, a, 2);
    return 1;
}

static void scen_door_info(unsigned so, char *out, int size)
{
    char cn[100];
    int o = 0;
    o += snprintf(out + o, size - o, "%s %08x action %d (prev %d) lock word +37B8 %08x, +4350 %08x, state byte +4354 %u, action words:", w_class(so, cn, sizeof cn), so, (int)rd32(so + 0x1A54), (int)rd32(so + 0x1A58), rd32(so + 0x37B8), rd32(so + 0x4350),
                  rd32(so + 0x4354) & 0xFF);
    for (unsigned k = 0; k < 12; k++) o += snprintf(out + o, size - o, " %08x", rd32(so + 0x1A38 + 4 * k));
}

/* net w door <a> <b> <c> [op <acttype>] [act <id>]: state of one set model; `op` runs the FSM's door operation on it, `act` a bare ChangeAction */
static void scen_door_command(const char *arg, char *out, int size)
{
    int a = 0, t = 0, id = 0;
    unsigned b = 0, c = 0;
    if (sscanf(arg, "%d %u %u", &a, &b, &c) < 3) {
        snprintf(out, size, "usage: net w door <a> <b> <c> [op <acttype>] [act <id>]");
        return;
    }
    if ((strstr(arg, "op ") || strstr(arg, "act ")) && !lt_on_game_thread()) {   /* a door virtual / ChangeAction runs on the game thread */
        char cmd[200];
        snprintf(cmd, sizeof cmd, "net w door %s", arg);
        lt_post(cmd, out, size);
        return;
    }
    unsigned so = w_find_active(SSETMODEL_CELL, a, b, c);
    if (!so) {
        snprintf(out, size, "no set model (%d,%u,%u) in the active lists", a, b, c);
        return;
    }
    char cn[100];
    w_class(so, cn, sizeof cn);
    int o = 0;
    const char *p;
    if (strstr(arg, "unlock")) {   /* net w door a b c unlock: ActType 0 (clear the lock bit) + the waiting action: the door offers its prompt */
        if (!lt_on_game_thread()) {
            char cmd[200];
            snprintf(cmd, sizeof cmd, "net w door %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        if (strstr(cn, "Coop")) {
            snprintf(out, size, "refused: %s is a co-op set model", cn);
            return;
        }
        scen_door_unlock_wait(so);
        o += snprintf(out + o, size - o, "unlocked, waiting: ");
    } else if ((p = strstr(arg, "op ")) && sscanf(p + 3, "%d", &t) == 1) {
        if (strstr(cn, "Coop")) {
            snprintf(out, size, "refused: %s is a co-op set model (a bare door operation crashes the host)", cn);
            return;
        }
        o += snprintf(out + o, size - o, "before: ");
        scen_door_info(so, out + o, size - o), o = (int)strlen(out);
        scen_door_op(so, t);
        o += snprintf(out + o, size - o, "\nop %d done\nafter:  ", t);
    } else if ((p = strstr(arg, "act ")) && sscanf(p + 4, "%d", &id) == 1) {
        if (strstr(cn, "Coop")) {
            snprintf(out, size, "refused: %s is a co-op set model", cn);
            return;
        }
        unsigned fn = rd32(rd32(so) + 0xB8);
        unsigned args[6] = {so, (unsigned)id, 1, 0, 0xFFFFFFFFu, 0};
        o += snprintf(out + o, size - o, "before: ");
        scen_door_info(so, out + o, size - o), o = (int)strlen(out);
        do_call(2, fn, args, 6);
        o += snprintf(out + o, size - o, "\nChangeAction(%d) done\nafter:  ", id);
    }
    scen_door_info(so, out + o, size - o);
}

/* ---- stage 6h: the stage transition table of the story (read-only research tool) ------------------------------------------------------------------------------------------------------------
 * The game checks every gameplay pass (FUN_00426B50 -> FUN_00886E00) whether the CURRENT stage (story+0x140380) with the variant (+0x140384) has an entry whose scenario-flag condition holds; the entry names the
 * NEXT stage and the game's own stage loader (aGame state 8) switches to it. The manager is the global at 0x1579134: +0x24 groups (array at +0x30), a group has +0x6C entries (array at +0x78), an entry holds
 * u16 episode (+4), u16 stage (+6), u16 variant (+8), i32 SF that must be set (+0xC, -1 = none), i32 SF that must be clear (+0x10, -1 = none), i32 next stage (+0x14); the stage records (manager +0x38 groups,
 * array at +0x44, same group layout, id at +4) carry a flag byte at +8 (bit 0: the stage can be entered, bit 2: the clear / ending entry). `net w stages [all]` prints the entries of the current episode. */
static unsigned scen_rd16(unsigned a)
{
    unsigned short v = 0;
    r2_safe_read((void *)a, &v, 2);
    return v;
}

static int scen_stage_flags(unsigned mgr, unsigned id)
{
    unsigned ng = rd32(mgr + 0x38), garr = rd32(mgr + 0x44);
    for (unsigned g = 0; g < ng && g < 32; g++) {
        unsigned obj = rd32(garr + 4 * g);
        if (!obj) continue;
        unsigned n = rd32(obj + 0x6C), arr = rd32(obj + 0x78);
        for (unsigned k = 0; k < n && k < 512; k++) {
            unsigned e = rd32(arr + 4 * k);
            if (e && rd32(e + 4) == id) return (int)(rd32(e + 8) & 0xFF);
        }
    }
    return -1;
}

static void scen_stages_command(const char *arg, char *out, int size)
{
    unsigned sb = story_block(), mgr = rd32(0x1579134);
    if (!sb || !mgr) {
        snprintf(out, size, "no story block / stage manager");
        return;
    }
    unsigned ep = rd32(sb + 0x140378), part = rd32(sb + 0x15C4A8), cur = rd32(sb + 0x140380), var = rd32(sb + 0x140384);
    int all = strstr(arg, "all") != NULL, o = 0;
    o += snprintf(out + o, size - o, "episode %u part %u (+0x1403E8 = %u) stage %u variant %u, manager %08x: %u groups\n", ep, part, rd32(sb + 0x1403E8), cur, var, mgr, rd32(mgr + 0x24));
    unsigned ng = rd32(mgr + 0x24), garr = rd32(mgr + 0x30);
    for (unsigned g = 0; g < ng && g < 32; g++) {
        unsigned obj = rd32(garr + 4 * g);
        if (!obj) continue;
        unsigned n = rd32(obj + 0x6C), arr = rd32(obj + 0x78);
        for (unsigned k = 0; k < n && k < 512; k++) {
            unsigned e = rd32(arr + 4 * k);
            if (!e) continue;
            unsigned eep = scen_rd16(e + 4), st = scen_rd16(e + 6), vr = scen_rd16(e + 8);
            if (!all && eep != ep) continue;
            int sfa = (int)rd32(e + 0xC), sfn = (int)rd32(e + 0x10);
            unsigned nx = rd32(e + 0x14);
            int here = eep == ep && st == cur && vr == var;
            if (o + 200 >= size) {
                snprintf(out + o, size - o, "...cut");
                return;
            }
            o += snprintf(out + o, size - o, "%s[%u.%u] ep %u stage %u var %u need SF %d (now %d) and not SF %d (now %d) -> next %d (flags %d)\n", here ? "*" : " ", g, k, eep, st, vr, sfa,
                          sfa >= 0 ? scen_get(sfa) : -1, sfn, sfn >= 0 ? scen_get(sfn) : -1, (int)nx, scen_stage_flags(mgr, nx));
        }
    }
}

/* ---- stage 6j: the background loader sBGLoad (static work of 6j, notes\stage6.md section 37) ---------------------------------------------------------------------------------------------------
 * What streams the next stage: sBGLoad (global pointer 0x1558204) runs FUN_0046A350 every frame for the VIEWPORT-0 player (the local human): it takes the player's world position (+0xE0) and the floor ids
 * the player stands on (+0x3F54 stage, +0x3F58 block, set by the floor ray cast in the character update), tests the BGLoad zones of the blocks that are loaded (resource `sNNNN_NN_bgl`, class
 * cZoneContentsBGLoad: one or more (stage, block) pairs per zone) and keeps up to five (episode, stage, block) triples in the list at +0x4B84 (the previous frame's copy at +0x4B48, the stages whose
 * resources are in flight at +0x4BC0). For every triple it calls FUN_00468310(this, episode, stage, block, 1), which queues the archives of the block (18 kinds, queue at +0x2C6C, 0x5A entries x 0x14);
 * a new STAGE also runs FUN_00422BF0 (stage resources). Blocks leave when the list no longer holds them (FUN_00466C30). The stage the STORY is in (story+0x140380) changes when aEpisodeBase (0x462780,
 * every frame) sees that player SLOT 0 stands on a floor of another stage (+0x3F54 != the area's id): it creates area aS%04d, whose start (sBioArea_vf10) sets the id.
 *   net w bgl              the lists, the zone registrations, the queue and, for every human slot, the floor ids and the world position
 *   net w bgl req <stage> <block> [episode]   the game's own load order for one block (FUN_00468310), as the zone code gives it
 *   net w bgl hit <stage> <block> [...]       the tail of the zone code: blocks added to the current list, unload / stage start / block loads run */
static void scen_bgl_command(const char *arg, char *out, int size)
{
    unsigned bg = rd32(0x1558204), sb = story_block();
    int o = 0;
    if (!bg || !sb) {
        snprintf(out, size, "no sBGLoad / story block");
        return;
    }
    while (*arg == ' ') arg++;
    if (!strncmp(arg, "mirror", 6)) {   /* net w bgl mirror [0|1]: the loader serves both humans (6j) */
        if (arg[6] == ' ' && (arg[7] == '0' || arg[7] == '1')) g_bgl_mirror = arg[7] - '0';
        char rs[160], ms[160], ls[160];
        bgl_fmt(g_bgl.remote, g_bgl.remote_n, rs, sizeof rs), bgl_fmt(g_bgl.mo, g_bgl.mo_n, ms, sizeof ms), bgl_fmt(g_bgl_local, g_bgl_local_n, ls, sizeof ls);
        snprintf(out, size, "loader mirror %s (hook %s, unload step called %u times, %u with the loader as this): my own list %s, the partner's %s, merged-only %s; merged %u, no room %u, dropped %u, slot writes %u; lists sent %u, received %u",
                 g_bgl_mirror ? "ON" : "off", o_bgl_unload ? "installed" : "NOT installed", g_bgl_calls, g_bgl_self_ok, ls, rs, ms, g_bgl.merged, g_bgl.overflow, g_bgl.removed, g_bgl_writes, g_bgl_tx, g_bgl_rx);
        return;
    }
    if (!strncmp(arg, "pupzones", 8)) {   /* net w bgl pupzones [0|1]: a partner's puppet fires trigger zones on this PC too (6j) */
        if (arg[8] == ' ' && (arg[9] == '0' || arg[9] == '1')) g_pup_zones = arg[9] - '0';
        snprintf(out, size, "puppets in trigger zones %s (hook %s): %u checks shown the puppet as a local human", g_pup_zones ? "ON" : "off", o_btn_cand ? "installed" : "NOT installed", g_pup_flips);
        return;
    }
    if (!strncmp(arg, "gate", 4)) {   /* net w bgl gate [0|1]: the story stage change waits until every human stands on the new stage's floor (6j) */
        if (arg[4] == ' ' && (arg[5] == '0' || arg[5] == '1')) g_stage_gate = arg[5] - '0';
        snprintf(out, size, "stage gate %s (hook %s): holds %u frames, released %u, timeouts %u (limit %u ms)", g_stage_gate ? "ON" : "off", o_epi_upd ? "installed" : "NOT installed", g_gate_holds, g_gate_releases, g_gate_timeouts, BGL_GATE_MS);
        return;
    }
    if (!strncmp(arg, "hit", 3)) {   /* net w bgl hit <stage> <block> [<stage> <block> ...]: what the zone code does after a zone hit, with the named blocks added to the current list (up to five in all) */
        int t[5][2], n = 0, k = 0;
        unsigned ep = rd32(sb + 0x140378);
        for (int i = 0; i < 5; i++) {
            int s = (int)rd32(bg + 0x4B84 + 12 * (unsigned)i + 4), b = (int)rd32(bg + 0x4B84 + 12 * (unsigned)i + 8);
            if (s >= 0 && b >= 0) t[n][0] = s, t[n][1] = b, n++;
        }
        const char *q = arg + 3;
        int s2, b2, c2;
        while (sscanf(q, "%d %d%n", &s2, &b2, &c2) == 2) {
            q += c2;
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= t[i][0] == s2 && t[i][1] == b2;
            if (dup) continue;
            if (n >= 5 || s2 < 100 || s2 > 5000 || b2 < 0 || b2 > 63) {
                snprintf(out, size, "usage: net w bgl hit <stage> <block> [...]  (the list holds five blocks; now %d)", n);
                return;
            }
            t[n][0] = s2, t[n][1] = b2, n++;
            k++;
        }
        unsigned neg = 0xFFFFFFFFu;
        for (int i = 0; i < 15; i++) wr(bg + 0x4B48 + 4 * (unsigned)i, (const void *)&(unsigned){rd32(bg + 0x4B84 + 4 * (unsigned)i)}, 4);   /* previous := current */
        for (int i = 0; i < 5; i++) {
            unsigned e = i < n ? ep : neg, s = i < n ? (unsigned)t[i][0] : neg, b = i < n ? (unsigned)t[i][1] : neg;
            wr(bg + 0x4B84 + 12 * (unsigned)i, &e, 4), wr(bg + 0x4B84 + 12 * (unsigned)i + 4, &s, 4), wr(bg + 0x4B84 + 12 * (unsigned)i + 8, &b, 4);
        }
        unsigned a1[1] = {bg};
        do_call(2, 0x466C30, a1, 1);   /* unload what the previous list had and the new one does not */
        do_call(2, 0x4668F0, a1, 1);   /* new stages: start positions + stage resources */
        for (int i = 0; i < n; i++) {
            unsigned a[5] = {bg, ep, (unsigned)t[i][0], (unsigned)t[i][1], 1};
            do_call(2, 0x468310, a, 5);
        }
        snprintf(out, size, "the current list is now %d blocks (%d new): the zone code's tail (unload, stage start, block loads) ran", n, k);
        o = (int)strlen(out);
        out[o++] = '\n';
    } else if (!strncmp(arg, "req", 3)) {
        unsigned st = 0, bl = 0, ep = rd32(sb + 0x140378);
        if (sscanf(arg + 3, "%u %u %u", &st, &bl, &ep) < 2 || st > 5000 || bl > 63 || ep > 9) {
            snprintf(out, size, "usage: net w bgl req <stage> <block> [episode]   (stage 100..5000, block 0..63)");
            return;
        }
        unsigned a[5] = {bg, ep, st, bl, 1};
        do_call(2, 0x468310, a, 5);
        snprintf(out, size, "FUN_00468310 asked for episode %u stage %u block %u (queue below)", ep, st, bl);
        o = (int)strlen(out);
        out[o++] = '\n';
    }
    o += snprintf(out + o, size - o, "sBGLoad %08x ; story: episode %u stage %u variant %u scene %u ; local slot (gm+0x7A8) %u\n", bg, rd32(sb + 0x140378), rd32(sb + 0x140380), rd32(sb + 0x140384),
                  rd32(sb + 0x140388), rd32(rd32(0x157AE00) + 0x7A8));
    static const struct { const char *name; unsigned off; } lists[3] = {{"current  (+0x4B84)", 0x4B84}, {"previous (+0x4B48)", 0x4B48}, {"stages   (+0x4BC0)", 0x4BC0}};
    for (int l = 0; l < 3; l++) {
        o += snprintf(out + o, size - o, "%s", lists[l].name);
        for (int i = 0; i < 5; i++) {
            int e = (int)rd32(bg + lists[l].off + 12 * (unsigned)i), s = (int)rd32(bg + lists[l].off + 12 * (unsigned)i + 4), b = (int)rd32(bg + lists[l].off + 12 * (unsigned)i + 8);
            if (s < 0 && b < 0) o += snprintf(out + o, size - o, " [-]");
            else o += snprintf(out + o, size - o, " [%d,%d,%d]", e, s, b);
        }
        o += snprintf(out + o, size - o, "\n");
    }
    int nz = 0;
    o += snprintf(out + o, size - o, "zone registrations (+0x20, 16 bytes: episode stage block zone#):");
    for (unsigned i = 0; i < 90 && o + 60 < size; i++) {
        unsigned z = rd32(bg + 0x20 + 16 * i + 12);
        if (z == 0xFFFFFFFFu) continue;
        o += snprintf(out + o, size - o, " [%d,%d,%d]#%u", (int)rd32(bg + 0x20 + 16 * i), (int)rd32(bg + 0x20 + 16 * i + 4), (int)rd32(bg + 0x20 + 16 * i + 8), z);
        nz++;
    }
    o += snprintf(out + o, size - o, " (%d)\nqueue (+0x2C6C: kind episode stage block):", nz);
    int nq = 0;
    for (unsigned i = 0; i < 90 && o + 60 < size; i++) {
        unsigned e = bg + 0x2C6C + 20 * i;
        int s = (int)rd32(e + 8), b = (int)rd32(e + 12);
        if (s < 0 && b < 0) continue;
        o += snprintf(out + o, size - o, " %d:%d/%d/%d", (int)rd32(e), (int)rd32(e + 4), s, b);
        nq++;
    }
    o += snprintf(out + o, size - o, " (%d)\n", nq);
    for (int slot = 0; slot < 8 && o + 200 < size; slot++) {
        unsigned p = player_obj(slot);
        if (!p) continue;
        /* the zone test of the game (cAreaHit_vf8) takes the position from the object's coordinate matrix: vtable slot 0x50 (thiscall, -1 = the object's own), translation in floats 12..14 */
        unsigned fn = rd32(rd32(p) + 0x50), mp = 0;
        float mx = 0, my = 0, mz = 0;
        if (fn > 0x400000 && fn < 0x1400000) {
            unsigned ca[2] = {p, 0xFFFFFFFFu};
            mp = do_call(2, fn, ca, 2);
            if (mp) mx = rdf(mp + 48), my = rdf(mp + 52), mz = rdf(mp + 56);
        }
        o += snprintf(out + o, size - o, "human slot %d %08x type %u (flags+C %08x, slot type +7920 %u, seat +7924 %08x) pos (%.0f,%.1f,%.0f) +E0 (%.0f,%.1f,%.0f) coord matrix %08x (%.0f,%.1f,%.0f) floor stage %d block %d\n", slot, p,
                      rd32(p + 0xC) & 7, rd32(p + 0xC), rd32(p + 0x7920), rd32(p + 0x7924), rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), rdf(p + 0xE0), rdf(p + 0xE4), rdf(p + 0xE8), mp, mx, my, mz,
                      (int)rd32(p + 0x3F54), (int)rd32(p + 0x3F58));
    }
}

/* ---- stage 6j: which trigger zones does the shape test of the game say a human is in? (puppet presence research) -----------------------------------------------------------------------------------
 * sAreaHit (singleton pointer 0x1578D34, +0x28 count, +0x34 array of cAreaHit zones) runs FUN_00824B90 for every enabled zone every frame: for each player slot in the zone's mask it asks the zone's virtual 0x20
 * (cAreaHit_vf8: coordinate matrix of the object -> shape test) and, on a hit, ORs 0x100 | (1 << character) into zone+0x10. `net w zonetest [slot|all]` asks the same question once for the human(s) of this PC:
 * which enabled zones contain the object. It answers whether a PUPPET (type 2 body of the partner) is inside a zone by the game's own test, independent of what the zone's consumer does with it. */
static void scen_zonetest_command(const char *arg, char *out, int size)
{
    unsigned sa = rd32(0x1578D34);
    int o = 0, only = -1;
    if (!sa) {
        snprintf(out, size, "no sAreaHit");
        return;
    }
    while (*arg == ' ') arg++;
    if (arg[0] >= '0' && arg[0] <= '7') only = arg[0] - '0';
    unsigned n = rd32(sa + 0x28), arr = rd32(sa + 0x34);
    if (n > 600) n = 600;
    o += snprintf(out + o, size - o, "sAreaHit %08x: %u zones\n", sa, n);
    for (int slot = 0; slot < 8; slot++) {
        unsigned p = player_obj(slot);
        if (!p || (only >= 0 && only != slot)) continue;
        int hits = 0, en = 0;
        o += snprintf(out + o, size - o, "slot %d %08x type %u at (%.0f,%.1f,%.0f): zones containing it:", slot, p, rd32(p + 0xC) & 7, rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48));
        for (unsigned i = 0; i < n; i++) {
            unsigned z = rd32(arr + 4 * i);
            if (!z) continue;
            unsigned char enabled = 0;
            r2_safe_read((void *)(z + 0xC), &enabled, 1);
            if (!enabled) continue;
            en++;
            unsigned fn = rd32(rd32(z) + 0x20);
            if (fn < 0x400000 || fn > 0x1400000) continue;
            unsigned a[2] = {z, p};
            if ((do_call(2, fn, a, 2) & 0xFF) == 1) {
                hits++;
                if (o + 100 < size) o += snprintf(out + o, size - o, " #%u(%08x flags %08x res %08x)", i, z, rd32(z + 0x18), rd32(z + 0x10));
            }
        }
        o += snprintf(out + o, size - o, " [%d of %d enabled]\n", hits, en);
    }
}

/* ---- stage 6j: the loader serves both humans (docs\world.md "6j", logic and tests: bgl_logic.h, tests\test_bgl.c) ----------------------------------------------------------------------------------
 * Hook on the unload step of the block streamer (`FUN_00466C30`, once a frame, ecx = sBGLoad): before the engine unloads what the previous frame's list had and the current one has not, the partner's own list
 * (received as EV_W_BGL) is put into the free slots of the current list and the loader is asked for those blocks (FUN_00468310, what the zone code itself calls). This PC's own list (without the merged-only
 * entries) is sent to the partner when it changes (10 Hz scan in scen_tick) and every 3 s. `net w bgl mirror 0|1` switches it (default on; it only acts while a session runs). */
static __attribute__((thiscall)) void h_bgl_unload(unsigned self)
{
    g_bgl_calls++;
    if (g_bgl_mirror && self && self == rd32(0x1558204)) {
        g_bgl_self_ok++;
        if (w_session_running()) {
            bgl_t cur[BGL_SLOTS], prev[BGL_SLOTS], cur2[BGL_SLOTS], ask[BGL_SLOTS], local[BGL_SLOTS];
            int ask_n = 0, ln = 0;
            bgl_read(self, 0x4B84, cur), bgl_read(self, 0x4B48, prev);
            if (bgl_merge(&g_bgl, cur, prev, cur2, local, &ln, ask, &ask_n))
                for (unsigned i = 0; i < BGL_SLOTS; i++)
                    if (cur2[i].ep != cur[i].ep || cur2[i].stage != cur[i].stage || cur2[i].block != cur[i].block) {
                        wr(self + 0x4B84 + 12 * i, &cur2[i], 12);
                        g_bgl_writes++;
                    }
            memcpy(g_bgl_local, local, sizeof local);
            g_bgl_local_n = ln;
            for (int i = 0; i < ask_n; i++) {
                unsigned a[5] = {self, (unsigned)ask[i].ep, (unsigned)ask[i].stage, (unsigned)ask[i].block, 1};
                do_call(2, 0x468310, a, 5);
            }
        }
    }
    o_bgl_unload(self);
}

static void bgl_event(const cp_event *e)
{
    bgl_t l[BGL_SLOTS];
    int c = bgl_unpack(e->body, e->len, l);
    if (c < 0) return;
    int same = c == g_bgl.remote_n;
    for (int i = 0; same && i < c; i++) same = l[i].ep == g_bgl.remote[i].ep && l[i].stage == g_bgl.remote[i].stage && l[i].block == g_bgl.remote[i].block;
    for (int i = 0; i < c; i++) g_bgl.remote[i] = l[i];
    g_bgl.remote_n = c;
    g_bgl_rx++;
    if (!same && g_bgl_logs < 60) {
        char s[160];
        bgl_fmt(l, c, s, sizeof s);
        g_bgl_logs++;
        r2_log(W_LOG "bgl: the partner's loader list is now %s (merged %u, no room %u, dropped %u)", s, g_bgl.merged, g_bgl.overflow, g_bgl.removed);
    }
}

static void bgl_tick(unsigned now)
{
    int running = w_session_running();
    if (!running) {
        if (g_bgl_was_running) {   /* the session ended: forget the partner's list (the merged-only entries leave with the engine's next zone hit) */
            g_bgl.remote_n = 0, g_bgl.mo_n = 0, g_bgl_sent_n = -1;
            g_bgl_was_running = 0;
        }
        return;
    }
    if (!g_bgl_was_running) g_bgl_was_running = 1, g_bgl_sent_n = -1;
    if (!g_bgl_mirror || now - g_bgl_scan_t < 100) return;
    g_bgl_scan_t = now;
    int same = g_bgl_local_n == g_bgl_sent_n;
    for (int i = 0; same && i < g_bgl_local_n; i++) same = g_bgl_local[i].ep == g_bgl_sent[i].ep && g_bgl_local[i].stage == g_bgl_sent[i].stage && g_bgl_local[i].block == g_bgl_sent[i].block;
    if (same && now - g_bgl_sent_t < 3000) return;
    if (!g_bgl_calls) return;
    unsigned char b[40];
    int len = bgl_pack(g_bgl_local, g_bgl_local_n, b);
    w_ev_send(EV_W_BGL, b, (unsigned)len);
    if (!same && g_bgl_logs < 60) {
        char s[160];
        bgl_fmt(g_bgl_local, g_bgl_local_n, s, sizeof s);
        g_bgl_logs++;
        r2_log(W_LOG "bgl: my loader list is now %s (sent)", s);
    }
    memcpy(g_bgl_sent, g_bgl_local, sizeof g_bgl_local);
    g_bgl_sent_n = g_bgl_local_n;
    g_bgl_sent_t = now;
    g_bgl_tx++;
}

static __attribute__((thiscall)) void h_epi_update(unsigned self)
{
    int faked = 0, cur = 0;
    if (g_stage_gate && self && w_session_running()) {
        unsigned p0 = player_obj(0);
        int target = p0 ? (int)rd32(p0 + 0x3F54) : -1;
        cur = (int)rd32(self + 0x30);
        if (target != -1 && target != 0 && target != cur) {   /* the area update is about to switch to the stage of slot 0's floor */
            int wait_for = -1;
            for (int slot = 1; slot < 8; slot++) {
                unsigned p = player_obj(slot);
                if (!p || (int)rd32(p + 0x1A08) <= 0 || (rd32(p + 0x7920) & 0xFF) == 1) continue;   /* none / dead / the AI partner */
                if ((int)rd32(p + 0x3F54) != target) { wait_for = slot; break; }
            }
            unsigned now = GetTickCount();
            if (wait_for >= 0) {
                if (!g_gate_since) g_gate_since = now ? now : 1;
                if (now - g_gate_since < BGL_GATE_MS) {
                    wr(self + 0x30, &target, 4);
                    faked = 1;
                    if (!g_gate_holds && g_gate_logs < 30) g_gate_logs++, r2_log(W_LOG "stage gate: slot 0 stands on a stage %d floor (area %d), the human of slot %d does not yet: the change waits", target, cur, wait_for);
                    g_gate_holds++;
                } else if (!g_gate_timeouts || now - g_gate_since < BGL_GATE_MS + 500) {
                    g_gate_timeouts++;
                    if (g_gate_logs < 30) g_gate_logs++, r2_log(W_LOG "stage gate: waited %u ms for the human of slot %d, the change goes ahead", now - g_gate_since, wait_for);
                }
            } else {
                if (g_gate_since) {
                    g_gate_releases++;
                    if (g_gate_logs < 30) g_gate_logs++, r2_log(W_LOG "stage gate: every human stands on a stage %d floor (waited %u ms): the change goes ahead", target, now - g_gate_since);
                }
                g_gate_since = 0;
            }
        } else if (target == cur) g_gate_since = 0;
    }
    o_epi_upd(self);
    if (faked) wr(self + 0x30, &cur, 4);
}

static __attribute__((thiscall)) void h_btn_cand(unsigned self, unsigned button)
{
    unsigned p = 0;
    int flipped = 0;
    if (g_pup_zones && g_cs && w_session_running()) {
        unsigned slot = rd32(self + 8);
        p = slot < 8 ? player_obj((int)slot) : 0;
        if (p && p != player_obj(g_my_slot) && rd32(p + 0x7920) == 2 && rd32(button + 0x20) == 3) {   /* button + 0x20 = the kind (case 3 of FUN_0040AF90: zone hit bits + callback = flag zones and AreaHit zones); door prompts and pickups are other kinds and keep their stock rules */
            *(volatile unsigned *)(p + 0x7920) = 0;
            flipped = 1;
            if (!g_pup_flips && g_pup_logs < 5) g_pup_logs++, r2_log(W_LOG "trigger zones: the partner's puppet (slot %u) is in the button %08x's zone: shown as a local human for the check", slot, button);
            g_pup_flips++;
        }
    }
    o_btn_cand(self, button);
    if (flipped) *(volatile unsigned *)(p + 0x7920) = 2;
}

static void scen_install(int *ok)
{
    static const unsigned char b_unload[] = {0x83, 0xEC, 0x08, 0x53, 0x55};   /* sub esp, 8; push ebx; push ebp */
    void *t = w_tramp(0x466C30, b_unload, 5);
    if (t) {
        o_bgl_unload = (bgl_unload_t)t;
        *ok += w_jmp(0x466C30, 5, (void *)h_bgl_unload);
    }
    static const unsigned char b_epi[] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1};   /* sub esp, 8; push esi; mov esi, ecx */
    t = w_tramp(0x462780, b_epi, 6);
    if (t) {
        o_epi_upd = (epi_upd_t)t;
        *ok += w_jmp(0x462780, 6, (void *)h_epi_update);
    }
    static const unsigned char b_btn[] = {0x51, 0x57, 0x8B, 0xF9, 0x80, 0x7F, 0x04, 0x00};   /* push ecx; push edi; mov edi, ecx; cmp byte [edi+4], 0 */
    t = w_tramp(0x40BFB0, b_btn, 8);
    if (t) {
        o_btn_cand = (btn_cand_t)t;
        *ok += w_jmp(0x40BFB0, 8, (void *)h_btn_cand);
    }
}
