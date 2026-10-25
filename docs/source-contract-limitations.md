# Source contract limitations

These findings come from static source inspection at VPS1 commit
`53c76051ed11964de1bf49f0b12e55cb81fcf4bc`. No code or runtime was changed.

- [AltTech composition](../alttech/src/app/main.cpp) launches six modules and
  initializes `ModuleSupervisor(6)`, but the `analytics_service_ready` log field
  reports `modules=5`. The source launch list is the documented topology;
  that log count is not authoritative inventory.
- [Gateway diagnostic client](../altsignalgateway/src/altsignalgateway/cli/client.cpp)
  verifies delivered/query payload hashes by canonical re-serialization of the
  parsed protobuf. The [server encoder](../altsignalgateway/src/altsignalgateway/core/codec.cpp)
  sends stored canonical payload bytes. Unknown fields can change serialization
  order in a consumer build; the diagnostic client therefore does not prove
  byte-exact forward-compatible verification. Consumers must hash the original
  payload-field wire bytes. VPS2 Ingest implements that boundary explicitly.

These are bounded source findings, not observations of a live outage.
