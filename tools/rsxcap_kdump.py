#!/usr/bin/env python3
"""Compare the constants of an RSX_CAP series, draw by draw (RSX_CAP_KDUMP).

The D3D12 backend's frame capture (RSX_CAP=start[:count[:stride]], RSX_CAP_DIR)
writes a frame_NN folder per capture. With RSX_CAP_KDUMP=1 each folder also
gets kdump.bin: every vertex and fragment constant the capture's draws into the
RSX_CAP_KDIFF targets drew with (all VP draws without RSX_CAP_KDIFF, or with
RSX_CAP_KDUMP=all). The layout is CapKdFile / CapKdRec in
libs/video/rsx_d3d12_backend.c.

A draw is matched from one capture to the next by its key, as on the
manifest's "prog" line: the fragment program's code hash with its inline
constants skipped (programs move: a title can copy its patched programs into a
ring every frame), the vertex program's hash, the textures the program samples,
and the occurrence among the capture's draws with those three. --key prog
leaves the textures out, --key op matches by position.

Registers: c0-c511 the vertex constants, x0-x15 what the backend appends to
them (the viewport epilogue), k0... the fragment program's inline constants,
alpha the alpha-test reference vec4. Components .x .y .z .w. list prints each
draw's fps, the structural hash RSX_FP_DEBUG=<fps>:<n>:<reg> takes to show a
register of that program on screen.

Commands:
  list DIR               the captures, their brightness and labels, and the draws
  separate DIR           registers whose values stay apart in lit and dark captures
  steps DIR              registers that jump between captures far more than they drift
  show DIR KEY REG       one register of one draw over the series
  diff DIR A B           what differs between captures A and B, draw by draw

A capture is lit or dark by the mean of its rt_<target>.bmp (the target the
draws write most, or --rt; RSX_CAP_ONLY keeps the capture to it) against
--threshold, or the best split of the means; --lit / --dark take capture
numbers instead ("14-32,40"), or frame counts ("@9060-9330").

Example:
  python tools/rsxcap_kdump.py separate D:/logs/cap --dark 8-13,45-59 --lit 14-32
"""
import argparse
import math
import os
import re
import struct
import sys

FILE_FMT = "<4sIQII"                       # CapKdFile: magic, version, frame_count, capidx, rec_bytes
REC_FMT = "<4s4I2Ii4I5I16I16I16I2I"        # CapKdRec, see read_kdump
VS_GUEST = 512                             # RSX_MAX_VERTEX_CONSTANTS
KIND = {0: "-", 1: "tex", 2: "rt", 3: "ds", 4: "dsc"}
COMP = "xyzw"


class Rec(object):
    __slots__ = ("op", "rt_off", "fp_addr", "fph", "fps", "vs_idx", "vsh", "tk", "occ", "nfp",
                 "vertex_count", "blend_key", "cmask", "alpha_ctl", "zeta_off",
                 "tex_off", "tex_fmt", "tex_kind", "vs", "fp", "key")


class Capture(object):
    def __init__(self, idx, path, frame, recs):
        self.idx, self.path, self.frame, self.recs = idx, path, frame, recs
        self.bright = None
        self.label = None                  # "L", "D" or None


def read_kdump(path, warn=None):
    """(frame_count, capidx, [Rec]) from one kdump.bin."""
    with open(path, "rb") as f:
        data = f.read()
    fs = struct.calcsize(FILE_FMT)
    if len(data) < fs:
        raise ValueError("%s: shorter than its header" % path)
    magic, ver, frame_count, capidx, rec_bytes = struct.unpack_from(FILE_FMT, data, 0)
    if magic != b"KDMP" or ver != 1:
        raise ValueError("%s: not a version 1 kdump (magic %r, version %d)" % (path, magic, ver))
    rs = struct.calcsize(REC_FMT)
    if rec_bytes != rs:
        raise ValueError("%s: records have %d-byte headers, this reader knows %d"
                         % (path, rec_bytes, rs))
    recs, pos = [], fs
    while pos < len(data):
        if pos + rs > len(data):
            break
        v = struct.unpack_from(REC_FMT, data, pos)
        if v[0] != b"KDR1":
            raise ValueError("%s: no record at byte %d" % (path, pos))
        vs_bytes, fp_bytes = v[65], v[66]
        if pos + rs + vs_bytes + fp_bytes > len(data):
            break
        r = Rec()
        r.op, r.rt_off, r.fp_addr, r.fph = v[1:5]
        r.fps = v[5] | (v[6] << 32)
        (r.vs_idx, r.vsh, r.tk, r.occ, r.nfp,
         r.vertex_count, r.blend_key, r.cmask, r.alpha_ctl, r.zeta_off) = v[7:17]
        r.tex_off, r.tex_fmt, r.tex_kind = v[17:33], v[33:49], v[49:65]
        pos += rs
        r.vs = struct.unpack_from("<%df" % (vs_bytes // 4), data, pos)
        pos += vs_bytes
        r.fp = struct.unpack_from("<%df" % (fp_bytes // 4), data, pos)
        pos += fp_bytes
        r.key = None
        recs.append(r)
    if pos < len(data) and warn:
        warn("%s: the last %d bytes are an incomplete record (ignored)" % (path, len(data) - pos))
    return frame_count, capidx, recs


def bmp_mean(path):
    """Mean (r, g, b) of a 24-bit BMP as the capture writes it, or None."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < 54 or data[:2] != b"BM":
        return None
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    if struct.unpack_from("<H", data, 28)[0] != 24 or w <= 0 or h == 0:
        return None
    h = abs(h)
    rowb = (w * 3 + 3) & ~3
    px = data[off:off + rowb * h]
    if len(px) < rowb * h:
        return None
    if rowb == w * 3:
        b, g, r = sum(px[0::3]), sum(px[1::3]), sum(px[2::3])
    else:
        r = g = b = 0
        for y in range(h):
            row = px[y * rowb:y * rowb + w * 3]
            b += sum(row[0::3]); g += sum(row[1::3]); r += sum(row[2::3])
    n = float(w * h)
    return (r / n, g / n, b / n)


def reg_name(r, j):
    """Name of flat value j of a record (vs values, then fp values)."""
    nvs = len(r.vs)
    vec, comp = divmod(j if j < nvs else j - nvs, 4)
    if j < nvs:
        bank = "c%d" % vec if vec < VS_GUEST else "x%d" % (vec - VS_GUEST)
    else:
        bank = "alpha" if vec == len(r.fp) // 4 - 1 else "k%d" % vec
    return "%s.%s" % (bank, COMP[comp])


def reg_index(r, name):
    """Flat index of the first component of a register named like c12, x0, k3, alpha
    (a component suffix .x/.y/.z/.w is allowed and kept)."""
    m = re.fullmatch(r"(c|x|k|alpha)(\d*)(?:\.([xyzw]))?", name.strip())
    if not m:
        raise ValueError("register %r: use c<n>, x<n>, k<n> or alpha" % name)
    bank, num, comp = m.group(1), m.group(2), m.group(3)
    c = COMP.index(comp) if comp else 0
    if bank == "alpha":
        return len(r.vs) + (len(r.fp) // 4 - 1) * 4 + c
    n = int(num or 0)
    if bank == "c":
        return n * 4 + c
    if bank == "x":
        return (VS_GUEST + n) * 4 + c
    return len(r.vs) + n * 4 + c


def fmt_key(k):
    if k[0] == "op":
        return "op%03d" % k[1]
    if k[0] == "prog":
        return "%08X/%08X#%d" % k[1:]
    return "%08X/%08X/%08X#%d" % k[1:]


def assign_keys(caps, mode):
    for c in caps:
        seen = {}
        for r in c.recs:
            if mode == "op":
                r.key = ("op", r.op)
            elif mode == "prog":
                base = (r.fph, r.vsh)
                n = seen.get(base, 0)
                seen[base] = n + 1
                r.key = ("prog", r.fph, r.vsh, n)
            else:
                r.key = ("tex", r.fph, r.vsh, r.tk, r.occ)


def load_series(capdir, args, warn):
    caps = []
    for name in os.listdir(capdir):
        m = re.fullmatch(r"frame_(\d+)", name)
        path = os.path.join(capdir, name)
        if not m or not os.path.isfile(os.path.join(path, "kdump.bin")):
            continue
        frame, _, recs = read_kdump(os.path.join(path, "kdump.bin"), warn)
        caps.append(Capture(int(m.group(1)), path, frame, recs))
    if not caps:
        raise SystemExit("no frame_NN/kdump.bin under %s (run with RSX_CAP_KDUMP=1)" % capdir)
    caps.sort(key=lambda c: c.idx)
    assign_keys(caps, args.key)

    # Brightness: the target the draws write most, unless --rt names one.
    if args.rt is not None:
        rt = int(args.rt, 16)
    else:
        count = {}
        for c in caps:
            for r in c.recs:
                count[r.rt_off] = count.get(r.rt_off, 0) + 1
        rt = max(count, key=count.get)
    bmp = "backbuffer.bmp" if rt == 0 else "rt_%08X.bmp" % rt
    for c in caps:
        m = bmp_mean(os.path.join(c.path, bmp))
        c.bright = None if m is None else sum(m) / 3.0

    # Labels.
    threshold = None
    if args.lit or args.dark:
        lit = parse_spec(args.lit or "", caps)
        dark = parse_spec(args.dark or "", caps)
        both = lit & dark
        if both:
            raise SystemExit("captures %s are in both --lit and --dark" % sorted(both))
        for c in caps:
            c.label = "L" if c.idx in lit else "D" if c.idx in dark else None
    else:
        vals = [c.bright for c in caps if c.bright is not None]
        if vals:
            threshold = args.threshold if args.threshold is not None else best_split(vals)
            for c in caps:
                if c.bright is not None and threshold is not None:
                    c.label = "L" if c.bright > threshold else "D"
    return caps, rt, threshold


def parse_spec(spec, caps):
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        by_frame = part.startswith("@")
        if by_frame:
            part = part[1:]
        a, _, b = part.partition("-")
        lo = int(a, 0)
        hi = int(b, 0) if b else lo
        for c in caps:
            v = c.frame if by_frame else c.idx
            if lo <= v <= hi:
                out.add(c.idx)
    return out


def best_split(vals):
    """The threshold between two groups of brightness values with the largest
    between-group variance (Otsu over the sorted values), or None if they are
    all equal."""
    v = sorted(vals)
    if v[0] == v[-1]:
        return None
    n, total = len(v), sum(v)
    best, at, acc = -1.0, None, 0.0
    for i in range(1, n):
        acc += v[i - 1]
        if v[i] == v[i - 1]:
            continue
        m1, m2 = acc / i, (total - acc) / (n - i)
        score = i * (n - i) * (m1 - m2) ** 2
        if score > best:
            best, at = score, (v[i - 1] + v[i]) / 2.0
    return at


def by_key(caps):
    out = {}
    for c in caps:
        for r in c.recs:
            out.setdefault(r.key, {})[c.idx] = r
    return out


def values(r):
    return r.vs + r.fp


def finite(x):
    return not (math.isnan(x) or math.isinf(x))


def g(x):
    return "%.6g" % x


# --------------------------------------------------------------------------
def cmd_list(caps, rt, threshold, args):
    print("brightness of %s, %s" % ("backbuffer" if rt == 0 else "rt %08X" % rt,
          "labels from --lit/--dark" if (args.lit or args.dark) else
          "threshold %s" % (g(threshold) if threshold is not None else "none (no images)")))
    for c in caps:
        print("frame_%02d frame_count=%d draws=%d bright=%s %s" % (
            c.idx, c.frame, len(c.recs), "-" if c.bright is None else "%.2f" % c.bright,
            c.label or "-"))
    keys = by_key(caps)
    cbi = caps_by_idx(caps)
    print("%d draw keys (--key %s):" % (len(keys), args.key))
    order = sorted(keys, key=lambda k: min(r.op for r in keys[k].values()))
    for k in order:
        m = keys[k]
        r = m[min(m)]
        ops = sorted(set(x.op for x in m.values()))
        vcs = sorted(set(x.vertex_count for x in m.values()))
        tex = " ".join("t%d=%s:%08X" % (u, KIND.get(r.tex_kind[u], "?"), r.tex_off[u])
                       for u in range(16) if r.tex_kind[u])
        nl = sum(1 for i in m if cbi[i].label == "L")
        nd = sum(1 for i in m if cbi[i].label == "D")
        print("  %s in %d captures (lit %d, dark %d) op%03d-%03d rt=%08X fps=%016X nfp=%d verts=%s %s" % (
            fmt_key(k), len(m), nl, nd, ops[0], ops[-1], r.rt_off, r.fps, r.nfp,
            ",".join(str(x) for x in vcs[:4]) + ("..." if len(vcs) > 4 else ""), tex))


def caps_by_idx(caps):
    return dict((c.idx, c) for c in caps)


def cmd_separate(caps, rt, threshold, args):
    keys = by_key(caps)
    cbi = caps_by_idx(caps)
    if not any(c.label for c in caps):
        raise SystemExit("no lit/dark labels: no images to measure, give --lit and --dark")
    print("lit captures %s; dark captures %s" % (
        spec_of([c.idx for c in caps if c.label == "L"]),
        spec_of([c.idx for c in caps if c.label == "D"])))
    found, per_key = [], {}
    for k, m in keys.items():
        lit = [m[i] for i in sorted(m) if cbi[i].label == "L"]
        dark = [m[i] for i in sorted(m) if cbi[i].label == "D"]
        if len(lit) < args.min or len(dark) < args.min:
            continue
        both = [m[i] for i in sorted(m) if cbi[i].label]
        n = min(len(values(r)) for r in both)
        lv_all = [values(r) for r in lit]
        dv_all = [values(r) for r in dark]
        order = [values(r) for r in both]
        for j in range(n):
            lv = [v[j] for v in lv_all]
            dv = [v[j] for v in dv_all]
            first = lv[0]
            if all(x == first for x in lv) and all(x == first for x in dv):
                continue
            lnan = sum(1 for x in lv if not finite(x))
            dnan = sum(1 for x in dv if not finite(x))
            if (lnan == len(lv)) != (dnan == len(dv)) and (lnan == 0 or dnan == 0):
                found.append((float("inf"), k, j, lv, dv, "non-finite on one side only"))
                continue
            lf = [x for x in lv if finite(x)]
            df = [x for x in dv if finite(x)]
            if len(lf) < args.min or len(df) < args.min:
                continue
            lo1, hi1, lo2, hi2 = min(lf), max(lf), min(df), max(df)
            if hi1 < lo2:
                gap = lo2 - hi1
            elif hi2 < lo1:
                gap = lo1 - hi2
            else:
                continue
            scale = max(abs(lo1), abs(hi1), abs(lo2), abs(hi2))
            if gap <= 1e-6 * scale:
                continue
            spread = max(hi1 - lo1, hi2 - lo2)
            seq = [v[j] for v in order if finite(v[j])]
            note = "monotonic over the series" if monotonic(seq) else ""
            found.append((gap / (spread + 1e-6 * scale), k, j, lf, df, note))
        per_key[k] = sum(1 for f in found if f[1] == k)
    if not found:
        print("no register separates lit from dark in any draw seen in at least %d of each" % args.min)
        return
    found.sort(key=lambda f: (-f[0], fmt_key(f[1]), f[2]))
    print("%d register components separate lit from dark (score = gap / larger spread):" % len(found))
    for score, k, j, lf, df, note in found[:args.top]:
        r = next(iter(keys[k].values()))
        if note.startswith("non-finite"):
            print("  %s %-9s %s" % (fmt_key(k), reg_name(r, j), note))
            continue
        print("  %s %-9s lit %s..%s (%d)  dark %s..%s (%d)  score %s%s" % (
            fmt_key(k), reg_name(r, j), g(min(lf)), g(max(lf)), len(lf),
            g(min(df)), g(max(df)), len(df), "inf" if math.isinf(score) else "%.3g" % score,
            "  [" + note + "]" if note else ""))
    if len(found) > args.top:
        print("  ... %d more (--top)" % (len(found) - args.top))
    print("per draw:")
    for k in sorted(per_key, key=lambda k: -per_key[k]):
        if per_key[k]:
            print("  %s %d" % (fmt_key(k), per_key[k]))
    if all(c.label != "D" or c.idx > max(x.idx for x in caps if x.label == "L") for c in caps) or \
       all(c.label != "D" or c.idx < min(x.idx for x in caps if x.label == "L") for c in caps):
        print("note: every dark capture is on one side of the lit ones, so anything that drifts "
              "with time separates too; label dark captures from both sides if there are any")


def monotonic(seq):
    if len(seq) < 3:
        return False
    up = all(b >= a for a, b in zip(seq, seq[1:]))
    down = all(b <= a for a, b in zip(seq, seq[1:]))
    return up or down


def spec_of(idxs):
    idxs = sorted(idxs)
    out, i = [], 0
    while i < len(idxs):
        j = i
        while j + 1 < len(idxs) and idxs[j + 1] == idxs[j] + 1:
            j += 1
        out.append(str(idxs[i]) if i == j else "%d-%d" % (idxs[i], idxs[j]))
        i = j + 1
    return ",".join(out) or "none"


def pick_keys(keys, caps, spec):
    """Keys named by spec: a key as printed (or a unique prefix of one), or
    opNNN@C, the draw op NNN of capture C."""
    if spec is None:
        return list(keys)
    m = re.fullmatch(r"op(\d+)@(\d+)", spec)
    if m:
        op, ci = int(m.group(1)), int(m.group(2))
        for c in caps:
            if c.idx == ci:
                for r in c.recs:
                    if r.op == op:
                        return [r.key]
        raise SystemExit("capture %d has no dumped draw op%03d" % (ci, op))
    hits = [k for k in keys if fmt_key(k).upper().startswith(spec.upper())]
    if not hits:
        raise SystemExit("no draw key starts with %s (see list)" % spec)
    return hits


def cmd_steps(caps, rt, threshold, args):
    keys = by_key(caps)
    found = []
    for k in pick_keys(keys, caps, args.draw):
        m = keys[k]
        idxs = sorted(m)
        if len(idxs) < 3:
            continue
        vals = [values(m[i]) for i in idxs]
        n = min(len(v) for v in vals)
        for j in range(n):
            seq = [v[j] for v in vals]
            if all(x == seq[0] for x in seq):
                continue
            fin = [abs(x) for x in seq if finite(x)]
            scale = max(fin) if fin else 0.0
            d = []
            for a, b in zip(seq, seq[1:]):
                if finite(a) and finite(b):
                    d.append(abs(b - a))
                elif finite(a) != finite(b):
                    d.append(float("inf"))
                else:
                    d.append(0.0)
            big = max(d)
            if big <= 1e-6 * scale:
                continue
            med = sorted(d)[len(d) // 2]
            ratio = big / (med + 1e-6 * scale) if not math.isinf(big) else float("inf")
            if ratio < args.ratio:
                continue
            at = [idxs[t + 1] for t in range(len(d)) if d[t] >= big / 2.0]
            if args.at is not None and args.at not in at:
                continue
            t = d.index(big)
            found.append((ratio, k, j, idxs[t], idxs[t + 1], seq[t], seq[t + 1], med, at))
    if not found:
        print("no register jumps by more than %g times its usual step" % args.ratio)
        return
    found.sort(key=lambda f: (-f[0], fmt_key(f[1]), f[2]))
    cbi = caps_by_idx(caps)
    print("%d register components jump (ratio = largest step / median step):" % len(found))
    for ratio, k, j, a, b, va, vb, med, at in found[:args.top]:
        r = next(iter(keys[k].values()))
        lab = "".join((cbi[i].label or "?") for i in (a, b))
        print("  %s %-9s %s -> %s at frame_%02d->%02d (%s)  median step %s  ratio %s  big steps into %s" % (
            fmt_key(k), reg_name(r, j), g(va), g(vb), a, b, lab, g(med),
            "inf" if math.isinf(ratio) else "%.3g" % ratio,
            spec_of(at) if len(at) <= 12 else spec_of(at[:12]) + ",..."))
    if len(found) > args.top:
        print("  ... %d more (--top)" % (len(found) - args.top))


def cmd_show(caps, rt, threshold, args):
    keys = by_key(caps)
    ks = pick_keys(keys, caps, args.draw)
    if len(ks) != 1:
        raise SystemExit("%s names %d draws; give more of the key" % (args.draw, len(ks)))
    m = keys[ks[0]]
    any_r = m[min(m)]
    j0 = reg_index(any_r, args.reg)
    one = "." in args.reg
    print("%s %s" % (fmt_key(ks[0]), args.reg))
    for c in caps:
        r = m.get(c.idx)
        if r is None:
            print("  frame_%02d frame_count=%d %s  (no such draw)" % (c.idx, c.frame, c.label or "-"))
            continue
        v = values(r)
        js = [j0] if one else range(j0 - j0 % 4, j0 - j0 % 4 + 4)
        if max(js) >= len(v):
            raise SystemExit("%s: this draw has no %s" % (fmt_key(ks[0]), args.reg))
        print("  frame_%02d frame_count=%d %s bright=%s op%03d  %s" % (
            c.idx, c.frame, c.label or "-", "-" if c.bright is None else "%.2f" % c.bright,
            r.op, " ".join(g(v[j]) for j in js)))


def cmd_diff(caps, rt, threshold, args):
    keys = by_key(caps)
    sel = set(pick_keys(keys, caps, args.draw))
    for k in sorted(keys, key=lambda k: min(r.op for r in keys[k].values())):
        if k not in sel:
            continue
        m = keys[k]
        if args.a not in m or args.b not in m:
            continue
        va, vb = values(m[args.a]), values(m[args.b])
        rows = []
        for j in range(min(len(va), len(vb))):
            x, y = va[j], vb[j]
            if x == y or (not finite(x) and not finite(y) and str(x) == str(y)):
                continue
            if finite(x) and finite(y):
                rel = abs(y - x) / max(abs(x), abs(y))
            else:
                rel = float("inf")
            rows.append((rel, j, x, y))
        if not rows:
            print("%s op%03d/op%03d: no differences" % (fmt_key(k), m[args.a].op, m[args.b].op))
            continue
        rows.sort(key=lambda t: (-t[0], t[1]))
        print("%s op%03d/op%03d: %d components differ, largest relative change first:" % (
            fmt_key(k), m[args.a].op, m[args.b].op, len(rows)))
        for rel, j, x, y in rows[:args.top]:
            print("  %-9s %s -> %s" % (reg_name(m[args.a], j), g(x), g(y)))
    only_a = [k for k in sel if args.a in keys[k] and args.b not in keys[k]]
    only_b = [k for k in sel if args.b in keys[k] and args.a not in keys[k]]
    for k in only_a:
        print("%s only in frame_%02d" % (fmt_key(k), args.a))
    for k in only_b:
        print("%s only in frame_%02d" % (fmt_key(k), args.b))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd")

    def common(sp):
        sp.add_argument("dir", help="the RSX_CAP_DIR of the series")
        sp.add_argument("--key", choices=("tex", "prog", "op"), default="tex",
                        help="how a draw is matched across captures (default tex)")
        sp.add_argument("--rt", help="hex colour target whose image gives the brightness")
        sp.add_argument("--threshold", type=float, help="brightness above which a capture is lit")
        sp.add_argument("--lit", help="lit captures, e.g. 14-32 or @9060-9330")
        sp.add_argument("--dark", help="dark captures, e.g. 8-13,45-59")

    sp = sub.add_parser("list"); common(sp)
    sp = sub.add_parser("separate"); common(sp)
    sp.add_argument("--min", type=int, default=2, help="lit and dark captures a draw needs (2)")
    sp.add_argument("--top", type=int, default=40)
    sp = sub.add_parser("steps"); common(sp)
    sp.add_argument("--draw", help="only this draw key (or prefix, or opNNN@C)")
    sp.add_argument("--ratio", type=float, default=8.0, help="largest step / median step (8)")
    sp.add_argument("--at", type=int, help="only jumps into this capture")
    sp.add_argument("--top", type=int, default=40)
    sp = sub.add_parser("show"); common(sp)
    sp.add_argument("draw", help="draw key (or prefix, or opNNN@C)")
    sp.add_argument("reg", help="c<n>, x<n>, k<n> or alpha, optionally .x/.y/.z/.w")
    sp = sub.add_parser("diff"); common(sp)
    sp.add_argument("a", type=int, help="capture number")
    sp.add_argument("b", type=int, help="capture number")
    sp.add_argument("--draw", help="only this draw key (or prefix, or opNNN@C)")
    sp.add_argument("--top", type=int, default=12, help="components per draw (12)")
    args = p.parse_args(argv)
    if not args.cmd:
        p.print_help()
        return 2

    def warn(msg):
        print("warning: " + msg, file=sys.stderr)

    caps, rt, threshold = load_series(args.dir, args, warn)
    {"list": cmd_list, "separate": cmd_separate, "steps": cmd_steps,
     "show": cmd_show, "diff": cmd_diff}[args.cmd](caps, rt, threshold, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
