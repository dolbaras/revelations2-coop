/* test_aim.c: offline checks of native/aim_logic.h (the LAB aim aid `net aim`: the shot vector of this PC's human is turned to a target point).
 * Built a second time with -DAIM_MODEL_BEFORE_FIX (the lab tool before `net aim`: only the body was turned, the shot vector stayed the pad's): that model must fail
 * (tests/build.sh), the way the main-PC bench gun1 (2026-11-08 00:18) did: 5 rounds at an enemy lying 300 units away, 0 hits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../native/aim_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned g_rng = 4242;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static float len3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

int main(void)
{
    /* 1. random geometry: after the redirect the ray goes through the target, the length (the weapon's range) and w are kept */
    for (int i = 0; i < 20000; i++) {
        float s[3] = {rndf(-20000, 20000), rndf(-500, 3000), rndf(-20000, 20000)};
        float t[3] = {s[0] + rndf(-1500, 1500), s[1] + rndf(-400, 400), s[2] + rndf(-1500, 1500)};
        if (len3((float[3]){t[0] - s[0], t[1] - s[1], t[2] - s[2]}) < 20) continue;
        float L = rndf(50, 6000), a = rndf(-3.14f, 3.14f), p = rndf(-0.6f, 0.6f);
        float d[4] = {L * cosf(p) * sinf(a), L * sinf(p), L * cosf(p) * cosf(a), 0.0f};
        float w = rndf(-1, 1);
        d[3] = w;
        float turn = -1;
        int r = aim_redirect(s, d, t, &turn);
        CHECK(r == 1);
        CHECK(fabsf(len3(d) - L) <= L * 1e-4f);
        CHECK(d[3] == w);
        float miss = aim_ray_miss(s, d, t);
        CHECK(miss >= 0 && miss < 0.05f + 1e-4f * len3((float[3]){t[0] - s[0], t[1] - s[1], t[2] - s[2]}));
        CHECK(turn >= 0 && turn <= 180.0f);
    }
    /* 2. the gun1 bench: Barry's muzzle at 150 units height, the pad's level shot (pitch 0) toward an enemy LYING 300 units away (its top joint at y 40):
     *    the level ray passes ~110 units above it; after the aid it hits the head sphere (radius 15) */
    {
        float s[3] = {3987, 1700 + 150, -8266}, head[3] = {3987 + 300, 1700 + 40, -8266};
        float d[4] = {2000, 0, 0, 0};
        float before = aim_ray_miss(s, d, head);
        CHECK(before > 100);   /* the stock vector really misses (true for both models: it is the input) */
        float turn;
        aim_redirect(s, d, head, &turn);
        CHECK(aim_ray_miss(s, d, head) < 15.0f);
        CHECK(turn > 15.0f && turn < 25.0f);   /* atan(110/300) = 20.1 deg */
        CHECK(fabsf(len3(d) - 2000) < 0.5f);
    }
    /* 3. a standing enemy 600 units away, the pad's vector 8 degrees to the side: hit after, miss before */
    {
        float s[3] = {0, 150, 0}, head[3] = {0, 165, 600};
        float a = 8.0f / 57.2957795f;
        float d[4] = {3000 * sinf(a), 0, 3000 * cosf(a), 0};
        CHECK(aim_ray_miss(s, d, head) > 60);
        aim_redirect(s, d, head, NULL);
        CHECK(aim_ray_miss(s, d, head) < 1.0f);
    }
    /* 4. degenerate inputs: no change */
    {
        float s[3] = {10, 20, 30}, d[4] = {1, 2, 3, 4}, keep[4];
        memcpy(keep, d, sizeof d);
        float turn = 5;
        CHECK(aim_redirect(s, d, s, &turn) == 0 && !memcmp(d, keep, sizeof d) && turn == 0);   /* target = start */
        float z[4] = {0, 0, 0, 0}, t[3] = {100, 20, 30};
        CHECK(aim_redirect(s, z, t, NULL) == 0 && z[0] == 0 && z[1] == 0 && z[2] == 0);       /* a zero vector stays zero */
        float nan_t[3] = {NAN, 0, 0};
        memcpy(d, keep, sizeof d);
        CHECK(aim_redirect(s, d, nan_t, NULL) == 0 && !memcmp(d, keep, sizeof d));
        float inf_t[3] = {INFINITY, 0, 0};
        CHECK(aim_redirect(s, d, inf_t, NULL) == 0 && !memcmp(d, keep, sizeof d));
        float zero_d[3] = {0, 0, 0};
        CHECK(aim_ray_miss(s, zero_d, t) < 0);
    }
    /* 5. the highest joint */
    {
        float pos[6][3] = {{0, 10, 0}, {0, 150, 0}, {0, NAN, 0}, {0, 172, 1}, {0, 172, 2}, {0, 80, 0}};
        CHECK(aim_pick_top((const float(*)[3])pos, 6) == 3);
        CHECK(aim_pick_top((const float(*)[3])pos, 0) == -1);
        CHECK(aim_pick_top((const float(*)[3])pos, 3) == 1);
        float allnan[2][3] = {{NAN, 0, 0}, {0, INFINITY, 0}};
        CHECK(aim_pick_top((const float(*)[3])allnan, 2) == -1);
    }
    /* 6. the camera aim (bench g0 calibration): body line 1.658 to the head of (151,0,3) 252 units away -> the camera yaw that hit it was 1.87, pitch 0.084 */
    {
        float h[3] = {6150, 1700, -8410}, t[3] = {6402, 1864, -8432}, yaw = 0, p = 0;
        CHECK(aim_cam_solve(h, t, AIM_CAM_RIGHT, AIM_CAM_EYE, AIM_CAM_PITCH_RAD, &yaw, &p) == 1);
        CHECK(fabsf(yaw - 1.87f) < 0.01f);
        CHECK(fabsf(p - 0.084f) < 0.01f);
        float yaw0 = 0, p0 = 0;   /* with no right offset the camera would look along the body line (the 4-degree miss of the first bench shots) */
        CHECK(aim_cam_solve(h, t, 0.0f, AIM_CAM_EYE, AIM_CAM_PITCH_RAD, &yaw0, &p0) == 1 && fabsf(yaw0 - atan2f(252, -22)) < 1e-4f);
        /* geometry: from the solved camera point the yaw points exactly at t; the pitch stays in [-1, 1]; degenerate inputs refuse */
        for (int i = 0; i < 20000; i++) {
            float hh[3] = {rndf(-20000, 20000), rndf(-500, 3000), rndf(-20000, 20000)};
            float tt[3] = {hh[0] + rndf(-1500, 1500), hh[1] + rndf(-2000, 2000), hh[2] + rndf(-1500, 1500)};
            if (hypotf(tt[0] - hh[0], tt[2] - hh[2]) < 120) continue;
            float y, pp;
            CHECK(aim_cam_solve(hh, tt, AIM_CAM_RIGHT, AIM_CAM_EYE, AIM_CAM_PITCH_RAD, &y, &pp) == 1);
            float cx = hh[0] - AIM_CAM_RIGHT * cosf(y), cz = hh[2] + AIM_CAM_RIGHT * sinf(y);
            float e = atan2f(tt[0] - cx, tt[2] - cz) - y;
            while (e > 3.14159265f) e -= 6.2831853f;
            while (e < -3.14159265f) e += 6.2831853f;
            CHECK(fabsf(e) < 2e-3f);
            CHECK(pp >= -1.0f && pp <= 1.0f);
        }
        float nanp[3] = {NAN, 0, 0};
        CHECK(aim_cam_solve(nanp, t, AIM_CAM_RIGHT, AIM_CAM_EYE, AIM_CAM_PITCH_RAD, &yaw, &p) == 0);
        CHECK(aim_cam_solve(h, h, AIM_CAM_RIGHT, AIM_CAM_EYE, AIM_CAM_PITCH_RAD, &yaw, &p) == 0);
    }
    printf("test_aim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
