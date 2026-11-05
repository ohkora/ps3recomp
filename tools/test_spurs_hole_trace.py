#!/usr/bin/env python3
"""GCM_HOLE_TRACE: the report names the SPU PUTs that covered a parked
command-buffer hole and the jobs whose descriptor or input names it
(runtime/spu/tests/test_spurs_hole_trace.c, which runs real jobs through the
MFC channel path)."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
           'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
           'spurs_policy_blob_weak.c', 'spu_interp.c']


def section(err, n):
    m = re.search(r'=== report %d\n(.*?)(?:=== |\Z)' % n, err, re.S)
    assert m, 'report %d missing:\n%s' % (n, err)
    return m.group(1)


def expect(text, patterns, what):
    for p in patterns:
        assert re.search(p, text, re.M), '%s: no line matches %r in:\n%s' % (what, p, text)


with tempfile.TemporaryDirectory(prefix='spurs-hole-trace-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu),
                    '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spurs_hole_trace.c'),
                    *[str(spu / name) for name in sources],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    base = {k: v for k, v in os.environ.items()
            if not k.startswith(('SPU_', 'SPURS_', 'PS3_SPU_', 'GCM_', 'YDKJ_', 'LBP_', 'GH3_'))}

    off = subprocess.run([str(binary), 'off'], capture_output=True, text=True,
                         timeout=60, cwd=tmp, env=base)
    assert off.returncode == 0, off.stderr[-2000:]
    assert 'off: dma hook null, report hook null' in off.stdout, off.stdout
    assert '[hole' not in off.stderr, off.stderr[-2000:]

    on = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                        cwd=tmp, env=dict(base, GCM_HOLE_TRACE='8'))
    assert on.returncode == 0, on.stdout + on.stderr[-3000:]
    assert 'hole1 after job A: 00041234' in on.stdout, on.stdout
    err = on.stderr
    expect(err, [r'^\[hole-trace\] on: .* at most 8 reports'], 'arming')

    expect(section(err, 1), [
        r'^\[hole\] #1 park io=0x00000100 ea=0x00040100 word=20000100 put=0x00000800 standing 2000 ms',
        r'words park-32\.\.park\+92:.* \[20000100\] CAFEF00D ',
        r'^\[hole\]   1 kept PUT\(s\) cover the park:',
        r'cover ea=0x00040100 \(park\+0\) size=16 old=20000100 new=00041234 CAFEF00D'
        r' job=#1\(0x00010000\) img=5 spu=0',
        r'named job #1 ea=0x00010000 img=5 .* ended t-.*names the park at desc\+0x30, .* 1 PUTs',
        r'its put ea=0x00040100 \(park\+0\)',
        r'^\[hole\]   no job running',
        r'above ea=0x00040300 \(park\+512\) .*job=#2\(0x00010100\)',
    ], 'report 1 (a PUT covered the park, then the park was written back)')

    expect(section(err, 2), [
        r'^\[hole\]   no kept PUT covers the park',
        r'named job #2 ea=0x00010100 img=6 .*names the park at desc\+0x34 \(io\)',
        r'its put ea=0x00040300 \(park\+256\) size=16 old=00000000 new=0004BEEF 11112222',
        r'below ea=0x00040100 \(park-256\)',
        r'above ea=0x00040300 \(park\+256\)',
    ], 'report 2 (the naming job PUT elsewhere)')

    expect(section(err, 3), [
        r'named job #3 ea=0x00010200 .*still running after .*names the park at in\+0x8',
        r'\(no kept PUT from this job\)',
        r'running job #3 ea=0x00010200 ',
    ], 'report 3 (asked for while the naming job runs)')
    expect(section(err, 4), [r'named job #3 .* ended t-', r'no job running'],
           'report 4 (after it ended)')

    capped = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                            cwd=tmp, env=dict(base, GCM_HOLE_TRACE='3'))
    assert capped.returncode == 0, capped.stderr[-2000:]
    assert '[hole] #3 park' in capped.stderr and '[hole] #4 park' not in capped.stderr, \
        capped.stderr[-3000:]

print('PASS: the hole report names the PUTs over a park and the jobs that name it')
