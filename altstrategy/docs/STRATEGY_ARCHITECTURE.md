# Strategy and proposal architecture

AltStrategy is the setup and event layer. Its detectors describe market events;
AltML forecasts market outcomes. Every valid proposal carries its decision
context and the forecasts available for the relevant horizons.

## Data flow

```text
AltSpotter + AltTech + AltML
             │
             ▼
    context_assembler
             │
             ▼
   strategy catalogue ──► candidates ──► ensemble/ranking
                                               │
                                               ▼
                                      trade plan / proposal
                                               │
                         ┌─────────────────────┴──────────────────┐
                         ▼                                        ▼
               signal archive / outcomes                  AltSignalQuality
```

AltStrategy consumes versioned upstream events and does not issue upstream
request/response calls. The context assembler admits fresh, eligible component
revisions and records their source versions in `DecisionContext`.

## Point-in-time inputs

A decision uses only values available at its T0. Each grid and venue is checked
against its own timestamp and freshness bound. A value is not substituted from
another venue, timeframe, or later revision. Missing and non-finite values stay
explicitly unavailable.

## Candidate and ranking stages

The strategy catalogue evaluates eligible setup detectors and records each
candidate. The ensemble combines direction, agreement, and family independence;
the adaptive allocator and ranking policies select candidates for plan
construction. Plan geometry describes entry, invalidation, targets, and time
limits. It is a proposal contract, not an order.

Execution costs, quality state, and forecast availability are carried as
attributes. Market signals are not suppressed by an economic estimate or by a
strategy's historical PnL. Only technically invalid proposal geometry or
expired decision context prevents publication.

## Persistence and outcomes

AltStrategy persists contexts, candidates, rankings, plans, proposals, market
outcomes, and attribution in its own PostgreSQL schema. Outcomes are reconstructed
from AltSpotter's durable market history. Execution outcomes, when supplied,
remain separate from market outcomes.

See [Decision context](DECISION_CONTEXT.md), [Market outcomes](MARKET_OUTCOMES.md),
and [Strategy catalogue](STRATEGY_CATALOGUE.md) for the detailed contracts.
