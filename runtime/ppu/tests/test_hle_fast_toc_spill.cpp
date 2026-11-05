/*
 * ps3recomp - ps3_hle_call fills the caller's TOC slot on every way out,
 * the fast path included.
 *
 * A lifted caller reloads r2 with `ld r2, 0x28(r1)` after each import call
 * (PPC64 ELFv1 cross-module ABI), and ps3_hle_call is what writes that slot.
 * The fast path for leaf imports (ps3_hle_register_fast, used by
 * sys_lwmutex_lock/unlock) returned before the spill, so a frame whose first
 * import was a fast call reloaded r2 from a slot nothing had written: 0 on a
 * zeroed stack. PlayStation All-Stars Battle Royale (BCUS98472) exits during
 * start-up that way.
 *
 * Built against the real ppu_hle.cpp, with a small guest memory and stubs for
 * the rest of the runtime, by tools/test_hle_fast_toc_spill.py.
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void ps3_hle_register(uint32_t nid, const char* name, void* handler);
void ps3_hle_register_fast(uint32_t nid, int (*fn)(ppu_context*));
void ps3_hle_call(uint32_t nid, ppu_context* ctx);
}

/* ---- a small big-endian guest memory ---------------------------------- */
/* ppu_recomp.h maps these names to its inline fast paths; this fixture
 * provides the out-of-line forms ppu_hle.cpp links against. */
#undef vm_read32
#undef vm_read64
#undef vm_write32
#undef vm_write64
static uint8_t g_mem[0x10000];
uint8_t* vm_base = g_mem;

extern "C" uint32_t vm_read32(uint64_t a)
{
    const uint8_t* p = g_mem + (a & 0xFFFFu);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
extern "C" uint64_t vm_read64(uint64_t a)
{
    return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4);
}
extern "C" void vm_write32(uint64_t a, uint32_t v)
{
    uint8_t* p = g_mem + (a & 0xFFFFu);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
extern "C" void vm_write64(uint64_t a, uint64_t v)
{
    vm_write32(a, (uint32_t)(v >> 32));
    vm_write32(a + 4, (uint32_t)v);
}

/* ---- the rest of the runtime, unused on these paths ------------------- */
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;
extern "C" void ppu_dump_bctrl_ring(uint32_t, const char*) {}
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*) {}
extern "C" void ppu_guest_callstack(const char*) {}
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
extern "C" uint32_t ppu_prof_resolve_host(void*) { return 0; }
extern "C" void ppu_prof_stamp(void*, unsigned) {}
extern "C" uint32_t prx_resolve_export(uint32_t) { return 0; }
extern "C" void ps3_indirect_call(ppu_context*) {}
extern "C" void ps3_msf(const char*, ...) {}

/* ---- the imports under test ------------------------------------------- */
enum : uint32_t { NID_FAST = 0x1A2B3C4Du, NID_FULL = 0x5E6F7081u };

static int s_fast_calls = 0, s_full_calls = 0;

/* A leaf import's fast form: finishes the call itself, leaves r2 alone. */
static int fast_form(ppu_context* ctx)
{
    s_fast_calls++;
    ctx->gpr[3] = 0;
    return 1;
}
static uint64_t full_handler(void)
{
    s_full_calls++;
    return 0;
}

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

/* A fresh guest frame: r1 points at it, its TOC slot holds nothing yet. */
static void fresh_frame(ppu_context* ctx, uint32_t sp)
{
    memset(ctx, 0, sizeof(*ctx));
    memset(g_mem, 0, sizeof(g_mem));
    ctx->gpr[1] = sp;
    ctx->gpr[2] = 0x00C94760u;   /* the caller's TOC */
}

int main()
{
    const uint32_t sp = 0x8000u;

    ps3_hle_register(NID_FAST, "fast_import", (void*)full_handler);
    ps3_hle_register_fast(NID_FAST, fast_form);
    ps3_hle_register(NID_FULL, "full_import", (void*)full_handler);

    ppu_context ctx;

    fresh_frame(&ctx, sp);
    ps3_hle_call(NID_FAST, &ctx);
    CHECK(s_fast_calls == 1 && s_full_calls == 0, "the fast form handled the call");
    CHECK(ctx.gpr[2] == 0x00C94760u, "r2 is unchanged across the fast call");
    CHECK(vm_read64(sp + 0x28) == 0x00C94760u,
          "fast path wrote the caller's TOC to 0x28(r1)");

    fresh_frame(&ctx, sp);
    ps3_hle_call(NID_FULL, &ctx);
    CHECK(s_full_calls == 1, "the full handler ran");
    CHECK(vm_read64(sp + 0x28) == 0x00C94760u,
          "full path wrote the caller's TOC to 0x28(r1)");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
