#include "internal/lan_discovery.h"
#include "lan_common.h"
#include "internal/sessions_internal.h"
#include "internal/logging.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define close closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#define DISCOVERY_MAGIC "EOSLAN"
// v2: announce carries the host's presence join-info string + data records
// (fixed-size block between the session flags and the attribute list).
#define DISCOVERY_VERSION 0x0002
#define MSG_ANNOUNCE 0x01
#define MSG_QUERY 0x02
#define MSG_USER_BEACON 0x03

#define MAX_CACHED_SESSIONS 64
/* Lobbies carry many attributes (Palworld sets ~24, each string value
 * serialized as a fixed 256 bytes -> ~8 KB). 4096 truncated the announce so
 * the client parsed 0 attributes. Loopback/LAN UDP handles larger datagrams. */
#define MAX_PACKET_SIZE 65536

typedef struct {
    Session session;
    char source_ip[16];
    uint64_t received_at;
} CachedSession;

struct DiscoveryService {
#ifdef _WIN32
    SOCKET socket_fd;
#else
    int socket_fd;
#endif
    uint16_t port;
    char broadcast_addr[16];
    bool localhost_mode;  // Enable localhost unicast for Wine/Proton
    bool query_received;  // Flag to indicate a query was received and we should broadcast
    bool key_by_source;   // cache key = session id + source ip (lobby member sync)
    DiscoveryRawHandler raw_handler;  // other message types (e.g. lobby invites)
    void* raw_ctx;

    CachedSession cache[MAX_CACHED_SESSIONS];
    int cache_count;

    UserBeacon user_cache[MAX_USER_BEACONS];
    int user_count;

    uint8_t recv_buffer[MAX_PACKET_SIZE];
    uint8_t send_buffer[MAX_PACKET_SIZE];
};

// sendto with error logging: a host whose announces never reached the LAN
// (Remnant II on one PC) failed silently before.
static int ds_send_one(DiscoveryService* ds, const char* buf, int len, int flags,
                       const struct sockaddr* to, int tolen) {
    static int fails = 0, oks = 0;
    int r = (int)sendto(ds->socket_fd, buf, len, flags, to, tolen);
    char ip[INET_ADDRSTRLEN] = "?";
    const struct sockaddr_in* sin = (const struct sockaddr_in*)to;
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
    if (r < 0) {
        fails++;
#ifdef _WIN32
        int err = WSAGetLastError();
#else
        int err = errno;
#endif
        if (fails <= 5 || fails % 200 == 0) {
            EOS_LOG_WARN("discovery sendto %s:%u len=%d FAILED err=%d (failures=%d)",
                         ip, (unsigned)ntohs(sin->sin_port), len, err, fails);
        }
    } else if (oks < 3) {
        oks++;
        EOS_LOG_INFO("discovery sendto %s:%u len=%d ok", ip, (unsigned)ntohs(sin->sin_port), len);
    }
    return r;
}

// Directed broadcast addresses of all other active IPv4 interfaces. Windows sends a
// packet for 255.255.255.255 only over the interface with the best route (the LAN
// card), so peers on a second network (ZeroTier, VPN) never heard our announces.
// The interface Windows picks for 255.255.255.255 is left out, so every network
// gets exactly one copy. Refreshed every 5 s (ZeroTier may come up later).
#define MAX_IFACE_BROADCASTS 16
static struct in_addr g_iface_bc[MAX_IFACE_BROADCASTS];
static int g_iface_bc_count = 0;
static uint64_t g_iface_bc_time = 0;

static void refresh_iface_broadcasts(DiscoveryService* ds) {
    uint64_t now = get_time_ms();
    if (g_iface_bc_time != 0 && now - g_iface_bc_time < 5000) return;
    g_iface_bc_time = now;
#ifdef _WIN32
    // local address of the interface Windows uses for the limited broadcast
    struct sockaddr_in limited = {0}, chosen = {0};
    limited.sin_family = AF_INET;
    limited.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    DWORD bytes = 0;
    if (WSAIoctl(ds->socket_fd, SIO_ROUTING_INTERFACE_QUERY, &limited, sizeof(limited),
                 &chosen, sizeof(chosen), &bytes, NULL, NULL) != 0) {
        chosen.sin_addr.s_addr = 0;
    }

    INTERFACE_INFO list[32];
    bytes = 0;
    if (WSAIoctl(ds->socket_fd, SIO_GET_INTERFACE_LIST, NULL, 0, list, sizeof(list),
                 &bytes, NULL, NULL) != 0) {
        return;  // keep the previous list
    }
    int count = 0;
    char desc[256] = "";
    int n = (int)(bytes / sizeof(INTERFACE_INFO));
    for (int i = 0; i < n && count < MAX_IFACE_BROADCASTS; i++) {
        u_long flags = list[i].iiFlags;
        if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK) || !(flags & IFF_BROADCAST)) continue;
        uint32_t ip = list[i].iiAddress.AddressIn.sin_addr.s_addr;
        uint32_t mask = list[i].iiNetmask.AddressIn.sin_addr.s_addr;
        if (ip == 0 || mask == 0 || mask == 0xFFFFFFFFu) continue;
        if ((ntohl(ip) & 0xFFFF0000u) == 0xA9FE0000u) continue;  // link-local 169.254/16
        if (ip == chosen.sin_addr.s_addr) continue;              // already reached via 255.255.255.255
        g_iface_bc[count].s_addr = ip | ~mask;
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &g_iface_bc[count], a, sizeof(a));
        if (strlen(desc) + strlen(a) + 2 < sizeof(desc)) { strcat(desc, " "); strcat(desc, a); }
        count++;
    }
    if (count != g_iface_bc_count) {
        char c[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &chosen.sin_addr, c, sizeof(c));
        EOS_LOG_INFO("discovery: 255.255.255.255 goes via %s, extra interface broadcasts:%s",
                     c, count ? desc : " none");
    }
    g_iface_bc_count = count;
#endif
}

// Every send to 255.255.255.255 also goes to the other interfaces' broadcast addresses.
static int ds_send_logged(DiscoveryService* ds, const char* buf, int len, int flags,
                          const struct sockaddr* to, int tolen) {
    int r = ds_send_one(ds, buf, len, flags, to, tolen);
    const struct sockaddr_in* sin = (const struct sockaddr_in*)to;
    if (sin->sin_family == AF_INET && sin->sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
        refresh_iface_broadcasts(ds);
        for (int i = 0; i < g_iface_bc_count; i++) {
            struct sockaddr_in d = *sin;
            d.sin_addr = g_iface_bc[i];
            ds_send_one(ds, buf, len, flags, (const struct sockaddr*)&d, sizeof(d));
        }
    }
    return r;
}
// Helper function to serialize session attribute
static int serialize_attribute(uint8_t* buf, const SessionAttribute* attr) {
    int offset = 0;

    // Key
    memset(buf + offset, 0, 64);
    strncpy((char*)(buf + offset), attr->key, 64);
    offset += 64;

    // Type
    buf[offset++] = (uint8_t)attr->type;

    // Advertisement type
    buf[offset++] = (uint8_t)attr->advertisement;

    // Value (based on type)
    switch (attr->type) {
        case EOS_SAT_Boolean:
            buf[offset++] = attr->value.as_bool ? 1 : 0;
            break;
        case EOS_SAT_Int64:
            *(int64_t*)(buf + offset) = (int64_t)attr->value.as_int64;
            offset += 8;
            break;
        case EOS_SAT_Double:
            *(double*)(buf + offset) = attr->value.as_double;
            offset += 8;
            break;
        case EOS_SAT_String:
            memset(buf + offset, 0, 256);
            strncpy((char*)(buf + offset), attr->value.as_string, 256);
            offset += 256;
            break;
        default:
            break;
    }

    return offset;
}

// Helper function to deserialize session attribute
static int deserialize_attribute(const uint8_t* buf, int max_len, SessionAttribute* attr) {
    if (max_len < 66) return -1;  // Minimum size
    int offset = 0;

    // Key
    memcpy(attr->key, buf + offset, 64);
    attr->key[63] = '\0';
    offset += 64;

    // Type
    attr->type = (EOS_ESessionAttributeType)buf[offset++];

    // Advertisement type
    attr->advertisement = (EOS_ESessionAttributeAdvertisementType)buf[offset++];

    // Value
    switch (attr->type) {
        case EOS_SAT_Boolean:
            if (offset + 1 > max_len) return -1;
            attr->value.as_bool = buf[offset++] != 0;
            break;
        case EOS_SAT_Int64:
            if (offset + 8 > max_len) return -1;
            attr->value.as_int64 = *(int64_t*)(buf + offset);
            offset += 8;
            break;
        case EOS_SAT_Double:
            if (offset + 8 > max_len) return -1;
            attr->value.as_double = *(double*)(buf + offset);
            offset += 8;
            break;
        case EOS_SAT_String:
            if (offset + 256 > max_len) return -1;
            memcpy(attr->value.as_string, buf + offset, 256);
            attr->value.as_string[255] = '\0';
            offset += 256;
            break;
        default:
            return -1;
    }

    return offset;
}

// Parse announcement packet into Session
static bool parse_announcement(const uint8_t* buf, int len, Session* session) {
    if (len < 643) return false;  // Minimum size for session header
    int offset = 0;

    // Session ID
    memcpy(session->session_id, buf + offset, 64);
    session->session_id[63] = '\0';
    offset += 64;

    // Session name
    memcpy(session->session_name, buf + offset, 256);
    session->session_name[255] = '\0';
    offset += 256;

    // Bucket ID
    memcpy(session->bucket_id, buf + offset, 256);
    session->bucket_id[255] = '\0';
    offset += 256;

    // Host address
    memcpy(session->host_address, buf + offset, 64);
    session->host_address[63] = '\0';
    offset += 64;

    // Owner ID — a ProductUserId is exactly 32 hex chars; null-terminate at [32]
    // (NOT [31], which clobbered the last char -> 31-char id -> FromString fails
    // -> owner/member NULL -> game reports "session is full").
    memcpy(session->owner_id_string, buf + offset, 32);
    session->owner_id_string[32] = '\0';
    offset += 32;

    // Reconstruct a VALID LOCAL ProductUserId pointer from the string. Without
    // this, session->owner_id is uninitialized garbage from the wire; when the
    // game reads a discovered session's owner and calls EOS_ProductUserId_IsValid
    // it dereferences that bad pointer and crashes (reading 0x...ffffffff) the
    // instant you try to join. FromString returns a persistent local handle.
    session->owner_id = EOS_ProductUserId_FromString(session->owner_id_string);

    if (offset + 10 > len) return false;

    // Max players
    session->max_players = ntohl(*(uint32_t*)(buf + offset));
    offset += 4;

    // Registered player count
    session->registered_player_count = ntohl(*(uint32_t*)(buf + offset));
    offset += 4;

    // State
    session->state = (EOS_EOnlineSessionState)buf[offset++];

    // Flags: bit0=join-in-progress, bits1-2=permission level (0/1/2 — must carry
    // JoinViaPresence=1, not collapse it to InviteOnly), bit3=presence-enabled,
    // bit4=sanctions-enabled, bit5=invites-allowed. The old format only stored a
    // single PublicAdvertised bit, so a JoinViaPresence host session was decoded
    // as InviteOnly here (and presence/sanctions read uninitialized garbage) —
    // the joiner then never saw the host's game as a joinable friend session.
    uint8_t flags = buf[offset++];
    session->join_in_progress_allowed = (flags & 0x01) != 0;
    session->permission_level = (EOS_EOnlineSessionPermissionLevel)((flags >> 1) & 0x03);
    session->presence_enabled = (flags & 0x08) != 0;
    session->sanctions_enabled = (flags & 0x10) != 0;
    session->invites_allowed = (flags & 0x20) != 0;

    // v2: presence join-info string (fixed 256 bytes)
    if (offset + PRESENCE_JOININFO_LEN > len) return false;
    memcpy(session->join_info, buf + offset, PRESENCE_JOININFO_LEN);
    session->join_info[PRESENCE_JOININFO_LEN - 1] = '\0';
    offset += PRESENCE_JOININFO_LEN;

    // v2: presence data records (count + fixed key/value pairs)
    if (offset + 1 > len) return false;
    uint8_t prec_count = buf[offset++];
    if (prec_count > MAX_PRESENCE_RECORDS) prec_count = MAX_PRESENCE_RECORDS;
    session->presence_record_count = 0;
    for (int i = 0; i < prec_count; i++) {
        if (offset + PRESENCE_KEY_LEN + PRESENCE_VALUE_LEN > len) return false;
        memcpy(session->presence_records[i].key, buf + offset, PRESENCE_KEY_LEN);
        session->presence_records[i].key[PRESENCE_KEY_LEN - 1] = '\0';
        offset += PRESENCE_KEY_LEN;
        memcpy(session->presence_records[i].value, buf + offset, PRESENCE_VALUE_LEN);
        session->presence_records[i].value[PRESENCE_VALUE_LEN - 1] = '\0';
        offset += PRESENCE_VALUE_LEN;
        session->presence_record_count++;
    }

    if (offset + 2 > len) return false;

    // Attributes
    uint16_t attr_count = ntohs(*(uint16_t*)(buf + offset));
    offset += 2;

    session->attribute_count = 0;
    for (int i = 0; i < attr_count && i < 64; i++) {
        int consumed = deserialize_attribute(buf + offset, len - offset, &session->attributes[i]);
        if (consumed < 0) break;
        offset += consumed;
        session->attribute_count++;
    }

    // Mark session as valid so it passes search filtering
    session->valid = true;

    return true;
}

// Add session to cache
static void add_to_cache(DiscoveryService* ds, const Session* session, const char* source_ip) {
    if (!ds || !session) return;

    // Check if session already exists in cache (update it). With key_by_source the
    // same id announced by different hosts (lobby owner + its members) gets one
    // entry per sender instead of the senders overwriting each other.
    for (int i = 0; i < ds->cache_count; i++) {
        if (strcmp(ds->cache[i].session.session_id, session->session_id) == 0 &&
            (!ds->key_by_source || strcmp(ds->cache[i].source_ip, source_ip) == 0)) {
            memcpy(&ds->cache[i].session, session, sizeof(Session));
            strncpy(ds->cache[i].source_ip, source_ip, sizeof(ds->cache[i].source_ip) - 1);
            ds->cache[i].received_at = get_time_ms();
            return;
        }
    }

    // Add new session; when the cache is full, recycle the entry heard from least
    // recently (entries never expired before, so long sessions that keep creating
    // lobbies silently stopped discovering new ones after 64).
    int slot = ds->cache_count;
    if (slot >= MAX_CACHED_SESSIONS) {
        slot = 0;
        for (int i = 1; i < ds->cache_count; i++) {
            if (ds->cache[i].received_at < ds->cache[slot].received_at) slot = i;
        }
    } else {
        ds->cache_count++;
    }
    memcpy(&ds->cache[slot].session, session, sizeof(Session));
    memset(ds->cache[slot].source_ip, 0, sizeof(ds->cache[slot].source_ip));
    strncpy(ds->cache[slot].source_ip, source_ip, sizeof(ds->cache[slot].source_ip) - 1);
    ds->cache[slot].received_at = get_time_ms();
}

// ===== User beacons (user existence + presence, independent of sessions) =====

static void add_user_to_cache(DiscoveryService* ds, const UserBeacon* user, const char* source_ip) {
    if (!ds || !user) return;
    for (int i = 0; i < ds->user_count; i++) {
        if (strncmp(ds->user_cache[i].epic_id, user->epic_id, 32) == 0) {
            UserBeacon* slot = &ds->user_cache[i];
            *slot = *user;
            strncpy(slot->source_ip, source_ip, sizeof(slot->source_ip) - 1);
            slot->source_ip[sizeof(slot->source_ip) - 1] = '\0';
            slot->last_seen = get_time_ms();
            slot->valid = true;
            return;
        }
    }
    if (ds->user_count < MAX_USER_BEACONS) {
        UserBeacon* slot = &ds->user_cache[ds->user_count++];
        *slot = *user;
        strncpy(slot->source_ip, source_ip, sizeof(slot->source_ip) - 1);
        slot->source_ip[sizeof(slot->source_ip) - 1] = '\0';
        slot->last_seen = get_time_ms();
        slot->valid = true;
        EOS_LOG_INFO("Discovered user beacon: '%s' (%s) from %s", slot->display_name, slot->epic_id, source_ip);
    }
}

void discovery_broadcast_user(DiscoveryService* ds, const UserBeacon* user) {
    if (!ds || !user) return;

    uint8_t* buf = ds->send_buffer;
    int offset = 0;

    memcpy(buf + offset, DISCOVERY_MAGIC, 6); offset += 6;
    *(uint16_t*)(buf + offset) = htons(DISCOVERY_VERSION); offset += 2;
    buf[offset++] = MSG_USER_BEACON;

    memset(buf + offset, 0, 33);
    strncpy((char*)(buf + offset), user->epic_id, 32); offset += 33;
    memset(buf + offset, 0, 33);
    strncpy((char*)(buf + offset), user->puid, 32); offset += 33;
    memset(buf + offset, 0, 24);                                  // steam_id (decimal, "" if unset)
    strncpy((char*)(buf + offset), user->steam_id, 23); offset += 24;
    memset(buf + offset, 0, PEER_DISPLAY_NAME_LEN);
    strncpy((char*)(buf + offset), user->display_name, PEER_DISPLAY_NAME_LEN - 1); offset += PEER_DISPLAY_NAME_LEN;
    memset(buf + offset, 0, PRESENCE_JOININFO_LEN);
    strncpy((char*)(buf + offset), user->join_info, PRESENCE_JOININFO_LEN - 1); offset += PRESENCE_JOININFO_LEN;

    uint8_t prec_count = (user->record_count > MAX_PRESENCE_RECORDS)
        ? MAX_PRESENCE_RECORDS : (uint8_t)user->record_count;
    buf[offset++] = prec_count;
    for (int i = 0; i < prec_count; i++) {
        memset(buf + offset, 0, PRESENCE_KEY_LEN);
        strncpy((char*)(buf + offset), user->records[i].key, PRESENCE_KEY_LEN - 1);
        offset += PRESENCE_KEY_LEN;
        memset(buf + offset, 0, PRESENCE_VALUE_LEN);
        strncpy((char*)(buf + offset), user->records[i].value, PRESENCE_VALUE_LEN - 1);
        offset += PRESENCE_VALUE_LEN;
    }
    // trailing field (newer builds): P2P listen port, so peers can reach EOS:<puid>
    // without an EOS session (Deadzone: sessions run on RallyHere). Older parsers stop
    // after the presence records and ignore it.
    *(uint16_t*)(buf + offset) = htons(user->p2p_port); offset += 2;

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ds->port);
    inet_pton(AF_INET, ds->broadcast_addr, &dest.sin_addr);
    ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&dest, sizeof(dest));

    if (ds->localhost_mode) {
        struct sockaddr_in lo = {0};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(ds->port);
        inet_pton(AF_INET, "127.255.255.255", &lo.sin_addr);
        ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&lo, sizeof(lo));
    }
}

static bool parse_user_beacon(const uint8_t* buf, int len, UserBeacon* user) {
    int offset = 0;
    memset(user, 0, sizeof(*user));

    if (offset + 33 + 33 + 24 + PEER_DISPLAY_NAME_LEN + PRESENCE_JOININFO_LEN + 1 > len) return false;
    memcpy(user->epic_id, buf + offset, 32); user->epic_id[32] = '\0'; offset += 33;
    memcpy(user->puid, buf + offset, 32); user->puid[32] = '\0'; offset += 33;
    memcpy(user->steam_id, buf + offset, 23); user->steam_id[23] = '\0'; offset += 24;
    memcpy(user->display_name, buf + offset, PEER_DISPLAY_NAME_LEN);
    user->display_name[PEER_DISPLAY_NAME_LEN - 1] = '\0'; offset += PEER_DISPLAY_NAME_LEN;
    memcpy(user->join_info, buf + offset, PRESENCE_JOININFO_LEN);
    user->join_info[PRESENCE_JOININFO_LEN - 1] = '\0'; offset += PRESENCE_JOININFO_LEN;

    uint8_t prec_count = buf[offset++];
    if (prec_count > MAX_PRESENCE_RECORDS) prec_count = MAX_PRESENCE_RECORDS;
    for (int i = 0; i < prec_count; i++) {
        if (offset + PRESENCE_KEY_LEN + PRESENCE_VALUE_LEN > len) return false;
        memcpy(user->records[i].key, buf + offset, PRESENCE_KEY_LEN);
        user->records[i].key[PRESENCE_KEY_LEN - 1] = '\0';
        offset += PRESENCE_KEY_LEN;
        memcpy(user->records[i].value, buf + offset, PRESENCE_VALUE_LEN);
        user->records[i].value[PRESENCE_VALUE_LEN - 1] = '\0';
        offset += PRESENCE_VALUE_LEN;
        user->record_count++;
    }
    if (offset + 2 <= len) {
        user->p2p_port = ntohs(*(const uint16_t*)(buf + offset));
        offset += 2;
    }
    user->valid = true;
    return true;
}

UserBeacon* discovery_get_users(DiscoveryService* ds, int* out_count) {
    if (!ds || !out_count) return NULL;
    *out_count = ds->user_count;
    return (ds->user_count > 0) ? &ds->user_cache[0] : NULL;
}

DiscoveryService* discovery_create(uint16_t port) {
#ifdef _WIN32
    // Initialize Winsock
    static bool winsock_initialized = false;
    if (!winsock_initialized) {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return NULL;
        }
        winsock_initialized = true;
    }
#endif

    DiscoveryService* ds = calloc(1, sizeof(DiscoveryService));
    if (!ds) return NULL;

    ds->port = port ? port : 23456;
    strcpy(ds->broadcast_addr, "255.255.255.255");
    ds->localhost_mode = should_use_localhost_mode();

    if (ds->localhost_mode) {
        EOS_LOG_INFO("Localhost discovery mode enabled (EOSLAN_LOCALHOST_MODE=1)");
    }

    // Create UDP socket
    ds->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (ds->socket_fd == INVALID_SOCKET) {
#else
    if (ds->socket_fd < 0) {
#endif
        free(ds);
        return NULL;
    }

    // Enable broadcast
    int broadcast = 1;
    setsockopt(ds->socket_fd, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    // Enable address reuse (for multiple instances on same machine)
    int reuse = 1;
    setsockopt(ds->socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(ds->socket_fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
#endif

    // Bind to port
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ds->port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ds->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(ds->socket_fd);
        free(ds);
        return NULL;
    }

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(ds->socket_fd, FIONBIO, &mode);
#else
    int flags = fcntl(ds->socket_fd, F_GETFL, 0);
    fcntl(ds->socket_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    return ds;
}

void discovery_destroy(DiscoveryService* ds) {
    if (!ds) return;

    if (ds->socket_fd >= 0) {
        close(ds->socket_fd);
    }

    free(ds);
}

void discovery_broadcast_session(DiscoveryService* ds, const Session* session) {
    if (!ds || !session) return;

    // Build announcement packet
    uint8_t* buf = ds->send_buffer;
    int offset = 0;

    // Header
    memcpy(buf + offset, DISCOVERY_MAGIC, 6); offset += 6;
    *(uint16_t*)(buf + offset) = htons(DISCOVERY_VERSION); offset += 2;
    buf[offset++] = MSG_ANNOUNCE;

    // Session data
    memset(buf + offset, 0, 64);
    strncpy((char*)(buf + offset), session->session_id, 64); offset += 64;

    memset(buf + offset, 0, 256);
    strncpy((char*)(buf + offset), session->session_name, 256); offset += 256;

    memset(buf + offset, 0, 256);
    strncpy((char*)(buf + offset), session->bucket_id, 256); offset += 256;

    memset(buf + offset, 0, 64);
    strncpy((char*)(buf + offset), session->host_address, 64); offset += 64;

    memset(buf + offset, 0, 32);
    strncpy((char*)(buf + offset), session->owner_id_string, 32); offset += 32;

    *(uint32_t*)(buf + offset) = htonl(session->max_players); offset += 4;
    *(uint32_t*)(buf + offset) = htonl(session->registered_player_count); offset += 4;
    buf[offset++] = (uint8_t)session->state;

    // bit0=join-in-progress, bits1-2=permission level (0/1/2), bit3=presence,
    // bit4=sanctions, bit5=invites-allowed (see deserialize for why the full
    // permission level must ride, not just a PublicAdvertised bit).
    uint8_t flags = 0;
    if (session->join_in_progress_allowed) flags |= 0x01;
    flags |= ((uint8_t)session->permission_level & 0x03) << 1;
    if (session->presence_enabled) flags |= 0x08;
    if (session->sanctions_enabled) flags |= 0x10;
    if (session->invites_allowed) flags |= 0x20;
    buf[offset++] = flags;

    // v2: presence join-info string (fixed 256 bytes)
    memset(buf + offset, 0, PRESENCE_JOININFO_LEN);
    strncpy((char*)(buf + offset), session->join_info, PRESENCE_JOININFO_LEN - 1);
    offset += PRESENCE_JOININFO_LEN;

    // v2: presence data records (count + fixed key/value pairs)
    uint8_t prec_count = (session->presence_record_count > MAX_PRESENCE_RECORDS)
        ? MAX_PRESENCE_RECORDS : (uint8_t)session->presence_record_count;
    buf[offset++] = prec_count;
    for (int i = 0; i < prec_count; i++) {
        memset(buf + offset, 0, PRESENCE_KEY_LEN);
        strncpy((char*)(buf + offset), session->presence_records[i].key, PRESENCE_KEY_LEN - 1);
        offset += PRESENCE_KEY_LEN;
        memset(buf + offset, 0, PRESENCE_VALUE_LEN);
        strncpy((char*)(buf + offset), session->presence_records[i].value, PRESENCE_VALUE_LEN - 1);
        offset += PRESENCE_VALUE_LEN;
    }

    // Attributes — write the count we actually serialize, and never overflow buf.
    uint8_t* attr_count_field = buf + offset; offset += 2;
    int attrs_written = 0;
    for (int i = 0; i < session->attribute_count && i < 64; i++) {
        if (offset + 400 > MAX_PACKET_SIZE) break;  // 322 max per attr; stay safe
        offset += serialize_attribute(buf + offset, &session->attributes[i]);
        attrs_written++;
    }
    *(uint16_t*)attr_count_field = htons((uint16_t)attrs_written);

    // Send broadcast
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ds->port);
    inet_pton(AF_INET, ds->broadcast_addr, &dest.sin_addr);

    ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&dest, sizeof(dest));

    // If localhost mode, also send to loopback broadcast for Wine/Proton support
    if (ds->localhost_mode) {
        struct sockaddr_in lo = {0};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(ds->port);
        // Use loopback broadcast (127.255.255.255) instead of unicast (127.0.0.1)
        // This allows packets to reach all sockets bound to the port on loopback
        inet_pton(AF_INET, "127.255.255.255", &lo.sin_addr);
        ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&lo, sizeof(lo));
    }
}

void discovery_send_query(DiscoveryService* ds, const char* bucket_filter) {
    if (!ds) return;

    // Build query packet
    uint8_t* buf = ds->send_buffer;
    int offset = 0;

    // Header
    memcpy(buf + offset, DISCOVERY_MAGIC, 6); offset += 6;
    *(uint16_t*)(buf + offset) = htons(DISCOVERY_VERSION); offset += 2;
    buf[offset++] = MSG_QUERY;

    // Bucket filter (optional)
    if (bucket_filter) {
        memset(buf + offset, 0, 256);
        strncpy((char*)(buf + offset), bucket_filter, 256);
    } else {
        memset(buf + offset, 0, 256);
    }
    offset += 256;

    // Send broadcast
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ds->port);
    inet_pton(AF_INET, ds->broadcast_addr, &dest.sin_addr);

    ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&dest, sizeof(dest));

    // If localhost mode, also send to loopback broadcast for Wine/Proton support
    if (ds->localhost_mode) {
        struct sockaddr_in lo = {0};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(ds->port);
        // Use loopback broadcast (127.255.255.255) instead of unicast (127.0.0.1)
        inet_pton(AF_INET, "127.255.255.255", &lo.sin_addr);
        ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&lo, sizeof(lo));
    }
}

void discovery_poll(DiscoveryService* ds) {
    if (!ds) return;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (true) {
#ifdef _WIN32
        int len = recvfrom(ds->socket_fd, (char*)ds->recv_buffer, MAX_PACKET_SIZE, 0,
                          (struct sockaddr*)&from, &from_len);
        if (len == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            break;
        }
#else
        ssize_t len = recvfrom(ds->socket_fd, ds->recv_buffer, MAX_PACKET_SIZE, 0,
                               (struct sockaddr*)&from, &from_len);
        if (len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
#endif

        // Parse header
        if (len < 9) continue;
        if (memcmp(ds->recv_buffer, DISCOVERY_MAGIC, 6) != 0) continue;

        uint16_t version = ntohs(*(uint16_t*)(ds->recv_buffer + 6));
        if (version != DISCOVERY_VERSION) continue;

        uint8_t msg_type = ds->recv_buffer[8];

        if (msg_type == MSG_ANNOUNCE) {
            Session session;
            if (parse_announcement(ds->recv_buffer + 9, len - 9, &session)) {
                // Get source IP
                char source_ip[16];
                inet_ntop(AF_INET, &from.sin_addr, source_ip, sizeof(source_ip));

                EOS_LOG_DEBUG("Received session announcement from %s: %s", source_ip, session.session_name);

                // Add to cache
                add_to_cache(ds, &session, source_ip);
            }
        } else if (msg_type == MSG_QUERY) {
            // Another instance is searching - set flag to trigger immediate broadcast
            ds->query_received = true;
            EOS_LOG_DEBUG("Received query from peer, will broadcast sessions immediately");
        } else if (msg_type != MSG_ANNOUNCE && msg_type != MSG_QUERY && msg_type != MSG_USER_BEACON) {
            if (ds->raw_handler) {
                char source_ip[16];
                inet_ntop(AF_INET, &from.sin_addr, source_ip, sizeof(source_ip));
                ds->raw_handler(ds->raw_ctx, msg_type, ds->recv_buffer + 9, (int)(len - 9), source_ip);
            }
        } else if (msg_type == MSG_USER_BEACON) {
            UserBeacon user;
            if (parse_user_beacon(ds->recv_buffer + 9, (int)(len - 9), &user)) {
                char source_ip[16];
                inet_ntop(AF_INET, &from.sin_addr, source_ip, sizeof(source_ip));
                add_user_to_cache(ds, &user, source_ip);
            }
        }
    }
}

Session* discovery_get_sessions(DiscoveryService* ds, int* out_count) {
    if (!ds || !out_count) return NULL;

    *out_count = ds->cache_count;
    if (ds->cache_count == 0) return NULL;

    // Return pointer to first session in cache
    return &ds->cache[0].session;
}

// The cache holds CachedSession entries (Session + source_ip + timestamp), so the
// pointer from discovery_get_sessions() must NOT be indexed as a Session array:
// entry [1] onward would be read at the wrong stride (lobby id = previous entry's
// source IP, then garbage). Callers iterate with this accessor instead.
const Session* discovery_get_session_at(DiscoveryService* ds, int index) {
    if (!ds || index < 0 || index >= ds->cache_count) return NULL;
    return &ds->cache[index].session;
}

uint64_t discovery_get_received_at(DiscoveryService* ds, int index) {
    if (!ds || index < 0 || index >= ds->cache_count) return 0;
    return ds->cache[index].received_at;
}

void discovery_set_raw_handler(DiscoveryService* ds, DiscoveryRawHandler handler, void* ctx) {
    if (!ds) return;
    ds->raw_handler = handler;
    ds->raw_ctx = ctx;
}

void discovery_broadcast_raw(DiscoveryService* ds, uint8_t msg_type, const uint8_t* payload, int len) {
    if (!ds || len < 0 || len > MAX_PACKET_SIZE - 9) return;
    uint8_t* buf = ds->send_buffer;
    int offset = 0;
    memcpy(buf + offset, DISCOVERY_MAGIC, 6); offset += 6;
    *(uint16_t*)(buf + offset) = htons(DISCOVERY_VERSION); offset += 2;
    buf[offset++] = msg_type;
    if (len > 0) { memcpy(buf + offset, payload, (size_t)len); offset += len; }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ds->port);
    inet_pton(AF_INET, ds->broadcast_addr, &dest.sin_addr);
    ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&dest, sizeof(dest));

    if (ds->localhost_mode) {
        struct sockaddr_in lo = {0};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(ds->port);
        inet_pton(AF_INET, "127.255.255.255", &lo.sin_addr);
        ds_send_logged(ds, (const char*)buf, offset, 0, (struct sockaddr*)&lo, sizeof(lo));
    }
}

void discovery_set_key_by_source(DiscoveryService* ds, bool on) {
    if (ds) ds->key_by_source = on;
}

const Session* discovery_find_cached_session(DiscoveryService* ds, const char* session_id) {
    if (!ds || !session_id) return NULL;
    for (int i = 0; i < ds->cache_count; i++) {
        if (strcmp(ds->cache[i].session.session_id, session_id) == 0) {
            return &ds->cache[i].session;
        }
    }
    return NULL;
}

void discovery_clear_sessions(DiscoveryService* ds) {
    if (!ds) return;
    ds->cache_count = 0;
}

void discovery_set_broadcast_addr(DiscoveryService* ds, const char* addr) {
    if (!ds || !addr) return;
    strncpy(ds->broadcast_addr, addr, sizeof(ds->broadcast_addr) - 1);
    ds->broadcast_addr[sizeof(ds->broadcast_addr) - 1] = '\0';
}

bool discovery_should_broadcast_now(DiscoveryService* ds) {
    if (!ds) return false;
    bool should = ds->query_received;
    ds->query_received = false;  // Clear flag after check
    return should;
}
