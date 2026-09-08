#pragma once

// Completion receives UTF-8 JSON valid only for the duration of the callback.
// The caller owns context until exactly one completion, on the main queue.
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*SocietyIosDriveCompletion)(void *context, const char *json);
void society_ios_drive_request(const char *action, const char *root, const char *identifier,
                               void *context, SocietyIosDriveCompletion completion);
#ifdef __cplusplus
}
#endif
