# MeshCore ESP Repeater Extended

This repo is a thin overlay on top of MeshCore focused on running it as a repeater.

Instead of forking upstream and maintaining patches, it pulls a fresh copy on every build and injects only what is needed: WiFi TCP bridge, simple HTTP panel, and MQTT reporting.

Nothing hidden, no magic. Just a controlled build process.

## What this is

- Overlay build system for MeshCore
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
The injection step is only wiring.

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

## MQTT

Disabled by default.

To enable:

- `--mqtt-enable`
- `--mqtt-iata <code>`

You also need at least one broker (`--letsmesh` or custom broker args).

Missing config stops the build early.

## Runtime

- WiFi + TCP bridge
- HTTP panel at `/`, `/stats`, `/health`
- MQTT publish
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
