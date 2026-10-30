#!/usr/bin/env bash
#
# tests — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.tests] cmd = "scripts/tests.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "tests"
    if [ ! -f "$CI_BUILD_DIR/CMakeCache.txt" ]; then
        ci_fail tests "no build configured in $CI_BUILD_DIR — run the build stage first"
    fi
    # This stage does NOT build. ctest reports 100% against whatever binary is on disk, including
    # one the current sources no longer compile — measured 2026-10-30: four compile errors in
    # tests/test_oracle_laws.cpp and still "100% tests passed, 0 tests failed out of 131", exit 0.
    # The tier is safe (the build stage runs first, and it does fail); a single-stage run is not,
    # and single-stage runs are exactly what the docs tell a person (or an agent) to do.
    ci_require_fresh tests jsonfuzz_tests
    if ! run_tests "$CI_BUILD_DIR" "$CI_LOG_DIR/tests.log"; then
        grep -E "FAILED|Failed|\*\*\*Failed|assert" "$CI_LOG_DIR/tests.log" | head -30 | sed 's/^/      /'
        ci_fail tests "test failures (all of them are above; full output in the log)" "$CI_LOG_DIR/tests.log"
    fi
    grep -E "tests passed|100% tests passed" "$CI_LOG_DIR/tests.log" | tail -1 | sed 's/^/      /'
    ci_pass tests
}

run_stage "$@"
