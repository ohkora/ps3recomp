#!/usr/bin/env python3
"""Code after a function's end that nothing references must become entries.

C++ virtual methods on the SPU are reached only through vtables the program
fills in at run time, so no branch, call, immediate or data word in the image
names them. The fixture is a tiny stripped SPU ELF: an entry that calls main
and C, then, between main and C, two methods nothing refers to (one straight
after main's return, one after padding), then a data-like pair that must not
be taken for code. A last function after C, past the final seed, stands for
.rodata linked into the executable segment and must not be searched.

Usage:
    python3 tools/test_find_spu_functions_gaps.py
"""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from find_spu_functions import (collect_uncovered_code_starts,  # noqa: E402
                                detect_functions)
from spu_disasm import spu_decode  # noqa: E402


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


BI_R0 = 0x35000000
NOP, LNOP, STOP0 = 0x40200000, 0x00200000, 0x00000000
MAIN, METHOD_A, METHOD_B, DATA, C, TAIL = 0x10, 0x18, 0x28, 0x34, 0x48, 0x50
TEXT = [
    ri16(0x066, MAIN >> 2, 0),           # 0x00 brsl $r0, main      (entry)
    ri16(0x066, (C - 0x04) >> 2, 0),     # 0x04 brsl $r0, C
    STOP0,                               # 0x08 stop 0x0
    LNOP,                                # 0x0C
    ri16(0x081, 7, 3),                   # 0x10 il $r3, 7           main
    BI_R0,                               # 0x14 bi $r0
    ri16(0x081, 9, 3),                   # 0x18 il $r3, 9           method A
    BI_R0,                               # 0x1C bi $r0
    LNOP,                                # 0x20 padding
    NOP,                                 # 0x24 padding
    ri10(0x1C, 1, 3, 3),                 # 0x28 ai $r3, $r3, 1      method B
    ri16(0x064, (C - 0x2C) >> 2, 0),     # 0x2C br C                (tail call)
    LNOP,                                # 0x30 padding
    0x3F800000,                          # 0x34 1.0f: decodes, but then...
    STOP0,                               # 0x38 ...a zero word: data
    LNOP,                                # 0x3C
    NOP,                                 # 0x40
    LNOP,                                # 0x44
    ri16(0x081, 1, 3),                   # 0x48 il $r3, 1           C
    BI_R0,                               # 0x4C bi $r0
    ri16(0x081, 2, 3),                   # 0x50 il $r3, 2           past the last seed
    BI_R0,                               # 0x54 bi $r0
]
TEXT_OFF = 0x80


def spu_elf():
    """A stripped EM_SPU executable with one R+X segment at LS 0."""
    text = b''.join(struct.pack('>I', w) for w in TEXT)
    buf = bytearray(TEXT_OFF + len(text))
    buf[0:52] = struct.pack('>16sHHIIIIIHHHHHH',
                            b'\x7fELF\x01\x02\x01' + bytes(9), 2, 23, 1, 0, 52,
                            0, 0, 52, 32, 1, 40, 0, 0)
    buf[52:84] = struct.pack('>IIIIIIII', 1, TEXT_OFF, 0, 0, len(text), len(text), 5, 0x80)
    buf[TEXT_OFF:] = text
    return bytes(buf)


def functions(buf=None, raw=False):
    if raw:
        buf = b''.join(struct.pack('>I', w) for w in TEXT)
    funcs, _ = detect_functions(buf or spu_elf(), verbose=False, raw=raw)
    return funcs


def insns(words, base=0):
    return {base + 4 * i: spu_decode(w, base + 4 * i) for i, w in enumerate(words)}


class UncoveredCodeSeeds(unittest.TestCase):
    def test_method_right_after_a_return_is_seeded(self):
        self.assertIn((METHOD_A, METHOD_A + 8), functions())

    def test_method_after_padding_is_seeded(self):
        self.assertIn((METHOD_B, METHOD_B + 8), functions())

    def test_data_that_runs_into_a_stop_is_not_seeded(self):
        self.assertNotIn(DATA, {s for s, _ in functions()})

    def test_past_the_last_seed_is_not_searched(self):
        self.assertNotIn(TAIL, {s for s, _ in functions()})

    def test_existing_functions_keep_their_extents(self):
        funcs = functions()
        self.assertIn((MAIN, MAIN + 8), funcs)
        self.assertIn((C, C + 8), funcs)

    def test_raw_job_images_are_not_searched(self):
        self.assertNotIn(METHOD_A, {s for s, _ in functions(raw=True)})

    def test_zero_word_after_a_return_is_not_seeded(self):
        code = insns([BI_R0, STOP0, ri16(0x081, 1, 3), BI_R0, BI_R0])
        self.assertEqual(collect_uncovered_code_starts([0, 0x10], {}, code), [])

    def test_code_that_runs_into_the_next_seed_is_not_seeded(self):
        code = insns([BI_R0, ri16(0x081, 1, 3), ri16(0x081, 2, 3), BI_R0])
        self.assertEqual(collect_uncovered_code_starts([0, 0xC], {}, code), [])

    def test_undecodable_word_rejects_the_run(self):
        # 0x50000000 is no SPU instruction (the disassembler prints `.word`).
        self.assertEqual(spu_decode(0x50000000, 0).mnemonic, ".word")
        code = insns([BI_R0, ri16(0x081, 1, 3), 0x50000000, BI_R0, BI_R0])
        self.assertEqual(collect_uncovered_code_starts([0, 0x10], {}, code), [])

    def test_back_to_back_methods_are_all_seeded(self):
        code = insns([BI_R0, LNOP, BI_R0, LNOP, ri16(0x081, 1, 3), BI_R0, BI_R0])
        self.assertEqual(collect_uncovered_code_starts([0, 0x18], {}, code), [0x8, 0x10])

    # A method whose own branch lands inside it: 0x04 il; 0x08 brz $r3,0x10;
    # 0x0C ai; 0x10 label: ai; 0x14 bi $r0. Every branch target is a seed, so
    # the walk from 0x04 meets the seed 0x10 before the return.
    FORWARD_LABEL = [BI_R0, ri16(0x081, 1, 3), ri16(0x040, 2, 3), ri10(0x1C, 1, 3, 3),
                     ri10(0x1C, 2, 3, 3), BI_R0, BI_R0]

    def test_method_with_its_own_forward_label_is_seeded(self):
        code = insns(self.FORWARD_LABEL)
        self.assertEqual(collect_uncovered_code_starts([0, 0x10, 0x18], {}, code, {0x10: [0x8]}),
                         [0x4])

    def test_label_also_reached_from_outside_is_not_walked_past(self):
        code = insns(self.FORWARD_LABEL)
        self.assertEqual(collect_uncovered_code_starts([0, 0x10, 0x18], {}, code,
                                                       {0x10: [0x8, 0x18]}), [])

    def test_seed_that_is_called_or_named_is_not_walked_past(self):
        # 0x10 is not in the label map: something calls or names it.
        code = insns(self.FORWARD_LABEL)
        self.assertEqual(collect_uncovered_code_starts([0, 0x10, 0x18], {}, code, {}), [])
        self.assertEqual(collect_uncovered_code_starts([0, 0x10, 0x18], {}, code), [])

    def test_last_seed_is_not_walked_past(self):
        code = insns(self.FORWARD_LABEL)
        self.assertEqual(collect_uncovered_code_starts([0, 0x10], {}, code, {0x10: [0x8]}), [])

    def test_method_with_a_loop_is_seeded(self):
        # 0x04 il; 0x08 loop: ai; 0x0C brnz $r3,0x08; 0x10 bi $r0. The label's
        # branch comes after it, still inside the method.
        code = insns([BI_R0, ri16(0x081, 1, 3), ri10(0x1C, 1, 3, 3), ri16(0x042, -1, 3),
                      BI_R0, BI_R0])
        self.assertEqual(collect_uncovered_code_starts([0, 0x8, 0x14], {}, code, {0x8: [0xC]}),
                         [0x4])

    def test_lifter_emits_entries_for_the_methods(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / 'task.elf'
            elf.write_bytes(spu_elf())
            out = Path(tmp) / 'out'
            run = subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'),
                                  '--auto-functions', str(elf), '-o', str(out)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            header = (out / 'spu_recomp.h').read_text()
            for start in (METHOD_A, METHOD_B):
                self.assertIn('spu_func_%08X(' % start, header)
            self.assertNotIn('spu_func_%08X(' % DATA, header)


# An image whose unreferenced method branches to a label inside itself before
# its return, as 0x23358 does in the title's task image (`brz $r5,0x233A0`
# over a vtable call). detect_functions must work out that 0x24 is only that
# method's label and seed the method; the label stays a seed of its own.
METHOD_L, LABEL, C2 = 0x18, 0x24, 0x30
TEXT_LABEL = [
    ri16(0x066, MAIN >> 2, 0),           # 0x00 brsl $r0, main      (entry)
    ri16(0x066, (C2 - 0x04) >> 2, 0),    # 0x04 brsl $r0, C2
    STOP0,                               # 0x08 stop 0x0
    LNOP,                                # 0x0C
    ri16(0x081, 7, 3),                   # 0x10 il $r3, 7           main
    BI_R0,                               # 0x14 bi $r0
    ri16(0x081, 1, 3),                   # 0x18 il $r3, 1           method
    ri16(0x040, (LABEL - 0x1C) >> 2, 3), # 0x1C brz $r3, label
    ri10(0x1C, 1, 3, 3),                 # 0x20 ai $r3, $r3, 1
    ri10(0x1C, 2, 3, 3),                 # 0x24 ai $r3, $r3, 2      label
    BI_R0,                               # 0x28 bi $r0
    LNOP,                                # 0x2C padding
    ri16(0x081, 1, 3),                   # 0x30 il $r3, 1           C2
    BI_R0,                               # 0x34 bi $r0
]


def label_elf(words=TEXT_LABEL):
    text = b''.join(struct.pack('>I', w) for w in words)
    buf = bytearray(TEXT_OFF + len(text))
    buf[0:52] = struct.pack('>16sHHIIIIIHHHHHH',
                            b'\x7fELF\x01\x02\x01' + bytes(9), 2, 23, 1, 0, 52,
                            0, 0, 52, 32, 1, 40, 0, 0)
    buf[52:84] = struct.pack('>IIIIIIII', 1, TEXT_OFF, 0, 0, len(text), len(text), 5, 0x80)
    buf[TEXT_OFF:] = text
    return bytes(buf)


class LabelsInsideUnreferencedCode(unittest.TestCase):
    def test_method_is_seeded_and_label_stays_a_seed(self):
        funcs, _ = detect_functions(label_elf(), verbose=False)
        self.assertIn((METHOD_L, LABEL), funcs)
        self.assertIn((LABEL, LABEL + 8), funcs)
        self.assertIn((MAIN, MAIN + 8), funcs)
        self.assertIn((C2, C2 + 8), funcs)

    def test_label_a_call_also_reaches_is_not_walked_past(self):
        # The entry calls the label as well (0x08: brsl $r0, label instead of
        # the stop), so it may be a function of its own: the method stays unseeded.
        words = list(TEXT_LABEL)
        words[2] = ri16(0x066, (LABEL - 0x08) >> 2, 0)
        funcs, _ = detect_functions(label_elf(words), verbose=False)
        self.assertNotIn(METHOD_L, {s for s, _ in funcs})

    def test_lifter_emits_an_entry_for_the_method(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / 'task.elf'
            elf.write_bytes(label_elf())
            out = Path(tmp) / 'out'
            run = subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'),
                                  '--auto-functions', str(elf), '-o', str(out)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('spu_func_%08X(' % METHOD_L, (out / 'spu_recomp.h').read_text())


if __name__ == '__main__':
    unittest.main()
