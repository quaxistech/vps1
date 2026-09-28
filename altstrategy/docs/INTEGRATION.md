# Integration

AltStrategy assembles point-in-time decision contexts from upstream market data
and forecasts, then publishes market proposals and outcomes.

## Upstream inputs

- **AltSpotter:** global instrument universe and durable Bitget market history
  used by the market-outcome tracker.
- **AltTech:** venue and cross-venue feature vectors, microstructure, regimes,
  zones, and quality events. AltStrategy consumes the versioned
  `alttech.v1` protobuf contract vendored under `proto/`.
- **AltML:** versioned Bitget market forecasts on the documented
  `prediction.bitget.<symbol>.<tf>.v1` subjects.

The `context_assembler` joins fresh component revisions into a versioned
`DecisionContext`; it does not issue request/response calls to upstream
services. Missing, stale, or incompatible inputs remain explicitly unavailable.

## Internal downstream boundary

AltStrategy publishes proposals, market outcomes, and lifecycle events to its
owned JetStream stream. AltSignalQuality consumes proposal events and publishes
final signals for AltSignalGateway. AltStrategy does not place orders or manage
positions.

External clients receive final signals through AltSignalGateway. The gateway
protocol is the external signal-delivery contract; consumers do not depend on
AltStrategy's internal NATS subjects or database schema.

## Archive exports

The training-snapshot exporter writes immutable snapshots and publishes their
metadata on `archive.snapshot.created.v1`. Snapshot artifacts are a batch export
format; live signal delivery is handled through AltSignalGateway.

## Contract compatibility

`proto/decision.proto` defines the AltStrategy-owned `altstrategy.v1` contract.
Vendored upstream protobuf files preserve their producer field numbers. The
wire-contract tests guard compatibility.
