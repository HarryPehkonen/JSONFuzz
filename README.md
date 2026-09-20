# JSONFuzz

A parser-agnostic, structure-aware JSON fuzzer. JSONFuzz generates and mutates JSON with
intent, and checks any parser behind a small interface against two fixed-point laws. It is
built so a parser (JSOM is the first adapter, in a later brief) can be fuzzed by byte
mutation *and* coordinated structure from the same deterministic byte stream.

The repo is a library (`jsonfuzz_core`), a CLI (`jsonfuzz`), a GoogleTest suite, and a
libFuzzer target (`fuzz_jsonfuzz`, clang only). It builds and tests green with **no JSON
parser installed** — the parser-agnostic interface is the only seam, and this repo's own
tests and fuzz target use a deliberately-correct toy SUT.

## What it does

- **Generate** — a deterministic, structure-aware generator turns any byte stream into a
  JSON document plus an *intent record* (the kinds and member counts it decided to
  produce). Same bytes ⇒ same document. The intent record is the oracle's independent
  truth, so no reference parser is needed for the fixed-point laws.
- **Mutate** — JSON-aware mutation operators (truncate, delete, duplicate, wrap, change
  kind, break number grammar, adversarial escapes, unbalance brackets, mismatch closers,
  inject invalid UTF-8, swap members). One entry point is shaped for libFuzzer; the same
  operators are usable directly.
- **Check** — two oracle laws against any `SystemUnderTest`:
  - **O1 fixed point / round trip**: if `parse(x)` is accepted, then `serialize()` must
    reparse and `serialize()` of that reparse must be byte-identical.
  - **O2 pointer contract**: every pointer the SUT returns must be non-empty, must be
    returned by `get()` as valid JSON, and `get(p)` must itself reparse.

The error path is `try { parse } catch { return; }` and the property runs **outside every
catch** — a broad `catch` around the property turns a wrong answer into "expected for
invalid input", which is exactly how JSOM's harness missed a serializer that emits invalid
JSON.

## Commands

| What | Command |
|---|---|
| Run the gate | `tools/ci.sh` |
| Fast tier (pre-commit) | `tools/ci.sh build tests` |
| Build the CLI | `cmake --build build -j$(nproc)` then `./build/jsonfuzz --version` |
| Generate a document | `./build/jsonfuzz gen` (add `--intent <file>` for the sidecar) |
| Mutate a document | `./build/jsonfuzz mutate --in <file> --op <name> --seed <n>` |
| Check a document | `./build/jsonfuzz check --sut <name> --in <file>` |
| List operators / SUTs | `./build/jsonfuzz --list-ops` / `./build/jsonfuzz --list-suts` |
| CLI smoke | `tools/ci.sh cli` |
| Fuzz smoke (10 s) | `tools/ci.sh fuzz` |

Exit codes: `0` ok, `1` an oracle violation, `2` usage error, `3` no such SUT.

## The interface

`include/jsonfuzz/sut.hpp` is the parser-agnostic boundary. Strings cross it; no parser
type is shared. A `SystemUnderTest` adapter lives in the parser's own repo (JSOM's is a
later brief). A registry picks an adapter by name for `--sut <name>`; with none installed
the CLI says so and exits 3.

## The operator list and its provenance

The operator set is informed by `probelabs/json-fuzz` (Go, MIT, Jul 2026) — a
grammar-based generator with 11 JSON-aware mutation operators and a list of correctness
gates. We borrow the *shape* of its operator list (truncate, delete, duplicate, wrap,
change-kind, number-grammar, escape, bracket, swap) and name which are ours:

- **Borrowed from json-fuzz**: truncate at a boundary, delete a member/element, duplicate
  a subtree, wrap in an array/object, change a value's kind, break a number's grammar,
  swap two members.
- **Ours**: replace a string with an adversarial escape (lone surrogate, invalid UTF-8,
  raw control char), unbalance brackets, mismatch a closer, inject invalid UTF-8. These
  target the escape/UTF-8 and bracket-mismatch bug classes the generator's adversarial
  alphabet is built to reach.

Each operator is documented in `include/jsonfuzz/mutate.hpp` with the bug class it targets,
and has one test.

## What this does NOT do yet

- **v0.2** — the JSOM adapter and an `LLVMFuzzerCustomMutator` linked into JSOM's existing
  harness (one campaign gets byte mutation *and* structure, no new binary), with the reach
  measurement before/after.
- **v0.3** — the rest of the oracle set: formatter-preset agreement, pointer-vs-independent
  walk, set-then-remove inverse (adds the write verbs), lazy-number text preservation,
  strict-accepts ⊆ lazy-accepts.
- **v0.4** — the differential oracle: feed the same bytes to two parsers (JSOM and
  nlohmann) and compare accept/reject and canonical output. It is last on purpose: a
  disagreement only tells you *something* differs, and one side may be deliberately lenient.

## License

Unlicense — public domain. See `LICENSE`.
