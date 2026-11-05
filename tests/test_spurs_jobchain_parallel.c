/* SPURS_JC_WORKERS: a job chain runs up to that many jobs at once, capped by its
 * maxContention and its instance's SPU count, and SYNC / LWSYNC still make the
 * jobs after them wait.
 *
 * The real libs/spurs/cellSpurs.c walker and runner threads run here. Only the
 * job binary dispatch (to watch which jobs run, when, and as which SPU) and
 * the completion-event push (to count completions) are observed; nothing in
 * the walker is replaced. Each fixture job sleeps, so jobs that may overlap
 * do; a few sleep longer, so a barrier that does not wait lets a job after it
 * start while one before it is still running. */
#define spu_workload_dispatch_job fixture_dispatch_job
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_job
#include <assert.h>
#include <stdatomic.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x40000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }

/* The SPU number spurs_job.c would give the job (cellSpursGetCurrentSpuId). */
extern SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;

enum { SPURS = 0x0800, PORT = 0x0900, SPURS_2SPU = 0x0A00,
       CHAIN_A = 0x1000, CHAIN_B = 0x1100, CHAIN_C = 0x1200, CHAIN_D = 0x1300,
       LIST_A = 0x3000, LIST_B = 0x3800, LIST_C = 0x3C00, LIST_D = 0x3E00, JOBS = 0x10000, BIN = 0x20000,
       QUEUE = 3, NJOBS = 32, JOB_MS = 30 };
#define JOBEA(k)  (JOBS + (u32)(k) * 0x80u)
#define END       0x7Full                          /* op 7, ext 15 */
#define SYNC      0x2ull                           /* op 2               */
#define LWSYNC    (0x2ull | (2u << 3))             /* op 2, another ext  */

static atomic_uint runs[NJOBS];
static atomic_uint started[NJOBS], ended[NJOBS];   /* order stamps, from 1 */
static atomic_uint stamp;
static atomic_int  now_running, max_running;
static atomic_int  spu_busy[8], spu_clash, spu_used[8];

int fixture_dispatch_job(const uint8_t* image, uint32_t size, uint32_t job_ea, uint32_t desc)
{
    (void)image; (void)size; (void)desc;
    assert(job_ea >= JOBS && (job_ea - JOBS) / 0x80u < NJOBS);
    const unsigned k = (job_ea - JOBS) / 0x80u, spu = g_spurs_job_spu_num;
    if (spu >= 8 || atomic_exchange(&spu_busy[spu], 1)) atomic_store(&spu_clash, 1);
    else atomic_store(&spu_used[spu], 1);
    const int n = atomic_fetch_add(&now_running, 1) + 1;
    for (int m = atomic_load(&max_running); n > m && !atomic_compare_exchange_weak(&max_running, &m, n);) {}
    atomic_store(&started[k], atomic_fetch_add(&stamp, 1) + 1);
    /* The first job after a wait always goes to runner 1, as no runner is busy
     * then: job 0 (before the SYNC) and job 6 (before the LWSYNC). They run
     * long, so the jobs after them end first, and a barrier that does not wait
     * would start the next job while they still run. */
    Sleep(k == 0 ? 10 * JOB_MS : k == 6 ? 5 * JOB_MS : JOB_MS);
    atomic_store(&ended[k], atomic_fetch_add(&stamp, 1) + 1);
    atomic_fetch_sub(&now_running, 1);
    if (spu < 8) atomic_store(&spu_busy[spu], 0);
    atomic_fetch_add(&runs[k], 1);
    return 1;
}

/* Completion events, in the order they were pushed: data1 = chain, data2 = index. */
static atomic_uint pushes;
static _Atomic uint64_t push_d1[256], push_d2[256];
int sys_event_queue_push_by_id(uint32_t q, uint64_t src, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)q; (void)src; (void)d3;
    const unsigned i = atomic_fetch_add(&pushes, 1);
    if (i < 256) { atomic_store(&push_d1[i], d1); atomic_store(&push_d2[i], d2); }
    return 0;
}

static void job(int k)                 /* a plausible 16-byte binary */
{
    vm_write64(JOBEA(k) + 0x00, BIN);
    vm_write32(JOBEA(k) + 0x08, 1u << 16);
}
static int chain_slot(u32 jc)
{
    for (int i = 0; i < MAX_JOBCHAINS; i++) if (s_jobchains[i].jc_ea == jc) return i;
    fprintf(stderr, "FAIL: chain 0x%X was not registered\n", jc); exit(1);
}
/* Wait until the chain's walker has stopped and the events have stopped. */
static void quiet(u32 jc)
{
    int s = chain_slot(jc), still = 0;
    unsigned last = atomic_load(&pushes);
    for (int i = 0; i < 10000 && still < 20; i++) {
        Sleep(1);
        unsigned now = atomic_load(&pushes);
        still = (!__atomic_load_n(&s_jobchains[s].running, __ATOMIC_ACQUIRE) && now == last) ? still + 1 : 0;
        last = now;
    }
    if (still < 20) { fprintf(stderr, "FAIL: the walker never stopped\n"); exit(1); }
}
static void reset_watch(void)
{
    atomic_store(&max_running, 0); atomic_store(&spu_clash, 0);
    for (int i = 0; i < 8; i++) atomic_store(&spu_used[i], 0);
}

static int failed;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); \
                                       fprintf(stderr, "\n"); failed = 1; } } while (0)

/* Every job in [a, b) started after every job in [p, q) ended. */
static void after(int a, int b, int p, int q, const char* what)
{
    unsigned last_end = 0, first_start = ~0u;
    for (int k = p; k < q; k++) if (atomic_load(&ended[k]) > last_end) last_end = atomic_load(&ended[k]);
    for (int k = a; k < b; k++) if (atomic_load(&started[k]) < first_start) first_start = atomic_load(&started[k]);
    CHECK(first_start > last_end, "%s: a job after the barrier started (stamp %u) before the jobs "
          "before it had ended (last end %u)", what, first_start, last_end);
}

int main(void)
{
    setenv("SPURS_JC_WORKERS", "3", 1);
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)(uintptr_t)SPURS, QUEUE, (u8*)(uintptr_t)PORT, 1) == CELL_OK);
    for (int k = 0; k < NJOBS; k++) job(k);

    /* Chain A, maxContention 6, so SPURS_JC_WORKERS=3 is the cap:
     *   jobs 0-5, SYNC, jobs 6-8, LWSYNC, job 9, END */
    {
        static const u64 cmds[] = { JOBEA(0), JOBEA(1), JOBEA(2), JOBEA(3), JOBEA(4), JOBEA(5), SYNC,
                                    JOBEA(6), JOBEA(7), JOBEA(8), LWSYNC, JOBEA(9), END };
        for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) vm_write64(LIST_A + 8u * i, cmds[i]);
        assert(cellSpursCreateJobChain(SPURS, CHAIN_A, LIST_A, 256, 16, 0, 6, 0) == CELL_OK);
        reset_watch();
        const unsigned p0 = atomic_load(&pushes);
        assert(cellSpursRunJobChain(CHAIN_A) == CELL_OK);
        quiet(CHAIN_A);
        for (int k = 0; k <= 9; k++) CHECK(atomic_load(&runs[k]) == 1, "chain A: job %d ran %u time(s)", k, atomic_load(&runs[k]));
        CHECK(atomic_load(&max_running) == 3, "chain A: %d jobs ran at once, want 3 (SPURS_JC_WORKERS)",
              atomic_load(&max_running));
        after(6, 9, 0, 6, "SYNC");
        after(9, 10, 6, 9, "LWSYNC");
        CHECK(!atomic_load(&spu_clash), "chain A: two jobs ran at once as the same SPU");
        CHECK(atomic_load(&spu_used[0]) && atomic_load(&spu_used[1]) && atomic_load(&spu_used[2]),
              "chain A: SPUs 0, 1 and 2 should all have run jobs");
        /* One event per job, in job order although jobs 1-5 end before job 0;
         * then END (repeating the last index) once the last job has finished;
         * then the walker's exit (0). */
        const unsigned n = atomic_load(&pushes) - p0;
        CHECK(n == 12, "chain A: %u completion events, want 10 jobs + END + exit", n);
        CHECK(atomic_load(&ended[1]) < atomic_load(&ended[0]),
              "chain A: job 1 should end before job 0 for the order check to mean anything");
        if (n == 12) {
            for (unsigned i = 0; i < 10; i++)
                CHECK(push_d1[p0 + i] == CHAIN_A && push_d2[p0 + i] == i,
                      "chain A: completion %u carries job %llu, want job %u (in order)", i,
                      (unsigned long long)push_d2[p0 + i], i);
            CHECK(push_d2[p0 + 10] == 9 && push_d2[p0 + 11] == 0,
                  "chain A: END/exit events carry %llu/%llu, want 9/0",
                  (unsigned long long)push_d2[p0 + 10], (unsigned long long)push_d2[p0 + 11]);
        }
    }

    /* Chain B, maxContention 1: never two jobs at once, whatever SPURS_JC_WORKERS
     * says. A title may rely on that order without a SYNC. */
    {
        static const u64 cmds[] = { JOBEA(10), JOBEA(11), JOBEA(12), JOBEA(13), END };
        for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) vm_write64(LIST_B + 8u * i, cmds[i]);
        assert(cellSpursCreateJobChain(SPURS, CHAIN_B, LIST_B, 256, 16, 0, 1, 0) == CELL_OK);
        reset_watch();
        assert(cellSpursRunJobChain(CHAIN_B) == CELL_OK);
        quiet(CHAIN_B);
        for (int k = 10; k <= 13; k++) CHECK(atomic_load(&runs[k]) == 1, "chain B: job %d ran %u time(s)", k, atomic_load(&runs[k]));
        CHECK(atomic_load(&max_running) == 1, "chain B (maxContention 1): %d jobs ran at once",
              atomic_load(&max_running));
        for (int k = 11; k <= 13; k++) after(k, k + 1, k - 1, k, "chain B order");
    }

    /* Chain C, maxContention 2: the chain's own cap, below SPURS_JC_WORKERS. */
    {
        static const u64 cmds[] = { JOBEA(14), JOBEA(15), JOBEA(16), JOBEA(17), JOBEA(18), JOBEA(19), END };
        for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) vm_write64(LIST_C + 8u * i, cmds[i]);
        assert(cellSpursCreateJobChain(SPURS, CHAIN_C, LIST_C, 256, 16, 0, 2, 0) == CELL_OK);
        reset_watch();
        assert(cellSpursRunJobChain(CHAIN_C) == CELL_OK);
        quiet(CHAIN_C);
        for (int k = 14; k <= 19; k++) CHECK(atomic_load(&runs[k]) == 1, "chain C: job %d ran %u time(s)", k, atomic_load(&runs[k]));
        CHECK(atomic_load(&max_running) == 2, "chain C (maxContention 2): %d jobs ran at once, want 2",
              atomic_load(&max_running));
        CHECK(!atomic_load(&spu_clash), "chain C: two jobs ran at once as the same SPU");
    }

    /* Chain D, maxContention 6 on an instance with 2 SPUs: runner k is SPU k, so
     * no more jobs than the instance has SPUs, and no SPU number past them. The
     * instance is entered in the table as an Initialize would leave it; its
     * kernel is not running, which nothing here needs. */
    {
        s_inst[0].nspus = 2;
        __atomic_store_n(&s_inst[0].ea, (u32)SPURS_2SPU, __ATOMIC_RELEASE);
        static const u64 cmds[] = { JOBEA(20), JOBEA(21), JOBEA(22), JOBEA(23), JOBEA(24), JOBEA(25), END };
        for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) vm_write64(LIST_D + 8u * i, cmds[i]);
        assert(cellSpursCreateJobChain(SPURS_2SPU, CHAIN_D, LIST_D, 256, 16, 0, 6, 0) == CELL_OK);
        reset_watch();
        assert(cellSpursRunJobChain(CHAIN_D) == CELL_OK);
        quiet(CHAIN_D);
        for (int k = 20; k <= 25; k++) CHECK(atomic_load(&runs[k]) == 1, "chain D: job %d ran %u time(s)", k, atomic_load(&runs[k]));
        CHECK(atomic_load(&max_running) == 2, "chain D (2 SPUs): %d jobs ran at once, want 2",
              atomic_load(&max_running));
        CHECK(!atomic_load(&spu_clash), "chain D: two jobs ran at once as the same SPU");
        for (int u = 2; u < 8; u++) CHECK(!atomic_load(&spu_used[u]), "chain D (2 SPUs): a job ran as SPU %d", u);
    }

    if (failed) return 1;
    free(vm_base);
    puts("PASS: SPURS_JC_WORKERS=3 runs 3 jobs at once on a chain allowing 6, 2 on one allowing 2, "
         "1 on one allowing 1, 2 on an instance with 2 SPUs; SYNC and LWSYNC wait for the jobs before "
         "them; every job has its own SPU number while it runs, below the instance's SPU count; one "
         "completion per job, in job order, then END and exit");
    return 0;
}
