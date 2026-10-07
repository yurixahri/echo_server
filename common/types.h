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
    FAILED = 500
};

enum ASN1_EVENT{
    CHAT = 0,
    SYSTEM,
    RESPONSE,
    CLIENT_CHECK_ID,
    NEW_USER_CHALLENGE,
    NEW_USER_PROOF,
    RESPONSE_USER_CHECK_ID,
    MEMBER_LIST,
    MEMBER_IN_VOICE_ROOM_LIST,
    MEMBER_DETAIL
};

enum MESSAGE_TYPE{
    TEXT = 0,
    IMAGE,
    VIDEO,
    OTHER
};

enum USER_STATUS{
    OFFLINE = 0,
    ONLINE,
    AWAY
};

typedef struct {
    uint64_t id;
    QString sender;
    QString msg;
    QDateTime timestamp;
} chat_message_t;

typedef struct {
    uint64_t uptime;
} system_status_t;

typedef struct {
    uint64_t status;
} respone_t;

typedef struct {
    uint id;
    uint state = WAITTING_INIT;
    QString pending_name;
    QString pending_uuid;
    QByteArray challenge;
    qint64 challenge_expire = 0;
    uint difficulty = 0;
} client_state_t;

extern QMap<QTcpSocket*, client_state_t> client_state_map;


#endif // TYPES_H
