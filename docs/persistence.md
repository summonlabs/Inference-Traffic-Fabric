# Persistence and recovery

## File format

```
offset  size  field
0       4     magic "ITFS"
4       2     format version (currently 1), little-endian
6       2     flags, must be zero
8       8     payload length, little-endian
16      4     CRC-32C of the payload
20      4     CRC-32C of bytes 0..19
24      N     payload
```

The payload is the canonical encoding of `PersistedCoordinatorState`: format version, epoch, previous
incarnation, boot count, the policy document, topology evidence, model and state generation
registries, terminal request history and every live request.

## Failure codes

| Condition | Status |
| --- | --- |
| file absent | `NOT_FOUND` |
| shorter than the header | `MALFORMED_INPUT` |
| magic mismatch | `MALFORMED_INPUT` |
| unknown format version | `UNSUPPORTED_VERSION` |
| header CRC mismatch | `INTEGRITY_MISMATCH` |
| declared length above the ceiling | `BOUNDS_EXCEEDED` |
| payload shorter than declared | `MALFORMED_INPUT` |
| bytes after the payload | `TRAILING_GARBAGE` |
| payload CRC mismatch | `INTEGRITY_MISMATCH` |
| non-canonical or impossible records | `MALFORMED_INPUT` |
| policy document invalid | `POLICY_INVALID` |
| duplicate identity in the request history | `ALREADY_EXISTS` |

## Write path

1. encode the document under the configured size ceiling;
2. create a temporary file next to the target;
3. write the header and payload;
4. flush to stable storage (`FlushFileBuffers` / `fsync`);
5. atomically replace the target (`MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on
   Windows, `rename` elsewhere);
6. remove the temporary on failure.

A reader therefore observes either the previous document or the new one, never a partial write.

## Recovery semantics

`Coordinator::boot_from` validates the entire document before applying anything, and clears
everything if validation fails, so a refused document leaves no partial state behind.

| Restored item | Result |
| --- | --- |
| epoch | advanced past the persisted value |
| incarnation | new `BootId`; the persisted one is retained only as history |
| boot count | monotonic; never decreases across restarts |
| policy | restored, with its generation advanced so old bindings are fenced |
| topology evidence | preserved but reported `REVALIDATION_REQUIRED` |
| model and state registries | preserved with generations, flagged for revalidation |
| terminal requests | restored as terminal, for idempotency and accounting |
| live requests | restored as `INTERRUPTED` with revalidation required |
| SLO contracts, route decisions, transfers, flows, deferrals | not persisted; authority must be re-established |

Because the incarnation differs, every decision minted before the restart fails
`validate_decision` with `STALE_EPOCH`, and because the registries are flagged, a transfer
bound to model generation N is refused until the generation is revalidated - after which the
generation fence still applies.

## Snapshot policy in the service

`CoordinatorService` snapshots after mutating messages, subject to
`snapshot_interval_nanos` (zero means after every mutating message), and again on shutdown.
Read-only messages never trigger a write. A snapshot failure is logged and reported; it never
silently degrades into an in-memory-only mode, and the coordinator reports the last error through
`last_snapshot_error()`.
