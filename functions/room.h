#ifndef ROOM_H
#define ROOM_H

#include "db/db.h"
#include "utils/asn1_parser.h"
#include "common/functions.h"
#include "common/types.h"
#include "functions/voice.h"
#include "functions/role.h"
#include <QTcpSocket>

void handle_create_room(QTcpSocket* client_socket, asn1_node element);
void handle_join_room(QTcpSocket* client_socket, asn1_node element);
void handle_leave_room(QTcpSocket* client_socket, asn1_node element);
void handle_chat_message(QTcpSocket* client_socket, asn1_node element);
void handle_delete_msg(QTcpSocket* client_socket, asn1_node element);
void handle_history_req(QTcpSocket* client_socket, asn1_node element);
void handle_update_server_settings(QTcpSocket* client_socket, asn1_node element);
void handle_kick_user(QTcpSocket* client_socket, asn1_node element);
void handle_update_room(QTcpSocket* client_socket, asn1_node element);
void send_room_list_packet(QTcpSocket* client_socket);
void send_room_message_history(QTcpSocket* client_socket, uint64_t room_id, uint64_t before_id = 0, int limit = 20);
void broadcast_room_list();

inline void send_room_list_packet(QTcpSocket* client_socket) {
    if (!client_socket || !client_socket->isOpen()) return;

    QSqlQuery query(db);
    if (!query.exec("SELECT id, name, (password IS NOT NULL AND password != ''), needed_voice_join_power, needed_chat_join_power, needed_speak_power FROM rooms ORDER BY id ASC")) {
        // qDebug() << "Failed to query rooms:" << query.lastError().text();
        return;
    }

    asn1_node element = NULL;
    int ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
    if (ret != ASN1_SUCCESS) {
        // printf("Failed to create ASN.1 element for room_list!\n");
        return;
    }

    asn1_write_value(element, "", "room_list", 1);

    int idx = 1;
    while (query.next()) {
        uint64_t room_id = query.value(0).toULongLong();
        QString room_name = query.value(1).toString();
        bool has_password = query.value(2).toBool();

        asn1_write_value(element, "room_list", "NEW", 1);
        QString item_prefix = QString("room_list.?%1").arg(idx);

        asn1_write_unsigned64(element, (item_prefix + ".id").toUtf8().constData(), room_id);
        asn1_write_string(element, (item_prefix + ".name").toUtf8().constData(), room_name);

        // Gather all users currently in this voice room
        QList<voice_user_t> v_users;
        {
            QMutexLocker locker(&voice_auth_mutex);
            for (const auto &st : client_state_map) {
                if (st.state == SUCCESS && st.current_voice_room_id == room_id) {
                    voice_user_t vu;
                    vu.id = st.id;
                    vu.name = st.pending_name;
                    vu.avatar = st.avatar_path;
                    if (voice_peers.contains(st.id)) {
                        vu.is_mute = voice_peers[st.id].is_mute;
                        vu.is_deaf = voice_peers[st.id].is_deaf;
                    }
                    v_users.append(vu);
                }
            }
        }

        std::sort(v_users.begin(), v_users.end(), [](const voice_user_t &a, const voice_user_t &b) {
            return natural_sort_less_than(a.name, b.name);
        });

        if (v_users.isEmpty()) {
            asn1_write_value(element, (item_prefix + ".users-in-voice-room").toUtf8().constData(), NULL, 0);
        } else {
            for (int k = 0; k < v_users.size(); ++k) {
                const voice_user_t &vu = v_users[k];
                asn1_write_value(element, (item_prefix + ".users-in-voice-room").toUtf8().constData(), "NEW", 1);
                QString u_prefix = QString("%1.users-in-voice-room.?%2").arg(item_prefix).arg(k + 1);
                asn1_write_unsigned64(element, (u_prefix + ".id").toUtf8().constData(), vu.id);
                asn1_write_string(element, (u_prefix + ".name").toUtf8().constData(), vu.name);
                asn1_write_string(element, (u_prefix + ".avatar").toUtf8().constData(), vu.avatar);
                asn1_write_bool(element, (u_prefix + ".is-mute").toUtf8().constData(), vu.is_mute);
                asn1_write_bool(element, (u_prefix + ".is-deaf").toUtf8().constData(), vu.is_deaf);
            }
        }

        asn1_write_bool(element, (item_prefix + ".has-password").toUtf8().constData(), has_password);
        asn1_write_unsigned64(element, (item_prefix + ".needed-voice-join-power").toUtf8().constData(), query.value(3).toULongLong());
        asn1_write_unsigned64(element, (item_prefix + ".needed-chat-join-power").toUtf8().constData(), query.value(4).toULongLong());
        asn1_write_unsigned64(element, (item_prefix + ".needed-speak-power").toUtf8().constData(), query.value(5).toULongLong());
        idx++;
    }

    asn1_server_send_packet(client_socket, element);
}

inline void broadcast_room_list() {
    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().state == SUCCESS && it.key() && it.key()->isOpen()) {
            send_room_list_packet(it.key());
        }
    }
}

inline void handle_create_room(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROOM, FAILED, "Unauthorized");
        return;
    }

    bool can_create = user_is_admin(client_state.id) || user_has_permission(client_state.id, "u_can_create_rooms");
    if (!can_create) {
        // qDebug() << "Create room rejected: user" << client_state.id << "has no create permission";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROOM, FAILED, "Permission u_can_create_rooms required");
        return;
    }

    QString name = asn1_read_string(element, "create-room.name").trimmed();
    if (name.isEmpty()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROOM, FAILED, "Room name cannot be empty");
        return;
    }

    QString password = asn1_read_string(element, "create-room.password");
    uint64_t limit_clients = asn1_read_int(element, "create-room.limit-clients");
    uint64_t needed_voice = asn1_read_int(element, "create-room.needed-voice-join-power");
    uint64_t needed_chat = asn1_read_int(element, "create-room.needed-chat-join-power");
    uint64_t needed_speak = asn1_read_int(element, "create-room.needed-speak-power");

    QVariant room_id = db_scalar(db,
        "INSERT INTO rooms(name, password, limit_clients, needed_voice_join_power, needed_chat_join_power, needed_speak_power) "
        "VALUES(?, ?, ?, ?, ?, ?) RETURNING id;",
        {name, password.isEmpty() ? QVariant() : password, limit_clients, needed_voice, needed_chat, needed_speak});

    if (!room_id.isValid()) {
        // qDebug() << "Failed to insert room into database";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROOM, FAILED, "Failed to create room");
        return;
    }

    // qDebug() << "Room created successfully! ID:" << room_id.toULongLong() << "Name:" << name
             // << "voice power:" << needed_voice << "chat power:" << needed_chat << "speak power:" << needed_speak;
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROOM, SUCCESS, "Room created successfully");

    broadcast_room_list();
}

inline void send_room_message_history(QTcpSocket* client_socket, uint64_t room_id, uint64_t before_id , int limit) {
    if (!client_socket || !client_socket->isOpen() || limit <= 0) return;

    QSqlQuery q(db);
    if (before_id == 0) {
        q.prepare("SELECT m.id, m.sender_id, u.name, m.payload, m.server_timestamp, m.type, COALESCE(u.avatar_path, '') "
                  "FROM messages m "
                  "JOIN users u ON m.sender_id = u.id "
                  "WHERE m.room_id = ? AND m.is_deleted = 0 "
                  "ORDER BY m.id DESC LIMIT ?");
        q.addBindValue(room_id);
        q.addBindValue(limit);
    } else {
        q.prepare("SELECT m.id, m.sender_id, u.name, m.payload, m.server_timestamp, m.type, COALESCE(u.avatar_path, '') "
                  "FROM messages m "
                  "JOIN users u ON m.sender_id = u.id "
                  "WHERE m.room_id = ? AND m.is_deleted = 0 AND m.id < ? "
                  "ORDER BY m.id DESC LIMIT ?");
        q.addBindValue(room_id);
        q.addBindValue(before_id);
        q.addBindValue(limit);
    }

    if (!q.exec()) return;

    QList<chat_message_t> rows;
    while (q.next()) {
        chat_message_t m;
        m.id = q.value(0).toULongLong();
        m.sender_id = q.value(1).toULongLong();
        m.sender = q.value(2).toString();
        m.msg = QString::fromUtf8(q.value(3).toByteArray());
        m.timestamp = QDateTime::fromSecsSinceEpoch(q.value(4).toULongLong());
        m.msg_type = q.value(5).toUInt();
        if (m.msg_type == 0) m.msg_type = MESSAGE_TYPE::TEXT;
        m.sender_avatar = q.value(6).toString();
        rows.append(m);
    }

    // Always send chronological (oldest to newest)
    std::reverse(rows.begin(), rows.end());

    for (const chat_message_t &m : rows) {
        asn1_node msg_elem = NULL;
        if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &msg_elem) == ASN1_SUCCESS) {
            asn1_write_value(msg_elem, "", "chat", 1);
            asn1_write_unsigned64(msg_elem, "chat.room-id", room_id);
            asn1_write_unsigned64(msg_elem, "chat.msg-id", m.id);
            asn1_write_unsigned64(msg_elem, "chat.sender-id", m.sender_id);
            asn1_write_string(msg_elem, "chat.sender", m.sender);
            asn1_write_string(msg_elem, "chat.sender-avatar", m.sender_avatar);
            asn1_write_string(msg_elem, "chat.content", m.msg);
            asn1_write_unsigned64(msg_elem, "chat.msg-type", m.msg_type);
            asn1_write_unsigned64(msg_elem, "chat.timestamp", m.timestamp.toSecsSinceEpoch());
            asn1_server_send_packet(client_socket, msg_elem);
            asn1_delete_structure(&msg_elem);
        }
    }

    send_tcp_response_event(
        client_socket, ASN1_RESPONE_EVENT::RESPONSE_HISTORY, SUCCESS,
        QString("%1:%2").arg(room_id).arg(rows.size()));
}

inline void handle_history_req(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t room_id = asn1_read_int(element, "history-req.room-id");
    if (client_state.current_chat_room_id != room_id) return;

    uint64_t before_id = asn1_read_int(element, "history-req.before-id");
    uint64_t limit = asn1_read_int(element, "history-req.limit");
    if (limit <= 0 || limit > 50) limit = 20;

    send_room_message_history(client_socket, room_id, before_id, limit);
}

inline void handle_join_room(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, FAILED, "Unauthorized");
        return;
    }

    uint64_t room_id = asn1_read_int(element, "join-room.room-id");
    QString password = asn1_read_string(element, "join-room.password");
    int join_type = asn1_read_int(element, "join-room.join-type");

    QSqlQuery query(db);
    query.prepare("SELECT password, limit_clients, needed_voice_join_power, needed_chat_join_power, needed_speak_power FROM rooms WHERE id = ?");
    query.addBindValue(room_id);
    if (!query.exec() || !query.next()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, FAILED, "Room not found");
        return;
    }

    QString real_pwd = query.value(0).toString();
    if (!real_pwd.isEmpty() && password != real_pwd) {
        // qDebug() << "Room password incorrect for room" << room_id << "join_type:" << join_type << "from" << client_socket;
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, PASSWORD_REQUIRED,
                                QString("%1:%2:Incorrect room password").arg(room_id).arg(join_type));
        return;
    }

    int needed_voice_power = query.value(2).toInt();
    int needed_chat_power = query.value(3).toInt();

    if (join_type == 0) {
        if (!user_is_admin(client_state.id)) {
            int user_chat_power = get_user_power(client_state.id, "u_chat_join_power");
            if (user_chat_power < needed_chat_power) {
                send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, FAILED,
                    QString("Insufficient chat join power (needed: %1, your power: %2)").arg(needed_chat_power).arg(user_chat_power));
                return;
            }
        }
        client_state.current_chat_room_id = room_id;
        // qDebug() << "User" << client_state.id << "joined chat room" << room_id;
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, SUCCESS, QString("%1:0").arg(room_id));
        send_room_message_history(client_socket, room_id, 0, 20);
    } else {
        if (!user_is_admin(client_state.id)) {
            int user_voice_power = get_user_power(client_state.id, "u_voice_join_power");
            if (user_voice_power < needed_voice_power) {
                send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, FAILED,
                    QString("Insufficient voice join power (needed: %1, your power: %2)").arg(needed_voice_power).arg(user_voice_power));
                return;
            }
        }
        if (client_state.current_voice_room_id > 0 &&
            client_state.current_voice_room_id != room_id) {
            voice_room_remove_user(client_state.current_voice_room_id, client_state.id);
        }
        int needed_speak_power = query.value(4).toInt();
        bool can_speak = user_is_admin(client_state.id) || (get_user_power(client_state.id, "u_speak_power") >= needed_speak_power);

        client_state.current_voice_room_id = room_id;
        voice_room_add_user(room_id, client_state.id, can_speak);
        // qDebug() << "User" << client_state.id << "validated voice for room" << room_id;
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_JOIN_ROOM, SUCCESS, QString("%1:1").arg(room_id));
        broadcast_room_list();
    }
}

inline void handle_leave_room(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    uint64_t room_id = asn1_read_int(element, "leave-room.room-id");
    int leave_type = asn1_read_int(element, "leave-room.leave-type");

    if (leave_type == 0) {
        if (client_state.current_chat_room_id == room_id) {
            client_state.current_chat_room_id = 0;
            // qDebug() << "User" << client_state.id << "left chat room" << room_id;
        }
    } else if (leave_type == 1) {
        if (client_state.current_voice_room_id == room_id) {
            client_state.current_voice_room_id = 0;
            voice_room_remove_user(room_id, client_state.id);
            // qDebug() << "User" << client_state.id << "left voice room" << room_id;
            broadcast_room_list();
        }
    }
}

inline void handle_chat_message(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t room_id = asn1_read_int(element, "chat.room-id");
    QString content = asn1_read_string(element, "chat.content").trimmed();
    if (content.isEmpty()) return;

    if (client_state.current_chat_room_id != room_id) {
        return;
    }

    qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (now_ms < client_state.chat_timeout_until) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CHAT, FAILED, "You are sending messages too fast. Rate limit exceeded, timed out for 5 seconds.");
        return;
    }
    while (!client_state.chat_msg_timestamps.isEmpty() && client_state.chat_msg_timestamps.first() < now_ms - 2000) {
        client_state.chat_msg_timestamps.removeFirst();
    }
    if (client_state.chat_msg_timestamps.size() >= 5) {
        client_state.chat_timeout_until = now_ms + 5000;
        client_state.chat_msg_timestamps.clear();
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CHAT, FAILED, "You are sending messages too fast. Rate limit exceeded, timed out for 5 seconds.");
        return;
    }
    client_state.chat_msg_timestamps.append(now_ms);

    QByteArray payload = content.toUtf8();
    QSqlQuery insert_q(db);
    insert_q.prepare("INSERT INTO messages (room_id, sender_id, payload, type) VALUES (?, ?, ?, 1) RETURNING id, server_timestamp");
    insert_q.addBindValue(room_id);
    insert_q.addBindValue(client_state.id);
    insert_q.addBindValue(payload);

    if (!insert_q.exec() || !insert_q.next()) {
        // qDebug() << "Failed to insert message:" << insert_q.lastError().text();
        return;
    }

    uint64_t msg_id = insert_q.value(0).toULongLong();
    uint64_t timestamp = insert_q.value(1).toULongLong();

    QString sender_avatar = db_scalar(db, "SELECT avatar_path FROM users WHERE id = ?", {client_state.id}).toString();

    // Broadcast only to clients currently in this room
    asn1_node msg_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &msg_elem) != ASN1_SUCCESS) {
        return;
    }
    asn1_write_value(msg_elem, "", "chat", 1);
    asn1_write_unsigned64(msg_elem, "chat.room-id", room_id);
    asn1_write_unsigned64(msg_elem, "chat.msg-id", msg_id);
    asn1_write_unsigned64(msg_elem, "chat.sender-id", client_state.id);
    asn1_write_string(msg_elem, "chat.sender", client_state.pending_name);
    asn1_write_string(msg_elem, "chat.sender-avatar", sender_avatar);
    asn1_write_string(msg_elem, "chat.content", content);
    asn1_write_unsigned64(msg_elem, "chat.msg-type", MESSAGE_TYPE::TEXT);
    asn1_write_unsigned64(msg_elem, "chat.timestamp", timestamp);

    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().state == SUCCESS && it.value().current_chat_room_id == room_id && it.key() && it.key()->isOpen()) {
            asn1_server_send_packet(it.key(), msg_elem);
        }
    }
    asn1_delete_structure(&msg_elem);
}

inline void handle_delete_msg(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t room_id = asn1_read_int(element, "delete-msg.room-id");
    uint64_t msg_id = asn1_read_int(element, "delete-msg.msg-id");

    QSqlQuery q(db);
    q.prepare("SELECT sender_id, payload, type FROM messages WHERE id = ? AND room_id = ?");
    q.addBindValue(msg_id);
    q.addBindValue(room_id);
    if (!q.exec() || !q.next()) return;

    uint64_t sender_id = q.value(0).toULongLong();
    if (sender_id != client_state.id) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_DELETE_MSG, FAILED, "Cannot delete other's message");
        return;
    }

    QByteArray payload = q.value(1).toByteArray();
    uint16_t msg_type = q.value(2).toUInt();

    // If message is a file/image/video, delete actual file from disk
    if (msg_type == MESSAGE_TYPE::IMAGE || msg_type == MESSAGE_TYPE::VIDEO || msg_type == MESSAGE_TYPE::OTHER) {
        QString rel_path = QString::fromUtf8(payload);
        if (rel_path.startsWith("files/room/") && !rel_path.contains("..")) {
            QString abs_path = QCoreApplication::applicationDirPath() + "/" + rel_path;
            QFile::remove(abs_path);
            // qDebug() << "File deleted from disk:" << abs_path;
        }
    }

    // Hard delete row from database for maximum privacy
    db_update(db, "DELETE FROM messages WHERE id = ?", {msg_id});
    // qDebug() << "Message row" << msg_id << "permanently deleted by user" << client_state.id;

    asn1_node del_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &del_elem) == ASN1_SUCCESS) {
        asn1_write_value(del_elem, "", "delete-msg", 1);
        asn1_write_unsigned64(del_elem, "delete-msg.room-id", room_id);
        asn1_write_unsigned64(del_elem, "delete-msg.msg-id", msg_id);

        for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
            if (it.value().state == SUCCESS && it.value().current_chat_room_id == room_id && it.key() && it.key()->isOpen()) {
                asn1_server_send_packet(it.key(), del_elem);
            }
        }
        asn1_delete_structure(&del_elem);
    }
}

inline void handle_update_server_settings(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_SERVER_SETTINGS, FAILED, "Unauthorized");
        return;
    }

    bool is_admin = db_scalar(db, "SELECT is_admin FROM users WHERE id = ?", {client_state.id}).toBool();
    if (!is_admin) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_SERVER_SETTINGS, FAILED, "Admin privilege required");
        return;
    }

    QString new_server_name = asn1_read_string(element, "update-server-settings.server-name").trimmed();
    QString new_server_pwd = asn1_read_string(element, "update-server-settings.server-password");

    if (!new_server_name.isEmpty()) {
        db_update(db, "INSERT INTO server_settings(key, value) VALUES('server_name', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", {new_server_name});
    }

    if (new_server_pwd == "__CLEAR__") {
        db_update(db, "INSERT INTO server_settings(key, value) VALUES('server_password', '') ON CONFLICT(key) DO UPDATE SET value = ''");
        // qDebug() << "Server password cleared by admin" << client_state.id;
    } else if (!new_server_pwd.isEmpty()) {
        db_update(db, "INSERT INTO server_settings(key, value) VALUES('server_password', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", {new_server_pwd});
        // qDebug() << "Server password updated by admin" << client_state.id;
    }

    QString cur_name = db_scalar(db, "SELECT value FROM server_settings WHERE key = 'server_name'").toString();
    if (cur_name.isEmpty()) cur_name = "Echo Server";

    // qDebug() << "Server settings updated by admin" << client_state.id << "name:" << cur_name << "pwd:" << (new_server_pwd.isEmpty() ? "<unchanged>" : "<set>");

    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().state == SUCCESS && it.key() && it.key()->isOpen()) {
            send_tcp_response_event(it.key(), ASN1_RESPONE_EVENT::RESPONSE_UPDATE_SERVER_SETTINGS, SUCCESS, cur_name);
        }
    }
}

inline void handle_kick_user(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    if (!user_is_admin(client_state.id) && !user_has_permission(client_state.id, "u_can_kick_users")) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_KICK_USER, FAILED, "No permission to kick users");
        return;
    }

    uint64_t target_user_id = asn1_read_int(element, "kick-user.user-id");
    uint64_t room_id = asn1_read_int(element, "kick-user.room-id");

    if (target_user_id == 0) return;
    if (user_is_admin(target_user_id) && !user_is_admin(client_state.id)) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_KICK_USER, FAILED, "Cannot kick an administrator");
        return;
    }

    QTcpSocket* target_socket = nullptr;
    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().id == target_user_id) {
            target_socket = it.key();
            break;
        }
    }

    if (!target_socket) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_KICK_USER, FAILED, "User not found online");
        return;
    }

    client_state_t &target_state = client_state_map[target_socket];
    uint64_t v_rid = target_state.current_voice_room_id;
    if (v_rid == 0 && room_id > 0) v_rid = room_id;

    if (v_rid > 0) {
        target_state.current_voice_room_id = 0;
        voice_room_remove_user(v_rid, target_user_id);
        broadcast_room_list();
        send_tcp_response_event(target_socket, ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS, "You were kicked from the voice room");
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_KICK_USER, SUCCESS, "User kicked from voice room");
    } else {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_KICK_USER, FAILED, "User is not in any voice room");
    }
}

inline void handle_update_room(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    if (!user_is_admin(client_state.id) && !user_has_permission(client_state.id, "u_can_create_rooms")) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROOM, FAILED, "No permission to edit room");
        return;
    }

    uint64_t room_id = asn1_read_int(element, "update-room.room-id");
    QString name = asn1_read_string(element, "update-room.name").trimmed();
    QString password = asn1_read_string(element, "update-room.password");
    uint64_t v_power = asn1_read_int(element, "update-room.needed-voice-join-power");
    uint64_t c_power = asn1_read_int(element, "update-room.needed-chat-join-power");
    uint64_t s_power = asn1_read_int(element, "update-room.needed-speak-power");

    if (room_id == 0 || name.isEmpty()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROOM, FAILED, "Invalid room data");
        return;
    }

    bool ok = false;
    if (password == "__CLEAR__") {
        ok = db_update(db, "UPDATE rooms SET name = ?, password = NULL, needed_voice_join_power = ?, needed_chat_join_power = ?, needed_speak_power = ? WHERE id = ?", {name, v_power, c_power, s_power, room_id});
    } else if (!password.isEmpty()) {
        ok = db_update(db, "UPDATE rooms SET name = ?, password = ?, needed_voice_join_power = ?, needed_chat_join_power = ?, needed_speak_power = ? WHERE id = ?", {name, password, v_power, c_power, s_power, room_id});
    } else {
        ok = db_update(db, "UPDATE rooms SET name = ?, needed_voice_join_power = ?, needed_chat_join_power = ?, needed_speak_power = ? WHERE id = ?", {name, v_power, c_power, s_power, room_id});
    }

    if (ok) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROOM, SUCCESS, "Room updated successfully");
        if (!password.isEmpty() && password != "__CLEAR__") {
            // Password changed: require re-authentication for anyone currently in room
            for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
                if (it.value().current_voice_room_id == room_id) {
                    it.value().current_voice_room_id = 0;
                    voice_room_remove_user(room_id, it.value().id);
                    send_tcp_response_event(it.key(), ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS,
                        "voice:Room password was updated. Please enter password to rejoin.");
                }
                if (it.value().current_chat_room_id == room_id) {
                    it.value().current_chat_room_id = 0;
                    send_tcp_response_event(it.key(), ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS,
                        QString("chat:%1:Room password was updated. Please enter password to rejoin.").arg(room_id));
                }
            }
        }
        recheck_active_user_permissions(0, room_id);
        broadcast_room_list();
    } else {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROOM, FAILED, "Failed to update room in database");
    }
}

#endif // ROOM_H
