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
 *   net w bgl req <stage> <block> [episode]   the game's own load order for one block (FUN_00468310), as the zone code gives it */
static void scen_bgl_command(const char *arg, char *out, int size)
{
    unsigned bg = rd32(0x1558204), sb = story_block();
    int o = 0;
    if (!bg || !sb) {
        snprintf(out, size, "no sBGLoad / story block");
        return;
    }
    while (*arg == ' ') arg++;
    if (!strncmp(arg, "req", 3)) {
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
        o += snprintf(out + o, size - o, "human slot %d %08x type %u pos (%.0f,%.1f,%.0f) +E0 (%.0f,%.1f,%.0f) coord matrix %08x (%.0f,%.1f,%.0f) floor stage %d block %d\n", slot, p, rd32(p + 0xC) & 7,
                      rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), rdf(p + 0xE0), rdf(p + 0xE4), rdf(p + 0xE8), mp, mx, my, mz, (int)rd32(p + 0x3F54), (int)rd32(p + 0x3F58));
    }
}
