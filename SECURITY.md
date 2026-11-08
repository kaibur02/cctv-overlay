# Security policy

## Reporting a vulnerability

Please report security problems **privately** through GitHub's
[private vulnerability reporting](../../security/advisories/new) for this repository, not in a public issue.
Include the version (`cctv-overlay --version`), what an attacker can do, and steps to reproduce. Do not include
real camera addresses, credentials or footage.

## What the project protects

- **Recorder credentials.** The password lives only in `~/.config/cctv-overlay/secrets.ini` (mode 0600) or the
  `CCTV_OVERLAY_PASSWORD` environment variable. It is never accepted in `config.ini`, on a command line, in an
  installer answers file, in status output or in logs (all log and error text is redacted). The installer reads
  it from a hidden terminal prompt only.
- **Local control only.** The daemon listens on a Unix socket in `$XDG_RUNTIME_DIR` (mode 0600, peer UID
  checked). It opens no network listener; its only network traffic is the RTSP connection to your cameras.
- **No input capture.** The overlay never takes keyboard focus and has an empty input region.

## Known limits

- RTSP is unencrypted; `rtsps://` is not supported. Use a dedicated view-only account on a trusted network.
- While the daemon runs, the password is in its memory (core dumps are disabled at start-up).
- A per-camera `url =` override can contain credentials in `config.ini`; this is discouraged and warned about.

Details: [`docs/behaviour.md`](docs/behaviour.md#security-model) and the review notes in
[`ARCHITECTURE.md`](ARCHITECTURE.md) §13.
