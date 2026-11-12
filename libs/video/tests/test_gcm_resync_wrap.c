/* A FIFO resync across the ring's wrap point sweeps both pieces it skips
 * (cellGcmSys.c gcm_fifo_resync_why).
 *
 * When the title's ring has wrapped, put sits behind get. A resync then skips
 * the rest of this lap (get up to the JUMP back to the ring's start) and the
 * start of the ring up to put. The sweep used to start at s_last_ring_off,
 * which sat above put, so it swept nothing: a flip marker in the skipped tail
 * was never counted, and every later flip waited for the drain forever.
 *
 * The walker parks near the end of the ring with put near its start. Between
 * the park and the wrap JUMP: a flip marker, a JUMP out of the ring to a block
 * that holds the next flip marker and a release and JUMPs back, a CALL to a
 * block with another release and a RETURN, two NV406E releases, a hole an SPU
 * has yet to fill, a method whose argument reads like a JUMP to the ring's
 * start, and the frame's flip marker with the runtime's wrap JUMP after it.
 * After the
 * wrap JUMP: stale words from the last lap (an NV406E release that must NOT be
 * replayed). From the ring's start to put: a flip marker and another release.
 * The flip in the out-of-ring block is the one a title lost for good when the
 * sweep stopped at the JUMP out; the flip after the JUMP-like argument is the
 * one it lost when it took that argument for the wrap, which it did whenever
 * an unfilled hole kept it from walking the commands up to the real wrap.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_resync_wrap.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_resync_wrap
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* The walker's other consumers: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* The NV4097 sync methods the resync hands on, in order. */
static u32 seen_m[16], seen_v[16], seen_n;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (seen_n < 16) { seen_m[seen_n] = method; seen_v[seen_n] = data; seen_n++; }
    return 0;
}
int  rsx_draw_engine_enabled(void) { return 0; }
/* The walker hands vertex-program runs here (#554) and asks for a vtable
 * backend (#466); this fixture has neither. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_set_display_buffer(u32 id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{ (void)id; (void)location; (void)offset; (void)pitch; (void)width; (void)height; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* One method, count 1, subchannel 0. */
static u32 put_method(u32 io, u32 method, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | method);
    gw32(IO_ADDR + io + 4, arg);
    return io + 8;
}

static void wait_past_grace(void)
{
    const unsigned long long t0 = ps3_ms_now();
    while (ps3_ms_now() - t0 <= GCM_PARK_GRACE_MS + 10u) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
#endif
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    u32 rb = 0, re = 0;
    check(gcm_fifo_ring_bounds(&rb, &re) && re > rb + 0x100u, "the context gives a ring");

    /* The tail of this lap. */
    const u32 park = re - 0x100u;
    u32 io = park;
    gw32(IO_ADDR + io, 0x20000000u | io); io += 4;      /* park: JUMP to itself */
    gw32(IO_ADDR + io, GCM_FLIP_MARKER | 1u); io += 4;  /* a queued flip */
    const u32 blk = 0x00080000u, sub = 0x00090000u;     /* blocks outside the ring */
    gw32(IO_ADDR + io, 0x20000000u | blk); io += 4;     /* out to a block ... */
    const u32 back = io;
    {
        u32 b = blk;
        gw32(IO_ADDR + b, GCM_FLIP_MARKER | 0u); b += 4; /* ... that closes a frame */
        b = put_method(b, 0x0064u, 0x0060u);
        b = put_method(b, 0x006Cu, 0x00006666u);
        gw32(IO_ADDR + b, 0x20000000u | back);            /* and JUMPs back */
        b = sub;
        b = put_method(b, 0x0064u, 0x0070u);
        b = put_method(b, 0x006Cu, 0x00007777u);
        gw32(IO_ADDR + b, 0x00020000u);                   /* RETURN */
    }
    gw32(IO_ADDR + io, sub | 2u); io += 4;              /* CALL a block */
    io = put_method(io, 0x0064u, 0x0030u);              /* NV406E semaphore offset */
    io = put_method(io, 0x006Cu, 0x0000AAAAu);          /* NV406E semaphore release */
    io = put_method(io, 0x0064u, 0x0080u);
    io = put_method(io, 0x006Cu, 0x00008888u);
    gw32(IO_ADDR + io, 0x20000000u | io); io += 4;      /* a hole an SPU has yet to fill, */
    gw32(IO_ADDR + io, 0xC0800000u); io += 4;           /* its reserved space not commands yet */
    io = put_method(io, 0x1FD8u, rb | 1u);              /* data that reads as JUMP -> start */
    gw32(IO_ADDR + io, GCM_FLIP_MARKER | 0u); io += 4;  /* the frame's flip, */
    s_fifo_wrap_jump_io = io;                           /* and the wrap the runtime wrote after it */
    gw32(IO_ADDR + io, 0x20000000u | rb); io += 4;      /* the wrap: JUMP to the start */
    io = put_method(io, 0x0064u, 0x0040u);              /* stale, from the last lap */
    io = put_method(io, 0x006Cu, 0x00000BADu);
    /* The start of the ring, up to put. */
    io = rb;
    gw32(IO_ADDR + io, 0); io += 4;                     /* NOP */
    gw32(IO_ADDR + io, GCM_FLIP_MARKER | 1u); io += 4;  /* the next flip */
    io = put_method(io, 0x0064u, 0x0050u);
    io = put_method(io, 0x006Cu, 0x00001234u);
    const u32 put = io;

    const u32 reached0 = s_fifo_flips_reached;
    s_fifo_flips_queued = reached0 + 4u;                /* the four flips above */
    s_fifo_getoff = park;
    vm_write32(ctrl + 4, park);
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();                         /* parks; the grace starts */
    check(s_fifo_getoff == park, "the first pass stops on the park");

    wait_past_grace();
    cellGcm_rsx_process_fifo();                         /* still parked: resync */
    check(s_fifo_getoff == put, "the resync moved get to put, across the wrap");
    check(s_fifo_flips_reached == reached0 + 4u,
          "every skipped flip marker was counted (tail, out-of-ring block, head)");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x80u) == 0x00008888u,
          "the release before an unfilled hole was replayed");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x60u) == 0x00006666u,
          "the release in the block the tail JUMPs to was replayed");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x70u) == 0x00007777u,
          "the release in the block the tail CALLs was replayed");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x30u) == 0x0000AAAAu,
          "the release in the rest of the lap was replayed");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x50u) == 0x00001234u,
          "the release at the start of the ring was replayed");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x40u) == 0,
          "the stale words after the wrap JUMP were not replayed");

    printf("[gcm_resync_wrap] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
