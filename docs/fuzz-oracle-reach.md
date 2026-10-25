# Fuzz oracle reach — measurements

This document records whether the oracle has teeth (sabotages, control first) and how much
of the input space actually reaches each law. It is the honest companion to the reach guard:
"a counter is non-zero" is not evidence of reach, so the numbers below are the evidence.
B5 (the v0.3 laws, newest) comes first; B3 (O1/O2) is kept below it unchanged, except that
its "cannot see" list is now superseded by B5's.

# B5 — the v0.3 laws (wrong-but-valid answers)

## The laws

O1/O2 only catch output that does not reparse or a pointer that does not resolve. B5 adds
five laws, each its own function in `include/jsonfuzz/oracle.hpp` (its limits are written
next to it), each returning a `LawResult` — structured violations (law, input, pointer,
expected, actual), an `evaluated` flag and a skip reason — and never throwing: a SUT throw
on the property path becomes a named violation of the law that made the call.

| Law | Property | Needs from the SUT |
|---|---|---|
| O3 pointer-value | `get(p)` equals the value an independent walk (nlohmann) of the canonical text finds at `p` | nothing new |
| O4 set-then-remove | `set(fresh, v)` applies, `get(fresh)` == v, `remove(fresh)` applies, `serialize()` == canonical | `set` / `remove` (optional; `Unsupported` = skip) |
| O5 preset-agreement | every `serialize_mode(m)` reparses to the same canonical text and is byte-stable | `serialize_mode_count()` ≥ 2 (default 1 = skip) |
| O6 number-text | every number token of the input appears in the reparsed canonical text | `preserves_number_text()` declared (default false = skip) |
| O7 strict-subset-of-lazy | strict accepts ⇒ lazy accepts, same canonical text (one direction) | nothing new |

`check_all()` runs O1..O7; the readings (and so the fuzz target) and the CLI `check` verb
use it. The fuzz target prints `LAW COUNTS` (inputs on which each law was *evaluated*) next
to `REACH COUNTS`, prints each violation with its law before trapping, and
`JSONFUZZ_REQUIRE_REACH=1` also fails the run when a law was never evaluated.

## Protocol

Same as B3: every sabotage in a `/tmp` copy of the worktree, a pristine copy of the
sabotaged file kept and restored (and the binary rebuilt) between runs; each campaign over
`fuzz/seeds` only, with a fresh empty writable corpus dir; a 60 s control on the untouched
copy FIRST; each sabotage at `-max_total_time=10` (`CI_FUZZ_SECONDS`). All sabotages are in
the toy SUT, because the toy is the SUT the fuzz target checks. Each one produces **valid
JSON** (or a valid accept/reject answer), so O1/O2 alone cannot see it.

Machine note: the host was shared with unrelated CPU-heavy jobs during this run (load
average 2.4–8 on 4 cores), so exec/s varies by 3× between runs of the SAME binary; the cost
comparison below therefore uses user-CPU time over a fixed input set, not exec/s.

## Sabotage table

Control (final code): 60 s, exit 0, zero artifacts, 222,234 runs, every law evaluated.

| # | Law | Sabotage (toy SUT, `src/jsonfuzz_sut.cpp`) | Finding? | Artifact | Seconds | Report source |
|---|---|---|---|---|---|---|
| S4 | O3 | `get_value`: an object member with a next sibling answers with the sibling's value | Yes, RC=77 | `crash-5bb89ad3…` (2 bytes; a seed) | 0.27 wall | O3 property: `pointer: /`, `expected: null`, `actual: {"":null}` |
| S5 | O4 | `remove_value`: an object member is not erased (`erase(it)` → `(void)it`), still `Applied` | Yes, RC=77 | `crash-adc83b19…` (1 byte; a seed) | 0.27 | O4 property: `pointer: /~0~1jf0`, `expected: {}`, `actual: {"~/jf0":null}` |
| S6 | O5 | `serialize_pretty`: a 2+ element array drops its last element (valid JSON) | Yes, RC=77 | `crash-907e159d…` (84 bytes; a seed) | 0.28 | O5 property: "mode 1 reparses to a different document", `…[1,2,3]…` vs `…[1,2]…` |
| S7 | O6 | `serialize_value`: the number text `-0` is emitted as `0` | Yes after the generator fix, RC=77 (survived 10 s before it) | `crash-22ea1c64…` (1 byte) | 0.28 | O6 property: `expected: -0`, `actual: 0` |
| S8 | O7 | `parse_number`: with `strict_numbers = false`, reject any exponent | Yes, RC=77 | `crash-0731c0b6…` (7 bytes; a seed) | 0.27 | O7 property: input `-850.0e0`, "strict accepted, lazy rejected" |
| S7b | O6 | `serialize_value`: the number text `1e309` is emitted as `inf` | **No** — exit 0, zero artifacts, 51,569 runs in 10 s | — | survived | (see "still cannot see") |

Every finding came from the PROPERTY (a `VIOLATION <law>` line printed before the trap), not
from the exception floor: no campaign log holds an uncaught exception, a `terminate`, or an
intent mismatch. "Seconds" is the wall time of the whole process (startup included); every
finding happened during seed replay or within the first mutations.

Each sabotage is also invisible to the other laws by construction, which the logs confirm:
S4 returns valid JSON (O2 blind), S5 returns the canonical text only after a reparse (O1
blind), S6's compact mode is untouched (O1/O3 blind), S7's `0` is numerically `-0` (O3
blind, O1 stable), S8's lazy parse is a clean rejection (O1..O6 just see fewer accepts).

### The S7 reach finding and its fix

S7 surviving was a reach finding of exactly the B3 S2 shape. The law was wired (the CLI
reports `[-0]` immediately), but the fuzz target generates at `adversarial_weight = 0`, and
there a bare `-0` was rare in fuzz inputs. libFuzzer inputs are short, `ByteSource` pads with
zero bytes, and the zero tail generated `0.0e0`. `E` exponents, `+` exponent signs and
20+ digit integers were not produced at all. The generator now emits number-TEXT forms
(`-0`, `-0.0`, `0E0`, `1e+2`, `1.50`, `1e-400`, a 24-digit integer) at any weight, with `-0`
at index 0 so the zero tail picks it, and the general grammar now uses `e`/`E`, an optional
`+`/`-` sign and one or two exponent digits (at most e99, so the reference still parses
it). TDD: `GeneratorReachTest.NumberTextFormsReachableAtZeroAdversarialWeight` ran RED first
("no 'E' exponent / no '+' exponent sign / no integer of 20+ digits at weight 0"), then
GREEN. Re-run: S7 died in 0.28 s. The test's frequency clause (a bare `-0` in at least 1
of 25 number-bearing documents) already held on 200-byte random streams; the short-input
rarity is shown by the campaign, not the unit test.

### A real defect the first law found (not a sabotage)

The first run of O3 against the toy reported `a pointer the SUT enumerated: the reference
walk finds no value there` for `{"a\"b":1}`. The toy enumerated (and resolved) pointers by
a key's RAW escaped body (`/a\"b`), where RFC 6901 names the decoded key (`/a"b`). O2 was
blind to it, because the toy was consistent with itself. Fixed: the toy decodes keys
(including `\uXXXX` and surrogate pairs) for enumeration, lookup and duplicate detection.
Tracked as `fuzz/regressions/escaped-key-pointer.json` plus
`ToySutTest.PointersUseTheDecodedKeyNotItsEscapedText` and
`PointerValueLawTest.TheToyResolvesAnEscapedKeyByItsValue`.

## Reach and cost

Per-reading reach is unchanged in shape: r1 and r3 are 100%, and r2 is about 1.8%.

| | before (B3 tree) | after (B5) |
|---|---|---|
| 60 s campaign, seeds only (runs; different load) | 133,339 | 222,234 (control) |
| r1 accepted | 100% | 100% |
| r2 accepted | 2,346 (1.76%) | 4,079 (1.84%) |
| r3 parsed | 100% | 100% |
| cov / ft | 19 / 20 | 26 / 26 |

Per-law evaluation in the 60 s control: O1, O2, O3, O5, O7 on 100% of inputs; O4 on
120,869 (54%): a scalar root has no parent to write into, and that is the documented skip.
O6 ran on 197,323 (89%); the skips are inputs holding a duplicated key, produced by the
DuplicateSubtree mutation. On the 9 seeds (the gate's `-runs=0` smoke) every law is
evaluated at least once, and O4 on 5 of the 9.

**Cost.** The same 2,000 fixed pseudo-random inputs (`-runs=0`, each input executed once)
were run through both binaries, interleaved, three times each, at load average ≈2.4:

| | user CPU, 3 runs | mean | per input |
|---|---|---|---|
| before | 0.494 / 0.320 / 0.359 s | 0.39 s | ≈0.20 ms |
| after | 0.897 / 0.837 / 0.874 s | 0.87 s | ≈0.43 ms |

So the v0.3 laws cost about **2.2×** CPU per input (process startup is ≈0.02 s, which is
negligible). An earlier pair under heavy load (≈8) measured 0.77 s vs 2.10 s (2.7×). In
the gate's 10 s budget this means fewer executions per run (the gate's fuzz stage ran 51,759
in this session). Every sabotage above was still found in under a second.

## What the oracle still cannot see after B5

- **Number texts the generator cannot produce.** `1e309` → `inf` (S7b) SURVIVED 10 s. The
  laws catch it at once if the input holds `1e309` (O1, O2 and O5 all fire, because `inf` is
  not JSON), but the fuzz target never generates `1e309` at weight 0. The reference parser
  rejects it, and reading 3 needs reference-parseable text. Reading 2 could reach it only
  through raw bytes, and reading 2 accepts about 1.8% of its inputs. The same holds for every
  adversarial-only form: lone surrogates, raw control characters, empty and duplicated keys
  from the generator.
- **O3 compares values, not texts.** Equality is nlohmann's: `1.0 == 1` and `-0 == 0`. A
  `get()` that returns the wrong member is invisible when that member holds an equal value
  (S4 on `{"a":1,"b":1}`). The text level belongs to O6. O3 skips documents the reference
  cannot model (`1e309`, lone surrogates, a lenient SUT's lazy numbers such as `-01`), and
  pointers that pass through a duplicated key.
- **Consistency is not correctness.** O3 checks `get()` against `serialize()`. A parser that
  drops or alters a member in its *model* has a consistent `get` and `serialize`, and O1, O2
  and O3 all pass it. Likewise, O3 only walks the pointers the SUT enumerates: a member that
  is in the canonical text but missing from `pointers()` is not reported. Catching these
  needs the differential oracle (v0.4) or an intent-vs-model comparison.
- **O4 is narrow by design.** It checks only fresh pointers, at most 4 parents per document
  and 5 fixed values. Array writes are only appends at `size`. Replacing an existing member,
  inserting in the middle of an array, `set("")` and removing pre-existing members are not
  checked: they are not identities, and they need a model of the expected result.
- **O5 judges "the same document" by the SUT's own canonical form.** A defect shared by
  every mode and the canonical serializer is invisible to O5. Whitespace and layout inside a
  mode are free as long as the mode is byte-stable.
- **O6 is a multiset containment, input ⊆ output.** A serializer that *adds* number text, or
  swaps the texts of two members (O3 catches the swap only if the values differ), passes O6.
  It is skipped when comments are allowed or a key is duplicated, and its scanner assumes
  double-quoted strings.
- **O7 is a tautology on the toy.** The toy ignores `strict_numbers`, so on the toy O7 only
  shows its teeth under sabotage. Its real reach depends on a SUT with a lazy grammar (JSOM,
  next run). The reverse direction is deliberately unchecked.
- **Reading 2's reach is still thin** (about 1.8% accepted), and coverage is still low
  (`cov: 26`), so libFuzzer has little to steer on. The reach comes from the generator.

# B3 — O1/O2 (kept for the record)

## Protocol

- All sabotage work happened in `/tmp` copies of the worktree (never the working tree, never
  `git checkout` inside a copy). A pristine copy of each sabotaged file was kept and the
  source restored between runs.
- Each campaign ran over `fuzz/seeds` ONLY, with a fresh empty corpus dir as the writable
  corpus and `fuzz/seeds` as the read-only seed dir. No accumulated corpus was used, so a
  finding is reach, not regression detection (memory).
- A 60 s control campaign on the untouched copy ran FIRST and was clean (exit 0, zero
  artifacts); without it, "the sabotage was found" is unfalsifiable.
- Each sabotage campaign used `-max_total_time=10` (the gate's `CI_FUZZ_SECONDS` budget).
- The fuzz target is clang-only, built with `-DPROJECT_BUILD_FUZZING=ON` into `build-fuzz`.

## Sabotage table

| # | Sabotage (diff) | Finding? | Artifact | Seconds | Report source |
|---|---|---|---|---|---|
| S1 | generator: `gen_object` closes every object with a trailing comma (`out += '}'` → `out += ",}"`) | Yes, RC=77 | `corpus/crash-adc83b19…` (1 byte: `\n`) | 0 | reading 3 (intent-vs-text, `intent_mismatch=1`) |
| S2 | toy SUT `get()`: when the pointer's last token holds `/`, return the wrong member (the first one) emitted with a trailing comma so the returned text fails to reparse | Yes after generator fix, RC=77 | `corpus/crash-c43064f7…` (26 bytes) | 1 | O2 property (`violations=1`) |
| S3 | toy SUT pointer walk: skip resolving any object key whose last token contains a digit | Yes, RC=77 | `corpus/crash-b4ff7458…` (6 bytes) | 1 | O2 property (`violations=1`) |

Control: 60 s on the untouched copy, exit 0, zero artifacts, 159,604 runs.

### The four facts per sabotage

- **S1 (wiring)** — the trailing-comma defect (the shape JSOM's harness could not see) is
  reported by reading 3: the generator's intent record is compared against a real parse of
  the text, and `{...,}` does not parse, so `intent_mismatch=1`. Found on the second seed
  (`\n`), 0 s. Not the O1/O2 property, not the exception floor.
- **S2 (wiring)** — the wrong-member defect in `get()` is reported by the O2 pointer law
  (`get(p)` must reparse). It survived the first 10 s run because the generator never
  produced a `/`-key at `adversarial_weight=0` (the fuzz target's configuration). After the
  generator fix (see below) it died in 1 s via `violations=1`. Report source: O2 property.
- **S3 (class-shaped, the reach probe)** — skipping digit-bearing keys is reported by O2
  (`get(p)` returned no value for a pointer the SUT itself enumerated). It DIED in 1 s via
  `violations=1`, because the generator already produces digit-bearing keys through the
  general alphabet (`pick_char` includes `0-9`). No generator fix was needed for S3. Report
  source: O2 property.

### The S2 reach finding and its fix

S2 surviving was a finding, not a failure: the pointer-key class (a key holding `/` or `~`)
was unreachable at `adversarial_weight=0`, because the generator only emitted such keys
through the adversarial pointer-key branch, which needs `adversarial()` true. The fuzz
target runs the generator at weight 0 (reading 3 needs parseable text), so the whole class
was invisible to the oracle readings. Fixed by adding `/` and `~` to the general key
alphabet (`pick_char`), so pointer keys are producible at any weight. TDD: the test
`GeneratorReachTest.PointerKeysReachableAtZeroAdversarialWeight` was written and run RED
first (it failed: "a key holding / or ~ was never produced at weight 0"), then the fix made
it GREEN. Recorded as an incident in `INCIDENTS.md`.

## Reach numbers (fixed generator, 60 s, seeds only)

| Reading | Inputs that reached the oracle laws | Share |
|---|---|---|
| 1 (generate → mutate → oracles) | 615,664 of 615,664 | 100% |
| 2 (mutate the input directly → oracles) | 10,719 of 615,664 | 1.7% |
| 3 (generate → intent vs text) | 615,664 of 615,664 | 100% |

- "Reached the oracle laws" = the primary text for that reading was ACCEPTED, so O1 and O2
  were evaluated to completion (the toy SUT never throws) rather than hitting a trivial
  early exit (a rejected parse). Reading 1's generated text is always valid JSON
  (non-adversarial options), so it reaches 100%. Reading 2's raw input is mostly invalid
  JSON, so it reaches only 1.7% — reading 2 sees the oracle laws rarely, and a defect that
  only shows on a *valid* raw input is hard for reading 2 to reach.
- Reading 3 reaches 100% because the generated text always parses.

### Coverage and speed, before and after the generator fix

| | cov | ft | exec/s |
|---|---|---|---|
| Before (old generator) | 16 | 17 | 10,699 |
| After (fixed generator) | 19 | 20 | 10,092 |

The fix added 3 features (the `/` and `~` characters opened new branches). exec/s is
roughly unchanged (~10k); the earlier 2,616/s reading was system load during parallel
builds, not a property of the target.

### Corpus

- Seeds: 8 (tracked in `fuzz/seeds/`).
- Accumulated corpus during the reach run: 0 new units (the fresh empty corpus dir stayed
  empty — the seeds already cover the few features the target exposes). So the numbers
  above are seeded, not accumulated.

## What the oracle could not see after B3 (superseded by the B5 list above)

- **A wrong-but-valid answer.** O2 in v0.1 only checks that `get(p)` reparses; it cannot
  tell a wrong member from the right one when both are valid JSON. S2 was only observable
  because the sabotage emitted invalid JSON. The stronger "pointer value equals an
  independent walk of the canonical text" oracle is v0.3.
- **Reading 2's reach is thin.** Only 1.7% of raw inputs are accepted, so reading 2 mostly
  exercises the rejection path. A defect that needs a valid raw input is hard for reading 2
  to reach; reading 1 (generated) is the workhorse.
- **The adversarial alphabet is only partially reachable in the oracle readings.** The fuzz
  target runs the generator at `adversarial_weight=0` (reading 3 needs parseable text), so
  lone surrogates, boundary numbers, empty/duplicate keys and control characters are not
  produced in readings 1 & 2. Pointer keys are now reachable (the S2 fix), but the rest of
  the adversarial alphabet is not. Reaching it would need the oracle readings to run the
  generator adversarially while reading 3 stays non-adversarial.
- **Coverage is low** (ft:20). The generator and oracle expose few branches, so libFuzzer's
  coverage guidance has little to steer on; the reach that exists comes from the generator
  being total and the alphabet being broad, not from deep branch exploration.

## What it would take to see the invisible classes

- v0.3's stronger O2 (pointer value vs independent walk) to catch wrong-but-valid members.
- Run readings 1 & 2 with an adversarial generator (separate from reading 3's non-adversarial
  text) so the full adversarial alphabet reaches the oracle laws.
- A reference model (v0.4 differential oracle) to catch disagreements that a single-parser
  fixed-point law cannot.
