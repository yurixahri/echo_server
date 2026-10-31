#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QDateTime>
#include <QTcpSocket>
#include <QMap>
#include <stdint.h>

enum {
    WAITTING_INIT = 102,
    WAITTING_PROOF,
    WAITTING_EXIST_USER,
    SUCCESS = 200,
    ERROR = 400,
    PASSWORD_REQUIRED = 401,
    FAILED = 500
};

enum ASN1_EVENT{
    CHAT = 0,
    SYSTEM,
    RESPONSE,
    CLIENT_CHECK_ID,
    NEW_USER_CHALLENGE,
    NEW_USER_PROOF,
    CLAIM_ADMIN_PRIVILEGE,
    MEMBER_LIST,
    MEMBER_DETAIL,
    ROOM_LIST,
    CREATE_ROOM,
    JOIN_ROOM,
    UPDATE_SERVER_SETTINGS,
    LEAVE_ROOM,
    FILE_UPLOAD_CHUNK,
    FILE_DOWNLOAD_REQ,
    FILE_DOWNLOAD_CHUNK,
    HISTORY_REQ,
    DELETE_MSG,
    GET_MY_INFO,
    MY_INFO,
    AVATAR_UPLOAD_CHUNK,
    GET_ROLES_REQ,
    ROLE_LIST,
    CREATE_ROLE,
    UPDATE_ROLE_PERMS,
    DELETE_ROLE,
    GET_USER_ROLES_REQ,
    USER_ROLES_LIST,
    ASSIGN_USER_ROLES,
    KICK_USER,
    UPDATE_ROOM,
    POKE_USER,
    GET_MEMBERS_REQ,
    GET_MEMBER_DETAIL_REQ
};

enum ASN1_RESPONE_EVENT{
    RESPONSE_USER_CHECK_ID = 0,
    RESPONSE_USER_CLAIM_ADMIN_PRIVILEGE,
    RESPONSE_CREATE_ROOM,
    RESPONSE_JOIN_ROOM,
    RESPONSE_UPDATE_SERVER_SETTINGS,
    RESPONSE_FILE_UPLOAD,
    RESPONSE_HISTORY,
    RESPONSE_DELETE_MSG,
    RESPONSE_AVATAR_UPLOAD,
    RESPONSE_CREATE_ROLE,
    RESPONSE_UPDATE_ROLE_PERMS,
    RESPONSE_DELETE_ROLE,
    RESPONSE_ASSIGN_USER_ROLES,
    RESPONSE_KICK_USER,
    RESPONSE_UPDATE_ROOM,
    RESPONSE_KICKED,
    RESPONSE_PERMS_UPDATED,
    RESPONSE_POKE_USER,
    RESPONSE_POKED,
    RESPONSE_CHAT
};

enum MESSAGE_TYPE{
    TEXT = 1,
    IMAGE = 2,
    VIDEO = 3,
    OTHER = 4
};

enum USER_STATUS{
    OFFLINE = 0,
    ONLINE,
    AWAY
};

struct chat_message_t {
    uint64_t id = 0;
    uint64_t room_id = 0;
    uint64_t sender_id = 0;
    QString sender;
    QString sender_avatar;
    QString msg;
    uint16_t msg_type = MESSAGE_TYPE::TEXT;
    QDateTime timestamp;
};

struct system_status_t {
    uint64_t uptime = 0;
};

struct respone_t {
    uint64_t status = 0;
};

struct client_state_t {
    uint id = 0;
    uint state = WAITTING_INIT;
    QString pending_name;
    QString pending_uuid;
    QString avatar_path;
    QByteArray challenge;
    qint64 challenge_expire = 0;
    uint difficulty = 0;
    uint64_t current_chat_room_id = 0;
    uint64_t current_voice_room_id = 0;
    qint64 chat_timeout_until = 0;
    QList<qint64> chat_msg_timestamps;
};

struct voice_user_t {
    uint64_t id = 0;
    QString name;
    QString avatar;
    bool is_mute = false;
    bool is_deaf = false;
};

struct role_t {
    uint64_t id = 0;
    QString name;
    QMap<QString, int> permissions;
};

struct user_role_item_t {
    uint64_t id = 0;
    QString name;
    bool assigned = false;
};

enum VOICE_PACKET_TYPE {
    VOICE_PING = 1,
    VOICE_DATA = 2,
    VOICE_LEAVE = 3,
    VOICE_STATE = 4
};

#pragma pack(push, 1)
struct voice_packet_header_t {
    uint32_t magic = 0x4543484F; // 'ECHO'
    uint8_t packet_type = 0;      // VOICE_PACKET_TYPE
    uint64_t user_id = 0;
    uint64_t room_id = 0;
    uint32_t seq = 0;
    uint8_t is_mute = 0;
    uint8_t is_deaf = 0;
    uint16_t payload_size = 0;
};
#pragma pack(pop)

extern QMap<QTcpSocket*, client_state_t> client_state_map;

#include <QCollator>

inline bool natural_sort_less_than(const QString &s1, const QString &s2) {
    static QCollator collator;
    static bool init = []() {
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        return true;
    }();
    return collator.compare(s1, s2) < 0;
}

#endif // TYPES_H
