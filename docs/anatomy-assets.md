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
