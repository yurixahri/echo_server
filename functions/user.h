#ifndef USER_H
#define USER_H

#include "db/db.h"
#include "utils/asn1_parser.h"
#include "common/types.h"
#include <QCryptographicHash>

void response_user_check_id(QTcpSocket* client_socket, bool is_approved){
    asn1_node element;
    int ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
    if (ret != ASN1_SUCCESS) {
        printf("Failed to create ASN.1 element!\n");
        return;
    }
    asn1_write_value(element, "", "response-user-check-id", 1);
    asn1_write_value(element, "response-user-check-id.status", is_approved ? "TRUE" : "FALSE", 1);
    asn1_server_send_packet(client_socket, element);
}

void start_existing_user_handshake(QTcpSocket* client_socket, client_state_t &client_state, QString &uuid){
    for (client_state_t &other_client: client_state_map) {
        if (other_client.pending_uuid == uuid && other_client.state == SUCCESS){
            // if user is already login server, reject le socket
            client_state_map.remove(client_socket);
            client_socket->disconnectFromHost();
            response_user_check_id(client_socket, false);
            return;
        }
    }
    bool exists = db_scalar(db, "SELECT EXISTS(SELECT 1 FROM users WHERE uuid = ?)", {uuid}).toBool();
    if (!exists){
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    uint64_t id = db_scalar(db, "SELECT id FROM users WHERE uuid = ? LIMIT 1", {uuid}).toULongLong();
    bool name_changed = db_update( db, "UPDATE users SET name = ? WHERE uuid = ? AND name != ?", {client_state.pending_name, uuid, client_state.pending_name});
    if (name_changed){
        // todo: send change name event for other client to update
    }
    client_state.state = SUCCESS;
    client_state.id = id;
    client_state.pending_uuid = uuid;
    response_user_check_id(client_socket, true);
}

void send_new_user_challenge(QTcpSocket* client_socket, client_state_t &client_state){
    asn1_node element;
    int ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
    if (ret != ASN1_SUCCESS) {
        printf("Failed to create ASN.1 element!\n");
        return;
    }
    QByteArray uuid = client_state.pending_uuid.toUtf8();
    asn1_write_value(element, "", "new-user-challenge", 1);
    asn1_write_value(element, "new-user-challenge.uuid", uuid.constData(), uuid.size());
    asn1_write_value(element, "new-user-challenge.challenge", client_state.challenge.constData(), client_state.challenge.size());
    asn1_write_unsigned64(element, "new-user-challenge.difficulty", client_state.difficulty);
    asn1_server_send_packet(client_socket, element);
}

void start_new_user_handshake(QTcpSocket* client_socket, client_state_t &client_state){
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

void check_user_id(QTcpSocket* client_socket, asn1_node element){
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != WAITTING_INIT) {
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        return;
    }

    // asn1_print_structure(stdout, element, "", ASN1_PRINT_ALL);
    QString uuid = asn1_read_string(element, "client-check-id.uuid");
    QString name = asn1_read_string(element, "client-check-id.name");
    client_state.pending_name = name;

    if (uuid.isEmpty()) {
        start_new_user_handshake(client_socket, client_state);
    }else{
        start_existing_user_handshake(client_socket, client_state, uuid);
    }
}

bool check_pow_difficulty(const QByteArray &hash, uint difficulty){
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

bool verify_new_user_proof(client_state_t &client_state, uint64_t solution){
    QByteArray data = client_state.pending_uuid.toUtf8() + client_state.challenge + QByteArray::number(solution);
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    return check_pow_difficulty(hash, client_state.difficulty);
}

void check_new_user_proof(QTcpSocket *client_socket, asn1_node element){
    client_state_t &client_state = client_state_map[client_socket];
    // asn1_print_structure(stdout, element, "", ASN1_PRINT_ALL);

    if (client_state.state != WAITTING_PROOF) {
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    if (QDateTime::currentSecsSinceEpoch() > client_state.challenge_expire){
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    QString uuid = asn1_read_string(element, "new-user-proof.uuid");

    if (uuid != client_state.pending_uuid) {
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    uint64_t solution = asn1_read_int(element, "new-user-proof.solution");

    if (!verify_new_user_proof(client_state, solution)) {
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    QVariant result = db_scalar(db, "INSERT INTO users(uuid, name) VALUES(?, ?) RETURNING id", {client_state.pending_uuid, client_state.pending_name});
    if (!result.isValid()) {
        client_state_map.remove(client_socket);
        client_socket->disconnectFromHost();
        response_user_check_id(client_socket, false);
        return;
    }

    client_state.id = result.toULongLong();
    client_state.state = SUCCESS;
    response_user_check_id(client_socket, true);
}



#endif // USER_H
