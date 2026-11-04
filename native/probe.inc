/* probe.inc: stage 6b, a generic function probe for reverse engineering on live hardware. `net w probe <hex va> <n> [maxlog] [stack]` detours the function at <va> with a thunk that
 * logs the registers, the stack arguments and (with `stack`) a walk of call sites on the stack, then runs the stolen prologue and continues. <n> is the number of prologue
 * bytes to steal (an instruction boundary >= 5, position independent: lab\probe_len.py computes it from the memory image). Log only, nothing is changed in the call.
 * Included from coop3.c before world.inc (w_command dispatches the console words probe / probes / unprobe).
 */
#define PROBE_MAX 64
typedef struct {
    unsigned target;
    unsigned n;
    unsigned char saved[16];
    unsigned char *thunk;
    unsigned hits;
    int maxlog, stack, active;
    int dump_arg, dump_len;   /* dump_arg: -1 none, 0..5 = stack argument index, 10 = ecx; dump_len bytes at that pointer are logged as hex words */
} probe_t;
static probe_t g_probes[PROBE_MAX];
static int g_np;

static int probe_is_callsite(unsigned v)
{
    unsigned char b[8];
    if (v < 0x401000 || v >= 0x1200000 || !r2_safe_read((void *)(v - 8), b, 8)) return 0;
    /* b[7] is the byte just before v */
    if (b[3] == 0xE8) return 1;                                              /* call rel32 */
    if (b[6] == 0xFF && (b[7] & 0x38) == 0x10 && (b[7] >> 6) == 3) return 1;  /* call reg */
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] >> 6) == 1) return 1;  /* call [reg+d8] */
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10 && ((b[5] >> 6) == 2 || b[5] == 0x15)) return 1; /* call [reg+d32] / call [abs] */
    if (b[6] == 0xFF && (b[7] & 0x38) == 0x10 && (b[7] >> 6) == 0) return 1;  /* call [reg] */
    return 0;
}

static void probe_stack(unsigned sp, char *out, int size)
{
    int o = 0, depth = 0;
    for (int i = 0; i < 96 && depth < 14 && o < size - 16; i++) {
        unsigned v = 0;
        if (!r2_safe_read((void *)(sp + 4 * (unsigned)i), &v, 4)) break;
        if (probe_is_callsite(v)) o += snprintf(out + o, size - o, " %x", v - 0x400000), depth++;
    }
}

static void __attribute__((cdecl, force_align_arg_pointer)) probe_log(unsigned id, unsigned *regs)
{
    if (id >= PROBE_MAX) return;
    probe_t *p = &g_probes[id];
    p->hits++;
    if ((int)p->hits > p->maxlog) return;
    unsigned sp = regs[3] + 4;   /* regs: edi esi ebp esp ebx edx ecx eax as pushed by pushad; esp is the value before pushad, +4 skips the pushfd copy */
    unsigned ret = 0, a[6] = {0, 0, 0, 0, 0, 0};
    r2_safe_read((void *)sp, &ret, 4);
    for (int i = 0; i < 6; i++) r2_safe_read((void *)(sp + 4 + 4 * (unsigned)i), &a[i], 4);
    char st[400] = "";
    if (p->stack) probe_stack(sp, st, sizeof st);
    r2_log(W_LOG "probe %08x #%u: ecx %08x edx %08x args %08x %08x %08x %08x %08x %08x ret exe+%x%s%s", p->target, p->hits, regs[6], regs[5], a[0], a[1], a[2], a[3], a[4], a[5], ret - 0x400000, p->stack ? " stack:" : "", st);
    if (p->dump_len > 0 && p->dump_arg >= 0) {
        unsigned ptr = p->dump_arg == 10 ? regs[6] : a[p->dump_arg % 6];
        char line[1100];
        int o = 0;
        for (int i = 0; i < p->dump_len && i < 512 && o < (int)sizeof line - 16; i += 4) {
            unsigned v = 0;
            if (!r2_safe_read((void *)(ptr + (unsigned)i), &v, 4)) break;
            o += snprintf(line + o, sizeof line - (unsigned)o, "%s%08x", i && (i % 32 == 0) ? " | " : " ", v);
        }
        r2_log(W_LOG "probe %08x #%u dump of %08x (%d bytes):%s", p->target, p->hits, ptr, p->dump_len, line);
    }
}

static int probe_install(unsigned target, unsigned n, int maxlog, int stack, int dump_arg, int dump_len, char *out, int size)
{
    for (int i = 0; i < g_np; i++)
        if (g_probes[i].target == target && g_probes[i].active) {
            snprintf(out, size, "already probed (%08x)", target);
            return 0;
        }
    if (g_np >= PROBE_MAX || n < 5 || n > 14) {
        snprintf(out, size, "probe: bad length or table full");
        return 0;
    }
    unsigned char cur[16];
    if (!r2_safe_read((void *)target, cur, n) || cur[0] == 0xE8 || cur[0] == 0xE9 || cur[0] == 0xEB || (cur[0] >= 0x70 && cur[0] <= 0x7F) || cur[0] == 0x0F) {
        snprintf(out, size, "probe: cannot read %08x or it starts with a branch", target);
        return 0;
    }
    if (cur[0] == 0xE9) {
        snprintf(out, size, "probe: %08x is already detoured", target);
        return 0;
    }
    probe_t *p = &g_probes[g_np];
    memset(p, 0, sizeof *p);
    unsigned char *t = VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    unsigned k = 0;
    t[k++] = 0x9C;                                      /* pushfd */
    t[k++] = 0x60;                                      /* pushad */
    t[k++] = 0x89, t[k++] = 0xE0;                       /* mov eax, esp */
    t[k++] = 0x50;                                      /* push eax */
    unsigned v32;
    int rel;
    t[k++] = 0x68, v32 = (unsigned)g_np, memcpy(t + k, &v32, 4), k += 4;   /* push id */
    t[k++] = 0xE8, rel = (int)((unsigned)probe_log - (unsigned)(t + k + 4)), memcpy(t + k, &rel, 4), k += 4;   /* call probe_log */
    t[k++] = 0x83, t[k++] = 0xC4, t[k++] = 0x08;        /* add esp, 8 */
    t[k++] = 0x61;                                      /* popad */
    t[k++] = 0x9D;                                      /* popfd */
    memcpy(t + k, cur, n), k += n;                      /* the stolen prologue */
    t[k++] = 0xE9, rel = (int)((target + n) - (unsigned)(t + k + 4)), memcpy(t + k, &rel, 4), k += 4;
    p->target = target, p->n = n, p->thunk = t, p->maxlog = maxlog, p->stack = stack, p->active = 1, p->dump_arg = dump_arg, p->dump_len = dump_len;
    memcpy(p->saved, cur, n);
    if (!w_jmp(target, (int)n, t)) {
        p->active = 0;
        snprintf(out, size, "probe: VirtualProtect failed");
        return 0;
    }
    g_np++;
    snprintf(out, size, "probe %d installed at %08x (%u bytes stolen), log %d calls%s", g_np - 1, target, n, maxlog, stack ? " with stack walks" : "");
    return 1;
}

static void probe_remove(unsigned target, char *out, int size)
{
    for (int i = 0; i < g_np; i++) {
        probe_t *p = &g_probes[i];
        if (p->target == target && p->active) {
            DWORD old;
            if (VirtualProtect((void *)target, p->n, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy((void *)target, p->saved, p->n);
                VirtualProtect((void *)target, p->n, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void *)target, p->n);
                p->active = 0;
                snprintf(out, size, "probe %08x removed (%u hits)", target, p->hits);
                return;
            }
        }
    }
    snprintf(out, size, "no active probe at %08x", target);
}

static void probe_list(char *out, int size)
{
    int o = 0;
    for (int i = 0; i < g_np; i++) o += snprintf(out + o, size - o, "probe %d: %08x n=%u hits %u log %d %s%s\n", i, g_probes[i].target, g_probes[i].n, g_probes[i].hits, g_probes[i].maxlog, g_probes[i].stack ? "stack " : "", g_probes[i].active ? "active" : "removed");
    if (!g_np) snprintf(out, size, "no probes");
}
