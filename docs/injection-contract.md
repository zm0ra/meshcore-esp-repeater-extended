# Injection Contract

This project injects a minimal integration layer into upstream `examples/simple_repeater`:

- include hook:
  - `#include "mcre_custom_entry.h"`
- lifecycle hooks:
  - `mcreCustomInit(this, fs)`
  - `mcreCustomLoop(this)`
- packet hooks:
  - `mcreCustomOnPacketRx(this, pkt, len, score, last_snr, last_rssi)`
  - `mcreCustomOnPacketTx(this, pkt, len)`

## Exact-once markers

Injector markers are inserted into upstream files and must exist exactly once:

- `MCRE_INJECT:include`
- `MCRE_INJECT:contract_include`
- `MCRE_INJECT:init`
- `MCRE_INJECT:loop`
- `MCRE_INJECT:on_rx`
- `MCRE_INJECT:on_tx`

If a marker is missing or duplicated, build stops immediately.

## Drift handling

If any anchor signature is missing, the injector aborts with a signature report:

- include anchors
- `MyMesh::begin(...)`
- `MyMesh::loop()`
- `MyMesh::logRx(...)`
- `MyMesh::logTx(...)`

This is intentional fail-fast behavior. Do not bypass it silently.
