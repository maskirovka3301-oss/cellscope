#ifndef CELLSCOPE_CLOUD_H
#define CELLSCOPE_CLOUD_H

#include <stdbool.h>
#include <stddef.h>

/* Enroll this device by email. Prints a user-facing result in message. */
bool cloud_enroll(const char *api_base, const char *email, char *message, size_t message_cap);

/* Refresh and return the device status: pending, active, or unknown. */
bool cloud_device_status(const char *api_base, char *status, size_t status_cap,
                         char *message, size_t message_cap);

/* Upload an exported dataset archive and its .sig sidecar. */
bool cloud_upload_archive(const char *api_base, const char *archive_path,
                          const char *sig_path, char *message, size_t message_cap);

/* Revoke the current device server-side and delete its local private key. */
bool cloud_reset_identity(const char *api_base, char *message, size_t message_cap);

#endif
