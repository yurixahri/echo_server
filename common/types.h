#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QDateTime>
#include <stdint.h>

typedef struct {
    uint64_t id;
    QString sender;
    QString msg;
    QDateTime timestamp;
} chat_message;

typedef struct {
    uint64_t uptime;
} system_status;

typedef struct {
    uint64_t status;
} respone;

#endif // TYPES_H
