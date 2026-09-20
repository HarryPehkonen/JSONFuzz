# JSONFuzz — the contract for whoever (or whatever) works in this repo

## What this is

JSONFuzz is a parser-agnostic, structure-aware JSON fuzzer: it generates and mutates
JSON with intent, and checks any parser behind a small interface against two fixed-point
laws. It is built so a parser (JSOM is the first adapter, in a later brief) can be fuzzed
by byte mutation *and* coordinated structure from the same deterministic byte stream.
"Working" means: the repo builds and tests green under the local gate, the fuzz target
builds and runs, and no document in this repo quotes a test count.

## Commands

| What | Command |
|---|---|
| Run the gate (do this before you claim anything works) | `tools/ci.sh` |
| Fast tier (pre-commit) | `tools/ci.sh build tests` |
| Tests only | `tools/ci.sh tests` |
| Build only | `tools/ci.sh build` |
| Format the files you touched | `clang-format -i <files>` |
| Fuzz smoke (10 s) | `tools/ci.sh fuzz` |
| CLI smoke | `tools/ci.sh cli` |
| Build the CLI | `cmake --build build -j$(nproc)` and run `./build/jsonfuzz --version` |
| One-time per clone: arm the hooks | `git config core.hooksPath .githooks` |

The gate's last line is the verdict: `GATE PASSED` or `GATE FAILED`. Never report work as
done on a run that did not print `GATE PASSED`. If a stage SKIPs because a tool is missing
on this machine, say so explicitly — a skipped check is not a passed check.

## Architecture

Four pieces, none of which (yet) includes a real parser:
- `jsonfuzz_core` (`include/jsonfuzz/`, `src/jsonfuzz_core.cpp`) — the library. In v0.1 it
  holds the artifact-identity `version()`; the generator, mutators and oracles land in
  Phase B.
- `jsonfuzz` CLI (`src/jsonfuzz_cli.cpp`) — currently only `--version` and `--help`
  (Phase A); the `gen`/`mutate`/`check` verbs arrive in Phase B.
- `jsonfuzz_tests` (`tests/`) — GoogleTest; the TDD suite.
- `fuzz_jsonfuzz` (`fuzz/fuzz_jsonfuzz.cpp`) — the libFuzzer target (clang only), built with
  `-DPROJECT_BUILD_FUZZING=ON` into `build-fuzz`.

The seams: `include/jsonfuzz/version.hpp` is CMake-generated from `project(JSONFuzz VERSION
...)` — the one source of truth for the version number.

## Invariants

- One version number: `project(JSONFuzz VERSION 0.1.0)` in `CMakeLists.txt` == the
  generated `include/jsonfuzz/version.hpp` (`JSONFUZZ_VERSION`) == `jsonfuzz --version`
  prints `JSONFuzz 0.1.0`. Enforced by the `version` stage of the gate and by
  `tests/test_version.cpp` / `tests/test_cli.cpp`.
- No document in `CI_DOCS_FILES` (README.md, CLAUDE.md, docs/BRIEF-v0.1.md) may quote a
  test count. Enforced by the `docs` stage. `INCIDENTS.md` is exempt (its entries name
  measurements, not suite sizes).
- No raw owning pointers, no `new`/`delete`, no `reinterpret_cast`, no C-style casts;
  `-Wall -Wextra -Wpedantic -Werror` on every project target. House style: JSOM's
  `CODING_STANDARDS.md`.

## Conventions

- TDD: a failing test first for every behaviour and every bug fix, watched RED then GREEN.
- Tests live in `tests/`. They are registered with `gtest_discover_tests`, so any file
  listed in `CMakeLists.txt` `jsonfuzz_tests` sources runs; add new test files there.
- The fuzz target must build from the first commit and be kept building; the `fuzz` stage
  runs a `-runs=0` seed smoke plus `CI_FUZZ_SECONDS` seconds.
- A defect the fuzzer finds becomes a tracked file in `fuzz/regressions/` AND a test.
- Commits: conventional-commit shape, one logical change each.
- The gate is `tools/ci.sh`, forked from the AI-DEV-STARTER kit. Every deviation from the
  template is written in the `JSONFUZZ ADAPTATIONS` block at the top of `tools/ci.sh` and
  in `.ai-dev-starter.json`.

## Gotchas

<!-- APPEND-ONLY. One line each, newest first, and each one is a real incident. -->

- The `tree` stage fails while work is uncommitted and untracked. During a session run the
  stages by name (`tools/ci.sh build tests`, `tools/ci.sh fuzz`, ...) and commit only when
  the orchestrator asks; the full default run is the committed-tree gate.
- The fuzz target is clang-only: build it with `-DPROJECT_BUILD_FUZZING=ON` and a clang
  compiler (the `fuzz` stage does this in its own `build-fuzz` dir). It will not build with
  the default compiler.
- `CI_VERSION_HEADER` and `CI_VERSION_BINARIES` are repo-set in `tools/ci.sh` to point at
  `build/generated/jsonfuzz/version.hpp` and `build/jsonfuzz` — see the ADAPTATIONS block.

## Do not

- Do not weaken a stage to make it pass. A gate that fails open is the thing this tooling
  prevents.
- Do not "fix" the gate by deleting a check. Every check carries its incident in
  `INCIDENTS.md`; if you believe a check is wrong, say so in a comment and let a human
  decide.
- Do not edit generated files: `build/generated/jsonfuzz/version.hpp`. Change
  `project(JSONFuzz VERSION ...)` in `CMakeLists.txt` and rebuild.
- Do not write a test count into README.md, CLAUDE.md, or docs/BRIEF-v0.1.md (the `docs`
  gate fails).

## Incidents

Every rule above that came from a failure is written down in `INCIDENTS.md` with the
symptom, the check that now catches it, and why deleting the check re-enables the bug.
Read it before removing anything that looks redundant.
