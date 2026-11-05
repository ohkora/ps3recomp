#!/usr/bin/env python3
"""The interpreter takes a pending SPU interrupt before the next instruction.

Builds runtime/spu/tests/test_spu_interpreter_takes_interrupts.c against the
real SPU runtime and runs each of its modes in a process of its own: a DMA list
parked on a stall-and-notify element, a one-instruction interrupts-enabled
window (`bie` onto `bid`), and a handler that resumes the list. The handler
must run in that window, whether the interpreter was entered directly or
through the dispatcher, and whether the handler is interpreted or lifted; with
interrupts disabled, or no vector at LS 0, nothing is taken and the program
still ends.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_', 'PS3_'))}

# Each mode: whether the interrupt is taken, and a line its stderr must carry.
MODES = {
    'interp':        (True,  '[spu-int] TAKEN img=78 events=0x2&0x2 vector->0x00600 (srr0=0x04044)'),
    'driver':        (True,  '[spu-int] TAKEN img=78 events=0x2&0x2 vector->0x00600 (srr0=0x04044)'),
    'driver-lifted': (True,  '[spu-int] TAKEN img=77 events=0x2&0x2 vector->0x00600 (srr0=0x04044)'),
    'disabled':      (False, '[mfc-list] STALL img=78 tag=0 elem@0x01570 left=1'),
    'novector':      (False, "LS0 word 0x00000000 is no branch -- not taken"),
}

with tempfile.TemporaryDirectory(prefix='spu-interp-takes-irq-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(spu),
                    '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_interpreter_takes_interrupts.c'),
                    *[str(spu / name) for name in sources],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    failed = []
    for mode, (taken, line) in MODES.items():
        r = subprocess.run([str(binary), mode], capture_output=True, text=True,
                           timeout=10, cwd=tmp, env=env)
        ok = r.returncode == 0 and f'PASS {mode}' in r.stdout and line in r.stderr
        if taken and '[mfc-list] RESUME img=' not in r.stderr:
            ok = False
        if not taken and '[spu-int] TAKEN' in r.stderr:
            ok = False
        if not ok:
            failed.append(mode)
            sys.stderr.write(f'--- {mode}: exit {r.returncode}\n{r.stdout}{r.stderr[-3000:]}\n')
        else:
            print(f'PASS: {mode}')
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: interpreted code takes the stall-and-notify interrupt in a '
          'one-instruction window, and leaves it alone when disabled or unvectored')
