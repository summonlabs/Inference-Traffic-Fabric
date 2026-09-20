# Architecture

`Inference Traffic Fabric` is a coordinator plus a transport plus a deterministic decision core.
This document describes the moving parts and the invariants that hold between them.

## Processes

```
      +-------------------+         framed, authenticated, integrity-checked frames
      |  itf_coordinator  | <-------------------------------------------------+
      |  (authority)      |                                                   |
      +---------+---------+                                                   |
                |                                                             |
        snapshot file (atomic replace)                                        |
                |                                                             |
      +---------v---------+      loopback TCP data plane      +---------------+
      |  prefill worker   | --------------------------------> | decode worker |
      |  (itf_sim)        |   checksummed chunks + content CRC |  (itf_sim)   |
      +-------------------+                                   +--------------+
```

- The **coordinator** owns all authority: policy, generations, lifecycle, transfers, flows and
  accounting. It persists only what belongs to this boundary.
- **Peers** (serving workers, clients, CLI tools) hold no authority. They present identities,
  generations and evidence, and act only on decisions the coordinator returns.
- The **data plane** is peer to peer. The coordinator authorizes a transfer and accounts for it; it
  never carries payload. Integrity is verified end to end by the receiving peer.

## Library layers

| Target | Contents | Depends on |
| --- | --- | --- |
| `itf::core` | identities, generations, codec, policy engine, ledger, coordinator, persistence, CRC-32C | the C++ standard library and threads |
| `itf::net` | framing, message bodies, TCP transport, session server, client | `itf::core` and the platform socket library |
| `itf::accel` | accelerator probe (CUDA backend, or an UNSUPPORTED stub) | nothing from the fabric |
| `itf::service` (not installed) | the coordinator service used by the tools and the integration tests | `itf::net`, `itf::accel` |

`itf::core` never touches a socket, and `itf::accel` never touches fabric state. That split is
what makes the core deterministic and testable with an injected clock.

## State ownership inside the coordinator

| Structure | Bound | Notes |
| --- | --- | --- |
| requests (live + retained terminal) | `max_live_requests` + `max_retained_terminal_requests` | hash-keyed; terminal history evicted oldest-first |
| attempts per request | `kMaxAttemptsPerRequest` (8), fixed inline array | no allocation per request |
| flows | `max_active_flows_total` | reverse-indexed by request and by transfer |
| transfers | `max_transfer_records` | bound to model and state generations at registration |
| deferral records | `max_defer_records` | one per (subject, class); evicted deterministically at the ceiling |
| SLO contracts, route decisions, models, states | per-limit | keyed lookups, never scanned on the hot path |
| completions | `max_retained_completion_records` | idempotent duplicate reporting |

Every structure has an explicit ceiling, and no externally supplied size reaches an allocation
without passing a bound check.

## Concurrency model

- The coordinator holds **one mutex** for the duration of each operation. No callback, log sink or
  user code runs while it is held, no method re-enters the coordinator, and `export_state` copies
  under the lock while the caller performs file I/O outside it.
- The **session server** runs one acceptor thread plus one thread per session, bounded by
  `max_sessions`. Session threads never take the server's session mutex; they only touch their
  own slot flag and atomics.
- `stop()` stops accepting, closes the listener, closes every session socket to unblock readers,
  then joins workers **outside** every shared lock.
- Statistics are updated under a short-lived mutex as events happen, so an open session is visible.

## Decision lifetime

A decision is a value: outcome, treatment, subject, echoed binding, authority stamp and a bounded
explanation log. It is not a handle. Before it can become a flow it must pass `validate_decision`,
which re-checks the epoch, incarnation, policy generation, request cancellation, attempt currency
and the request's authority floor. This is what makes cancellation and supersession final even for
decisions that were minted earlier and are still held by a caller.

## Trust boundary

All wire and file input is untrusted: fixed framing with explicit lengths, CRC-32C over both the
header and the payload, bounded payloads and collection counts, canonical decoding, trailing-garbage
rejection, per-session monotonic sequences with replay refusal, and identity binding from the
authenticated session envelope rather than from fields the sender supplies. Provenance claims such
as topology generations, model generations and observing incarnations are overwritten by the
coordinator, never trusted.
