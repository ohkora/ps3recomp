#!/usr/bin/env python3
"""A returned cellFsWrite is in the file (runtime/ppu/ppu_fs.cpp).

ppu_fs.cpp keeps each guest fd as its own stdio stream on its own host
descriptor. Bytes that fwrite leaves in a stream's buffer are invisible to
another fd on the same file, and lost if the process is killed before the
next seek or close. FIOS builds its HDD cache this way: one fd fills the data
file while others read it back. This builds
runtime/ppu/tests/test_fs_write_visible.cpp (which includes ppu_fs.cpp)
against a ppu_recomp.h made from the lifter's own HEADER_PREAMBLE and runs it
twice:

  1. two fds on one file: what one writes, the other reads, tail included;
  2. --write-and-exit: a 1000-byte write, then _Exit with no close; this
     script then checks that the host file holds the 1000 bytes.

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_ppu_fs_write_visible.py [--exe-out PATH] [--build-only]
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_write_visible.cpp")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "."]
DATA = "ps3recomp_fs_write_visible.dat"


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("g++")
            or shutil.which("clang++"))


def _stub_header():
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


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
        print("skip test_ppu_fs_write_visible: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        exe = args.exe_out or os.path.join(td, "test_fs_write_visible")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        cmd += ["-o", exe, FIXTURE]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0

        r = subprocess.run([exe, td], capture_output=True, text=True, timeout=60)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        fails = 1 if r.returncode else 0

        r = subprocess.run([exe, "--write-and-exit", td], capture_output=True, text=True,
                           timeout=60)
        path = os.path.join(td, DATA)
        data = open(path, "rb").read() if os.path.exists(path) else b""
        ok = r.returncode == 0 and len(data) == 1000 and data == b"\x33" * 1000
        print(("[PASS] " if ok else "[FAIL] ") +
              f"a process killed right after a 1000-byte cellFsWrite leaves "
              f"{len(data)} bytes in the file (want 1000; exit {r.returncode})")
        if not ok:
            fails += 1
        print("PASS" if not fails else "FAIL")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
