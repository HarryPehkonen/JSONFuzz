#!/usr/bin/env bash
#
# docs — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.docs] cmd = "scripts/docs.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "docs (no file in CI_DOCS_FILES quotes a test count)"
    # The owner's rule (docs/BRIEF-v0.1.md): no document may quote a test count — badge,
    # prose, label or parenthetical. INCIDENTS.md is exempt: its entries are incident
    # measurements (a number of findings, a number of seconds), not suite sizes, so it is
    # deliberately not in CI_DOCS_FILES.
    local pat='[0-9]+[[:space:]]*tests?|tests?[[:space:]]*[:=][[:space:]]*[0-9]+|test[[:space:]]+cases?[[:space:]]*[:=][[:space:]]*[0-9]+'
    local bad=0 f
    for f in $CI_DOCS_FILES; do
        if [ -f "$f" ] && grep -qnE "$pat" "$f"; then
            printf '    test count quoted in %s:\n' "$f"
            grep -nE "$pat" "$f" | sed 's/^/      /'
            bad=1
        fi
    done
    if [ "$bad" -eq 1 ]; then
        ci_fail docs "a file in CI_DOCS_FILES quotes a test count — the owner's rule. INCIDENTS.md is exempt (incident measurements, not suite sizes)."
    fi
    printf '    no test counts in CI_DOCS_FILES\n'
    ci_pass docs
}

run_stage "$@"
