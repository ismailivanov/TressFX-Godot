#!/usr/bin/env python3
"""Maya ASCII (.ma) hair curves -> TressFX .tfx (version 4, no strand UVs).

Reads every `createNode nurbsCurve` block, takes its CVs and resamples each curve to
VERTS points by arc length. Units are kept (set import_scale on the hair node).

usage: ma_to_tfx.py input.ma output.tfx [verts_per_strand=16] [--split-tail-z=<value>]
  --split-tail-z: strands whose tip z is below the value go to <output>_tail.tfx, the rest to
  <output>_head.tfx (a ponytail gets its own TressFXHair node with looser settings).
"""
import re
import struct
import sys


def read_curves(path):
    curves = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    for m in re.finditer(r'setAttr "\.cc" -type "nurbsCurve"\s+(.*?);', text, re.S):
        nums = m.group(1).split()
        degree, spans, form, rational, dim = int(nums[0]), int(nums[1]), int(nums[2]), nums[3], int(nums[4])
        nknots = int(nums[5])
        ncv = int(nums[6 + nknots])
        vals = list(map(float, nums[7 + nknots:7 + nknots + ncv * (dim + (1 if rational == "yes" else 0))]))
        stride = dim + (1 if rational == "yes" else 0)
        curves.append([tuple(vals[i * stride:i * stride + 3]) for i in range(ncv)])
    return curves


def resample(pts, n):
    seg = [((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 for a, b in zip(pts, pts[1:])]
    total = sum(seg)
    out = []
    for k in range(n):
        d = total * k / (n - 1)
        i = 0
        while i < len(seg) - 1 and d > seg[i]:
            d -= seg[i]
            i += 1
        t = 0.0 if seg[i] == 0 else d / seg[i]
        a, b = pts[i], pts[i + 1]
        out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t))
    return out


def write_tfx(dst, curves, verts):
    with open(dst, "wb") as f:
        f.write(struct.pack("<fIIIIIII", 4.0, len(curves), verts, 160, 0, 0, 0, 0))
        f.write(b"\0" * 128)
        for c in curves:
            for k, p in enumerate(resample(c, verts)):
                f.write(struct.pack("<4f", p[0], p[1], p[2], 0.0 if k < 2 else 1.0))
    xs = [p[0] for c in curves for p in c]
    ys = [p[1] for c in curves for p in c]
    zs = [p[2] for c in curves for p in c]
    print(f"{dst}: {len(curves)} strands x {verts} verts; bounds x[{min(xs):.1f} {max(xs):.1f}] "
          f"y[{min(ys):.1f} {max(ys):.1f}] z[{min(zs):.1f} {max(zs):.1f}]")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--"))
    src, dst = args[0], args[1]
    verts = int(args[2]) if len(args) > 2 else 16
    curves = [c for c in read_curves(src) if len(c) >= 2]
    if "split-tail-z" in opts:
        z = float(opts["split-tail-z"])
        tail = [c for c in curves if c[-1][2] < z]
        head = [c for c in curves if c[-1][2] >= z]
        base = dst[:-4] if dst.endswith(".tfx") else dst
        write_tfx(base + "_head.tfx", head, verts)
        write_tfx(base + "_tail.tfx", tail, verts)
    else:
        write_tfx(dst, curves, verts)


if __name__ == "__main__":
    main()
