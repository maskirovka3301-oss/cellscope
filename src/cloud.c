#include "cloud.h"

#include <curl/curl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "util.h"

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

#define DEVICE_SERVICE "com.cellscope.device"
#define DEVICE_ACCOUNT "ed25519"

#define CLOUD_MAX_RESPONSE (1024 * 1024)

typedef struct {
    char *data;
    size_t len;
} Buffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buffer *b = (Buffer *)userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > CLOUD_MAX_RESPONSE) return 0;
    char *next = realloc(b->data, b->len + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static bool cloud_ready(char *err, size_t cap)
{
    if (sodium_init() < 0) {
        snprintf(err, cap, "Could not initialize libsodium");
        return false;
    }
    return true;
}

static bool key_path(char *out, size_t cap, const char *name)
{
    char dir[CS_PATH_MAX];
    if (!cs_app_data_dir(dir, sizeof(dir))) return false;
    if (!cs_mkdirs(dir)) return false;
    cs_path_join(out, cap, dir, name);
    return true;
}

static bool read_legacy_key(const char *path, unsigned char sk[crypto_sign_SECRETKEYBYTES],
                            unsigned char pk[crypto_sign_PUBLICKEYBYTES])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char raw[crypto_sign_SECRETKEYBYTES + crypto_sign_PUBLICKEYBYTES];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n == crypto_sign_SECRETKEYBYTES || n == sizeof(raw)) {
        memcpy(sk, raw, crypto_sign_SECRETKEYBYTES);
        if (n == crypto_sign_SECRETKEYBYTES)
            memcpy(pk, sk + crypto_sign_SECRETKEYBYTES - crypto_sign_PUBLICKEYBYTES,
                   crypto_sign_PUBLICKEYBYTES);
        else
            memcpy(pk, raw + crypto_sign_SECRETKEYBYTES, crypto_sign_PUBLICKEYBYTES);
        return true;
    }
    return false;
}

static bool write_legacy_key(const char *path, const unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
#ifdef _WIN32
    FILE *f = fopen(path, "wb");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
#endif
    if (!f) return false;
    fwrite(sk, 1, crypto_sign_SECRETKEYBYTES, f);
    bool ok = fclose(f) == 0;
    if (!ok) return false;
#if !defined(_WIN32)
    chmod(path, 0600);
#endif
    return true;
}

static bool delete_legacy_key(const char *path)
{
#if defined(_WIN32)
    return DeleteFileA(path) != 0;
#else
    return unlink(path) == 0;
#endif
}

#if defined(__APPLE__)
static bool keychain_load(unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, CFSTR(DEVICE_SERVICE));
    CFDictionaryAddValue(query, kSecAttrAccount, CFSTR(DEVICE_ACCOUNT));
    CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = NULL;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFRelease(query);
    bool ok = false;
    if (st == errSecSuccess && result && CFGetTypeID(result) == CFDataGetTypeID() &&
        CFDataGetLength((CFDataRef)result) == crypto_sign_SECRETKEYBYTES) {
        memcpy(sk, CFDataGetBytePtr((CFDataRef)result), crypto_sign_SECRETKEYBYTES);
        ok = true;
    }
    if (result) CFRelease(result);
    return ok;
}

static bool keychain_save(const unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, CFSTR(DEVICE_SERVICE));
    CFDictionaryAddValue(query, kSecAttrAccount, CFSTR(DEVICE_ACCOUNT));
    SecItemDelete(query);
    CFDataRef data = CFDataCreate(NULL, sk, crypto_sign_SECRETKEYBYTES);
    CFDictionaryAddValue(query, kSecValueData, data);
    CFDictionaryAddValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
    OSStatus st = SecItemAdd(query, NULL);
    CFRelease(data);
    CFRelease(query);
    return st == errSecSuccess;
}

static bool keychain_delete(void)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, CFSTR(DEVICE_SERVICE));
    CFDictionaryAddValue(query, kSecAttrAccount, CFSTR(DEVICE_ACCOUNT));
    OSStatus st = SecItemDelete(query);
    CFRelease(query);
    return st == errSecSuccess || st == errSecItemNotFound;
}
#elif defined(_WIN32)
static bool dpapi_load(const char *path, unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 4096) { fclose(f); return false; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    DATA_BLOB in = {(DWORD)got, buf}, out = {0};
    bool ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out) &&
              out.cbData == crypto_sign_SECRETKEYBYTES;
    if (ok) memcpy(sk, out.pbData, crypto_sign_SECRETKEYBYTES);
    if (out.pbData) LocalFree(out.pbData);
    free(buf);
    return ok;
}

static bool dpapi_save(const char *path, const unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    DATA_BLOB in = {crypto_sign_SECRETKEYBYTES, (BYTE *)sk}, out = {0};
    if (!CryptProtectData(&in, L"CellScope device identity", NULL, NULL, NULL, 0, &out))
        return false;
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) {
        ok = fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
        if (fclose(f) != 0) ok = false;
    }
    LocalFree(out.pbData);
    return ok;
}
#endif

static bool secure_key_load(const char *path, unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    (void)path;
    if (getenv("CELLSCOPE_DISABLE_KEYCHAIN")) return false;
#if defined(__APPLE__)
    return keychain_load(sk);
#elif defined(_WIN32)
    return dpapi_load(path, sk);
#else
    (void)path;
    return false;
#endif
}

static bool secure_key_save(const char *path, const unsigned char sk[crypto_sign_SECRETKEYBYTES])
{
    if (getenv("CELLSCOPE_DISABLE_KEYCHAIN")) return write_legacy_key(path, sk);
#if defined(__APPLE__)
    if (keychain_save(sk)) return true;
    return write_legacy_key(path, sk);
#elif defined(_WIN32)
    return dpapi_save(path, sk);
#else
    return write_legacy_key(path, sk);
#endif
}

static void secure_key_delete(const char *path)
{
#if defined(__APPLE__)
    keychain_delete();
    delete_legacy_key(path);
#elif defined(_WIN32)
    delete_legacy_key(path);
#else
    delete_legacy_key(path);
#endif
}

static bool load_or_create_keys(unsigned char pk[crypto_sign_PUBLICKEYBYTES],
                                unsigned char sk[crypto_sign_SECRETKEYBYTES],
                                char *err, size_t cap)
{
    char path[CS_PATH_MAX];
    if (!key_path(path, sizeof(path), "cloud-device.key")) {
        snprintf(err, cap, "Could not determine the device key directory");
        return false;
    }
    if (secure_key_load(path, sk)) {
        memcpy(pk, sk + crypto_sign_SECRETKEYBYTES - crypto_sign_PUBLICKEYBYTES,
               crypto_sign_PUBLICKEYBYTES);
        return true;
    }
    if (read_legacy_key(path, sk, pk)) {
        if (secure_key_save(path, sk)) {
#if !defined(_WIN32) && !defined(__APPLE__)
            /* secure save wrote the same file */
#else
            delete_legacy_key(path);
#endif
        }
        return true;
    }
    if (crypto_sign_keypair(pk, sk) != 0) {
        snprintf(err, cap, "Could not generate the device key");
        return false;
    }
    if (!secure_key_save(path, sk)) {
        snprintf(err, cap, "Could not store the device key securely");
        return false;
    }
    return true;
}

static void hex(const unsigned char *src, size_t n, char *out, size_t cap)
{
    static const char *d = "0123456789abcdef";
    if (cap < n * 2 + 1) return;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[src[i] >> 4];
        out[i * 2 + 1] = d[src[i] & 15];
    }
    out[n * 2] = '\0';
}

static void random_hex(size_t n, char *out, size_t cap)
{
    unsigned char raw[64];
    if (n > sizeof(raw)) n = sizeof(raw);
    randombytes_buf(raw, n);
    hex(raw, n, out, cap);
}

static void b64(const unsigned char *src, size_t n, char *out, size_t cap)
{
    sodium_bin2base64(out, cap, src, n, sodium_base64_VARIANT_ORIGINAL);
}

static long http_json(const char *method, const char *url, const char *body,
                      char **out, char *err, size_t cap)
{
    *out = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, cap, "Could not initialize HTTP");
        return 0;
    }
    Buffer b = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CellScope/0.1");
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        snprintf(err, cap, "%s", curl_easy_strerror(rc));
        free(b.data);
        return 0;
    }
    if (!b.data) b.data = calloc(1, 1);
    *out = b.data;
    return code;
}

static bool http_put_file(const char *url, const char *path, const char *checksum_b64,
                          char *err, size_t cap)
{
    Buffer response = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, cap, "Could not read %s", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, cap, "Could not size %s", path);
        return false;
    }
    long long size = ftell(f);
    rewind(f);
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(f);
        snprintf(err, cap, "Could not initialize upload");
        return false;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    char checksum_header[256];
    snprintf(checksum_header, sizeof(checksum_header), "x-amz-checksum-sha256: %s",
             checksum_b64);
    headers = curl_slist_append(headers, checksum_header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, f);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CellScope/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(f);
    if (rc != CURLE_OK) {
        snprintf(err, cap, "PUT %s failed: %s", url, curl_easy_strerror(rc));
        free(response.data);
        return false;
    }
    if (code < 200 || code >= 300) {
        snprintf(err, cap, "PUT %s returned HTTP %ld: %.300s", url, code,
                 response.data ? response.data : "(empty response)");
        free(response.data);
        return false;
    }
    free(response.data);
    return true;
}

static bool file_hashes(const char *path, char *sha512_out, size_t sha512_cap,
                        char *sha256_b64_out, size_t sha256_cap, long long *size_out,
                        char *err, size_t err_cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_cap, "Could not read %s", path);
        return false;
    }
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha256_state st256;
    crypto_hash_sha256_init(&st256);
    unsigned char buf[65536];
    long long total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crypto_hash_sha512_update(&st, buf, n);
        crypto_hash_sha256_update(&st256, buf, n);
        total += (long long)n;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        snprintf(err, err_cap, "Could not read %s", path);
        return false;
    }
    unsigned char digest[crypto_hash_sha512_BYTES];
    crypto_hash_sha512_final(&st, digest);
    hex(digest, sizeof(digest), sha512_out, sha512_cap);
    unsigned char digest256[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&st256, digest256);
    b64(digest256, sizeof(digest256), sha256_b64_out, sha256_cap);
    if (size_out) *size_out = total;
    return true;
}

static void api_join(char *out, size_t cap, const char *base, const char *path)
{
    cs_strlcpy(out, base, cap);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
    cs_strlcat(out, path, cap);
}

static void api_failure(char *message, size_t cap, const char *method, const char *url,
                        long code, const char *response)
{
    char err[512] = {0};
    if (response && cs_json_get_string(response, "error", err, sizeof(err)) && err[0])
        snprintf(message, cap, "%s %s -> HTTP %ld: %s", method, url, code, err);
    else
        snprintf(message, cap, "%s %s -> HTTP %ld: %.200s", method, url, code,
                 response ? response : "(empty response)");
}

static bool enroll_request(const char *api_base, const char *email, char *message,
                           size_t message_cap)
{
    char err[512] = {0};
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    if (!load_or_create_keys(pk, sk, err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    char public_key[128], nonce[64], device_id[128];
    b64(pk, sizeof(pk), public_key, sizeof(public_key));
    random_hex(16, nonce, sizeof(nonce));
    hex(pk, 16, device_id, sizeof(device_id));

    char issued[32];
    snprintf(issued, sizeof(issued), "%lld", (long long)(cs_epoch_ms() / 1000));
    char canonical[2048];
    snprintf(canonical, sizeof(canonical), "cellscope-v1\nenroll\n%s\n%s\n%s\n%s",
             device_id, email, nonce, issued);
    unsigned char sig[crypto_sign_BYTES];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, (const unsigned char *)canonical,
                             strlen(canonical), sk) != 0) {
        snprintf(message, message_cap, "Could not sign enrollment");
        return false;
    }
    char signature[128];
    b64(sig, (size_t)sig_len, signature, sizeof(signature));

    char body[4096];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"email\":\"%s\",\"public_key\":\"%s\","
             "\"nonce\":\"%s\",\"issued_at\":%s,\"signature\":\"%s\"}",
             device_id, email, public_key, nonce, issued, signature);
    char url[CS_PATH_MAX];
    api_join(url, sizeof(url), api_base, "/v1/enroll");
    char *response = NULL;
    long code = http_json("POST", url, body, &response, err, sizeof(err));
    if (code == 202 || code == 200) {
        snprintf(message, message_cap, "Check %s for the verification link.", email);
        free(response);
        return true;
    }
    api_failure(message, message_cap, "POST", url, code, response);
    free(response);
    return false;
}

bool cloud_enroll(const char *api_base, const char *email, char *message, size_t message_cap)
{
    char err[512] = {0};
    if (!cloud_ready(err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    if (!api_base || !api_base[0] || !email || !email[0]) {
        snprintf(message, message_cap, "Set the upload API URL and email in Settings first.");
        return false;
    }
    return enroll_request(api_base, email, message, message_cap);
}

bool cloud_device_status(const char *api_base, char *status, size_t status_cap,
                         char *message, size_t message_cap)
{
    status[0] = '\0';
    char err[512] = {0};
    if (!cloud_ready(err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    if (!load_or_create_keys(pk, sk, err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    char device_id[128];
    hex(pk, 16, device_id, sizeof(device_id));
    char path[256], url[CS_PATH_MAX];
    snprintf(path, sizeof(path), "/v1/device/%s", device_id);
    api_join(url, sizeof(url), api_base, path);
    char *response = NULL;
    long code = http_json("GET", url, NULL, &response, err, sizeof(err));
    if (code == 200 && cs_json_get_string(response, "status", status, status_cap)) {
        free(response);
        return true;
    }
    api_failure(message, message_cap, "GET", url, code, response);
    free(response);
    return false;
}

bool cloud_upload_archive(const char *api_base, const char *archive_path,
                          const char *sig_path, char *message, size_t message_cap)
{
    char err[1024] = {0};
    if (!cloud_ready(err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    if (!api_base || !api_base[0]) {
        snprintf(message, message_cap, "Set the upload API URL in Settings first.");
        return false;
    }
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    if (!load_or_create_keys(pk, sk, err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    char device_id[128];
    hex(pk, 16, device_id, sizeof(device_id));
    char archive_name[256];
    cs_strlcpy(archive_name, archive_path, sizeof(archive_name));
    char *slash = strrchr(archive_name, '/');
#ifdef _WIN32
    char *bslash = strrchr(archive_name, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) memmove(archive_name, slash + 1, strlen(slash + 1) + 1);
    if (!cs_starts_with(archive_name, "dataset-") ||
        !cs_ends_with(archive_name, ".tar.gz")) {
        snprintf(message, message_cap, "Archive must be a dataset-*.tar.gz file.");
        return false;
    }

    long long archive_size = 0, sig_size = 0;
    char archive_hash[129], sig_hash[129];
    char archive_checksum[128], sig_checksum[128];
    if (!file_hashes(archive_path, archive_hash, sizeof(archive_hash),
                     archive_checksum, sizeof(archive_checksum), &archive_size,
                     err, sizeof(err)) ||
        !file_hashes(sig_path, sig_hash, sizeof(sig_hash),
                     sig_checksum, sizeof(sig_checksum), &sig_size,
                     err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }

    char upload_id[64], nonce[64], issued[32];
    random_hex(16, upload_id, sizeof(upload_id));
    random_hex(16, nonce, sizeof(nonce));
    snprintf(issued, sizeof(issued), "%lld", (long long)(cs_epoch_ms() / 1000));
    char canonical[4096];
    snprintf(canonical, sizeof(canonical),
             "cellscope-v1\nupload\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
             device_id, upload_id, archive_name, archive_hash, archive_checksum,
             sig_checksum, nonce, issued);
    unsigned char sig[crypto_sign_BYTES];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, (const unsigned char *)canonical,
                             strlen(canonical), sk) != 0) {
        snprintf(message, message_cap, "Could not sign upload request");
        return false;
    }
    char signature[128];
    b64(sig, (size_t)sig_len, signature, sizeof(signature));

    char body[8192];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"upload_id\":\"%s\",\"archive_name\":\"%s\","
             "\"archive_sha512\":\"%s\",\"archive_sha256\":\"%s\","
             "\"sig_sha256\":\"%s\",\"nonce\":\"%s\",\"issued_at\":%s,"
             "\"signature\":\"%s\"}",
             device_id, upload_id, archive_name, archive_hash, archive_checksum,
             sig_checksum, nonce, issued, signature);
    char url[CS_PATH_MAX];
    api_join(url, sizeof(url), api_base, "/v1/uploads");
    char *response = NULL;
    long code = http_json("POST", url, body, &response, err, sizeof(err));
    if (code != 201) {
        api_failure(message, message_cap, "POST", url, code, response);
        free(response);
        return false;
    }
    char archive_url[CS_PATH_MAX] = {0}, sig_url[CS_PATH_MAX] = {0};
    bool parsed = cs_json_get_string(response, "archive_url", archive_url, sizeof(archive_url)) &&
                  cs_json_get_string(response, "sig_url", sig_url, sizeof(sig_url));
    free(response);
    if (!parsed) {
        snprintf(message, message_cap, "Upload response was missing object URLs.");
        return false;
    }

    if (!http_put_file(archive_url, archive_path, archive_checksum, err, sizeof(err)) ||
        !http_put_file(sig_url, sig_path, sig_checksum, err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }

    random_hex(16, nonce, sizeof(nonce));
    snprintf(issued, sizeof(issued), "%lld", (long long)(cs_epoch_ms() / 1000));
    snprintf(canonical, sizeof(canonical),
             "cellscope-v1\ncomplete\n%s\n%s\n%s\n%lld\n%lld\n%s\n%s",
             device_id, upload_id, archive_hash, archive_size, sig_size, nonce, issued);
    if (crypto_sign_detached(sig, &sig_len, (const unsigned char *)canonical,
                             strlen(canonical), sk) != 0) {
        snprintf(message, message_cap, "Could not sign completion");
        return false;
    }
    b64(sig, (size_t)sig_len, signature, sizeof(signature));
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"upload_id\":\"%s\",\"archive_sha512\":\"%s\","
             "\"archive_size\":%lld,\"sig_size\":%lld,\"nonce\":\"%s\","
             "\"issued_at\":%s,\"signature\":\"%s\"}",
             device_id, upload_id, archive_hash, archive_size, sig_size, nonce, issued,
             signature);
    char complete_path[256];
    snprintf(complete_path, sizeof(complete_path), "/v1/uploads/%s/complete", upload_id);
    api_join(url, sizeof(url), api_base, complete_path);
    code = http_json("POST", url, body, &response, err, sizeof(err));
    if (code == 200) {
        snprintf(message, message_cap, "Upload accepted by CellScope.");
        free(response);
        return true;
    }
    api_failure(message, message_cap, "POST", url, code, response);
    free(response);
    return false;
}

bool cloud_reset_identity(const char *api_base, char *message, size_t message_cap)
{
    char err[512] = {0};
    if (!cloud_ready(err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    char path[CS_PATH_MAX];
    key_path(path, sizeof(path), "cloud-device.key");
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    if (!load_or_create_keys(pk, sk, err, sizeof(err))) {
        snprintf(message, message_cap, "%s", err);
        return false;
    }
    char device_id[128], nonce[64], issued[32], canonical[1024], signature[128];
    hex(pk, 16, device_id, sizeof(device_id));
    random_hex(16, nonce, sizeof(nonce));
    snprintf(issued, sizeof(issued), "%lld", (long long)(cs_epoch_ms() / 1000));
    snprintf(canonical, sizeof(canonical), "cellscope-v1\nrevoke\n%s\n%s\n%s",
             device_id, nonce, issued);
    unsigned char sig[crypto_sign_BYTES];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, (const unsigned char *)canonical,
                             strlen(canonical), sk) != 0) {
        snprintf(message, message_cap, "Could not sign identity reset");
        return false;
    }
    b64(sig, (size_t)sig_len, signature, sizeof(signature));
    char body[2048], url[CS_PATH_MAX];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"nonce\":\"%s\",\"issued_at\":%s,"
             "\"signature\":\"%s\"}", device_id, nonce, issued, signature);
    if (api_base && api_base[0]) {
        api_join(url, sizeof(url), api_base, "/v1/device/revoke");
        char *response = NULL;
        long code = http_json("POST", url, body, &response, err, sizeof(err));
        if (code != 200) {
            api_failure(message, message_cap, "POST", url, code, response);
            free(response);
            return false;
        }
        free(response);
    }
    secure_key_delete(path);
    snprintf(message, message_cap, "Device identity revoked; a new identity will be created on next enrollment.");
    return true;
}
