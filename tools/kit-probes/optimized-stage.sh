#!/usr/bin/env bash
# guards: templates/cpp/ci.sh
#
# Kit-conformance probe for the optimized-build-stage kit fix (card t_5c2a8ab2).
#
#   probes/optimized-stage.sh <gate-script> [repo-root]
#
# THE GAP, measured. Every stage of templates/cpp/ci.sh built CI_BUILD_TYPE=Debug and Debug is
# the kit's own default, so a copy of this gate could be green while the configuration the
# repo's own pipeline builds was never configured, built or tested. Computo (2026-09-20, card
# t_45a28893): the Pages workflow passed no build type and a FetchContent'd subproject
# defaulted CMAKE_BUILD_TYPE to Release, so CI built -O3 -DNDEBUG — a gcc 14
# -Wmaybe-uninitialized false positive redded the deploy for twelve days and ./build.sh
# (Release) had been broken on the developer's own box the whole way, behind a GREEN local
# gate. The same shape reaches any repo whose second configuration is built by hand or by a
# workflow: the gate and the pipeline build different things, and only one of them is checked.
#
# The fix this probe guards: a stage that builds the SECOND, non-Debug configuration, holds its
# own log to the `warning:` rule the `build` stage uses, runs the same test command in its own
# build dir, sits in the tier that runs by default, and whose build dir the tree stage audits.
# Checked by name and by behaviour — no kit checkout, no network, no build, under a second.
#
# R1  a `stage_release` exists
# R2  it configures a build type that is NOT Debug (a "release" stage building Debug is the
#     false fix: it would run the same configuration twice and prove nothing)
# R3  it holds its OWN log to the `warning:` rule and fails on findings
# R4  it runs the test command in its own build dir
# R5  it is in CI_DEFAULT_STAGES (a stage that exists but never runs is a decoration)
# R6  the tree stage knows its build dir (the gate's own footprint must stay ignored)
#
# Limits, honestly: R5 checks the default list, not a repo's .githooks/pre-push (which spells
# its tier out — read the hook when porting). R2 resolves the knob's DEFAULT from the script;
# a Debug value pushed in at run time through .ci.env is beyond any text probe. And as with
# every probe, this is name- and contract-level: a semantic regression inside a stage that is
# still present is not caught — that costs a real build, which is what the port does by hand.
#
# Exit 0 = PROBE VERIFIED, 1 = PROBE FAILED.
set -uo pipefail

S=${1:?usage: optimized-stage.sh <gate-script> [repo-root]}
[ -f "$S" ] || { echo "PROBE FAILED (no such script: $S)"; exit 1; }

fails=0
oks=0

check() {  # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

# The body of a shell function: from its `name() {` line to the next `}` in column 0.
fn_body() {  # fn_body <function-name>
    awk -v fn="$1" '
        $0 ~ "^" fn "\\(\\)[[:space:]]*\\{" { f = 1 }
        f { print }
        f && /^\}/ { exit }' "$S"
}

printf '=== probe: optimized-build stage (a second, non-Debug configuration is built and checked)\n'
printf '    script: %s\n' "$S"

# R1 — the stage exists.
if grep -qE '^stage_release\(\)[[:space:]]*\{' "$S"; then
    check 0 "a stage_release() exists"
    body=$(fn_body stage_release)
else
    check 1 "a stage_release() exists — nothing in this gate builds an optimized configuration"
    body=""
fi

# R2 — the configuration it builds is not Debug. Resolve the build type the way the shell would
# see it: a literal, or a knob whose default is read out of this script.
cfg=$(printf '%s\n' "$body" | grep -oE -- '-DCMAKE_BUILD_TYPE=[^[:space:]]+' | head -1)
if [ -z "$cfg" ]; then
    check 1 "its configure passes a build type (no -DCMAKE_BUILD_TYPE= in the stage)"
    build_type=""
elif printf '%s' "$cfg" | grep -qE '\$\{?CI_'; then
    knob=$(printf '%s' "$cfg" | sed -E 's/.*\$\{?([A-Za-z_][A-Za-z0-9_]*).*/\1/')
    defline=$(grep -E "^${knob}=" "$S" | head -1)
    build_type=$(printf '%s' "$defline" | sed -E 's/^[A-Za-z_][A-Za-z0-9_]*=\$\{[A-Za-z_][A-Za-z0-9_]*:-([^}]*)\}.*$/\1/; s/^[A-Za-z_][A-Za-z0-9_]*=([^${}]*).*$/\1/')
    if [ -z "$build_type" ]; then
        check 1 "the knob ${knob} has a readable default in this script (found: '${defline}')"
    else
        check 0 "the build type comes from ${knob} (default: ${build_type})"
    fi
else
    build_type=$(printf '%s' "$cfg" | sed -E 's/^[^=]*=//; s/"//g')
    check 0 "the build type is stated literally in the configure line"
fi
if [ -n "$build_type" ] && [ "$build_type" != "Debug" ]; then
    check 0 "that configuration is not Debug (${build_type}) — the Debug stages still cover Debug"
else
    check 1 "that configuration must NOT be Debug (got '${build_type:-none}'): a second Debug build proves nothing"
fi

# R3 — its own log is held to the `warning:` rule.
printf '%s' "$body" | grep -qF "'warning:' \"\$CI_LOG_DIR/release-build.log\"" \
    && printf '%s' "$body" | grep -qF 'ci_fail release'
check $? "it counts \`warning:\` in its own build log and fails on findings"

# R4 — the same test command, in that build dir.
printf '%s' "$body" | grep -qF 'run_tests "$CI_RELEASE_BUILD_DIR"' \
    && printf '%s' "$body" | grep -qF 's|\$CI_BUILD_DIR|$CI_RELEASE_BUILD_DIR|'
check $? "it runs the test command in its own build dir (the suite runs optimized too)"

# R5 — it is in the tier that runs.
if grep -E '^CI_DEFAULT_STAGES=' "$S" | grep -qw 'release'; then
    check 0 "release is in CI_DEFAULT_STAGES — the stage actually runs"
else
    check 1 "release is NOT in CI_DEFAULT_STAGES — the stage exists but never runs"
fi

# R6 — the tree stage knows its build dir.
if printf '%s\n' "$(fn_body stage_tree)" | grep -q 'CI_RELEASE_BUILD_DIR'; then
    check 0 "the tree stage creates/audits the release build dir (the gate footprint stays ignored)"
else
    check 1 "the tree stage does not know the release build dir — its own output would fail the next run"
fi

printf 'PROBE %s (%d ok, %d failed)\n' \
    "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
