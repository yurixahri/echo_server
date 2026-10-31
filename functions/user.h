#ifndef USER_H
#define USER_H

#include "db/db.h"
#include "utils/asn1_parser.h"
#include "common/functions.h"
#include "common/types.h"
#include "functions/room.h"
#include <QCryptographicHash>

void check_user_id(QTcpSocket* client_socket, asn1_node element);

void start_existing_user_handshake(QTcpSocket* client_socket, client_state_t &client_state, QString &uuid);

void start_new_user_handshake(QTcpSocket* client_socket, client_state_t &client_state);
void send_new_user_challenge(QTcpSocket* client_socket, client_state_t &client_state);
void check_new_user_proof(QTcpSocket *client_socket, asn1_node element);
bool verify_new_user_proof(client_state_t &client_state, uint64_t solution);
bool check_pow_difficulty(const QByteArray &hash, uint difficulty);

void claim_admin_privilege(QTcpSocket* client_socket, asn1_node element);

// implemment
inline void check_user_id(QTcpSocket* client_socket, asn1_node element){
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != WAITTING_INIT) {
        // qDebug() << "Rejected: client state is not WAITTING_INIT:" << client_state.state;
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    // asn1_print_structure(stdout, element, "", ASN1_PRINT_ALL);
    QString server_pwd = db_scalar(db, "SELECT value FROM server_settings WHERE key = 'server_password'").toString();
    QString client_pwd = asn1_read_string(element, "client-check-id.password");
    if (!server_pwd.isEmpty() && client_pwd != server_pwd) {
        // qDebug() << "Server password required / incorrect from" << client_socket;
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, PASSWORD_REQUIRED, "Server password required");
        return;
    }

    QString uuid = asn1_read_string(element, "client-check-id.uuid");
    QString name = asn1_read_string(element, "client-check-id.name");
    client_state.pending_name = name;
    // qDebug() << "Server check_user_id from client:" << client_socket << "uuid:" << uuid << "name:" << name;

    if (uuid.isEmpty()) {
        start_new_user_handshake(client_socket, client_state);
    }else{
        start_existing_user_handshake(client_socket, client_state, uuid);
    }
}

inline void start_existing_user_handshake(QTcpSocket* client_socket, client_state_t &client_state, QString &uuid){
    for (auto it = client_state_map.begin(); it != client_state_map.end(); ) {
        if (it.key() != client_socket && it.value().pending_uuid == uuid) {
            QTcpSocket *old_socket = it.key();
            it = client_state_map.erase(it);
            old_socket->disconnectFromHost();
            old_socket->deleteLater();
        } else {
            ++it;
        }
    }

    bool exists = db_scalar(db, "SELECT EXISTS(SELECT 1 FROM users WHERE uuid = ?)", {uuid}).toBool();
    if (!exists){
        start_new_user_handshake(client_socket, client_state);
        return;
    }

    uint64_t id = db_scalar(db, "SELECT id FROM users WHERE uuid = ? LIMIT 1", {uuid}).toULongLong();
    QString avt = db_scalar(db, "SELECT avatar_path FROM users WHERE id = ?", {id}).toString();
    bool is_admin = db_scalar(db, "SELECT is_admin FROM users WHERE id = ?", {id}).toBool();
    bool can_kick = user_has_permission(id, "u_can_kick_users");
    bool name_changed = db_update( db, "UPDATE users SET name = ? WHERE uuid = ? AND name != ?", {client_state.pending_name, uuid, client_state.pending_name});
    if (name_changed){
        // todo: send change name event for other client to update
    }
    client_state.state = SUCCESS;
    client_state.id = id;
    client_state.pending_uuid = uuid;
    client_state.avatar_path = avt;
    QString srv_name = db_scalar(db, "SELECT value FROM server_settings WHERE key = 'server_name'").toString();
    if (srv_name.isEmpty()) srv_name = "Echo Server";
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, SUCCESS, QString("%1:%2:%3:%4:%5").arg(id).arg(avt).arg(srv_name).arg(is_admin ? 1 : 0).arg(can_kick ? 1 : 0));
    send_room_list_packet(client_socket);
}

inline void start_new_user_handshake(QTcpSocket* client_socket, client_state_t &client_state){
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray challenge(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(challenge.data()),
        challenge.size() / sizeof(quint32)
        );

    client_state.pending_uuid = uuid;
    client_state.challenge = challenge;
    client_state.challenge_expire = QDateTime::currentSecsSinceEpoch() + 30;
    client_state.difficulty = 16;
    client_state.state = WAITTING_PROOF;

    send_new_user_challenge(client_socket, client_state);
}

inline void send_new_user_challenge(QTcpSocket* client_socket, client_state_t &client_state){
    asn1_node element;
    int ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
    if (ret != ASN1_SUCCESS) {
        // printf("Failed to create ASN.1 element!\n");
        return;
    }
    asn1_write_value(element, "", "new-user-challenge", 1);
    asn1_write_string(element, "new-user-challenge.uuid", client_state.pending_uuid);
    asn1_write_byte_array(element, "new-user-challenge.challenge",client_state.challenge);
    asn1_write_unsigned64(element, "new-user-challenge.difficulty", client_state.difficulty);
    asn1_server_send_packet(client_socket, element);
}

inline void check_new_user_proof(QTcpSocket *client_socket, asn1_node element){
    client_state_t &client_state = client_state_map[client_socket];
    // asn1_print_structure(stdout, element, "", ASN1_PRINT_ALL);

    if (client_state.state != WAITTING_PROOF) {
        // qDebug() << "Proof rejected: state is not WAITTING_PROOF (" << client_state.state << ")";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, FAILED);
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    if (QDateTime::currentSecsSinceEpoch() > client_state.challenge_expire){
        // qDebug() << "Proof rejected: challenge expired";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, FAILED);
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    QString uuid = asn1_read_string(element, "new-user-proof.uuid");

    if (uuid != client_state.pending_uuid) {
        // qDebug() << "Proof rejected: uuid mismatch (" << uuid << "!=" << client_state.pending_uuid << ")";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, FAILED);
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    uint64_t solution = asn1_read_int(element, "new-user-proof.solution");
    // qDebug() << "Server received proof solution:" << solution;

    if (!verify_new_user_proof(client_state, solution)) {
        // qDebug() << "Proof rejected: verify_new_user_proof failed for solution:" << solution;
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, FAILED);
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    QVariant result = db_scalar(db, "INSERT INTO users(uuid, name) VALUES(?, ?) RETURNING id", {client_state.pending_uuid, client_state.pending_name});
    if (!result.isValid()) {
        // qDebug() << "Proof rejected: db insert failed";
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, FAILED);
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    client_state.id = result.toULongLong();
    client_state.state = SUCCESS;
    // qDebug() << "New user registered successfully! ID:" << client_state.id << "UUID:" << client_state.pending_uuid;
    QString srv_name = db_scalar(db, "SELECT value FROM server_settings WHERE key = 'server_name'").toString();
    if (srv_name.isEmpty()) srv_name = "Echo Server";
    bool can_kick = user_has_permission(client_state.id, "u_can_kick_users");
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CHECK_ID, SUCCESS, QString("%1::%2:0:%3").arg(client_state.id).arg(srv_name).arg(can_kick ? 1 : 0));
    send_room_list_packet(client_socket);
}

inline bool verify_new_user_proof(client_state_t &client_state, uint64_t solution){
    QByteArray data = client_state.pending_uuid.toUtf8() + client_state.challenge + QByteArray::number(solution);
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    return check_pow_difficulty(hash, client_state.difficulty);
}

inline bool check_pow_difficulty(const QByteArray &hash, uint difficulty){
    if (difficulty > static_cast<uint>(hash.size() * 8)) return false;

    uint full_bytes = difficulty / 8;
    uint remaining_bits = difficulty % 8;

    for (uint i = 0; i < full_bytes; i++) {
        if (static_cast<quint8>(hash[i]) != 0) return false;
    }

    if (remaining_bits > 0) {
        quint8 value = static_cast<quint8>(hash[full_bytes]);
        quint8 mask = static_cast<quint8>(0xFF << (8 - remaining_bits));

        if ((value & mask) != 0) return false;
    }

    return true;
}

void claim_admin_privilege(QTcpSocket* client_socket, asn1_node element){
    client_state_t &client_state = client_state_map[client_socket];
    QString key = asn1_read_string(element, "claim-admin-privilege.key");
    QString value = db_scalar(db,
                              "SELECT value FROM server_settings WHERE key = ? AND value = ? AND value != ''",
                              {"initial_admin_token", key}).toString();
    if (!value.isEmpty()){
        db_update( db, "UPDATE users SET is_admin = 1 WHERE id = ?", {client_state.id});
        db_update( db, "UPDATE server_settings SET value = '' WHERE key = ?", {"initial_admin_token"});
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CLAIM_ADMIN_PRIVILEGE, SUCCESS);
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_PERMS_UPDATED, SUCCESS, "1:1");
        recheck_active_user_permissions(client_state.id, 0);
    }else{
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_USER_CLAIM_ADMIN_PRIVILEGE, FAILED);
    }

}

extern qint64 server_start_timestamp;

inline void handle_get_my_info(QTcpSocket* client_socket, asn1_node element) {
    Q_UNUSED(element);
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    QSqlQuery q(db);
    q.prepare("SELECT uuid, name, avatar_path, created_at FROM users WHERE id = ?");
    q.addBindValue(client_state.id);
    if (!q.exec() || !q.next()) return;

    QString uuid = q.value(0).toString();
    QString name = q.value(1).toString();
    QString avatar = q.value(2).toString();
    uint64_t created_at = q.value(3).toULongLong();
    uint64_t uptime = (uint64_t)qMax((qint64)0, QDateTime::currentSecsSinceEpoch() - server_start_timestamp);

    asn1_node resp_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &resp_elem) == ASN1_SUCCESS) {
        asn1_write_value(resp_elem, "", "my-info", 1);
        asn1_write_unsigned64(resp_elem, "my-info.id", client_state.id);
        asn1_write_string(resp_elem, "my-info.uuid", uuid);
        asn1_write_string(resp_elem, "my-info.name", name);
        asn1_write_string(resp_elem, "my-info.avatar", avatar);
        asn1_write_unsigned64(resp_elem, "my-info.created-at", created_at);
        asn1_write_unsigned64(resp_elem, "my-info.server-uptime", uptime);
        asn1_server_send_packet(client_socket, resp_elem);
        asn1_delete_structure(&resp_elem);
    }
}

inline void handle_poke_user(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t target_user_id = asn1_read_int(element, "poke-user.target-user-id");
    QString msg = asn1_read_string(element, "poke-user.message").trimmed();

    if (target_user_id == 0 || target_user_id == client_state.id) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_POKE_USER, FAILED, "Cannot poke yourself");
        return;
    }

    static QMap<uint64_t, qint64> last_poke_timestamps;
    qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (last_poke_timestamps.contains(client_state.id)) {
        qint64 diff = now_ms - last_poke_timestamps[client_state.id];
        if (diff < 5000) {
            int remain_sec = qCeil((5000 - diff) / 1000.0);
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_POKE_USER, FAILED,
                                    QString("Poke on cooldown (%1s)").arg(remain_sec));
            return;
        }
    }

    QTcpSocket *target_socket = nullptr;
    QString target_name;
    for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
        if (it.value().id == target_user_id && it.value().state == SUCCESS && it.key() && it.key()->isOpen()) {
            target_socket = it.key();
            target_name = it.value().pending_name;
            break;
        }
    }

    if (!target_socket) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_POKE_USER, FAILED, "User is not online");
        return;
    }

    last_poke_timestamps[client_state.id] = now_ms;

    QString poke_payload = QString("%1:%2:%3").arg(client_state.pending_name).arg(client_state.id).arg(msg);
    send_tcp_response_event(target_socket, ASN1_RESPONE_EVENT::RESPONSE_POKED, SUCCESS, poke_payload);
    send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_POKE_USER, SUCCESS, QString("Poked %1!").arg(target_name));
}

inline void handle_get_members(QTcpSocket* client_socket, asn1_node element) {
    Q_UNUSED(element);
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    QSet<uint64_t> online_ids;
    for (const auto &st : client_state_map) {
        if (st.state == SUCCESS) online_ids.insert(st.id);
    }

    QSqlQuery q(db);
    if (!q.exec("SELECT id, name, COALESCE(avatar_path, ''), is_admin FROM users ORDER BY id ASC;")) {
        return;
    }

    struct temp_member_item_t {
        uint64_t id = 0;
        QString name;
        QString avatar;
        bool is_online = false;
        bool is_admin = false;
    };

    QList<temp_member_item_t> member_rows;
    while (q.next()) {
        temp_member_item_t m;
        m.id = q.value(0).toULongLong();
        m.name = q.value(1).toString();
        m.avatar = q.value(2).toString();
        m.is_admin = q.value(3).toInt() == 1;
        m.is_online = online_ids.contains(m.id);
        member_rows.append(m);
    }

    std::sort(member_rows.begin(), member_rows.end(), [](const temp_member_item_t &a, const temp_member_item_t &b) {
        return natural_sort_less_than(a.name, b.name);
    });

    asn1_node resp_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &resp_elem) != ASN1_SUCCESS) return;
    asn1_write_value(resp_elem, "", "member_list", 1);

    int idx = 1;
    for (const auto &m : member_rows) {
        asn1_write_value(resp_elem, "member_list", "NEW", 1);
        QString prefix = QString("member_list.?%1").arg(idx);
        asn1_write_unsigned64(resp_elem, (prefix + ".id").toUtf8().constData(), m.id);
        asn1_write_string(resp_elem, (prefix + ".name").toUtf8().constData(), m.name);
        asn1_write_string(resp_elem, (prefix + ".avatar").toUtf8().constData(), m.avatar);
        asn1_write_bool(resp_elem, (prefix + ".is-online").toUtf8().constData(), m.is_online);
        asn1_write_bool(resp_elem, (prefix + ".is-admin").toUtf8().constData(), m.is_admin);
        idx++;
    }

    asn1_server_send_packet(client_socket, resp_elem);
    asn1_delete_structure(&resp_elem);
}

inline void handle_get_member_detail(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t target_user_id = asn1_read_int(element, "get-member-detail-req.user-id");
    if (target_user_id == 0) return;

    QSqlQuery q(db);
    q.prepare("SELECT id, name, COALESCE(avatar_path, ''), created_at, is_admin FROM users WHERE id = ?");
    q.addBindValue(target_user_id);
    if (!q.exec() || !q.next()) return;

    uint64_t uid = q.value(0).toULongLong();
    QString name = q.value(1).toString();
    QString avatar = q.value(2).toString();
    uint64_t created_at = q.value(3).toULongLong();
    bool is_admin = q.value(4).toInt() == 1;

    bool is_online = false;
    for (const auto &st : client_state_map) {
        if (st.state == SUCCESS && st.id == uid) {
            is_online = true;
            break;
        }
    }

    QSqlQuery rq(db);
    rq.prepare("SELECT r.name FROM roles r JOIN user_roles ur ON r.id = ur.role_id WHERE ur.user_id = ? ORDER BY r.id ASC");
    rq.addBindValue(uid);
    QStringList role_names;
    if (rq.exec()) {
        while (rq.next()) {
            role_names.append(rq.value(0).toString());
        }
    }
    QString roles_csv = role_names.join(",");

    asn1_node resp_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &resp_elem) == ASN1_SUCCESS) {
        asn1_write_value(resp_elem, "", "member_detail", 1);
        asn1_write_unsigned64(resp_elem, "member_detail.id", uid);
        asn1_write_string(resp_elem, "member_detail.name", name);
        asn1_write_string(resp_elem, "member_detail.avatar", avatar);
        asn1_write_unsigned64(resp_elem, "member_detail.created-at", created_at);
        asn1_write_unsigned64(resp_elem, "member_detail.last-seen", is_admin ? 1 : 0);
        asn1_write_unsigned64(resp_elem, "member_detail.status", is_online ? 1 : 0);
        if (roles_csv.isEmpty()) {
            asn1_write_value(resp_elem, "member_detail.roles", NULL, 0);
        } else {
            QByteArray utf8 = roles_csv.toUtf8();
            asn1_write_value(resp_elem, "member_detail.roles", utf8.data(), utf8.size());
        }
        asn1_server_send_packet(client_socket, resp_elem);
        asn1_delete_structure(&resp_elem);
    }
}


#endif // USER_H
