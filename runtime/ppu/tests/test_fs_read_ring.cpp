/*
 * ps3recomp - PS3_FS_RING (ppu_fs.cpp): which file transfer is behind a guest
 * buffer, and two threads moving one fd's position.
 *
 * Drives the real handlers over a small guest memory and prints a marker line
 * to stderr before each step; tools/test_ppu_fs_read_ring.py checks the
 * "[fsring]" lines between the markers.
 *
 *   test_fs_read_ring DIR          run the steps (with PS3_FS_RING set)
 *   test_fs_read_ring --off DIR    one read and one report (PS3_FS_RING unset)
 */
#include "ppu_fs.cpp"
#include <cstdlib>

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static void wr32(uint32_t a, uint32_t v) { vm_write32(a, v); }
static void wr64(uint32_t a, uint64_t v) { vm_write64(a, v); }

/* Guest memory layout. */
enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, POSP = 0x510, AIO = 0x600, AIOID = 0x640,
                  WBUF = 0x1000, RBUF1 = 0x20000, RBUF2 = 0x30000, RBUF3 = 0x34000,
                  RBUF4 = 0x38000, RBUF5 = 0x3C000, NOTHING = 0x50000 };
enum : uint32_t { O_RDONLY_ = 0x0, O_RDWR_ = 0x2 };

static void step(const char* name) { fflush(stdout); fprintf(stderr, "== %s ==\n", name); fflush(stderr); }

static ppu_context mk(uint32_t tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = tid;
    return ctx;
}
static uint32_t fs_open(uint32_t tid, uint32_t flags)
{
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = PATH; ctx.gpr[4] = flags; ctx.gpr[5] = FDP;
    cellFsOpen(&ctx);
    if (ctx.gpr[3] != 0) { printf("FAIL: cellFsOpen returned 0x%llX\n", (unsigned long long)ctx.gpr[3]); exit(1); }
    return rd32(FDP);
}
static uint64_t fs_seek(uint32_t tid, uint32_t fd, int64_t off)
{
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = fd; ctx.gpr[4] = (uint64_t)off; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = POSP;
    cellFsLseek(&ctx);
    return rd64(POSP);
}
static uint64_t fs_read(uint32_t tid, uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsRead(&ctx);
    return rd64(NP);
}
static uint64_t fs_write(uint32_t tid, uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsWrite(&ctx);
    return rd64(NP);
}
static void fs_aio_read(uint32_t tid, uint32_t fd, uint64_t off, uint32_t buf, uint64_t n)
{
    wr32(AIO + 0x00, fd); wr64(AIO + 0x08, off); wr32(AIO + 0x10, buf); wr64(AIO + 0x18, n);
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = AIO; ctx.gpr[4] = AIOID; ctx.gpr[5] = 0;   /* no completion callback */
    cellFsAioRead(&ctx);
}
static void fs_close(uint32_t tid, uint32_t fd)
{
    ppu_context ctx = mk(tid);
    ctx.gpr[3] = fd;
    cellFsClose(&ctx);
}

static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 7u + (i >> 8) * 13u + (i >> 16)); }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x60000];
    vm_base = mem; ppu_vm_size = sizeof mem;
    const int off = argc > 2 && strcmp(argv[1], "--off") == 0;
    const char* dir = argc > 1 ? argv[argc - 1] : NULL;
    if (!dir || dir[0] == '-') { printf("usage: test_fs_read_ring [--off] DIR\n"); return 2; }

    /* The file: 0x30000 bytes no two 64-byte stretches of which are alike. */
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_read_ring.dat", dir);
    { FILE* f = fopen(host, "wb");
      if (!f) { printf("FAIL: cannot create %s\n", host); return 1; }
      for (uint32_t i = 0; i < 0x30000; i++) fputc(pattern(i), f);
      fclose(f); }
    ppu_vfs_root = dir;
    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_read_ring.dat");

    if (off) {
        const uint32_t fd = fs_open(1, O_RDONLY_);
        fs_seek(1, fd, 0x1000);
        const uint64_t n = fs_read(1, fd, RBUF1, 0x100);
        step("off: report");
        ppu_fs_ring_report(RBUF1, 0x100, "off");
        fs_close(1, fd);
        printf("read %llu\n", (unsigned long long)n);
        return n == 0x100 && mem[RBUF1] == pattern(0x1000) ? 0 : 1;
    }

    /* Three guest threads share one fd, as FIOS's media threads share the
     * HDD cache's data file. */
    const uint32_t fd = fs_open(6, O_RDONLY_);

    step("1: a read, then the buffer untouched");
    fs_seek(6, fd, 0x10000);
    const uint64_t r1 = fs_read(6, fd, RBUF1, 0x8000);
    printf("r1 %llu (want 32768)\n", (unsigned long long)r1);
    ppu_fs_ring_report(RBUF1 + 0x100, 0x200, "one");

    step("2: the buffer written after the read");
    mem[RBUF1 + 0x4000] ^= 0xFF;
    ppu_fs_ring_report(RBUF1, 0x8000, "two");
    mem[RBUF1 + 0x4000] ^= 0xFF;

    step("3: thread 7 seeks and reads between thread 6's seek and read");
    fs_seek(6, fd, 0x4000);
    fs_seek(7, fd, 0x18000);
    const uint64_t r2 = fs_read(7, fd, RBUF2, 0x1000);
    const uint64_t r3 = fs_read(6, fd, RBUF3, 0x1000);
    printf("r2 %llu r3 %llu, RBUF3 holds file 0x%X (want 0x19000, the race)\n",
           (unsigned long long)r2, (unsigned long long)r3,
           mem[RBUF3] == pattern(0x19000) && mem[RBUF3 + 1] == pattern(0x19001) ? 0x19000u : 0u);

    step("4: the file written after the read");
    const uint32_t wfd = fs_open(8, O_RDWR_);
    memset(mem + WBUF, 0x55, 0x100);
    fs_seek(8, wfd, 0x10040);
    const uint64_t w1 = fs_write(8, wfd, WBUF, 0x100);
    printf("w1 %llu (want 256)\n", (unsigned long long)w1);
    ppu_fs_ring_report(RBUF1, 0x10, "four");

    step("5: an aio read");
    fs_aio_read(9, fd, 0x2000, RBUF5, 0x400);
    ppu_fs_ring_report(RBUF5 + 0x3F0, 0x20, "five");

    step("6: nothing touched it");
    ppu_fs_ring_report(NOTHING, 0x1000, "six");

    step("7: the ring wraps");
    for (int i = 0; i < 20; i++) { fs_seek(10, fd, 0x20000 + 16 * i); fs_read(10, fd, RBUF4 + 16 * i, 16); }
    ppu_fs_ring_report(RBUF1, 0x8000, "seven");
    ppu_fs_ring_report(RBUF4, 20 * 16, "seven-b");

    step("8: write the ring out");
    char out[1100];
    snprintf(out, sizeof out, "%s/ring.txt", dir);
    const long nw = ppu_fs_ring_write(out);
    long lines = 0, heads = 0;
    if (FILE* f = fopen(out, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) { if (line[0] == '#') heads++; else lines++; }
        fclose(f);
    }
    printf("ring_write %ld, %ld entry lines, %ld header lines\n", nw, lines, heads);

    fs_close(8, wfd);
    fs_close(6, fd);
    remove(host);
    remove(out);
    step("end");
    return 0;
}
