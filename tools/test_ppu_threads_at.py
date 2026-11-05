#!/usr/bin/env python3
"""PS3_THREADS_AT reports every guest thread, at each time asked for.

Builds the real runtime/ppu/ppu_loader.cpp with
runtime/ppu/tests/test_ppu_threads_at.cpp, which registers three guest
threads: one inside an HLE import, one inside an lv2 syscall, one in neither,
the first two with a guest stack two callers deep. With PS3_THREADS_AT=0.1,0.3
there must be two reports, each naming every thread, the call it is inside
and the callers on its stack; without it, none.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import stub_header  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

EXPECT = [
    '[THREADS] --- t=0.1s: 3 guest thread(s) ---',
    '[THREADS] --- t=0.3s: 3 guest thread(s) ---',
    '[THREADS] guest-tid 1   inside HLE sys_lwmutex_lock  lr=0x00030024 r3=0x00001234 sp=0x00F00000',
    '[THREADS] guest-tid 7   inside syscall 130  lr=0x00020044 r3=0x00000005 sp=0x00E00000',
    '[THREADS] guest-tid 9   no HLE import or syscall in flight  lr=0x00020100',
    '[GSTACK:t=0.1s tid=1] chain: func_00030000+0x20 func_00020000+0x10',
    '[GSTACK:t=0.3s tid=7] chain: func_00030000+0x50 func_00020000+0x40',
]


PEEK = ';'.join([
    '[t1.r30+8]:0x20',        # follow tid 1's r30 to the second object
    't1.r30+0x4-4:12',        # register arithmetic, decimal length
    '[[0x00C00000+8]+4]:8',   # two pointers from a "global"
    't7.ctr:4',               # ctr
    '0x00FFFFF8:0x10',        # runs off the end of guest memory
    '0x00C00000:0x1000',      # capped at 0x200 bytes
    't42.r3:4',               # errors, each on its own line
    't1.r40:4',
    '[t1.r30:4',
    '0x100',
    't1.lr:0',
    '[0xFFFFFFFF+8]:4',
])

PEEK_EXPECT = [
    '[PEEK] dumped after each report: ' + PEEK,
    '[PEEK] t=0.1s [t1.r30+8] = 0x00C00100 (32 bytes)',
    '[PEEK]   0x00C00100: AAAA0001 00C00000 AAAA0003 AAAA0004 AAAA0005 AAAA0006 AAAA0007 AAAA0008',
    '[PEEK] t=0.1s t1.r30+0x4-4 = 0x00C00000 (12 bytes)',
    '[PEEK]   0x00C00000: 11111111 22222222 00C00100',
    '[PEEK] t=0.1s [[0x00C00000+8]+4] = 0x00C00000 (8 bytes)',
    '[PEEK] t=0.3s t7.ctr = 0x00C00100 (4 bytes)',
    '[PEEK]   0x00C00100: AAAA0001',
    '[PEEK]   0x00FFFFF8: 00000000 00000000 (outside guest memory)',
    '[PEEK] t=0.1s 0x00C00000 = 0x00C00000 (512 bytes)',
    '[PEEK]   0x00C001E0:',
    '[PEEK] t=0.1s t42.r3: no guest thread with that id',
    '[PEEK] t=0.1s t1.r40: registers are r0-r31',
    '[PEEK] t=0.1s [t1.r30: missing ]',
    '[PEEK] t=0.1s 0x100: expected <expr>:<len>',
    '[PEEK] t=0.1s t1.lr:0: bad length',
    '[PEEK] t=0.1s [0xFFFFFFFF+8]: a pointer it reads is outside guest memory',
]


def run(binary, threads_at, peek=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_', 'BCTRL_'))}
    if threads_at is not None:
        env['PS3_THREADS_AT'] = threads_at
    if peek is not None:
        env['PS3_PEEK'] = peek
    r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
    if r.returncode or 'fixture done' not in r.stdout:
        sys.stderr.write(r.stderr[-2000:])
        raise SystemExit('FAIL: fixture exited with %d' % r.returncode)
    return r.stderr


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX threads and mmap')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-threads-at-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_threads_at.cpp'),
                        *flags, '-o', str(binary)], check=True)

        err = run(binary, '0.1,0.3')
        missing = [e for e in EXPECT if e not in err]
        if missing:
            sys.stderr.write(err[-3000:])
            raise SystemExit('FAIL: report lines missing:\n  ' + '\n  '.join(missing))
        if err.count('[THREADS] --- ') != 2:
            sys.stderr.write(err[-3000:])
            raise SystemExit('FAIL: expected exactly two reports')
        print('PASS: two reports, each thread with its call and callers')

        err = run(binary, None)
        if '[THREADS]' in err:
            sys.stderr.write(err[-2000:])
            raise SystemExit('FAIL: a report without PS3_THREADS_AT')
        print('PASS: no report unless asked for')

        if '[PEEK]' in run(binary, '0.1,0.3'):
            raise SystemExit('FAIL: a [PEEK] line without PS3_PEEK')
        err = run(binary, '0.1,0.3', PEEK)
        missing = [e for e in PEEK_EXPECT if e not in err]
        if missing:
            sys.stderr.write('\n'.join(l for l in err.splitlines() if l.startswith('[PEEK]'))[-4000:] + '\n')
            raise SystemExit('FAIL: [PEEK] lines missing:\n  ' + '\n  '.join(missing))
        if err.count('[PEEK] t=0.1s [t1.r30+8] = ') != 1 or err.count('[PEEK] t=0.3s [t1.r30+8] = ') != 1:
            raise SystemExit('FAIL: expected one dump per item at each report time')
        if '[PEEK]   0x00C00200:' in err or 'VM-OOB' in err:
            raise SystemExit('FAIL: a dump past its 0x200-byte cap, or a guest-memory bounds report')
        if err.index('[THREADS] --- t=0.3s') > err.index('[PEEK] t=0.3s [t1.r30+8]'):
            raise SystemExit('FAIL: the dumps come before their thread report')
        print('PASS: PS3_PEEK dumps registers, pointers and globals after each report, '
              'and names each bad item')


if __name__ == '__main__':
    main()
