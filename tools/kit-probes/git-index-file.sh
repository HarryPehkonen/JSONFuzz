#!/usr/bin/env bash
# guards: templates/cpp/ci.sh
# guards: templates/deno/gate.sh
# guards: templates/python/gate.sh
#
# Kit-conformance probe for the 2026-09-20 fix: a gate must not let git's TEMPORARY
# index reach the commands it runs.
#
#   probes/git-index-file.sh <gate-script> [repo-root]
#
# THE DEFECT, measured (Computo, card t_9541aa62; the kit copy is card t_0a9a0018).
# `git commit -- <path>` builds a temporary index and exports its path to the pre-commit
# hook as GIT_INDEX_FILE (`<repo>/.git/next-index-XXXXXX.lock`). Everything the hook
# starts inherits it, so any `git` command the gate runs inside ANOTHER repository reads
# THIS repo's index entries against that repository's object store and dies on the first
# blob it does not have:
#
#   fatal: unable to read 691e2bdafaf312970644391de042d38c2c5972d8
#   CMake Error at .../jsom-populate-gitupdate.cmake:186 (message): Failed to get the status
#
# — a pathspec commit failing its OWN gate at `build`, naming a dependency update, on a
# tree that builds fine. The payload is not exotic: cmake's FetchContent update step is
# `git --git-dir=.git status --porcelain` in the clone; a `pip install git+https://…`
# requirement, a probe that clones into a temp dir, or a repo's own kitprobes script all
# reach it the same way. The fix is ONE line in the gate's prologue — `unset
# GIT_INDEX_FILE`, beside `export NO_COLOR=1` — because the variable is inherited by every
# process the gate starts, so `env -u` on individual commands would cover the instance and
# leave the class.
#
# WHAT THIS PROBE CHECKS. The fix is a property of the gate's own environment block, and
# the only honest way to check it cheaply is to run that block's bytes in the environment
# that actually carries the variable:
#
#   P1  the gate unsets GIT_INDEX_FILE at top level, in its prologue (before its first
#       function definition — the region every caller gets before any stage runs)
#   P2  sourcing the gate's own export/unset lines leaves the variable unset when it
#       arrives set (the bytes behave, not just the spelling)
#   P3  the mechanism is live here: with the variable pointing at one repo's index, a
#       `git status` in a DIFFERENT repo dies — so P4/P5 are testing something real
#   P4  a REAL `git commit -- <path>` in a throwaway repo, whose hook runs the gate's own
#       environment block and then that cross-repo `git status`, PASSES — and the hook
#       testifies that it really did inherit the temporary index (no vacuous pass)
#   P5  the same commit against the SAME bytes with the `unset GIT_INDEX_FILE` line
#       removed FAILS. This is the check that gives P4 teeth: a probe that cannot show the
#       defect when the fix is absent is a decoration.
#
# WHY NOT "run the gate's tree/format stages both ways and diff their output" (the shape
# the Computo card suggested): it was tried, and it passes vacuously. Measured on a real
# pathspec commit in a throwaway clone, with the pre-fix gate: the tree and format stage
# OUTPUTS are byte-identical with the inherited temporary index and with the real one, and
# so are every input they read — the only views that differ (`git diff --cached HEAD`, and
# the staged-vs-unstaged letter in `git status`) are read by no stage. The defect needs a
# git command in a different repository to be visible at all, which is why the payload
# here is one.
#
# LIMITS, stated rather than hidden: this checks the gate's environment block, not the
# whole file, so a copy that unsets the variable somewhere else (inside a stage function,
# or below its first function definition) fails P1 while still being "safe by accident" —
# the kit's contract is the prologue, because that is the one place every caller
# (by hand, both hooks, the nightly checkout) reaches. A copy that re-creates the variable
# LATER in the file is not caught either: no stage of the gate is executed here, because
# running one honestly costs a build. It says nothing about whether the gate's own commands
# are otherwise correct. It needs no kit checkout, no network, no build, and no repo: three
# throwaway repos in a temp dir, under a second.
#
# Exit 0 = PROBE VERIFIED, 1 = PROBE FAILED.
set -uo pipefail

S=${1:?usage: git-index-file.sh <gate-script> [repo-root]}
[ -f "$S" ] || { printf 'PROBE FAILED (no such script: %s)\n' "$S"; exit 1; }
ROOT=${2:-$(cd "$(dirname "$S")/.." && pwd)}
fails=0
oks=0

check() {  # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/probe-git-index-file.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

printf '=== probe: GIT_INDEX_FILE must not reach the gate (a pathspec commit runs its own hook)\n'
printf '    script: %s\n    root:   %s\n' "$S" "$ROOT"

# --- P1: the prologue unsets it, at top level, before the first function definition ----
prologue_end=$(awk '/^[A-Za-z_][A-Za-z0-9_]*\(\)[[:space:]]*\{/ {print NR; exit}' "$S")
[ -n "$prologue_end" ] || prologue_end=$(( $(wc -l < "$S") + 1 ))
found_line=$(awk -v end="$prologue_end" 'NR < end && /^unset([[:space:]]+-[a-zA-Z]+)*[[:space:]]+GIT_INDEX_FILE([[:space:]]|$)/ {print NR; exit}' "$S")
if [ -n "$found_line" ]; then
    check 0 "the prologue unsets GIT_INDEX_FILE (line $found_line, before the first function)"
else
    check 1 "the prologue does not unset GIT_INDEX_FILE: a pathspec commit hands git's temporary index to every process the gate runs"
fi

# --- the gate's own environment block: its top-level export/unset lines -----------------
ENV_FIXED=$TMP/gate-env-fixed.sh
ENV_MUTANT=$TMP/gate-env-mutant.sh
awk -v end="$prologue_end" 'NR < end' "$S" | grep -E '^(export|unset)[[:space:]]' > "$ENV_FIXED" || true
grep -vE '^unset([[:space:]]+-[a-zA-Z]+)*[[:space:]]+GIT_INDEX_FILE([[:space:]]|$)' "$ENV_FIXED" > "$ENV_MUTANT" || true

# --- P2: the bytes behave — source them with the variable set, and it must be gone ------
arrived=$(env GIT_INDEX_FILE=/nonexistent/arrived.index bash -c \
    '. "$1"; printf "%s" "${GIT_INDEX_FILE:-}"' _ "$ENV_FIXED" 2>/dev/null || printf 'SOURCE-ERROR')
if [ -z "$arrived" ] && [ "$arrived" != "SOURCE-ERROR" ]; then
    check 0 "sourcing the gate's environment block clears an inherited GIT_INDEX_FILE"
else
    check 1 "sourcing the gate's environment block leaves GIT_INDEX_FILE='$arrived' (it must be unset by the block itself)"
fi

# --- two throwaway repos: 'this' one and the 'other' one the payload runs git in --------
THIS=$TMP/this-repo
OTHER=$TMP/other-repo
new_repo() {
    git init -q --initial-branch=main "$1" || return 1
    git -C "$1" config user.email probe@example.invalid
    git -C "$1" config user.name probe
}
new_repo "$THIS" && new_repo "$OTHER" || { printf 'PROBE FAILED (could not create the throwaway repos)\n'; exit 1; }
printf 'this repo, %s\n' "$(date +%s%N)" > "$THIS/alpha-only-here.txt"
printf 'other repo, %s\n' "$(date +%s%N)" > "$OTHER/beta-only-here.txt"
git -C "$THIS" add -A >/dev/null && git -C "$THIS" commit -qm init
git -C "$OTHER" add -A >/dev/null && git -C "$OTHER" commit -qm init

# --- P3: the mechanism is live: a cross-repo git command under a foreign index dies ----
payload() { git --no-optional-locks -c status.renames=true -C "$1" status --porcelain; }
GIT_INDEX_FILE=$THIS/.git/index payload "$OTHER" >/dev/null 2>"$TMP/handmade.txt"
handmade_rc=$?
printf '    by hand: GIT_INDEX_FILE=<this-repo>/.git/index git -C <other-repo> status --porcelain\n'
printf '             rc=%s  %s\n' "$handmade_rc" "$(head -1 "$TMP/handmade.txt")"
if [ "$handmade_rc" -ne 0 ]; then
    check 0 "the leak is reproducible here (rc=$handmade_rc): the payload dies without the fix"
else
    check 1 "the leak is NOT reproducible in this environment (rc=0) — P4/P5 would prove nothing"
fi

# --- P4/P5: a real pathspec commit whose hook runs the gate's block + the payload -------
# The hook is identical in both runs; only the environment block it sources differs.
run_pathspec_commit() {  # run_pathspec_commit <envblock> <hook-dir> <capture-file>
    local envblock=$1 hookdir=$2 capture=$3
    mkdir -p "$hookdir"
    cat > "$hookdir/pre-commit" <<'HOOK'
#!/usr/bin/env bash
printf '%s' "${GIT_INDEX_FILE:-}" > "$PROBE_CAPTURE"
[ -n "${GIT_INDEX_FILE:-}" ] || { printf 'probe hook: GIT_INDEX_FILE is NOT set — the test would be vacuous\n' >&2; exit 91; }
# shellcheck disable=SC1090
. "$PROBE_ENVBLOCK"
git --no-optional-locks -c status.renames=true -C "$PROBE_OTHER_REPO" status --porcelain
HOOK
    chmod +x "$hookdir/pre-commit"
    printf 'more of this repo, %s\n' "$(date +%s%N)" >> "$THIS/alpha-only-here.txt"
    PROBE_CAPTURE=$capture PROBE_ENVBLOCK=$envblock PROBE_OTHER_REPO=$OTHER \
        git -C "$THIS" -c core.hooksPath="$hookdir" -c commit.gpgsign=false \
        commit -q -m "probe: a pathspec commit on a repo whose git env is not the gate's" \
        -- alpha-only-here.txt >"$capture.log" 2>&1
    return $?
}

run_pathspec_commit "$ENV_FIXED" "$TMP/hook-fixed" "$TMP/capture-fixed.txt"
fixed_rc=$?
leak_seen=$(cat "$TMP/capture-fixed.txt" 2>/dev/null || true)
printf '    pathspec commit, gate env block: rc=%s, hook saw GIT_INDEX_FILE=%s\n' \
    "$fixed_rc" "${leak_seen:-<unset>}"
if [ "$fixed_rc" -eq 0 ] && [ -n "$leak_seen" ]; then
    check 0 "a real pathspec commit passes its own hook with the gate's environment block (it inherited $leak_seen)"
elif [ -z "$leak_seen" ]; then
    check 1 "the hook never saw GIT_INDEX_FILE (rc=$fixed_rc) — this run proves nothing about the fix"
else
    check 1 "a real pathspec commit FAILED (rc=$fixed_rc) with the gate's environment block — the fix is not effective"
fi

run_pathspec_commit "$ENV_MUTANT" "$TMP/hook-mutant" "$TMP/capture-mutant.txt"
mutant_rc=$?
mutant_leak=$(cat "$TMP/capture-mutant.txt" 2>/dev/null || true)
printf '    pathspec commit, same bytes minus the unset: rc=%s, hook saw GIT_INDEX_FILE=%s\n' \
    "$mutant_rc" "${mutant_leak:-<unset>}"
printf '      %s\n' "$(head -1 "$TMP/capture-mutant.txt.log" 2>/dev/null)"
if [ "$mutant_rc" -ne 0 ] && [ -n "$mutant_leak" ]; then
    check 0 "with the unset line removed the same commit is refused (rc=$mutant_rc) — the check has teeth"
else
    check 1 "with the unset line removed the commit still succeeded (rc=$mutant_rc) — this probe is not testing the fix"
fi

printf 'PROBE %s (%d ok, %d failed)\n' \
    "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
