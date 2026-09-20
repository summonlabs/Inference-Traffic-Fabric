# Testing and proof obligations

Tests are proof obligations. Every suite below is a separate executable registered with CTest under
a category label, and no test uses a timeout: a hanging test is treated as a defect to diagnose.

Measurements below are from the Release build on the development host.

| Suite | Category | Checks | What it proves |
| --- | --- | --- | --- |
| `test_foundation` | unit | 56 | identity parsing and rendering, generations, CRC-32C vectors, checked arithmetic, canonical codec round-trip, truncation, trailing bytes, bounds, nil rejection, unknown enums, UTF-8 validation (including overlong, surrogate and truncated forms), clock semantics. |
| `test_traffic_classification` | unit | 103 | stable labels for every class and stage, terminality, legal and illegal stage transitions, canonical class per stage, stage-dependent class legality, subject validity, contradictory metadata. |
| `test_policy_engine` | unit | 16 | canonical defaults validate, reserved-weight overflow, bulk classes may not reserve, conservative rule required for UNKNOWN admission, undefined treatments refused, starvation guard bounds, canonical encoding round-trip, foreign rule counts refused. |
| `test_ledger_lifecycle` | unit | 79 | registration, attempt binding, illegal transitions, attempt isolation and supersession, cancellation idempotency and authority floor, completion dispositions, generation mismatch refusal, flow accounting closure, flow and live-request limits, attempt ceilings, retention eviction, interruption and revalidation, failure publication. |
| `test_decision_explain` | unit | 174 | exhaustive reason-code text, classification of every reason, explanation recording and its policy bound, authority-denial classification, decisions without a resolvable subject still round-trip, deferral hints and hold times. |
| `test_accounting_closure` | unit | 22 | per-class outcome counters, per-request accounting, deferral counters, starvation admissions, peak flow tracking, closure after completion and cancellation, direct API misuse reported deterministically, transfer flows closing with their owner request. |
| `test_integration_lifecycle` | integration | 76 | the full serving lifecycle over loopback TCP against a real service, secret rejection, cancellation finality, transfer generation fencing, shutdown, restart with epoch advance and terminal-history retention. |
| `test_scale_indexes` | integration | 5 | 20k request lifecycles with per-operation cost that does not grow with scale, and bounded terminal history under eviction. |
| `test_property_state_machine` | property | 834 | seeded randomized operation sequences with invariant verification after every step, cancellation finality for every cancelled request, reproduction from the printed seed. |
| `test_property_starvation` | property | 107 | seeded bulk pressure against streaming traffic: bulk always defers, streaming is always admitted within its configured budget, guards scale with the configuration. |
| `test_concurrency_authority` | concurrency | 654 | concurrent cancellation followed by concurrent attempts to regain authority (none succeed), concurrent policy and evidence publication, repeated coordinator lifecycle. |
| `test_concurrency_server` | concurrency | 95 | repeated server start/stop, concurrent clients, stop while a session is blocked mid-read, session capacity refusal, abrupt disconnects, service lifecycle churn. |
| `test_adversarial_codec` | adversarial | 19 | exhaustive frame-header validation, truncated and trailing message bodies, hostile handshakes, chunk bounds, 4000 random byte-soup decodes (none accepted), every single-bit flip in a frame detected by checksum. |
| `test_adversarial_state` | adversarial | 628 | nil and unknown identities, contradictory metadata, transfer bounds, decision replay after completion, stale decisions after cancellation, bounded deferral records, global flow capacity, corrupt durable documents (300 single-byte corruptions), oversized payloads. |
| `test_persistence_format` | persistence | 23 | round-trip and determinism, atomic replacement without debris, and a specific status for every header and payload defect. |
| `test_persistence_recovery` | persistence | 41 | epoch advance, revalidation-required evidence, interrupted live requests, retained terminal history, monotonic boot identity, and refused documents never partially applying. |
| `test_protocol_session` | protocol | 40 | handshake version and secret mismatches, messages before the handshake, garbage producing one deterministic error frame, negotiated frame limit enforcement, truncated frames not wedging the server. |
| `test_protocol_replay` | protocol | 42 | replayed and reordered sequences refused, persistent replay closing the session, corrupted payloads refused by checksum, per-session sequence isolation. |
| `test_multiprocess_serving` | multiprocess | 65 | real coordinator and worker processes: a 1 MiB KV transfer with content verification, cancellation revoking a completed data transfer's authority, a killed prefill worker whose attempt cannot later publish success, and a live decode replacement fencing the retired attempt. |
| `test_multiprocess_restart` | multiprocess | 42 | abrupt coordinator death and restart over a durable snapshot, model advancement refused until revalidation, corrupt and truncated snapshots preventing startup, and a removed snapshot starting a fresh epoch. |

Total: **20 suites, 3121 checks, 0 failures** in Release. The same suites pass in Debug and under
AddressSanitizer (`-DITF_ENABLE_ASAN=ON`).

## Seed reproducibility

Property and adversarial suites accept `--seed N` and print the seed they used. A failing run
reproduces exactly from its printed seed:

```sh
./build/test_property_state_machine --seed 12345
```

## Proof labels

| Surface | Label |
| --- | --- |
| Policy, ledger, persistence, framing | REAL: deterministic in-process proofs |
| Loopback TCP transport and sessions | REAL: real sockets in every protocol test |
| Multiprocess serving, worker death, coordinator restart | REAL: separate OS processes, process kills, real durable files |
| Accelerator execution | REAL when a CUDA device is present; the probe launches a kernel and verifies a 4 MiB device-to-host copy |
| AddressSanitizer | REAL: the whole suite runs under it |
| Multi-node and disaggregated topology | SYNTHETIC: declared evidence with `provenance=SYNTHETIC` |
| RDMA, RoCE, InfiniBand, SmartNIC/DPU, NVLink, programmable switches | UNSUPPORTED: no such hardware is present |

## Running the suite

```sh
ctest --test-dir build --output-on-failure          # everything
ctest --test-dir build -L multiprocess              # one category
ctest --test-dir build -R test_persistence          # by name
```

The multiprocess suites locate `itf_coordinator` and `itf_sim` next to the test executable,
so tests and tools must be built from the same tree.
