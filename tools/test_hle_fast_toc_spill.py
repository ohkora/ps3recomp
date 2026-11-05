#!/usr/bin/env python3
"""ps3_hle_call fills the caller's TOC slot on the fast path too.

Lifted callers reload r2 from 0x28(r1) after every import call. The fast path
for leaf imports (ps3_hle_register_fast) used to return before writing that
slot, so a frame whose first import was a fast sys_lwmutex_lock reloaded r2
from a slot nothing had written. PlayStation All-Stars Battle Royale
(BCUS98472) got r2 = 0 that way and exited during start-up.

This builds runtime/ppu/tests/test_hle_fast_toc_spill.cpp against the real
runtime/ppu/ppu_hle.cpp and runs it. It needs a host C++ compiler (under CI=1
a missing one fails instead of skipping).

Usage:
    python3 tools/test_hle_fast_toc_spill.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_ppu_scaffold import stub_header

ROOT = Path(__file__).resolve().parents[1]


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
            or shutil.which("g++"))


def main():
    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            print("FAIL: no C++ compiler found")
            return 1
        print("skip test_hle_fast_toc_spill: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory(prefix="hle-fast-toc-") as tmp:
        tmp = Path(tmp)
        (tmp / "ppu_recomp.h").write_text(stub_header())
        exe = tmp / "test_hle_fast_toc_spill"
        cmd = [cxx, "-std=c++17", "-O1", "-w",
               "-I" + str(tmp), "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "runtime" / "platform"),
               str(ROOT / "runtime" / "ppu" / "ppu_hle.cpp"),
               str(ROOT / "runtime" / "ppu" / "tests" / "test_hle_fast_toc_spill.cpp"),
               "-o", str(exe)]
        if os.name != "nt":
            cmd += ["-pthread"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("PPU_", "PS3_", "PS3RECOMP_", "SPU_", "FLOW_", "HLE_"))}
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
        sys.stdout.write(r.stdout)
        if r.returncode < 0:
            print("FAIL: fixture crashed (signal %d)" % -r.returncode)
            return 1
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
