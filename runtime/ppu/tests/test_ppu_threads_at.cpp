// PS3_THREADS_AT: at the times asked for, a report of every guest thread --
// the HLE import or lv2 syscall it is inside, its lr/r3/sp, and the callers
// on its guest stack.
//
// Three guest threads are registered the way the boot path and
// sys_ppu_thread_create register them: one inside an HLE import, one inside a
// syscall, one in neither. The first two have a guest stack two callers deep.
// The runner (tools/test_ppu_threads_at.py) sets PS3_THREADS_AT and checks the
// reports this prints to stderr.
//
// Built by tools/test_ppu_threads_at.py against the real ppu_loader.cpp.
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include "win32_compat.h"

static void nop(ppu_context*) {}

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[3] = {{0x20000, nop, "outer"}, {0x30000, nop, "inner"},
                                      {0, nullptr, nullptr}};
const uint64_t function_table_count = 2;

// ppu_hle.cpp keeps this table; the fixture fills it by hand.
const char* g_hle_inflight[64];
extern uint32_t g_sc_inflight[64];
void ppu_resv_register(ppu_context* c);

// The report runs on a thread of its own, started through the Win32 shim.
struct start { PS3_THREAD_FN fn; LPVOID arg; };
static void* trampoline(void* p)
{
    start s = *(start*)p;
    delete (start*)p;
    s.fn(s.arg);
    return nullptr;
}
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN fn, LPVOID arg, DWORD, DWORD*)
{
    pthread_t t;
    if (pthread_create(&t, nullptr, trampoline, new start{fn, arg})) return nullptr;
    pthread_detach(t);
    return (HANDLE)(uintptr_t)1;
}
BOOL CloseHandle(HANDLE) { return TRUE; }
}

static void be32(uint32_t ea, uint32_t v)
{
    v = __builtin_bswap32(v);
    std::memcpy(vm_base + ea, &v, 4);
}

// A guest stack two callers deep: sp -> frame A -> frame B -> end. The ELFv1
// back chain is the 64-bit word at each frame's base; the caller's lr sits at
// back chain + 0x10.
static void stack(uint32_t sp, uint32_t ra_outer, uint32_t ra_inner)
{
    const uint32_t a = sp + 0x80, b = sp + 0x100;
    be32(sp + 4, a);
    be32(a + 0x14, ra_inner);
    be32(a + 4, b);
    be32(b + 0x14, ra_outer);
    be32(b + 4, 0);
}

static ppu_context ctx[3];

int main()
{
    void* p = mmap(nullptr, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;

    // tid 1: the entry thread, parked in an HLE import, called from
    // outer+0x10 -> inner+0x20.
    ctx[0].thread_id = 1;
    ctx[0].gpr[1] = 0x00F00000;
    ctx[0].gpr[3] = 0x1234;
    ctx[0].lr = 0x00030024;
    stack(0x00F00000, 0x00020010, 0x00030020);
    g_hle_inflight[1] = "sys_lwmutex_lock";

    // tid 7: inside lv2 syscall 130 (sys_event_queue_receive).
    ctx[1].thread_id = 7;
    ctx[1].gpr[1] = 0x00E00000;
    ctx[1].gpr[3] = 0x5;
    ctx[1].lr = 0x00020044;
    stack(0x00E00000, 0x00020040, 0x00030050);
    g_sc_inflight[7] = 130;

    // tid 9: in neither (running guest code, or exited).
    ctx[2].thread_id = 9;
    ctx[2].gpr[1] = 0x00D00000;
    ctx[2].lr = 0x00020100;

    for (ppu_context& c : ctx) ppu_resv_register(&c);

    // The runner asks for reports at about 0.1 s and 0.3 s; wait past both.
    struct timespec ts = {0, 600 * 1000 * 1000};
    nanosleep(&ts, nullptr);
    std::fflush(stderr);
    std::printf("fixture done\n");
    return 0;
}
