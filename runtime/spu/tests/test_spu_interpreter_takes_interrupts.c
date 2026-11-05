/* The interpreter takes a pending SPU interrupt before the next instruction.
 *
 * The program below follows the WWS job manager. It issues a DMA list whose
 * first element is the stall-and-notify sentinel (0x80000000: S set, size 0),
 * so the list parks at once and raises Sn. It then opens an interrupts-enabled
 * window one instruction boundary long -- `bie` onto a `bid` -- and relies on
 * its handler, vectored through the `bra` at LS 0, to resume the list in that
 * window. Lifted code takes interrupts at transfers, in the drain loop; code
 * the interpreter runs makes no transfer there, so before the interpreter
 * checked for them it never took this one: the handler never ran, the list
 * stayed parked and the transfer never arrived.
 *
 * Everything runs through the real spu_interp.c, spu_drain.c (the take),
 * spu_channels.c and spu_dma.h (events, the list, its stall and its resume).
 * One mode per process, named by argv[1]:
 *   interp        spu_interp_run_until from the program's start, handler
 *                 interpreted too
 *   driver        spu_run_with_halt: the SMC microstep hands the program to the
 *                 interpreter, as it does for a policy module's unlifted code;
 *                 handler interpreted
 *   driver-lifted the same, with a lift registered for the handler: the take
 *                 hands it to the lifted path, whose dispatch at srr0 restores
 *                 the registers
 *   disabled      `bi` for the `bie`: nothing is taken, the list stays parked
 *   novector      no branch at LS 0: not taken, and the program still ends
 */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
extern uint8_t g_vm_page_bitmap[65536 / 8];
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
u128 spu_rdch(spu_context* ctx, uint32_t channel);              /* spu_channels.c */
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);

enum {
    HANDLER = 0x00600, PROG = 0x04000, LIST = 0x01570, DEST = 0x02000,
    EA = 0x10000,                       /* the list's guest source */
    RUNS = 0x01000,                     /* handler runs (the handler counts) */
    STALLS = 0x01010,                   /* the stalled tags a handler run found */
    SEEN10 = 0x01100,                   /* $10 as the interrupted code saw it */
    TAGSTAT = 0x01110,                  /* MFC_RdTagStat for tag 0 at the end */
    WINDOW = PROG + 0x44,               /* the `bid`: the take's resume point */
};

/* Encoders for the forms used (SPU ISA 1.2). */
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) { return op9 << 23 | (i16 & 0xFFFF) << 7 | rt; }
static uint32_t ri18(uint32_t op7, uint32_t i18, uint32_t rt) { return op7 << 25 | (i18 & 0x3FFFF) << 7 | rt; }
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) { return op8 << 24 | (i10 & 0x3FF) << 14 | ra << 7 | rt; }
static uint32_t rr(uint32_t op11, uint32_t ra, uint32_t rt) { return op11 << 21 | ra << 7 | rt; }
#define IL(rt, i)       ri16(0x081, (i), (rt))
#define ILA(rt, a)      ri18(0x21, (a), (rt))
#define AI(rt, ra, i)   ri10(0x1C, (i), (ra), (rt))
#define LQA(rt, a)      ri16(0x061, (a) >> 2, (rt))
#define STQA(rt, a)     ri16(0x041, (a) >> 2, (rt))
#define BRZ(rt, words)  ri16(0x040, (words), (rt))   /* relative, in words */
#define BRA(a)          ri16(0x060, (a) >> 2, 0)
#define RDCH(rt, ch)    rr(0x00D, (ch), (rt))
#define WRCH(ch, rt)    rr(0x10D, (ch), (rt))
#define BI(ra)          rr(0x1A8, (ra), 0)
#define BIE(ra)         (BI(ra) | 0x40000u)
#define BID(ra)         (BI(ra) | 0x80000u)
#define IRETE           (rr(0x1AA, 0, 0) | 0x40000u)
#define STOP(code)      (code)

static spu_context ctx;
static const uint8_t PATTERN[16] = { 0xC0, 0xFF, 0xEE, 0x01, 0x23, 0x45, 0x67, 0x89,
                                     0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76, 0x98 };

static void put32(uint32_t lsa, uint32_t v)
{
    ctx.ls[lsa] = (uint8_t)(v >> 24); ctx.ls[lsa + 1] = (uint8_t)(v >> 16);
    ctx.ls[lsa + 2] = (uint8_t)(v >> 8); ctx.ls[lsa + 3] = (uint8_t)v;
}
static uint32_t get32(uint32_t lsa)
{
    return (uint32_t)ctx.ls[lsa] << 24 | (uint32_t)ctx.ls[lsa + 1] << 16 |
           (uint32_t)ctx.ls[lsa + 2] << 8 | ctx.ls[lsa + 3];
}
static void code(uint32_t at, const uint32_t* w, unsigned n)
{
    for (unsigned i = 0; i < n; i++) put32(at + 4 * i, w[i]);
}

/* The handler, run as a lift. Unlike the interpreted one it reads ch25 before
 * it acknowledges Sn, so the ack clears the event and it runs once. (With the
 * job manager's order the ack leaves Sn pending -- see the interpreted handler
 * -- and the lifted path takes that second interrupt before its dispatch at
 * srr0 has restored the registers; real handlers restore their own. Not what
 * this test is about.) */
static void lifted_handler(spu_context* c)
{
    c->gpr[10] = spu_splat_u32(0x77);            /* clobber, as the interpreted one does */
    const uint32_t stalled = spu_rdch(c, MFC_RdListStallStat)._u32[0];
    spu_wrch(c, SPU_WrEventAck, spu_rdch(c, SPU_RdEventStat));
    if (stalled) {
        put32(STALLS, stalled);
        spu_wrch(c, MFC_WrListStallAck, spu_splat_u32(0));
    }
    put32(RUNS, get32(RUNS) + 1);
    /* irete, as the lifter emits it */
    c->int_enable = 1;
    c->pc = c->srr0;
    g_spu_trampoline_fn = spu_indirect_branch;
}

static void entry(spu_context* c)
{
    c->pc = PROG;
    g_spu_trampoline_fn = spu_indirect_branch;
}

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s line %d: %s\n", mode, __LINE__, #c); } } while (0)

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s interp|driver|driver-lifted|disabled|novector\n", argv[0]); return 2; }
    const char* mode = argv[1];
    const int lifted = !strcmp(mode, "driver-lifted");
    const int driver = lifted || !strcmp(mode, "driver");
    const int disabled = !strcmp(mode, "disabled");
    const int novector = !strcmp(mode, "novector");
    if (!driver && !disabled && !novector && strcmp(mode, "interp")) { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    const int taken = !disabled && !novector;

    ppu_vm_size = 0x100000;
    vm_base = (uint8_t*)calloc(1, ppu_vm_size);
    if (!vm_base) return 2;
    memset(g_vm_page_bitmap, 0xFF, sizeof g_vm_page_bitmap);   /* guest memory is committed */
    memcpy(vm_base + EA, PATTERN, sizeof PATTERN);

    spu_context_init(&ctx, 0);
    ctx.image_id = lifted ? 77 : 78;     /* both have lifts; only 77 has one at the handler */
    ctx.policy_mode = 1;                 /* a policy module, like the job manager */
    spu_begin_image(77);
    spu_register_function(HANDLER, lifted_handler);
    spu_begin_image(78);
    spu_register_function(0x30000, entry);   /* unrelated: image 78 has lifted code */

    /* The interrupt vector the job manager plants at LS 0 on entry. */
    if (!novector) put32(0, BRA(HANDLER));
    /* The list: the stall-and-notify sentinel, then 16 bytes from EA. */
    put32(LIST, 0x80000000u); put32(LIST + 4, EA);
    put32(LIST + 8, sizeof PATTERN); put32(LIST + 12, EA);

    const uint32_t prog[] = {
        IL(3, 2),                   /* 4000  Sn */
        WRCH(SPU_WrEventMask, 3),
        IL(4, DEST),
        WRCH(MFC_LSA, 4),
        IL(5, 0),                   /* 4010 */
        WRCH(MFC_EAH, 5),
        IL(6, LIST),
        WRCH(MFC_EAL, 6),
        IL(7, 16),                  /* 4020  two elements */
        WRCH(MFC_Size, 7),
        WRCH(MFC_TagID, 5),         /*       tag 0 */
        IL(8, 0x45),                /*       GETLB */
        WRCH(MFC_Cmd, 8),           /* 4030  parks on the sentinel: Sn */
        IL(10, 0x55),               /*       live across the window */
        ILA(11, WINDOW),
        ILA(12, WINDOW + 4),
        disabled ? BI(11) : BIE(11),/* 4040  interrupts on, onto the bid */
        BID(12),                    /* 4044  interrupts off: taken before this */
        STQA(10, SEEN10),           /* 4048 */
        IL(13, 1),
        WRCH(MFC_WrTagMask, 13),    /* 4050 */
        IL(14, 2),                  /*       all */
        WRCH(MFC_WrTagUpdate, 14),
        RDCH(15, MFC_RdTagStat),
        STQA(15, TAGSTAT),          /* 4060 */
        STOP(0x2001),
    };
    code(PROG, prog, sizeof prog / sizeof prog[0]);

    /* The handler in the job manager's order: ack Sn, then read ch25. The ack
     * comes while ch25 is unread, so Sn stays pending (spu_channels.c,
     * WrEventAck) and it runs a second time, finds nothing stalled and
     * returns. */
    const uint32_t handler[] = {
        IL(10, 0x77),               /* 0600  clobber a live register */
        RDCH(20, SPU_RdEventStat),
        WRCH(SPU_WrEventAck, 20),
        RDCH(21, MFC_RdListStallStat),
        BRZ(21, 4),                 /* 0610  nothing stalled: skip the resume */
        STQA(21, STALLS),
        IL(22, 0),
        WRCH(MFC_WrListStallAck, 22),   /* resume tag 0 */
        LQA(23, RUNS),              /* 0620 */
        AI(23, 23, 1),
        STQA(23, RUNS),
        IRETE,
    };
    code(HANDLER, handler, sizeof handler / sizeof handler[0]);

    uint32_t rc = 0;
    if (driver) {
        spu_run_with_halt(entry, &ctx);
        rc = ctx.stop_code;
    } else {
        rc = spu_interp_run_until(&ctx, PROG, 0);
    }

    CHECK(rc == 0x2001);                                     /* ran to its stop */
    CHECK(get32(SEEN10) == 0x55);                            /* registers preserved */
    CHECK(ctx.int_enable == 0);                              /* the bid ran */
    CHECK(!ctx.irq_saved);                                   /* no snapshot left armed */
    if (taken) {
        CHECK(get32(RUNS) == (lifted ? 1u : 2u));
        CHECK(get32(STALLS) == 1);                           /* tag 0 */
        CHECK(ctx.srr0 == WINDOW);                           /* taken in the window */
        CHECK(!memcmp(ctx.ls + DEST, PATTERN, sizeof PATTERN));   /* the resumed element landed */
        CHECK(ctx.list_stall_mask == 0);                     /* nothing parked */
        CHECK(get32(TAGSTAT) == 1);                          /* tag 0 complete */
        CHECK((ctx.event_status & 2u) == 0);                 /* Sn acknowledged */
    } else {
        CHECK(get32(RUNS) == 0);
        CHECK(ctx.list_stall_mask == 1);                     /* still parked */
        CHECK(get32(TAGSTAT) == 0);
        CHECK(ctx.ls[DEST] == 0);
    }
    if (fails) return 1;
    printf("PASS %s\n", mode);
    return 0;
}

/* What the linked runtime files reach outside the SPU runtime. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
