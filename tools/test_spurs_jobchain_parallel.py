#!/usr/bin/env python3
"""SPURS_JC_WORKERS: a job chain runs up to that many jobs at once, capped by
its maxContention and its instance's SPU count, and SYNC / LWSYNC still wait
for the jobs before them (tests/test_spurs_jobchain_parallel.c runs the real
walker and runners)."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-jobchain-par-') as tmp:
    out = Path(tmp) / 'test'
    units = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload',
             'spurs_policy', 'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap',
             'spurs_policy_blob_weak', 'spu_interp']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu'),
                    '-I' + str(root / 'runtime/platform'),
                    str(root / 'tests/test_spurs_jobchain_parallel.c'),
                    *[str(root / 'runtime/spu' / f'{u}.c') for u in units],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(out)],
                   check=True)
    result = subprocess.run([str(out)], timeout=60, capture_output=True, text=True)
    print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else '')
    if result.returncode:
        print(result.stderr[-3000:])
        raise SystemExit('test_spurs_jobchain_parallel failed')
