/* Real runtime transition regression. Hand-encoded guest bytes force the full
 * interpreter (ori is outside the SMC microstep subset), then call lifted code.
 * The lifted functions below use the same call/drain/return ABI as generated C.
 * No game data, DMA, service, GPU, or runtime dispatcher mocks are involved. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);
static spu_context ctx;
static void word(uint32_t pc, uint32_t w) {
    ctx.ls[pc] = w >> 24; ctx.ls[pc+1] = w >> 16;
    ctx.ls[pc+2] = w >> 8; ctx.ls[pc+3] = w;
}
static void finish(spu_context* c) {
    c->gpr[11]._u32[0]++;
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}
static void helper(spu_context* c) {
    c->gpr[10] = spu_ai(c->gpr[10], 1);
    SPU_RET(c);
}
/* This is a valid registered entry which a nested call's return boundary must
 * NOT execute: the corresponding host frame owns its continuation. */
static void wrong_continuation(spu_context* c) {
    c->gpr[21]._u32[0]++;
    finish(c);
}
static void tail_entry(spu_context* c) {
    c->pc = 0x4000; g_spu_trampoline_fn = spu_indirect_branch;
}
static int queued_call;
static void nested_entry(spu_context* c) {
    c->gpr[0] = spu_link(0x204);
    c->host_depth++;
    c->pc = 0x4000;
    if (queued_call) g_spu_trampoline_fn = spu_indirect_branch;
    else spu_indirect_branch(c);
    spu_drain_call(c, 0x204);
    c->host_depth--;
    c->gpr[20]._u32[0]++;
    finish(c);
}
/* `bisl $0,$5` at 0x500 into code nothing was lifted for, above the jm2 kernel
 * base (SPU_INTERP_UNLIFTED's path), as the lifter emits it: link, dispatch,
 * drain, then this frame's own continuation. */
static void unlifted_call_entry(spu_context* c) {
    c->gpr[5]._u32[0] = 0x16000;
    c->gpr[0] = spu_link(0x504);
    c->pc = 0x16000;
    c->host_depth++;
    spu_indirect_branch(c);
    spu_drain_call(c, 0x504);
    c->host_depth--;
    c->gpr[20]._u32[0]++;
    finish(c);
}
static void unlifted_tail_entry(spu_context* c) {
    c->pc = 0x16000; g_spu_trampoline_fn = spu_indirect_branch;
}
/* The same transfer at top level, with $0 holding the return point of a call
 * to the code: no host frame waits there, so the guest's own continuation
 * must run rather than the run ending at it. */
static void unlifted_top_link_entry(spu_context* c) {
    c->gpr[5]._u32[0] = 0x16000;
    c->gpr[0] = spu_link(0x504);
    unlifted_tail_entry(c);
}
/* A computed jump into unlifted code, run as a hop of the drain for the call
 * that returns to 0x204. $0 still holds the return point of an earlier call
 * (0x600 -> 0x7000), which is not where this code returns. */
static void hop_stale_link_entry(spu_context* c) {
    c->gpr[0] = spu_link(0x604);
    c->host_depth++;
    c->pc = 0x16000;
    g_spu_trampoline_fn = spu_indirect_branch;
    spu_drain_call(c, 0x204);
    c->host_depth--;
    c->gpr[20]._u32[0]++;
    finish(c);
}
/* Two drains deep: the outer call's drain runs a hop into a function that
 * makes its own call, and that callee returns to an unlifted mid-function pc.
 * The inner drain puts the outer drain's return pc back and restarts dispatch
 * from the driver, which discards both host frames. */
static void stray_return(spu_context* c) {
    c->gpr[0] = spu_link(0x4100);
    SPU_RET(c);
}
static void inner_call(spu_context* c) {
    c->gpr[0] = spu_link(0x308);
    c->host_depth++;
    c->pc = 0x320;
    spu_indirect_branch(c);
    spu_drain_call(c, 0x308);
    c->host_depth--;
    c->gpr[20]._u32[0] += 10;   /* in a frame the restart discarded: never runs */
    finish(c);
}
static void restart_entry(spu_context* c) {
    c->gpr[0] = spu_link(0x204);
    c->host_depth++;
    c->pc = 0x300;
    g_spu_trampoline_fn = spu_indirect_branch;   /* the call runs as a hop of this drain */
    spu_drain_call(c, 0x204);
    c->host_depth--;
    c->gpr[20]._u32[0]++;
    finish(c);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* mode = argv[1];
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.policy_mode = 1; /* live policy APIs are below the unlifted job */
    ctx.status = SPU_STATUS_RUNNING;
    spu_begin_image(77);
    spu_register_function(0x200, helper);
    if (strcmp(mode, "nested-return-unlifted") && strcmp(mode, "restart-stale-return"))
        spu_register_function(0x204, wrong_continuation);
    spu_register_function(0x240, finish);
    if (!strcmp(mode, "restart-stale-return")) {
        spu_register_function(0x300, inner_call);
        spu_register_function(0x320, stray_return);
    }
    if (!strcmp(mode, "top-zero")) spu_register_function(0, finish);
    if (!strcmp(mode, "negative-stepcap")) {
        ctx.image_id = -1;
        spu_begin_image(0);
        spu_register_function(0x4004, finish); /* wildcard ignored by pure interpretation */
    }
    word(0x4000, 0x04000082); /* ori $2,$1,0: force full interpreter */
    void (*entry)(spu_context*) = tail_entry;
    if (!strcmp(mode, "top-zero")) {
        word(0x4004, 0x30000000); /* bra 0: registered, valid lifted entry */
    } else if (!strcmp(mode, "top")) {
        word(0x4004, 0x30004800); /* bra 0x240 */
    } else if (!strncmp(mode, "nested-return", 13)) {
        entry = nested_entry; queued_call = 1;
        word(0x204, 0x1C004A95); /* ai $21,$21,1: must NOT be interpreted */
        word(0x208, 0x30004800); /* bra 0x240 */
        word(0x4004, 0x35000000); /* bi $0 -> 0x204 */
    } else if (!strcmp(mode, "nested-calls")) {
        entry = nested_entry;
        word(0x4004, 0x31004000); /* brasl $0,0x200 */
        word(0x4008, 0x04000082); /* ori $2,$1,0 */
        word(0x400C, 0x31004000); /* brasl $0,0x200 */
        word(0x4010, 0x04000082); /* ori $2,$1,0 */
        word(0x4014, 0x30004800); /* bra 0x240 */
    } else if (!strcmp(mode, "stop-zero") || !strcmp(mode, "nested-stop")) {
        if (!strcmp(mode, "nested-stop")) entry = nested_entry;
        word(0x4004, 0x00000000); /* stop 0 */
    } else if (!strcmp(mode, "stop-code")) {
        word(0x4004, 0x00002123); /* stop 0x2123 */
    } else if (!strcmp(mode, "halt")) {
        word(0x4004, 0x7F000102); /* heqi $2,0: ori copied initial zero */
    } else if (!strcmp(mode, "smc")) {
        word(0x4000, 0x40812003); /* il $3,0x240 (microstep supported) */
        word(0x4004, 0x35000180); /* bi $3 */
    } else if (!strncmp(mode, "smc-ldst", 8)) {
        /* A generated save/restore stub's shape: quadword stores and loads,
         * then a branch. "smc-ldst" runs it on the microstep's load/store fast
         * loop (LS diagnostics read as off); "smc-ldst-diag" leaves the gates
         * unread, so the generic path runs it. Same result either way. */
        if (!strcmp(mode, "smc-ldst")) g_spu_ls_watch_n = g_spu_ls_probe = g_spu_smc_watch = 0;
        for (int k = 0; k < 4; k++) {
            ctx.gpr[5]._u32[k] = 0x11223344u * (uint32_t)(k + 1);
            ctx.gpr[6]._u32[k] = 0xA0B0C0D0u ^ (uint32_t)k;
        }
        ctx.gpr[1]._u32[0] = 0x3000;
        word(0x4000, 0x40812003); /* il   $3,0x240 */
        word(0x4004, 0x20FE0005); /* stqa $5,0x3F000 */
        word(0x4008, 0x24008086); /* stqd $6,0x20($1) */
        word(0x400C, 0x30FE0007); /* lqa  $7,0x3F000 */
        word(0x4010, 0x34008088); /* lqd  $8,0x20($1) */
        word(0x4014, 0x35000180); /* bi   $3 */
    } else if (!strcmp(mode, "stepcap") || !strcmp(mode, "negative-stepcap")) {
        word(0x4004, 0x04000082);
        word(0x4008, 0x30004800);
    } else if (!strcmp(mode, "unlifted-call-return")) {
        /* Run with SPU_INTERP_UNLIFTED=1. The interpreter must hand back at
         * the call's return point, so the waiting host frame continues. */
        entry = unlifted_call_entry; ctx.policy_mode = 0;
        word(0x500, 0x35200280);   /* bisl $0,$5: the call site */
        word(0x504, 0x1C004A95);   /* ai $21,$21,1: the caller's code, must NOT be interpreted */
        word(0x508, 0x30004800);   /* bra 0x240 */
        word(0x16000, 0x04000082); /* ori $2,$1,0 */
        word(0x16004, 0x35000000); /* bi $0 -> 0x504 */
    } else if (!strcmp(mode, "unlifted-top-rejoin")) {
        /* SPU_INTERP_UNLIFTED=1, no enclosing call: the rejoin at a lifted
         * entry is a transfer, and the top-level drain must run it. */
        entry = unlifted_tail_entry; ctx.policy_mode = 0;
        word(0x16000, 0x04000082); /* ori $2,$1,0 */
        word(0x16004, 0x30004800); /* bra 0x240 */
    } else if (!strcmp(mode, "unlifted-top-link")) {
        entry = unlifted_top_link_entry; ctx.policy_mode = 0;
        word(0x500, 0x35200280);   /* bisl $0,$5 */
        word(0x504, 0x1C004A95);   /* ai $21,$21,1: the guest's own continuation */
        word(0x508, 0x30004800);   /* bra 0x240 */
        word(0x16000, 0x04000082); /* ori $2,$1,0 */
        word(0x16004, 0x35000000); /* bi $0 -> 0x504 */
    } else if (!strcmp(mode, "unlifted-hop-stale-link")) {
        entry = hop_stale_link_entry; ctx.policy_mode = 0;
        word(0x600, 0x330D4000);   /* brsl $0,0x7000: an earlier call */
        word(0x604, 0x04000082);   /* ori $2,$1,0 */
        word(0x608, 0x30004080);   /* bra 0x204: the real return point */
        word(0x16000, 0x04000082); /* ori $2,$1,0 */
        word(0x16004, 0x3000C080); /* bra 0x604 */
    } else if (!strcmp(mode, "restart-stale-return")) {
        /* A return lands on unlifted 0x4100 two drains deep and the inner
         * drain restarts dispatch. The outer drain's return pc (0x204) has
         * no frame after that, so the interpreter must not stop there. */
        entry = restart_entry;
        word(0x4100, 0x04000082);  /* ori $2,$1,0: past the microstep set */
        word(0x4104, 0x30004080);  /* bra 0x204 */
        word(0x204, 0x1C004A95);   /* ai $21,$21,1: runs, from the driver */
        word(0x208, 0x30004800);   /* bra 0x240 */
    } else if (!strcmp(mode, "pure-interpreter")) {
        word(0x4004, 0x30004800);
        uint32_t pc = spu_interp_run_until(&ctx, 0x4000, 0);
        assert(pc == 0x240 && ctx.status == SPU_STATUS_RUNNING);
        assert(g_spu_trampoline_fn == NULL); /* adapter owns scheduling */
        puts("pure-interpreter PASS"); return 0;
    } else { fprintf(stderr, "unknown mode\n"); return 2; }
    int halted = spu_run_with_halt(entry, &ctx);
    printf("%s: halted=%d status=%X pc=%X helper=%u finish=%u host-cont=%u wrong-cont=%u\n",
           mode, halted, ctx.status, ctx.pc, ctx.gpr[10]._u32[0],
           ctx.gpr[11]._u32[0], ctx.gpr[20]._u32[0], ctx.gpr[21]._u32[0]);
    fflush(stdout);
    if (!strcmp(mode, "stepcap") || !strcmp(mode, "negative-stepcap")) {
        assert(halted == 0 && ctx.status == SPU_STATUS_RUNNING);
        assert(ctx.pc == 0x4004 && ctx.gpr[11]._u32[0] == 0);
        assert(g_spu_trampoline_fn == NULL);
        puts("PASS"); return 0;
    }
    if (!strcmp(mode, "unlifted-top-link")) {
        assert(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
        assert(ctx.gpr[11]._u32[0] == 1 && ctx.gpr[21]._u32[0] == 1);
        assert(ctx.gpr[20]._u32[0] == 0);
        puts("PASS"); return 0;
    }
    if (!strcmp(mode, "restart-stale-return")) {
        /* The restart took the host frames with it, so the caller's
         * continuation at 0x204 runs interpreted from the driver, then the
         * lifted finish. The run must not end at the dead return point. */
        assert(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
        assert(ctx.gpr[11]._u32[0] == 1 && ctx.gpr[21]._u32[0] == 1);
        assert(ctx.gpr[20]._u32[0] == 0);
        puts("PASS"); return 0;
    }
    assert(ctx.gpr[21]._u32[0] == 0);
    if (!strncmp(mode, "smc-ldst", 8)) {
        for (int k = 0; k < 4; k++) {
            const uint32_t v5 = ctx.gpr[5]._u32[k], v6 = ctx.gpr[6]._u32[k];
            const uint8_t* a = &ctx.ls[0x3F000 + 4 * k];
            const uint8_t* b = &ctx.ls[0x3020 + 4 * k];
            assert(a[0] == (uint8_t)(v5 >> 24) && a[1] == (uint8_t)(v5 >> 16) &&
                   a[2] == (uint8_t)(v5 >> 8) && a[3] == (uint8_t)v5);   /* big-endian in LS */
            assert(b[0] == (uint8_t)(v6 >> 24) && b[3] == (uint8_t)v6);
            assert(ctx.gpr[7]._u32[k] == v5 && ctx.gpr[8]._u32[k] == v6);
        }
    }
    if (!strncmp(mode, "stop", 4) || !strcmp(mode, "nested-stop") || !strcmp(mode, "halt")) {
        /* Baseline-equivalence controls, not assertions of architectural stop
         * correctness. The adapter's inherited numeric-stop-code behavior is
         * deliberately outside this narrowly scoped rejoin fix. */
        assert(ctx.gpr[11]._u32[0] == 0 && ctx.gpr[20]._u32[0] == 0);
        assert(halted == (!strcmp(mode, "nested-stop") ? 1 : 0));
        assert(ctx.status == (!strcmp(mode, "stop-code") ?
               SPU_STATUS_STOPPED_BY_STOP : SPU_STATUS_STOPPED_BY_HALT));
        assert(ctx.stop_code == (!strcmp(mode, "halt") ? 0x1000u :
               !strcmp(mode, "stop-code") ? 0x2123u : 0u));
        puts("PASS (legacy stop/halt baseline unchanged)"); return 0;
    }
    assert(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    assert(ctx.gpr[11]._u32[0] == 1);
    assert(ctx.gpr[20]._u32[0] == (!strncmp(mode, "nested-return", 13) ||
                                    !strcmp(mode, "unlifted-call-return") ||
                                    !strcmp(mode, "unlifted-hop-stale-link") ? 1u : 0u));
    assert(ctx.gpr[10]._u32[0] == (!strcmp(mode, "nested-calls") ? 2u : 0u));
    puts("PASS"); return 0;
}

/* Link-only boundaries outside this CPU-only fixture. Fail if any is reached. */
/* spu_workload.c can retain the LV2 runner's publication calls at link time;
 * this fixture runs its own context directly, without an LV2 SPU thread. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
