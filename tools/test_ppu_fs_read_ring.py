#!/usr/bin/env python3
"""PS3_FS_RING (runtime/ppu/ppu_fs.cpp): the file transfers behind a guest buffer.

Builds runtime/ppu/tests/test_fs_read_ring.cpp (which includes ppu_fs.cpp)
against a ppu_recomp.h made from the lifter's own HEADER_PREAMBLE, runs it
with PS3_FS_RING=16 and checks the "[fsring]" lines it prints after each step:

  1. a read: one transfer touches its buffer, now=same, file=same;
  2. the buffer written after the read: now=CHANGED, file=same;
  3. thread 7 seeks and reads between thread 6's seek and read on one fd: a
     RACE line naming thread 6, where its seek left the fd and where its read
     started, and thread 7;
  4. the file written after the read: file=DIFFERS and a "later write" line;
  5. an aio read is remembered too;
  6. a range no transfer touched: none;
  7. after 40 more operations the ring holds the last 16 only;
  8. ppu_fs_ring_write writes the 16 entries and the path table.

Then runs it once with PS3_FS_RING unset: the read works and the report says
nothing was remembered.

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_ppu_fs_read_ring.py [--exe-out PATH] [--build-only]
"""

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_read_ring.cpp")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "."]


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("g++")
            or shutil.which("clang++"))


def _stub_header():
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def _sections(err):
    """stderr split at the fixture's "== name ==" markers -> {name: [fsring lines]}."""
    out, cur = {}, None
    for line in err.splitlines():
        m = re.match(r"== (.+) ==$", line)
        if m:
            cur = m.group(1)
            out[cur] = []
        elif cur is not None and line.startswith("[fsring]"):
            out[cur].append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-out", help="keep the built test program at this path")
    ap.add_argument("--build-only", action="store_true",
                    help="build (for another machine, e.g. a Windows target) and stop")
    args = ap.parse_args()

    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            print("FAIL: no C++ compiler found")
            return 1
        print("skip test_ppu_fs_read_ring: no C++ compiler")
        return 0
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("[PASS] " if ok else "[FAIL] ") + what)
        if not ok:
            fails += 1

    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        exe = args.exe_out or os.path.join(td, "test_fs_read_ring")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        cmd += ["-o", exe, FIXTURE]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        cmd += ["-lpthread"] if os.name != "nt" else []
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0

        env = dict(os.environ, PS3_FS_RING="16")
        r = subprocess.run([exe, td], capture_output=True, text=True, timeout=60, env=env)
        sys.stdout.write(r.stdout)
        check(r.returncode == 0, f"the fixture ran (exit {r.returncode})")
        s = _sections(r.stderr)
        if "end" not in s:
            print(r.stderr[-4000:])
            check(False, "the fixture reached its end")
            return 1

        def entries(sec):
            return [l for l in s.get(sec, []) if l.startswith("[fsring]   #")]

        one = entries("1: a read, then the buffer untouched")
        check(len(one) == 1 and " read thread 6 " in one[0] and "pos=0x10000 want=32768 got=32768" in one[0]
              and "now=same file=same" in one[0],
              "1: one read touches the buffer, at file 0x10000, now=same file=same")
        check("r1 32768" in r.stdout, "1: the read brought 32768 bytes")

        two = entries("2: the buffer written after the read")
        check(len(two) == 1 and "now=CHANGED file=same" in two[0],
              "2: a byte changed after the read shows as now=CHANGED, file=same")

        race = [l for l in s.get("3: thread 7 seeks and reads between thread 6's seek and read", [])
                if "RACE" in l]
        check(len(race) == 1 and "thread 6's lseek left the position at 0x4000" in race[0]
              and "its read starts at 0x19000" in race[0] and "last used by thread 7" in race[0],
              "3: one RACE line: thread 6's seek left 0x4000, its read started at 0x19000 after thread 7")
        check("RBUF3 holds file 0x19000" in r.stdout, "3: the raced read did read from 0x19000")

        four = s.get("4: the file written after the read", [])
        e4 = [l for l in four if l.startswith("[fsring]   #")]
        later = [l for l in four if "later write" in l]
        check(len(e4) == 1 and "now=same file=DIFFERS" in e4[0],
              "4: the file written over the read's range shows as file=DIFFERS")
        check(len(later) == 1 and "thread 8" in later[0] and "pos=0x10040 got=256" in later[0],
              "4: the later write is listed (thread 8, 0x10040, 256 bytes)")
        check(not any("RACE" in l for l in four),
              "4: a seek and write by one thread on its own fd is no race")

        five = entries("5: an aio read")
        check(len(five) == 1 and " aio read thread 9 " in five[0] and "pos=0x2000" in five[0]
              and "now=same file=same" in five[0],
              "5: the aio read is remembered with its offset")

        six = s.get("6: nothing touched it", [])
        check(len(six) == 1 and "0 of the remembered transfers touch it" in six[0],
              "6: a range no transfer touched has no entries")

        seven = s.get("7: the ring wraps", [])
        head = [l for l in seven if "seven:" in l]
        m = re.search(r"ring #(\d+)-#(\d+)", head[0]) if head else None
        check(bool(m) and int(m.group(2)) - int(m.group(1)) + 1 == 16
              and "0 of the remembered transfers touch it" in head[0],
              "7: after 40 more operations the ring holds 16 and the first read has left it")
        seven_b = [l for l in seven if l.startswith("[fsring]   #")]
        check(len(seven_b) == 8, f"7: the last 8 of the 20 small reads are still there ({len(seven_b)})")

        check("ring_write 16, 16 entry lines, 2 header lines" in r.stdout,
              "8: ppu_fs_ring_write writes 16 entries, the format line and 1 path")

        r2 = subprocess.run([exe, "--off", td], capture_output=True, text=True, timeout=60,
                            env={k: v for k, v in os.environ.items() if k != "PS3_FS_RING"})
        s2 = _sections(r2.stderr)
        offl = s2.get("off: report", [])
        check(r2.returncode == 0 and "read 256" in r2.stdout,
              "off: with PS3_FS_RING unset the read works")
        check(len(offl) == 1 and "PS3_FS_RING is not set" in offl[0]
              and not any("[fsring]" in l for l in r2.stderr.splitlines() if "off:" not in l),
              "off: the report says nothing was remembered, and nothing else is printed")

    print("PASS" if not fails else f"FAIL ({fails} failed)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
