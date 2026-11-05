#!/usr/bin/env python3
"""Functions named only by a static constructor list must become entries.

crt0 calls a C++ SPU program's global constructors through a register, reading
their addresses from .ctors; no instruction branches to them. The fixture is a
tiny SPU ELF whose constructor at 0x18 is referenced by nothing but the list
{-1, 0x18, 0} in its data segment. Before collect_ctor_list_targets, 0x18 was
a gap between functions: lifted code had no entry for it, and the runtime
ended the task at the constructor call.

Usage:
    python3 tools/test_find_spu_functions_ctors.py
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from find_spu_functions import detect_functions  # noqa: E402


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


BI_R0 = rr(0x1A8, 0, 0, 0)
TEXT = [
    ri16(0x066, 0x10 >> 2, 0),   # 0x00 brsl $r0, 0x10   (entry calls main)
    0x00000000,                  # 0x04 stop
    0x40200000,                  # 0x08 nop
    0x00200000,                  # 0x0C lnop
    ri16(0x081, 7, 3),           # 0x10 il $r3, 7        main
    BI_R0,                       # 0x14 bi $r0
    ri16(0x081, 9, 3),           # 0x18 il $r3, 9        the constructor
    BI_R0,                       # 0x1C bi $r0
]
CTOR = 0x18
DATA_VA = 0x200
TEXT_OFF, DATA_OFF, STR_OFF, SH_OFF = 0x100, 0x180, 0x1C0, 0x200


def words(ws):
    return b''.join(struct.pack('>I', w) for w in ws)


def spu_elf(data_words, named=True):
    """An EM_SPU executable: R+X text at LS 0, R+W data at DATA_VA. With
    `named`, section headers name the data `.ctors`; without, it is stripped."""
    text, data = words(TEXT), words(data_words)
    shstr = b'\0.text\0.ctors\0.shstrtab\0'
    shnum = 4 if named else 0
    buf = bytearray(SH_OFF + 40 * shnum)
    buf[0:52] = struct.pack('>16sHHIIIIIHHHHHH',
                            b'\x7fELF\x01\x02\x01' + bytes(9), 2, 23, 1, 0, 52,
                            SH_OFF if named else 0, 0, 52, 32, 2, 40, shnum,
                            3 if named else 0)
    buf[52:84] = struct.pack('>IIIIIIII', 1, TEXT_OFF, 0, 0, len(text), len(text), 5, 0x80)
    buf[84:116] = struct.pack('>IIIIIIII', 1, DATA_OFF, DATA_VA, DATA_VA,
                              len(data), len(data), 6, 0x10)
    buf[TEXT_OFF:TEXT_OFF + len(text)] = text
    buf[DATA_OFF:DATA_OFF + len(data)] = data
    if named:
        buf[STR_OFF:STR_OFF + len(shstr)] = shstr
        sections = [
            (0, 0, 0, 0, 0, 0),
            (1, 1, 6, 0, TEXT_OFF, len(text)),             # .text  AX
            (7, 1, 3, DATA_VA, DATA_OFF, len(data)),       # .ctors WA
            (14, 3, 0, 0, STR_OFF, len(shstr)),            # .shstrtab
        ]
        for i, (name, typ, flags, addr, off, size) in enumerate(sections):
            buf[SH_OFF + 40 * i:SH_OFF + 40 * (i + 1)] = struct.pack(
                '>IIIIIIIIII', name, typ, flags, addr, off, size, 0, 0, 4, 0)
    return bytes(buf)


def starts(buf):
    funcs, _ = detect_functions(buf, verbose=False)
    return {s for s, _ in funcs}


class CtorListSeeds(unittest.TestCase):
    def test_named_ctors_section_seeds_the_constructor(self):
        self.assertIn(CTOR, starts(spu_elf([0xFFFFFFFF, CTOR, 0])))

    def test_stripped_image_list_shape_seeds_the_constructor(self):
        self.assertIn(CTOR, starts(spu_elf([0xFFFFFFFF, CTOR, 0], named=False)))

    def test_constructor_ends_at_its_return(self):
        funcs, _ = detect_functions(spu_elf([0xFFFFFFFF, CTOR, 0]), verbose=False)
        self.assertIn((CTOR, CTOR + 8), funcs)

    def test_unterminated_run_is_not_a_list(self):
        self.assertNotIn(CTOR, starts(spu_elf([0xFFFFFFFF, CTOR, 7], named=False)))

    def test_mid_function_address_is_not_a_constructor(self):
        # 0x14 follows `il`, which falls through: data, not a function start.
        self.assertNotIn(0x14, starts(spu_elf([0xFFFFFFFF, 0x14, 0], named=False)))

    def test_named_image_without_a_list_is_not_searched(self):
        # Names present but no .ctors entry for 0x18: the shape scan stays off.
        buf = bytearray(spu_elf([0xFFFFFFFF, CTOR, 0]))
        buf[STR_OFF + 7:STR_OFF + 13] = b'.dataX'                   # rename .ctors
        self.assertNotIn(CTOR, starts(bytes(buf)))

    def test_lifter_emits_an_entry_for_the_constructor(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / 'task.elf'
            elf.write_bytes(spu_elf([0xFFFFFFFF, CTOR, 0], named=False))
            out = Path(tmp) / 'out'
            run = subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'),
                                  '--auto-functions', str(elf), '-o', str(out)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            header = (out / 'spu_recomp.h').read_text()
            self.assertIn('spu_func_%08X(' % CTOR, header)


if __name__ == '__main__':
    unittest.main()
