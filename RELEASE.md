# Release v0.1.0 — Reliable Field Upload

## Status

Implementation complete — ready for `release/0.1.0`.

## Planned features

| Feature | Description | Completed commit |
|---|---|---|
| `feature/empty-dataset-guard` | Refuse to archive or upload datasets with no cell samples and no readings. | `7b62212` |
| `feature/dataset-manifest` | Add a self-describing `manifest.json` (dataset metadata, platform, capture counts, git commit) to every archive. | `3f08f5c` |
| `feature/upload-history` | Persist and display upload results, errors, timestamps and B2 object keys. | `5e248e3` |
| `feature/async-upload-queue` | Move uploads to a background queue with retries and restart-safe pending jobs. | `f3b4626` |
| `feature/device-key-keychain` | Store the Ed25519 identity in the OS credential store where available, with device-key reset/revocation. | `7f66d0a` |

## Acceptance criteria

- Empty datasets cannot be compressed or uploaded.
- Every uploaded archive contains `manifest.json`.
- The Upload dialog shows current queue state and recent upload history.
- Network uploads do not block the UI and failed jobs can retry after restart.
- Device identity is protected by macOS Keychain / Windows DPAPI where available; Linux falls back safely and documents it.
- The release workflow builds all platforms before creating `v0.1.0`.

## Release checklist

- [x] Feature scope agreed
- [x] Feature branches merged into `develop`
- [ ] `release/0.1.0` pushed and all platform builds green
- [ ] GitHub release `v0.1.0` generated from the release branch
- [ ] Release branch fast-forwarded into `main`
- [ ] `main` pushed and release assets refreshed
- [ ] `main` merged back into `develop`
