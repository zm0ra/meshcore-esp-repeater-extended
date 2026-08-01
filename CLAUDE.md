# meshcore-esp-repeater-extended

Thin build-overlay on top of upstream MeshCore, focused on running it as a repeater.
Pulls a fresh upstream copy on every build and injects only what's needed (WiFi TCP
bridge, HTTP debug/ops panel, MQTT reporting) — not a fork, no maintained patch set.

## Stack
- `./build.sh` — wraps `tools/build_repeater.sh`, the actual build entrypoint
- `config/` — build-time injected config, `custom/` — the overlay itself
- No test suite — verify via a real build + flash

## Conventions
- Never hand-edit anything under a fetched-upstream directory — changes belong in
  `custom/`/`config/`, the overlay layer, so they survive the next upstream pull.
