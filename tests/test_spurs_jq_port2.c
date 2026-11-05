/* cellSpursJq port2 push/sync through the real libs/spurs/cellSpursJq.c.
 *
 * Unregistered, _cellSpursJobQueuePort2PushJobBody and cellSpursJobQueuePort2Sync
 * faked CELL_OK: the job was dropped and Sync returned at once over output the
 * job never wrote. This checks that a push hands the job's binary, descriptor
 * EA and size to the lifted-job dispatcher before it returns (so the job's
 * effects are visible to the PPU by Sync), that a MISS still returns CELL_OK,
 * and that malformed pushes are refused without dispatching.
 *
 * spu_workload_dispatch_job is replaced by a recorder that plays the job: it
 * writes a marker to the output EA held in the job's user data. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cellSpursJq.h"

#define MEM_SIZE 0x400000u   /* covers 0x330000, the SDK version a shifted create used as a pointer */
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* Write-watch / reservation hooks the inline vm_write accessors reference. */
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }

static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- recorder standing in for runtime/spu/spu_workload.c --------------- */
static int      n_calls;
static uint32_t last_image_ea, last_size, last_job, last_desc;
static int      lifted = 1;     /* 0: behave like an unregistered binary */

int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size)
{
    n_calls++;
    last_image_ea = (uint32_t)(image - vm_base);
    last_size = image_size;
    last_job = job_ea;
    last_desc = job_desc_size;
    if (!lifted) return 0;
    /* The "job": user data word at +0x34 is an output EA; write the binary's
     * first word there, so the PPU can see the job ran on this descriptor. */
    uint32_t out = rd32(job_ea + 0x34);
    if (out) wr32(out, rd32((uint32_t)(image - vm_base)));
    return 1;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { PORT = 0x1000, JOB = 0x2000, BIN = 0x10000, OUT = 0x3000 };

static void make_job(uint32_t job, uint32_t bin, uint32_t size_bin, uint32_t out)
{
    memset(vm_base + job, 0, 256);
    wr32(job + 0x00, 0);
    wr32(job + 0x04, bin | 0x1);                  /* low flag bit set: must be masked */
    wr32(job + 0x08, (size_bin >> 4) << 16);      /* sizeBinary (be u16) */
    wr32(job + 0x30, 0);
    wr32(job + 0x34, out);
}

int main(void)
{
    vm_base = calloc(1, MEM_SIZE);
    ppu_vm_size = MEM_SIZE;
    if (!vm_base) return 2;
    wr32(BIN, 0xCAFEF00Du);

    /* 1. A push runs the job before it returns. */
    make_job(JOB, BIN, 47648, OUT);
    s32 rc = _cellSpursJobQueuePort2PushJobBody(PORT, JOB, 256, 3, 0);
    CHECK(rc == CELL_OK, "push rc=0x%X", (unsigned)rc);
    CHECK(n_calls == 1, "dispatch calls=%d", n_calls);
    CHECK(last_image_ea == BIN, "image ea=0x%X (flag bits not masked?)", last_image_ea);
    CHECK(last_size == 47648, "image size=%u", last_size);
    CHECK(last_job == JOB && last_desc == 256, "job=0x%X desc=%u", last_job, last_desc);
    CHECK(rd32(OUT) == 0xCAFEF00Du, "job output not visible after push: 0x%08X", rd32(OUT));
    CHECK(cellSpursJobQueuePort2Sync(PORT, 0) == CELL_OK, "sync after push");
    CHECK(g_spurs_jq_port2_pushed == 1 && g_spurs_jq_port2_ran == 1, "counters %u/%u",
          g_spurs_jq_port2_pushed, g_spurs_jq_port2_ran);

    /* Upper register halves are ignored (the HLE passes 64-bit GPRs). */
    rc = _cellSpursJobQueuePort2PushJobBody(0xFFFFFFFF00000000ull | PORT,
                                            0xFFFFFFFF00000000ull | JOB, 128, 0, 0);
    CHECK(rc == CELL_OK && n_calls == 2 && last_job == JOB && last_desc == 128,
          "high halves: rc=0x%X calls=%d", (unsigned)rc, n_calls);

    /* 2. An odd descriptor size falls back to the dispatcher's default. */
    rc = _cellSpursJobQueuePort2PushJobBody(PORT, JOB, 7, 0, 0);
    CHECK(rc == CELL_OK && last_desc == 0, "bad sizeDesc passed through: %u", last_desc);

    /* 3. An unlifted binary is a MISS: no output, but the push succeeds. */
    lifted = 0;
    wr32(OUT, 0);
    rc = _cellSpursJobQueuePort2PushJobBody(PORT, JOB, 256, 0, 0);
    CHECK(rc == CELL_OK && n_calls == 4, "miss: rc=0x%X calls=%d", (unsigned)rc, n_calls);
    CHECK(rd32(OUT) == 0, "miss wrote output");
    CHECK(g_spurs_jq_port2_ran == 3, "ran=%u after miss", g_spurs_jq_port2_ran);
    lifted = 1;

    /* 4. Malformed pushes never reach the dispatcher. */
    int before = n_calls;
    CHECK(_cellSpursJobQueuePort2PushJobBody(0, JOB, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "null port");
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, 0, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "null job");
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, JOB + 4, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "misaligned job");
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, MEM_SIZE - 0x10, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "job past guest memory");
    make_job(JOB + 0x100, 0, 1024, 0);
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, JOB + 0x100, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "no binary");
    make_job(JOB + 0x100, BIN, 0, 0);
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, JOB + 0x100, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "zero sizeBinary");
    make_job(JOB + 0x100, MEM_SIZE - 0x100, 0x1000, 0);
    CHECK(_cellSpursJobQueuePort2PushJobBody(PORT, JOB + 0x100, 256, 0, 0) ==
          (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT, "binary past guest memory");
    CHECK(n_calls == before, "a malformed push was dispatched");
    CHECK(g_spurs_jq_port2_rejected == 7, "rejected=%u", g_spurs_jq_port2_rejected);
    CHECK(cellSpursJobQueuePort2Sync(0, 0) == (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT,
          "sync on a null port");

    /* 5. Create + Port2Create in the order BCUS98472 uses them:
     * _cellSpursCreateJobQueue(rev=2, sdk=0x00330000, spurs, jq, attr) and
     * Port2Create(port, jq). The SDK version must not be taken as a pointer,
     * and the port must end up naming the queue without the queue being
     * overwritten from the (empty) port. */
    {
        enum { SPURS = 0x8000, JQ = 0x9000, ATTR = 0xA000, PORT2 = 0xB000, SDK = 0x00330000 };
        wr32(SDK, 0x5A5A5A5Au);
        memset(vm_base + PORT2, 0, 128);
        CHECK(cellSpursJobQueueAttributeInitialize((CellSpursJobQueueAttribute*)(uintptr_t)ATTR) == CELL_OK,
              "attr init");
        /* Occupy slot 0 so the queue lands in a nonzero slot, which shows
         * whether the port really received it. */
        CHECK(_cellSpursCreateJobQueue(2, SDK, SPURS, JQ + 0x100, ATTR) == CELL_OK, "create #0");
        CHECK(_cellSpursCreateJobQueue(2, SDK, SPURS, JQ, ATTR) == CELL_OK, "create #1");
        CHECK(rd32(SDK) == 0x5A5A5A5Au, "create wrote through the SDK version as a pointer");
        uint32_t qword = *(uint32_t*)(vm_base + JQ);
        CHECK(qword == 1, "queue handle %u, expected slot 1", qword);
        CHECK(cellSpursJobQueuePort2Create((CellSpursJobQueuePort*)(uintptr_t)PORT2,
                                           (CellSpursJobQueue*)(uintptr_t)JQ) == CELL_OK, "port2 create");
        CHECK(*(uint32_t*)(vm_base + JQ) == 1, "Port2Create overwrote the queue");
        CHECK(*(uint32_t*)(vm_base + PORT2) == 1, "port does not name the queue");
    }

    fprintf(stderr, "spurs-jq-port2: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
