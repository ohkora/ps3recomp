/*
 * GCM_HOLE_TRACE (runtime/spu/spurs_job.c): the report says which SPU PUTs
 * covered a parked command-buffer hole and which jobs named it.
 *
 * Three jobs run through spu_run_spurs_job, each a C entry function that DMAs
 * through the real MFC channel path (spu_wrch -> mfc_do_transfer), so the
 * hook in spu_dma.h is exercised as well as the recorder:
 *   A  names hole 1 by EA in its descriptor and PUTs its commands over it;
 *      the "PPU" then writes the self-JUMP back, so the park stands again;
 *   B  names hole 2 by IO offset in its descriptor but PUTs 256 bytes past it;
 *   C  names hole 3 in its input buffer and asks for the report while it is
 *      still running.
 * The report for each must name the right PUTs and jobs. Run with the
 * environment variable unset, nothing is installed.
 *
 * Built and run by tools/test_spurs_hole_trace.py.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"
#include "spu_workload.h"   /* spu_run_spurs_job */

extern uint8_t g_vm_page_bitmap[65536 / 8];
extern void (*g_spu_dma_note_hook)(spu_context*, uint32_t, uint32_t, const uint8_t*, uint32_t);
extern void (*g_spu_hole_report_hook)(uint32_t, uint32_t, uint32_t, uint32_t, unsigned long long);
extern void (*g_spu_atomic_note_hook)(spu_context*, uint32_t, uint32_t);

#define JOB_A    0x00010000u
#define JOB_B    0x00010100u
#define JOB_C    0x00010200u
#define BIN_EA   0x00020000u
#define CMD_BASE 0x00040000u     /* IO offset 0 of the "command buffer" */
#define HOLE1    (CMD_BASE + 0x100u)
#define HOLE2    (CMD_BASE + 0x200u)
#define HOLE3    (CMD_BASE + 0x400u)
#define IN_EA    0x00050000u
#define LOCK_EA  0x00060000u

static void put32(uint32_t ea, uint32_t v)
{
    g_guest_mem[ea] = (uint8_t)(v >> 24); g_guest_mem[ea + 1] = (uint8_t)(v >> 16);
    g_guest_mem[ea + 2] = (uint8_t)(v >> 8); g_guest_mem[ea + 3] = (uint8_t)v;
}
static void put16(uint32_t ea, uint16_t v)
{
    g_guest_mem[ea] = (uint8_t)(v >> 8); g_guest_mem[ea + 1] = (uint8_t)v;
}
static void ls_put32(spu_context* c, uint32_t lsa, uint32_t v)
{
    uint8_t* p = c->ls + (lsa & SPU_LS_MASK);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t self_jump(uint32_t ea) { return 0x20000000u | (ea - CMD_BASE); }

/* The channel writes a lifted job issues for one PUT. */
static void dma_put(spu_context* c, uint32_t lsa, uint32_t ea, uint32_t size)
{
    u128 v; memset(&v, 0, sizeof v);
    v._u32[0] = lsa;         spu_wrch(c, MFC_LSA, v);
    v._u32[0] = 0;           spu_wrch(c, MFC_EAH, v);
    v._u32[0] = ea;          spu_wrch(c, MFC_EAL, v);
    v._u32[0] = size;        spu_wrch(c, MFC_Size, v);
    v._u32[0] = 1;           spu_wrch(c, MFC_TagID, v);
    v._u32[0] = MFC_PUT_CMD; spu_wrch(c, MFC_Cmd, v);
}

static void job_a(spu_context* c)          /* fills hole 1 */
{
    ls_put32(c, 0x8000, 0x00041234u); ls_put32(c, 0x8004, 0xCAFEF00Du);
    ls_put32(c, 0x8008, 0); ls_put32(c, 0x800C, 0);
    dma_put(c, 0x8000, HOLE1, 16);
}
/* A GET and a GETLLAR, so the per-image history has one of each kind. */
static void dma_get(spu_context* c, uint32_t lsa, uint32_t ea, uint32_t size)
{
    u128 v; memset(&v, 0, sizeof v);
    v._u32[0] = lsa;         spu_wrch(c, MFC_LSA, v);
    v._u32[0] = 0;           spu_wrch(c, MFC_EAH, v);
    v._u32[0] = ea;          spu_wrch(c, MFC_EAL, v);
    v._u32[0] = size;        spu_wrch(c, MFC_Size, v);
    v._u32[0] = 2;           spu_wrch(c, MFC_TagID, v);
    v._u32[0] = MFC_GET_CMD; spu_wrch(c, MFC_Cmd, v);
}
static void getllar(spu_context* c, uint32_t lsa, uint32_t ea)
{
    u128 v; memset(&v, 0, sizeof v);
    v._u32[0] = lsa;             spu_wrch(c, MFC_LSA, v);
    v._u32[0] = 0;               spu_wrch(c, MFC_EAH, v);
    v._u32[0] = ea;              spu_wrch(c, MFC_EAL, v);
    v._u32[0] = MFC_GETLLAR_CMD; spu_wrch(c, MFC_Cmd, v);
}

static void job_b(spu_context* c)          /* misses hole 2 */
{
    dma_get(c, 0x9000, IN_EA, 32);
    getllar(c, 0x9080, LOCK_EA);
    ls_put32(c, 0x8000, 0x0004BEEFu); ls_put32(c, 0x8004, 0x11112222u);
    dma_put(c, 0x8000, HOLE2 + 0x100u, 16);
}
static void job_c(spu_context* c)          /* still running at the report */
{
    (void)c;
    g_spu_hole_report_hook(HOLE3 - CMD_BASE, HOLE3, self_jump(HOLE3), 0x800u, 2000);
}

static void build_job(uint32_t job, uint32_t user0, uint32_t user1, int with_input)
{
    put32(job + 0x00, 0);
    put32(job + 0x04, BIN_EA);
    put16(job + 0x08, 0x400 >> 4);
    put16(job + 0x0A, with_input ? 8 : 0);  /* input DMA list bytes */
    put32(job + 0x14, with_input ? 0x100 : 0);
    put32(job + 0x30, user0);
    put32(job + 0x34, user1);
}

int main(int argc, char** argv)
{
    memset(g_vm_page_bitmap, 0xFF, sizeof g_vm_page_bitmap);
    const int off = argc > 1 && !strcmp(argv[1], "off");

    put32(HOLE1, self_jump(HOLE1));
    put32(HOLE2, self_jump(HOLE2));
    put32(HOLE3, self_jump(HOLE3));
    build_job(JOB_A, HOLE1, 0, 0);
    build_job(JOB_B, 0x12345678u, HOLE2 - CMD_BASE, 0);
    build_job(JOB_C, 0x40u, IN_EA, 1);           /* input list: 0x40 bytes from IN_EA */
    put32(IN_EA + 8, HOLE3);

    spu_run_spurs_job(job_a, 5, JOB_A, 256);
    if (off) {
        printf("off: dma hook %s, report hook %s, atomic hook %s\n",
               g_spu_dma_note_hook ? "SET" : "null", g_spu_hole_report_hook ? "SET" : "null",
               g_spu_atomic_note_hook ? "SET" : "null");
        return 0;
    }
    if (!g_spu_dma_note_hook || !g_spu_hole_report_hook || !g_spu_atomic_note_hook) {
        printf("FAIL: GCM_HOLE_TRACE set but the hooks are not installed\n");
        return 1;
    }
    /* The job's commands landed, then the "PPU" wrote the park back. */
    printf("hole1 after job A: %08X\n",
           ((uint32_t)g_guest_mem[HOLE1] << 24) | ((uint32_t)g_guest_mem[HOLE1 + 1] << 16) |
           ((uint32_t)g_guest_mem[HOLE1 + 2] << 8) | g_guest_mem[HOLE1 + 3]);
    put32(HOLE1, self_jump(HOLE1));
    spu_run_spurs_job(job_b, 6, JOB_B, 256);
    fflush(stdout);
    fprintf(stderr, "=== report 1\n");
    g_spu_hole_report_hook(HOLE1 - CMD_BASE, HOLE1, self_jump(HOLE1), 0x800u, 2000);
    fprintf(stderr, "=== report 2\n");
    g_spu_hole_report_hook(HOLE2 - CMD_BASE, HOLE2, self_jump(HOLE2), 0x800u, 2001);
    fprintf(stderr, "=== report 3\n");
    spu_run_spurs_job(job_c, 5, JOB_C, 256);
    fprintf(stderr, "=== report 4\n");
    g_spu_hole_report_hook(HOLE3 - CMD_BASE, HOLE3, self_jump(HOLE3), 0x800u, 2002);
    fprintf(stderr, "=== end\n");
    printf("done\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the job runner and the SPU channel layer, stubbed as in
 * test_spurs_job_stack.c. None of it is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
