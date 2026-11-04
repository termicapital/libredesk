# ADR 0002: Additive device error-contract repair

- Status: accepted
- Date: 2026-08-22
- Decision owner: the project maintainer

## Context

M2 route tests showed that strict telemetry/coredump validation and an unavailable OTA manifest produced the explicit codes `invalid_payload` and `ota_unavailable`, but the M1 error enum and OpenAPI route responses did not document them. Leaving that mismatch would make the implementation violate its frozen contract.

## Decision

With the device owner's explicit approval, make a non-breaking additive revision:

- add `invalid_payload` and `ota_unavailable` to `contracts/error.schema.json`;
- document authentication, validation, throttling, oversized-payload, and unavailable-manifest responses on the affected routes;
- validate every error body the HTTP implementation emits against the shared error schema.

The latest-reading schema and schema version remain unchanged. Existing clients that already accept the documented error object are unaffected.

## Consequences

New route-level tests are required to prevent future undocumented error codes. Further contract changes still require a separate approval gate under PLAN.md §14.2.
