#!/usr/bin/env bash
#
# cli — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.cli] cmd = "scripts/cli.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "cli (smoke: documented flags, unknown flag fails)"
    if ! bash "$REPO_ROOT/tools/cli_smoke.sh" "$CI_BUILD_DIR/jsonfuzz" > "$CI_LOG_DIR/cli.log" 2>&1; then
        ci_fail cli "a CLI check failed" "$CI_LOG_DIR/cli.log"
    fi
    tail -n 1 "$CI_LOG_DIR/cli.log" | sed 's/^/      /'
    ci_pass cli
}

run_stage "$@"
