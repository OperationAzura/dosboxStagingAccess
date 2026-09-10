# Darklands accessibility fork

This branch is based on DOSBox Staging `v0.83.0` (`7b40053b7`) and adds a small
localhost API used by [DarkText](https://github.com/OperationAzura/darktext)
and [Darklands Coords](https://github.com/OperationAzura/darklands-coords).
See the [umbrella repository](https://github.com/OperationAzura/darklands-accessibility)
for complete installation and architecture documentation.

## Changes

- Captures the latest native emulated video frame and serves it as binary PPM.
- Injects DOS keyboard and mouse input without requiring host focus/capture.
- Reuses DOSBox Staging's existing web-server memory endpoints for live
  coordinate discovery.
- Keeps the web server disabled by default and retains host-header validation.

## Endpoints

| Method and path | Purpose |
| --- | --- |
| `GET /api/v1/video/frame` | Latest frame as `image/x-portable-pixmap` |
| `POST /api/v1/input/key` | Key `press`, `down`, or `up` |
| `POST /api/v1/input/mouse/move` | Relative DOS mouse movement |
| `POST /api/v1/input/mouse/button` | Mouse button `click`, `down`, or `up` |
| `GET /api/v1/memory/:offset/:len` | Existing memory-read endpoint used for calibration |

Example:

```bash
curl -X POST http://127.0.0.1:8086/api/v1/input/key \
  -H 'Content-Type: application/json' \
  -d '{"key":"kp6","action":"press"}'
```

## Build

Follow the upstream platform guide in `docs/build-linux.md`. A typical build is:

```bash
meson setup build
meson compile -C build
```

Run with the API explicitly enabled and loopback-bound:

```bash
build/dosbox \
  --set webserver_enabled=on \
  --set webserver_bind_address=127.0.0.1 \
  --set webserver_port=8086
```

## Security

The API exposes emulator memory and synthetic input. Never bind it to a public
or untrusted interface. The default remains disabled and the accessibility
launcher binds it to `127.0.0.1`.

## Fork workflow

The imported repository retains the upstream Git history as a shallow clone.
After creating a GitHub fork:

```bash
git remote rename origin upstream
git remote add origin git@github.com:OperationAzura/dosboxStagingAccess.git
git push -u origin darklands-accessibility
```

Fetch additional upstream history later with `git fetch --unshallow upstream`
if desired.
