# Fuzz oracle reach — B3 measurement

This document records the B3 run: whether the oracle has teeth (three sabotages, control
first), and how much of the input space actually reaches each reading's oracle laws. It is
the honest companion to the reach guard: "a counter is non-zero" is not evidence of reach,
so the numbers below are the evidence.

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

## What the oracle still cannot see after this run

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
