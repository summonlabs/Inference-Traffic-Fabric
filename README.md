# Inference Traffic Fabric 1.0.0

A vendor-neutral C++20 runtime that decides **what network treatment an inference request or
serving flow is allowed to receive right now**, given its serving stage, SLO, state locality,
routing decision, capacity, congestion evidence and the exact authority generations in play.

It is a library plus a coordinator service, not a scheduler, a router or a model server. It owns
the *traffic-policy* half of inference serving and deliberately owns nothing else.

```
request/attempt + stage + generations + evidence + policy  ->  ALLOWED | DEGRADED | DEFERRED | REJECTED
```

Every decision is deterministic, explained, generation-bound and accountable.

---

## 1. Exact boundary

### Owned by this repository

- request and serving-attempt **traffic lifecycle** (registration, stage progression, completion,
  failure, cancellation, replacement);
- **stage-aware semantic classification** of inference traffic into the fabric's traffic classes;
- **SLO/deadline and tail-latency sensitivity** as it affects network treatment;
- **disaggregated prefill/decode handoff policy** (handoff, KV and state transfer authorization);
- **KV/state transfer urgency and integrity metadata** (per-frame checksums, post-transfer
  verification, verified-byte accounting);
- **streaming-response pacing class** and starvation protection for streaming egress;
- **model/adapter movement isolation** from latency-critical request traffic;
- **cancellation propagation**: a cancelled request cannot regain network authority;
- **stale route/model/state/topology/SLO/policy/epoch fencing**;
- explicit **defer / reject / degrade** outcomes with stable reason codes;
- **explanations** and **per-request network accounting**;
- **bounded retention and privacy-conscious metadata**: prompt text, token ids and payload bytes are
  never required, and never carried, in order to classify traffic.

### Explicitly not owned

| Adjacent system | Why it is out of scope |
| --- | --- |
| Model routing / path planning | The fabric consumes a route decision generation; it never computes routes. |
| Inference scheduling / admission control | It consumes a serving stage and an SLO contract; it does not choose where work runs. |
| KV storage / cache eviction | It authorizes and accounts for transfer; it does not store or evict KV. |
| Model residency management | It fences on model generations; it does not load, pin or evict models. |
| Generic bandwidth brokerage | It governs inference-specific classes; it is not a general traffic manager. |
| Application-layer serving logic | Tokenization, sampling, batching policy and response content are outside. |

The coordinator is authoritative for *traffic treatment*, not for the serving plan. Evidence and
plans arrive from adjacent systems as generations and are fenced, echoed and explained.

---

## 2. Core model and authority semantics

### Identities and generations

| Type | Meaning |
| --- | --- |
| `RequestId`, `AttemptId`, `StateTransferId`, `FlowId`, `SessionId`, `BootId`, `PeerId` | 128-bit lowercase-hex identities. The nil identity is never issued. |
| `RequestGeneration` | Advances when a request is replanned (attempt replacement, revalidation). |
| `AttemptGeneration` | Per-request monotonic counter; the identity of one serving attempt. |
| `ModelGeneration`, `StateGeneration` | Which model version and which state version a transfer may touch. |
| `RouteDecisionGeneration`, `SloContractGeneration`, `TopologyGeneration`, `PolicyGeneration` | Generations of adjacent-system decisions and of this fabric's configuration and evidence. |
| `CoordinatorEpoch`, `BootId` (incarnation), `AuthoritySequence` | Identity of one coordinator incarnation and the monotonic sequence stamped on every decision. |

`GenerationBinding` carries the generations a caller believes are current. An absent binding field
is *not* the same as a matching one: absence means "not bound", and rules that require current
evidence refuse rather than assume.

### Authority rules

1. **Observation is not authority.** Topology, capacity and congestion arrive as evidence with a
   provenance label and an observing incarnation; they are never a grant.
2. **Eligibility is not authority.** A registered attempt may still be refused because its
   generations moved.
3. **Every decision carries an `AuthorityStamp`** (epoch, incarnation, policy generation,
   sequence). `validate_decision` re-checks it before any flow is opened.
4. **Cancellation is final.** Cancelling raises the request's *authority floor*; a decision minted
   before the cancellation is refused even if the caller still holds it.
5. **Attempt identity is the retry fence.** A superseded attempt cannot advance stages, open flows
   or publish completion, regardless of what the caller claims.
6. **Completion is not commit.** A success publication commits once per attempt; duplicates are
   suppressed, and a publication for a cancelled, failed or superseded attempt is refused.
7. **Persistence is not currentness.** After a restart every restored dynamic fact - topology,
   model and state generations, live requests - returns as `REVALIDATION_REQUIRED` or
   `INTERRUPTED`.
8. **UNKNOWN stays distinct.** A missing stage, class, transfer or generation is refused
   (`REJECTED_UNKNOWN_STAGE`, `REJECTED_UNKNOWN_TRANSFER`, `REJECTED_STALE_*`), never
   promoted to a positive decision.

### Traffic classes

`REQUEST_INGRESS`, `PREFILL_INPUT`, `PREFILL_DECODE_HANDOFF`, `KV_TRANSFER`,
`STATE_TRANSFER`, `MODEL_TRANSFER`, `ADAPTER_TRANSFER`, `DECODE_STREAM`,
`SPECULATIVE_BRANCH`, `RESPONSE_EGRESS`, `CONTROL`, `UNKNOWN`

A class is legal only in serving stages that can produce it; a caller's declaration is re-derived
from the authoritative stage and the decision is marked `DEGRADED_CLASS_REDERIVED`.

---

## 3. Deterministic decisions

`Coordinator::evaluate` applies a fixed rule order. Every rule that fires is recorded in the
decision's bounded explanation log.

1. **Authority binding** - epoch and policy generation of the request, then the envelope.
2. **Subject resolution** - request/attempt lookup, cancellation, revalidation, terminality,
   attempt currency, request-generation fence.
3. **Transfer binding** - transfer existence, terminality, model/state generation fences.
4. **Stage resolution** - the ledger's stage is authoritative; a disagreeing claim is recorded and
   the decision is degraded. An `UNKNOWN` stage is refused unless the policy explicitly permits a
   conservative fallback rule.
5. **Class classification** - derived from the stage; illegal declarations are re-derived.
6. **Policy rule** - a disabled class is refused (`REJECTED_CLASS_DISABLED`).
7. **Evidence freshness** - missing, stale or revalidation-required topology, route, SLO and model
   generations are refused for the classes that require them.
8. **Deadline** - an expired deadline is refused; a missing deadline degrades pacing to best effort.
9. **Capacity and congestion** - bulk classes yield to streaming pressure and to the bulk in-flight
   cap; latency classes are deferred only when they hold no reserved weight.
10. **Starvation guard** - a starvation-protected flow deferred `max_defer_count` times, or for
    `max_defer_horizon_nanos`, is admitted with `ADMITTED_STARVATION_GUARD`. The guard only
    overrides capacity deferrals; it never overrides an authority refusal.
11. **Accounting** - the outcome, class and bytes are recorded per request, per attempt and per class.

Outcomes are `ALLOWED`, `DEGRADED`, `DEFERRED` and `REJECTED`; deferrals carry a hold
time and a retry hint. Reason codes are stable and exhaustive (see `docs/policy.md`).

---

## 4. Persistence semantics

Durable state is a single versioned snapshot:

```
magic[4]="ITFS" | format u16 | flags u16 | payload_len u64 | payload_crc32c u32 | header_crc32c u32 | payload
```

- Only this boundary's state is persisted: epoch, boot count, policy document, topology evidence
  (marked for revalidation on load), model/state generation registries, bounded terminal history, and
  every live request (restored as interrupted).
- Writes go to a temporary file, are flushed to stable storage, and then **atomically replace** the
  target (`MoveFileEx` on Windows, `rename` elsewhere). A partial write is never visible under
  the target name.
- Malformed, truncated, oversized, wrong-version, checksum-mismatched or internally impossible
  documents are refused with a specific status code and **no partial application**: recovery
  validates the whole document before applying anything.
- A refused document stops the coordinator from starting. It is never silently replaced by a fresh
  epoch.

---

## 5. Process, epoch and generation behavior

- A boot assigns a new `BootId` (incarnation) and advances `CoordinatorEpoch` past the persisted
  one. A fresh boot with no durable state starts at epoch 1.
- Every decision is stamped with the current epoch, incarnation, policy generation and a monotonic
  authority sequence; a decision from a previous incarnation is refused with
  `REJECTED_STALE_EPOCH`.
- Restored evidence is `REVALIDATION_REQUIRED` until re-established (`publish_topology`,
  revalidate the model or state generation). Restored registries keep their generation numbers, so a
  transfer bound to generation N is still refused after N advances.
- Sessions are authenticated per connection by a shared secret, and every request carries a
  per-session monotonic sequence. Replays and reordered frames are rejected with
  `REPLAY_REJECTED`, and a session that keeps replaying is closed.
- Shutdown stops accepting, signals sessions, closes sockets to unblock readers, joins outside every
  shared lock and leaves accounting closed.

---

## 6. Proof surfaces and labels

| Dimension | Label | Evidence |
| --- | --- | --- |
| Core policy and state machine | **REAL** | Deterministic unit, property, concurrency and adversarial suites. |
| Framed transport over loopback TCP | **REAL** | `test_protocol_*`: real sockets, real checksums, real replay refusal. |
| Multiprocess serving (coordinator plus workers, real KV transfer) | **REAL** | `test_multiprocess_*`: separate OS processes, loopback TCP data plane, process kills. |
| Coordinator restart and abrupt death | **REAL** | Kill-and-restart with a durable snapshot; corrupt and truncated snapshots refuse startup. |
| Accelerator execution (CUDA) | **REAL** | `itfctl accel-probe`: real kernel launch and a 4 MiB device-to-host copy with pattern verification. |
| AddressSanitizer coverage | **REAL** | All 20 suites pass with `-DITF_ENABLE_ASAN=ON`. |
| Multi-node / disaggregated topology | **SYNTHETIC** | Topology evidence published with `provenance=SYNTHETIC`. |
| RDMA, RoCE, InfiniBand, SmartNIC/DPU, NVLink, programmable switches | **UNSUPPORTED** | No such hardware is present; the fabric never claims physical validation for it. |
| Non-CUDA accelerators | **UNSUPPORTED** | Only one accelerator backend exists. |

Measured on the development host: `itfctl accel-probe` reports `label=REAL`,
`device="NVIDIA GeForce RTX 5090"`, `compute_capability=12.0`, `bytes_verified=4194304`,
`mismatches=0`, with about 7.4 GB/s of verified device-to-host copy bandwidth.

---

## 7. Build, test, install and consume

### Requirements

- CMake 3.25 or newer, a C++20 compiler and a platform threading library.
- Optional: a CUDA toolkit for the real accelerator probe. Without one, the build produces an
  explicitly UNSUPPORTED stub and nothing else changes.

### Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with a CUDA toolkit present, use the Ninja generator from a developer command prompt so
that `nvcc` is discoverable:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

The Visual Studio generator works as well; without the CUDA Visual Studio integration it builds the
UNSUPPORTED accelerator stub instead of the CUDA backend.

Options: `ITF_BUILD_TOOLS`, `ITF_BUILD_EXAMPLES`, `ITF_BUILD_TESTS`,
`ITF_STRICT_WARNINGS`, `ITF_ENABLE_ASAN` and `ITF_CUDA=AUTO|ON|OFF`.

Tests never use timeouts; a hanging test is treated as a defect. Every first-party translation unit
is compiled with MSVC `/W4 /WX` (or `-Wall -Wextra -Wpedantic -Werror` elsewhere) and produces
zero warnings.

### Install and consume

```sh
cmake --install build --prefix /tmp/itf
cmake -S consumer -B consumer-build -DCMAKE_PREFIX_PATH=/tmp/itf
cmake --build consumer-build && ./consumer-build/itf_consumer
```

`consumer/` is an independent project that only uses
`find_package(InferenceTrafficFabric 1.0 REQUIRED)`. The exported targets are `itf::core`,
`itf::net` and `itf::accel`. On Windows the accelerator backend is a shared library
(`itf_accel.dll`) so that the toolkit dependency stays private to it.

### Examples and tools

```sh
./build/itf_example_policy       # deterministic decisions and refusals
./build/itf_example_lifecycle    # cancellation, failure, replacement, duplicate completion
./build/itf_example_transport    # a coordinator hosted behind the framed transport
./build/itf_example_accel        # accelerator evidence labelling

./build/itfctl version
./build/itfctl simulate cancel
./build/itfctl simulate starvation
./build/itfctl accel-probe

./build/itf_coordinator --port 7777 --token SECRET --state-dir /tmp/itf-state
./build/itfctl status --port 7777 --token SECRET
./build/itfctl accounting --port 7777 --token SECRET

./build/itf_sim decode  --token SECRET --data-port 9100 --result-file /tmp/decode.txt
./build/itf_sim prefill --coordinator 127.0.0.1:7777 --token SECRET \
    --request <32-hex> --attempt <32-hex> --peer-port 9100 --bytes 1048576 --chunks 16
```

`itf_sim` runs real processes: the prefill worker registers and authorizes a transfer, streams
checksummed chunks to the decode worker over loopback TCP, and commits the completion through the
coordinator.

---

## 8. Public API sketch

```cpp
#include "itf/coordinator.hpp"
#include "itf/client.hpp"
#include "itf/session.hpp"
#include "itf/persistence.hpp"

itf::VirtualClock clock;                 // or itf::steady_clock_singleton()
itf::CoordinatorConfig config;           // policy + bounded limits
config.policy = itf::PolicyConfig::canonical_defaults(itf::PolicyGeneration::from(1));
itf::Coordinator coordinator(config, &clock);
coordinator.boot_fresh();

coordinator.publish_topology(evidence);  // SYNTHETIC or MEASURED, never assumed
coordinator.set_model_generation(model_hash, std::nullopt);
coordinator.advance_state_generation(state_hash, std::nullopt);

coordinator.register_request(request);
coordinator.register_attempt(request, attempt, model_hash);
coordinator.issue_route_decision(request, attempt, target_node, lateral, disaggregated);
coordinator.register_slo_contract(request, tail_budget_nanos, deadline_nanos, min_stream_share);
coordinator.advance_stage(request, attempt, itf::ServingStage::Admitted);

itf::Decision decision = coordinator.evaluate(traffic_request);
if (decision.permits_traffic()) {
  auto flow = coordinator.open_flow(decision, declared_bytes);
  coordinator.record_flow_bytes(flow.value(), bytes, /*inbound=*/true);
}

itf::CompletionPublication publication;   // request, attempt, generation, byte totals
coordinator.publish_completion(publication);  // Committed | DuplicateSuppressed | Refused

itf::AccountingSnapshot snapshot = coordinator.accounting();
itf::Status invariants = coordinator.verify_invariants();
```

Over the wire, `itf::net::Client` exposes the same operations (`register_request`,
`register_attempt`, `advance_stage`, `evaluate`, `open_flow`,
`authorize_transfer`, `publish_completion`, `fail_attempt`, `replace_attempt`,
`cancel_request`), and `itf::service::CoordinatorService` hosts them behind the framed session
server.

---

## 9. Limitations actually observed

- **Single-mutex coordinator.** Every coordinator operation serializes on one mutex; there is no
  read-parallel evaluation path. Measured cost is under a microsecond per request lifecycle, and the
  scale suite shows no quadratic growth (per-operation time at 20k requests is not larger than at
  2k), but throughput does not scale with cores.
- **Declared capacity, not measured capacity.** Fabric capacity, queue depth and congestion are
  evidence fields; the fabric does not meter links.
- **Loopback by default.** The session server binds `127.0.0.1` unless told otherwise, and there is
  no TLS: authentication is a shared secret over the chosen bind address.
- **One accelerator backend.** CUDA only; other vendors are UNSUPPORTED.
- **Snapshot, not a log.** Durability is a single atomically replaced file with no compaction and no
  write-ahead log. Snapshots are written after mutating messages (configurable interval) and on
  shutdown.
- **Bounded history.** Terminal requests are retained up to `max_retained_terminal_requests`
  (default 2048) and evicted oldest-first; a duplicate completion for an evicted request answers
  `NOT_FOUND`.
- **Deferral table eviction.** Under extreme deferral pressure the bounded deferral table retires one
  entry deterministically, which can reset a flow's deferral history.
- **Generations never wrap.** The 64-bit counters are advanced only by the coordinator; exhaustion is
  not handled and is not reachable in practice.
- **Windows toolchain notes.** The MSVC AddressSanitizer runtime is present in the Build Tools
  installation on this host but not in the Community installation. The Visual Studio generator cannot
  build the CUDA backend without the CUDA Visual Studio integration; the Ninja generator can.

---

## 10. Repository layout

```
include/itf/       public headers (core, transport, session, persistence, accelerator)
src/               implementation
tools/             itf_coordinator (service), itfctl (inspection), itf_sim (multiprocess simulator)
examples/          four runnable examples over supported paths
consumer/          independent find_package consumer
tests/             unit, integration, property, concurrency, adversarial, persistence, protocol, multiprocess
docs/              architecture, policy, persistence, protocol and test documentation
```

Test inventory: **20 suites, 3121 checks, 0 failures** in the Release configuration; the same suites
pass in Debug and under AddressSanitizer.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
