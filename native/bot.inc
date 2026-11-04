/* bot.inc: stage 6c test aid, a waypoint walker inside the game (no console round trips). Included from coop3.c after world.inc.
 * It drives one game pad slot with a synthetic XInput state (testmode_pad_set) and steers by closed loop on the position read-out: the stick angle relative to the
 * camera is the unknown, so the camera heading is estimated from the real displacement (heading = camera + s * stick angle) after a two-step calibration
 * (forward, then right) that also finds the handedness s. A blocked walker wiggles sideways and presses the interact key now and then (doors, prompts).
 *   net bot go <x> <z> [<x> <z> ...]   walk the route (up to 48 points); net bot stop; net bot status; net bot slot <n>; net bot radius <units>; net bot shots <ms>|off;
 *   net bot fire <ms>                  hold aim (LT) and tap fire (RT) for that long; net bot key <name>: interact key for stuck handling (default f)
 * Used by the stage 6c test scripts (lab\s6c_*.sh); never active unless a console command starts it. */
#define BOT_MAXWP 48
static struct {
    int on, slot, n, idx, calib, have_cam;
    float wx[BOT_MAXWP], wz[BOT_MAXWP], radius, cam, s, phi;
    unsigned t_wp, t_meas, t_stuck, t_key, t_log, t_wig, t_shot;
    float mx, mz;     /* position at the last displacement measurement */
    float cx, cz;     /* position at the calibration step start */
    float h0;
    unsigned shot_ms, wig_until;
    float wig_off;
    int wig_dir;
    unsigned fails;
    int key_dik, key_vk;
    unsigned buttons;   /* extra pad buttons held while walking (sprint etc.) */
    int stat_wp_done, stat_stuck, stat_keys;
    unsigned fire_until, fire_start;   /* net bot fire <ms>: LT held (aim), RT tapped every 450 ms */
} g_bot = {.slot = 0, .radius = 70.0f, .key_dik = 0, .shot_ms = 0};

static float bot_norm(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } bot_gamepad_t;   /* the XINPUT_GAMEPAD layout */

static void bot_pad_send(bot_gamepad_t *g)
{
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 1, g);   /* in a co-op session the human's pad is the real-pad path (game user 0), as `rpad` */
    else testmode_pad_set(g_bot.slot, 1, g);
}

static void bot_pad(float phi, int mag, unsigned buttons)
{
    bot_gamepad_t g;
    memset(&g, 0, sizeof g);
    g.wButtons = (WORD)buttons;
    g.sThumbLX = (SHORT)(sinf(phi) * (float)mag);
    g.sThumbLY = (SHORT)(cosf(phi) * (float)mag);
    unsigned now = GetTickCount();
    if ((int)(now - g_bot.fire_until) < 0) {
        g.bLeftTrigger = 255;
        if ((now - g_bot.fire_start) % 450 < 200) g.bRightTrigger = 255;
    }
    bot_pad_send(&g);
}

static void bot_stop(const char *why)
{
    if (g_bot.on) r2_log("bot: stopped (%s) after %d waypoints, %d stuck events, %d interact presses", why, g_bot.stat_wp_done, g_bot.stat_stuck, g_bot.stat_keys);
    g_bot.on = 0;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 0, NULL);
    else testmode_pad_set(g_bot.slot, 0, NULL);
}

static void bot_press_interact(void)
{
    if (!g_bot.key_dik) inject_key_lookup("f", &g_bot.key_dik, &g_bot.key_vk);
    inject_key(g_bot.key_dik, 150);
    g_bot.stat_keys++;
}

static void bot_frame(void)
{
    if (!g_bot.on) {
        if (g_bot.fire_until) {   /* fire-only mode: no walking, the triggers only */
            if ((int)(GetTickCount() - g_bot.fire_until) >= 0) {
                g_bot.fire_until = 0;
                bot_stop("fire done");
            } else bot_pad(0.0f, 0, 0);
        }
        return;
    }
    unsigned now = GetTickCount();
    unsigned p = player_obj(g_bot.slot);
    if (!p) return;
    float x = rdf(p + 0x40), z = rdf(p + 0x48);
    if (g_bot.shot_ms && now - g_bot.t_shot >= g_bot.shot_ms) {
        g_bot.t_shot = now;
        d3d_request_shot();
    }
    if (now - g_bot.t_log >= 1500) {
        g_bot.t_log = now;
        r2_log("bot: pos (%.0f,%.0f,%.0f) act %d hp %d -> wp %d/%d (%.0f,%.0f) cam %.0f deg s %.0f calib %d", x, z, rdf(p + 0x44), (int)rd32(p + 0x1A54), (int)rd32(p + 0x1A08), g_bot.idx + 1, g_bot.n, g_bot.wx[g_bot.idx], g_bot.wz[g_bot.idx],
               g_bot.cam * 57.2958f, g_bot.s, g_bot.calib);
    }
    float tx = g_bot.wx[g_bot.idx], tz = g_bot.wz[g_bot.idx], dx = tx - x, dz = tz - z, dist = sqrtf(dx * dx + dz * dz);
    if (dist < g_bot.radius) {
        g_bot.stat_wp_done++;
        r2_log("bot: waypoint %d (%.0f,%.0f) reached at (%.0f,%.0f)", g_bot.idx + 1, tx, tz, x, z);
        if (++g_bot.idx >= g_bot.n) {
            bot_stop("route done");
            return;
        }
        g_bot.t_wp = now, g_bot.t_stuck = 0;
        return;
    }
    if (now - g_bot.t_wp > 40000) {
        r2_log("bot: waypoint %d timed out (40 s), skipping", g_bot.idx + 1);
        if (++g_bot.idx >= g_bot.n) {
            bot_stop("route done (last waypoint timed out)");
            return;
        }
        g_bot.t_wp = now;
        return;
    }
    if (now - g_bot.t_meas < 150) return;   /* control rate ~6 Hz */
    unsigned dt = now - g_bot.t_meas;
    float mx = x - g_bot.mx, mz = z - g_bot.mz, moved = sqrtf(mx * mx + mz * mz);
    /* calibration: 0 = walk forward 700 ms, 1 = stick right 700 ms (needs real displacement), 2 = servo */
    if (g_bot.calib < 2) {
        if (!g_bot.t_wig) {
            g_bot.t_wig = now, g_bot.cx = x, g_bot.cz = z;
            bot_pad(g_bot.calib == 0 ? 0.0f : 1.5707963f, 32000, g_bot.buttons);
            g_bot.t_meas = now, g_bot.mx = x, g_bot.mz = z;
            return;
        }
        if (now - g_bot.t_wig < 750) return;
        float ex = x - g_bot.cx, ez = z - g_bot.cz, d = sqrtf(ex * ex + ez * ez);
        g_bot.t_wig = 0;
        if (d < 25.0f) {   /* blocked or still in a cutscene: try again, interact in case it is a door */
            if (++g_bot.fails % 3 == 0) bot_press_interact();
            if (g_bot.fails > 12) bot_stop("calibration failed: no displacement");
            return;
        }
        float h = atan2f(ez, ex);
        if (g_bot.calib == 0) g_bot.h0 = h, g_bot.calib = 1;
        else {
            float dh = bot_norm(h - g_bot.h0);
            if (fabsf(dh) < 0.6f || fabsf(dh) > 2.5f) {   /* not a quarter turn: the camera moved or a wall bent the path; again from the start */
                r2_log("bot: calibration step gave %.0f deg, repeating", dh * 57.2958f);
                g_bot.calib = 0;
                return;
            }
            g_bot.s = dh > 0 ? 1.0f : -1.0f;
            g_bot.cam = g_bot.h0;
            g_bot.calib = 2, g_bot.have_cam = 1;
            r2_log("bot: calibrated, camera heading %.0f deg, handedness %.0f", g_bot.cam * 57.2958f, g_bot.s);
        }
        g_bot.t_meas = now, g_bot.mx = x, g_bot.mz = z;
        return;
    }
    /* servo: update the camera estimate from the displacement of the last interval, then steer */
    if (moved > 12.0f && dt > 0) {
        float h = atan2f(mz, mx), cam = h - g_bot.s * g_bot.phi;
        g_bot.cam = bot_norm(g_bot.cam + 0.7f * bot_norm(cam - g_bot.cam));
        g_bot.t_stuck = 0;
    } else {
        if (!g_bot.t_stuck) g_bot.t_stuck = now;
    }
    g_bot.t_meas = now, g_bot.mx = x, g_bot.mz = z;
    float want = atan2f(dz, dx), phi = g_bot.s * bot_norm(want - g_bot.cam);
    if (g_bot.t_stuck && (int)(now - g_bot.t_stuck) > 900 && (int)(now - g_bot.wig_until) >= 0) {   /* blocked: leave the straight line to one side for 1.5 s, the side flips and the angle grows with every failure; press interact (a door / prompt) every 2 s */
        g_bot.stat_stuck++;
        g_bot.wig_dir = g_bot.wig_dir > 0 ? -1 : 1;
        g_bot.wig_off = (float)g_bot.wig_dir * (1.0f + 0.5f * (float)(g_bot.stat_stuck % 4));
        g_bot.wig_until = now + 1500;
        if (g_bot.stat_stuck % 3 == 0 && g_bot.n < BOT_MAXWP) {   /* still stuck after three sidesteps: insert a detour point 350 units to the side of the straight line (alternating) and go there first */
            float len = dist > 1.0f ? dist : 1.0f, px = -dz / len, pz = dx / len, side = (g_bot.stat_stuck / 3) % 2 ? 1.0f : -1.0f;
            for (int k = g_bot.n; k > g_bot.idx; k--) g_bot.wx[k] = g_bot.wx[k - 1], g_bot.wz[k] = g_bot.wz[k - 1];
            g_bot.wx[g_bot.idx] = x + px * 350.0f * side + dx / len * 120.0f, g_bot.wz[g_bot.idx] = z + pz * 350.0f * side + dz / len * 120.0f;
            g_bot.n++;
            g_bot.t_wp = now;
            r2_log("bot: stuck at (%.0f,%.0f), detour point (%.0f,%.0f) inserted", x, z, g_bot.wx[g_bot.idx], g_bot.wz[g_bot.idx]);
        }
        g_bot.t_stuck = now + 1500;
        if (now - g_bot.t_key > 2000) g_bot.t_key = now, bot_press_interact();
    }
    if ((int)(now - g_bot.wig_until) < 0) phi += g_bot.wig_off;
    g_bot.phi = phi;
    bot_pad(phi, 32000, g_bot.buttons);
}

static void bot_command(const char *rest, char *out, int size)
{
    char sub[16] = "";
    sscanf(rest, "%15s", sub);
    const char *arg = rest + strlen(sub);
    while (*arg == ' ') arg++;
    if (!strcmp(sub, "go")) {
        float v[2 * BOT_MAXWP];
        int n = 0;
        const char *q = arg;
        char *end;
        for (float f = strtof(q, &end); end != q && n < 2 * BOT_MAXWP; f = strtof(q, &end)) v[n++] = f, q = end;
        if (n < 2) {
            snprintf(out, size, "usage: net bot go <x> <z> [<x> <z> ...]");
            return;
        }
        g_bot.n = n / 2;
        for (int i = 0; i < g_bot.n; i++) g_bot.wx[i] = v[2 * i], g_bot.wz[i] = v[2 * i + 1];
        g_bot.idx = 0, g_bot.on = 1, g_bot.calib = g_bot.have_cam ? 2 : 0, g_bot.t_wp = GetTickCount(), g_bot.t_meas = 0, g_bot.t_wig = 0, g_bot.t_stuck = 0, g_bot.fails = 0;
        g_bot.stat_wp_done = g_bot.stat_stuck = g_bot.stat_keys = 0;
        unsigned p = player_obj(g_bot.slot);
        if (p) g_bot.mx = rdf(p + 0x40), g_bot.mz = rdf(p + 0x48);
        g_bot.phi = 0;
        r2_log("bot: route of %d points started from (%.0f,%.0f), slot %d", g_bot.n, g_bot.mx, g_bot.mz, g_bot.slot);
        snprintf(out, size, "bot: route of %d waypoints, slot %d%s", g_bot.n, g_bot.slot, g_bot.have_cam ? " (camera known)" : " (calibrating)");
    } else if (!strcmp(sub, "stop")) {
        bot_stop("console");
        snprintf(out, size, "bot stopped");
    } else if (!strcmp(sub, "slot")) {
        g_bot.slot = atoi(arg) & 3;
        snprintf(out, size, "bot pad slot %d", g_bot.slot);
    } else if (!strcmp(sub, "radius")) {
        if (atof(arg) > 5.0) g_bot.radius = (float)atof(arg);
        snprintf(out, size, "bot waypoint radius %.0f", g_bot.radius);
    } else if (!strcmp(sub, "shots")) {
        g_bot.shot_ms = (unsigned)atoi(arg);
        snprintf(out, size, "bot screenshots every %u ms", g_bot.shot_ms);
    } else if (!strcmp(sub, "fire")) {   /* net bot fire <ms>: hold aim (LT) and tap fire (RT) for that long, also while walking (pad slot / co-op: the local human's real-pad path) */
        unsigned ms = (unsigned)atoi(arg);
        g_bot.fire_start = GetTickCount(), g_bot.fire_until = ms ? g_bot.fire_start + ms : 0;
        snprintf(out, size, ms ? "bot: aim + fire for %u ms" : "bot: fire stopped", ms);
    } else if (!strcmp(sub, "recal")) {
        g_bot.have_cam = 0;
        snprintf(out, size, "bot will calibrate again at the next route");
    } else if (!strcmp(sub, "buttons")) {
        g_bot.buttons = (unsigned)strtoul(arg, NULL, 16);
        snprintf(out, size, "bot extra buttons %04x", g_bot.buttons);
    } else if (!strcmp(sub, "key")) {
        char k[32] = "f";
        sscanf(arg, "%31s", k);
        if (inject_key_lookup(k, &g_bot.key_dik, &g_bot.key_vk)) snprintf(out, size, "bot interact key %s", k);
        else snprintf(out, size, "unknown key %s", k);
    } else {
        unsigned p = player_obj(g_bot.slot);
        snprintf(out, size, "bot %s, slot %d, waypoint %d/%d, camera %.0f deg (known %d, handedness %.0f), radius %.0f, pos (%.0f,%.0f), done %d, stuck events %d, interact presses %d", g_bot.on ? "ON" : "off", g_bot.slot, g_bot.idx + 1, g_bot.n,
                 g_bot.cam * 57.2958f, g_bot.have_cam, g_bot.s, g_bot.radius, p ? rdf(p + 0x40) : 0.0f, p ? rdf(p + 0x48) : 0.0f, g_bot.stat_wp_done, g_bot.stat_stuck, g_bot.stat_keys);
    }
}
