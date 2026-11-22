import { verifyAsync } from "@noble/ed25519";

interface Env {
  DB: D1Database;
  B2_REGION: string;
  B2_S3_HOST: string;
  B2_BUCKET: string;
  B2_KEY_ID: string;
  B2_APP_KEY: string;
  B2_READ_KEY_ID: string;
  B2_READ_APP_KEY: string;
  BREVO_API_KEY: string;
  BREVO_SENDER_EMAIL: string;
  BREVO_SENDER_NAME: string;
  VERIFY_BASE_URL: string;
  ALLOWLIST_REQUIRED?: string;
}

const encoder = new TextEncoder();
const MAX_ARCHIVE_BYTES = 64 * 1024 * 1024;

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });

const bad = (message: string, status = 400) => json({ error: message }, status);

function toHex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function fromB64(value: string): Uint8Array {
  const bin = atob(value);
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
}

function toB64(bytes: Uint8Array): string {
  return btoa(String.fromCharCode(...bytes));
}

function toB64Url(bytes: Uint8Array): string {
  return toB64(bytes).replaceAll("+", "-").replaceAll("/", "_").replaceAll("=", "");
}

function randomHex(bytes: number): string {
  return toHex(crypto.getRandomValues(new Uint8Array(bytes)));
}

async function sha256Hex(value: string): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", encoder.encode(value))));
}

async function sha512Hex(data: ArrayBuffer): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-512", data)));
}

function canonicalEnroll(body: any): string {
  return ["cellscope-v1", "enroll", body.device_id, body.email, body.nonce, String(body.issued_at)].join("\n");
}

function canonicalUpload(body: any): string {
  return ["cellscope-v1", "upload", body.device_id, body.upload_id, body.archive_name,
    body.archive_sha512, body.archive_sha256, body.sig_sha256, body.nonce,
    String(body.issued_at)].join("\n");
}

function canonicalRevoke(body: any): string {
  return ["cellscope-v1", "revoke", body.device_id, body.nonce, String(body.issued_at)].join("\n");
}

function canonicalComplete(body: any): string {
  return ["cellscope-v1", "complete", body.device_id, body.upload_id, body.archive_sha512,
    String(body.archive_size), String(body.sig_size), body.nonce, String(body.issued_at)].join("\n");
}

function fresh(issuedAt: unknown): boolean {
  const n = Number(issuedAt);
  return Number.isInteger(n) && Math.abs(Math.floor(Date.now() / 1000) - n) <= 120;
}

function validId(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{8,80}$/.test(value);
}

function validArchive(value: unknown): value is string {
  return typeof value === "string" &&
    /^dataset-[A-Za-z0-9._-]{1,80}\.tar\.gz$/.test(value);
}

async function verifyDeviceSignature(publicKeyB64: string, signatureB64: string, message: string) {
  try {
    const publicKey = fromB64(publicKeyB64);
    const signature = fromB64(signatureB64);
    if (publicKey.length !== 32 || signature.length !== 64) return false;
    return await verifyAsync(signature, encoder.encode(message), publicKey);
  } catch {
    return false;
  }
}

async function consumeNonce(db: D1Database, deviceId: string, nonce: string): Promise<boolean> {
  if (!validId(nonce)) return false;
  const now = Math.floor(Date.now() / 1000);
  await db.prepare("DELETE FROM used_nonces WHERE expires_at < ?").bind(now).run();
  const result = await db.prepare(
    "INSERT OR IGNORE INTO used_nonces (device_id, nonce, expires_at) VALUES (?, ?, ?)",
  ).bind(deviceId, nonce, now + 600).run();
  return (result.meta.changes ?? 0) === 1;
}

async function sendVerificationEmail(env: Env, to: string, url: string): Promise<void> {
  const response = await fetch("https://api.brevo.com/v3/smtp/email", {
    method: "POST",
    headers: {
      "api-key": env.BREVO_API_KEY,
      "content-type": "application/json",
      accept: "application/json",
    },
    body: JSON.stringify({
      sender: { email: env.BREVO_SENDER_EMAIL, name: env.BREVO_SENDER_NAME },
      to: [{ email: to }],
      subject: "Verify your CellScope device",
      textContent: `Verify this CellScope device:\n\n${url}\n\nThe link expires shortly. If you did not request this, ignore this email.`,
      htmlContent: `<p>Verify this CellScope device:</p><p><a href="${url}">Verify device</a></p><p>The link expires shortly. If you did not request this, ignore this email.</p>`,
    }),
  });
  if (!response.ok) {
    throw new Error(`Brevo returned ${response.status}: ${(await response.text()).slice(0, 300)}`);
  }
}

async function hmac(key: Uint8Array, value: string): Promise<Uint8Array> {
  const cryptoKey = await crypto.subtle.importKey(
    "raw", key as unknown as BufferSource, { name: "HMAC", hash: "SHA-256" }, false, ["sign"],
  );
  return new Uint8Array(await crypto.subtle.sign("HMAC", cryptoKey, encoder.encode(value)));
}

function encodeRfc3986(value: string): string {
  return encodeURIComponent(value).replace(/[!'()*]/g, (c) =>
    `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
}

async function presignB2(env: Env, method: string, key: string, expiresIn = 180,
                         extraHeaders: Record<string, string> = {}): Promise<string> {
  const keyId = method === "PUT" ? env.B2_KEY_ID : env.B2_READ_KEY_ID;
  const appKey = method === "PUT" ? env.B2_APP_KEY : env.B2_READ_APP_KEY;
  const now = new Date();
  const amzDate = now.toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z");
  const dateStamp = amzDate.slice(0, 8);
  const scope = `${dateStamp}/${env.B2_REGION}/s3/aws4_request`;
  const host = env.B2_S3_HOST;
  const uri = `/${encodeRfc3986(env.B2_BUCKET)}/${key.split("/").map(encodeRfc3986).join("/")}`;
  const extraNames = Object.keys(extraHeaders).map((k) => k.toLowerCase()).sort();
  const signedHeaders = ["host", ...extraNames].join(";");
  const params: Record<string, string> = {
    "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
    "X-Amz-Credential": `${keyId}/${scope}`,
    "X-Amz-Date": amzDate,
    "X-Amz-Expires": String(expiresIn),
    "X-Amz-SignedHeaders": signedHeaders,
  };
  const query = Object.keys(params).sort().map((k) =>
    `${encodeRfc3986(k)}=${encodeRfc3986(params[k])}`).join("&");
  const canonicalHeaderLines = [`host:${host}`];
  for (const name of extraNames) {
    const value = extraHeaders[name] ?? extraHeaders[name.toLowerCase()];
    if (value === undefined) continue;
    canonicalHeaderLines.push(`${name}:${value.trim()}`);
  }
  const canonicalRequest = [
    method, uri, query, canonicalHeaderLines.join("\n") + "\n", signedHeaders,
    "UNSIGNED-PAYLOAD",
  ].join("\n");
  const stringToSign = [
    "AWS4-HMAC-SHA256", amzDate, scope,
    await sha256Hex(canonicalRequest),
  ].join("\n");
  const kDate = await hmac(encoder.encode(`AWS4${appKey}`), dateStamp);
  const kRegion = await hmac(kDate, env.B2_REGION);
  const kService = await hmac(kRegion, "s3");
  const kSigning = await hmac(kService, "aws4_request");
  const signature = toHex(await hmac(kSigning, stringToSign));
  return `https://${host}${uri}?${query}&X-Amz-Signature=${signature}`;
}

async function enroll(request: Request, env: Env): Promise<Response> {
  const body = await request.json<any>().catch(() => null);
  if (!body || !validId(body.device_id) || !validId(body.nonce) || !fresh(body.issued_at))
    return bad("invalid enrollment request");
  if (typeof body.email !== "string" || !/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(body.email))
    return bad("invalid email");
  if (typeof body.public_key !== "string" || typeof body.signature !== "string")
    return bad("missing public key or signature");

  const allowlistRequired = (env.ALLOWLIST_REQUIRED ?? "false").toLowerCase() === "true";
  if (allowlistRequired) {
    const allowed = await env.DB.prepare(
      "SELECT 1 FROM allowed_emails WHERE lower(email) = lower(?)",
    ).bind(body.email).first();
    if (!allowed) return bad("email is not on the allowlist", 403);
  }

  if (!await verifyDeviceSignature(body.public_key, body.signature, canonicalEnroll(body)))
    return bad("invalid device signature", 401);
  if (!await consumeNonce(env.DB, body.device_id, body.nonce))
    return bad("nonce already used", 409);

  const existing = await env.DB.prepare(
    "SELECT device_id, email, public_key, status FROM devices WHERE device_id = ?",
  ).bind(body.device_id).first<any>();
  if (existing) {
    if (existing.email !== body.email || existing.public_key !== body.public_key)
      return bad("device id is already bound to another identity", 409);
    if (existing.status === "active") return json({ status: "active" });
  }

  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    "INSERT INTO devices (device_id, email, public_key, status, created_at) VALUES (?, ?, ?, 'pending', ?) " +
    "ON CONFLICT(device_id) DO UPDATE SET email = excluded.email, public_key = excluded.public_key, status = 'pending'",
  ).bind(body.device_id, body.email, body.public_key, now).run();

  const token = toB64Url(crypto.getRandomValues(new Uint8Array(32)));
  const tokenHash = await sha256Hex(token);
  await env.DB.prepare(
    "INSERT INTO enroll_tokens (token_hash, device_id, expires_at) VALUES (?, ?, ?)",
  ).bind(tokenHash, body.device_id, now + 15 * 60).run();

  const verifyUrl = `${env.VERIFY_BASE_URL.replace(/\/$/, "")}/verify?token=${encodeURIComponent(token)}`;
  try {
    await sendVerificationEmail(env, body.email, verifyUrl);
  } catch (error) {
    await env.DB.prepare("DELETE FROM enroll_tokens WHERE token_hash = ?").bind(tokenHash).run();
    return bad(`email delivery failed: ${String(error)}`, 502);
  }
  return json({ status: "pending", message: "check your email to verify this device" }, 202);
}

async function verify(request: Request, env: Env): Promise<Response> {
  const token = new URL(request.url).searchParams.get("token") || "";
  if (!token) return new Response("Missing token", { status: 400 });
  const hash = await sha256Hex(token);
  const row = await env.DB.prepare(
    "SELECT t.device_id, t.expires_at, t.used_at, d.email FROM enroll_tokens t " +
    "JOIN devices d ON d.device_id = t.device_id WHERE t.token_hash = ?",
  ).bind(hash).first<any>();
  if (!row || row.used_at || row.expires_at < Math.floor(Date.now() / 1000))
    return new Response("This verification link is invalid or expired.", { status: 410 });
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare("UPDATE enroll_tokens SET used_at = ? WHERE token_hash = ?").bind(now, hash).run();
  await env.DB.prepare("UPDATE devices SET status = 'active', verified_at = ? WHERE device_id = ?")
    .bind(now, row.device_id).run();
  return new Response(
    "<!doctype html><meta charset='utf-8'><title>CellScope verified</title>" +
    "<body style='font:16px system-ui;padding:3rem'><h1>Device verified</h1>" +
    "<p>You can return to CellScope and upload your dataset.</p></body>",
    { headers: { "content-type": "text/html; charset=utf-8" } },
  );
}

async function deviceStatus(request: Request, env: Env, deviceId: string): Promise<Response> {
  if (!validId(deviceId)) return bad("invalid device id");
  const row = await env.DB.prepare("SELECT status FROM devices WHERE device_id = ?")
    .bind(deviceId).first<any>();
  return row ? json({ status: row.status }) : bad("device not found", 404);
}

async function createUpload(request: Request, env: Env): Promise<Response> {
  const body = await request.json<any>().catch(() => null);
  if (!body || !validId(body.device_id) || !validId(body.upload_id) || !validId(body.nonce) ||
      !fresh(body.issued_at) || !validArchive(body.archive_name) ||
      typeof body.archive_sha512 !== "string" || !/^[a-f0-9]{128}$/.test(body.archive_sha512) ||
      typeof body.archive_sha256 !== "string" || !/^[A-Za-z0-9+/]{43}=$/.test(body.archive_sha256) ||
      typeof body.sig_sha256 !== "string" || !/^[A-Za-z0-9+/]{43}=$/.test(body.sig_sha256))
    return bad("invalid upload request");

  const device = await env.DB.prepare(
    "SELECT public_key, status FROM devices WHERE device_id = ?",
  ).bind(body.device_id).first<any>();
  if (!device) return bad("device not enrolled", 403);
  if (device.status !== "active") return bad("device email is not verified", 403);
  if (!await verifyDeviceSignature(device.public_key, body.signature, canonicalUpload(body)))
    return bad("invalid device signature", 401);
  if (!await consumeNonce(env.DB, body.device_id, body.nonce))
    return bad("nonce already used", 409);

  const archiveKey = body.archive_name;
  const sigKey = `${archiveKey}.sig`;
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    "INSERT INTO uploads (upload_id, device_id, archive_name, archive_sha512, archive_key, sig_key, status, created_at, expires_at) " +
    "VALUES (?, ?, ?, ?, ?, ?, 'pending', ?, ?)",
  ).bind(body.upload_id, body.device_id, body.archive_name, body.archive_sha512,
    archiveKey, sigKey, now, now + 15 * 60).run();

  return json({
    upload_id: body.upload_id,
    archive_url: await presignB2(env, "PUT", archiveKey, 180,
      { "x-amz-checksum-sha256": body.archive_sha256 }),
    sig_url: await presignB2(env, "PUT", sigKey, 180,
      { "x-amz-checksum-sha256": body.sig_sha256 }),
    expires_in: 180,
  }, 201);
}

async function completeUpload(request: Request, env: Env, uploadId: string): Promise<Response> {
  const body = await request.json<any>().catch(() => null);
  if (!body || body.upload_id !== uploadId || !validId(body.device_id) ||
      !validId(body.nonce) || !fresh(body.issued_at) ||
      typeof body.archive_sha512 !== "string" || !/^[a-f0-9]{128}$/.test(body.archive_sha512) ||
      !Number.isInteger(body.archive_size) || !Number.isInteger(body.sig_size) ||
      body.archive_size < 1 || body.sig_size < 1)
    return bad("invalid completion request");

  const upload = await env.DB.prepare(
    "SELECT u.*, d.public_key FROM uploads u JOIN devices d ON d.device_id = u.device_id " +
    "WHERE u.upload_id = ? AND u.device_id = ?",
  ).bind(uploadId, body.device_id).first<any>();
  if (!upload) return bad("upload not found", 404);
  if (upload.status !== "pending") return bad("upload was already completed", 409);
  if (upload.expires_at < Math.floor(Date.now() / 1000)) return bad("upload request expired", 409);
  if (body.archive_sha512 !== upload.archive_sha512) return bad("hash does not match request", 409);
  if (!await verifyDeviceSignature(upload.public_key, body.signature, canonicalComplete(body)))
    return bad("invalid device signature", 401);
  if (!await consumeNonce(env.DB, body.device_id, body.nonce))
    return bad("nonce already used", 409);
  if (body.archive_size > MAX_ARCHIVE_BYTES) return bad("archive is too large", 413);

  const sigResponse = await fetch(await presignB2(env, "GET", upload.sig_key, 180));
  if (!sigResponse.ok) return bad(`could not read uploaded signature in B2 (HTTP ${sigResponse.status})`, 409);
  const sigBytes = await sigResponse.arrayBuffer();
  const sigSize = sigBytes.byteLength;
  /* B2 can return 403 for HEAD on Object Lock objects even when GET works.
     Read both objects and verify their bytes and sizes instead. */
  const archive = await fetch(await presignB2(env, "GET", upload.archive_key, 180));
  if (!archive.ok) return bad(`could not read uploaded archive in B2 (HTTP ${archive.status})`, 409);
  const bytes = await archive.arrayBuffer();
  const archiveSize = bytes.byteLength;
  if (archiveSize !== body.archive_size || sigSize !== body.sig_size)
    return bad("uploaded object sizes do not match", 409);
  if (await sha512Hex(bytes) !== upload.archive_sha512) {
    await env.DB.prepare("UPDATE uploads SET status = 'rejected' WHERE upload_id = ?").bind(uploadId).run();
    return bad("archive SHA-512 does not match", 409);
  }
  const sigText = new TextDecoder().decode(sigBytes).trim();
  if (sigText !== `${upload.archive_sha512}  ${upload.archive_name}`) {
    await env.DB.prepare("UPDATE uploads SET status = 'rejected' WHERE upload_id = ?").bind(uploadId).run();
    return bad("signature sidecar does not match the archive", 409);
  }

  await env.DB.prepare(
    "UPDATE uploads SET status = 'accepted', archive_size = ?, sig_size = ? WHERE upload_id = ?",
  ).bind(archiveSize, sigSize, uploadId).run();
  return json({ status: "accepted" });
}

async function revokeDevice(request: Request, env: Env): Promise<Response> {
  const body = await request.json<any>().catch(() => null);
  if (!body || !validId(body.device_id) || !validId(body.nonce) || !fresh(body.issued_at))
    return bad("invalid revoke request");
  const device = await env.DB.prepare("SELECT public_key, status FROM devices WHERE device_id = ?")
    .bind(body.device_id).first<any>();
  if (!device) return bad("device not found", 404);
  if (!await verifyDeviceSignature(device.public_key, body.signature, canonicalRevoke(body)))
    return bad("invalid device signature", 401);
  if (!await consumeNonce(env.DB, body.device_id, body.nonce))
    return bad("nonce already used", 409);
  await env.DB.prepare("UPDATE devices SET status = 'revoked' WHERE device_id = ?")
    .bind(body.device_id).run();
  return json({ status: "revoked" });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { status: 204 });
    if (url.pathname === "/health") return json({ ok: true });
    if (url.pathname === "/verify" && request.method === "GET") return verify(request, env);
    if (url.pathname === "/v1/enroll" && request.method === "POST") return enroll(request, env);
    if (url.pathname.startsWith("/v1/device/") && request.method === "GET")
      return deviceStatus(request, env, url.pathname.slice("/v1/device/".length));
    if (url.pathname === "/v1/uploads" && request.method === "POST") return createUpload(request, env);
    if (url.pathname === "/v1/device/revoke" && request.method === "POST") return revokeDevice(request, env);
    if (url.pathname.startsWith("/v1/uploads/")) {
      const id = url.pathname.slice("/v1/uploads/".length);
      if (id.endsWith("/complete") && request.method === "POST")
        return completeUpload(request, env, id.slice(0, -"/complete".length));
    }
    return bad("not found", 404);
  },
};
