#!/usr/bin/env python3
"""The D3D12 guest-FP debug switches act on the HLSL the decompiler writes now.

FP_ONE, FP_SHOW, FP_IDCOLOR, FP_TEX and FP_KILL (libs/video/rsx_d3d12_backend.c,
vp_get_fp_pso) rewrite a program's HLSL before it is compiled. They looked for
"_po.c0 = " and "PSOut _po;", which the decompiler no longer writes, so on the
buffered (guest-FP) path every one of them did nothing, silently. They also
named a program only by its address, which a title that copies its programs
into a ring every frame changes every frame.

rsx_d3d12_backend.c only builds on Windows, so the test cuts fp_dbg_named,
fp_dbg_set_output and fp_dbg_discard out of the source, compiles them with the
real decompiler (libs/video/rsx_fp_decompiler.c) and checks, on its output:
- one colour target: the override lands on "_o" ahead of the NaN-guarded return;
- after rsx_fp_apply_alpha_test_buffered: on "_rsx_out", after the discard test;
- several targets, and a depth export: on "_out.t0";
- FP_KILL: a discard is the first statement of main;
- an HLSL text of no known shape is left alone (returns 0);
- a program is named by its address or its ucode hash, nothing else.

Run: python tools/test_d3d12_fp_debug_switches.py   (needs a C compiler: CC, cc, clang or gcc)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
%s
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define SWZ_IDENT ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
static char hlsl[65536];
static void show(const char* tag) { printf("=== %%s\n%%s\n=== end\n", tag, hlsl); }
static int build(int mrt, u32 ctrl)
{
    u8 prog[32];
    /* MOV r0, COL0 (and MOV r2, COL0: the second fp32 colour export) */
    put_word(prog + 0, (0x01u << 24) | (0xFu << 9) | (mrt ? 0u : 1u));
    put_word(prog + 4, 1u | SWZ_IDENT | EXEC_ALWAYS);
    put_word(prog + 8, 0); put_word(prog + 12, 0);
    put_word(prog + 16, (0x01u << 24) | (0xFu << 9) | (2u << 1) | 1u);
    put_word(prog + 20, 1u | SWZ_IDENT | EXEC_ALWAYS);
    put_word(prog + 24, 0); put_word(prog + 28, 0);
    u32 nk = 0;
    return rsx_fp_decompile_buffered_ex(prog, mrt ? 32 : 16, ctrl, 0, hlsl, sizeof hlsl, &nk);
}
int main(void)
{
    build(0, 0x40u);
    printf("set1 %%d\n", fp_dbg_set_output(hlsl, sizeof hlsl, "float4(1,0,0,1)")); show("one");
    build(0, 0x40u);
    printf("alpha %%d\n", rsx_fp_apply_alpha_test_buffered(hlsl, sizeof hlsl, 0x0204u));
    printf("set2 %%d\n", fp_dbg_set_output(hlsl, sizeof hlsl, "float4(0,1,0,1)")); show("alpha");
    build(1, 0x40u);
    printf("set3 %%d\n", fp_dbg_set_output(hlsl, sizeof hlsl, "float4(0,0,1,1)")); show("mrt");
    build(0, 0x40u | 0x2u);
    printf("set4 %%d\n", fp_dbg_set_output(hlsl, sizeof hlsl, "float4(1,1,0,1)")); show("depth");
    build(0, 0x40u);
    printf("kill %%d\n", fp_dbg_discard(hlsl, sizeof hlsl)); show("kill");
    strcpy(hlsl, "float4 main() { return 1; }\n");
    printf("odd %%d\n", fp_dbg_set_output(hlsl, sizeof hlsl, "x")); show("odd");
    setenv("FP_KILL", "1A2B3C4D", 1);
    printf("named %%d %%d %%d\n", fp_dbg_named("FP_KILL", 0x00118201u, 0x1A2B3C4Du),
           fp_dbg_named("FP_KILL", 0x1A2B3C4Du, 0x0u), fp_dbg_named("FP_KILL", 0x00118201u, 0x0u));
    setenv("FP_KILL", "0x00118201", 1);
    printf("named2 %%d %%d\n", fp_dbg_named("FP_KILL", 0x00118201u, 0x1A2B3C4Du),
           fp_dbg_named("FP_ONE", 0x00118201u, 0x1A2B3C4Du));
    return 0;
}
'''


def cut(src, sig):
    """The function that starts with sig, to its closing brace (braces inside
    string and character literals do not count)."""
    m = re.search(re.escape(sig) + r"[^{]*\{", src)
    assert m, "%s not found in libs/video/rsx_d3d12_backend.c" % sig
    i, depth, quote = m.end(), 1, None
    while depth:
        ch = src[i]
        if quote:
            if ch == "\\":
                i += 1
            elif ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        else:
            depth += {"{": 1, "}": -1}.get(ch, 0)
        i += 1
    return src[m.start():i]


def compiler():
    return shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")


def run_harness():
    cc = compiler()
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_d3d12_fp_debug_switches: no C compiler")
        raise SystemExit(0)
    src = open(os.path.join(ROOT, "libs/video/rsx_d3d12_backend.c"), newline="").read()
    src = src.replace("\r\n", "\n")
    funcs = "\n".join(cut(src, s) for s in (
        "static int fp_dbg_named(", "static int fp_dbg_set_output(", "static int fp_dbg_discard("))
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        exe = os.path.join(td, "t.exe" if os.name == "nt" else "t")
        with open(c, "w") as f:
            f.write(HARNESS % funcs)
        r = subprocess.run([cc, "-std=gnu11", "-O0", "-w", "-I" + os.path.join(ROOT, "include"),
                            "-I" + os.path.join(ROOT, "libs/video"), "-o", exe, c,
                            os.path.join(ROOT, "libs/video/rsx_fp_decompiler.c")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return subprocess.run([exe], capture_output=True, text=True, check=True).stdout


def section(out, tag):
    m = re.search(r"=== %s\n(.*?)\n=== end\n" % tag, out, re.S)
    assert m, tag
    return m.group(1)


def test_switches():
    out = run_harness()
    for k in ("set1", "set2", "set3", "set4", "kill"):
        assert re.search(r"^%s 1$" % k, out, re.M), "%s did not apply:\n%s" % (k, out)
    assert re.search(r"^alpha 1$", out, re.M), out
    assert re.search(r"^odd 0$", out, re.M), out

    one = section(out, "one")
    assert "    _o = float4(1,0,0,1);\n    return (_o == _o) ? _o : (float4)0;\n}" in one, one
    alpha = section(out, "alpha")
    assert re.search(r"if \(!\(.*\)\) discard;\n    _rsx_out = float4\(0,1,0,1\);\n"
                     r"    return _rsx_out;\n\}", alpha), alpha
    mrt = section(out, "mrt")
    assert "PSOutput main(PSInput input)" in mrt, mrt
    assert "    _out.t0 = float4(0,0,1,1);\n    return _out;\n}" in mrt, mrt
    depth = section(out, "depth")
    assert "SV_Depth" in depth and "    _out.t0 = float4(1,1,0,1);\n    return _out;\n}" in depth, depth
    kill = section(out, "kill")
    assert re.search(r"main\(PSInput input\)[^{\n]*\{\n    discard;\n", kill), kill
    assert section(out, "odd").strip() == "float4 main() { return 1; }", out

    assert re.search(r"^named 1 1 0$", out, re.M), out       # the hash, or an address equal to it
    assert re.search(r"^named2 1 0$", out, re.M), out        # the address; FP_ONE unset


if __name__ == "__main__":
    test_switches()
    print("PASS: the FP debug switches rewrite every output shape of the buffered decompile")
