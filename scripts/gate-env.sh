#!/usr/bin/env bash
#
# JSONFuzz's gate: the configuration every stage reads, and the helpers they share.
#
# The POLICY is gate.toml (which stages exist, which tier runs which of them, how a failure
# is recognised). The ENGINE is kit-ci, one binary installed once per machine
# (cmake --install build --prefix ~/.local). Everything a stage needs BEYOND that policy -
# a build dir, a job count, a fuzz budget - lives HERE, so the policy file stays a list of
# stages.
#
# SOURCED, never executed: scripts/gate.sh does not need it; every stage script does
# (`. "$(dirname "$0")/gate-env.sh"`). The knobs are the .ci.env knobs the old tools/ci.sh
# carried, each with the default it had there, and .ci.env is still sourced last, so a
# machine with no .ci.env behaves exactly as it did before the conversion
# (2026-10-06, card t_075c0a6f).

set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO_ROOT" || exit 1

# No colour from the tools: this script greps their output (warning:, error:) and ANSI
# escapes defeat the greps. The escapes this script prints itself are for the human.
export NO_COLOR=1

# git exports GIT_INDEX_FILE to a hook when the commit is made with a PATHSPEC
# (`git commit -- <path>`): it names git's TEMPORARY index for that one commit, not this
# repository's index. Every process a hook starts inherits it, and any `git` command the
# gate runs inside ANOTHER repository — cmake's FetchContent update step in a `build`
# stage, a `pip install git+https://…` requirement, a probe that clones into a temp dir, a
# repo's own kitprobes script — then reads THIS repo's index entries against that
# repository's object store and dies on the first blob it does not have:
#     fatal: unable to read 691e2bdafaf312970644391de042d38c2c5972d8
#     CMake Error at .../jsom-populate-gitupdate.cmake:186 (message): Failed to get the status
# — so a pathspec commit fails its OWN gate at `build`, naming a dependency update, on a
# tree that builds fine (measured 2026-09-20 on Computo, cards t_9541aa62 -> t_0a9a0018).
#
# Unset it once, here, rather than `env -u` in front of each command that shells out to
# git: the variable reaches everything the gate starts, so a per-invocation fix would
# cover the instance and leave the class. It is also the safe place, not merely the cheap
# one — measured on a real pathspec commit in a throwaway clone, with the gate's own
# stages run both ways: every input the `tree` and `format` stages read is identical and
# their output is byte-identical, and the two views that do differ (`git diff --cached
# HEAD`, the staged-vs-unstaged letter) are read by no stage. `probes/git-index-file.sh`
# holds every copy to this by running the block below inside a real pathspec commit.
unset GIT_INDEX_FILE

# ---------------------------------------------------------------- defaults + config
CI_JOBS=${CI_JOBS:-$(nproc 2>/dev/null || echo 4)}
CI_BUILD_DIR=${CI_BUILD_DIR:-build}
CI_ASAN_BUILD_DIR=${CI_ASAN_BUILD_DIR:-build-asan}
CI_TSAN_BUILD_DIR=${CI_TSAN_BUILD_DIR:-build-tsan}
CI_LOG_DIR=${CI_LOG_DIR:-.ci-logs}
CI_STRICT_TOOLS=${CI_STRICT_TOOLS:-0}           # 1 = a missing tool fails instead of SKIPping
CI_KEEP_TMP=${CI_KEEP_TMP:-0}                   # 1 = keep the pristine temp dir for inspection
CI_DEFAULT_STAGES=${CI_DEFAULT_STAGES:-"tree docs format kitprobes build tests release cli version asan fuzz tsan tidy pristine"}
# `format` joined the fast tier on 2026-10-06 — see INCIDENTS.md.
CI_FAST_STAGES=${CI_FAST_STAGES:-"tree format build tests"}
CI_TIDY_BASELINE=${CI_TIDY_BASELINE:-.ci/tidy-baseline.txt}
CI_BUILD_TYPE=${CI_BUILD_TYPE:-Debug}
# The SECOND configuration, built and tested by the `release` stage. A gate whose every
# stage builds one build type cannot see the class the other one produces (a `warning:`
# that only exists under -O2/-O3, code that only misbehaves with NDEBUG). Point this at
# whatever the repo's own pipeline builds; Release is the usual answer.
CI_RELEASE_BUILD_DIR=${CI_RELEASE_BUILD_DIR:-build-release}
CI_RELEASE_BUILD_TYPE=${CI_RELEASE_BUILD_TYPE:-Release}
# The source set the format and tidy stages own. Extend for your layout.
CI_SOURCE_GLOBS=${CI_SOURCE_GLOBS:-"'include/jsonfuzz/*.hpp' 'src/*.cpp' 'tests/*.cpp' 'fuzz/*.cpp'"}
# Where the SECOND copy of the version number lives. This repo generates it under a
# jsonfuzz/ subdir of the build tree (see ADAPTATIONS #3).
CI_VERSION_HEADER=${CI_VERSION_HEADER:-"$CI_BUILD_DIR/generated/jsonfuzz/version.hpp"}
# The binaries that must answer --version with the same number (see ADAPTATIONS #3).
CI_VERSION_BINARIES=${CI_VERSION_BINARIES:-"$CI_BUILD_DIR/jsonfuzz"}
# The test runner. ctest is the portable default; override with a single test binary if
# your project does not register tests with add_test().
CI_TEST_CMD=${CI_TEST_CMD:-"ctest --test-dir \$CI_BUILD_DIR --output-on-failure -j \$CI_JOBS"}
# Fuzzing (see ADAPTATIONS #5).
CI_FUZZ_SECONDS=${CI_FUZZ_SECONDS:-10}
CI_FUZZ_BUILD_DIR=${CI_FUZZ_BUILD_DIR:-build-fuzz}
# The files the `docs` stage holds to the owner's rule: no quoted test count. INCIDENTS.md
# is deliberately NOT in this list — its entries are incident measurements, not suite
# sizes, so it is exempt (the stage says so).
CI_DOCS_FILES=${CI_DOCS_FILES:-"README.md CLAUDE.md docs/BRIEF-v0.1.md"}

if [ -f .ci.env ]; then
    # shellcheck disable=SC1091
    . ./.ci.env
fi


# The old tools/ci.sh took these as FLAGS (--require-clean, --changed, --extra-checks,
# --write-tidy-baseline, --write-coverage-baseline). kit-ci's vocabulary has no per-run flags
# for a stage: a caller sets the knob in the ENVIRONMENT it launches the gate with, and the
# default below is what the script had. .githooks/pre-push does exactly that with
# CI_REQUIRE_CLEAN=1. (Until this line the assignment overwrote whatever the caller exported,
# so the push hook's flag-as-env-var had no effect at all - measured 2026-10-06.)

REQUIRE_CLEAN=${CI_REQUIRE_CLEAN:-0}
ALLOW_UNTRACKED=0
WRITE_TIDY_BASELINE=0
STAGES_REQUESTED=()

# ---------------------------------------------------------------- plumbing
RESULT_LINES=()
FAILED_STAGE=""
RAN_STAGES=()
RUN_TMP_DIRS=()





require_tool() {  # require_tool <tool> <stage>; returns 1 when the caller should skip
    local tool="$1" stage="$2"
    if command -v "$tool" >/dev/null 2>&1; then return 0; fi
    if [ "$CI_STRICT_TOOLS" = "1" ]; then
        ci_fail "$stage" "$tool not installed (CI_STRICT_TOOLS=1) — nothing was checked"
    fi
    ci_skip "$stage" "$tool not installed — this gate was NOT exercised on this machine"
    return 1
}

# The file set the format gate owns: same list the CMake `format` target should use.
# --others --exclude-standard includes new files that are not committed yet: while
# working, a new source file is not in `git ls-files`, and a gate that cannot see it
# lets it through unformatted until after it is committed (that incident is in
# INCIDENTS.md).
ci_sources() {
    # shellcheck disable=SC2086
    eval "git ls-files --cached --others --exclude-standard -- $CI_SOURCE_GLOBS" | sort -u
}

# clang-tidy needs a compile database entry per translation unit, so headers are not
# passed here: diagnostics inside headers still surface through the sources that include
# them (that is what .clang-tidy's HeaderFilterRegex is for).
ci_tidy_sources() {
    # shellcheck disable=SC2086
    eval "git ls-files --cached --others --exclude-standard -- $CI_SOURCE_GLOBS" \
        | grep -E '\.(cpp|cc|cxx)$' \
        | sort -u
}

# The comparable form of a clang-tidy finding — applied to BOTH sides of the baseline
# comparison, so the file a repo captures and the log this gate just wrote are the same
# shape:
#
#   <repo>/src/foo.cpp:42:7: warning: ...   ->   src/foo.cpp: warning: ...
#
#   * the repo root is stripped: clang-tidy reports the path it was handed by the compile
#     database, which CMake writes as an absolute path, so a baseline captured in one clone
#     names no finding in a checkout at another path (the nightly clean checkout, a
#     colleague's machine) and every inherited finding reads as new;
#   * :line:column is stripped, so the same finding after an unrelated edit above it is
#     still the same finding. This is line-blind on purpose, and the flip side is worth
#     knowing: a SECOND identical finding in a file that already has one collapses into the
#     first. Fix the baselined finding instead of growing the baseline.
#
# `tools/ci.sh --write-tidy-baseline` captures the baseline through this same function, so
# the documented way to accept findings cannot drift from the way they are compared.
tidy_key() {
    awk -v root="$REPO_ROOT/" '
        { i = index($0, root); if (i) $0 = substr($0, i + length(root)); print }' \
        | sed 's/:[0-9]*:[0-9]*:/:/'
}

run_tests() {  # run_tests <build-dir> <logfile>
    # shellcheck disable=SC2086
    eval "$CI_TEST_CMD" > "$2" 2>&1
}

mkdir -p "$CI_LOG_DIR"


# ---------------------------------------------------------------- stage helpers
cleanup() {
    local dir
    if [ "$CI_KEEP_TMP" != "1" ]; then
        for dir in "${RUN_TMP_DIRS[@]:-}"; do
            [ -n "$dir" ] && [ -d "$dir" ] && rm -rf "$dir"
        done
    fi
}

trap cleanup EXIT

# ---------------------------------------------------------------- verdict shims
# kit-ci calls a stage and reads its EXIT STATUS: 0 is a pass, non-zero is a failure, and
# the engine names the stage and prints the first lines of a failed stage's output itself
# (SPEC.md §4). The stage bodies in this directory were split verbatim out of the old
# tools/ci.sh and still speak that script's vocabulary, so it is defined here, once:
#
#   ci_pass <stage>                 END the stage, exit 0
#   ci_fail <stage> <reason> [log]  print why, show the log's tail, exit 1
#   ci_skip <stage> <reason>        say so; the caller then exits 0 (a SKIP is not a failure,
#                                   which is also what kit-ci's `when = "tool:<name>"` means)
#   ci_begin <title>                the old banner line
ci_begin() { printf '\n==> %s\n' "$1"; }
ci_pass() { exit 0; }
ci_skip() { printf '    SKIP: %s\n' "$2"; }
ci_fail() {
    printf 'FAILED: %s — %s\n' "$1" "$2" >&2
    [ -n "${3:-}" ] && show_log "$3"
    exit 1
}
show_log() {  # show_log <file> — the tail, for out-of-order output like a build log
    local file="${1:-}"
    if [ -n "$file" ] && [ -f "$file" ]; then
        printf -- '--- %s (last 25 lines) ---\n' "$file" >&2
        tail -n 25 "$file" | sed 's/^/      /' >&2
    fi
    return 0
}
