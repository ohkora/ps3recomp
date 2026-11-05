#!/usr/bin/env python3
"""tools/rsxcap_kdump.py reads what the D3D12 capture writes, and finds the
registers that tell lit captures from dark ones.

1. Layout: CapKdFile and CapKdRec are cut out of libs/video/rsx_d3d12_backend.c
   (which only builds on Windows) and compiled into a writer; the reader must
   get back every field and value it wrote, and stop cleanly at a record cut
   short by a capture that did not finish.
2. Analysis: a synthetic 40-capture series where one register of one draw is
   1 in the lit captures and 0 in the dark ones (with two lit captures inside
   a dark stretch), one draw's register turns non-finite in the dark ones, and
   the rest is what a scene does anyway: camera constants drifting every
   capture, a register that steps once inside the lit stretch, noise, two
   draws of one program, a draw seen only in lit captures. separate must name
   exactly the two planted registers; steps must find the one-off step at its
   capture and leave the drift alone.

Run: python tools/test_rsxcap_kdump.py   (part 1 needs a C compiler: CC, cc, clang or gcc)
"""
import contextlib
import io
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rsxcap_kdump as K  # noqa: E402

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t u64;
%s
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;
    CapKdFile h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "KDMP", 4); h.version = 1; h.frame_count = 9150; h.capidx = 20;
    h.rec_bytes = (u32)sizeof(CapKdRec);
    fwrite(&h, sizeof h, 1, f);
    for (int n = 0; n < 2; n++) {
        CapKdRec r; memset(&r, 0, sizeof r);
        memcpy(r.magic, "KDR1", 4);
        r.op = 34 + n; r.rt_off = 0x01080000; r.fp_addr = 0x00118201; r.fph = 0xA1B2C3D4;
        r.fps_lo = 0x89ABCDEF; r.fps_hi = 0x01234567u + n;
        r.vs_idx = n - 1; r.vsh = 0x11223344; r.tk = 0x55667788; r.occ = n; r.nfp = 3;
        r.vertex_count = 1234; r.blend_key = 0xBEEF; r.cmask = 0xF; r.alpha_ctl = 0x10705;
        r.zeta_off = 0x01BC0000;
        for (int u = 0; u < 16; u++) {
            r.tex_off[u] = 0x100 * u + n; r.tex_fmt[u] = 0x80 + u; r.tex_kind[u] = u %% 5;
        }
        r.vs_bytes = 8448; r.fp_bytes = 4 * 16;
        fwrite(&r, sizeof r, 1, f);
        static float vs[8448 / 4];
        for (int i = 0; i < 8448 / 4; i++) vs[i] = i * 0.5f + n;
        fwrite(vs, sizeof vs, 1, f);
        float fp[16];
        for (int i = 0; i < 16; i++) fp[i] = -(float)i - n;
        fwrite(fp, sizeof fp, 1, f);
    }
    fclose(f);
    printf("%%u %%u\n", (unsigned)sizeof(CapKdFile), (unsigned)sizeof(CapKdRec));
    return 0;
}
'''


def layout_source():
    src = open(os.path.join(ROOT, "libs/video/rsx_d3d12_backend.c"), newline="").read()
    src = src.replace("\r\n", "\n")
    out = []
    for name in ("CapKdFile", "CapKdRec"):
        m = re.search(r"typedef struct \{[^{}]*\} %s;" % name, src)
        assert m, "%s not found in libs/video/rsx_d3d12_backend.c" % name
        out.append(m.group(0))
    return "\n".join(out)


def compiler():
    return shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")


def test_layout(td):
    cc = compiler()
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip layout part: no C compiler")
        return
    c = os.path.join(td, "w.c")
    exe = os.path.join(td, "w.exe" if os.name == "nt" else "w")
    with open(c, "w") as f:
        f.write(HARNESS % layout_source())
    r = subprocess.run([cc, "-std=c11", "-O0", "-w", "-o", exe, c], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    path = os.path.join(td, "kdump.bin")
    sizes = subprocess.run([exe, path], capture_output=True, text=True, check=True).stdout.split()
    assert int(sizes[0]) == struct.calcsize(K.FILE_FMT), "CapKdFile is %s bytes, reader %d" % (
        sizes[0], struct.calcsize(K.FILE_FMT))
    assert int(sizes[1]) == struct.calcsize(K.REC_FMT), "CapKdRec is %s bytes, reader %d" % (
        sizes[1], struct.calcsize(K.REC_FMT))

    frame, capidx, recs = K.read_kdump(path)
    assert (frame, capidx, len(recs)) == (9150, 20, 2), (frame, capidx, len(recs))
    for n, r in enumerate(recs):
        got = (r.op, r.rt_off, r.fp_addr, r.fph, r.fps, r.vs_idx, r.vsh, r.tk, r.occ, r.nfp,
               r.vertex_count, r.blend_key, r.cmask, r.alpha_ctl, r.zeta_off)
        want = (34 + n, 0x01080000, 0x00118201, 0xA1B2C3D4, (0x01234567 + n) << 32 | 0x89ABCDEF,
                n - 1, 0x11223344, 0x55667788, n, 3, 1234, 0xBEEF, 0xF, 0x10705, 0x01BC0000)
        assert got == want, (got, want)
        assert list(r.tex_off) == [0x100 * u + n for u in range(16)]
        assert list(r.tex_fmt) == [0x80 + u for u in range(16)]
        assert list(r.tex_kind) == [u % 5 for u in range(16)]
        assert len(r.vs) == 8448 // 4 and len(r.fp) == 16
        assert r.vs[0] == n and r.vs[2111] == 2111 * 0.5 + n
        assert r.fp[15] == -15.0 - n
        assert K.reg_name(r, 400) == "c100.x" and K.reg_name(r, 2048) == "x0.x"
        assert K.reg_name(r, 2112 + 4 + 1) == "k1.y" and K.reg_name(r, 2112 + 12 + 3) == "alpha.w"
        for name in ("c100.x", "x0.x", "k1.y", "alpha.w"):
            assert K.reg_name(r, K.reg_index(r, name)) == name

    # A capture cut short in its last record: the complete ones are kept.
    data = open(path, "rb").read()
    with open(path, "wb") as f:
        f.write(data[:-100])
    warned = []
    frame, capidx, recs = K.read_kdump(path, warned.append)
    assert len(recs) == 1 and recs[0].op == 34 and warned, (len(recs), warned)
    print("layout: the reader gets back every field the C structs wrote")


# --------------------------------------------------------------------------
LIT = set(range(10, 26)) | {30, 32}
RT = 0x01080000


def write_bmp(path, level, w=5, h=2):
    rowb = (w * 3 + 3) & ~3
    row = bytes([level, level, level] * w) + b"\0" * (rowb - w * 3)
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<IIIIiiHH", hdr, 2, 54 + rowb * h, 0, 54, 40, w, h, 1, 24)
    struct.pack_into("<I", hdr, 34, rowb * h)
    with open(path, "wb") as f:
        f.write(bytes(hdr) + row * h)


def rec_bytes(op, fph, vsh, tk, occ, vs, fp, nfp, rt=RT):
    tex_off = [0] * 16
    tex_fmt = [0] * 16
    tex_kind = [0] * 16
    tex_kind[0], tex_off[0], tex_fmt[0] = 1, 0x0A000000 + op, 0x86
    tex_kind[5], tex_off[5] = 2, 0x01800000
    head = struct.pack(K.REC_FMT, b"KDR1", op, rt, 0x00100000 + 0x380 * op, fph,
                       0x5000 + op, 0xF00D, -1, vsh, tk, occ,
                       nfp, 300 + op, 0, 0xF, 0, 0x01400000,
                       *(tex_off + tex_fmt + tex_kind + [len(vs) * 4, len(fp) * 4]))
    return head + struct.pack("<%df" % len(vs), *vs) + struct.pack("<%df" % len(fp), *fp)


def make_series(td):
    rng = random.Random(1)
    cap = os.path.join(td, "cap")
    os.mkdir(cap)
    for i in range(40):
        d = os.path.join(cap, "frame_%02d" % i)
        os.mkdir(d)
        lit = i in LIT
        write_bmp(os.path.join(d, "rt_%08X.bmp" % RT), 150 if lit else 8)
        recs = []

        def bank(drift=True):
            vs = [0.0] * (528 * 4)
            if drift:                                   # camera: moves every capture
                vs[0] = i * 0.1
                vs[5] = 2.0 - i * 0.03
            return vs

        nfp = 4
        fp = [0.0] * ((nfp + 1) * 4)
        # A: the planted separator c100.x, plus noise in k2.y and a fixed alpha.
        vs = bank()
        vs[400] = 1.0 if lit else 0.0
        fpa = list(fp)
        fpa[2 * 4 + 1] = rng.random()
        fpa[nfp * 4] = 0.5
        recs.append(rec_bytes(0, 0xAAAA0001, 0xA001, 0xA002, 0, vs, fpa, nfp))
        # B: steps once, at capture 20, inside the lit stretch.
        vs = bank()
        vs[200 * 4 + 2] = 5.0 if i < 20 else -5.0
        recs.append(rec_bytes(1, 0xBBBB0001, 0xB001, 0xB002, 0, vs, fp, nfp))
        # C: two draws of one program, told apart by occurrence.
        for occ in (0, 1):
            vs = bank()
            vs[50 * 4 + 3] = 0.25 + 0.5 * occ
            recs.append(rec_bytes(2 + occ, 0xCCCC0001, 0xC001, 0xC002, occ, vs, fp, nfp))
        # E: non-finite in the dark captures.
        vs = bank(False)
        vs[10 * 4] = 0.5 if lit else float("nan")
        recs.append(rec_bytes(4, 0xEEEE0001, 0xE001, 0xE002, 0, vs, fp, nfp))
        # A shadow map pass: more draws than the scene's, into a depth surface with no image.
        for occ in range(8):
            vs = bank()
            recs.append(rec_bytes(6 + occ, 0x5AD00001, 0x5A01, 0x5A02, occ, vs, fp, nfp, rt=0x01BC0000))
        # D: only drawn in lit captures.
        if lit:
            vs = bank()
            vs[300 * 4] = rng.random()
            recs.append(rec_bytes(5, 0xDDDD0001, 0xD001, 0xD002, 0, vs, fp, nfp))
        with open(os.path.join(d, "kdump.bin"), "wb") as f:
            f.write(struct.pack(K.FILE_FMT, b"KDMP", 1, 8850 + 15 * i, i, struct.calcsize(K.REC_FMT)))
            f.write(b"".join(recs))
    return cap


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = K.main(list(argv))
    assert rc == 0, rc
    return out.getvalue()


def test_analysis(td):
    cap = make_series(td)

    out = run("list", cap)
    m = re.search(r"threshold (\S+)", out)
    assert m and 8 < float(m.group(1)) < 150, out
    assert "14 draw keys" in out and "brightness of rt 01080000" in out, out
    assert re.search(r"DDDD0001/0000D001/0000D002#0 in 18 captures \(lit 18, dark 0\)", out), out
    assert re.search(r"CCCC0001/0000C001/0000C002#1 in 40 captures", out), out
    assert "t0=tex:0A000000 t5=rt:01800000" in out, out
    assert "fps=0000F00D00005000 " in out, out

    out = run("separate", cap)
    rows = [l.split() for l in out.splitlines() if l.startswith("  ") and "." in l.split()[1]
            and not l.startswith("  ...")]
    named = [(r[0].split("/")[0], r[1]) for r in rows]
    assert named == [("EEEE0001", "c10.x"), ("AAAA0001", "c100.x")], out
    assert "lit 1..1 (18)  dark 0..0 (22)" in out, out
    assert "non-finite on one side only" in out, out
    assert "note:" not in out, out

    # Explicit labels give the same answer; dark only after the lit stretch
    # lets the drift through, and the note says so.
    out2 = run("separate", cap, "--lit", "10-25,30,32", "--dark", "0-9,26-29,31,33-39")
    assert [l for l in out2.splitlines() if "c100.x" in l] == \
        [l for l in out.splitlines() if "c100.x" in l], out2
    out3 = run("separate", cap, "--lit", "10-25", "--dark", "33-39")
    assert "c0.x" in out3 and "note:" in out3 and "monotonic" in out3, out3
    out4 = run("separate", cap, "--lit", "@9000-9225", "--dark", "@8850-8985")
    assert "c100.x" in out4, out4

    out = run("steps", cap)
    step = [l for l in out.splitlines() if "BBBB0001" in l and "c200.z" in l]
    assert len(step) == 1 and "5 -> -5 at frame_19->20 (LL)" in step[0] \
        and "median step 0 " in step[0] and step[0].endswith("into 20"), out
    a = [l for l in out.splitlines() if "AAAA0001" in l and "c100.x" in l]
    assert len(a) == 1 and "into 10,26,30-33" in a[0], out
    assert not [l for l in out.splitlines() if " c0.x" in l or " c1.y" in l], out
    out = run("steps", cap, "--at", "20")
    assert "c200.z" in out and "c100.x" not in out, out
    out = run("steps", cap, "--draw", "op1@3")
    assert "c200.z" in out and "AAAA" not in out, out

    out = run("show", cap, "AAAA", "c100.x")
    lines = [l for l in out.splitlines() if l.startswith("  frame_")]
    assert len(lines) == 40 and lines[10].endswith(" 1") and lines[9].endswith(" 0"), out
    out = run("show", cap, "AAAA", "alpha")
    assert out.splitlines()[1].endswith("0.5 0 0 0"), out
    out = run("show", cap, "DDDD", "c300.x")
    assert "(no such draw)" in out, out

    out = run("diff", cap, "15", "35", "--draw", "AAAA")
    assert re.search(r"c100\.x\s+1 -> 0", out) and "c0.x" in out, out
    out = run("diff", cap, "15", "35")
    assert "DDDD0001/0000D001/0000D002#0 only in frame_15" in out, out
    assert "CCCC0001/0000C001/0000C002#1 op003/op003" in out, out

    # --key prog keeps two draws of one program apart by order alone.
    out = run("list", cap, "--key", "prog")
    assert "CCCC0001/0000C001#1 in 40 captures" in out, out
    out = run("list", cap, "--key", "op")
    assert re.search(r"op005 in \d+ captures", out), out
    print("analysis: separate and steps find the planted registers and nothing else")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as td:
        test_layout(td)
        test_analysis(td)
    print("PASS: rsxcap_kdump reads the capture's constant dump and finds the separating registers")
