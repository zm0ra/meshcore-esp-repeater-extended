# MeshCore ESP Repeater Extended

This repo is a thin build overlay system on top of MeshCore focused on running it as a repeater.

Instead of forking upstream and maintaining patches, it pulls a fresh copy on every build and injects only what is needed: WiFi TCP bridge, HTTP debug/ops panel, and MQTT reporting.

Nothing hidden, no magic. Just a controlled build process.

## In plain words

This extension makes MeshCore on ESP32 much easier to run as a real repeater node day to day.

You get an HTTP debug/ops panel, so most setup can be done from a browser instead of CLI.  
That includes regular node settings and location selection on a map.

The panel also shows live repeater state: visible neighbors (from adverts), map markers, MQTT state, and runtime activity.

There is a `/stats` JSON endpoint (Meshtastic-style structure), so it is easy to integrate with external tools like Home Assistant.

There is also:

- `/config-export` for config export
- config import support
- `/health` returning `OK` for simple monitoring

MQTT reporting is included as publish-only mode.  
You can use LetsMesh observer flows or your own broker setup (for example `mqtt.meshstats.pl`).

Under the hood this is still an overlay, not an upstream fork.  
Build pulls clean MeshCore and layers this functionality on top, which keeps updates manageable.

If you already use MeshCore and want a repeater that is easy to monitor and configure from a browser, this is exactly what it is for.

## Why this repo stands out

- Deterministic injection contract
- Exact-once marker validation
- No long-lived patch stack
- Runtime-first configuration model

## What this is

- Build overlay layer for MeshCore
- Adds repeater-oriented features
- Keeps upstream untouched
- Fails fast when something breaks upstream

## What this is not

- Not a MeshCore fork
- Not separate firmware
- Does not try to silently fix upstream changes

## How it works

Build flow:

1. Clone fresh MeshCore into `build/meshcore-upstream`
2. Copy custom modules into `examples/simple_repeater`
3. Inject small hooks into `MyMesh.cpp` and `MyMesh.h`
4. Verify everything was injected exactly once
5. Build with PlatformIO

All actual logic is in `custom/src/*.cpp`.
The injection step is only wiring: no AST, no patch files, just anchor-based text injection.

## Repo layout

```text
meshcore-esp-repeater-extended/
├── build.sh
├── config/
├── custom/
├── docs/
└── tools/
```

## Usage

List targets:

```bash
./build.sh --list-targets
```

Basic build:

```bash
./build.sh --build
```

Specific board:

```bash
./build.sh --target heltec_v3 --build
```

Build + upload:

```bash
./build.sh --target xiao_s3_wio --build --upload
```

Upload only:

```bash
./build.sh --target xiao_s3_wio --upload
```

More:

```bash
./build.sh
```

## Config

Compile-time config lives in `config/*.env`.
Runtime config is handled via web UI.

Only required bootstrap values go into env files.
Everything else should stay runtime-configurable.

## MQTT

Disabled by default.
MQTT mode here is publish-only.

To enable:

- `--mqtt-enable`
- `--mqtt-iata <code>`

You also need at least one broker (`--letsmesh` or custom broker args).

Custom broker example:

```bash
./build.sh --build --mqtt-enable --mqtt-iata ABC \
  --mqtt-custom-name meshstats \
  --mqtt-custom-host mqtt.meshstats.pl \
  --mqtt-custom-user <user> \
  --mqtt-custom-pass <pass>
```

Missing config stops the build early.

## Runtime

- WiFi + TCP bridge
- HTTP debug/ops panel at `/`, `/stats`, `/health`
- MQTT publish-only reporting
- In-memory state and logs

Everything is in `custom/src/`.

## Safety

- Injector checks anchors before modifying code
- Ensures hooks are inserted exactly once
- Fails fast if upstream changes

## Security

HTTP panel has no auth.
Use only in trusted networks.

## Multi-board support

Works with ESP32 repeater envs from upstream.

List available:

```bash
./build.sh --list-esp32-envs
```

Use predefined targets or pass `--pio-env`.

## Testing

Main real-device testing was done on **Seeed XIAO ESP32S3** (`xiao_s3_wio`).

Additional target profiles are included for other ESP32 boards (for example Heltec V3), but XIAO S3 is currently the primary validated path.

## Notes

- `config/local.env` is local only (gitignored)
- Do not commit build artifacts
- Screenshots in `docs/demo/screenshots` are examples only

## Docs

- Injection contract: [docs/injection-contract.md](docs/injection-contract.md)
- Screenshot walkthrough: [docs/demo/screenshots/README.md](docs/demo/screenshots/README.md)
