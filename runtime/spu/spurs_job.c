/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include "spu_coherency.h"
#include "spu_workload.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/log_first.h"
#ifdef _WIN32
#  include <windows.h>     /* QueryPerformanceCounter, for GCM_HOLE_TRACE */
#else
#  include <time.h>
#endif

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * The job context is a stack local, so by the time the PPU looks there is
 * nothing to read and every poll fails. Retain the store and let the syscall
 * resolve a chain handle to it. */
static uint8_t  s_job_ls[SPU_LS_SIZE];
static int      s_job_ls_valid;
uint32_t        g_spurs_job_ls_handle;   /* set by the chain walker */

const uint8_t* spurs_job_ls_for_handle(uint32_t handle)
{
    if (!s_job_ls_valid || !handle || handle != g_spurs_job_ls_handle) return 0;
    return s_job_ls;
}

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }

/* ---------------------------------------------------------------------------
 * GCM_HOLE_TRACE=1: who fills each command-buffer hole.
 *
 * A title whose SPU jobs write draws into the RSX command stream leaves each
 * job a hole: the PPU parks the RSX on a self-JUMP at the hole's start, goes
 * on writing commands after it, and the job PUTs its commands over the hole.
 * PlayStation All-Stars Battle Royale leaves one hole a frame standing from
 * some point on -- with a chain's jobs on several host threads and on one --
 * and the RSX waits on it until the FIFO walker gives up on the park
 * (cellGcmSys.c, "jump-park"). Nothing said which job owned the hole, whether
 * it ran, or where its PUTs went.
 *
 * Armed, this keeps the last HT_PUTS SPU PUTs (destination, size, the word
 * each replaced and the first two it wrote, the job, SPU number, host thread,
 * image and pc that issued it) and the last HT_JOBS jobs (descriptor EA, the
 * first 256 bytes of the descriptor and of the input buffer, the host thread
 * and SPU number, when it started and ended, how many GETs and PUTs, its last
 * GET source). When the walker gives up on a park it calls
 * g_spu_hole_report_hook, which prints "[hole]" lines:
 *   - the words around the park as they stand;
 *   - every kept PUT that covered the park, oldest first;
 *   - the jobs whose descriptor or input names the park (its EA or its IO
 *     offset), with every PUT each of them issued;
 *   - the jobs still running;
 *   - the PUTs nearest the park on either side.
 * GCM_HOLE_TRACE=<n> prints at most n reports (1 means the default, 6).
 *
 * Off (the default) it costs a NULL test per DMA (spu_dma.h) and one load
 * per job. The hooks are defined here so the DMA engine and the GCM walker
 * need no other link-time partner.
 * -----------------------------------------------------------------------*/
void (*g_spu_dma_note_hook)(spu_context* spu, uint32_t ea, uint32_t size,
                            const uint8_t* src, uint32_t cmd) = 0;
void (*g_spu_hole_report_hook)(uint32_t io, uint32_t ea, uint32_t word,
                               uint32_t put, unsigned long long age_ms) = 0;

#define HT_PUTS (1u << 18)
#define HT_JOBS (1u << 13)
#define HT_SNAP 64u              /* words kept from a descriptor / an input */

typedef struct {
    uint64_t t;                  /* host ns */
    uint32_t seq;                /* claim number + 1; 0 while being written */
    uint32_t ea, size;
    uint32_t old0;               /* the word at ea before the PUT */
    uint32_t new0, new1;         /* the first two words it wrote */
    uint32_t job;                /* the issuing job's seq, 0 = not a job */
    uint32_t pc;
    uint16_t img; uint8_t spu, thr;
} HtPut;

typedef struct {
    uint64_t t0, t1;             /* host ns; t1 = 0 while running */
    uint32_t seq;
    uint32_t ea;
    uint32_t n_get, n_put, last_get;
    uint16_t img; uint8_t spu, thr;
    uint32_t nd, ni;             /* words kept in d[] and in[] */
    uint32_t d[HT_SNAP], in[HT_SNAP];
} HtJob;

static HtPut*   s_ht_put;
static HtJob*   s_ht_job;
static uint32_t s_ht_put_n, s_ht_job_n, s_ht_thr_n;
static int      s_ht_state;      /* 0 unknown, 1 off, 2 starting, 3 on */
static int      s_ht_reports = 6;
static SPURS_JOB_TLS uint32_t t_ht_job;   /* seq of the job this thread runs */
static SPURS_JOB_TLS uint32_t t_ht_thr;   /* small host thread number */

static uint64_t ht_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint32_t ht_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t ht_thr(void)
{
    if (!t_ht_thr) t_ht_thr = __atomic_add_fetch(&s_ht_thr_n, 1, __ATOMIC_RELAXED);
    return (uint8_t)t_ht_thr;
}

/* The record of the job this thread is running, or NULL. */
static HtJob* ht_cur_job(void)
{
    const uint32_t s = t_ht_job;
    if (!s) return 0;
    HtJob* j = &s_ht_job[(s - 1u) & (HT_JOBS - 1u)];
    return __atomic_load_n(&j->seq, __ATOMIC_RELAXED) == s ? j : 0;
}

static void ht_note_dma(spu_context* spu, uint32_t ea, uint32_t size,
                        const uint8_t* src, uint32_t cmd)
{
    HtJob* j = ht_cur_job();
    if (cmd & 0x40u) {                         /* GET family */
        if (j) { j->n_get++; j->last_get = ea; }
        return;
    }
    if (!(cmd & 0x20u)) return;                /* not a PUT */
    if (j) j->n_put++;
    const uint32_t n = __atomic_fetch_add(&s_ht_put_n, 1u, __ATOMIC_RELAXED);
    HtPut* p = &s_ht_put[n & (HT_PUTS - 1u)];
    __atomic_store_n(&p->seq, 0u, __ATOMIC_RELAXED);
    p->t    = ht_now();
    p->ea   = ea;
    p->size = size;
    p->old0 = size >= 4 ? ht_be32(vm_base + ea) : 0;   /* called before the copy */
    p->new0 = size >= 4 ? ht_be32(src) : 0;
    p->new1 = size >= 8 ? ht_be32(src + 4) : 0;
    p->job  = j ? t_ht_job : 0;
    p->pc   = spu->pc & SPU_LS_MASK;
    p->img  = (uint16_t)spu->image_id;
    p->spu  = (uint8_t)g_spurs_job_spu_num;
    p->thr  = ht_thr();
    __atomic_store_n(&p->seq, n + 1u, __ATOMIC_RELEASE);
}

static double ht_ago_ms(uint64_t now, uint64_t t)
{
    return t && t <= now ? (double)(now - t) / 1e6 : -1.0;
}

static void ht_print_put(const char* tag, const HtPut* p, uint32_t park, uint64_t now)
{
    const HtJob* j = p->job ? &s_ht_job[(p->job - 1u) & (HT_JOBS - 1u)] : 0;
    const uint32_t jea = (j && j->seq == p->job) ? j->ea : 0;
    fprintf(stderr, "[hole]   %s ea=0x%08X (park%+d) size=%u old=%08X new=%08X %08X"
                    " job=#%u(0x%08X) img=%u spu=%u thr=%u pc=0x%05X t-%.2fms\n",
            tag, p->ea, (int)(p->ea - park), p->size, p->old0, p->new0, p->new1,
            p->job, jea, p->img, p->spu, p->thr, p->pc, ht_ago_ms(now, p->t));
}

static void ht_print_job(const char* tag, const HtJob* j, uint64_t now, const char* where)
{
    fprintf(stderr, "[hole]   %s job #%u ea=0x%08X img=%u spu=%u thr=%u started t-%.2fms %s%.2fms%s%s,"
                    " %u GETs (last 0x%08X), %u PUTs\n",
            tag, j->seq, j->ea, j->img, j->spu, j->thr, ht_ago_ms(now, j->t0),
            j->t1 ? "ended t-" : "still running after ",
            j->t1 ? ht_ago_ms(now, j->t1) : ht_ago_ms(now, j->t0),
            where ? ", names the park at " : "", where ? where : "",
            j->n_get, j->last_get, j->n_put);
}

static void ht_report(uint32_t io, uint32_t ea, uint32_t word, uint32_t put,
                      unsigned long long age_ms)
{
    static int n_rep;
    if (n_rep >= s_ht_reports) return;
    const int rep = ++n_rep;
    const uint64_t now = ht_now();
    const uint32_t pn = __atomic_load_n(&s_ht_put_n, __ATOMIC_ACQUIRE);
    const uint32_t jn = __atomic_load_n(&s_ht_job_n, __ATOMIC_ACQUIRE);
    const uint32_t p0 = pn > HT_PUTS ? pn - HT_PUTS : 0;
    const uint32_t j0 = jn > HT_JOBS ? jn - HT_JOBS : 0;
    const HtPut* first_put = pn > p0 ? &s_ht_put[p0 & (HT_PUTS - 1u)] : 0;
    const HtJob* first_job = jn > j0 ? &s_ht_job[j0 & (HT_JOBS - 1u)] : 0;
    fprintf(stderr, "[hole] #%d park io=0x%08X ea=0x%08X word=%08X put=0x%08X standing %llu ms;"
                    " kept %u PUTs (back %.1f ms) and %u jobs (back %.1f ms)\n",
            rep, io, ea, word, put, age_ms, pn - p0,
            first_put ? ht_ago_ms(now, first_put->t) : 0.0, jn - j0,
            first_job ? ht_ago_ms(now, first_job->t0) : 0.0);
    /* The command words around the park, as they stand now. */
    if (ea >= 32u) {
        char line[512]; int o = 0;
        for (int i = -8; i < 24; i++) {
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%08X", i == 0 ? " [" : " ",
                          ht_be32(vm_base + (uint32_t)(ea + (uint32_t)(i * 4))));
            if (i == 0) o += snprintf(line + o, sizeof line - (size_t)o, "]");
        }
        fprintf(stderr, "[hole]   words park-32..park+92:%s\n", line);
    }

    /* PUTs that covered the park word, oldest first (at most 16, newest kept). */
    { uint32_t hit[16]; int nh = 0, total = 0;
      for (uint32_t n = p0; n < pn; n++) {
          const HtPut* p = &s_ht_put[n & (HT_PUTS - 1u)];
          if (p->seq != n + 1u) continue;
          if (ea < p->ea || ea >= p->ea + p->size) continue;
          total++;
          if (nh == 16) { memmove(hit, hit + 1, sizeof hit[0] * 15u); nh = 15; }
          hit[nh++] = n;
      }
      if (!total) fprintf(stderr, "[hole]   no kept PUT covers the park\n");
      else fprintf(stderr, "[hole]   %d kept PUT(s) cover the park%s:\n", total,
                   total > 16 ? " (the last 16)" : "");
      for (int i = 0; i < nh; i++)
          ht_print_put("cover", &s_ht_put[hit[i] & (HT_PUTS - 1u)], ea, now); }

    /* Jobs whose descriptor or input names the park, newest first. */
    { int named = 0;
      for (uint32_t n = jn; n-- > j0 && named < 6; ) {
          const HtJob* j = &s_ht_job[n & (HT_JOBS - 1u)];
          if (j->seq != n + 1u) continue;
          char where[48]; where[0] = 0;
          for (uint32_t i = 0; i < j->nd && !where[0]; i++)
              if (j->d[i] == ea || j->d[i] == io) snprintf(where, sizeof where, "desc+0x%X%s", i * 4u, j->d[i] == io ? " (io)" : "");
          for (uint32_t i = 0; i < j->ni && !where[0]; i++)
              if (j->in[i] == ea || j->in[i] == io) snprintf(where, sizeof where, "in+0x%X%s", i * 4u, j->in[i] == io ? " (io)" : "");
          if (!where[0]) continue;
          named++;
          ht_print_job("named", j, now, where);
          int np = 0;
          for (uint32_t m = p0; m < pn && np < 16; m++) {
              const HtPut* p = &s_ht_put[m & (HT_PUTS - 1u)];
              if (p->seq == m + 1u && p->job == j->seq) { ht_print_put("  its put", p, ea, now); np++; }
          }
          if (!np) fprintf(stderr, "[hole]     (no kept PUT from this job)\n");
      }
      if (!named) fprintf(stderr, "[hole]   no kept job descriptor or input names ea 0x%08X or io 0x%08X\n", ea, io); }

    /* Jobs still running. */
    { int nr = 0;
      for (uint32_t n = jn; n-- > j0 && nr < 8; ) {
          const HtJob* j = &s_ht_job[n & (HT_JOBS - 1u)];
          if (j->seq != n + 1u || j->t1) continue;
          nr++;
          ht_print_job("running", j, now, 0);
      }
      if (!nr) fprintf(stderr, "[hole]   no job running\n"); }

    /* The nearest PUTs below and above the park (the latest PUT per address). */
    { uint32_t below[6], above[6]; int nb = 0, na = 0;
      for (uint32_t n = pn; n-- > p0; ) {
          const HtPut* p = &s_ht_put[n & (HT_PUTS - 1u)];
          if (p->seq != n + 1u) continue;
          if (ea >= p->ea && ea < p->ea + p->size) continue;      /* listed above */
          const uint32_t d = p->ea < ea ? ea - p->ea : p->ea - ea;
          if (d > 0x2000u) continue;
          uint32_t* set = p->ea < ea ? below : above;
          int* cnt = p->ea < ea ? &nb : &na;
          int dup = 0;
          for (int i = 0; i < *cnt; i++)
              if (s_ht_put[set[i] & (HT_PUTS - 1u)].ea == p->ea) { dup = 1; break; }
          if (dup) continue;
          if (*cnt < 6) { set[(*cnt)++] = n; continue; }
          /* Keep the six closest: replace the farthest if this one is closer. */
          int far_i = 0; uint32_t far_d = 0;
          for (int i = 0; i < 6; i++) {
              const uint32_t e = s_ht_put[set[i] & (HT_PUTS - 1u)].ea;
              const uint32_t dd = e < ea ? ea - e : e - ea;
              if (dd > far_d) { far_d = dd; far_i = i; }
          }
          if (d < far_d) set[far_i] = n;
      }
      for (int pass = 0; pass < 2; pass++) {
          uint32_t* set = pass ? above : below; const int cnt = pass ? na : nb;
          for (int i = 0; i < cnt; i++)
              ht_print_put(pass ? "above" : "below", &s_ht_put[set[i] & (HT_PUTS - 1u)], ea, now);
      }
      if (!nb && !na) fprintf(stderr, "[hole]   no kept PUT within 0x2000 bytes of the park\n"); }
    fflush(stderr);
}

/* Arm on the first job when GCM_HOLE_TRACE is set. */
static int ht_on(void)
{
    int st = __atomic_load_n(&s_ht_state, __ATOMIC_ACQUIRE);
    if (st == 3) return 1;
    if (st != 0) return 0;
    const char* e = getenv("GCM_HOLE_TRACE");
    if (!e || !*e || (*e == '0' && !e[1])) {
        __atomic_store_n(&s_ht_state, 1, __ATOMIC_RELEASE);
        return 0;
    }
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s_ht_state, &expect, 2, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == 3;
    s_ht_put = (HtPut*)calloc(HT_PUTS, sizeof *s_ht_put);
    s_ht_job = (HtJob*)calloc(HT_JOBS, sizeof *s_ht_job);
    if (!s_ht_put || !s_ht_job) {
        free(s_ht_put); free(s_ht_job); s_ht_put = 0; s_ht_job = 0;
        fprintf(stderr, "[hole-trace] no memory for the trace -- off\n");
        __atomic_store_n(&s_ht_state, 1, __ATOMIC_RELEASE);
        return 0;
    }
    { const int n = atoi(e); if (n > 1) s_ht_reports = n; }
    g_spu_dma_note_hook = ht_note_dma;
    g_spu_hole_report_hook = ht_report;
    fprintf(stderr, "[hole-trace] on: the last %u SPU PUTs and %u jobs are kept;"
                    " at most %d reports\n", HT_PUTS, HT_JOBS, s_ht_reports);
    __atomic_store_n(&s_ht_state, 3, __ATOMIC_RELEASE);
    return 1;
}

static void ht_job_begin(uint32_t job_ea, int image_id, uint32_t dsz,
                         const uint8_t* ls, uint32_t io_ls, uint32_t io_used)
{
    t_ht_job = 0;
    if (!ht_on()) return;
    const uint32_t n = __atomic_fetch_add(&s_ht_job_n, 1u, __ATOMIC_RELAXED);
    HtJob* j = &s_ht_job[n & (HT_JOBS - 1u)];
    __atomic_store_n(&j->seq, 0u, __ATOMIC_RELAXED);
    j->t0 = ht_now(); j->t1 = 0;
    j->ea = job_ea;
    j->n_get = j->n_put = j->last_get = 0;
    j->img = (uint16_t)image_id;
    j->spu = (uint8_t)g_spurs_job_spu_num;
    j->thr = ht_thr();
    j->nd = dsz / 4u < HT_SNAP ? dsz / 4u : HT_SNAP;
    for (uint32_t i = 0; i < j->nd; i++) j->d[i] = g32(job_ea + i * 4u);
    j->ni = io_ls ? (io_used / 4u < HT_SNAP ? io_used / 4u : HT_SNAP) : 0;
    for (uint32_t i = 0; i < j->ni; i++) j->in[i] = ht_be32(ls + io_ls + i * 4u);
    __atomic_store_n(&j->seq, n + 1u, __ATOMIC_RELEASE);
    t_ht_job = n + 1u;
}

static void ht_job_end(void)
{
    HtJob* j = ht_cur_job();
    if (j) j->t1 = ht_now();
    t_ht_job = 0;
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    if (!entry || !job_ea) return -1;

    /* ---- descriptor ---------------------------------------------------- */
    uint32_t ea_bin   = (uint32_t)(g64(job_ea + JH_EA_BINARY) & ~1ull);
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return -1;

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint32_t io_ls     = p;                p += ALIGN1024(size_io);
    uint32_t out_ls    = p;                p += ALIGN1024(size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = p;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (p + sz > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (p > ctx_ls)
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13" crt (WWS/Edge jobs,
     * Twisted Metal's JobQueue jobs): jm2 parks an LS pointer to its kernel
     * block there, and the crt returns 0x80410A11 and STOPs before main when
     * it is 0 -- so the job never runs and never clears its completion
     * counter. Hand it the free 16 bytes after the context (zeroed); the crt
     * only stashes the pointer and clears the field again. */
    uint32_t jm_iface = memcmp(ls + 0x30, "JOBCRT Ver13", 12) == 0 ? ctx_ls + 0x30 : 0;
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data, so overlay
     * the kernel's view: SPU 0, our DMA tag, tracing off. */
    if (jm_iface) {
        memset(ls + 0x170, 0, 0x10);
        memset(ls + 0x1C0, 0, 0x10);
        ls32(ls, 0x1C8, g_spurs_job_spu_num);
        ls32(ls, 0x1CC, JOB_DMA_TAG);
        memset(ls + 0x210, 0, 0x10);
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static int _d = 0;
      if (getenv("SPURS_JOB_DESCDUMP") && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (atoi(getenv("SPURS_JOB_DESCDUMP")) >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (getenv("SPURS_JOB_DUMP")) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* GCM_HOLE_TRACE: this job's descriptor and input, before it runs. */
    ht_job_begin(job_ea, image_id, dsz, ls, io_ls, io_used);

    /* ---- enter the job -------------------------------------------------- */
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;
    memcpy(ctx.ls, ls, SPU_LS_SIZE);
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctx.gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctx.gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctx.gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx.gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx.gpr[3]._u32[0], ctx.gpr[4]._u32[0], ctx.gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    spu_run_with_halt(entry, &ctx);
    ht_job_end();

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    /* Keep the store readable. Jobs of one chain can end at the same time
     * (SPURS_JC_WORKERS), so one copy at a time: a job that finds another
     * copying leaves the store to it. */
    { static int s_job_ls_busy;
      if (!__atomic_exchange_n(&s_job_ls_busy, 1, __ATOMIC_ACQUIRE)) {
          memcpy(s_job_ls, ctx.ls, SPU_LS_SIZE);
          s_job_ls_valid = 1;
          __atomic_store_n(&s_job_ls_busy, 0, __ATOMIC_RELEASE);
      } }
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static int img = -2; static uint32_t lsa; static int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctx.ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctx.ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }

    g_spurs_job_mbox      = spu_channel_has_data(&ctx.ch_out_mbox)
                          ? ctx.ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx.ch_out_intr_mbox)
                          ? ctx.ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (getenv("SPURS_JOB_DUMP")) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx.status, ctx.stop_code, ctx.pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    spu_mfc_release(&ctx);
    spu_coh_unregister(&ctx);   /* stack local: out of the reserver set */
    { extern void spu_mfc_release(spu_context*); spu_mfc_release(&ctx); }
    free(ls);
    return 0;
}
