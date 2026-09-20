# Policy and decision semantics

## Policy document

`PolicyConfig` holds one `ClassPolicyRule` per traffic class plus fabric-wide settings. A rule
carries the treatment (priority, pacing, integrity, isolation), reserved weight, burst and in-flight
bounds, the evidence it requires (topology, SLO, route, model, state, deadline), starvation-guard
bounds, and whether the class yields to latency-critical traffic.

`PolicyConfig::canonical_defaults` produces the documented starting point. Its reserved weights
total 900 of 1000; bulk classes hold no reserved weight at all, which is enforced by validation
(`BULK_RESERVED`).

Validation refuses: reserved weight overflow, reserved weight on a bulk class, enabled rules with an
undefined treatment, a starvation guard without bounds, and - importantly - enabling
`allow_unknown_stage` or `allow_unknown_traffic_class` without a conservative rule for the
UNKNOWN class.

## Evaluation order

The order is fixed and load-bearing: authority is decided before capacity, so the starvation guard
can never admit traffic that has lost its authority.

| Step | Rule | Possible outcome |
| --- | --- | --- |
| 1 | structural validation of the request | `REJECTED_SUBJECT_KIND_MISMATCH` |
| 2 | epoch binding | `REJECTED_STALE_EPOCH` |
| 3 | policy generation binding | `REJECTED_STALE_POLICY_GENERATION` |
| 4 | request lookup and lifecycle | `REJECTED_UNKNOWN_REQUEST`, `REJECTED_REQUEST_CANCELLED`, `REJECTED_REVALIDATION_REQUIRED`, `REJECTED_REQUEST_TERMINAL` |
| 5 | attempt identity | `REJECTED_UNKNOWN_ATTEMPT`, `REJECTED_STALE_ATTEMPT`, `REJECTED_STALE_REQUEST_GENERATION` |
| 6 | transfer binding | `REJECTED_UNKNOWN_TRANSFER`, `REJECTED_TRANSFER_ALREADY_TERMINAL`, `REJECTED_STALE_MODEL_GENERATION`, `REJECTED_STALE_STATE_GENERATION` |
| 7 | serving stage | `REJECTED_UNKNOWN_STAGE`, `DEGRADED_STAGE_CLAIM_OVERRIDDEN` |
| 8 | class classification | `DEGRADED_CLASS_REDERIVED`, `REJECTED_UNKNOWN_CLASS`, `REJECTED_CLASS_NOT_LEGAL_FOR_STAGE` |
| 9 | policy rule | `REJECTED_CLASS_DISABLED` |
| 10 | topology evidence | `REJECTED_EVIDENCE_UNKNOWN`, `REJECTED_EVIDENCE_STALE`, `REJECTED_STALE_TOPOLOGY_GENERATION`, `DEGRADED_STALE_TOPOLOGY` |
| 11 | SLO contract | `REJECTED_STALE_SLO_CONTRACT` |
| 12 | route decision | `REJECTED_STALE_ROUTE_DECISION` |
| 13 | model generation | `REJECTED_STALE_MODEL_GENERATION` |
| 14 | deadline | `REJECTED_DEADLINE_EXPIRED`, `DEGRADED_MISSING_SLO_CONTRACT` |
| 15 | capacity and congestion | `DEFERRED_CONGESTION`, `DEFERRED_BULK_YIELD`, `DEFERRED_BULK_CAPACITY` |
| 16 | starvation guard | `ADMITTED_STARVATION_GUARD` |
| 17 | admission | `ADMITTED`, `DEGRADED_PRIORITY_LOWERED`, `ADMITTED_CONSERVATIVE` |

## Reason codes

Admission: `ADMITTED`, `ADMITTED_STARVATION_GUARD`, `ADMITTED_CONSERVATIVE`.

Degradation: `DEGRADED_STAGE_CLAIM_OVERRIDDEN`, `DEGRADED_CLASS_REDERIVED`,
`DEGRADED_STALE_TOPOLOGY`, `DEGRADED_MISSING_SLO_CONTRACT`, `DEGRADED_INTEGRITY_RAISED`,
`DEGRADED_PRIORITY_LOWERED`.

Deferral: `DEFERRED_CAPACITY`, `DEFERRED_CONGESTION`, `DEFERRED_BULK_YIELD`,
`DEFERRED_BULK_CAPACITY`, `DEFERRED_BULK_CONCURRENCY`.

Refusal: `REJECTED_UNKNOWN_REQUEST`, `REJECTED_UNKNOWN_ATTEMPT`,
`REJECTED_REQUEST_CANCELLED`, `REJECTED_REQUEST_TERMINAL`, `REJECTED_STALE_ATTEMPT`,
`REJECTED_STALE_REQUEST_GENERATION`, `REJECTED_STALE_ROUTE_DECISION`,
`REJECTED_STALE_MODEL_GENERATION`, `REJECTED_STALE_STATE_GENERATION`,
`REJECTED_STALE_SLO_CONTRACT`, `REJECTED_STALE_TOPOLOGY_GENERATION`,
`REJECTED_STALE_POLICY_GENERATION`, `REJECTED_STALE_EPOCH`, `REJECTED_STALE_AUTHORITY`,
`REJECTED_UNKNOWN_STAGE`, `REJECTED_UNKNOWN_CLASS`, `REJECTED_CLASS_NOT_LEGAL_FOR_STAGE`,
`REJECTED_CLASS_DISABLED`, `REJECTED_SUBJECT_KIND_MISMATCH`, `REJECTED_UNKNOWN_TRANSFER`,
`REJECTED_EVIDENCE_UNKNOWN`, `REJECTED_EVIDENCE_STALE`, `REJECTED_REVALIDATION_REQUIRED`,
`REJECTED_DEADLINE_EXPIRED`, `REJECTED_CAPACITY_EXHAUSTED`, `REJECTED_FLOW_LIMIT`,
`REJECTED_NOT_AUTHORIZED`, `REJECTED_INTEGRITY_UNAVAILABLE`,
`REJECTED_DUPLICATE_COMPLETION`, `REJECTED_TRANSFER_ALREADY_TERMINAL`.

Lifecycle: `COMMITTED_COMPLETION`, `DUPLICATE_COMPLETION_SUPPRESSED`, `ATTEMPT_FAILED`,
`ATTEMPT_SUPERSEDED`, `CANCELLED`.

`Decision::is_authority_denial()` distinguishes refusals that come from lost authority from
refusals that come from shape, capacity or configuration.

## Starvation protection

A class marked `starvation_protected` cannot be deferred forever. Each time a capacity rule would
defer it, the coordinator records a deferral for that (subject, class). Once the count reaches
`max_defer_count`, or the elapsed wait reaches `max_defer_horizon_nanos`, the next
evaluation admits it with `ADMITTED_STARVATION_GUARD` and records the event in the per-class
accounting (`starvation_admissions`).

Bulk classes yield instead: while any starvation-protected class is deferred, every bulk class is
deferred with `DEFERRED_BULK_YIELD`, and bulk admission is additionally capped by
`bulk_in_flight_cap_bytes`. This is the property proven by `test_property_starvation`.

## Conservatism for UNKNOWN

An attempt whose serving stage has not been established has an authoritative `UNKNOWN` stage.
Nothing is admitted for it unless the operator opts in with `allow_unknown_stage` *and* supplies a
conservative rule for the UNKNOWN class. Even then the outcome is `DEGRADED` with
`ADMITTED_CONSERVATIVE` and the treatment is rewritten to zero reserved weight, best-effort
pacing and background isolation.
