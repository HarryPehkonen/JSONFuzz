# Coding Task Brief — JSONFuzz v0.1: a parser-agnostic, structure-aware JSON fuzzer (new repo, kit-wired)

You are creating a **new repository** at `/home/harri/hermes-workspace/JSONFuzz` (name:
`JSONFuzz`, C++17, CMake, GoogleTest, libFuzzer via clang only). Everything below is decided —
do not re-open the design; if something is genuinely impossible, say so in the report instead of
improvising a different design.

The owner: Harri Pehkonen. He does not want the world, he wants **step 1–3 of a ladder** that
works, is gated, and is honest about what it does not do. His standing rules (binding here):
TDD — a failing test first for every behaviour; a fuzz target from commit 1, kept building, run
for 10 s in the gate; no hosted CI, the gate is a script in the repo; **no document may quote a
test count** (describe coverage instead); license = **Unlicense, not MIT**; every deviation from
a template is written down with its reason.

## Why this repo exists (the measured case)

JSOM (`~/hermes-workspace/JSOM`, the owner's C++17 JSON library) is fuzzed today by
`tests/fuzzer.cpp`. Four holes were measured on 2026-09-20, all by reading and by experiment,
not by opinion:

| hole | measurement |
|---|---|
| reach | byte mutation of 5 committed seeds; no generator, so no coordinated structure (many keys, escapes, deep-but-legal nesting) is reachable except by luck |
| configurations | 4 documented parse options, 2 fuzzed; `allow_comments` and `convert_unicode_escapes` never appear in the harness |
| oracle | the round-trip property sits **inside** the broad `catch`: a sabotage that makes the compact serializer emit `{...,}` (invalid JSON) was **not found** (exit 0, 0 findings), while the same library + same seeds + the property moved outside the catch gave exit 77 and `ROUND-TRIP REPARSE FAILED` (artifact written). Control on a clean library: both clean |
| write path | `set_at`, `remove_at`, `extract_at`, `find_paths`, `at_multiple`, `count_paths` are never called by the harness |

Prior art to steal from, not to copy: `probelabs/json-fuzz` (Go, MIT, Jul 2026) — grammar-based
generator, 11 JSON-aware mutation operators, and a list of correctness gates; its commit message
names 7 real defects it found in `encoding/json`-adjacent code. Read its README before writing
the operator list, and name in our README which operators are borrowed, which are ours.

## Approved design — implement exactly this

### 1. The boundary: a parser-agnostic interface (`include/jsonfuzz/sut.hpp`)

Strings cross the boundary; no parser type is shared. Small on purpose:

```cpp
struct ParseConfig { bool allow_comments=false; bool unicode_escapes=false; bool strict_numbers=false; int max_depth=256; };
struct ParseResult { bool accepted; std::string error; };   // REJECTION IS AN OUTCOME, NOT A THROW

class SystemUnderTest {                  // one adapter per parser, lives in THAT parser's repo
 public:
  virtual ~SystemUnderTest() = default;
  virtual ParseResult parse(std::string_view text, const ParseConfig&) = 0;
  virtual std::string  serialize() const = 0;                        // canonical text of the last accepted parse
  virtual std::vector<std::string> pointers(int max_depth) const = 0; // RFC 6901 pointers into it
  virtual std::optional<std::string> get(std::string_view pointer) const = 0;  // that value, serialized
};
```

- JSOM is the FIRST adapter and it is written **in JSOM's repo**, in a later brief — JSONFuzz
  must never depend on JSOM, and must build and test with no adapter present.
- A registry picks an adapter by name for the CLI (`--sut <name>`); with none installed the CLI
  says so and exits 3.
- The interface has no `set`/`remove` in v0.1. The write-path oracles are v0.3; add the verbs
  then, with the oracles that need them (do not add unused API now).

### 2. `include/jsonfuzz/generator.hpp` — structure, from bytes, deterministically

- `Generated generate(const GenOptions&, ByteSource&)` where `ByteSource` is a bounded,
  deterministic byte reader (the same stream libFuzzer hands us). Same bytes ⇒ same document.
- `GenOptions`: `max_depth` (default 8), `max_members`, `max_string`, `nesting_bias`,
  `adversarial_weight`.
- `Generated` = the document TEXT plus an **intent record**: the kinds and member counts it
  decided to produce. The intent record is the oracle's independent truth for the
  fixed-point oracles — that is why no reference parser is needed for them.
- Adversarial alphabet (weighted, all of it reachable): `\u0000`, lone surrogates (`\ud800`,
  `\uDBFF`, `\uDC00`), `\uFFFF`, control characters, strings of length 0 and of `max_string`,
  duplicate keys, empty key, keys containing `/` and `~` (pointer escaping), numbers at the
  grammar boundary (`0`, `-0`, `1e309`, `1e-400`, a 20-digit integer, `-01`, `1.0.`, `2.e+3`),
  empty object/array, whitespace and newline variants, and nesting at exactly `max_depth`.
- It must be TOTAL: any byte stream produces a document. A generator that can fail is a fuzzer
  that reports its own bugs as findings.

### 3. `include/jsonfuzz/mutate.hpp` — JSON-aware mutation operators

- One entry point shaped for libFuzzer: `size_t mutate(uint8_t* data, size_t size, size_t max_size, unsigned seed)`
  → also usable directly (`MutateResult apply(std::string_view, Op op, uint64_t seed)`), and
  `enum class Op` with `name()` + `--list-ops` in the CLI, one test per operator.
- Operators (each documented in the header with the bug class it targets): truncate at a value /
  key / structure / escape boundary; delete a member or element; duplicate a subtree; wrap in an
  array or an object; change a value's kind; break a number's grammar; replace a string with an
  adversarial escape (lone surrogate, invalid UTF-8, raw control char); unbalance brackets;
  mismatch a closer; inject invalid UTF-8; swap two members.
- Internally the operators may use nlohmann/json (FetchContent, pinned tag) as the toolkit's own
  model. **Do not write a JSON parser in this repo.** The model is ours, the SUT is theirs.
- Mutating an input that does not parse must return the input unchanged (never throw).

### 4. `include/jsonfuzz/oracle.hpp` — v0.1 has exactly TWO laws

Given a `SystemUnderTest` and an input text:

- **O1 fixed point / round trip.** If `parse(x)` is accepted, then `serialize()` must reparse
  (`accepted`) and `serialize()` of that reparse must be **byte-identical**. This one law is the
  round trip and idempotence at once, and it is the law whose violation JSOM's harness cannot
  see today (see the table above).
- **O2 pointer contract.** Every pointer string the SUT returns from `pointers(d)` must be
  non-empty, must be returned by `get()` as valid JSON, and `get(p)` must itself reparse.
  (The stronger "pointer value equals an independent walk of the canonical text" oracle is v0.3,
  because it needs a reference model.)

**Hard rule, from a measurement — write it in the header and honour it in the harness:** the
error path is ``try { parse } catch { return; }``, and the property runs **outside every catch**.
A broad `catch` around the property turns a wrong answer into "expected for invalid input", which
is exactly how JSOM's harness missed a serializer that emits invalid JSON.

### 5. `fuzz/fuzz_jsonfuzz.cpp` — the day-one target (commit 1)

Same bytes, three readings, counted separately: (1) generate → mutate → oracles; (2) mutate the
input directly → oracles; (3) generate → the intent record must be consistent with the text
(member counts and kinds). The SUT for this target is a **toy SUT in `tests/`-adjacent code**
(a minimal `std::variant` model, ~100 lines, deliberately correct) so the repo can be fuzzed
before JSOM exists. A `-runs=0` seed smoke with `JSONFUZZ_REQUIRE_REACH=1` must exit non-zero
unless **each** reading reached its assertion (count each one; "some input got there" is the
guard that hid the blind spot in Permuto).

### 6. CLI `jsonfuzz` (`src/jsonfuzz_cli.cpp`)

`gen` (text to stdout, `--intent <file>` for the sidecar), `mutate --in --op --seed` (deterministic),
`check --sut <name> --in <file>` (run the oracles, print each violation), `--version`,
`--list-ops`, `--list-suts`, `--help`. Exit codes: 0 ok, 1 an oracle violation, 2 usage error,
3 no such SUT. `tools/cli_smoke.sh` asserts every documented flag, both exit codes, and that an
**unknown** flag FAILS (JSOM shipped a CLI that swallowed unknown options and exited 0).

### 7. The kit, and the gate

The repo is wired from `~/hermes-workspace/AI-DEV-STARTER` (kit HEAD
`9f3ff726ae5bcf0b45ccde51d2582a6dd0b0edab`, confirmed published with `git ls-remote origin`):

- copy, per `PLUNK-IN.md`: `templates/cpp/ci.sh` → `tools/ci.sh`, `templates/cpp/.ci.env.example`
  → `.ci.env.example`, `templates/hooks/{pre-commit,pre-push}` → `.githooks/` (mode 100755),
  `INCIDENTS.md`, `templates/CLAUDE.md.template` → `CLAUDE.md` (fill every placeholder — you can
  write this file; a Hermes agent cannot, the protected-file guard refuses it unattended),
  `.gitignore` from PLUNK-IN step 3, and `LICENSE` = **Unlicense**.
- `git config core.hooksPath .githooks`; two hooks: pre-commit runs the fast tier
  (`tree build tests`), pre-push runs the full set with `--require-clean`. Neither hook passes a
  stage list — `CI_DEFAULT_STAGES` in `tools/ci.sh` stays the one definition.
- `CI_DEFAULT_STAGES="tree docs format kitprobes build tests release cli version asan fuzz tsan tidy pristine"`.
  `docs` carries the owner's rule: no file in `CI_DOCS_FILES` may quote a test count (badge,
  prose, label or parenthetical form); `INCIDENTS.md` is exempt with the reason written in the stage.
  `CI_FUZZ_SECONDS=10`.
- Adopt the kit's probes: copy `probes/{tidy-baseline,gate-stage-guards,optimized-stage,git-index-file}.sh`
  into `tools/kit-probes/` and make the `kitprobes` stage run them against `tools/ci.sh`; all four
  must print VERIFIED (the kit's HEAD template implements all four fixes — if one fails, the port
  is wrong, not the probe).
- `.ai-dev-starter.json` with `record_kind: "at-copy"`, `revision` = the kit HEAD above,
  `files[]` for every copied artifact with real `repo_sha256`/`kit_sha256`, and the four probes
  listed under `adopted_fixes` (with `adopted_in` naming the commit in THIS repo — so two commits
  per adoption, artifact first, record-only second; that is the convention, see
  `AI-DEV-STARTER/docs/KIT-REVISION-CONVENTION.md`).
- A `JSONFUZZ ADAPTATIONS` block at the top of `tools/ci.sh` listing every deviation and why.
- `.clang-format` (LLVM-derived, 4-space, 100 columns — the suite's convention) and a
  repo-scoped `.clang-tidy` (`bugprone-*`,`performance-*`, positive `HeaderFilterRegex` matching
  only `include/jsonfuzz/.*\.hpp$|JSONFuzz/(src|tests|fuzz)/.*\.cpp$` — an unfiltered run lints
  the dependency's headers and can never be green).
- Artifact identity: `project(JSONFuzz VERSION 0.1.0)` ↔ the CMake-generated
  `include/jsonfuzz/version.hpp` ↔ what the binary prints for `--version`.
- `.gitignore`: `build*/`, `.ci-logs/`, `.ci.env`, `fuzz/corpus/`, `*.intent.json`.
  `fuzz/seeds/` and `fuzz/regressions/` are **tracked** (the seeds are the documentation
  examples; a defect found becomes a tracked reproducer).
- `docs/BRIEF-v0.1.md` = this brief, committed, so the next session resumes without re-deriving.
- `README.md`: what the tool is, the operator list with its provenance, how to run each command,
  and an explicit "what this does NOT do" section (v0.2–v0.4, below).

## Out of scope for v0.1 (do not build these — they are later briefs)

- **v0.2** the JSOM adapter + `LLVMFuzzerCustomMutator` linked into JSOM's existing harness (one
  campaign gets byte mutation *and* structure, no new binary), with the reach measurement before/after.
- **v0.3** the oracle set: formatter-preset agreement, pointer-vs-independent-walk,
  set-then-remove inverse (adds the write verbs), lazy-number text preservation,
  strict-accepts ⊆ lazy-accepts.
- **v0.4** the **differential oracle**: feed the same bytes to two parsers (JSOM and nlohmann) and
  compare accept/reject and canonical output. It is last on purpose: a disagreement only tells you
  *something* differs, and one side may be deliberately lenient (JSOM accepts `-01` by design), so
  it is only useful once the single-parser laws are trusted and sabotage-verified.

## Constraints

- TDD in both directions: a failing test FIRST for every behaviour and every bug fix, watched RED
  then GREEN. A defect the harness finds becomes a tracked file in `fuzz/regressions/` AND a test.
- `-Wall -Wextra -Wpedantic -Werror`; no raw owning pointers, no `new`/`delete`, no
  `reinterpret_cast`, no C-style casts (JSOM's `CODING_STANDARDS.md` is the house style).
- Never weaken a stage to make it pass. A gate that fails open is the thing this tooling prevents.
- **Do NOT commit, stage, branch or push.** Leave the work in the worktree; the orchestrator
  reviews the diff, commits, and then runs the full `--require-clean` tier.
- The sacrificial/verification work happens in `/tmp` COPIES. Never `git checkout` inside a copy
  (it restores HEAD, which is pre-change) and never touch the real repo with a sabotage.

## Evidence you must produce (exact commands and real output, in your report)

- **R1** `tools/ci.sh build tests` → `GATE PASSED`; and `tools/ci.sh --list` showing the stages.
- **R2** `tools/ci.sh fuzz` and `tools/ci.sh cli` → green; paste the four counters from the
  `-runs=0` seed smoke with `JSONFUZZ_REQUIRE_REACH=1`.
- **R3** the four kit probes run VERIFIED against `tools/ci.sh` (one command, real output).
- **R4** **the oracle has teeth — three sabotages, in a `/tmp` copy, each found inside the 10 s
  budget, with a clean control run first:** (S1) the generator emits a trailing comma before a
  closer; (S2) the string-escape mutation operator drops the backslash; (S3) class-shaped — the
  toy SUT's `get()` returns the wrong member when the pointer contains `/`. For each: the diff,
  the command, whether libFuzzer reported a finding, the artifact path, the seconds, and whether
  the finding came from the property or from the exception floor. Copy the WORKTREE, keep a
  pristine copy of each file, restore between runs, and run the control campaign first.
- **R5** the generator's totality: 10,000 documents from `jsonfuzz gen` across seeds/depths parse
  as valid JSON in an INDEPENDENT parser (nlohmann), 0 failures; report the number actually run.
- **R6** `tools/cli_smoke.sh`: documented flags, exit codes 0/1/2/3, and an unknown flag failing.
- **R7** cost: exec/s and coverage of the fuzz target, and the wall clock of the full gate on this
  machine (4 cores), so the budget is on the record.

## Report at the end

1. Files created, with line counts, and one paragraph describing the shape of the generator +
   the oracle wiring.
2. R1–R7: the command and the real output for each.
3. The honest limits: which class of defect is still unreachable, and what you would do next.
4. Anything you could not do, and the exact blocker. Do not paper over it.
