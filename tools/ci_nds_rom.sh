#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITARM:=$DEVKITPRO/devkitARM}"
: "${MAX_FRAMES:=3600}"

case "$(uname -s)" in
  Darwin)
    CORE_EXT=dylib
    DL_LIBS=
    ;;
  *)
    CORE_EXT=so
    DL_LIBS=-ldl
    ;;
esac

: "${LIBRETRO_CORE:=$ROOT/.bench/melonds-ds-dist/cores/melondsds_libretro.$CORE_EXT}"
: "${NDS_RUNNER:=$ROOT/.bench/libretro_nds_runner}"

export DEVKITPRO DEVKITARM
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

mkdir -p "$ROOT/.bench" "$ROOT/.bench/libretro-system" "$ROOT/.bench/libretro-save" "$ROOT/bench"

echo "DEVKITPRO=$DEVKITPRO"
echo "DEVKITARM=$DEVKITARM"
command -v arm-none-eabi-gcc
command -v ndstool
arm-none-eabi-gcc --version | head -n 1

make -C nds clean
make -C nds
test -s nds/uzstd_nds_bench.nds

cc -std=c99 -O2 -Wall -Wextra -pedantic tools/libretro_nds_runner.c -o "$NDS_RUNNER" $DL_LIBS

if [ ! -s "$LIBRETRO_CORE" ]; then
  echo "missing libretro core: $LIBRETRO_CORE" >&2
  exit 1
fi

LIBRETRO_SYSTEM_DIR="$ROOT/.bench/libretro-system" \
LIBRETRO_SAVE_DIR="$ROOT/.bench/libretro-save" \
"$NDS_RUNNER" "$LIBRETRO_CORE" nds/uzstd_nds_bench.nds "$MAX_FRAMES" \
  | tee bench/nds_emulator_results.json

python3 - <<'PY'
import json
from pathlib import Path

path = Path("bench/nds_emulator_results.json")
payload = json.loads(path.read_text())
assert payload["status"] == "passed", payload
assert payload["case_count"] == 5, payload
assert payload["failures"] == 0, payload
for item in payload["results"]:
    assert item["roundtrip"] is True, item
    assert item["src_hash"] == item["dec_hash"], item
    assert item["comp_bytes"] > 0, item
print("nds libretro validation passed")
PY
