#!/usr/bin/env bash
#
# fuzz — split out of the old tools/ci.sh (2026-10-06, card t_075c0a6f).
#
# Called from gate.toml as `[stage.fuzz] cmd = "scripts/fuzz.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "fuzz (clang-only libFuzzer: -runs=0 seed smoke + ${CI_FUZZ_SECONDS}s)"
    if ! command -v clang++ >/dev/null 2>&1; then
        ci_skip fuzz "clang++ not installed (libFuzzer needs clang)"
        return 0
    fi
    # clang only, in its own build dir so the default build is never touched.
    cmake -S . -B "$CI_FUZZ_BUILD_DIR" -DPROJECT_BUILD_FUZZING=ON -DJSONFUZZ_BUILD_FUZZING=ON \
        -DJSONFUZZ_BUILD_TESTS=OFF -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        > "$CI_LOG_DIR/fuzz-configure.log" 2>&1 \
        || ci_fail fuzz "cmake configure failed" "$CI_LOG_DIR/fuzz-configure.log"
    cmake --build "$CI_FUZZ_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/fuzz-build.log" 2>&1 \
        || ci_fail fuzz "fuzz target build failed" "$CI_LOG_DIR/fuzz-build.log"
    mkdir -p fuzz/corpus
    # Seed smoke: -runs=0 plays every seed through the target once and reports the count.
    # JSONFUZZ_REQUIRE_REACH=1 makes the target exit non-zero unless EVERY reading reached
    # its assertion (brief §5: "at least one input got there" is the guard that hid a
    # blind spot in a sibling repo).
    if ! JSONFUZZ_REQUIRE_REACH=1 "$CI_FUZZ_BUILD_DIR/fuzz_jsonfuzz" fuzz/seeds fuzz/corpus \
        -runs=0 -artifact_prefix=fuzz/corpus/ > "$CI_LOG_DIR/fuzz-smoke.log" 2>&1; then
        ci_fail fuzz "the -runs=0 seed smoke failed (a reading starved, or a finding)" "$CI_LOG_DIR/fuzz-smoke.log"
    fi
    grep -E "Done|running [0-9]+ inputs|INFO:.*files found" "$CI_LOG_DIR/fuzz-smoke.log" | tail -2 | sed 's/^/      /'
    # The short real campaign.
    if ! "$CI_FUZZ_BUILD_DIR/fuzz_jsonfuzz" fuzz/seeds fuzz/corpus \
        -artifact_prefix=fuzz/corpus/ -max_total_time="$CI_FUZZ_SECONDS" > "$CI_LOG_DIR/fuzz.log" 2>&1; then
        ci_fail fuzz "the fuzzer found something (artifact in fuzz/corpus/, add a regression test)" "$CI_LOG_DIR/fuzz.log"
    fi
    grep -E "Done|stat::number_of_executed_units|stat::new_units_added" "$CI_LOG_DIR/fuzz.log" | tail -2 | sed 's/^/      /'
    ci_pass fuzz
}

run_stage "$@"
