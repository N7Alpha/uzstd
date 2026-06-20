#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${UZSTD_COVERAGE_DIR:-"$root/coverage"}
iters=${UZSTD_FUZZ_MAX_ITERS:-100000}
cc_cmd=${CC:-}

if [ -z "$cc_cmd" ]; then
    if command -v clang >/dev/null 2>&1; then
        cc_cmd=clang
    else
        cc_cmd=cc
    fi
fi

if command -v llvm-profdata >/dev/null 2>&1; then
    profdata=(llvm-profdata)
elif command -v xcrun >/dev/null 2>&1; then
    profdata=(xcrun llvm-profdata)
else
    echo "llvm-profdata not found" >&2
    exit 1
fi

if command -v llvm-cov >/dev/null 2>&1; then
    cov=(llvm-cov)
elif command -v xcrun >/dev/null 2>&1; then
    cov=(xcrun llvm-cov)
else
    echo "llvm-cov not found" >&2
    exit 1
fi

mkdir -p "$build_dir"
rm -f "$build_dir"/uzstd_fuzz.*.profraw "$build_dir"/uzstd_fuzz.profdata

exe="$build_dir/uzstd_fuzz_cov"
profile="$build_dir/uzstd_fuzz.profdata"
report="$build_dir/coverage.txt"
annotated="$build_dir/uzstd_h_coverage.txt"

"$cc_cmd" -std=c99 -O1 -g \
    -fprofile-instr-generate -fcoverage-mapping \
    -Wall -Wextra -pedantic \
    "$root/tools/fuzz_forever.c" -o "$exe"

LLVM_PROFILE_FILE="$build_dir/uzstd_fuzz.%p.profraw" \
UZSTD_FUZZ_DIR="${UZSTD_FUZZ_DIR:-"$build_dir/fuzz_artifacts"}" \
UZSTD_FUZZ_MAX_ITERS="$iters" \
UZSTD_FUZZ_COVERAGE=1 \
"$exe"

profraws=( "$build_dir"/uzstd_fuzz.*.profraw )
if [ ! -e "${profraws[0]}" ]; then
    echo "no .profraw files were produced" >&2
    exit 1
fi

"${profdata[@]}" merge -sparse "${profraws[@]}" -o "$profile"
"${cov[@]}" report "$exe" -instr-profile="$profile" "$root/uzstd.h" | tee "$report"
"${cov[@]}" show "$exe" -instr-profile="$profile" "$root/uzstd.h" > "$annotated"
"${cov[@]}" show "$exe" -instr-profile="$profile" "$root/uzstd.h" \
    -show-branches=count > "$build_dir/branches.txt"

echo
echo "coverage profile: $profile"
echo "coverage report:  $report"
echo "annotated source: $annotated"

# 100%-over-reachable-code gate: every uncovered line and every uncovered branch
# side must be on a source line documented in tools/coverage_allowlist.txt
# (defensive/dead-by-invariant code, or a known llvm-cov mapping artifact). Any
# other uncovered line or branch fails the gate, so it doubles as a regression
# check. The allowlist is keyed by exact stripped source-line text.
echo
python3 - "$root/uzstd.h" "$build_dir/branches.txt" "$annotated" "$root/tools/coverage_allowlist.txt" <<'PY'
import re, sys
src_path, br_path, ann_path, allow_path = sys.argv[1:5]
src = open(src_path).read().splitlines()

allowed = set()
for ln in open(allow_path):
    s = ln.strip()
    if s and not s.startswith('#'):
        allowed.add(s)

# uncovered branch sides: "Branch (L:C): [True: N, False: M]" with a zero side
unc_branch = {}
for ln in open(br_path):
    m = re.search(r'Branch \((\d+):\d+\): \[True: ([\d.]+\w?), False: ([\d.]+\w?)\]', ln)
    if m and (m.group(2) == '0' or m.group(3) == '0'):
        unc_branch[int(m.group(1))] = src[int(m.group(1)) - 1].strip()

# uncovered lines: annotated "  <line>|      0|<text>" (execution count exactly 0)
unc_line = {}
for ln in open(ann_path):
    m = re.match(r'\s*(\d+)\|\s*0\|(.*)', ln)
    if m:
        unc_line[int(m.group(1))] = src[int(m.group(1)) - 1].strip()

bad_branch = sorted(l for l, t in unc_branch.items() if t not in allowed)
bad_line = sorted(l for l, t in unc_line.items() if t not in allowed)
used = {t for t in unc_branch.values() if t in allowed} | \
       {t for t in unc_line.values() if t in allowed}
stale = sorted(a for a in allowed if a not in used)

status = 0
print("=== coverage gate ===")
print("uncovered lines: %d (all documented)" % len(unc_line) if not bad_line
      else "uncovered lines: %d" % len(unc_line))
print("uncovered branch lines: %d; documented allowlist entries used: %d"
      % (len(unc_branch), len(used)))
if stale:
    print("WARNING: %d allowlist entries match no current miss "
          "(now covered, or text changed?):" % len(stale))
    for s in stale:
        print("  - %s" % s)
for l in bad_line:
    print("FAIL: undocumented uncovered line uzstd.h:%d  %s" % (l, unc_line[l]))
    status = 1
for l in bad_branch:
    print("FAIL: undocumented uncovered branch uzstd.h:%d  %s" % (l, unc_branch[l]))
    status = 1
if status == 0:
    print("PASS: every line and branch is covered or documented as unreachable.")
sys.exit(status)
PY
