/*
 * ps3recomp - a returned cellFsWrite is in the file (ppu_fs.cpp).
 *
 * ppu_fs.cpp keeps each guest fd as its own stdio stream on its own host
 * descriptor. Bytes that fwrite leaves in a stream's buffer are invisible to
 * another fd on the same file, and are lost if the process is killed before
 * the next seek or close. On cellFs a write that has returned is in the file.
 * FIOS builds its HDD cache this way: one fd fills the data file while others
 * read it back, and a test run that ends by killing the title leaves the
 * index pointing at data that never arrived.
 *
 *   tools/test_ppu_fs_write_visible.py [--exe-out PATH]
 *   test_fs_write_visible DIR                  two fds on one file
 *   test_fs_write_visible --write-and-exit DIR write 1000 bytes, then _Exit
 *                                              without closing (the runner
 *                                              checks the host file)
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

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

/* Guest memory layout used below. */
enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, POSP = 0x510,
                  WBUF = 0x1000 /* 40 KB */, RBUF = 0x10000 /* 64 KB */ };
enum : uint32_t { O_RDONLY_ = 0x0, O_RDWR_CREAT = 0x42, O_RDWR_CREAT_TRUNC = 0x242 };

static uint32_t fs_open(uint32_t flags, uint32_t* rc)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = PATH; ctx.gpr[4] = flags; ctx.gpr[5] = FDP;
    cellFsOpen(&ctx);
    *rc = (uint32_t)ctx.gpr[3];
    return rd32(FDP);
}
static uint64_t fs_write(uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsWrite(&ctx);
    return rd64(NP);
}
static uint64_t fs_read(uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsRead(&ctx);
    return rd64(NP);
}
static uint64_t fs_seek(uint32_t fd, int64_t off)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = (uint64_t)off; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = POSP;
    cellFsLseek(&ctx);
    return rd64(POSP);
}
static void fs_close(uint32_t fd)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd;
    cellFsClose(&ctx);
}
static int all_are(uint32_t at, uint64_t n, uint8_t v)
{
    for (uint64_t i = 0; i < n; i++) if (vm_base[at + i] != v) return 0;
    return 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x20000];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const int die = argc > 2 && strcmp(argv[1], "--write-and-exit") == 0;
    const char* dir = argc > 1 ? argv[argc - 1] : NULL;
    if (!dir || (argc > 1 && strcmp(dir, "--write-and-exit") == 0)) {
        printf("usage: test_fs_write_visible [--write-and-exit] DIR\n");
        return 2;
    }
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_write_visible.dat", dir);
    ppu_vfs_root = dir;
    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_write_visible.dat");
    uint32_t rc = 0;

    if (die) {
        /* A run that ends with the title killed: write, then leave without a
         * close, a seek or an exit handler, as TerminateProcess or SIGKILL
         * would. */
        remove(host);
        const uint32_t fd = fs_open(O_RDWR_CREAT_TRUNC, &rc);
        if (rc != 0) { printf("FAIL (could not create %s)\n", host); return 1; }
        memset(mem + WBUF, 0x33, 1000);
        const uint64_t wrote = fs_write(fd, WBUF, 1000);
        printf("wrote %llu\n", (unsigned long long)wrote);
        std::_Exit(wrote == 1000 ? 0 : 1);
    }

    remove(host);
    /* The writer: FIOS's open, O_RDWR | O_CREAT on a file that does not exist. */
    const uint32_t wfd = fs_open(O_RDWR_CREAT, &rc);
    CHECK(rc == 0, "cellFsOpen(O_RDWR|O_CREAT) returns CELL_OK (fd %u)", wfd);
    if (rc != 0) { printf("FAIL (could not create %s)\n", host); return 1; }

    /* A small write, smaller than any stdio buffer: all of it would stay in
     * the writer's buffer. */
    memset(mem + WBUF, 0x11, 300);
    const uint64_t w1 = fs_write(wfd, WBUF, 300);
    CHECK(w1 == 300, "cellFsWrite writes 300 bytes (%llu)", (unsigned long long)w1);

    /* The reader: a second fd on the same file. */
    const uint32_t rfd = fs_open(O_RDONLY_, &rc);
    CHECK(rc == 0 && rfd != wfd, "a second cellFsOpen(O_RDONLY) on the same file (fd %u)", rfd);
    memset(mem + RBUF, 0xEE, 0x10000);
    const uint64_t r1 = fs_read(rfd, RBUF, 0x10000);
    CHECK(r1 == 300 && all_are(RBUF, 300, 0x11),
          "the second fd reads the 300 bytes the first one wrote (got %llu)", (unsigned long long)r1);

    /* A large write: the stream writes the bulk and buffers the tail. The
     * reader seeks before it reads, as FIOS does. */
    memset(mem + WBUF, 0x22, 40000);
    const uint64_t w2 = fs_write(wfd, WBUF, 40000);
    CHECK(w2 == 40000, "cellFsWrite writes 40000 more bytes (%llu)", (unsigned long long)w2);
    const uint64_t pos = fs_seek(rfd, 300);
    CHECK(pos == 300, "the reader seeks to 300");
    memset(mem + RBUF, 0xEE, 0x10000);
    const uint64_t r2 = fs_read(rfd, RBUF, 0x10000);
    CHECK(r2 == 40000 && all_are(RBUF, 40000, 0x22),
          "the second fd reads all 40000 bytes, tail included (got %llu)", (unsigned long long)r2);

    /* What the host file holds before either fd is closed. */
    {
        uint64_t hn = 0;
        if (FILE* f = fopen(host, "rb")) {
            static uint8_t disk[0x10000];
            size_t k;
            while ((k = fread(disk, 1, sizeof disk, f)) > 0) hn += k;
            fclose(f);
        }
        CHECK(hn == 40300, "host file is %llu bytes with both fds still open (want 40300)",
              (unsigned long long)hn);
    }
    fs_close(rfd);
    fs_close(wfd);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
