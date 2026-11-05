#!/usr/bin/env python3
"""cellSpursCreateTask starts the task in the slot it took, whatever another thread does meanwhile.

The real cellSpursCreateTask and taskset builders run against a synthetic VM.
The fixture sizes the task image at the point another thread can interleave
(between this call publishing its slot in the shared taskset/task globals and
dispatching the task) and runs that thread's step there:

- a second cellSpursCreateTask on the same taskset, as two PPU threads do when
  a title creates its tasks from two threads;
- a task exit starting the next task of another taskset, which sets the same
  globals from an SPU thread.

Each dispatch has to name this call's taskset and the slot it wrote to the
taskId out-param, and that slot's TaskInfo has to hold this call's arguments.
"""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires POSIX C toolchain')
    raise SystemExit(0)
FIXTURE = r'''
#include "libs/spurs/cellSpurs.c"
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No SPU runs here; observer-only services are inert. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea; return 0;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}
void spu_taskset_mark_running(uint32_t taskset_ea, uint32_t t) {(void)taskset_ea;(void)t;}
void spu_taskset2_set_exit(uint32_t taskset_ea, uint32_t task, int exited, uint32_t code)
{(void)taskset_ea;(void)task;(void)exited;(void)code;}

static _Alignas(128) uint8_t memory[0x10000];
enum { FX_SPURS = 0x1000, FX_TASKSET = 0x4000, FX_OTHER_TASKSET = 0x6000, FX_ARGS = 0x8000,
       FX_IDS = 0x8800, FX_ELF = 0x9000, FX_CONTEXT = 0xA000 };
static unsigned failed;
static void expect(int ok, const char* what)
{ if (!ok) { failed++; fprintf(stderr, "FAIL: %s\n", what); } }

/* The task each dispatch runs the job as. spu_workload_dispatch_async takes it
 * from the globals when it is called (runtime/spu/spu_workload.c). */
static struct { uint32_t taskset, task, context; } runs[8];
static unsigned n_runs;
static void record(uint32_t taskset, uint32_t task, uint32_t context)
{
    if (n_runs < 8) { runs[n_runs].taskset = taskset; runs[n_runs].task = task;
                      runs[n_runs].context = context; }
    n_runs++;
}
int spu_workload_dispatch_async(const uint8_t* image, uint32_t size, uint32_t context)
{ (void)image; (void)size; record(g_ydkj_real_taskset_ea, g_ydkj_real_taskid, context); return 1; }
int spu_workload_dispatch_task(const uint8_t* image, uint32_t size, uint32_t context,
                               uint32_t taskset, uint32_t task)
{ (void)image; (void)size; record(taskset, task, context); return 1; }

static s32 create(uint32_t n)
{
    for (uint32_t w = 0; w < 4; w++) vm_write32(FX_ARGS + 0x10 * n + 4 * w, 0x11110000u * (n + 1) + w);
    return cellSpursCreateTask((CellSpursTaskset*)(uintptr_t)FX_TASKSET,
                               (CellSpursTaskId*)(uintptr_t)(FX_IDS + 4 * n),
                               (void*)(uintptr_t)FX_ELF, (void*)(uintptr_t)(FX_CONTEXT + 0x100 * n),
                               0, 0, FX_ARGS + 0x10 * n);
}

/* What another thread does between this call's write of the globals and its
 * dispatch. The image is sized in that window, so run it there, once. */
static enum { NONE, CREATE, EXIT } interleave;
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    (void)image; (void)max_avail;
    int what = interleave; interleave = NONE;
    if (what == CREATE) expect(create(1) == CELL_OK, "the other thread's CreateTask succeeds");
    if (what == EXIT) { g_ydkj_real_taskset_ea = FX_OTHER_TASKSET; g_ydkj_real_taskid = 5; }
    return 0x100;
}

static void fresh_taskset(void)
{
    expect(cellSpursCreateTaskset((CellSpurs*)(uintptr_t)FX_SPURS,
                                  (CellSpursTaskset*)(uintptr_t)FX_TASKSET, 0, NULL, 0) == CELL_OK,
           "taskset created");
    n_runs = 0; memset(runs, 0, sizeof(runs));
}

/* The dispatch for task n names this taskset and the slot n's call returned,
 * and that slot's TaskInfo holds n's arguments: the job runs n, not another. */
static void runs_as_created(unsigned run, uint32_t n, const char* what)
{
    char m[256];
    const uint32_t slot = vm_read32(FX_IDS + 4 * n);
    snprintf(m, sizeof m, "%s: dispatch names the taskset (got 0x%08X)", what, runs[run].taskset);
    expect(runs[run].taskset == FX_TASKSET, m);
    snprintf(m, sizeof m, "%s: dispatch names slot %u (got %u)", what, slot, runs[run].task);
    expect(runs[run].task == slot, m);
    snprintf(m, sizeof m, "%s: dispatch carries the task's context", what);
    expect(runs[run].context == FX_CONTEXT + 0x100 * n, m);
    const uint32_t ti = spurs_taskset_taskinfo_ea(FX_TASKSET, runs[run].task < 128 ? runs[run].task : 0);
    snprintf(m, sizeof m, "%s: the dispatched slot's TaskInfo holds the task's arguments", what);
    expect(vm_read32(ti + TI_ARGS) == vm_read32(FX_ARGS + 0x10 * n), m);
}

int main(void)
{
    vm_base = memory; ppu_vm_size = sizeof(memory); s_inst[0].ea = FX_SPURS;

    fresh_taskset();
    expect(create(0) == CELL_OK, "plain CreateTask");
    expect(n_runs == 1 && vm_read32(FX_IDS) == 0, "plain CreateTask takes slot 0 and dispatches once");
    runs_as_created(0, 0, "plain CreateTask");

    /* Two PPU threads: the second CreateTask runs inside the first's window. */
    fresh_taskset();
    interleave = CREATE;
    expect(create(0) == CELL_OK, "first CreateTask");
    expect(n_runs == 2, "each CreateTask dispatches once");
    expect(vm_read32(FX_IDS) == 0 && vm_read32(FX_IDS + 4) == 1, "the two calls take slots 0 and 1");
    runs_as_created(0, 1, "CreateTask inside another's window");
    runs_as_created(1, 0, "CreateTask overtaken by another");

    /* A task exit on an SPU thread starts a task of another taskset. */
    fresh_taskset();
    interleave = EXIT;
    expect(create(2) == CELL_OK, "CreateTask while a task exit starts another task");
    expect(n_runs == 1, "one dispatch");
    runs_as_created(0, 2, "CreateTask overtaken by a task exit");

    printf("cellSpursCreateTask dispatch identity: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='spurs-create-task-dispatch-') as name:
    tmp = Path(name); fixture = tmp/'fixture.c'; fixture.write_text(FIXTURE)
    binary = tmp/'fixture'
    linker = ['-Wl,-dead_strip'] if platform.system() == 'Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
        '-ffunction-sections', '-fdata-sections', '-I'+str(ROOT), '-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'runtime/spu'), str(fixture), str(ROOT/'libs/spurs/spurs_taskset.c'),
        *linker, '-pthread', '-lm', '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_'))}
    subprocess.run([str(binary)], check=True, timeout=10, cwd=tmp, env=env)
