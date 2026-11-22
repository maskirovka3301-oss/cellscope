# Release v0.1.0 — Reliable Field Upload

## Status

Planning complete — implementation in progress.

## Planned features

| Feature | Description | Completed commit |
|---|---|---|
| `feature/empty-dataset-guard` | Refuse to archive or upload datasets with no cell samples and no readings. | |
| `feature/dataset-manifest` | Add a self-describing `manifest.json` (dataset metadata, platform, capture counts, file hashes) to every archive. | |
| `feature/upload-history` | Persist and display upload results, errors, timestamps and B2 object keys. | |
| `feature/async-upload-queue` | Move uploads to a background queue with retries and restart-safe pending jobs. | |
| `feature/device-key-keychain` | Store the Ed25519 identity in the OS credential store where available, with device-key reset/revocation. | |

## Acceptance criteria

- Empty datasets cannot be compressed or uploaded.
- Every uploaded archive contains `manifest.json`.
- The Upload dialog shows current queue state and recent upload history.
- Network uploads do not block the UI and failed jobs can retry after restart.
- Device identity is protected by macOS Keychain / Windows DPAPI where available; Linux falls back safely and documents it.
- The release workflow builds all platforms before creating `v0.1.0`.

## Release checklist

- [x] Feature scope agreed
- [ ] Feature branches merged into `develop`
- [ ] `release/0.1.0` pushed and all platform builds green
- [ ] GitHub release `v0.1.0` generated from the release branch
- [ ] Release branch fast-forwarded into `main`
- [ ] `main` pushed and release assets refreshed
- [ ] `main` merged back into `develop`
