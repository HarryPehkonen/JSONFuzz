#!/usr/bin/env bash
# guards: templates/cpp/ci.sh
#
# Kit-conformance probe for the kit fix of 2026-09-20: the tidy-baseline recipe
# (`tidy_key` applied to BOTH sides of the comparison + a `--write-tidy-baseline`
# capture that routes through the same normaliser).
#
#   probes/tidy-baseline.sh <path/to/gate-script> [repo-root]
#
# WHY A PROBE AND NOT A HASH. The repos' copies of the kit's ci.sh are FORKS, not
# copies: measured against the kit blob at fb387f2 the diffs are Computo 60, Permuto 89,
# JSOM 582, jsonTools 586 differing lines (kit header replaced, stages added/dropped,
# repo-local defaults, adaptation prose). Byte-comparison against the kit blob can
# therefore only ever answer "differs" for an `adapted: true` entry — it cannot tell a
# deliberate local decision from a fork that is simply MISSING a kit fix, which is
# exactly the 2026-09-20 failure. What a fork CAN be held to is the fix's CONTRACT.
# That is what this probe checks: the contract's parts, by name and by behaviour, in
# whatever copy the repo actually runs. No kit checkout, no network, no build, <1 s.
#
# WHERE IT RUNS. Inside each repo's own gate: the `kitprobes` stage runs every script in
# tools/kit-probes/ against the gate script itself. The kit runs its own copies through
# tools/kit-probes.sh. A fix that must propagate ships a probe — that is the rule
# (docs/KIT-REVISION-CONVENTION.md, "So how drift is actually caught: probes").
#
# What it does NOT do: it cannot see a semantic regression inside a present function
# (it feeds the normaliser one synthetic finding and checks the transformation, it does
# not run tidy). A dynamic probe of the same fix costs a real build + tidy run.
#
# Exit 0 = PROBE VERIFIED, 1 = PROBE FAILED.
set -uo pipefail

S=${1:?usage: tidy-baseline.sh <gate-script> [repo-root]}
[ -f "$S" ] || { echo "PROBE FAILED (no such script: $S)"; exit 1; }
ROOT=${2:-$(cd "$(dirname "$S")/.." && pwd)}
fails=0
oks=0

check() {  # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

printf '=== probe: tidy-baseline fix contract\n'
printf '    script: %s\n    root:   %s\n' "$S" "$ROOT"

# P1 — the normaliser exists as a function.
grep -qE '^[a-z_]+\(\) *\{' "$S"
check $? "a normaliser function is defined at top level"

# P2 — the normaliser BEHAVES: it strips the repo root and the :line:col from a finding.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sed -n '/^tidy_key() {/,/^}/p' "$S" > "$TMP/tidy_key.sh"
SAMPLE="$ROOT/src/x.cpp:12:34: warning: test thing [-Wfoo]"
OUT=$(REPO_ROOT="$ROOT" bash -c "source '$TMP/tidy_key.sh' 2>/dev/null; printf '%s\n' \"\$1\" | tidy_key || echo __NO_FN__" _ "$SAMPLE" 2>/dev/null)
printf '    sample in : %s\n    sample out: %s\n' "$SAMPLE" "${OUT:-<empty>}"
case "$OUT" in
    *__NO_FN__*|"") fails=$((fails + 1)); printf '  FAIL tidy_key() is missing or unusable, so findings cannot be normalised\n' ;;
    *":12:34"*)     fails=$((fails + 1)); printf '  FAIL tidy_key() does not strip :line:col — every moved line looks like a new finding\n' ;;
    *"$ROOT"*)      fails=$((fails + 1)); printf '  FAIL tidy_key() does not strip the repo root — a second checkout re-reports everything\n' ;;
    *src/x.cpp*)    oks=$((oks + 1)); printf '  ok   tidy_key() strips the repo root and :line:col (one key per finding)\n' ;;
    *)              fails=$((fails + 1)); printf '  FAIL tidy_key() output is not recognisable as the input finding\n' ;;
esac

# P3 — both sides of the comparison go through it.
grep -qE 'tidy_key *< *"\$CI_TIDY_BASELINE"' "$S"
check $? "the baselined side is normalised (tidy_key < \$CI_TIDY_BASELINE)"
grep -qE '\| *tidy_key' "$S"
check $? "the fresh-log side is normalised (tidy.log | tidy_key)"

# P4 — the accepting route exists and shares the same normaliser.
grep -q -- '--write-tidy-baseline' "$S"
check $? "--write-tidy-baseline is documented"
grep -qE '^ *--write-tidy-baseline\)' "$S"
check $? "--write-tidy-baseline is a real flag branch"
grep -qE '\|.*tidy_key.*> *"\$CI_TIDY_BASELINE"' "$S"
check $? "the capture writes \$CI_TIDY_BASELINE through tidy_key (cannot drift from the comparison)"

printf 'PROBE %s (%d ok, %d failed)\n' \
    "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
