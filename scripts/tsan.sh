#!/usr/bin/env bash
#
# tsan — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.tsan] cmd = "scripts/tsan.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "tsan (ThreadSanitizer, separate build dir)"
    # A thread sanitizer on a single-threaded suite still catches static/shared state
    # touched from more than one thread — the bug this gate exists for.
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_TSAN_BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/tsan-configure.log" 2>&1 \
        || ci_fail tsan "cmake configure failed" "$CI_LOG_DIR/tsan-configure.log"
    cmake --build "$CI_TSAN_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/tsan-build.log" 2>&1 \
        || ci_fail tsan "ThreadSanitizer build failed" "$CI_LOG_DIR/tsan-build.log"
    local saved="$CI_TEST_CMD"
    CI_TEST_CMD="$(printf '%s' "$saved" | sed "s|\$CI_BUILD_DIR|$CI_TSAN_BUILD_DIR|g")"
    if ! TSAN_OPTIONS=halt_on_error=1 run_tests "$CI_TSAN_BUILD_DIR" "$CI_LOG_DIR/tsan.log"; then
        grep -E "WARNING: ThreadSanitizer|data race|FAILED" "$CI_LOG_DIR/tsan.log" | head -20 | sed 's/^/      /'
        ci_fail tsan "ThreadSanitizer reported a data race (or a wrong answer)" "$CI_LOG_DIR/tsan.log"
    fi
    CI_TEST_CMD="$saved"
    printf '    no data races reported\n'
    ci_pass tsan
}

run_stage "$@"
