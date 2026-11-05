/* PAD_NO_HOST=1: cellPad leaves the computer's own controllers alone. Two
 * pads are plugged in and hold TRIANGLE. Without the switch they are opened,
 * read and rumbled (the control half, so the fake is shown to work). With it
 * none is opened, read or rumbled, ports 1 and up read unplugged, and port 0
 * stays the virtual pad that PAD_FILE still drives.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_no_host.c $(sdl2-config --libs) -o /tmp/test_pad_no_host && /tmp/test_pad_no_host
 *
 * No controller is opened: the test defines every SDL call the backend makes
 * (they take precedence over libSDL2's) and counts them.
 */
#include "../cellPad.c"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }   /* PAD_CLOCK=flips */
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* ---- the fake SDL: two pads, both holding TRIANGLE (SDL's Y) ------------ */
#define FAKE_PADS 2
static int s_pad[FAKE_PADS];
static int s_inits, s_lists, s_opens, s_reads, s_rumbles;

Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return 0; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; s_inits++; return 0; }
SDL_bool SDL_SetHint(const char* name, const char* value) { (void)name; (void)value; return SDL_TRUE; }
const char* SDL_GetError(void) { return "fake"; }
int SDL_NumJoysticks(void) { s_lists++; return FAKE_PADS; }
SDL_bool SDL_IsGameController(int idx) { return idx >= 0 && idx < FAKE_PADS ? SDL_TRUE : SDL_FALSE; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int idx) { return 10 + idx; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int idx)
{
    SDL_JoystickGUID g; memset(&g, 0, sizeof g);
    g.data[0] = (Uint8)(0xA0 + idx);
    return g;
}
SDL_GameController* SDL_GameControllerOpen(int idx)
{
    assert(idx >= 0 && idx < FAKE_PADS);
    s_opens++;
    return (SDL_GameController*)&s_pad[idx];
}
void SDL_GameControllerClose(SDL_GameController* gc) { (void)gc; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { (void)gc; return SDL_TRUE; }
const char* SDL_GameControllerName(SDL_GameController* gc) { (void)gc; return "Fake pad"; }
SDL_bool SDL_GameControllerHasRumble(SDL_GameController* gc) { (void)gc; return SDL_TRUE; }
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms)
{
    (void)gc; (void)ms;
    if (lo || hi) s_rumbles++;   /* the closing "motors off" on shutdown is not a rumble */
    return 0;
}
void SDL_GameControllerUpdate(void) {}
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b)
{
    (void)gc;
    s_reads++;
    return b == SDL_CONTROLLER_BUTTON_Y;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a) { (void)gc; (void)a; return 0; }
int SDL_PeepEvents(SDL_Event* ev, int n, SDL_eventaction action, Uint32 lo, Uint32 hi)
{
    (void)ev; (void)n; (void)action; (void)lo; (void)hi;
    return 0;
}

/* ---- helpers ------------------------------------------------------------ */
/* port_status[port] from cellPadGetInfo2 (guest big-endian). */
static u32 info_status(u32 port)
{
    const uint32_t ea = 0x1000;
    assert(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)ea) == CELL_OK);
    const uint8_t* b = vm_base + ea + 12 + port * 4;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}
/* DIGITAL2 of a fresh cellPadGetData(port). */
static unsigned data_digital2(u32 port)
{
    const uint32_t ea = 0x2000;
    usleep((PAD_POLL_INTERVAL_MS + 2) * 1000);
    assert(cellPadGetData(port, (CellPadData*)(uintptr_t)ea) == CELL_OK);
    const uint8_t* b = vm_base + ea + 4 + CELL_PAD_BTN_OFFSET_DIGITAL2 * 2;
    return (unsigned)b[0] << 8 | b[1];
}
static s32 set_act(u32 port, u8 small, u8 large)
{
    CellPadActParam* act = (CellPadActParam*)(vm_base + 0x3000);
    act->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = small;
    act->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = large;
    return cellPadSetActDirect(port, (CellPadActParam*)(uintptr_t)0x3000);
}
static void pad_file_put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}
#define TRIANGLE_BIT 0x10u   /* in DIGITAL2 */
#define CROSS_BIT    0x40u

/* ---- the tests ------------------------------------------------------------ */
int main(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    /* PAD_FILE is read once, on the first cellPadGetData, so it is set for
     * both halves. It stays empty until the second one. */
    char pad_file[] = "/tmp/test_pad_no_host_XXXXXX";
    int fd = mkstemp(pad_file);
    assert(fd >= 0);
    close(fd);
    setenv("PAD_FILE", pad_file, 1);

    /* Control: without the switch both pads are opened, read and rumbled. */
    unsetenv("PAD_NO_HOST");
    s_pad_no_host = -1;
    assert(cellPadInit(4) == CELL_OK);
    assert(s_inits == 1 && s_opens == FAKE_PADS);
    assert(info_status(1) == CELL_PAD_STATUS_CONNECTED);
    assert(data_digital2(0) & TRIANGLE_BIT);
    assert(s_reads > 0);
    assert(set_act(0, 1, 200) == CELL_OK && s_rumbles == 1);
    assert(cellPadEnd() == CELL_OK);

    /* PAD_NO_HOST=1: the same two pads are never listed, opened, read or
     * rumbled. */
    setenv("PAD_NO_HOST", "1", 1);
    s_pad_no_host = -1;
    s_inits = s_lists = s_opens = s_reads = s_rumbles = 0;
    assert(cellPadInit(4) == CELL_OK);
    assert(s_inits == 0 && s_lists == 0 && s_opens == 0);

    /* Port 0 is still the virtual pad, and none of the host's buttons get
     * through. Port 1 reads unplugged. */
    assert(info_status(0) == CELL_PAD_STATUS_CONNECTED);
    assert(info_status(1) == CELL_PAD_STATUS_DISCONNECTED);
    assert(!(data_digital2(0) & TRIANGLE_BIT));
    usleep((PAD_SDL_RESCAN_MS + 50) * 1000);   /* past the rescan period: still nothing listed */
    assert(!(data_digital2(0) & TRIANGLE_BIT));
    assert(s_lists == 0 && s_opens == 0 && s_reads == 0);

    /* A script still drives port 0: PAD_FILE presses CROSS for one report. */
    pad_file_put(pad_file, "0x4000 1\n");
    assert(data_digital2(0) & CROSS_BIT);
    assert(!(data_digital2(0) & CROSS_BIT));   /* one report, then released */

    /* Rumble: the title's request is accepted and kept, no motor runs. */
    assert(set_act(0, 1, 200) == CELL_OK);
    assert(s_rumble[0].lo == 200 * 257 && s_rumble[0].hi == 65535);
    assert(set_act(1, 1, 255) == CELL_OK);
    assert(s_rumbles == 0);

    assert(cellPadEnd() == CELL_OK);
    assert(s_rumbles == 0 && s_opens == 0);
    unlink(pad_file);
    printf("test_pad_no_host: all checks passed\n");
    return 0;
}
