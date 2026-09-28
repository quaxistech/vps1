# Service independence

AltStrategy's market-data and proposal pipeline does not depend on an external
execution consumer being connected. It consumes upstream market and forecast
data, persists its own decision records, and computes market outcomes from
AltSpotter history.

The service publishes proposals to the internal AltSignalQuality stage. Final
signal delivery is handled by AltSignalGateway and can resume independently
from a durable client cursor. Execution feedback is optional and does not gate
proposal generation or market-outcome computation.

AltStrategy owns its database schema, NATS stream, configuration, and service
lifecycle. It does not write to AltSpotter, AltTech, AltML, or downstream service
storage.
