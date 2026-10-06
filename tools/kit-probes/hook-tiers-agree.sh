#!/usr/bin/env bash
# guards: templates/hooks/pre-commit
# guards: templates/hooks/pre-push
# guards: templates/cpp/ci.sh
#
# Kit-conformance probe for the tier fix of 2026-10-06 (found when an unformatted commit passed
# pre-commit and only a hand run caught it).
#
#   probes/hook-tiers-agree.sh <gate-script> [repo-root]
#
# THE DEFECT, measured. The two tiers were written in four places — the gate's CI_DEFAULT_STAGES,
# the gate's header comment, PLUNK-IN.md's table, and each hook's own argument list — so they could
# disagree, and did. In FSMTable on 2026-10-06 the gate's list had gained `fuzz` (a stage its SPEC
# requires) and `lint`; the installed pre-push hook still named the kit's original eleven, so every
# push ran one stage fewer than a hand run and the stage it skipped was the fuzzer. The same day the
# pre-commit hook — `build tests`, the kit's fast tier — passed a commit whose files clang-format
# would have rewritten, because `format` sat in the minutes-tier. Both are the failure the kit's own
# comment warns about ("a stage that exists but never runs is a decoration"), and neither was visible
# to any check.
#
# The fix moves both lists into the gate as CI_FAST_STAGES and CI_FULL_STAGES and has the hooks pass
# the word `fast` or `full`. Four things can still go wrong, and this probe checks each:
#
#   G1  the gate declares both tiers                        (else the lists live somewhere else again)
#   G2  each hook names a tier and no hook names a stage    (the drift itself)
#   G3  every `stage_*` the gate defines is in the full tier (a stage that never runs)
#   G4  the fast tier is a subset of the full one            (a stage no hook can reach)
#   G5  the lists printed in the gate's header comment — and in PLUNK-IN.md when this is the kit —
#       equal the variables                                  (documentation that cannot drift)
#
# Limits, as with every probe: G1, G3 and G5 read the source as text rather than by running it, so an
# assignment written in an unusual shape is a false negative. That is why G3 counts what it examined
# and fails on an empty set instead of passing vacuously, and why G2 requires both hooks to be there:
# a hook that moved is a hook that stopped guarding, and "nothing to check" must not read as green.
#
# Exit 0 = PROBE VERIFIED, 1 = PROBE FAILED.
set -uo pipefail

S=${1:?usage: hook-tiers-agree.sh <gate-script|hook> [repo-root]}
[ -f "$S" ] || { echo "PROBE FAILED (no such script: $S)"; exit 1; }
R=${2:-}
# The kit's runner passes the file a probe GUARDS, which for this probe may be a hook; a repo's
# `kitprobes` stage passes the gate. Take either: if the argument is not the gate, the gate is the
# one beside it (kit layout first, then repo layout, then by walking up from the hook).
hook_only=""
if [ "$(basename "$S")" != "ci.sh" ]; then
    hook_only=$S
    S=""
    for cand in "${R:+$R/tools/ci.sh}" "${R:+$R/templates/cpp/ci.sh}"; do
        [ -n "$cand" ] && [ -f "$cand" ] && { S=$cand; break; }
    done
    if [ -z "$S" ]; then
        d=$(dirname "$hook_only")
        while [ "$d" != "/" ] && [ "$d" != "." ]; do
            for cand in "$d/tools/ci.sh" "$d/templates/cpp/ci.sh"; do
                [ -f "$cand" ] && { S=$cand; break 2; }
            done
            d=$(dirname "$d")
        done
    fi
    [ -n "$S" ] || { echo "PROBE FAILED (guarded file $hook_only: no gate script beside it)"; exit 1; }
fi
[ -n "${R:-}" ] || R=$(cd "$(dirname "$S")/.." && pwd)
[ -d "$R" ] || { echo "PROBE FAILED (no such repo root: $R)"; exit 1; }

fails=0
oks=0

check() { # check <rc> <description>
    if [ "$1" -eq 0 ]; then oks=$((oks + 1)); printf '  ok   %s\n' "$2"
    else fails=$((fails + 1)); printf '  FAIL %s\n' "$2"; fi
}

note_fail() {
    fails=$((fails + 1))
    printf '  FAIL %s\n' "$1"
}

# `a b c` -> ` a b c `, so membership is one case-pattern and not a word-splitting accident.
padded() { printf ' %s ' "$1"; }

printf '=== probe: the hook tiers agree with the gate (one list, two hooks)\n'
printf '    script: %s\n    repo:   %s\n' "$S" "$R"

# ---------------------------------------------------------------- G1: the tiers are declared
fast=$(sed -n 's/^CI_FAST_STAGES=${CI_FAST_STAGES:-"\(.*\)"}/\1/p' "$S" | head -1)
full=$(sed -n 's/^CI_FULL_STAGES=${CI_FULL_STAGES:-"\(.*\)"}/\1/p' "$S" | head -1)
full_from=CI_FULL_STAGES
# A repo may single-source the full tier as the DEFAULT list instead of naming a CI_FULL_STAGES —
# JSOM, jsonTools and UnicodeChecker do, and their pre-push passes no stage list at all, which is
# the same guarantee by a different spelling. What matters is that each tier has ONE definition in
# the gate; which variable holds it is not the contract. (Reading only CI_FULL_STAGES made this
# probe report those repos as having no tiers, which was this check's own input set failing, not
# their gates.)
if [ -z "${full:-}" ]; then
    full=$(sed -n 's/^CI_DEFAULT_STAGES=${CI_DEFAULT_STAGES:-"\(.*\)"}/\1/p' "$S" | head -1)
    full_from=CI_DEFAULT_STAGES
fi
if [ -z "${fast:-}" ] || [ -z "${full:-}" ]; then
    note_fail "the gate does not declare both tiers — expected CI_FAST_STAGES and a full list (CI_FULL_STAGES or CI_DEFAULT_STAGES)"
    printf '       (fast: %s / full: %s) — with the lists elsewhere, a hook has to repeat them\n' \
        "${fast:-none}" "${full:-none}"
else
    check 0 "the gate declares both tiers (fast: $fast | full: $full — from $full_from)"
fi

# ---------------------------------------------------------------- G2: the hooks name a tier
hooks_seen=0
stages_in_hooks=0
# Which hooks this copy carries: the repo layout first, the kit's template layout second. When the
# kit's runner pointed the probe at one guarded hook, that is the hook this run is about.
hooks=()
if [ -n "$hook_only" ]; then
    hooks=("$hook_only")
else
    for h in pre-commit pre-push; do
        for d in "$R/.githooks" "$R/templates/hooks"; do
            [ -f "$d/$h" ] && { hooks+=("$d/$h"); break; }
        done
    done
fi
for hook in ${hooks[@]+"${hooks[@]}"}; do
    [ -f "$hook" ] || continue
    hooks_seen=$((hooks_seen + 1))
    name=$(basename "$hook")
    # Only the line that executes the gate, and only its arguments: a comment may name stages, and
    # `[ -x "$root/tools/ci.sh" ]` is a test, not an invocation.
    # Strip the quotes around the gate's own path, and any line continuation, so what is left is
    # exactly the arguments: the tier word and flags, or — the defect — a list of stage names.
    # The whole exec statement, continuations included: a hook may spread its arguments over
    # several lines with backslashes, and reading only the first line would call that "no gate".
    args=$(awk '/^[[:space:]]*exec[[:space:]].*ci\.sh/ { capture = 1 } capture { print $0; if ($0 !~ /\\[[:space:]]*$/) exit }' "$hook" |
        sed 's/.*ci\.sh//' | tr -d '"' | tr '\\' ' ' | tr -s ' \t' '\n\n\n' | grep -v '^$' || true)
    bad=$(printf '%s\n' "${args:-}" | grep -vE '^(fast|full|--[a-z-]+)$' || true)
    if [ -z "$(printf '%s' "${args:-}" | tr -d '[:space:]')" ]; then
        note_fail "$name never executes the gate — a hook that cannot run the gate is not a tier"
    elif [ -n "${bad:-}" ]; then
        stages_in_hooks=$((stages_in_hooks + 1))
        note_fail "$name passes the gate a stage name: $bad"
        printf '       a hook names the tier (fast | full); the stages live in the gate, once\n'
    else
        check 0 "$name names a tier ($(printf '%s' "$args" | tr '\n' ' ' | sed 's/ *$//'))"
    fi
done
if [ -z "$hook_only" ] && [ "$hooks_seen" -ne 2 ]; then
    note_fail "found $hooks_seen of 2 hooks — an unarmed gate is worse than none, because it looks armed"
fi

# ---------------------------------------------------------------- G3/G4: the tiers cover the stages
stage_total=0
decorations=0
# The stage set comes from the gate's own `--list`, not from a pattern over the source: the set of
# things a gate can be asked for is the gate's answer, whether it dispatches through a case table,
# a `stage_$1` call, or something else. Reading function names would also sweep up helpers — FSMgine's
# banner helper is literally called stage_banner.
listout=$("$S" --list 2>/dev/null)
listed=$(printf '%s\n' "$listout" | sed -n 's/^stages:[[:space:]]*//p')
how="--list"
if [ -z "${listed:-}" ]; then
    listed=$(grep -oE '^[[:space:]]+[a-z0-9_]+\)[[:space:]]+stage_[a-z0-9_]+[[:space:]]*;;' "$S" |
        sed 's/^[[:space:]]*//; s/).*//' | sort -u | tr '\n' ' ')
    how="the dispatch table"
fi
if [ -z "${listed:-}" ]; then
    note_fail "cannot learn the stage set (neither a 'stages:' line from --list nor a dispatch table) — this check would examine an empty set"
fi
for stage in ${listed:-}; do
    stage_total=$((stage_total + 1))
    body=$(awk -v fn="^stage_${stage}\\(\\) \\{" '$0 ~ fn { inside = 1 } inside { print } inside && /^}/ { exit }' "$S")
    # A forwarding alias is a stage whose whole body is one call to another stage — `lint` naming
    # `tidy`, in the kit's own template. Detected by that shape rather than by a helper's name:
    # keying on `ci_begin` once marked six real stages in FSMgine as aliases, which quietly shrank
    # this check's input set to nothing. Comments do not count as body.
    inner=$(printf '%s\n' "$body" | sed '1d; $d' | grep -vE '^[[:space:]]*(#|$)' || true)
    inner_lines=$(printf '%s\n' "$inner" | grep -c . || true)
    if [ "${inner_lines:-0}" -eq 1 ] &&
        printf '%s\n' "$inner" | grep -qE '^[[:space:]]*stage_[a-z0-9_]+[[:space:]]+"\$@"[[:space:]]*$'; then
        printf '    (skipped stage_%s: a forwarding alias, not a stage of its own)\n' "$stage"
        continue
    fi
    case "$(padded "${full:-}")" in
        *" $stage "*) : ;;
        *)
            # ...unless the gate itself says the stage is opt-in. JSOM's --list prints
            # "(opt-in, not in the default set: coverage )" for a stage its own comment calls
            # "informational, never a gate" — that is a decision the gate STATES, not a stage that
            # fell out of a list. A stage in no tier that says nothing about being opt-in is still a
            # failure, which is what this check is for.
            if printf '%s\n' "$listout" | grep -i 'opt-in' | grep -qw "$stage"; then
                printf '    (skipped stage_%s: in no tier, and the gate declares it opt-in)\n' "$stage"
            else
                decorations=$((decorations + 1))
                note_fail "the gate defines stage_$stage and $full_from never runs it — a stage that exists but never runs is a decoration"
            fi
            ;;
    esac
done
if [ "$stage_total" -eq 0 ]; then
    note_fail "no dispatched stage_* functions found in $S — this check examined an empty set and proves nothing"
elif [ "$decorations" -eq 0 ]; then
    # Only when nothing was reported: an "ok" next to a failure reads as a contradiction.
    check 0 "every stage the gate defines is reachable from the full tier ($stage_total defined)"
fi

if [ -n "${fast:-}" ] && [ -n "${full:-}" ]; then
    loose=""
    for stage in $fast; do
        case "$(padded "$full")" in
            *" $stage "*) : ;;
            *) loose="$loose $stage" ;;
        esac
    done
    if [ -n "$loose" ]; then
        note_fail "the fast tier stages a list the full tier does not run:$loose — no hook could reach them"
    else
        check 0 "the fast tier is a subset of the full one (${#fast} characters of list, $(printf '%s' "$fast" | wc -w) stage(s))"
    fi
fi

# ---------------------------------------------------------------- G5: the printed lists equal the variables
doc_fast=$(grep -E '^#   fast[[:space:]]+\(pre-commit\)' "$S" | head -1 | sed 's/^#   fast[[:space:]]*(pre-commit)[[:space:]]*//' | sed 's/^--[a-z-]* //; s/[[:space:]]*$//')
doc_full=$(grep -A 1 -E '^#   full[[:space:]]+\(pre-push\)' "$S" | sed 's/^#   full[[:space:]]*(pre-push)[[:space:]]*//; s/^#[[:space:]]*//; s/[[:space:]]*$//' | tr '\n' ' ' | sed 's/[[:space:]]*$//; s/^--[a-z-]* //')
if [ -z "${doc_fast:-}" ] || [ -z "${doc_full:-}" ]; then
    note_fail "the gate's header comment no longer prints the two tiers — the block this probe compares is gone"
else
    if [ "$doc_fast" = "${fast:-}" ]; then
        check 0 "the gate's comment prints the fast tier ($doc_fast)"
    else
        note_fail "the gate's comment prints the fast tier as '$doc_fast' and the variable says '${fast:-}'"
    fi
    if [ "$doc_full" = "${full:-}" ]; then
        check 0 "the gate's comment prints the full tier"
    else
        note_fail "the gate's comment prints the full tier as '$doc_full' and the variable says '${full:-}'"
    fi
fi

# PLUNK-IN.md is the kit's own document; a plunked repo does not carry it, so its absence is not a
# failure — but when it is there, its table is a claim and is compared like the comment above.
if [ -f "$R/PLUNK-IN.md" ]; then
    t_fast=$(grep -E '^\| fast \|' "$R/PLUNK-IN.md" | head -1 | awk -F'|' '{ gsub(/^[ \t]+|[ \t]+$/, "", $4); print $4 }')
    t_full=$(grep -E '^\| full \|' "$R/PLUNK-IN.md" | head -1 | awk -F'|' '{ gsub(/^[ \t]+|[ \t]+$/, "", $4); print $4 }')
    # The cell is markdown: backticks first, then the flag the hook passes rather than the gate's list.
    t_fast=$(printf '%s' "$t_fast" | sed 's/`//g; s/^[[:space:]]*//; s/[[:space:]]*$//')
    t_full=$(printf '%s' "$t_full" | sed 's/`//g; s/^[[:space:]]*//; s/[[:space:]]*$//' | sed 's/^--[a-z-]* //')
    if [ "$t_fast" = "${fast:-}" ] && [ "$t_full" = "${full:-}" ]; then
        check 0 "PLUNK-IN.md's table prints both tiers"
    else
        note_fail "PLUNK-IN.md's table says fast='$t_fast' full='$t_full', the gate says fast='${fast:-}' full='${full:-}'"
    fi
else
    printf '    (no PLUNK-IN.md here: the kit document is not part of a plunked repo)\n'
fi

printf 'PROBE %s (%d ok, %d failed)\n' \
    "$([ "$fails" -eq 0 ] && echo VERIFIED || echo FAILED)" "$oks" "$fails"
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
