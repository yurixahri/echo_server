#ifndef SERVER_ROLE_H
#define SERVER_ROLE_H

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include "db/db.h"
#include "utils/asn1_parser.h"
#include "common/types.h"
#include "common/functions.h"
#include "functions/voice.h"

inline bool user_is_admin(uint64_t user_id) {
    if (user_id == 0) return false;
    return db_scalar(db, "SELECT is_admin FROM users WHERE id = ?", {user_id}).toBool();
}

inline int get_user_power(uint64_t user_id, const QString &perm_name) {
    if (user_id == 0) return 0;
    if (user_is_admin(user_id)) return 1000000;

    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(MAX(rp.value), 0) "
              "FROM user_roles ur "
              "JOIN role_permissions rp ON ur.role_id = rp.role_id "
              "JOIN permissions p ON rp.permission_id = p.id "
              "WHERE ur.user_id = ? AND p.name = ?;");
    q.addBindValue(user_id);
    q.addBindValue(perm_name);
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

inline bool user_has_permission(uint64_t user_id, const QString &perm_name) {
    return get_user_power(user_id, perm_name) > 0;
}

inline void recheck_active_user_permissions(uint64_t target_user_id = 0, uint64_t target_room_id = 0) {
    bool need_broadcast_room_list = false;

    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        QTcpSocket *sock = it.key();
        client_state_t &st = it.value();
        if (st.state != SUCCESS || !sock || !sock->isOpen()) continue;

        uint64_t uid = st.id;
        if (target_user_id > 0 && uid != target_user_id) continue;

        bool is_admin = user_is_admin(uid);

        // 1. Voice room check
        uint64_t v_rid = st.current_voice_room_id;
        if (v_rid > 0 && (target_room_id == 0 || target_room_id == v_rid)) {
            QSqlQuery q(db);
            q.prepare("SELECT needed_voice_join_power, needed_speak_power FROM rooms WHERE id = ?");
            q.addBindValue(v_rid);
            if (q.exec() && q.next()) {
                int needed_voice = q.value(0).toInt();
                int needed_speak = q.value(1).toInt();

                int user_voice_power = is_admin ? 1000000 : get_user_power(uid, "u_voice_join_power");
                int user_speak_power = is_admin ? 1000000 : get_user_power(uid, "u_speak_power");

                if (!is_admin && user_voice_power < needed_voice) {
                    st.current_voice_room_id = 0;
                    voice_room_remove_user(v_rid, uid);
                    send_tcp_response_event(sock, ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS,
                        QString("voice:Insufficient voice join power (needed: %1, yours: %2)").arg(needed_voice).arg(user_voice_power));
                    need_broadcast_room_list = true;
                    // qDebug() << "Realtime perms: User" << uid << "kicked from voice room" << v_rid << "(power:" << user_voice_power << "<" << needed_voice << ")";
                } else {
                    bool can_speak = is_admin || (user_speak_power >= needed_speak);
                    {
                        QMutexLocker locker(&voice_auth_mutex);
                        voice_user_can_speak[uid] = can_speak;
                    }
                    // qDebug() << "Realtime perms: User" << uid << "voice speak power:" << user_speak_power
                             // << "needed:" << needed_speak << "can_speak:" << can_speak;
                }
            } else {
                st.current_voice_room_id = 0;
                voice_room_remove_user(v_rid, uid);
                send_tcp_response_event(sock, ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS, "voice:Room no longer exists");
                need_broadcast_room_list = true;
            }
        }

        // 2. Chat room check
        uint64_t c_rid = st.current_chat_room_id;
        if (c_rid > 0 && (target_room_id == 0 || target_room_id == c_rid)) {
            QSqlQuery q(db);
            q.prepare("SELECT needed_chat_join_power FROM rooms WHERE id = ?");
            q.addBindValue(c_rid);
            if (q.exec() && q.next()) {
                int needed_chat = q.value(0).toInt();
                int user_chat_power = is_admin ? 1000000 : get_user_power(uid, "u_chat_join_power");

                if (!is_admin && user_chat_power < needed_chat) {
                    st.current_chat_room_id = 0;
                    send_tcp_response_event(sock, ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS,
                        QString("chat:%1:Insufficient chat join power (needed: %2, yours: %3)").arg(c_rid).arg(needed_chat).arg(user_chat_power));
                    // qDebug() << "Realtime perms: User" << uid << "kicked from chat room" << c_rid << "(power:" << user_chat_power << "<" << needed_chat << ")";
                }
            } else {
                st.current_chat_room_id = 0;
                send_tcp_response_event(sock, ASN1_RESPONE_EVENT::RESPONSE_KICKED, SUCCESS,
                    QString("chat:%1:Room no longer exists").arg(c_rid));
            }
        }
    }

    if (need_broadcast_room_list) {
        broadcast_room_list();
    }
}

inline void handle_get_roles(QTcpSocket* client_socket, asn1_node element) {
    Q_UNUSED(element);
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    QSqlQuery role_q(db);
    if (!role_q.exec("SELECT id, name FROM roles ORDER BY id ASC;")) {
        // qDebug() << "Failed to query roles:" << role_q.lastError().text();
        return;
    }

    QList<QPair<uint64_t, QString>> roles;
    while (role_q.next()) {
        roles.append({role_q.value(0).toULongLong(), role_q.value(1).toString()});
    }

    asn1_node resp_elem = nullptr;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &resp_elem) != ASN1_SUCCESS) {
        return;
    }
    asn1_write_value(resp_elem, "", "role-list", 1);

    int idx = 1;
    for (const auto &r : roles) {
        uint64_t role_id = r.first;
        QString role_name = r.second;

        // Fetch permissions for this role
        QSqlQuery perm_q(db);
        perm_q.prepare("SELECT p.name, rp.value "
                       "FROM permissions p "
                       "LEFT JOIN role_permissions rp ON p.id = rp.permission_id AND rp.role_id = ? "
                       "ORDER BY p.id ASC;");
        perm_q.addBindValue(role_id);

        QJsonObject perms_obj;
        if (perm_q.exec()) {
            while (perm_q.next()) {
                QString p_name = perm_q.value(0).toString();
                int p_val = perm_q.value(1).isNull() ? 0 : perm_q.value(1).toInt();
                perms_obj[p_name] = p_val;
            }
        }
        QString perms_json = QString::fromUtf8(QJsonDocument(perms_obj).toJson(QJsonDocument::Compact));

        asn1_write_value(resp_elem, "role-list", "NEW", 1);
        QString prefix = QString("role-list.?%1").arg(idx);
        asn1_write_unsigned64(resp_elem, (prefix + ".id").toUtf8().constData(), role_id);
        asn1_write_string(resp_elem, (prefix + ".name").toUtf8().constData(), role_name);
        asn1_write_string(resp_elem, (prefix + ".permissions").toUtf8().constData(), perms_json);
        idx++;
    }

    asn1_server_send_packet(client_socket, resp_elem);
    asn1_delete_structure(&resp_elem);
}

inline void handle_create_role(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS || !user_is_admin(client_state.id)) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROLE, FAILED, "Admin required");
        return;
    }

    QString role_name = asn1_read_string(element, "create-role.name").trimmed();
    if (role_name.isEmpty()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROLE, FAILED, "Role name cannot be empty");
        return;
    }

    QVariant new_id = db_scalar(db, "INSERT INTO roles (name) VALUES (?) RETURNING id;", {role_name});
    if (!new_id.isValid()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROLE, FAILED, "Failed to create role");
        return;
    }

    uint64_t rid = new_id.toULongLong();
    // Default 0 for all permissions
    QSqlQuery q(db);
    q.exec(QString("INSERT INTO role_permissions (role_id, permission_id, value) "
                   "SELECT %1, id, 0 FROM permissions;").arg(rid));

    // qDebug() << "Admin" << client_state.id << "created role:" << role_name << "ID:" << rid;
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_CREATE_ROLE, SUCCESS, QString::number(rid));
    handle_get_roles(client_socket, nullptr);
}

inline void handle_update_role_perms(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS || !user_is_admin(client_state.id)) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROLE_PERMS, FAILED, "Admin required");
        return;
    }

    uint64_t role_id = asn1_read_int(element, "update-role-perms.role-id");
    QString perms_json = asn1_read_string(element, "update-role-perms.permissions");

    QJsonDocument doc = QJsonDocument::fromJson(perms_json.toUtf8());
    if (!doc.isObject()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROLE_PERMS, FAILED, "Invalid JSON data");
        return;
    }

    QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        QString perm_name = it.key();
        int val = it.value().toInt();

        QSqlQuery q(db);
        q.prepare("INSERT INTO role_permissions (role_id, permission_id, value) "
                  "VALUES (?, (SELECT id FROM permissions WHERE name = ?), ?) "
                  "ON CONFLICT(role_id, permission_id) DO UPDATE SET value = excluded.value;");
        q.addBindValue(role_id);
        q.addBindValue(perm_name);
        q.addBindValue(val);
        q.exec();
    }

    // qDebug() << "Admin" << client_state.id << "updated permissions for role:" << role_id;
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_UPDATE_ROLE_PERMS, SUCCESS, "Permissions saved");

    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().state == SUCCESS && it.key() && it.key()->isOpen()) {
            uint64_t uid = it.value().id;
            bool is_admin = user_is_admin(uid);
            bool can_kick = is_admin || user_has_permission(uid, "u_can_kick_users");
            send_tcp_response_event(it.key(), ASN1_RESPONE_EVENT::RESPONSE_PERMS_UPDATED, SUCCESS,
                                    QString("%1:%2").arg(is_admin ? 1 : 0).arg(can_kick ? 1 : 0));
        }
    }
    recheck_active_user_permissions(0, 0);
}

inline void handle_delete_role(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS || !user_is_admin(client_state.id)) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_DELETE_ROLE, FAILED, "Admin required");
        return;
    }

    uint64_t role_id = asn1_read_int(element, "delete-role.role-id");
    db_update(db, "DELETE FROM roles WHERE id = ?;", {role_id});

    // qDebug() << "Admin" << client_state.id << "deleted role:" << role_id;
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_DELETE_ROLE, SUCCESS, "Role deleted");
    handle_get_roles(client_socket, nullptr);
    recheck_active_user_permissions(0, 0);
}

inline void handle_get_user_roles(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS || !user_is_admin(client_state.id)) {
        return;
    }

    uint64_t target_user_id = asn1_read_int(element, "get-user-roles-req.user-id");

    // All roles
    QSqlQuery q(db);
    q.prepare("SELECT r.id, r.name, (CASE WHEN ur.user_id IS NOT NULL THEN 1 ELSE 0 END) "
              "FROM roles r "
              "LEFT JOIN user_roles ur ON r.id = ur.role_id AND ur.user_id = ? "
              "ORDER BY r.id ASC;");
    q.addBindValue(target_user_id);

    QJsonArray arr;
    if (q.exec()) {
        while (q.next()) {
            QJsonObject item;
            item["id"] = q.value(0).toInt();
            item["name"] = q.value(1).toString();
            item["assigned"] = q.value(2).toInt() == 1;
            arr.append(item);
        }
    }

    QString roles_json = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));

    asn1_node resp = nullptr;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &resp) != ASN1_SUCCESS) return;

    asn1_write_value(resp, "", "user-roles-list", 1);
    asn1_write_unsigned64(resp, "user-roles-list.user-id", target_user_id);
    asn1_write_string(resp, "user-roles-list.roles-json", roles_json);

    asn1_server_send_packet(client_socket, resp);
    asn1_delete_structure(&resp);
}

inline void handle_assign_user_roles(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS || !user_is_admin(client_state.id)) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_ASSIGN_USER_ROLES, FAILED, "Admin required");
        return;
    }

    uint64_t target_user_id = asn1_read_int(element, "assign-user-roles.user-id");
    QString role_ids_str = asn1_read_string(element, "assign-user-roles.role-ids");

    db_update(db, "DELETE FROM user_roles WHERE user_id = ?;", {target_user_id});

    QStringList ids = role_ids_str.split(',', Qt::SkipEmptyParts);
    for (const QString &id_str : ids) {
        uint64_t rid = id_str.trimmed().toULongLong();
        if (rid > 0) {
            db_update(db, "INSERT OR IGNORE INTO user_roles (user_id, role_id) VALUES (?, ?);", {target_user_id, rid});
        }
    }

    // qDebug() << "Admin" << client_state.id << "assigned roles" << role_ids_str << "to user" << target_user_id;
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_ASSIGN_USER_ROLES, SUCCESS, "Roles assigned");

    bool is_target_admin = user_is_admin(target_user_id);
    bool can_target_kick = is_target_admin || user_has_permission(target_user_id, "u_can_kick_users");
    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().id == target_user_id && it.key() && it.key()->isOpen()) {
            send_tcp_response_event(it.key(), ASN1_RESPONE_EVENT::RESPONSE_PERMS_UPDATED, SUCCESS,
                                    QString("%1:%2").arg(is_target_admin ? 1 : 0).arg(can_target_kick ? 1 : 0));
        }
    }
    recheck_active_user_permissions(target_user_id, 0);
}

#endif // SERVER_ROLE_H
