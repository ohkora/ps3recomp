/*
 * ps3recomp - cellSpursJq HLE implementation
 *
 * SPURS Job Queue — manages job submission and tracking.
 * The original push entry points accept jobs and mark them complete without
 * running them. The port2 push (_cellSpursJobQueuePort2PushJobBody) runs
 * the job's lifted SPU binary before it returns; see the port2 section.
 */

#include "cellSpursJq.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include "spu_workload.h"   /* spu_workload_dispatch_job: lifted SPURS job dispatch */
#include "ps3emu/log_first.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv */
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    u32 maxJobs;
    u32 jobCount;       /* number of pending (not-yet-completed) jobs */
    u32 nextJobId;
    u32 submitCount;    /* total jobs submitted over lifetime */
    u32 completeCount;  /* total jobs completed over lifetime */
    /*
     * Per-job completion ring: tracks the last CELL_SPURS_JQ_MAX_JOBS
     * job IDs that have completed.  Wait/TryWait check this.
     */
    u32 completedIds[CELL_SPURS_JQ_MAX_JOBS];
    u32 completedHead;  /* next write position in the ring */
} JqState;

static JqState s_queues[CELL_SPURS_JQ_MAX_QUEUES];

static int jq_trace(void);   /* SPURS_JQ_TRACE=1, see the port2 section */

/* Check whether a job ID has completed in the given queue */
static int jq_is_completed(const JqState* q, CellSpursJobId id)
{
    /* Job ID 0 is invalid / never assigned */
    if (id == 0) return 0;

    for (int i = 0; i < CELL_SPURS_JQ_MAX_JOBS; i++) {
        if (q->completedIds[i] == id)
            return 1;
    }
    return 0;
}

/* Record a job as completed */
static void jq_mark_completed(JqState* q, CellSpursJobId id)
{
    q->completedIds[q->completedHead] = id;
    q->completedHead = (q->completedHead + 1) % CELL_SPURS_JQ_MAX_JOBS;
    q->completeCount++;
    if (q->jobCount > 0)
        q->jobCount--;
}

static int jq_alloc(void)
{
    for (int i = 0; i < CELL_SPURS_JQ_MAX_QUEUES; i++) {
        if (!s_queues[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSpursJobQueueAttribute*);
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    memset(attr, 0, sizeof(*attr));
    attr->maxJobs = 64;
    attr->maxGrab = 1;
    /* Default priority: normal on all SPUs */
    for (int i = 0; i < 8; i++)
        attr->priorities[i] = CELL_SPURS_JQ_PRIORITY_NORMAL;

    return CELL_OK;
}

s32 cellSpursJobQueueAttributeSetMaxGrab(CellSpursJobQueueAttribute* attr, u32 maxGrab)
{
    attr = GUEST_PTR(attr, CellSpursJobQueueAttribute*);
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    attr->maxGrab = maxGrab;
    return CELL_OK;
}

s32 cellSpursCreateJobQueue(void* spurs, CellSpursJobQueue* jq,
                            const CellSpursJobQueueAttribute* attr,
                            void* buffer, u32 bufferSize)
{
    attr = GUEST_PTR(attr, const CellSpursJobQueueAttribute*);
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    (void)spurs; (void)buffer; (void)bufferSize;
    printf("[cellSpursJq] CreateJobQueue()\n");
    if (jq_trace())
        fprintf(stderr, "[cellSpursJq] CreateJobQueue(spurs=0x%08X jq=0x%08X attr=0x%08X buf=0x%08X size=%u)\n",
                (u32)(uintptr_t)spurs, jq ? (u32)((u8*)jq - vm_base) : 0,
                attr ? (u32)((const u8*)attr - vm_base) : 0,
                (u32)(uintptr_t)buffer, bufferSize);

    if (!jq || !attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    int slot = jq_alloc();
    if (slot < 0) return (s32)CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY;

    memset(&s_queues[slot], 0, sizeof(JqState));
    s_queues[slot].in_use = 1;
    s_queues[slot].maxJobs = attr->maxJobs;
    s_queues[slot].jobCount = 0;
    s_queues[slot].nextJobId = 1;
    s_queues[slot].submitCount = 0;
    s_queues[slot].completeCount = 0;
    s_queues[slot].completedHead = 0;

    *jq = (u32)slot;
    return CELL_OK;
}

s32 cellSpursDestroyJobQueue(CellSpursJobQueue* jq)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    printf("[cellSpursJq] DestroyJobQueue(%u)\n", slot);
    s_queues[slot].in_use = 0;
    return CELL_OK;
}

/* Host-pointer core. cellSpursJobQueuePort2PushSync reads the queue id out of
 * the port into a LOCAL and pushes with its address, so it cannot go through
 * the entry point -- that translates its argument as a guest EA and would
 * resolve a host stack address to a nonsense queue slot. Same shape as
 * cellHttpUtil's escape core. */
static s32 jq_push_host(const u32* jq, const void* job,
                        u32 jobSize, u32 tag, u32* id_ea)
{
    (void)job; (void)jobSize; (void)tag;

    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    JqState* q = &s_queues[slot];

    if (q->jobCount >= q->maxJobs)
        return (s32)CELL_SPURS_JQ_ERROR_QUEUE_FULL;

    u32 jobId = q->nextJobId++;
    q->jobCount++;
    q->submitCount++;

    if (id_ea) vm_write32((u32)(uintptr_t)id_ea, jobId);

    /*
     * Job is immediately "complete" since we don't execute SPU code,
     * but we now record completion so Wait/TryWait can track it.
     */
    jq_mark_completed(q, jobId);

    return CELL_OK;
}

s32 cellSpursJobQueuePush(CellSpursJobQueue* jq, const void* job,
                          u32 jobSize, u32 tag, CellSpursJobId* id)
{
    return jq_push_host(GUEST_PTR(jq, const u32*), job, jobSize, tag, id);
}

s32 cellSpursJobQueuePushJob(CellSpursJobQueue* jq, const CellSpursJob256* job,
                             u32 tag, CellSpursJobId* id)
{
    return cellSpursJobQueuePush(jq, job, sizeof(CellSpursJob256), tag, id);
}

s32 cellSpursJobQueuePort2Create(CellSpursJobQueue* jq, CellSpursJobQueuePort* port)
{
    /* Argument order unconfirmed against the SDK: trace both raw values and
     * the first word each points at, so a call site can be checked. */
    if (jq_trace())
        fprintf(stderr, "[cellSpursJq] Port2Create(r3=0x%08X [0x%08X] r4=0x%08X [0x%08X])\n",
                (u32)(uintptr_t)jq, jq ? vm_read32((u32)(uintptr_t)jq) : 0,
                (u32)(uintptr_t)port, port ? vm_read32((u32)(uintptr_t)port) : 0);
    port = GUEST_PTR(port, CellSpursJobQueuePort*);
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq || !port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    *port = *jq; /* Port is just a reference to the queue */
    return CELL_OK;
}

s32 cellSpursJobQueuePort2Destroy(CellSpursJobQueuePort* port)
{
    (void)port;
    return CELL_OK;
}

s32 cellSpursJobQueuePort2PushSync(CellSpursJobQueuePort* port,
                                   const void* job, u32 jobSize,
                                   u32 tag, CellSpursJobId* id)
{
    if (jq_trace())
        fprintf(stderr, "[cellSpursJq] Port2PushSync(r3=0x%08X r4=0x%08X r5=0x%X r6=0x%X r7=0x%08X)\n",
                (u32)(uintptr_t)port, (u32)(uintptr_t)job, jobSize, tag, (u32)(uintptr_t)id);
    port = GUEST_PTR(port, CellSpursJobQueuePort*);
    if (!port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    CellSpursJobQueue jq = *port;
    return jq_push_host((const u32*)&jq, job, jobSize, tag, id);
}

/* ---------------------------------------------------------------------------
 * Port2 job submission: the job actually runs.
 *
 * _cellSpursJobQueuePort2PushJobBody (the body behind the SDK's inline
 * cellSpursJobQueuePort2PushJob) hands the queue a CellSpursJobHeader -- the
 * same 48-byte header a job chain uses (eaBinary, sizeBinary, DMA lists, ...)
 * followed by the job's user data. On hardware the job-queue policy module
 * grabs it on an SPU, stages it in local store and enters the job's CRT.
 *
 * Unregistered, both port2 imports faked CELL_OK: the pushes were dropped, the
 * Sync "succeeded" at once, and the PPU went on to consume output the job
 * never wrote. PlayStation All-Stars (BCUS98472) does exactly that on its load
 * screen -- 11 pushes and 23 syncs, then a black frame for the whole load.
 *
 * We run the job synchronously on the pushing thread, through the same
 * fingerprint -> lifted-entry dispatch and jm2-style staging the job-chain
 * walker uses (spu_workload_dispatch_job / spu_run_spurs_job). A push returns
 * once the job has completed, so every job pushed through a port has finished
 * by the time the PPU calls Sync, and Sync has nothing left to wait for. An
 * unlifted binary is reported as a dispatch MISS (and dumped with
 * SPU_DUMP_MISS) instead of vanishing silently.
 *
 * Only the job pointer and descriptor size are needed, so the port object is
 * not interpreted. SPURS_JQ_TRACE=1 prints the raw arguments of every port2
 * call, to check the call sites against this ABI on a new title.
 * -----------------------------------------------------------------------*/
enum {                              /* CellSpursJobHeader, job_descriptor.h */
    JQ_JH_EA_BINARY   = 0x00,       /* be u64: binary EA, low bits = flags */
    JQ_JH_SIZE_BINARY = 0x08,       /* be u16: binary size >> 4           */
    JQ_JH_SIZE        = 0x30,
};

/* Top of the guest range a job may live in. ppu_vm_size is 0 until the
 * loader maps the image (and in unit tests that never set it); RSX local
 * memory at 0xC0000000 never holds a job, so cap there. */
extern uint32_t ppu_vm_size;
static u64 jq_guest_limit(void)
{
    return ppu_vm_size ? (u64)ppu_vm_size : 0xC0000000ull;
}

static int jq_trace(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("SPURS_JQ_TRACE"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s;
}

/* Counters, readable by tests. */
u32 g_spurs_jq_port2_pushed;     /* pushes that reached a job binary   */
u32 g_spurs_jq_port2_ran;        /* ... of which a lifted job ran      */
u32 g_spurs_jq_port2_rejected;   /* pushes refused as invalid          */

static s32 jq_port2_push(const char* who, u32 port_ea, u32 job_ea,
                         u32 size_desc, u32 tag, u32 flag)
{
    if (jq_trace())
        fprintf(stderr, "[cellSpursJq] %s(port=0x%08X job=0x%08X sizeDesc=%u tag=%u flag=0x%X)\n",
                who, port_ea, job_ea, size_desc, tag, flag);

    /* The header is 16-byte aligned on every path the SDK allows (128 for a
     * queue descriptor), and its binary must lie inside guest memory. */
    if (!port_ea || !job_ea || (job_ea & 0xF) ||
        (u64)job_ea + JQ_JH_SIZE > jq_guest_limit()) {
        g_spurs_jq_port2_rejected++;
        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 8))
              fprintf(stderr, "[cellSpursJq] %s: bad port/job (port=0x%08X job=0x%08X) -- rejected\n",
                      who, port_ea, job_ea); }
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    }

    u32 ea_bin   = (u32)(vm_read64(job_ea + JQ_JH_EA_BINARY) & ~0xFull);
    u32 size_bin = (u32)vm_read16(job_ea + JQ_JH_SIZE_BINARY) << 4;
    if (!ea_bin || !size_bin || size_bin > 0x40000u ||
        (u64)ea_bin + size_bin > jq_guest_limit()) {
        g_spurs_jq_port2_rejected++;
        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 8))
              fprintf(stderr, "[cellSpursJq] %s: job 0x%08X has no plausible binary "
                              "(ea=0x%08X size=%u) -- rejected\n",
                      who, job_ea, ea_bin, size_bin); }
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    }

    /* sizeDesc is the descriptor's byte size (64, 128, 256, ... up to 1 KB).
     * Anything else is not a size this ABI knows: run with jm2's default
     * rather than copy a wild length into local store. */
    if (size_desc < 64 || size_desc > 1024 || (size_desc & 15)) size_desc = 0;

    g_spurs_jq_port2_pushed++;
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[cellSpursJq] %s: job 0x%08X bin 0x%08X+%u sizeDesc=%u -- running\n",
                  who, job_ea, ea_bin, size_bin, size_desc); }
    if (spu_workload_dispatch_job(vm_base + ea_bin, size_bin, job_ea, size_desc))
        g_spurs_jq_port2_ran++;
    /* A MISS is reported by the dispatcher itself. The push still succeeds,
     * like a job chain's: the SDK has no error for "job did nothing", and
     * failing it would only send the title down an error path real hardware
     * never takes. */
    return CELL_OK;
}

/* SDK: _cellSpursJobQueuePort2PushJobBody(CellSpursJobQueuePort2* port,
 *          CellSpursJobHeader* job, unsigned sizeDesc, unsigned tag,
 *          unsigned flag) */
s32 _cellSpursJobQueuePort2PushJobBody(u64 port, u64 job, u64 sizeDesc,
                                       u64 tag, u64 flag)
{
    return jq_port2_push("Port2PushJobBody", (u32)port, (u32)job,
                         (u32)sizeDesc, (u32)tag, (u32)flag);
}

/* SDK: cellSpursJobQueuePort2Sync(CellSpursJobQueuePort2* port, unsigned flag)
 * Waits for every job pushed through the port. Pushes run to completion
 * before they return, so there is nothing outstanding. */
s32 cellSpursJobQueuePort2Sync(u64 port, u64 flag)
{
    if (jq_trace())
        fprintf(stderr, "[cellSpursJq] Port2Sync(port=0x%08X flag=0x%X) pushed=%u ran=%u\n",
                (u32)port, (u32)flag, g_spurs_jq_port2_pushed, g_spurs_jq_port2_ran);
    if (!(u32)port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSpursJobQueueSendSignal(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    /* No-op: jobs are already complete */
    return CELL_OK;
}

s32 cellSpursJobQueueWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    const JqState* q = &s_queues[slot];

    /*
     * If no jobs have ever been submitted, treat as an error —
     * nothing can complete if nothing was pushed.
     */
    if (q->submitCount == 0)
        return (s32)CELL_SPURS_JQ_ERROR_JOB_NOT_FOUND;

    if (!jq_is_completed(q, id))
        return (s32)CELL_SPURS_JQ_ERROR_JOB_NOT_FOUND;

    return CELL_OK;
}

s32 cellSpursJobQueueTryWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    const JqState* q = &s_queues[slot];

    /* No jobs submitted yet — return BUSY */
    if (q->submitCount == 0)
        return (s32)CELL_SPURS_JQ_ERROR_BUSY;

    if (!jq_is_completed(q, id))
        return (s32)CELL_SPURS_JQ_ERROR_BUSY;

    return CELL_OK;
}

s32 cellSpursJobQueueGetCount(CellSpursJobQueue* jq, u32* count)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq || !count) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, s_queues[slot].jobCount);
    return CELL_OK;
}

s32 cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueue* jq)
{
    (void)jq;
    return CELL_OK;
}
