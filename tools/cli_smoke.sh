#!/usr/bin/env bash
# CLI smoke tests for the `jsonfuzz` binary. Phase A documents exactly two flags
# (--version, --help); every check below is a claim the CLI makes in its own help text.
# The unknown-flag check is load-bearing: JSOM once shipped a CLI that swallowed unknown
# options and exited 0, so a flag nobody documented must FAIL here.
#
# Usage: tools/cli_smoke.sh [path-to-jsonfuzz]     (default: build/jsonfuzz)
set -u

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
JSONFUZZ=${1:-$REPO_ROOT/build/jsonfuzz}

pass=0
fail=0

check() { # check <description> <expected-exit> <command...>
    local desc=$1 expected=$2
    shift 2
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" -eq "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '      FAIL %s: exit %s (expected %s)\n        %s\n' \
            "$desc" "$rc" "$expected" "$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
    fi
}

expect_output() { # expect_output <description> <pattern> <command...>
    local desc=$1 pattern=$2
    shift 2
    local out
    out=$("$@" 2>&1)
    if printf '%s' "$out" | grep -qE "$pattern"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '      FAIL %s: output did not match /%s/\n        %s\n' \
            "$desc" "$pattern" "$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
    fi
}

if [ ! -x "$JSONFUZZ" ]; then
    printf '      FAIL: no executable at %s\n' "$JSONFUZZ"
    exit 1
fi

check        "--version exits 0"                0 "$JSONFUZZ" --version
expect_output "--version prints the exact string" '^JSONFuzz 0\.1\.0$' "$JSONFUZZ" --version
check        "--help exits 0"                   0 "$JSONFUZZ" --help
expect_output "--help names the binary"         'JSONFuzz' "$JSONFUZZ" --help
check        "no arguments is a usage error"    2 "$JSONFUZZ"
check        "unknown flag FAILS (exit 2)"      2 "$JSONFUZZ" --bogus
check        "unknown flag FAILS (exit 2)"      2 "$JSONFUZZ" gen

printf '      %s checks passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
