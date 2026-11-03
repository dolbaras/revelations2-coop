/*
 * Inline hooks into the game's own code for tracing (log only, the original always runs with its original arguments).
 * Only for the supported build: the bytes at each target are checked first and nothing is patched on a mismatch.
 * Installed after the first Present (the Steam stub has decrypted .text by then) when REV2_TRACE=1 or the console says "trace on".
 *
 * Addresses are virtual addresses of build 3658995 (image base 0x400000, no ASLR); names are ours:
 *   0x8882F0  GameMan::SetMode(mode, update)   [this+0x8F4] = mode (0 normal, 1 split screen, 2/3/4 other); update -> camera/renderer refresh
 *   0x8882D0  GameMan::SetSlot(slot, value)    [this+slot*4+0x8F8] = value for slot 0/1 (which pad/player sits in the slot, -1 = none)
 *   0x776300  StoryBlock::SetCoopMode(on)      0 = leave split screen (slot1 = -1, mode 0, [gm+0x8F0] = 0), 1 = join (slot0 = 0, slot1 = 1, mode 1, [gm+0x8F0] = 1)
 * GameMan instance pointer: [0x157AE00]; renderer/camera-system pointer: [0x15DE88C]; story block: [0x157D120].
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "r2.h"

#define THISCALL __attribute__((thiscall))

static void *make_tramp(unsigned target, const unsigned char *expect, int n)
{
    unsigned char cur[16];
    if (n < 5 || n > 12 || !r2_safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        r2_log("trace: bytes at %08x differ from the expected build, not patching", target);
        return NULL;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return NULL;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    return t;
}

static int patch_jmp(unsigned target, int n, void *hook)
{
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    unsigned char *p = (unsigned char *)target;
    for (int i = 5; i < n; i++)
        p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    return 1;
}

static unsigned rva(const void *a) { return (unsigned)a - 0x400000; }

typedef void (THISCALL *SetMode_t)(void *self, int mode, int update);
typedef void (THISCALL *SetSlot_t)(void *self, int slot, int value);
typedef void (THISCALL *SetCoop_t)(void *self, int on);
static SetMode_t orig_SetMode;
static SetSlot_t orig_SetSlot;
static SetCoop_t orig_SetCoop;

static void THISCALL t_SetMode(void *self, int mode, int update)
{
    const void *ret = __builtin_return_address(0);
    int before = *(int *)((char *)self + 0x8F4), flag = *(int *)((char *)self + 0x8F0);
    if (mode != before)   /* (the game calls SetMode(0, 1) every frame from exe+0xAC77E: only changes are logged) */
        r2_log("trace: GameMan::SetMode(mode=%d, update=%d) was mode %d flag8f0 %d, caller exe+%x, thread %lu", mode, update, before, flag, rva(ret), GetCurrentThreadId());
    orig_SetMode(self, mode, update);
}

static void THISCALL t_SetSlot(void *self, int slot, int value)
{
    const void *ret = __builtin_return_address(0);
    int was = slot >= 0 && slot < 2 ? *(int *)((char *)self + 0x8F8 + 4 * slot) : -9;
    if (was != value)
        r2_log("trace: GameMan::SetSlot(slot=%d, value=%d) was %d, caller exe+%x, thread %lu", slot, value, was, rva(ret), GetCurrentThreadId());
    orig_SetSlot(self, slot, value);
}

static void THISCALL t_SetCoop(void *self, int on)
{
    const void *ret = __builtin_return_address(0);
    r2_log("trace: StoryBlock::SetCoopMode(on=%d), caller exe+%x, thread %lu", on, rva(ret), GetCurrentThreadId());
    orig_SetCoop(self, on);
}

void trace_install(void)
{
    static int done;
    if (done || !r2_text_decrypted())
        return;
    done = 1;
    static const unsigned char b_mode[] = {0x80, 0x7C, 0x24, 0x08, 0x00};
    static const unsigned char b_slot[] = {0x8B, 0x54, 0x24, 0x04, 0x83, 0xFA, 0x01};
    static const unsigned char b_coop[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0};
    void *t;
    if ((t = make_tramp(0x8882F0, b_mode, 5))) {
        orig_SetMode = (SetMode_t)t;
        patch_jmp(0x8882F0, 5, (void *)t_SetMode);
    }
    if ((t = make_tramp(0x8882D0, b_slot, 7))) {
        orig_SetSlot = (SetSlot_t)t;
        patch_jmp(0x8882D0, 7, (void *)t_SetSlot);
    }
    if ((t = make_tramp(0x776300, b_coop, 6))) {
        orig_SetCoop = (SetCoop_t)t;
        patch_jmp(0x776300, 6, (void *)t_SetCoop);
    }
    r2_log("trace: inline hooks installed (SetMode %s, SetSlot %s, SetCoopMode %s)", orig_SetMode ? "yes" : "no", orig_SetSlot ? "yes" : "no", orig_SetCoop ? "yes" : "no");
}

/* console "gm": the game-mode manager fields */
void trace_gm_state(char *out, int size)
{
    unsigned gm = 0;
    if (!r2_safe_read((void *)0x157AE00, &gm, 4) || !gm) {
        snprintf(out, size, "no game-mode manager yet");
        return;
    }
    unsigned v[8];
    r2_safe_read((void *)(gm + 0x8F0), v, sizeof v);
    snprintf(out, size, "GameMan %08x: flag8f0=%d mode8f4=%d slot0(8f8)=%d slot1(8fc)=%d  [+900..]= %d %d %d %d", gm, (int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4],
             (int)v[5], (int)v[6], (int)v[7]);
}

/* console "setmode <mode> <update>" and "setslot <slot> <value>" and "setcoop <on>": call the game's own functions (stage 3 first look) */
void trace_call(const char *which, int a, int b, char *out, int size)
{
    unsigned gm = 0, sb = 0;
    r2_safe_read((void *)0x157AE00, &gm, 4);
    r2_safe_read((void *)0x157D120, &sb, 4);
    if (!r2_text_decrypted() || !gm) {
        snprintf(out, size, "game not ready");
        return;
    }
    if (!strcmp(which, "setmode")) {
        ((SetMode_t)0x8882F0)((void *)gm, a, b);
    } else if (!strcmp(which, "setslot")) {
        ((SetSlot_t)0x8882D0)((void *)gm, a, b);
    } else if (!strcmp(which, "setcoop")) {
        if (!sb) {
            snprintf(out, size, "no story block");
            return;
        }
        ((SetCoop_t)0x776300)((void *)sb, a);
    }
    snprintf(out, size, "%s(%d, %d) called", which, a, b);
}
