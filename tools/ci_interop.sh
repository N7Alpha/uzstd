#!/usr/bin/env bash
set -euo pipefail

cc_cmd=${CC:-cc}
cflags=${CFLAGS:--std=c99 -O2 -Wall -Wextra -pedantic}
exe=${UZSTD_EXE:-./uzstd}
artifact_dir=${ARTIFACT_DIR:-artifacts/local}
py=${PYTHON:-python3}
zstd=${ZSTD_EXE:-zstd}
case "$exe" in
    *.exe|*/*.exe) tmp_dir=${UZSTD_TMPDIR:-uzstd_tmp_ci} ;;
    *) tmp_dir=${UZSTD_TMPDIR:-/tmp/uzstd_ci} ;;
esac
export UZSTD_TMPDIR="$tmp_dir"

mkdir -p "$artifact_dir" "$tmp_dir"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    "$cc_cmd" $cflags uzstd.c -o "$exe"
fi
"$exe"

"$py" - "$tmp_dir" <<'PY'
import sys
from pathlib import Path
out = Path(sys.argv[1]) / 'corpus'
out.mkdir(parents=True, exist_ok=True)
cases = {
    'empty.bin': b'',
    'one.bin': b'x',
    'rle.bin': b'A' * (256 * 1024),
    'text.bin': (b'the quick brown fox jumps over the lazy dog. ' * 8192),
    'struct.bin': b''.join(((1000000 + i).to_bytes(4, 'little') for i in range(65536))),
}
x = 0x12345678
buf = bytearray()
for _ in range(256 * 1024):
    x = (x * 1664525 + 1013904223) & 0xffffffff
    buf.append((x >> 24) & 0xff)
cases['random.bin'] = bytes(buf)
cases['uzstd.h'] = Path('uzstd.h').read_bytes()
for name, data in cases.items():
    (out / name).write_bytes(data)
PY

for src in "$tmp_dir"/corpus/*; do
    [ -f "$src" ] || continue
    base=$(basename "$src")
    for level in 1 2 3 4 5 6 7 8 9; do
        "$exe" "$src" "$level" >"$tmp_dir/uzstd.log"
        "$zstd" -q -d -f "$tmp_dir/uzstd_file.zst" -o "$tmp_dir/${base}.uzstd.${level}.out"
        cmp -s "$src" "$tmp_dir/${base}.uzstd.${level}.out"
    done
    for level in 1 3 5 9 15 19; do
        "$zstd" -q -f --single-thread "-$level" "$src" -o "$tmp_dir/${base}.zstd.${level}.zst"
        "$exe" -d "$tmp_dir/${base}.zstd.${level}.zst" >"$tmp_dir/zstd.log"
        cmp -s "$src" "$tmp_dir/uzstd_out.bin"
        "$zstd" -q -f --single-thread "--check" "-$level" "$src" -o "$tmp_dir/${base}.zstd.${level}.check.zst"
        "$exe" -d "$tmp_dir/${base}.zstd.${level}.check.zst" >"$tmp_dir/zstd-check.log"
        cmp -s "$src" "$tmp_dir/uzstd_out.bin"
    done
done

for level in 1 5 9; do
    "$exe" uzstd.h "$level" >"$tmp_dir/artifact.log"
    cp "$tmp_dir/uzstd_file.zst" "$artifact_dir/uzstd.h.l${level}.zst"
done

"$py" - "$artifact_dir" <<'PY'
import hashlib
import sys
from pathlib import Path
d = Path(sys.argv[1])
with (d / 'sha256.txt').open('w') as f:
    f.write(hashlib.sha256(Path('uzstd.h').read_bytes()).hexdigest() + '  uzstd.h\n')
    for p in sorted(d.glob('*.zst')):
        f.write(hashlib.sha256(p.read_bytes()).hexdigest() + f'  {p.name}\n')
PY
