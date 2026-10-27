/**
 * EOS-LAN Emulator - Lobby Interface (core module)
 *
 * Mirrors src/sessions.c: same LobbyState lifecycle, LAN announce/discovery
 * reuse, callback-queue usage and logging idioms, ported to the EOS Lobby
 * interface that Palworld relies on.
 *
 * This file is the shared owner of the helper symbols declared in
 * internal/lobby_internal.h (lobby_create/destroy/tick, the find_* helpers,
 * generate_lobby_id and lobby_fire_member_status). The other three lobby
 * modules (lobby_modification.c, lobby_details.c, lobby_search.c) call into
 * these and define the EOS_LobbyModification_* / EOS_LobbyDetails_* /
 * EOS_LobbySearch_* / *_Release functions, so none of those are defined here.
 */

#include "eos/eos_lobby.h"
#include "eos/eos_lobby_types.h"
#include "eos/eos_common.h"
#include "internal/lobby_internal.h"
#include "internal/sessions_internal.h"   /* Session struct (LAN wire format) */
#include "internal/connect_internal.h"    /* local users (LAN invites) */
#include "internal/lan_discovery.h"
#include "internal/callbacks.h"
#include "internal/p2p_internal.h"
#include "internal/logging.h"
#include "lan_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Handle magic numbers (must match the values documented in lobby_internal.h
 * and used by the sibling lobby modules). */
#define LOBBY_STATE_MAGIC   0x4C4F4259  /* "LOBY" */
#define LOBBY_MOD_MAGIC     0x4C4D4F44  /* "LMOD" */
#define LOBBY_SEARCH_MAGIC  0x4C534348  /* "LSCH" */
#define LOBBY_DETAILS_MAGIC 0x4C445448  /* "LDTH" */

/* ===========================================================================
 * Stable lobby-id string ring
 *
 * EOS callback info structs carry the lobby id as a const char*. The callback
 * queue copies the info struct by value but NOT the string it points at, and
 * the callback fires on a later EOS_Platform_Tick. A lobby's own storage may be
 * removed (LeaveLobby/DestroyLobby do a memmove on the array) before the queued
 * callback runs, so we must not hand out a pointer into local_lobbies[]. Every
 * LobbyId we put into a queued callback is copied into this small ring first.
 * ===========================================================================*/
#define LOBBY_ID_RING_SIZE 64
static char g_lobby_id_ring[LOBBY_ID_RING_SIZE][LOBBY_ID_LEN + 1];
static int  g_lobby_id_ring_pos = 0;

static const char* stable_lobby_id(const char* id) {
    char* slot;
    if (!id) {
        return NULL;
    }
    slot = g_lobby_id_ring[g_lobby_id_ring_pos];
    g_lobby_id_ring_pos = (g_lobby_id_ring_pos + 1) % LOBBY_ID_RING_SIZE;
    strncpy(slot, id, LOBBY_ID_LEN);
    slot[LOBBY_ID_LEN] = '\0';
    return slot;
}

/* ===========================================================================
 * Small PUID helpers (mirrors how connect.c exposes ProductUserId strings).
 * ===========================================================================*/
static void puid_to_string(EOS_ProductUserId id, char* out, size_t out_size) {
    int32_t len;
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!id) {
        return;
    }
    len = (int32_t)out_size;
    EOS_ProductUserId_ToString(id, out, &len);
}

static bool puid_equals(EOS_ProductUserId a, EOS_ProductUserId b) {
    char sa[LOBBY_OWNER_ID_STRING_LEN];
    char sb[LOBBY_OWNER_ID_STRING_LEN];
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    puid_to_string(a, sa, sizeof(sa));
    puid_to_string(b, sb, sizeof(sb));
    return sa[0] != '\0' && strcmp(sa, sb) == 0;
}

/* ===========================================================================
 * Helpers declared in lobby_internal.h
 * ===========================================================================*/

void generate_lobby_id(char* buffer, size_t buffer_size) {
    static const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t len;
    size_t i;

    if (!buffer || buffer_size == 0) {
        return;
    }

    len = (buffer_size > 32) ? 32 : buffer_size - 1;

    srand((unsigned int)time(NULL) ^ (unsigned int)get_time_ms());

    for (i = 0; i < len; i++) {
        buffer[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buffer[len] = '\0';
}

Lobby* find_local_lobby_by_id(LobbyState* state, const char* id) {
    int i;
    if (!state || !id) {
        return NULL;
    }
    for (i = 0; i < state->local_lobby_count; i++) {
        if (state->local_lobbies[i].valid &&
            strcmp(state->local_lobbies[i].lobby_id, id) == 0) {
            return &state->local_lobbies[i];
        }
    }
    return NULL;
}

Lobby* find_local_lobby_by_owner(LobbyState* state, EOS_ProductUserId owner) {
    char owner_str[LOBBY_OWNER_ID_STRING_LEN];
    int i;
    if (!state || !owner) {
        return NULL;
    }
    puid_to_string(owner, owner_str, sizeof(owner_str));
    if (owner_str[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < state->local_lobby_count; i++) {
        if (state->local_lobbies[i].valid &&
            strcmp(state->local_lobbies[i].owner_id_string, owner_str) == 0) {
            return &state->local_lobbies[i];
        }
    }
    return NULL;
}

LobbyMember* lobby_find_member(Lobby* lobby, EOS_ProductUserId member) {
    char member_str[LOBBY_OWNER_ID_STRING_LEN];
    int i;
    if (!lobby || !member) {
        return NULL;
    }
    puid_to_string(member, member_str, sizeof(member_str));
    for (i = 0; i < lobby->member_count; i++) {
        if (!lobby->members[i].valid) {
            continue;
        }
        if (puid_equals(lobby->members[i].member_id, member)) {
            return &lobby->members[i];
        }
        if (member_str[0] != '\0' &&
            strcmp(lobby->members[i].member_id_string, member_str) == 0) {
            return &lobby->members[i];
        }
    }
    return NULL;
}

/* Append a member to a lobby (no-op if already present or full). */
static LobbyMember* lobby_add_member(Lobby* lobby, EOS_ProductUserId member) {
    LobbyMember* m;
    if (!lobby || !member) {
        return NULL;
    }
    m = lobby_find_member(lobby, member);
    if (m) {
        return m;
    }
    if (lobby->member_count >= MAX_LOBBY_MEMBERS) {
        return NULL;
    }
    m = &lobby->members[lobby->member_count++];
    memset(m, 0, sizeof(*m));
    m->member_id = member;
    puid_to_string(member, m->member_id_string, sizeof(m->member_id_string));
    m->attribute_count = 0;
    m->valid = true;
    return m;
}

/* Remove a member by index (shift remaining down). */
static void lobby_remove_member_at(Lobby* lobby, int index) {
    if (!lobby || index < 0 || index >= lobby->member_count) {
        return;
    }
    memmove(&lobby->members[index],
            &lobby->members[index + 1],
            sizeof(LobbyMember) * (lobby->member_count - index - 1));
    lobby->member_count--;
}

/* ===========================================================================
 * Notification list management
 * ===========================================================================*/
static EOS_NotificationId lobby_add_notification(LobbyState* state,
                                                 LobbyNotificationEntry** list,
                                                 void* callback,
                                                 void* client_data) {
    LobbyNotificationEntry* entry;
    if (!state || !list || !callback) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    entry = calloc(1, sizeof(LobbyNotificationEntry));
    if (!entry) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    entry->id = state->next_notification_id++;
    entry->callback = callback;
    entry->client_data = client_data;
    entry->next = *list;
    *list = entry;
    return entry->id;
}

static void lobby_remove_notification(LobbyNotificationEntry** list, EOS_NotificationId id) {
    if (!list || id == EOS_INVALID_NOTIFICATIONID) {
        return;
    }
    while (*list) {
        if ((*list)->id == id) {
            LobbyNotificationEntry* to_remove = *list;
            *list = (*list)->next;
            free(to_remove);
            return;
        }
        list = &(*list)->next;
    }
}

static void lobby_free_notification_list(LobbyNotificationEntry* list) {
    while (list) {
        LobbyNotificationEntry* next = list->next;
        free(list);
        list = next;
    }
}

/* ===========================================================================
 * Notification fire helpers (all go through the platform callback queue, never
 * inline, exactly like sessions queues its completion callbacks).
 * ===========================================================================*/

void lobby_fire_member_status(LobbyState* state, const char* lobby_id,
                              EOS_ProductUserId target, EOS_ELobbyMemberStatus status) {
    LobbyNotificationEntry* e;
    const char* stable;
    if (!state || !state->platform || !state->platform->callbacks) {
        return;
    }
    stable = stable_lobby_id(lobby_id);
    for (e = state->member_status_notifications; e; e = e->next) {
        EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo info;
        memset(&info, 0, sizeof(info));
        info.ClientData = e->client_data;
        info.LobbyId = stable;
        info.TargetUserId = target;
        info.CurrentStatus = status;
        callback_queue_push(state->platform->callbacks, e->callback, &info, sizeof(info));
    }
    EOS_LOG_DEBUG("Fired member-status %d for lobby %s", (int)status,
                  lobby_id ? lobby_id : "(null)");
}

static void lobby_fire_lobby_update(LobbyState* state, const char* lobby_id) {
    LobbyNotificationEntry* e;
    const char* stable;
    if (!state || !state->platform || !state->platform->callbacks) {
        return;
    }
    stable = stable_lobby_id(lobby_id);
    for (e = state->lobby_update_notifications; e; e = e->next) {
        EOS_Lobby_LobbyUpdateReceivedCallbackInfo info;
        memset(&info, 0, sizeof(info));
        info.ClientData = e->client_data;
        info.LobbyId = stable;
        callback_queue_push(state->platform->callbacks, e->callback, &info, sizeof(info));
    }
}

static void lobby_fire_member_update(LobbyState* state, const char* lobby_id,
                                     EOS_ProductUserId target) {
    LobbyNotificationEntry* e;
    const char* stable;
    if (!state || !state->platform || !state->platform->callbacks) {
        return;
    }
    stable = stable_lobby_id(lobby_id);
    for (e = state->member_update_notifications; e; e = e->next) {
        EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo info;
        memset(&info, 0, sizeof(info));
        info.ClientData = e->client_data;
        info.LobbyId = stable;
        info.TargetUserId = target;
        callback_queue_push(state->platform->callbacks, e->callback, &info, sizeof(info));
    }
}

/* ===========================================================================
 * LAN announce <-> Session wire mapping
 *
 * The shared LAN discovery layer (lan_discovery.c) only knows how to serialize
 * the Session struct. We reuse it by mapping a Lobby onto a Session for the
 * wire. To keep lobby announces from colliding with session announces we run
 * the lobby discovery on a DEDICATED UDP port (discovery_port + 1) -- a
 * distinct announce path, per the contract.
 *
 * LobbyAttribute and SessionAttribute share an identical key buffer, type enum
 * (EOS_EAttributeType) and value union, so attributes copy across directly;
 * lobby attribute visibility maps to session advertisement type.
 * ===========================================================================*/

/* defined further down; the sync helpers queue a re-announce */
static void lobby_request_announce(LobbyState* state);

/* ===========================================================================
 * LAN member sync
 *
 * The announce wire format only carries the owner and a member count, so a
 * remote join stayed invisible to the owner and the owner's later lobby
 * updates never reached joiners (Returnal: the host waits for the member to
 * join, then sets MatchResult; the joiner waits for that update and times
 * out). Lobby announces now reuse the otherwise unused presence records to
 * carry the sender ("__lan_src") and the member list ("__lan_mem"):
 *   - owner: a member re-announcing the joined lobby = join/keep-alive; a
 *     member that stops re-announcing is dropped after LOBBY_STALE_MS;
 *   - member: the owner's announces are authoritative for attributes and
 *     members; when they stop, the lobby is reported CLOSED.
 * Only announces younger than LOBBY_STALE_MS count, because the discovery
 * cache keeps the last packet of every sender forever.
 * ===========================================================================*/
#define LAN_SRC_KEY "__lan_src"
#define LAN_MEM_KEY "__lan_mem"

static void lobby_wire_add_record(Session* s, const char* key, const char* value) {
    PresenceRecord* r;
    if (!value || !value[0] || s->presence_record_count >= MAX_PRESENCE_RECORDS) {
        return;
    }
    r = &s->presence_records[s->presence_record_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->key, key, PRESENCE_KEY_LEN - 1);
    strncpy(r->value, value, PRESENCE_VALUE_LEN - 1);
}

static bool lobby_wire_lists_member(const Session* s, const char* id) {
    int i;
    for (i = 0; i < s->presence_record_count; i++) {
        if (strcmp(s->presence_records[i].key, LAN_MEM_KEY) == 0 &&
            strcmp(s->presence_records[i].value, id) == 0) {
            return true;
        }
    }
    return false;
}

static LobbyMember* lobby_find_member_str(Lobby* l, const char* id) {
    int i;
    for (i = 0; i < l->member_count; i++) {
        if (l->members[i].valid && strcmp(l->members[i].member_id_string, id) == 0) {
            return &l->members[i];
        }
    }
    return NULL;
}

static bool lobby_is_own(const Lobby* l) {
    return l->self_id_string[0] != '\0' && strcmp(l->self_id_string, l->owner_id_string) == 0;
}

static void lobby_sync_from_wire(LobbyState* state, Lobby* l, const Lobby* cand,
                                 const Session* s, uint64_t received_at) {
    const char* src = NULL;
    int i;

    if (!l->self_id_string[0] || !received_at ||
        get_time_ms() - received_at > LOBBY_STALE_MS) {
        return;
    }
    for (i = 0; i < s->presence_record_count; i++) {
        if (strcmp(s->presence_records[i].key, LAN_SRC_KEY) == 0) {
            src = s->presence_records[i].value;
            break;
        }
    }
    if (!src || !src[0] || strcmp(src, l->self_id_string) == 0) {
        return;  /* foreign build without sync, or our own echo */
    }

    if (lobby_is_own(l)) {
        /* We own it: the sender is a member announcing its joined copy. */
        LobbyMember* m = lobby_find_member_str(l, src);
        if (m) {
            if (received_at > m->last_seen) m->last_seen = received_at;
            return;
        }
        if (l->max_members && (uint32_t)l->member_count >= l->max_members) {
            EOS_LOG_WARN("LAN sync: %s wants into full lobby %s", src, l->lobby_id);
            return;
        }
        m = lobby_add_member(l, EOS_ProductUserId_FromString(src));
        if (!m) {
            return;
        }
        m->last_seen = received_at;
        l->last_updated = get_time_ms();
        EOS_LOG_INFO("LAN sync: %s joined our lobby %s (%d members)", src, l->lobby_id, l->member_count);
        lobby_fire_member_status(state, l->lobby_id, m->member_id, EOS_LMS_JOINED);
        lobby_fire_lobby_update(state, l->lobby_id);
        lobby_request_announce(state);
        return;
    }

    if (strcmp(src, l->owner_id_string) != 0) {
        return;  /* only the owner is authoritative for a joined lobby */
    }
    if (received_at > l->last_seen) l->last_seen = received_at;

    {
        bool changed = false;

        if (cand->attribute_count != l->attribute_count ||
            memcmp(cand->attributes, l->attributes,
                   sizeof(LobbyAttribute) * (size_t)cand->attribute_count) != 0) {
            memcpy(l->attributes, cand->attributes, sizeof(l->attributes));
            l->attribute_count = cand->attribute_count;
            changed = true;
        }
        if (cand->max_members != l->max_members || cand->permission_level != l->permission_level) {
            l->max_members = cand->max_members;
            l->permission_level = cand->permission_level;
            changed = true;
        }

        /* Members the owner lists that we did not know yet. */
        for (i = 0; i < s->presence_record_count; i++) {
            const char* id = s->presence_records[i].value;
            if (strcmp(s->presence_records[i].key, LAN_MEM_KEY) != 0 || !id[0]) {
                continue;
            }
            if (!lobby_find_member_str(l, id)) {
                LobbyMember* m = lobby_add_member(l, EOS_ProductUserId_FromString(id));
                if (m) {
                    lobby_fire_member_status(state, l->lobby_id, m->member_id, EOS_LMS_JOINED);
                    changed = true;
                }
            }
        }
        /* Members the owner no longer lists (never drop ourselves: the owner only
         * lists us once it heard our first re-announce). */
        for (i = l->member_count - 1; i >= 0; i--) {
            const char* id = l->members[i].member_id_string;
            if (!id[0] || strcmp(id, l->self_id_string) == 0 || lobby_wire_lists_member(s, id)) {
                continue;
            }
            {
                EOS_ProductUserId gone = l->members[i].member_id;
                lobby_remove_member_at(l, i);
                lobby_fire_member_status(state, l->lobby_id, gone, EOS_LMS_LEFT);
                changed = true;
            }
        }

        if (changed) {
            l->last_updated = get_time_ms();
            EOS_LOG_INFO("LAN sync: lobby %s updated from owner (%d attrs, %d members)",
                         l->lobby_id, l->attribute_count, l->member_count);
            lobby_fire_lobby_update(state, l->lobby_id);
        }
    }
}

/* Owner: drop members that stopped re-announcing. Member: report CLOSED once
 * the owner's announces stop (lobby destroyed or owner gone). */
static void lobby_sync_timeouts(LobbyState* state, uint64_t now) {
    int li;
    for (li = 0; li < state->local_lobby_count; li++) {
        Lobby* l = &state->local_lobbies[li];
        if (!l->valid || !l->self_id_string[0]) {
            continue;
        }
        if (lobby_is_own(l)) {
            int mi;
            for (mi = l->member_count - 1; mi >= 0; mi--) {
                LobbyMember* m = &l->members[mi];
                if (m->last_seen && now - m->last_seen > LOBBY_STALE_MS) {
                    EOS_ProductUserId gone = m->member_id;
                    EOS_LOG_INFO("LAN sync: member %s timed out in lobby %s", m->member_id_string, l->lobby_id);
                    lobby_remove_member_at(l, mi);
                    lobby_fire_member_status(state, l->lobby_id, gone, EOS_LMS_LEFT);
                    lobby_fire_lobby_update(state, l->lobby_id);
                    lobby_request_announce(state);
                }
            }
        } else if (l->last_seen && now - l->last_seen > LOBBY_STALE_MS) {
            LobbyMember* self = lobby_find_member_str(l, l->self_id_string);
            EOS_LOG_INFO("LAN sync: owner stopped announcing lobby %s -> closed", l->lobby_id);
            l->last_seen = 0;
            lobby_fire_member_status(state, l->lobby_id, self ? self->member_id : NULL, EOS_LMS_CLOSED);
        }
    }
}

/* ===========================================================================
 * LAN lobby invites
 *
 * There is no Epic overlay, so invites travel as a broadcast on the lobby
 * discovery port. The invitee's emulator reports InviteReceived and, unless
 * EOSLAN_AUTO_ACCEPT_INVITES=0, accepts it shortly after -- what a click in
 * the Epic overlay would do. The game then copies the lobby by invite id and
 * joins it as usual (LAN member sync takes over from there).
 * ===========================================================================*/
#define MSG_LOBBY_INVITE 0x10
#define INVITE_AUTO_ACCEPT_DELAY_MS 1500
#define INVITE_WAIT_FOR_LOBBY_MS 10000

/* Fixed-size wire payload (char arrays only, no padding). */
typedef struct {
    char invite_id[LOBBY_ID_LEN + 1];
    char lobby_id[LOBBY_ID_LEN + 1];
    char from[LOBBY_OWNER_ID_STRING_LEN];
    char to[LOBBY_OWNER_ID_STRING_LEN];
} LobbyInviteWire;

static EOS_ProductUserId lobby_local_user_by_string(LobbyState* state, const char* id) {
    ConnectState* cs = (state && state->platform) ? state->platform->connect : NULL;
    int i;
    if (!cs || !id || !id[0]) {
        return NULL;
    }
    for (i = 0; i < MAX_LOCAL_USERS; i++) {
        if (cs->users[i].in_use && _stricmp(cs->users[i].user_id.id_string, id) == 0) {
            return (EOS_ProductUserId)&cs->users[i].user_id;
        }
    }
    return NULL;
}

static EOS_ProductUserId lobby_any_local_user(LobbyState* state) {
    ConnectState* cs = (state && state->platform) ? state->platform->connect : NULL;
    int i;
    if (!cs) {
        return NULL;
    }
    for (i = 0; i < MAX_LOCAL_USERS; i++) {
        if (cs->users[i].in_use) {
            return (EOS_ProductUserId)&cs->users[i].user_id;
        }
    }
    return NULL;
}

static LobbyInvite* lobby_find_invite(LobbyState* state, const char* invite_id) {
    int i;
    if (!state || !invite_id) {
        return NULL;
    }
    for (i = 0; i < MAX_LOBBY_INVITES; i++) {
        if (state->invites[i].valid && strcmp(state->invites[i].invite_id, invite_id) == 0) {
            return &state->invites[i];
        }
    }
    return NULL;
}

static bool lobby_is_known(LobbyState* state, const char* lobby_id) {
    int i;
    if (find_local_lobby_by_id(state, lobby_id)) {
        return true;
    }
    for (i = 0; i < state->discovered_lobby_count; i++) {
        if (state->discovered_lobbies[i].valid &&
            strcmp(state->discovered_lobbies[i].lobby_id, lobby_id) == 0) {
            return true;
        }
    }
    return false;
}

static void lobby_on_discovery_raw(void* ctx, uint8_t msg_type, const uint8_t* data, int len,
                                   const char* source_ip) {
    LobbyState* state = (LobbyState*)ctx;
    LobbyInviteWire w;
    LobbyInvite* inv;
    LobbyNotificationEntry* e;
    EOS_ProductUserId local;
    int i;
    int slot = -1;

    if (!state || msg_type != MSG_LOBBY_INVITE || len < (int)sizeof(w)) {
        return;
    }
    memcpy(&w, data, sizeof(w));
    w.invite_id[LOBBY_ID_LEN] = '\0';
    w.lobby_id[LOBBY_ID_LEN] = '\0';
    w.from[LOBBY_OWNER_ID_STRING_LEN - 1] = '\0';
    w.to[LOBBY_OWNER_ID_STRING_LEN - 1] = '\0';

    local = lobby_local_user_by_string(state, w.to);
    if (!local || lobby_find_invite(state, w.invite_id)) {
        return;  /* not for us, or a duplicate of the double send */
    }

    for (i = 0; i < MAX_LOBBY_INVITES; i++) {
        if (!state->invites[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < MAX_LOBBY_INVITES; i++) {
            if (state->invites[i].received_at < state->invites[slot].received_at) {
                slot = i;
            }
        }
    }
    inv = &state->invites[slot];
    memset(inv, 0, sizeof(*inv));
    strncpy(inv->invite_id, w.invite_id, sizeof(inv->invite_id) - 1);
    strncpy(inv->lobby_id, w.lobby_id, sizeof(inv->lobby_id) - 1);
    strncpy(inv->from, w.from, sizeof(inv->from) - 1);
    strncpy(inv->to, w.to, sizeof(inv->to) - 1);
    inv->received_at = get_time_ms();
    inv->accept_at = state->auto_accept_invites ? inv->received_at + INVITE_AUTO_ACCEPT_DELAY_MS : 0;
    inv->valid = true;

    EOS_LOG_INFO("Lobby invite %s received from %s (%s) for lobby %s%s", inv->invite_id, inv->from,
                 source_ip ? source_ip : "?", inv->lobby_id,
                 state->auto_accept_invites ? " - will auto-accept" : "");

    if (!state->platform || !state->platform->callbacks) {
        return;
    }
    for (e = state->invite_received_notifications; e; e = e->next) {
        EOS_Lobby_LobbyInviteReceivedCallbackInfo info;
        memset(&info, 0, sizeof(info));
        info.ClientData = e->client_data;
        info.InviteId = stable_lobby_id(inv->invite_id);
        info.LocalUserId = local;
        info.TargetUserId = EOS_ProductUserId_FromString(inv->from);
        callback_queue_push(state->platform->callbacks, e->callback, &info, sizeof(info));
    }
}

static void lobby_process_invites(LobbyState* state, uint64_t now) {
    int i;
    for (i = 0; i < MAX_LOBBY_INVITES; i++) {
        LobbyInvite* inv = &state->invites[i];
        LobbyNotificationEntry* e;
        EOS_ProductUserId local;

        if (!inv->valid || inv->accepted || !inv->accept_at || now < inv->accept_at) {
            continue;
        }
        if (!lobby_is_known(state, inv->lobby_id)) {
            if (now - inv->received_at < INVITE_WAIT_FOR_LOBBY_MS) {
                continue;  /* lobby announce not heard yet */
            }
            EOS_LOG_WARN("Lobby invite %s: lobby %s never announced, dropping", inv->invite_id, inv->lobby_id);
            inv->accepted = true;
            continue;
        }
        inv->accepted = true;
        local = lobby_local_user_by_string(state, inv->to);
        EOS_LOG_INFO("Lobby invite %s auto-accepted (lobby %s)", inv->invite_id, inv->lobby_id);
        if (!state->platform || !state->platform->callbacks) {
            continue;
        }
        for (e = state->invite_accepted_notifications; e; e = e->next) {
            EOS_Lobby_LobbyInviteAcceptedCallbackInfo info;
            memset(&info, 0, sizeof(info));
            info.ClientData = e->client_data;
            info.InviteId = stable_lobby_id(inv->invite_id);
            info.LocalUserId = local;
            info.TargetUserId = EOS_ProductUserId_FromString(inv->from);
            info.LobbyId = stable_lobby_id(inv->lobby_id);
            callback_queue_push(state->platform->callbacks, e->callback, &info, sizeof(info));
        }
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_SendInvite(EOS_HLobby Handle, const EOS_Lobby_SendInviteOptions* Options,
                                            void* ClientData, const EOS_Lobby_OnSendInviteCallback CompletionDelegate) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    EOS_Lobby_SendInviteCallbackInfo info;

    memset(&info, 0, sizeof(info));
    info.ClientData = ClientData;
    info.ResultCode = EOS_InvalidParameters;

    if (!state || state->magic != LOBBY_STATE_MAGIC || !Options || !Options->LobbyId || !Options->TargetUserId) {
        goto done;
    }
    info.LobbyId = stable_lobby_id(Options->LobbyId);
    if (!find_local_lobby_by_id(state, Options->LobbyId)) {
        info.ResultCode = EOS_NotFound;
        goto done;
    }
    {
        LobbyInviteWire w;
        memset(&w, 0, sizeof(w));
        generate_lobby_id(w.invite_id, sizeof(w.invite_id));
        strncpy(w.lobby_id, Options->LobbyId, LOBBY_ID_LEN);
        puid_to_string(Options->LocalUserId ? Options->LocalUserId : lobby_any_local_user(state),
                       w.from, sizeof(w.from));
        puid_to_string(Options->TargetUserId, w.to, sizeof(w.to));
        /* UDP broadcast: send twice; the invitee drops duplicates by invite id. */
        discovery_broadcast_raw(state->discovery, MSG_LOBBY_INVITE, (const uint8_t*)&w, (int)sizeof(w));
        discovery_broadcast_raw(state->discovery, MSG_LOBBY_INVITE, (const uint8_t*)&w, (int)sizeof(w));
        EOS_LOG_INFO("Lobby invite %s sent: lobby %s from %s to %s", w.invite_id, w.lobby_id, w.from, w.to);
        lobby_request_announce(state);  /* make sure the invitee hears the lobby soon */
    }
    info.ResultCode = EOS_Success;

done:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

static void lobby_to_session(const Lobby* lobby, Session* s) {
    int i;
    memset(s, 0, sizeof(*s));

    strncpy(s->session_id, lobby->lobby_id, sizeof(s->session_id) - 1);
    /* session_name is unused on the lobby path; carry the lobby id for logs. */
    strncpy(s->session_name, lobby->lobby_id, sizeof(s->session_name) - 1);
    strncpy(s->bucket_id, lobby->bucket_id, sizeof(s->bucket_id) - 1);
    strncpy(s->host_address, lobby->host_address, sizeof(s->host_address) - 1);
    strncpy(s->owner_id_string, lobby->owner_id_string, sizeof(s->owner_id_string) - 1);

    s->max_players = lobby->max_members;
    s->registered_player_count = lobby->member_count;
    s->state = EOS_OSS_Pending;
    s->permission_level = (lobby->permission_level == EOS_LPL_PUBLICADVERTISED)
        ? EOS_OSPF_PublicAdvertised : EOS_OSPF_InviteOnly;
    s->join_in_progress_allowed = true;
    s->invites_allowed = lobby->allow_invites;
    s->presence_enabled = lobby->presence_enabled;

    s->attribute_count = lobby->attribute_count;
    if (s->attribute_count > MAX_SESSION_ATTRIBUTES) {
        s->attribute_count = MAX_SESSION_ATTRIBUTES;
    }
    for (i = 0; i < s->attribute_count; i++) {
        const LobbyAttribute* la = &lobby->attributes[i];
        SessionAttribute* sa = &s->attributes[i];
        strncpy(sa->key, la->key, sizeof(sa->key) - 1);
        sa->key[sizeof(sa->key) - 1] = '\0';
        sa->type = la->type;
        memcpy(&sa->value, &la->value, sizeof(sa->value));
        sa->advertisement = (la->visibility == EOS_LAT_PUBLIC)
            ? EOS_SAAT_Advertise : EOS_SAAT_DontAdvertise;
    }

    /* LAN member sync: sender + member list ride in the presence records. */
    s->presence_record_count = 0;
    lobby_wire_add_record(s, LAN_SRC_KEY, lobby->self_id_string);
    for (i = 0; i < lobby->member_count; i++) {
        if (lobby->members[i].valid) {
            lobby_wire_add_record(s, LAN_MEM_KEY, lobby->members[i].member_id_string);
        }
    }

    s->valid = true;
}

static void session_to_lobby(const Session* s, Lobby* lobby) {
    int i;
    memset(lobby, 0, sizeof(*lobby));

    strncpy(lobby->lobby_id, s->session_id, sizeof(lobby->lobby_id) - 1);
    strncpy(lobby->bucket_id, s->bucket_id, sizeof(lobby->bucket_id) - 1);
    strncpy(lobby->host_address, s->host_address, sizeof(lobby->host_address) - 1);
    strncpy(lobby->owner_id_string, s->owner_id_string, sizeof(lobby->owner_id_string) - 1);
    /* owner_id is parsed lazily by the discovered-lobby bookkeeping so we do not
     * leak a fresh ProductUserId every announce. */
    lobby->owner_id = NULL;

    lobby->max_members = s->max_players;
    lobby->member_count = s->registered_player_count;
    if (lobby->member_count > MAX_LOBBY_MEMBERS) {
        lobby->member_count = MAX_LOBBY_MEMBERS;
    }
    if (lobby->member_count < 0) {
        lobby->member_count = 0;
    }
    lobby->permission_level = (s->permission_level == EOS_OSPF_PublicAdvertised)
        ? EOS_LPL_PUBLICADVERTISED : EOS_LPL_INVITEONLY;
    lobby->allow_invites = s->invites_allowed;
    lobby->allow_host_migration = true;
    lobby->allow_join_by_id = true;
    lobby->presence_enabled = s->presence_enabled;

    lobby->attribute_count = s->attribute_count;
    if (lobby->attribute_count > MAX_LOBBY_ATTRIBUTES) {
        lobby->attribute_count = MAX_LOBBY_ATTRIBUTES;
    }
    for (i = 0; i < lobby->attribute_count; i++) {
        const SessionAttribute* sa = &s->attributes[i];
        LobbyAttribute* la = &lobby->attributes[i];
        strncpy(la->key, sa->key, sizeof(la->key) - 1);
        la->key[sizeof(la->key) - 1] = '\0';
        la->type = sa->type;
        memcpy(&la->value, &sa->value, sizeof(la->value));
        la->visibility = (sa->advertisement == EOS_SAAT_Advertise)
            ? EOS_LAT_PUBLIC : EOS_LAT_PRIVATE;
    }

    lobby->valid = true;
}

/* Force a re-announce of local lobbies on the next tick. */
static void lobby_request_announce(LobbyState* state) {
    if (state) {
        state->last_announce_time = 0;
    }
}

/* ===========================================================================
 * LobbyState lifecycle
 * ===========================================================================*/

LobbyState* lobby_create(PlatformState* platform) {
    LobbyState* state;
    uint16_t base_port;
    uint16_t lobby_port;
    const char* broadcast_addr;
    uint32_t interval;

    if (!platform) {
        EOS_LOG_ERROR("Cannot create LobbyState: NULL platform");
        return NULL;
    }

    state = calloc(1, sizeof(LobbyState));
    if (!state) {
        EOS_LOG_ERROR("Failed to allocate LobbyState");
        return NULL;
    }

    state->magic = LOBBY_STATE_MAGIC;
    state->platform = platform;

    base_port = platform->lan_config.discovery_port;
    broadcast_addr = platform->lan_config.broadcast_address;
    interval = platform->lan_config.announcement_interval_ms;

    state->announce_interval_ms = interval ? interval : 2000;
    state->last_announce_time = 0;
    state->next_notification_id = 1;

    /* Dedicated announce port so lobby and session announces never collide. */
    lobby_port = (base_port < 65535) ? (uint16_t)(base_port + 1) : (uint16_t)(base_port - 1);

    state->discovery = discovery_create(lobby_port);
    if (!state->discovery) {
        EOS_LOG_ERROR("Failed to create lobby discovery service on port %u", lobby_port);
        free(state);
        return NULL;
    }

    discovery_set_broadcast_addr(state->discovery, broadcast_addr);
    /* Owner and members announce the same lobby id: keep one cache entry per sender. */
    discovery_set_key_by_source(state->discovery, true);
    /* LAN invites arrive as a custom message type on the lobby port. */
    discovery_set_raw_handler(state->discovery, lobby_on_discovery_raw, state);
    {
        const char* aa = getenv("EOSLAN_AUTO_ACCEPT_INVITES");
        state->auto_accept_invites = !(aa && aa[0] == '0');
    }

    EOS_LOG_INFO("LobbyState created with LAN discovery: port=%u, broadcast=%s, interval=%ums",
                 lobby_port, broadcast_addr, state->announce_interval_ms);
    return state;
}

void lobby_destroy(LobbyState* state) {
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }

    lobby_free_notification_list(state->lobby_update_notifications);
    lobby_free_notification_list(state->member_update_notifications);
    lobby_free_notification_list(state->member_status_notifications);
    lobby_free_notification_list(state->invite_received_notifications);
    lobby_free_notification_list(state->invite_accepted_notifications);
    lobby_free_notification_list(state->join_accepted_notifications);

    if (state->discovery) {
        discovery_destroy(state->discovery);
        state->discovery = NULL;
    }

    state->magic = 0;
    free(state);
}

/* Wire-discovered lobbies carry the member COUNT but not the member entries
 * (only the owner id string is serialized). Seed members[0] with the
 * already-parsed owner so EOS_LobbyDetails_GetMemberByIndex(0) returns the
 * host's ProductUserId instead of NULL — Palworld needs it to identify/join
 * the host; without it the lobby looks invalid and it reports "session is full". */
static void lobby_seed_owner_member(Lobby* l) {
    if (!l || l->member_count < 1) {
        return;
    }
    l->members[0].member_id = l->owner_id;  /* shared pointer (parsed once) */
    strncpy(l->members[0].member_id_string, l->owner_id_string,
            sizeof(l->members[0].member_id_string) - 1);
    l->members[0].member_id_string[sizeof(l->members[0].member_id_string) - 1] = '\0';
    l->members[0].valid = true;
}

void lobby_tick(LobbyState* state) {
    bool should_broadcast_now;
    uint64_t now;
    int cache_count = 0;
    Session* cache;
    int c;

    if (!state || state->magic != LOBBY_STATE_MAGIC || !state->discovery) {
        return;
    }

    /* Drain incoming packets (announcements + queries). */
    discovery_poll(state->discovery);

    should_broadcast_now = discovery_should_broadcast_now(state->discovery);

    now = get_time_ms();
    if (should_broadcast_now ||
        (now - state->last_announce_time >= state->announce_interval_ms)) {
        int i;
        for (i = 0; i < state->local_lobby_count; i++) {
            Lobby* l = &state->local_lobbies[i];
            if (l->valid) {
                Session wire;
                lobby_to_session(l, &wire);
                discovery_broadcast_session(state->discovery, &wire);
                EOS_LOG_DEBUG("Broadcasted lobby: %s", l->lobby_id);
            }
        }
        state->last_announce_time = now;
    }

    lobby_sync_timeouts(state, now);
    lobby_process_invites(state, now);

    /* Convert the discovery cache into discovered_lobbies. We update in place
     * keyed by lobby id and never reorder/remove entries: a discovered lobby's
     * lazily-parsed owner_id pointer can escape into LobbyDetails/search-result
     * snapshots, so it must stay valid for the life of those handles. The owner
     * ProductUserId is therefore parsed at most once per distinct lobby. */
    cache = discovery_get_sessions(state->discovery, &cache_count);
    for (c = 0; c < cache_count; c++) {
        Lobby cand;
        Lobby* slot = NULL;
        int i;

        const Session* ds_entry = discovery_get_session_at(state->discovery, c);
        if (!ds_entry) continue;
        session_to_lobby(ds_entry, &cand);

        /* Our own (created or joined) lobby: announces from other LAN members carry
         * membership/attribute changes (LAN sync); our own echoes are ignored there. */
        {
            Lobby* own = find_local_lobby_by_id(state, cand.lobby_id);
            if (own) {
                lobby_sync_from_wire(state, own, &cand, ds_entry,
                                     discovery_get_received_at(state->discovery, c));
                continue;
            }
        }
        cand.last_seen = discovery_get_received_at(state->discovery, c);

        for (i = 0; i < state->discovered_lobby_count; i++) {
            if (strcmp(state->discovered_lobbies[i].lobby_id, cand.lobby_id) == 0) {
                slot = &state->discovered_lobbies[i];
                break;
            }
        }

        /* Owner and members announce the same id; keep the freshest copy. */
        if (slot && cand.last_seen < slot->last_seen) {
            continue;
        }

        if (slot) {
            EOS_ProductUserId keep = slot->owner_id;  /* preserve parsed owner */
            *slot = cand;
            if (keep) {
                slot->owner_id = keep;
            } else if (slot->owner_id_string[0] != '\0') {
                slot->owner_id = EOS_ProductUserId_FromString(slot->owner_id_string);
            }
            lobby_seed_owner_member(slot);
        } else {
            Lobby* ns = NULL;
            if (state->discovered_lobby_count < MAX_DISCOVERED_LOBBIES) {
                ns = &state->discovered_lobbies[state->discovered_lobby_count++];
            } else {
                /* Full (entries are never removed): recycle the least recently
                 * heard one once it is stale, so new lobbies stay discoverable. */
                int oldest = 0;
                for (i = 1; i < state->discovered_lobby_count; i++) {
                    if (state->discovered_lobbies[i].last_seen < state->discovered_lobbies[oldest].last_seen) {
                        oldest = i;
                    }
                }
                if (get_time_ms() - state->discovered_lobbies[oldest].last_seen > LOBBY_STALE_MS) {
                    ns = &state->discovered_lobbies[oldest];
                }
            }
            if (!ns) {
                continue;
            }
            *ns = cand;
            if (ns->owner_id_string[0] != '\0') {
                ns->owner_id = EOS_ProductUserId_FromString(ns->owner_id_string);
            }
            lobby_seed_owner_member(ns);
        }
    }

    if (state->discovered_lobby_count > 0) {
        EOS_LOG_TRACE("Tracking %d discovered lobbies", state->discovered_lobby_count);
    }
}

/* ===========================================================================
 * Platform interface getter
 * ===========================================================================*/
EOS_DECLARE_FUNC(EOS_HLobby) EOS_Platform_GetLobbyInterface(EOS_HPlatform Handle) { EOS_LOG_TRACE("ENTER");
    PlatformState* platform = (PlatformState*)Handle;
    EOS_LOG_API_ENTER();
    if (!platform) {
        EOS_LOG_ERROR("Invalid platform handle");
        return NULL;
    }
    return (EOS_HLobby)platform->lobby;
}

/* ===========================================================================
 * Core EOS_Lobby_* functions
 * ===========================================================================*/

/* Stringify a lobby attribute into a presence data record (presence records are
 * always key/value strings, lobby attributes are typed). */
static void lobby_attr_to_record(const LobbyAttribute* la, PresenceRecord* out) {
    char val[PRESENCE_VALUE_LEN];
    switch (la->type) {
        case EOS_AT_BOOLEAN: snprintf(val, sizeof(val), "%s", la->value.as_bool ? "true" : "false"); break;
        case EOS_AT_INT64:   snprintf(val, sizeof(val), "%lld", (long long)la->value.as_int64); break;
        case EOS_AT_DOUBLE:  snprintf(val, sizeof(val), "%g", la->value.as_double); break;
        case EOS_AT_STRING:
        default:             snprintf(val, sizeof(val), "%s", la->value.as_string); break;
    }
    strncpy(out->key, la->key, PRESENCE_KEY_LEN - 1);   out->key[PRESENCE_KEY_LEN - 1] = '\0';
    strncpy(out->value, val, PRESENCE_VALUE_LEN - 1);   out->value[PRESENCE_VALUE_LEN - 1] = '\0';
}

/* Mirror a presence-enabled lobby into the local user's published EOS presence so
 * a friend's EOS_Presence_CopyPresence/GetJoinInfo return a joinable record. Real
 * EOS does this automatically for bPresenceEnabled lobbies; games like StarRupture
 * rely on it and never call the Presence interface themselves (their host only
 * creates a presence-enabled lobby with a CUSTOMJOININFO attribute). We expose the
 * lobby id as the join-info (the joiner can EOS_Lobby_JoinLobbyById it) and surface
 * the lobby attributes as presence data records. Records are capped at
 * MAX_PRESENCE_RECORDS, so the join-critical keys are emitted first regardless of
 * attribute order. social_bridge no-ops this if the game manages its own presence. */
static void lobby_sync_local_presence(const Lobby* l) {
    if (!l || !l->presence_enabled) return;

    static const char* const priority[] = {
        "CUSTOMJOININFO", "MapName", "NumPublicConnections", "NumPrivateConnections",
    };
    const int priority_count = (int)(sizeof(priority) / sizeof(priority[0]));

    PresenceRecord recs[MAX_PRESENCE_RECORDS];
    int n = 0;
    bool used[MAX_LOBBY_ATTRIBUTES];
    memset(used, 0, sizeof(used));

    /* Pass 1: priority keys, in priority order. */
    for (int k = 0; k < priority_count && n < MAX_PRESENCE_RECORDS; k++) {
        for (int i = 0; i < l->attribute_count; i++) {
            if (used[i]) continue;
            if (strncmp(l->attributes[i].key, priority[k], MAX_LOBBY_ATTRIBUTE_KEY_LEN) != 0) continue;
            lobby_attr_to_record(&l->attributes[i], &recs[n++]);
            used[i] = true;
            break;
        }
    }
    /* Pass 2: remaining attributes, in order, until the cap. */
    for (int i = 0; i < l->attribute_count && n < MAX_PRESENCE_RECORDS; i++) {
        if (used[i]) continue;
        lobby_attr_to_record(&l->attributes[i], &recs[n++]);
        used[i] = true;
    }

    social_bridge_set_presence_from_lobby(l->lobby_id, recs, n);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_CreateLobby(
    EOS_HLobby Handle,
    const EOS_Lobby_CreateLobbyOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnCreateLobbyCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l = NULL;
    char ip[64];

    EOS_Lobby_CreateLobbyCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }

    if (!Options || !Options->LocalUserId) {
        goto queue_callback;
    }

    /* Be lenient about ApiVersion (the Lobby CreateLobby struct has been
     * revised many times); the game may have been built against an older SDK. */
    if (Options->ApiVersion < EOS_LOBBY_CREATELOBBY_API_LATEST) {
        EOS_LOG_WARN("CreateLobby ApiVersion=%d (expected %d) - proceeding",
                     Options->ApiVersion, EOS_LOBBY_CREATELOBBY_API_LATEST);
    }

    if (state->local_lobby_count >= MAX_LOCAL_LOBBIES) {
        info.ResultCode = EOS_LimitExceeded;
        goto queue_callback;
    }

    l = &state->local_lobbies[state->local_lobby_count];
    memset(l, 0, sizeof(*l));

    /* Identity */
    if (Options->LobbyId && Options->LobbyId[0] != '\0') {
        strncpy(l->lobby_id, Options->LobbyId, sizeof(l->lobby_id) - 1);
    } else {
        generate_lobby_id(l->lobby_id, sizeof(l->lobby_id));
    }

    /* Owner = creating user; auto-joins as first member. */
    l->owner_id = Options->LocalUserId;
    puid_to_string(l->owner_id, l->owner_id_string, sizeof(l->owner_id_string));
    strncpy(l->self_id_string, l->owner_id_string, sizeof(l->self_id_string) - 1);

    /* Configuration */
    l->max_members = Options->MaxLobbyMembers;
    if (l->max_members == 0 || l->max_members > MAX_LOBBY_MEMBERS) {
        l->max_members = MAX_LOBBY_MEMBERS;
    }
    l->permission_level = Options->PermissionLevel;
    l->presence_enabled = (Options->bPresenceEnabled == EOS_TRUE);
    l->allow_invites = (Options->bAllowInvites == EOS_TRUE);
    l->allow_host_migration = (Options->bDisableHostMigration != EOS_TRUE);
    l->allow_join_by_id = (Options->bEnableJoinById == EOS_TRUE);
    l->rtc_room_enabled = (Options->bEnableRTCRoom == EOS_TRUE);
    if (Options->BucketId) {
        strncpy(l->bucket_id, Options->BucketId, sizeof(l->bucket_id) - 1);
    }

    /* Advertise the P2P listen address (IP:p2p_port) so a joining client knows
     * where to send its CONNECT/DATA. Prefer the live bound port/ip from the
     * P2P transport; fall back to the discovery port if P2P isn't up yet. */
    {
        uint16_t p2p_port = state->platform->p2p
                                ? p2p_get_listen_port(state->platform->p2p) : 0;
        const char* p2p_ip = state->platform->p2p
                                 ? p2p_get_listen_ip(state->platform->p2p) : NULL;
        if (p2p_ip && p2p_ip[0] && p2p_port != 0) {
            format_address(l->host_address, sizeof(l->host_address), p2p_ip, p2p_port);
        } else if (get_local_ip(ip, sizeof(ip))) {
            uint16_t port = (p2p_port != 0) ? p2p_port
                                            : state->platform->lan_config.discovery_port;
            format_address(l->host_address, sizeof(l->host_address), ip, port);
        }
    }

    l->created_at = get_time_ms();
    l->last_updated = l->created_at;
    l->valid = true;

    /* Add the owner as the first member. */
    lobby_add_member(l, l->owner_id);

    state->local_lobby_count++;

    /* Start announcing immediately. */
    lobby_request_announce(state);

    /* A presence-enabled lobby ties to the local user's presence (real EOS).
     * Attributes are added later via UpdateLobby; this seeds the join-info now. */
    lobby_sync_local_presence(l);

    info.ResultCode = EOS_Success;
    info.LobbyId = stable_lobby_id(l->lobby_id);

    EOS_LOG_INFO("Lobby created: %s (owner=%s, max=%u)",
                 l->lobby_id, l->owner_id_string, l->max_members);

    /* Owner JOINED notification. */
    lobby_fire_member_status(state, l->lobby_id, l->owner_id, EOS_LMS_JOINED);

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_DestroyLobby(
    EOS_HLobby Handle,
    const EOS_Lobby_DestroyLobbyOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnDestroyLobbyCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    int index;

    EOS_Lobby_DestroyLobbyCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyId) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(Options->LobbyId);

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Only the owner may destroy. */
    if (Options->LocalUserId && !puid_equals(Options->LocalUserId, l->owner_id)) {
        info.ResultCode = EOS_Lobby_NotOwner;
        goto queue_callback;
    }

    /* This lobby drove our presence join-info; drop it so friends stop seeing us
     * as joinable. */
    if (l->presence_enabled) social_bridge_clear_lobby_presence();

    index = (int)(l - state->local_lobbies);
    if (index >= 0 && index < state->local_lobby_count) {
        memmove(&state->local_lobbies[index],
                &state->local_lobbies[index + 1],
                sizeof(Lobby) * (state->local_lobby_count - index - 1));
        state->local_lobby_count--;
    }

    info.ResultCode = EOS_Success;
    EOS_LOG_INFO("Lobby destroyed: %s", Options->LobbyId);

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

/* Shared body for JoinLobby / JoinLobbyById: install a snapshot as a local
 * lobby, add the local user, register the host address for P2P, fire JOINED. */
static EOS_EResult lobby_join_common(LobbyState* state, const Lobby* snapshot,
                                     EOS_ProductUserId local_user, bool presence_enabled) {
    Lobby* l;

    if (!snapshot || !local_user) {
        return EOS_InvalidParameters;
    }

    /* Already joined? Treat as success (idempotent). */
    l = find_local_lobby_by_id(state, snapshot->lobby_id);
    if (l) {
        lobby_add_member(l, local_user);
        return EOS_Success;
    }

    if (state->local_lobby_count >= MAX_LOCAL_LOBBIES) {
        return EOS_LimitExceeded;
    }

    l = &state->local_lobbies[state->local_lobby_count];
    *l = *snapshot;
    l->valid = true;
    l->presence_enabled = presence_enabled;
    l->created_at = get_time_ms();
    l->last_updated = l->created_at;
    l->source_ip[0] = '\0';
    l->last_seen = 0;

    /* Reconstruct the owner ProductUserId if the snapshot came from a discovered
     * lobby (owner stored only as a string on the wire). */
    if (!l->owner_id && l->owner_id_string[0] != '\0') {
        l->owner_id = EOS_ProductUserId_FromString(l->owner_id_string);
    }

    /* Add the local user as a member. */
    lobby_add_member(l, local_user);
    memset(l->self_id_string, 0, sizeof(l->self_id_string));
    puid_to_string(local_user, l->self_id_string, sizeof(l->self_id_string));

    /* Register the host address into the P2P address book so P2P can route to
     * the lobby owner (analogous to a session join). */
    if (state->platform && state->platform->p2p && l->owner_id &&
        l->host_address[0] != '\0') {
        p2p_register_peer_address(state->platform->p2p, l->owner_id, l->host_address);
    }

    state->local_lobby_count++;
    lobby_request_announce(state);

    /* A joiner of a presence-enabled lobby also reflects it in its own presence
     * (real EOS), so its friends can in turn join through it. */
    lobby_sync_local_presence(l);

    lobby_fire_member_status(state, l->lobby_id, local_user, EOS_LMS_JOINED);
    return EOS_Success;
}

EOS_DECLARE_FUNC(void) EOS_Lobby_JoinLobby(
    EOS_HLobby Handle,
    const EOS_Lobby_JoinLobbyOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnJoinLobbyCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    LobbyDetailsHandle* details;

    EOS_Lobby_JoinLobbyCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyDetailsHandle || !Options->LocalUserId) {
        goto queue_callback;
    }

    details = (LobbyDetailsHandle*)Options->LobbyDetailsHandle;
    if (!details || details->magic != LOBBY_DETAILS_MAGIC) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(details->lobby.lobby_id);
    info.ResultCode = lobby_join_common(state, &details->lobby, Options->LocalUserId,
                                        Options->bPresenceEnabled == EOS_TRUE);

    if (info.ResultCode == EOS_Success) {
        EOS_LOG_INFO("Joined lobby: %s", details->lobby.lobby_id);
    }

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_JoinLobbyById(
    EOS_HLobby Handle,
    const EOS_Lobby_JoinLobbyByIdOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnJoinLobbyByIdCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* found = NULL;

    EOS_Lobby_JoinLobbyByIdCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyId || !Options->LocalUserId) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(Options->LobbyId);

    /* Look in local lobbies first, then discovered ones. */
    found = find_local_lobby_by_id(state, Options->LobbyId);
    if (!found) {
        int i;
        for (i = 0; i < state->discovered_lobby_count; i++) {
            if (state->discovered_lobbies[i].valid &&
                strcmp(state->discovered_lobbies[i].lobby_id, Options->LobbyId) == 0) {
                found = &state->discovered_lobbies[i];
                break;
            }
        }
    }

    if (!found) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    info.ResultCode = lobby_join_common(state, found, Options->LocalUserId,
                                        Options->bPresenceEnabled == EOS_TRUE);

    if (info.ResultCode == EOS_Success) {
        EOS_LOG_INFO("Joined lobby by id: %s", Options->LobbyId);
    }

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_LeaveLobby(
    EOS_HLobby Handle,
    const EOS_Lobby_LeaveLobbyOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLeaveLobbyCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    int index;

    EOS_Lobby_LeaveLobbyCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyId || !Options->LocalUserId) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(Options->LobbyId);

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Notify before tearing down: the LobbyId is taken from the stable ring and
     * TargetUserId is the caller-owned LocalUserId, so neither dangles. */
    lobby_fire_member_status(state, Options->LobbyId, Options->LocalUserId, EOS_LMS_LEFT);

    if (l->presence_enabled) social_bridge_clear_lobby_presence();

    index = (int)(l - state->local_lobbies);
    if (index >= 0 && index < state->local_lobby_count) {
        memmove(&state->local_lobbies[index],
                &state->local_lobbies[index + 1],
                sizeof(Lobby) * (state->local_lobby_count - index - 1));
        state->local_lobby_count--;
    }

    info.ResultCode = EOS_Success;
    EOS_LOG_INFO("Left lobby: %s", Options->LobbyId);

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_UpdateLobbyModification(
    EOS_HLobby Handle,
    const EOS_Lobby_UpdateLobbyModificationOptions* Options,
    EOS_HLobbyModification* OutLobbyModificationHandle
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    LobbyModificationHandle* mod;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !Options->LobbyId || !OutLobbyModificationHandle) {
        return EOS_InvalidParameters;
    }

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        return EOS_NotFound;
    }

    mod = calloc(1, sizeof(LobbyModificationHandle));
    if (!mod) {
        return EOS_LimitExceeded;
    }

    mod->magic = LOBBY_MOD_MAGIC;
    mod->is_new = false;
    mod->local_user = Options->LocalUserId;
    strncpy(mod->lobby_id, l->lobby_id, sizeof(mod->lobby_id) - 1);
    /* Seed staged with the current lobby so partial setters compose correctly. */
    mod->staged = *l;
    mod->member_attribute_count = 0;

    *OutLobbyModificationHandle = (EOS_HLobbyModification)mod;
    return EOS_Success;
}

EOS_DECLARE_FUNC(void) EOS_Lobby_UpdateLobby(
    EOS_HLobby Handle,
    const EOS_Lobby_UpdateLobbyOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnUpdateLobbyCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    LobbyModificationHandle* mod;
    Lobby* l;
    bool member_attrs_applied = false;
    EOS_ProductUserId updated_member = NULL;

    EOS_Lobby_UpdateLobbyCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyModificationHandle) {
        goto queue_callback;
    }

    mod = (LobbyModificationHandle*)Options->LobbyModificationHandle;
    if (!mod || mod->magic != LOBBY_MOD_MAGIC) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(mod->lobby_id);

    l = find_local_lobby_by_id(state, mod->lobby_id);
    if (!l) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Consume staged lobby-level config + attributes, preserving identity and
     * live membership. */
    if (mod->staged.bucket_id[0] != '\0') {
        strncpy(l->bucket_id, mod->staged.bucket_id, sizeof(l->bucket_id) - 1);
        l->bucket_id[sizeof(l->bucket_id) - 1] = '\0';
    }
    if (mod->staged.max_members > 0) {
        l->max_members = mod->staged.max_members;
    }
    l->permission_level = mod->staged.permission_level;
    l->allow_invites = mod->staged.allow_invites;
    l->attribute_count = mod->staged.attribute_count;
    if (l->attribute_count > MAX_LOBBY_ATTRIBUTES) {
        l->attribute_count = MAX_LOBBY_ATTRIBUTES;
    }
    memcpy(l->attributes, mod->staged.attributes,
           sizeof(LobbyAttribute) * l->attribute_count);
    EOS_LOG_INFO("UpdateLobby: committed %d attribute(s) to lobby %s",
                 l->attribute_count, l->lobby_id);

    /* Apply staged local-member attributes to the modifying member. */
    if (mod->member_attribute_count > 0 && mod->local_user) {
        LobbyMember* m = lobby_find_member(l, mod->local_user);
        if (m) {
            int n = mod->member_attribute_count;
            if (n > MAX_MEMBER_ATTRIBUTES) {
                n = MAX_MEMBER_ATTRIBUTES;
            }
            memcpy(m->attributes, mod->member_attributes, sizeof(LobbyAttribute) * n);
            m->attribute_count = n;
            member_attrs_applied = true;
            updated_member = mod->local_user;
        }
    }

    l->last_updated = get_time_ms();
    lobby_request_announce(state);

    info.ResultCode = EOS_Success;
    EOS_LOG_INFO("Lobby updated: %s", l->lobby_id);

    /* Re-derive presence now that the lobby's attributes (CUSTOMJOININFO, MapName,
     * conn counts) are committed, so friends' CopyPresence sees the join records. */
    lobby_sync_local_presence(l);

    /* Notify listeners that the lobby (and possibly a member) changed. */
    lobby_fire_lobby_update(state, l->lobby_id);
    if (member_attrs_applied) {
        lobby_fire_member_update(state, l->lobby_id, updated_member);
    }

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_PromoteMember(
    EOS_HLobby Handle,
    const EOS_Lobby_PromoteMemberOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnPromoteMemberCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    LobbyMember* target;

    EOS_Lobby_PromoteMemberCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyId || !Options->TargetUserId) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(Options->LobbyId);

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Only the current owner may promote. */
    if (Options->LocalUserId && !puid_equals(Options->LocalUserId, l->owner_id)) {
        info.ResultCode = EOS_Lobby_NotOwner;
        goto queue_callback;
    }

    target = lobby_find_member(l, Options->TargetUserId);
    if (!target) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    l->owner_id = Options->TargetUserId;
    puid_to_string(l->owner_id, l->owner_id_string, sizeof(l->owner_id_string));
    l->last_updated = get_time_ms();
    lobby_request_announce(state);

    info.ResultCode = EOS_Success;
    EOS_LOG_INFO("Promoted member %s in lobby %s", l->owner_id_string, l->lobby_id);

    lobby_fire_member_status(state, Options->LobbyId, Options->TargetUserId, EOS_LMS_PROMOTED);

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_KickMember(
    EOS_HLobby Handle,
    const EOS_Lobby_KickMemberOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnKickMemberCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    LobbyMember* target;

    EOS_Lobby_KickMemberCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_InvalidParameters;
    info.ClientData = ClientData;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        goto queue_callback;
    }
    if (!Options || !Options->LobbyId || !Options->TargetUserId) {
        goto queue_callback;
    }

    info.LobbyId = stable_lobby_id(Options->LobbyId);

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Only the owner may kick. */
    if (Options->LocalUserId && !puid_equals(Options->LocalUserId, l->owner_id)) {
        info.ResultCode = EOS_Lobby_NotOwner;
        goto queue_callback;
    }

    target = lobby_find_member(l, Options->TargetUserId);
    if (!target) {
        info.ResultCode = EOS_NotFound;
        goto queue_callback;
    }

    /* Notify before removing the member. */
    lobby_fire_member_status(state, Options->LobbyId, Options->TargetUserId, EOS_LMS_KICKED);

    lobby_remove_member_at(l, (int)(target - l->members));
    l->last_updated = get_time_ms();
    lobby_request_announce(state);

    info.ResultCode = EOS_Success;
    EOS_LOG_INFO("Kicked member from lobby %s", l->lobby_id);

queue_callback:
    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_CreateLobbySearch(
    EOS_HLobby Handle,
    const EOS_Lobby_CreateLobbySearchOptions* Options,
    EOS_HLobbySearch* OutLobbySearchHandle
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    LobbySearchHandle* search;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !OutLobbySearchHandle) {
        return EOS_InvalidParameters;
    }

    search = calloc(1, sizeof(LobbySearchHandle));
    if (!search) {
        return EOS_LimitExceeded;
    }

    search->magic = LOBBY_SEARCH_MAGIC;
    search->lobby_state = state;
    search->max_results = (Options->MaxResults > 0) ? Options->MaxResults : 10;
    if (search->max_results > EOS_LOBBY_MAX_SEARCH_RESULTS) {
        search->max_results = EOS_LOBBY_MAX_SEARCH_RESULTS;
    }

    *OutLobbySearchHandle = (EOS_HLobbySearch)search;
    return EOS_Success;
}

/* Build a LobbyDetails snapshot for an existing local or discovered lobby. */
static EOS_EResult lobby_copy_details(LobbyState* state, const char* lobby_id,
                                      EOS_ProductUserId local_user,
                                      EOS_HLobbyDetails* out) {
    Lobby* found;
    LobbyDetailsHandle* details;

    if (!out) {
        return EOS_InvalidParameters;
    }

    found = find_local_lobby_by_id(state, lobby_id);
    if (!found) {
        int i;
        for (i = 0; i < state->discovered_lobby_count; i++) {
            if (state->discovered_lobbies[i].valid &&
                strcmp(state->discovered_lobbies[i].lobby_id, lobby_id) == 0) {
                found = &state->discovered_lobbies[i];
                break;
            }
        }
    }

    if (!found) {
        return EOS_NotFound;
    }

    details = calloc(1, sizeof(LobbyDetailsHandle));
    if (!details) {
        return EOS_LimitExceeded;
    }

    details->magic = LOBBY_DETAILS_MAGIC;
    details->lobby = *found;
    details->local_user = local_user;

    /* Ensure the snapshot has a usable owner ProductUserId. */
    if (!details->lobby.owner_id && details->lobby.owner_id_string[0] != '\0') {
        details->lobby.owner_id = EOS_ProductUserId_FromString(details->lobby.owner_id_string);
    }

    *out = (EOS_HLobbyDetails)details;
    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_CopyLobbyDetailsHandle(
    EOS_HLobby Handle,
    const EOS_Lobby_CopyLobbyDetailsHandleOptions* Options,
    EOS_HLobbyDetails* OutLobbyDetailsHandle
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !Options->LobbyId || !OutLobbyDetailsHandle) {
        return EOS_InvalidParameters;
    }

    return lobby_copy_details(state, Options->LobbyId, Options->LocalUserId,
                              OutLobbyDetailsHandle);
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_CopyLobbyDetailsHandleByInviteId(
    EOS_HLobby Handle,
    const EOS_Lobby_CopyLobbyDetailsHandleByInviteIdOptions* Options,
    EOS_HLobbyDetails* OutLobbyDetailsHandle
) { EOS_LOG_TRACE("ENTER");
    /* Invites arrive over the LAN (see "LAN lobby invites"). */
    LobbyState* state = (LobbyState*)Handle;
    LobbyInvite* inv;
    if (OutLobbyDetailsHandle) {
        *OutLobbyDetailsHandle = NULL;
    }
    if (!state || state->magic != LOBBY_STATE_MAGIC || !Options || !Options->InviteId || !OutLobbyDetailsHandle) {
        return EOS_InvalidParameters;
    }
    inv = lobby_find_invite(state, Options->InviteId);
    if (!inv) {
        EOS_LOG_WARN("CopyLobbyDetailsHandleByInviteId: unknown invite %s", Options->InviteId);
        return EOS_NotFound;
    }
    return lobby_copy_details(state, inv->lobby_id, lobby_local_user_by_string(state, inv->to),
                              OutLobbyDetailsHandle);
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_CopyLobbyDetailsHandleByUiEventId(
    EOS_HLobby Handle,
    const EOS_Lobby_CopyLobbyDetailsHandleByUiEventIdOptions* Options,
    EOS_HLobbyDetails* OutLobbyDetailsHandle
) { EOS_LOG_TRACE("ENTER");
    /* No social-overlay UI events in LAN mode. */
    (void)Handle;
    (void)Options;
    if (OutLobbyDetailsHandle) {
        *OutLobbyDetailsHandle = NULL;
    }
    EOS_LOG_DEBUG("CopyLobbyDetailsHandleByUiEventId: no UI events in LAN mode");
    return EOS_NotFound;
}

/* ===========================================================================
 * Connect string (best-effort; the lobby id is the connect string)
 * ===========================================================================*/
EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_GetConnectString(
    EOS_HLobby Handle,
    const EOS_Lobby_GetConnectStringOptions* Options,
    char* OutBuffer,
    uint32_t* InOutBufferLength
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;
    uint32_t required;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !Options->LobbyId || !OutBuffer || !InOutBufferLength) {
        return EOS_InvalidParameters;
    }

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        return EOS_NotFound;
    }

    required = (uint32_t)strlen(l->lobby_id) + 1;
    if (*InOutBufferLength < required) {
        *InOutBufferLength = required;
        return EOS_LimitExceeded;
    }

    strcpy(OutBuffer, l->lobby_id);
    *InOutBufferLength = required;
    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_ParseConnectString(
    EOS_HLobby Handle,
    const EOS_Lobby_ParseConnectStringOptions* Options,
    char* OutBuffer,
    uint32_t* InOutBufferLength
) { EOS_LOG_TRACE("ENTER");
    uint32_t required;

    (void)Handle;
    if (!Options || !Options->ConnectString || !OutBuffer || !InOutBufferLength) {
        return EOS_InvalidParameters;
    }

    /* GetConnectString emits the lobby id verbatim, so parsing is a pass-through. */
    required = (uint32_t)strlen(Options->ConnectString) + 1;
    if (*InOutBufferLength < required) {
        *InOutBufferLength = required;
        return EOS_LimitExceeded;
    }

    strcpy(OutBuffer, Options->ConnectString);
    *InOutBufferLength = required;
    return EOS_Success;
}

/* ===========================================================================
 * RTC room functions - safe stubs (LAN emulator has no RTC voice; Palworld
 * does not require it). Lobbies report no RTC room.
 * ===========================================================================*/
EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_GetRTCRoomName(
    EOS_HLobby Handle,
    const EOS_Lobby_GetRTCRoomNameOptions* Options,
    char* OutBuffer,
    uint32_t* InOutBufferLength
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    Lobby* l;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !Options->LobbyId || !OutBuffer || !InOutBufferLength) {
        return EOS_InvalidParameters;
    }

    l = find_local_lobby_by_id(state, Options->LobbyId);
    if (!l) {
        return EOS_NotFound;
    }

    /* No RTC room is ever created in the emulator. */
    if (OutBuffer && *InOutBufferLength > 0) {
        OutBuffer[0] = '\0';
    }
    *InOutBufferLength = 0;
    return EOS_Disabled;
}

EOS_DECLARE_FUNC(void) EOS_Lobby_JoinRTCRoom(
    EOS_HLobby Handle,
    const EOS_Lobby_JoinRTCRoomOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnJoinRTCRoomCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;

    EOS_Lobby_JoinRTCRoomCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_Disabled;  /* no RTC in LAN mode */
    info.ClientData = ClientData;
    info.LobbyId = (Options && Options->LobbyId) ? stable_lobby_id(Options->LobbyId) : NULL;

    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(void) EOS_Lobby_LeaveRTCRoom(
    EOS_HLobby Handle,
    const EOS_Lobby_LeaveRTCRoomOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLeaveRTCRoomCallback CompletionDelegate
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;

    EOS_Lobby_LeaveRTCRoomCallbackInfo info;
    memset(&info, 0, sizeof(info));
    info.ResultCode = EOS_Disabled;
    info.ClientData = ClientData;
    info.LobbyId = (Options && Options->LobbyId) ? stable_lobby_id(Options->LobbyId) : NULL;

    if (CompletionDelegate && state && state->platform && state->platform->callbacks) {
        callback_queue_push(state->platform->callbacks, (void*)CompletionDelegate, &info, sizeof(info));
    }
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_Lobby_IsRTCRoomConnected(
    EOS_HLobby Handle,
    const EOS_Lobby_IsRTCRoomConnectedOptions* Options,
    EOS_Bool* bOutIsConnected
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;

    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_InvalidParameters;
    }
    if (!Options || !Options->LobbyId || !bOutIsConnected) {
        return EOS_InvalidParameters;
    }

    *bOutIsConnected = EOS_FALSE;

    if (!find_local_lobby_by_id(state, Options->LobbyId)) {
        return EOS_NotFound;
    }
    return EOS_Disabled;  /* lobby exists but has no RTC room */
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyRTCRoomConnectionChanged(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyRTCRoomConnectionChangedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnRTCRoomConnectionChangedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    /* No RTC rooms exist, so this notification can never fire. */
    (void)Handle;
    (void)Options;
    (void)ClientData;
    (void)NotificationFn;
    return EOS_INVALID_NOTIFICATIONID;
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyRTCRoomConnectionChanged(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    (void)Handle;
    (void)InId;
}

/* ===========================================================================
 * Notification register/remove pairs
 *
 * These are REAL: they store handlers into the LobbyState linked lists and hand
 * out ids from next_notification_id. lobby_fire_member_status / the update and
 * announce paths above walk these lists and queue the callbacks.
 * ===========================================================================*/

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyLobbyUpdateReceived(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyLobbyUpdateReceivedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLobbyUpdateReceivedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->lobby_update_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyLobbyUpdateReceived(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->lobby_update_notifications, InId);
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLobbyMemberUpdateReceivedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->member_update_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->member_update_notifications, InId);
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLobbyMemberStatusReceivedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->member_status_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->member_status_notifications, InId);
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyLobbyInviteReceived(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyLobbyInviteReceivedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLobbyInviteReceivedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->invite_received_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyLobbyInviteReceived(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->invite_received_notifications, InId);
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyLobbyInviteAccepted(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyLobbyInviteAcceptedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnLobbyInviteAcceptedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->invite_accepted_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyLobbyInviteAccepted(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->invite_accepted_notifications, InId);
}

EOS_DECLARE_FUNC(EOS_NotificationId) EOS_Lobby_AddNotifyJoinLobbyAccepted(
    EOS_HLobby Handle,
    const EOS_Lobby_AddNotifyJoinLobbyAcceptedOptions* Options,
    void* ClientData,
    const EOS_Lobby_OnJoinLobbyAcceptedCallback NotificationFn
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    (void)Options;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return EOS_INVALID_NOTIFICATIONID;
    }
    return lobby_add_notification(state, &state->join_accepted_notifications,
                                  (void*)NotificationFn, ClientData);
}

EOS_DECLARE_FUNC(void) EOS_Lobby_RemoveNotifyJoinLobbyAccepted(
    EOS_HLobby Handle,
    EOS_NotificationId InId
) { EOS_LOG_TRACE("ENTER");
    LobbyState* state = (LobbyState*)Handle;
    if (!state || state->magic != LOBBY_STATE_MAGIC) {
        return;
    }
    lobby_remove_notification(&state->join_accepted_notifications, InId);
}
