#!/usr/bin/env bash
# guards: templates/cpp/ci.sh
#
# Kit-conformance probe for the kit fix of 2026-10-06: `format-checks-staged`.
#
# WHY A PROBE AND NOT A HASH: the same reason as every other probe here. A repo's copy of the
# gate is a FORK (jsonTools differs from the kit by 586 lines, JSOM by 582, Permuto by 89,
# Computo by 60 — measured 2026-09-20), so no byte comparison can say whether the copy carries
# this fix.
#
# THE CONTRACT. The `format` stage must check what a COMMIT WOULD RECORD, not only what is on
# disk. clang-format reads files from the working tree; `git commit` records the index. Stage an
# unformatted file, then format it on disk — which is exactly what a developer does after this
# very stage rejects their commit — and a working-tree-only check passes while the commit lands
# the unformatted text. HEAD then differs from the working tree, so the next `--require-clean`
# push fails in `tree` with "uncommitted changes to tracked files": a message that names no
# formatting problem at all, one stage away from the cause. Measured 2026-10-06 in a throwaway
# clone of FSMTable, and reproduced here as P1.
#
# A copy that checks only the working tree does not fail this probe in the "gate is broken" way.
# It fails it by PASSING a case that must fail, which is the failure mode that matters: a gate
# certifying a commit it did not look at.
#
# WHAT IT CHECKS
#   P0  the fixture is what it claims: clang-format wants to rewrite the "unformatted" text and
#       leaves the "formatted" text alone (without this the probe could pass vacuously)
#   P1  CONTRACT: index unformatted + working tree formatted -> the gate FAILS
#   P2  NEGATIVE CONTROL: index == working tree == formatted -> the gate PASSES
#   P3  PARTIAL STAGE: index formatted, working tree different but also formatted -> the gate
#       PASSES (the fix must not outlaw `git add -p`; a naive "index != disk" rule would)
#
# A gate with no `format` stage is a printed SKIP, not a FAIL: the contract has no subject there
# (a deno or python gate formats with deno fmt --check / ruff format --check, which this probe
# does not model), so calling that copy "behind" would be the false verdict this probe exists to
# avoid. Same for a machine without clang-format: the stage itself skips there, so there is
# nothing to test.
#
# Exit 0 = PROBE VERIFIED (or SKIP, printed), 1 = PROBE FAILED.
set -uo pipefail

G=${1:?usage: format-checks-staged.sh <gate-script> [repo-root]}
[ -f "$G" ] || { printf 'PROBE FAILED (no such file: %s)\n' "$G"; exit 1; }
ROOT=${2:-$(cd "$(dirname "$G")/.." && pwd)}

fails=0
oks=0
check() {  # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

printf '=== probe: the format stage checks the staged copy, not only the working tree\n'

if ! command -v clang-format > /dev/null 2>&1; then
    printf '  SKIP no clang-format on this machine — the stage itself skips here, so there is nothing to test\n'
    printf 'PROBE SKIPPED (1 skipped)\n'
    exit 0
fi
if ! grep -q '^stage_format()' "$G"; then
    printf '  SKIP this gate has no format stage, so the contract has no subject\n'
    printf 'PROBE SKIPPED (1 skipped)\n'
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/repo/tools" "$TMP/repo/src"
cp "$G" "$TMP/repo/tools/ci.sh"
chmod +x "$TMP/repo/tools/ci.sh"
# The stage's verdict depends on the repo's style, so test it under this repo's own config.
[ -f "$ROOT/.clang-format" ] && cp "$ROOT/.clang-format" "$TMP/repo/.clang-format"
REPO=$TMP/repo
BAD='int probe_value() { int      x = 1; return x; }
int probe_second() { int      y = 2; return y; }
'
cd "$REPO" || exit 1
git init -q -b main .
git config user.email probe@probe.local
git config user.name probe
printf '%s' "$BAD" > src/probe.cpp
clang-format -i src/probe.cpp
GOOD=$(cat src/probe.cpp)
# The base commit holds the FORMATTED text, so that staging the unformatted one below is a real
# staged change (`git diff --cached` must see it) -- otherwise the probe tests nothing.
printf '%s' "$GOOD" > src/probe.cpp
git add -A > /dev/null && git commit -qm base
printf '%s' "$BAD" > "$TMP/bad.txt"
printf '%s' "$GOOD" > "$TMP/good.txt"
# A second formatted variant, generated INSIDE the repo so the style lookup finds .clang-format and
# the language comes from a .cpp name. (A fixture built at a .txt path is neither: clang-format
# treats it as another language and passes it unchanged, which is a probe bug, not a gate bug.)
printf '%s\n// a third line, so this variant differs from the other formatted one\n' "$GOOD" > src/probe2.cpp
clang-format -i src/probe2.cpp
cp src/probe2.cpp "$TMP/good2.txt"
rm -f src/probe2.cpp

# P0 — the fixture has teeth
# Fed through stdin with --assume-filename, exactly as the gate checks the staged blob: a file
# argument would use the fixture's own .txt extension for the language and find no .clang-format.
clang-format --dry-run -Werror --assume-filename=src/probe.cpp - < "$TMP/bad.txt" > /dev/null 2>&1
check $([ $? -ne 0 ] && echo 0 || echo 1) "P0 the 'unformatted' fixture is text clang-format rewrites"
clang-format --dry-run -Werror --assume-filename=src/probe.cpp - < "$TMP/good.txt" > /dev/null 2>&1
check $? "P0 the 'formatted' fixture is text clang-format leaves alone"
clang-format --dry-run -Werror --assume-filename=src/probe.cpp - < "$TMP/good2.txt" > /dev/null 2>&1
check $? "P0 the second 'formatted' fixture is text clang-format leaves alone (P3 rests on it)"

run_gate() {  # run_gate <staged-file> <disk-file>; echoes PASS or FAIL
    cp "$1" src/probe.cpp
    git add src/probe.cpp > /dev/null
    cp "$2" src/probe.cpp
    if ./tools/ci.sh format > "$TMP/out.txt" 2>&1; then echo PASS; else echo FAIL; fi
}

# P1 — the contract
check $([ "$(run_gate "$TMP/bad.txt" "$TMP/good.txt")" = FAIL ] && echo 0 || echo 1) \
      "P1 CONTRACT: unformatted in the index, formatted on disk -> the gate fails"
# P2 — negative control
check $([ "$(run_gate "$TMP/good.txt" "$TMP/good.txt")" = PASS ] && echo 0 || echo 1) \
      "P2 NEGATIVE CONTROL: index == working tree, formatted -> the gate passes"
# P3 — partial staging stays legal
check $([ "$(run_gate "$TMP/good.txt" "$TMP/good2.txt")" = PASS ] && echo 0 || echo 1) \
      "P3 PARTIAL STAGE: index formatted, working tree different but formatted -> the gate passes"

printf 'PROBE %s (%s ok, %s failed)\n' "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
