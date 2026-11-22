# CellScope upload backend

A Cloudflare Worker + D1 + Brevo + Backblaze B2 backend for signed CellScope
dataset uploads. The app generates an Ed25519 device key, enrolls it by email
once, and signs every upload request with that key.

## Architecture

```text
CellScope
  POST /v1/enroll          signed device + email
  GET  /v1/device/:id      enrollment status
  POST /v1/uploads         signed upload request
  PUT  presigned B2 URLs   archive + .sig
  POST /v1/uploads/:id/complete
  GET  /verify             one-time email link

Worker/D1 ── Brevo ── verification email
Worker/D1 ── B2 S3 ── 180-second presigned PUT URLs
```

The B2 write key is used only to create the presigned PUT URLs. A separate
read-only B2 key is used to verify uploaded objects. Neither key is in the app.

## 1. Create the D1 database

```bash
cd cloudflare
npm install
npx wrangler login
npx wrangler d1 create cellscope-upload
```

Copy the printed `database_id` into `wrangler.toml`.

Apply the schema:

```bash
npx wrangler d1 migrations apply cellscope-upload --remote
```

## 2. Create the B2 bucket and keys

Create a private B2 bucket, for example `cellscope-datasets`.

Create two application keys:

- **write key**: bucket-only, `writeFiles` capability;
- **read key**: bucket-only, `readFiles` capability.

Do not use the account master key. Note the S3 endpoint (`s3.<region>.backblazeb2.com`).

## 3. Configure Brevo

Create a Brevo account and a transactional API key. Verify the sender address
used for `BREVO_SENDER_EMAIL`. Free Brevo allows 300 transactional emails/day.
Only enrollment sends email; authenticated uploads do not.

## 4. Configure the Worker

Edit `wrangler.toml`:

```toml
B2_REGION = "us-west-004"
B2_S3_HOST = "s3.us-west-004.backblazeb2.com"
B2_BUCKET = "cellscope-datasets"
VERIFY_BASE_URL = "https://cellscope-upload.YOUR-SUBDOMAIN.workers.dev"
BREVO_SENDER_EMAIL = "verify@your-domain"
BREVO_SENDER_NAME = "CellScope"
```

Store secrets with Wrangler:

```bash
npx wrangler secret put B2_KEY_ID
npx wrangler secret put B2_APP_KEY
npx wrangler secret put B2_READ_KEY_ID
npx wrangler secret put B2_READ_APP_KEY
npx wrangler secret put BREVO_API_KEY
```

Deploy:

```bash
npx wrangler deploy
```

The resulting `https://cellscope-upload.<subdomain>.workers.dev` is the API URL
to put in CellScope’s Settings.

Uploaded objects are written at the bucket root as
`dataset-<name>.tar.gz` and `dataset-<name>.tar.gz.sig`. The B2 application key
must allow that prefix; if it is restricted to `datasets/`, update the key or
the restriction.

## 5. Enrollment access

Enrollment is open by default: any valid email can enroll, but the device still
has to click the Brevo verification link. To require an allowlist, set
`ALLOWLIST_REQUIRED = "true"` in `wrangler.toml` and seed approved addresses:

```bash
npx wrangler d1 execute cellscope-upload --remote \
  --command "INSERT INTO allowed_emails (email, note) VALUES ('you@example.com', 'approved')"
```

With allowlisting disabled, keep an eye on Brevo quotas and add rate limiting
before public deployment.

## 6. CellScope settings

Set these fields in Settings → `upload_api_url` and `upload_email`.

Then:

1. Open **Upload**.
2. Click **Enroll email**.
3. Follow the Brevo verification link.
4. Click **Upload** after verification.

The first upload enrolls the device. Later uploads are signed with the same
device key and do not require email verification. The app uploads from a
persistent queue, retries failures, writes a signed `manifest.json` into each
archive, and can reset/revoke the device identity. On macOS the key lives in
Keychain; Windows uses DPAPI; Linux uses a 0600 file.

## Security properties

- Ed25519 device key is generated and stored locally at
  `~/.cellscope/cloud-device.key` with mode `0600` on Unix.
- Every enrollment/upload/complete request has a unique nonce and timestamp.
- The Worker verifies the signature against the enrolled public key.
- Enrollment is open by default; set `ALLOWLIST_REQUIRED=true` to restrict it to
  D1-approved emails.
- The app uploads directly to single-object, 180-second B2 PUT URLs.
- The Worker verifies archive size, SHA-512, and the `.sig` sidecar after upload.
- B2 write and read credentials are separate; neither is sent to the app.
- The Worker has no inbound connection to the app.

## Troubleshooting

- `email is not on the allowlist`: either set `ALLOWLIST_REQUIRED = "true"` and
  seed `allowed_emails`, or leave the default `"false"` for open enrollment.
- `Brevo returned 401: Key not found`: replace the Worker secret with a valid
  Brevo **API key** (not an SMTP key):
  ```bash
  npx wrangler secret put BREVO_API_KEY
  ```
  Create it under Brevo → SMTP & API → API Keys. Also verify
  `BREVO_SENDER_EMAIL` as a sender in Brevo.
- `invalid device signature`: update CellScope; older builds read the Ed25519
  public key from the wrong offset when reloading the device key.
- `B2 PUT ... 403 InvalidAccessKeyId`: the key ID is not accepted at this S3
  endpoint. Verify that `B2_REGION`/`B2_S3_HOST` exactly match the bucket's
  Endpoint. An application key is valid only for its account/bucket region.
  Legacy regions such as `us-west-003` have no `s3.<region>.backblazeb2.com`
  endpoint; create the bucket in an S3-compatible region such as `us-west-004`,
  or create a new B2 account in that region, then create new keys.
- `B2 PUT ... 403 AccessDenied`: the key is valid but lacks `writeFiles`
  (write key) or `readFiles` (read key), or it is scoped to another bucket.
- A key can authenticate successfully against the Native API and still be
  unusable for the S3 API if the bucket's region has no S3 endpoint. Check the
  bucket Endpoint first, not just the key.

## Object Lock buckets

If the B2 bucket has Object Lock enabled, B2 requires a checksum header on every
PUT. CellScope signs `x-amz-checksum-sha256` into both presigned URLs and sends
the matching SHA-256 for the archive and `.sig` sidecar. Do not remove that
header. B2 may return 403 for `HEAD` on Object Lock objects even when `GET`
works; the Worker verifies with `GET` responses instead.

## Limits and operational notes

- D1 free plan: 5M row reads/day, 100k writes/day, 5 GB.
- Workers free plan: 100k requests/day.
- Brevo free: 300 emails/day.
- B2 storage and operations follow the B2 plan.
- Add rate limiting and abuse controls before open deployment.
- The Worker currently uses a 64 MiB archive cap because it downloads the
  archive to verify SHA-512. Increase this only after testing Worker memory/CPU.

## Operational checklist

- Keep B2 bucket private.
- Rotate B2 keys if exposed.
- Keep Brevo API key in Wrangler secrets, never in the repository.
- Back up D1.
- Monitor Worker errors and Brevo bounces.
- Restrict the write key to the dataset bucket; do not grant delete capability.
