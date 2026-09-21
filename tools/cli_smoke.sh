#!/usr/bin/env bash
# CLI smoke tests for the `jsonfuzz` binary (brief §6). Every documented verb and flag is
# asserted here, including every exit code (0 ok, 1 an oracle violation, 2 usage error, 3
# no such SUT) and that an UNKNOWN flag FAILS — JSOM once shipped a CLI that swallowed
# unknown options and exited 0, so a flag nobody documented must fail loudly.
#
# Usage: tools/cli_smoke.sh [path-to-jsonfuzz]     (default: build/jsonfuzz)
set -u

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
JSONFUZZ=${1:-$REPO_ROOT/build/jsonfuzz}

pass=0
fail=0
TMP=$(mktemp -d "${TMPDIR:-/tmp}/jsonfuzz-cli-smoke-XXXXXX")
trap 'rm -rf "$TMP"' EXIT

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

# --- identity and help ------------------------------------------------------
check        "--version exits 0"                0 "$JSONFUZZ" --version
expect_output "--version prints the exact string" '^JSONFuzz 0\.1\.0$' "$JSONFUZZ" --version
check        "--help exits 0"                   0 "$JSONFUZZ" --help
expect_output "--help names the binary"         'JSONFuzz' "$JSONFUZZ" --help
check        "no arguments is a usage error"    2 "$JSONFUZZ"
check        "unknown flag FAILS (exit 2)"      2 "$JSONFUZZ" --bogus
check        "unknown verb FAILS (exit 2)"      2 "$JSONFUZZ" frobnicate

# --- listings ---------------------------------------------------------------
check        "--list-ops exits 0"               0 "$JSONFUZZ" --list-ops
expect_output "--list-ops names an operator"     'swap-members' "$JSONFUZZ" --list-ops
check        "--list-suts exits 0"              0 "$JSONFUZZ" --list-suts
expect_output "--list-suts names the toy"       '^toy$' "$JSONFUZZ" --list-suts

# --- gen --------------------------------------------------------------------
check        "gen exits 0"                      0 "$JSONFUZZ" gen --seed 1
expect_output "gen emits a document"            '.' "$JSONFUZZ" gen --seed 1
check        "gen is deterministic"             0 "$JSONFUZZ" gen --seed 1
[ "$("$JSONFUZZ" gen --seed 1)" = "$("$JSONFUZZ" gen --seed 1)" ] \
    && pass=$((pass + 1)) || { fail=$((fail + 1)); printf '      FAIL gen is deterministic\n'; }
check        "gen unknown flag FAILS (exit 2)"  2 "$JSONFUZZ" gen --nope
"$JSONFUZZ" gen --seed 2 --intent "$TMP/intent.json" >/dev/null 2>&1
if [ -s "$TMP/intent.json" ] && grep -q object_count "$TMP/intent.json"; then
    pass=$((pass + 1))
else
    fail=$((fail + 1)); printf '      FAIL gen --intent wrote no sidecar\n'
fi

# --- mutate -----------------------------------------------------------------
printf '{"a":1}' > "$TMP/in.json"
check        "mutate exits 0"                   0 "$JSONFUZZ" mutate --in "$TMP/in.json" --op wrap-in-array --seed 1
expect_output "mutate applies the operator"     '^\[\{"a":1\}\]$' "$JSONFUZZ" mutate --in "$TMP/in.json" --op wrap-in-array --seed 1
check        "mutate unknown op FAILS (exit 2)" 2 "$JSONFUZZ" mutate --in "$TMP/in.json" --op nope --seed 1
check        "mutate missing --in FAILS (exit 2)" 2 "$JSONFUZZ" mutate --op wrap-in-array --seed 1

# --- check ------------------------------------------------------------------
printf '{"a":1,"b":[true,null],"c~d":2}' > "$TMP/ok.json"
printf 'not json' > "$TMP/bad.json"
printf '{"a":1}' > "$TMP/obj.json"
check        "check toy on valid json exits 0"  0 "$JSONFUZZ" check --sut toy --in "$TMP/ok.json"
check        "check toy on invalid json exits 0" 0 "$JSONFUZZ" check --sut toy --in "$TMP/bad.json"
check        "check unknown SUT exits 3"        3 "$JSONFUZZ" check --sut no-such --in "$TMP/ok.json"
check        "check broken SUT exits 1"        1 "$JSONFUZZ" check --sut broken-comma --in "$TMP/obj.json"
expect_output "check broken SUT names the law"  'O1' "$JSONFUZZ" check --sut broken-comma --in "$TMP/obj.json"
check        "check missing --in FAILS (exit 2)" 2 "$JSONFUZZ" check --sut toy

printf '      %s checks passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
