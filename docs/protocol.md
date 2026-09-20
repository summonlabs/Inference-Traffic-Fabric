# Wire protocol

## Framing

```
0   4   magic "ITF1"
4   2   protocol version (currently 1)
6   2   message type
8   4   flags (must be zero in this version)
12  4   payload length
16  8   sequence (per direction, strictly increasing from 1)
24  8   correlation (the sequence of the frame being answered)
32  4   CRC-32C of the payload
36  4   CRC-32C of bytes 0..35
40  N   payload
```

Decoding validates magic, version, message type, flags, the header checksum, the absolute payload
ceiling and the negotiated ceiling, in that order. A violation produces a deterministic
`ErrorResponse` followed by session close.

## Session establishment

1. the peer sends `HELLO_REQUEST` with its name, version, boot identity, the shared secret and the
   largest payload it accepts;
2. the server validates the frame version, the body version, the name shape, the secret (constant
   time) and the payload ceiling;
3. the server mints a fresh `SessionId` and `PeerId`, negotiates
   `min(server limit, client limit)` and answers with `HELLO_RESPONSE` carrying its epoch,
   incarnation, policy generation, topology generation and state.

Messages sent before the handshake are refused with `NOT_AUTHORIZED` and close the session.
Sessions are never resumed: identity is per connection, so a replay from a previous connection cannot
be mistaken for a continuation.

## Messages

| Type | Direction | Body |
| --- | --- | --- |
| `HELLO_REQUEST` / `HELLO_RESPONSE` | both | handshake |
| `REGISTER_REQUEST` | peer to coordinator | request identity and deadline |
| `REGISTER_ATTEMPT` | peer to coordinator | attempt identity and model hash |
| `STAGE_UPDATE` | peer to coordinator | serving stage transition |
| `CANCEL_REQUEST` | peer to coordinator | cancellation with a reason |
| `FAIL_ATTEMPT` | peer to coordinator | attempt failure with a reason |
| `REPLACE_ATTEMPT` | peer to coordinator | supersede an attempt with a new identity |
| `EVALUATE_TRAFFIC` | peer to coordinator | a traffic request |
| `DECISION_RESPONSE` | coordinator to peer | the decision, its binding and its explanation |
| `OPEN_FLOW` | peer to coordinator | turn a held decision into an accounted flow |
| `REGISTER_TRANSFER` | peer to coordinator | state/model bound transfer registration |
| `AUTHORIZE_TRANSFER` | peer to coordinator | request transfer authorization |
| `TRANSFER_COMPLETE` | peer to coordinator | verified byte count for a transfer |
| `COMPLETION_PUBLISH` | peer to coordinator | success or failure publication |
| `ACCOUNTING_QUERY` / `STATUS_QUERY` / `POLICY_QUERY` | peer to coordinator | reads |
| `TOPOLOGY_PUBLISH` | peer to coordinator | evidence with a provenance label |
| `PUBLISH_SLO_CONTRACT` | peer to coordinator | SLO contract for a request |
| `ISSUE_ROUTE_DECISION` | peer to coordinator | bind a route decision generation |
| `ADVANCE_MODEL_GENERATION` | peer to coordinator | model or state generation, advance or revalidate |
| `DATA_CHUNK` / `DATA_COMPLETE` | peer to peer | transfer payload and its content checksum |
| `HEARTBEAT` / `SHUTDOWN` | peer to coordinator | liveness and shutdown |

Each request type has exactly one response type, resolved by `wire::response_type_for` rather than
by enum adjacency.

## Adversarial behavior

- Replays: a sequence not greater than the last accepted one is refused with `REPLAY_REJECTED`;
  more than eight replays close the session.
- Corruption: any header or payload checksum mismatch produces `INTEGRITY_MISMATCH` and closes the
  session.
- Oversized frames: refused with `BOUNDS_EXCEEDED` against the negotiated limit, before any
  allocation.
- Trailing bytes inside a body: refused with `TRAILING_GARBAGE`.
- Unknown enum values, nil identities inside required fields and zero generations where a generation
  is required are all refused with `MALFORMED_INPUT`.
