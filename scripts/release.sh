#!/usr/bin/env bash
#
# release — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.release] cmd = "scripts/release.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "release ($CI_RELEASE_BUILD_TYPE: the configuration an optimized build uses)"
    # No -DCMAKE_EXPORT_COMPILE_COMMANDS here: the compile database is the `build` stage's
    # (tidy reads $CI_BUILD_DIR), and a second one would only be a decoy.
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_RELEASE_BUILD_DIR" -DCMAKE_BUILD_TYPE="$CI_RELEASE_BUILD_TYPE" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/release-configure.log" 2>&1 \
        || ci_fail release "cmake configure failed" "$CI_LOG_DIR/release-configure.log"
    cmake --build "$CI_RELEASE_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/release-build.log" 2>&1 \
        || ci_fail release "optimized build failed (-Werror is on: a warning is a build failure)" "$CI_LOG_DIR/release-build.log"
    # The same rule the `build` stage applies, to this log instead: -Werror only covers the
    # targets it is wired onto, and an optimization-dependent diagnostic is a warning before
    # it is an error. This is the check that reports the class the Debug stages cannot see.
    local warns
    warns=$(grep -c 'warning:' "$CI_LOG_DIR/release-build.log" || true)
    if [ "${warns:-0}" -gt 0 ]; then
        grep 'warning:' "$CI_LOG_DIR/release-build.log" | head -5 | sed 's/^/      /'
        ci_fail release "$warns compiler warning(s) in an optimized build" "$CI_LOG_DIR/release-build.log"
    fi
    printf '    built with no warnings at %s\n' "$CI_RELEASE_BUILD_TYPE"
    local saved="$CI_TEST_CMD"
    # shellcheck disable=SC2086
    CI_TEST_CMD="$(printf '%s' "$saved" | sed "s|\$CI_BUILD_DIR|$CI_RELEASE_BUILD_DIR|g")"
    if ! run_tests "$CI_RELEASE_BUILD_DIR" "$CI_LOG_DIR/release-tests.log"; then
        grep -E "FAILED|Failed|\*\*\*Failed|assert" "$CI_LOG_DIR/release-tests.log" | head -30 | sed 's/^/      /'
        ci_fail release "test failures in an optimized build (all of them are above; full output in the log)" "$CI_LOG_DIR/release-tests.log"
    fi
    CI_TEST_CMD="$saved"
    grep -E "tests passed|100% tests passed" "$CI_LOG_DIR/release-tests.log" | tail -1 | sed 's/^/      /'
    ci_pass release
}

run_stage "$@"
