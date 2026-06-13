#!/usr/bin/env bash
set -euo pipefail

cc_cmd=${CC:-cc}
cflags=${CFLAGS:--std=c99 -O2 -Wall -Wextra -pedantic}
exe=${UZSTD_EXE:-./uzstd_mesh}
download_dir=${DOWNLOAD_DIR:-downloaded}
py=${PYTHON:-python3}
case "$exe" in
    *.exe|*/*.exe) tmp_dir=${UZSTD_TMPDIR:-uzstd_tmp_mesh} ;;
    *) tmp_dir=${UZSTD_TMPDIR:-/tmp/uzstd_mesh} ;;
esac
export UZSTD_TMPDIR="$tmp_dir"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    "$cc_cmd" $cflags uzstd.c -o "$exe"
fi

"$py" - "$tmp_dir" <<'PY'
import hashlib
import sys
from pathlib import Path
tmp = Path(sys.argv[1])
tmp.mkdir(parents=True, exist_ok=True)
(tmp / 'local.sha256').write_text(hashlib.sha256(Path('uzstd.h').read_bytes()).hexdigest())
PY

expected=$(cat "$tmp_dir/local.sha256")
found=0
while IFS= read -r -d '' file; do
    found=$((found + 1))
    "$exe" -d "$file" >"$tmp_dir/decode.log"
    read -r actual decoded_bytes <<EOF
$("$py" - "$tmp_dir/uzstd_out.bin" <<'PY'
import hashlib
import sys
from pathlib import Path
p = Path(sys.argv[1])
data = p.read_bytes()
print(hashlib.sha256(data).hexdigest(), len(data))
PY
)
EOF
    if [ "$actual" != "$expected" ]; then
        local_bytes=$("$py" - <<'PY'
from pathlib import Path
print(len(Path('uzstd.h').read_bytes()))
PY
)
        echo "mesh mismatch for $file" >&2
        echo "expected sha256=$expected bytes=$local_bytes" >&2
        echo "actual   sha256=$actual bytes=$decoded_bytes" >&2
        exit 1
    fi
done < <(find "$download_dir" -name 'uzstd.h.l*.zst' -type f -print0)

if [ "$found" -eq 0 ]; then
    echo "no mesh artifacts found under $download_dir" >&2
    exit 1
fi

echo "verified $found mesh artifacts"
