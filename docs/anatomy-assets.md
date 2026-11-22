# Anatomy 3D assets

The purchased anatomy model is not stored in Git or in the application
release. It is downloaded from a separate B2 bucket at runtime.

## Manifest

The base URL is set with `CELLSCOPE_ANATOMY_URL` and must contain a
`manifest.txt` file:

```text
version=1
file=anatomy-medium.glb sha512=<128 lowercase hex chars> size=12345678
file=code-atlas.png sha512=<128 lowercase hex chars> size=1234567
file=license.txt sha512=<128 lowercase hex chars> size=4567
```

Rules:

- `version` identifies a cache directory under
  `~/.cellscope/assets/anatomy/<version>/`.
- File names are flat; no directories, `..`, `/` or `\`.
- Every file is verified against SHA-512 before being installed and reused.
- Failed downloads are removed and reported in the Symptom Report tab.
- Only list the LOD and atlas files needed by the current release; the app
  downloads every file in the manifest.

Example:

```bash
export CELLSCOPE_ANATOMY_URL='https://example-b2.example.com/cellscope-anatomy'
```

The bucket may be public, or the URL may point to a Worker endpoint that
returns a short-lived presigned URL. B2 application keys must never be embedded
in the app.

## Cache

```text
~/.cellscope/assets/anatomy/<version>/
  anatomy-medium.glb
  code-atlas.png
  license.txt
```

The 2D body map remains available until the 3D renderer consumes this cache.


## Private B2 delivery

For a private bucket, point `CELLSCOPE_ANATOMY_URL` at the Worker route:

```text
https://<worker>/v1/anatomy
```

The Worker routes `manifest.txt` and each flat asset name to a 300-second
presigned B2 GET. Configure the Worker:

```bash
cd cloudflare
npx wrangler secret put B2_ANATOMY_KEY_ID
npx wrangler secret put B2_ANATOMY_APP_KEY
```

Set the non-secret values in `wrangler.toml` or the dashboard:

```toml
[vars]
B2_ANATOMY_BUCKET = "your-anatomy-bucket"
B2_ANATOMY_PREFIX = ""
B2_S3_HOST = "s3.<region>.backblazeb2.com"
B2_REGION = "<region>"
```

The key should be read-only and scoped to the anatomy bucket. The Worker
accepts only flat names matching `[A-Za-z0-9][A-Za-z0-9._-]{0,127}` and returns
302 redirects; the app's curl downloader follows them and verifies SHA-512.
