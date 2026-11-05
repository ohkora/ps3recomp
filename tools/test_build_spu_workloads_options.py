#!/usr/bin/env python3
"""build_spu_workloads.py: --merge-chunks and --extra-funcs work on their own.

The "extra entries" message sat in the --merge-chunks branch and indexed
extra_funcs[img], so --merge-chunks crashed with a KeyError on any image
without --extra-funcs, and --extra-funcs alone never said what it added.

Usage:
    python3 tools/test_build_spu_workloads_options.py
"""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
IMAGE = 'task'

TEXT = [
    (0x081 << 23) | (7 << 7) | 3,   # 0x00 il $r3, 7
    0x1A8 << 21,                    # 0x04 bi $r0
    (0x081 << 23) | (9 << 7) | 3,   # 0x08 il $r3, 9   (reached only indirectly)
    0x1A8 << 21,                    # 0x0C bi $r0
]


def spu_elf():
    """An EM_SPU executable with one R+X segment at LS 0 and no sections."""
    text = b''.join(struct.pack('>I', w) for w in TEXT)
    hdr = struct.pack('>16sHHIIIIIHHHHHH', b'\x7fELF\x01\x02\x01' + bytes(9),
                      2, 23, 1, 0, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    ph = struct.pack('>IIIIIIII', 1, 0x80, 0, 0, len(text), len(text), 5, 0x80)
    return (hdr + ph).ljust(0x80, b'\0') + text


class BuildOptions(unittest.TestCase):
    def build(self, *extra):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / 'images').mkdir()
        (root / 'images' / (IMAGE + '.elf')).write_bytes(spu_elf())
        run = subprocess.run([sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                              '--images', str(root / 'images'),
                              '--lifted', str(root / 'lifted'),
                              '--out', str(root / 'spu_workloads.c'), *extra],
                             capture_output=True, text=True)
        return run, root

    def test_merge_chunks_without_extra_funcs(self):
        run, root = self.build('--merge-chunks')
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue((root / 'spu_workloads.c').exists())

    def test_extra_funcs_without_merge_chunks_is_reported_and_lifted(self):
        run, root = self.build('--extra-funcs', IMAGE + '=0x8')
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('%s: extra entries 0x8' % IMAGE, run.stdout)
        header = (root / 'lifted' / IMAGE / 'spu_recomp.h').read_text()
        self.assertIn('spu_func_00000008(', header)


if __name__ == '__main__':
    unittest.main()
