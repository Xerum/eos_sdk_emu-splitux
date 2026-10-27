#include "eos/eos_sessions.h"
#include "eos/eos_sessions_types.h"
#include "internal/sessions_internal.h"
#include "internal/logging.h"
#include <stdlib.h>
#include <string.h>

// SessionModification handle functions

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetBucketId(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetBucketIdOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    if (!Options->BucketId) {
        return EOS_InvalidParameters;
    }

    strncpy(mod->session.bucket_id, Options->BucketId, sizeof(mod->session.bucket_id) - 1);
    mod->session.bucket_id[sizeof(mod->session.bucket_id) - 1] = '\0';

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetHostAddress(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetHostAddressOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    if (!Options->HostAddress) {
        return EOS_InvalidParameters;
    }

    // A game-built address ("EOS:<puid>:<socket>:<channel>", Remnant II) is what
    // joiners must get back in EOS_SessionDetails_Info::HostAddress, but our own
    // host_address has to stay the LAN ip:port of the P2P transport. Keep the
    // game's value in a hidden attribute that travels with the announce.
    // Also Redpoint's "<puid>.<socket>.eosp2p:<channel>" form (Smalland).
    if (strncmp(Options->HostAddress, "EOS:", 4) == 0 || strstr(Options->HostAddress, ".eosp2p") != NULL) {
        SessionAttribute* attr = NULL;
        for (int i = 0; i < mod->session.attribute_count; i++) {
            if (strcmp(mod->session.attributes[i].key, LAN_HOSTADDR_ATTR) == 0) {
                attr = &mod->session.attributes[i];
                break;
            }
        }
        if (!attr) {
            if (mod->session.attribute_count >= MAX_SESSION_ATTRIBUTES) {
                return EOS_LimitExceeded;
            }
            attr = &mod->session.attributes[mod->session.attribute_count++];
        }
        memset(attr, 0, sizeof(*attr));
        strncpy(attr->key, LAN_HOSTADDR_ATTR, sizeof(attr->key) - 1);
        attr->type = EOS_AT_STRING;
        attr->advertisement = EOS_SAAT_Advertise;
        strncpy(attr->value.as_string, Options->HostAddress, sizeof(attr->value.as_string) - 1);
        EOS_LOG_INFO("SetHostAddress: game address '%s' kept as %s", Options->HostAddress, LAN_HOSTADDR_ATTR);
        return EOS_Success;
    }

    strncpy(mod->session.host_address, Options->HostAddress, sizeof(mod->session.host_address) - 1);
    mod->session.host_address[sizeof(mod->session.host_address) - 1] = '\0';

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetPermissionLevel(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetPermissionLevelOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    mod->session.permission_level = Options->PermissionLevel;

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetJoinInProgressAllowed(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetJoinInProgressAllowedOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    mod->session.join_in_progress_allowed = (Options->bAllowJoinInProgress == EOS_TRUE);

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetMaxPlayers(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetMaxPlayersOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    if (Options->MaxPlayers == 0 || Options->MaxPlayers > EOS_SESSIONS_MAXREGISTEREDPLAYERS) {
        return EOS_InvalidParameters;
    }

    mod->session.max_players = Options->MaxPlayers;

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_SetInvitesAllowed(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_SetInvitesAllowedOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    mod->session.invites_allowed = (Options->bInvitesAllowed == EOS_TRUE);

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_AddAttribute(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_AddAttributeOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    if (!Options->SessionAttribute || !Options->SessionAttribute->Key) {
        return EOS_InvalidParameters;
    }

    const EOS_Sessions_AttributeData* attr_data = Options->SessionAttribute;

    // Check if attribute already exists and update it
    int existing_index = -1;
    for (int i = 0; i < mod->session.attribute_count; i++) {
        if (strcmp(mod->session.attributes[i].key, attr_data->Key) == 0) {
            existing_index = i;
            break;
        }
    }

    SessionAttribute* attr;
    if (existing_index >= 0) {
        // Update existing attribute
        attr = &mod->session.attributes[existing_index];
    } else {
        // Add new attribute
        if (mod->session.attribute_count >= MAX_SESSION_ATTRIBUTES) {
            return EOS_LimitExceeded;
        }
        attr = &mod->session.attributes[mod->session.attribute_count++];
    }

    // Copy attribute data
    strncpy(attr->key, attr_data->Key, sizeof(attr->key) - 1);
    attr->key[sizeof(attr->key) - 1] = '\0';
    attr->type = attr_data->ValueType;
    attr->advertisement = Options->AdvertisementType;

    switch (attr_data->ValueType) {
        case EOS_AT_BOOLEAN:
            attr->value.as_bool = attr_data->Value.AsBool;
            break;
        case EOS_AT_INT64:
            attr->value.as_int64 = attr_data->Value.AsInt64;
            break;
        case EOS_AT_DOUBLE:
            attr->value.as_double = attr_data->Value.AsDouble;
            break;
        case EOS_AT_STRING:
            if (attr_data->Value.AsUtf8) {
                strncpy(attr->value.as_string, attr_data->Value.AsUtf8, sizeof(attr->value.as_string) - 1);
                attr->value.as_string[sizeof(attr->value.as_string) - 1] = '\0';
            } else {
                attr->value.as_string[0] = '\0';
            }
            break;
        default:
            return EOS_InvalidParameters;
    }

    return EOS_Success;
}

EOS_DECLARE_FUNC(EOS_EResult) EOS_SessionModification_RemoveAttribute(
    EOS_HSessionModification Handle,
    const EOS_SessionModification_RemoveAttributeOptions* Options
) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (!mod || mod->magic != 0x534D4F44) {
        return EOS_InvalidParameters;
    }

    if (!Options || Options->ApiVersion < 1) {
        return EOS_InvalidParameters;
    }

    if (!Options->Key) {
        return EOS_InvalidParameters;
    }

    // Find and remove attribute
    for (int i = 0; i < mod->session.attribute_count; i++) {
        if (strcmp(mod->session.attributes[i].key, Options->Key) == 0) {
            // Shift remaining attributes down
            memmove(&mod->session.attributes[i],
                    &mod->session.attributes[i + 1],
                    sizeof(SessionAttribute) * (mod->session.attribute_count - i - 1));
            mod->session.attribute_count--;
            return EOS_Success;
        }
    }

    return EOS_NotFound;
}

EOS_DECLARE_FUNC(void) EOS_SessionModification_Release(EOS_HSessionModification Handle) { EOS_LOG_TRACE("ENTER");
    SessionModificationHandle* mod = (SessionModificationHandle*)Handle;

    if (mod && mod->magic == 0x534D4F44) {
        mod->magic = 0;  // Invalidate
        free(mod);
    }
}
