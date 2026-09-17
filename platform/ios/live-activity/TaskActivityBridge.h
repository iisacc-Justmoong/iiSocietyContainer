#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Strings are copied synchronously. Publishing is serialized on the main actor.
void society_activity_restore(void);
void society_activity_begin(const char *task, const char *title, const char *symbol, const char *detail);
void society_activity_update(const char *task, const char *detail, int64_t completed, int64_t total, const char *phase);
void society_activity_finish(const char *task, const char *state);
// A new explicit user request can enable a previously dismissed sync card.
void society_activity_allow_restart(const char *task);
#ifdef __cplusplus
}
#endif
