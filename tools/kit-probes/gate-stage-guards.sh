#!/usr/bin/env bash
# guards: templates/cpp/ci.sh
#
# Kit-conformance probe for the gate-integrity fix of 2026-09-20 (card t_0cc793fb, found while
# porting the probe stage into Computo's fork).
#
#   probes/gate-stage-guards.sh <gate-script> [repo-root]
#
# THE DEFECT, measured. `set -u` + `local -a <name>` (declared, never filled) + `"${#<name>[@]}"`
# is an unbound-variable error on bash 5.2 — and bash does not stop at the stage: it unwinds out
# of the stage function AND out of the dispatch loop, so the run falls through to the end and
# prints "all N stage(s) passed ... GATE PASSED" with every stage after the aborted one never
# executed. Measured on Computo's fork before the fix (tools/ci.sh line 357, stage_format with no
# C++ file in the touched set):
#
#   tools/ci.sh: line 357: sources: unbound variable
#   --- ci summary ---   /   pass  tree   /   all 10 stage(s) passed in 0s   /   GATE PASSED   rc 0
#
# — one stage of ten executed, a green verdict, and a `git push` that went through on it. The
# kit's contract rule 1 ("a stage that did not run must never read as green", DESIGN-NOTES D6) is
# exactly what that violates, and it is invisible to the summary that rule 1 introduced.
#
# The probe checks the three holes closed by the fix:
#   G1  no unassigned `local -a <name>` in the gate            (the trigger)
#   G2  the dispatch loop fails a stage that returns non-zero without reporting a verdict
#   G3  the verdict comes from what RAN: a requested stage that did not run fails the run
#       (the backstop — the only guard that can catch an unwind of the loop itself)
#
# Limits, as with every probe: G1 is a pattern, not a proof that no other shell error exists, and
# G2/G3 are checked by their text. G3 is the reason that is acceptable — it turns any future
# silent stage loss into a red run, whatever the cause.
#
# Exit 0 = PROBE VERIFIED, 1 = PROBE FAILED.
set -uo pipefail

S=${1:?usage: gate-stage-guards.sh <gate-script> [repo-root]}
[ -f "$S" ] || { echo "PROBE FAILED (no such script: $S)"; exit 1; }

fails=0
oks=0

check() {  # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

printf '=== probe: gate stage guards (a stage that dies or fails must not read green)\n'
printf '    script: %s\n' "$S"

# G1 — the trigger. An unassigned `local -a x` plus `set -u` is the abort that started all this.
unassigned=$(grep -cE '^[[:space:]]*local -a[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$' "$S" || true)
if [ "${unassigned:-0}" -eq 0 ]; then
    oks=$((oks + 1)); printf '  ok   no unassigned `local -a <name>` (set -u cannot abort on one)\n'
else
    fails=$((fails + 1))
    printf '  FAIL %s unassigned `local -a <name>` declaration(s): with `set -u`, "${#name[@]}" on\n' "$unassigned"
    printf '       one of them aborts the stage AND the dispatch loop, and the run reads green\n'
    grep -nE '^[[:space:]]*local -a[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$' "$S" | sed 's/^/       /'
fi

# G2 — a stage that returns non-zero without reporting a verdict is a failure.
grep -qF 'exited non-zero without reporting a verdict' "$S"
check $? "the dispatch loop fails a stage that returns non-zero without reporting"

# G3 — the verdict is derived from what ran.
grep -qF 'stage(s) did not run' "$S"
check $? "the run fails when a requested stage did not run (the backstop for an unwind)"

printf 'PROBE %s (%d ok, %d failed)\n' \
    "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
