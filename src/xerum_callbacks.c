/**
 * EOS-LAN Emulator - Completion helper for former no-op stubs
 *
 * Several stubs (Achievements, Stats, Leaderboards, Ecom, UI, Lobby invites)
 * used to return without ever firing their completion delegate, which leaves
 * games waiting forever (e.g. Returnal's ownership and achievement queries).
 * They now report success with empty results through this helper.
 *
 * Kept in its own translation unit so the stub files don't need
 * platform_internal.h (whose includes clash with their void* signatures).
 */

#include "eos/eos_sdk.h"
#include "internal/platform_internal.h"
#include "internal/callbacks.h"
#include "internal/logging.h"
#include <string.h>
#include <stdint.h>

void xerum_queue_completion(void* handle, void* callback_fn, const void* info, size_t info_size) {
    PlatformState* p = (PlatformState*)handle;
    if (!callback_fn || !p || !p->callbacks) {
        EOS_LOG_WARN("xerum_queue_completion: no platform/callback (handle=%p)", handle);
        return;
    }
    callback_queue_push(p->callbacks, callback_fn, info, info_size);
}

/* Generic completion for stubs whose callback info we do not model: every EOS
 * *CallbackInfo starts with { EOS_EResult ResultCode; void* ClientData; ... }, so a
 * zeroed buffer with those two set reads as "result, no ids, zero counts" for any
 * of them. Without it such calls never complete and games wait forever (Remnant II
 * drops back to the main menu after its TitleStorage query never answers). */
void xerum_complete_generic(void* handle, void* callback_fn, void* client_data, int result) {
    union {
        struct { int32_t ResultCode; void* ClientData; } head;
        uint8_t raw[512];
    } info;
    memset(&info, 0, sizeof(info));
    info.head.ResultCode = result;
    info.head.ClientData = client_data;
    xerum_queue_completion(handle, callback_fn, &info, sizeof(info));
}
