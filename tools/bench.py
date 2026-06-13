#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import shutil
import statistics
import subprocess
import time
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "bench"


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def timed(cmd, **kw):
    t0 = time.perf_counter_ns()
    run(cmd, **kw)
    return time.perf_counter_ns() - t0


def size(path):
    return Path(path).stat().st_size


def compiler_exists(name):
    return shutil.which(name) is not None


def real_gcc():
    for name in ("gcc-15", "gcc-14", "gcc-13", "gcc-12"):
        if compiler_exists(name):
            return name
    return None


def compiler_rows():
    rows = []
    if compiler_exists("clang"):
        rows.append(("clang", "clang"))
    gcc = real_gcc()
    if gcc:
        rows.append((gcc, gcc))
    return rows


def build_compile_table():
    rows = []
    for compiler, label in compiler_rows():
        for opt in ("-Os", "-O2", "-O3"):
            obj = BENCH / f"uzstd-{label}-{opt[1:]}.o"
            cmd = [compiler, "-std=c99", opt, "-DUZSTD_IMPLEMENTATION", "-x", "c", "-c", "uzstd.h", "-o", str(obj)]
            try:
                ns = timed(cmd)
                rows.append({
                    "target": "uzstd.h",
                    "compiler": label,
                    "config": opt,
                    "compile_ms": ns / 1_000_000,
                    "object_bytes": size(obj),
                })
            except Exception as exc:
                rows.append({
                    "target": "uzstd.h",
                    "compiler": compiler,
                    "config": opt,
                    "compile_ms": None,
                    "object_bytes": None,
                    "error": str(exc),
                })
        for target, define in (
            ("uzstd compressor only", "-DUZSTD_NO_DECOMPRESSOR"),
            ("uzstd decompressor only", "-DUZSTD_NO_COMPRESSOR"),
        ):
            suffix = target.replace(" ", "-")
            obj = BENCH / f"{suffix}-{label}-Os.o"
            cmd = [compiler, "-std=c99", "-Os", "-DUZSTD_IMPLEMENTATION", define, "-x", "c", "-c", "uzstd.h", "-o", str(obj)]
            try:
                ns = timed(cmd)
                rows.append({
                    "target": target,
                    "compiler": label,
                    "config": "-Os",
                    "compile_ms": ns / 1_000_000,
                    "object_bytes": size(obj),
                })
            except Exception as exc:
                rows.append({
                    "target": target,
                    "compiler": compiler,
                    "config": "-Os",
                    "compile_ms": None,
                    "object_bytes": None,
                    "error": str(exc),
                })
    if compiler_exists("tcc"):
        obj = BENCH / "uzstd-tcc-Os.o"
        cmd = ["tcc", "-std=c99", "-Os", "-DUZSTD_IMPLEMENTATION", "-x", "c", "-c", "uzstd.h", "-o", str(obj)]
        try:
            ns = timed(cmd)
            rows.append({
                "target": "uzstd.h",
                "compiler": "tcc",
                "config": "-Os",
                "compile_ms": ns / 1_000_000,
                "object_bytes": size(obj),
            })
        except Exception as exc:
            rows.append({
                "target": "uzstd.h",
                "compiler": "tcc",
                "config": "-Os",
                "compile_ms": None,
                "object_bytes": None,
                "error": str(exc),
            })
    return rows


def zstd_core_unity():
    version = "1.5.7"
    tar_path = ROOT / ".bench" / f"zstd-{version}.tar.gz"
    src_dir = ROOT / ".bench" / f"zstd-{version}"
    tar_path.parent.mkdir(exist_ok=True)
    if not tar_path.exists():
        urllib.request.urlretrieve(
            f"https://github.com/facebook/zstd/releases/download/v{version}/zstd-{version}.tar.gz",
            tar_path,
        )
    if not src_dir.exists():
        with tarfile.open(tar_path) as tf:
            tf.extractall(tar_path.parent)
    files = []
    for sub in ("common", "compress", "decompress"):
        for p in sorted((src_dir / "lib" / sub).glob("*.c")):
            if p.name in ("zstdmt_compress.c", "zstd_ddict.c"):
                continue
            files.append(p)
    unity = ROOT / ".bench" / "zstd_core_unity.c"
    unity.write_text(
        "#define ZSTD_DISABLE_ASM 1\n#define ZSTD_MULTITHREAD 0\n"
        + "".join(f'#include "{p.resolve()}"\n' for p in files)
    )
    includes = [
        "-I" + str((src_dir / "lib").resolve()),
        "-I" + str((src_dir / "lib" / "common").resolve()),
        "-I" + str((src_dir / "lib" / "compress").resolve()),
        "-I" + str((src_dir / "lib" / "decompress").resolve()),
    ]
    loc = sum(1 for p in files for _ in p.open())
    return unity, includes, loc


def build_zstd_unity_table():
    rows = []
    try:
        unity, includes, _ = zstd_core_unity()
    except Exception as exc:
        return [{
            "target": "zstd core v1.5.7 unity",
            "compiler": "cc",
            "config": "no dict/legacy/deprecated",
            "compile_ms": None,
            "object_bytes": None,
            "error": str(exc),
        }]
    for compiler, label in compiler_rows():
        for opt in ("-O3",):
            obj = BENCH / f"zstd-core-{label}-{opt[1:]}.o"
            cmd = [compiler, opt, "-DNDEBUG", *includes, "-c", str(unity), "-o", str(obj)]
            try:
                ns = timed(cmd)
                rows.append({
                    "target": "zstd core v1.5.7 unity",
                    "compiler": label,
                    "config": opt + " no-dict",
                    "compile_ms": ns / 1_000_000,
                    "object_bytes": size(obj),
                })
            except Exception as exc:
                rows.append({
                    "target": "zstd core v1.5.7 unity",
                    "compiler": compiler,
                    "config": opt + " no-dict",
                    "compile_ms": None,
                    "object_bytes": None,
                    "error": str(exc),
                })
    return rows


def compress_with_uzstd(exe, src, level, out):
    run([str(exe), str(src), str(level)])
    shutil.copyfile("/tmp/uzstd_file.zst", out)


def decompress_with_uzstd(exe, src, out):
    run([str(exe), "-d", str(src)])
    shutil.copyfile("/tmp/uzstd_out.bin", out)


def throughput_rows(corpora, iterations, small_warmups):
    exe = BENCH / "uzstd_bench"
    run(["cc", "-std=c99", "-O3", "uzstd.c", "-o", str(exe)])
    zstd = shutil.which("zstd")
    rows = []
    uzstd_levels = [{"label": str(level), "order": level, "args": [str(level)]} for level in range(1, 10)]
    zstd_levels = [
        {"label": "-5", "order": -5, "args": ["--fast=5"]},
        {"label": "1", "order": 1, "args": ["-1"]},
        {"label": "3", "order": 3, "args": ["-3"]},
        {"label": "default", "order": 3.5, "args": []},
        {"label": "6", "order": 6, "args": ["-6"]},
        {"label": "9", "order": 9, "args": ["-9"]},
        {"label": "12", "order": 12, "args": ["-12"]},
        {"label": "15", "order": 15, "args": ["-15"]},
        {"label": "19", "order": 19, "args": ["-19"]},
    ]
    for file, label in corpora:
        data_bytes = size(file)
        for codec, levels in (("uzstd", uzstd_levels), ("zstd", zstd_levels)):
            if codec == "zstd" and not zstd:
                continue
            for spec in levels:
                level = spec["label"]
                out = BENCH / f"{label}.{codec}.{level}.zst".replace("=", "_").replace("/", "_")
                if label == "uzstd.h" and ((codec == "uzstd" and level == "1") or (codec == "zstd" and level == "-5")):
                    for _ in range(small_warmups):
                        if codec == "uzstd":
                            compress_with_uzstd(exe, file, level, out)
                        else:
                            run([zstd, "-q", "-f", "--single-thread", *spec["args"], str(file), "-o", str(out)])
                samples = []
                for _ in range(iterations):
                    t0 = time.perf_counter_ns()
                    if codec == "uzstd":
                        compress_with_uzstd(exe, file, level, out)
                    else:
                        run([zstd, "-q", "-f", "--single-thread", *spec["args"], str(file), "-o", str(out)])
                    samples.append(time.perf_counter_ns() - t0)
                c_ns = statistics.median(samples)
                dec_out = BENCH / f"{file.name}.{codec}.{level}.out"
                samples = []
                for _ in range(iterations):
                    t0 = time.perf_counter_ns()
                    if codec == "uzstd":
                        decompress_with_uzstd(exe, out, dec_out)
                    else:
                        run([zstd, "-q", "-d", "-f", str(out), "-o", str(dec_out)])
                    samples.append(time.perf_counter_ns() - t0)
                d_ns = statistics.median(samples)
                if hashlib.sha256(file.read_bytes()).digest() != hashlib.sha256(dec_out.read_bytes()).digest():
                    raise RuntimeError(f"roundtrip mismatch: {file} {codec} {level}")
                rows.append({
                    "file": label,
                    "label": label,
                    "codec": codec,
                    "level": level,
                    "level_order": spec["order"],
                    "input_bytes": data_bytes,
                    "compressed_bytes": size(out),
                    "compress_bytes_per_ns": data_bytes / c_ns,
                    "decompress_bytes_per_ns": data_bytes / d_ns,
                })
    return rows


def loc_rows():
    uzstd_lines = sum(1 for _ in (ROOT / "uzstd.h").open())
    try:
        _, _, zstd_lines = zstd_core_unity()
    except Exception:
        zstd_lines = "unavailable"
    return [
        {"component": "uzstd.h", "lines": uzstd_lines, "note": "single-header codec"},
        {"component": "zstd core v1.5.7", "lines": zstd_lines, "note": "common+compress+decompress, no dictBuilder/legacy/deprecated/mt"},
    ]


def md_table(headers, rows):
    out = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    out.extend("| " + " | ".join(str(c) for c in row) + " |" for row in rows)
    return "\n".join(out)


def fmt_ms(v):
    return "n/a" if v is None else f"{v:.1f}"


def human_size(n):
    units = ("B", "KiB", "MiB", "GiB")
    v = float(n)
    for unit in units:
        if v < 1024 or unit == units[-1]:
            return f"{v:.1f} {unit}" if unit != "B" else f"{int(v)} B"
        v /= 1024


def write_svg(file_name, points, title):
    path = BENCH / file_name
    width, height = 1100, 560
    left, right, top, bottom = 190, 70, 54, 80
    plot_w, plot_h = width - left - right, height - top - bottom
    if not points:
        path.write_text(f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}'><text x='20' y='40'>{title}: no data</text></svg>\n")
        return
    xs = [p["compressed_bytes"] / 1024 for p in points]
    ys = [max(p["compress_bytes_per_ns"], 1e-9) for p in points]
    xmin, xmax = min(xs), max(xs)
    if xmin == xmax:
        xmax = xmin + 1
    xpad = max((xmax - xmin) * 0.10, 1.0)
    xmin = max(0, xmin - xpad)
    xmax = xmax + xpad
    ymin, ymax = min(ys), max(ys)
    ymin = 10 ** math.floor(math.log10(max(ymin * 0.75, 1e-9)))
    ymax = 10 ** math.ceil(math.log10(ymax * 1.35))
    lymin, lymax = math.log10(ymin), math.log10(ymax)
    colors = {"uzstd": "#f08a4b", "zstd": "#5dbb72"}
    markers = {"uzstd": "circle", "zstd": "square"}
    names = {"uzstd": "uzstd", "zstd": "zstd 1.5.7"}
    def sx(x):
        return left + (x - xmin) * plot_w / (xmax - xmin)
    def sy(y):
        return top + (lymax - math.log10(max(y, 1e-9))) * plot_h / (lymax - lymin)
    def fmt_x(v):
        return f"{v:.0f}" if xmax >= 100 else f"{v:.1f}"
    def fmt_y(v):
        if v >= 1:
            return f"{v:.2f} bytes/ns"
        return f"{v:.3f} bytes/ns" if v >= 0.01 else f"{v:.4f} bytes/ns"
    major_y = []
    exp = math.floor(math.log10(ymin))
    while 10 ** exp <= ymax * 1.0001:
        major_y.append(10 ** exp)
        exp += 1
    minor_y = []
    for e in range(math.floor(math.log10(ymin)), math.ceil(math.log10(ymax))):
        for m in range(2, 10):
            v = m * (10 ** e)
            if ymin < v < ymax:
                minor_y.append(v)
    x_step = 10 ** math.floor(math.log10(max((xmax - xmin) / 7, 1e-9)))
    if (xmax - xmin) / x_step > 12:
        x_step *= 2
    if (xmax - xmin) / x_step > 12:
        x_step *= 2.5
    first_x = math.ceil(xmin / x_step) * x_step
    x_ticks = []
    x = first_x
    while x <= xmax + 1e-9:
        x_ticks.append(x)
        x += x_step
    groups = {}
    for p in points:
        groups.setdefault(p["codec"], []).append(p)
    for rows in groups.values():
        rows.sort(key=lambda p: p.get("level_order", p["level"]))
    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' viewBox='0 0 {width} {height}'>",
        "<style>",
        "text{font-family:Menlo,Consolas,monospace;fill:#f4f4f4;font-size:18px}",
        ".tick{fill:#f4f4f4;font-size:16px}.minor{stroke:#313131;stroke-width:1}.major{stroke:#707070;stroke-width:1}",
        ".axis{stroke:#777;stroke-width:1.2}.label{font-size:20px}.note{fill:#111;font-size:16px}",
        "</style>",
        "<rect width='100%' height='100%' fill='#1f3858'/>",
        f"<rect x='{left}' y='{top}' width='{plot_w}' height='{plot_h}' fill='#101112' stroke='#444' stroke-width='1.5'/>",
        f"<text x='{width/2}' y='28' text-anchor='middle' font-size='22'>Compression Throughput vs Size</text>",
        f"<text x='{width/2}' y='49' text-anchor='middle' font-size='14'>{title}</text>",
    ]
    for x in x_ticks:
        px = sx(x)
        parts.append(f"<line class='minor' x1='{px:.1f}' y1='{top}' x2='{px:.1f}' y2='{top+plot_h}'/>")
        parts.append(f"<text class='tick' x='{px:.1f}' y='{top+plot_h+32}' text-anchor='middle'>{fmt_x(x)}</text>")
    for y in minor_y:
        py = sy(y)
        parts.append(f"<line class='minor' x1='{left}' y1='{py:.1f}' x2='{left+plot_w}' y2='{py:.1f}'/>")
    for y in major_y:
        py = sy(y)
        parts.append(f"<line class='major' x1='{left}' y1='{py:.1f}' x2='{left+plot_w}' y2='{py:.1f}'/>")
        parts.append(f"<text class='tick' x='{left-12}' y='{py+6:.1f}' text-anchor='end'>{fmt_y(y)}</text>")
    parts.extend([
        f"<line class='axis' x1='{left}' y1='{top+plot_h}' x2='{left+plot_w}' y2='{top+plot_h}'/>",
        f"<line class='axis' x1='{left}' y1='{top}' x2='{left}' y2='{top+plot_h}'/>",
        f"<text class='label' x='{width/2}' y='{height-24}' text-anchor='middle'>Compressed Size (KiB)</text>",
        f"<text class='label' transform='translate(34 {top+plot_h/2}) rotate(-90)' text-anchor='middle'>Throughput</text>",
    ])
    legend_w = 250
    legend_h = 34 + 30 * len(groups)
    legend_x = left + plot_w - legend_w - 18
    legend_y = top + plot_h - legend_h - 18
    legend_parts = [f"<rect x='{legend_x}' y='{legend_y}' width='{legend_w}' height='{legend_h}' fill='#151515' stroke='#686868' stroke-width='1.5'/>"]
    for i, codec in enumerate(sorted(groups)):
        color = colors.get(codec, "#9aa")
        y = legend_y + 28 + i * 30
        legend_parts.append(f"<rect x='{legend_x+18}' y='{y-14}' width='18' height='18' fill='{color}' opacity='0.88'/>")
        legend_parts.append(f"<text x='{legend_x+46}' y='{y+3}'>{names.get(codec, codec)}</text>")
    marker_boxes = []
    label_requests = []
    placed_labels = [(legend_x, legend_y, legend_w, legend_h)]
    def overlaps(a, b, pad=0):
        ax, ay, aw, ah = a
        bx, by, bw, bh = b
        return ax < bx + bw + pad and ax + aw + pad > bx and ay < by + bh + pad and ay + ah + pad > by

    def add_level_label(x, y, text, preferred):
        w, h = max(84, 9 * len(text) + 12), 24
        candidates = [
            preferred,
            (18, -34), (18, 48), (-w - 18, -12), (-w - 18, 38),
            (28, -64), (28, 72), (-w - 28, -44), (-w - 28, 68),
            (42, -94), (42, 102), (-w - 42, -74), (-w - 42, 98),
        ]
        chosen = None
        for dx, dy in candidates:
            lx = x + dx
            ly = y + dy - h
            lx = min(max(lx, left + 6), left + plot_w - w - 6)
            ly = min(max(ly, top + 6), top + plot_h - h - 6)
            box = (lx, ly, w, h)
            if any(overlaps(box, b, 8) for b in placed_labels):
                continue
            if any(overlaps(box, b, 8) for b in marker_boxes):
                continue
            chosen = box
            break
        if chosen is None:
            return
        lx, ly, w, h = chosen
        placed_labels.append(chosen)
        corners = [(lx, ly), (lx + w, ly), (lx, ly + h), (lx + w, ly + h)]
        cx, cy = min(corners, key=lambda p: (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y))
        parts.append(f"<line x1='{cx:.1f}' y1='{cy:.1f}' x2='{x:.1f}' y2='{y:.1f}' stroke='#f8f8f8' stroke-width='1.7'/>")
        parts.append(f"<rect x='{lx:.1f}' y='{ly:.1f}' width='{w}' height='{h}' fill='#f8f8f8'/>")
        parts.append(f"<text class='note' x='{lx+5:.1f}' y='{ly+18:.1f}'>{text}</text>")

    label_offsets = {"uzstd": (14, -12), "zstd": (14, 34)}
    for codec, rows in sorted(groups.items()):
        color = colors.get(codec, "#9aa")
        coords = [(sx(p["compressed_bytes"] / 1024), sy(p["compress_bytes_per_ns"]), p) for p in rows]
        poly = " ".join(f"{x:.1f},{y:.1f}" for x, y, _ in coords)
        parts.append(f"<polyline points='{poly}' fill='none' stroke='{color}' stroke-width='2.2' opacity='0.78'/>")
        for x, y, p in coords:
            if markers.get(codec) == "square":
                parts.append(f"<rect x='{x-5:.1f}' y='{y-5:.1f}' width='10' height='10' fill='{color}' stroke='{color}' stroke-width='2' opacity='0.94'/>")
                marker_boxes.append((x - 7, y - 7, 14, 14))
            else:
                parts.append(f"<circle cx='{x:.1f}' cy='{y:.1f}' r='6' fill='{color}' stroke='{color}' stroke-width='2' opacity='0.94'/>")
                marker_boxes.append((x - 8, y - 8, 16, 16))
        if coords:
            dx, dy = label_offsets.get(codec, (10, -10))
            endpoint_indexes = sorted(set([0, len(coords) - 1]))
            for idx in endpoint_indexes:
                x, y, p = coords[idx]
                label_requests.append((x, y, f"level={p['level']}", (dx, dy)))
    for x, y, text, preferred in label_requests:
        add_level_label(x, y, text, preferred)
    parts.extend(legend_parts)
    parts.append("</svg>\n")
    path.write_text("\n".join(parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=1)
    ap.add_argument("--small-warmups", type=int, default=100, help="warmup compressions before timing the tiny text corpus")
    ap.add_argument("--mixed-corpus", help="optional mixed text/binary corpus path for the large benchmark plot")
    args = ap.parse_args()
    BENCH.mkdir(exist_ok=True)
    corpora = [(ROOT / "uzstd.h", "uzstd.h")]
    if args.mixed_corpus:
        corpora.append((Path(args.mixed_corpus).resolve(), "mixed text/binary corpus"))
    compile_rows = build_compile_table() + build_zstd_unity_table()
    perf_rows = throughput_rows(corpora, args.iterations, args.small_warmups)
    payload = {"compile": compile_rows, "performance": perf_rows, "loc": loc_rows()}
    (BENCH / "results.json").write_text(json.dumps(payload, indent=2) + "\n")
    compile_md = md_table(
        ["Target", "Compiler", "Config", "Compile ms", ".o bytes"],
        [[r["target"], r["compiler"], r["config"], fmt_ms(r["compile_ms"]), "n/a" if r["object_bytes"] is None else r["object_bytes"]] for r in compile_rows],
    )
    loc_md = md_table(["Component", "Lines", "Note"], [[r["component"], r["lines"], r["note"]] for r in loc_rows()])
    (BENCH / "results.md").write_text("### Compile and object size\n\n" + compile_md + "\n\n### Lines of code\n\n" + loc_md + "\n")
    for f, label in corpora:
        kind = "text" if label == "uzstd.h" else "mixed text and binary data"
        slug = "uzstd_h" if label == "uzstd.h" else "mixed_corpus"
        title_name = label if label == "uzstd.h" else f.name
        title = f"{title_name} - {human_size(size(f))}, {kind}"
        write_svg(
            f"{slug}.svg",
            [r for r in perf_rows if r["label"] == label],
            title,
        )


if __name__ == "__main__":
    main()
