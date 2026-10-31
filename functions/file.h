#ifndef SERVER_FILE_H
#define SERVER_FILE_H

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QTcpSocket>
#include "db/db.h"
#include "utils/asn1_parser.h"
#include "common/functions.h"
#include "common/types.h"

void broadcast_room_list();

struct server_upload_t {
    QString upload_id;
    uint64_t room_id;
    QString orig_filename;
    uint64_t total_size = 0;
    uint64_t received_size = 0;
    uint16_t msg_type = MESSAGE_TYPE::OTHER;
    QString temp_path;
};

inline QMap<QString, server_upload_t> server_uploads;

inline QString get_deduplicated_filename(const QString &dir_path, const QString &orig_name) {
    QFileInfo fi(orig_name);
    QString base = fi.completeBaseName();
    QString ext = fi.suffix().isEmpty() ? "" : "." + fi.suffix();

    QString name = orig_name;
    int count = 1;
    while (QFile::exists(dir_path + "/" + name)) {
        name = QString("%1 (%2)%3").arg(base).arg(count).arg(ext);
        count++;
    }
    return name;
}

inline void handle_file_upload_chunk(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Unauthorized");
        return;
    }

    uint64_t room_id = asn1_read_int(element, "file-upload-chunk.room-id");
    if (client_state.current_chat_room_id != room_id) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Not in this chat room");
        return;
    }

    uint64_t total_size = asn1_read_int(element, "file-upload-chunk.total-size");
    if (total_size > 10 * 1024 * 1024) { // 10MB limit
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "File size exceeds 10MB limit");
        return;
    }

    QString upload_id = asn1_read_string(element, "file-upload-chunk.upload-id");
    QString file_name = asn1_read_string(element, "file-upload-chunk.file-name");
    uint64_t offset = asn1_read_int(element, "file-upload-chunk.offset");
    uint16_t msg_type = asn1_read_int(element, "file-upload-chunk.msg-type");
    QByteArray data = asn1_read_bytes(element, "file-upload-chunk.data");

    if (upload_id.isEmpty() || file_name.isEmpty() || data.isEmpty()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Invalid file chunk");
        return;
    }

    QString temp_dir = QCoreApplication::applicationDirPath() + "/files/temp";
    QDir().mkpath(temp_dir);
    QString temp_path = temp_dir + "/" + upload_id + ".tmp";

    QFile temp_file(temp_path);
    if (offset == 0) {
        if (!temp_file.open(QIODevice::WriteOnly)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Cannot create temp file");
            return;
        }
        server_upload_t up;
        up.upload_id = upload_id;
        up.room_id = room_id;
        up.orig_filename = QFileInfo(file_name).fileName();
        up.total_size = total_size;
        up.received_size = 0;
        up.msg_type = msg_type;
        up.temp_path = temp_path;
        server_uploads.insert(upload_id, up);
    } else {
        if (!temp_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Cannot append temp file");
            return;
        }
    }

    temp_file.seek(offset);
    temp_file.write(data);
    temp_file.close();

    if (!server_uploads.contains(upload_id)) return;
    server_uploads[upload_id].received_size += data.size();

    // When upload complete
    if (offset + data.size() >= total_size) {
        server_upload_t up = server_uploads.take(upload_id);

        QString room_files_dir = QCoreApplication::applicationDirPath() + "/files/room/" + QString::number(room_id);
        QDir().mkpath(room_files_dir);

        QString safe_orig_name = QFileInfo(up.orig_filename).fileName();
        QString final_name = get_deduplicated_filename(room_files_dir, safe_orig_name);
        QString final_abs_path = room_files_dir + "/" + final_name;

        QFile::remove(final_abs_path);
        if (!QFile::rename(up.temp_path, final_abs_path)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Failed to store file");
            return;
        }

        QString rel_path = QString("files/room/%1/%2").arg(room_id).arg(final_name);

        QSqlQuery insert_q(db);
        insert_q.prepare("INSERT INTO messages (room_id, sender_id, payload, type) VALUES (?, ?, ?, ?) RETURNING id, server_timestamp");
        insert_q.addBindValue(room_id);
        insert_q.addBindValue(client_state.id);
        insert_q.addBindValue(rel_path.toUtf8());
        insert_q.addBindValue(up.msg_type);

        if (!insert_q.exec() || !insert_q.next()) {
            // qDebug() << "Failed to insert file message:" << insert_q.lastError().text();
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, FAILED, "Failed to save message");
            return;
        }

        uint64_t msg_id = insert_q.value(0).toULongLong();
        uint64_t timestamp = insert_q.value(1).toULongLong();

        QString sender_avatar = db_scalar(db, "SELECT avatar_path FROM users WHERE id = ?", {client_state.id}).toString();

        // Broadcast to clients currently in this room
        asn1_node msg_elem = NULL;
        if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &msg_elem) == ASN1_SUCCESS) {
            asn1_write_value(msg_elem, "", "chat", 1);
            asn1_write_unsigned64(msg_elem, "chat.room-id", room_id);
            asn1_write_unsigned64(msg_elem, "chat.msg-id", msg_id);
            asn1_write_unsigned64(msg_elem, "chat.sender-id", client_state.id);
            asn1_write_string(msg_elem, "chat.sender", client_state.pending_name);
            asn1_write_string(msg_elem, "chat.sender-avatar", sender_avatar);
            asn1_write_string(msg_elem, "chat.content", rel_path);
            asn1_write_unsigned64(msg_elem, "chat.msg-type", up.msg_type);
            asn1_write_unsigned64(msg_elem, "chat.timestamp", timestamp);

            for (auto it = client_state_map.begin(); it != client_state_map.end(); ++it) {
                if (it.value().state == SUCCESS && it.value().current_chat_room_id == room_id && it.key() && it.key()->isOpen()) {
                    asn1_server_send_packet(it.key(), msg_elem);
                }
            }
            asn1_delete_structure(&msg_elem);
        }

        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_FILE_UPLOAD, SUCCESS, rel_path);
    }
}

inline void handle_file_download_req(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) return;

    uint64_t room_id = asn1_read_int(element, "file-download-req.room-id");
    QString rel_path = asn1_read_string(element, "file-download-req.file-path");
    rel_path.replace("\\", "/");
    uint64_t offset = asn1_read_int(element, "file-download-req.offset");

    // Prevent directory traversal
    if (rel_path.contains("..") || (!rel_path.startsWith("files/room/") && !rel_path.startsWith("files/avt/"))) {
        // qDebug() << "Invalid file path in download request:" << rel_path;
        return;
    }

    bool is_avatar = rel_path.startsWith("files/avt/");
    if (!is_avatar && client_state.current_chat_room_id != room_id) {
        // qDebug() << "Rejected download: client" << client_state.id << "not in room" << room_id;
        return;
    }

    QString abs_path = QCoreApplication::applicationDirPath() + "/" + rel_path;
    QFile file(abs_path);
    if (!file.open(QIODevice::ReadOnly)) {
        // qDebug() << "Download failed, file not found:" << abs_path;
        return;
    }

    uint64_t total_size = file.size();
    if (offset >= total_size) return;

    file.seek(offset);
    const qint64 CHUNK_SIZE = 32768; // 32 KB chunk
    QByteArray chunk_data = file.read(CHUNK_SIZE);

    asn1_node chunk_elem = NULL;
    if (asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &chunk_elem) == ASN1_SUCCESS) {
        asn1_write_value(chunk_elem, "", "file-download-chunk", 1);
        asn1_write_unsigned64(chunk_elem, "file-download-chunk.room-id", room_id);
        asn1_write_string(chunk_elem, "file-download-chunk.file-path", rel_path);
        asn1_write_unsigned64(chunk_elem, "file-download-chunk.total-size", total_size);
        asn1_write_unsigned64(chunk_elem, "file-download-chunk.offset", offset);
        asn1_write_byte_array(chunk_elem, "file-download-chunk.data", chunk_data);

        asn1_server_send_packet(client_socket, chunk_elem);
        asn1_delete_structure(&chunk_elem);
    }
}

inline bool is_valid_avatar_file(const QString &file_path, const QString &orig_filename) {
    QString ext = QFileInfo(orig_filename).suffix().toLower();
    static const QSet<QString> allowed_exts = {"png", "jpg", "jpeg", "webp", "avif", "bmp", "gif"};
    if (!allowed_exts.contains(ext)) return false;

    QFile f(file_path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray header = f.read(16);
    f.close();

    if (header.size() < 4) return false;
    const unsigned char *b = reinterpret_cast<const unsigned char*>(header.constData());

    if (ext == "png" && header.size() >= 8) {
        return (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
                b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A);
    }
    if ((ext == "jpg" || ext == "jpeg") && header.size() >= 3) {
        return (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF);
    }
    if (ext == "gif" && header.size() >= 6) {
        return (header.startsWith("GIF87a") || header.startsWith("GIF89a"));
    }
    if (ext == "bmp" && header.size() >= 2) {
        return (b[0] == 0x42 && b[1] == 0x4D);
    }
    if (ext == "webp" && header.size() >= 12) {
        return (header.startsWith("RIFF") && header.mid(8, 4) == "WEBP");
    }
    if (ext == "avif" && header.size() >= 12) {
        return (header.mid(4, 4) == "ftyp" &&
               (header.mid(8, 4) == "avif" || header.mid(8, 4) == "avis" || header.mid(8, 4) == "mif1"));
    }

    return true;
}

inline void handle_avatar_upload_chunk(QTcpSocket* client_socket, asn1_node element) {
    client_state_t &client_state = client_state_map[client_socket];
    if (client_state.state != SUCCESS) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Unauthorized");
        return;
    }

    uint64_t total_size = asn1_read_int(element, "avatar-upload-chunk.total-size");
    if (total_size > 2 * 1024 * 1024) { // 2MB limit
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Avatar exceeds 2MB limit");
        return;
    }

    QString upload_id = asn1_read_string(element, "avatar-upload-chunk.upload-id");
    QString file_name = asn1_read_string(element, "avatar-upload-chunk.file-name");
    uint64_t offset = asn1_read_int(element, "avatar-upload-chunk.offset");
    QByteArray data = asn1_read_bytes(element, "avatar-upload-chunk.data");

    if (upload_id.isEmpty() || file_name.isEmpty() || data.isEmpty()) {
        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Invalid chunk");
        return;
    }

    QString temp_dir = QCoreApplication::applicationDirPath() + "/files/temp";
    QDir().mkpath(temp_dir);
    QString temp_path = temp_dir + "/avt_" + upload_id + ".tmp";

    QFile temp_file(temp_path);
    if (offset == 0) {
        if (!temp_file.open(QIODevice::WriteOnly)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Cannot create temp file");
            return;
        }
    } else {
        if (!temp_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Cannot write temp file");
            return;
        }
    }

    temp_file.seek(offset);
    temp_file.write(data);
    temp_file.close();

    if (offset + data.size() >= total_size) {
        if (!is_valid_avatar_file(temp_path, file_name)) {
            QFile::remove(temp_path);
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Invalid image format. Allowed: PNG, JPG, WEBP, AVIF, BMP, GIF");
            return;
        }

        QString avt_dir = QCoreApplication::applicationDirPath() + "/files/avt";
        QDir().mkpath(avt_dir);

        QString ext = QFileInfo(file_name).suffix().toLower();
        if (ext.isEmpty()) ext = "png";
        QString final_name = QString("%1_%2.%3").arg(client_state.id).arg(QDateTime::currentSecsSinceEpoch()).arg(ext);
        QString final_abs = avt_dir + "/" + final_name;

        // Delete old avatar file if present
        QString old_avatar = db_scalar(db, "SELECT avatar_path FROM users WHERE id = ?", {client_state.id}).toString();
        if (!old_avatar.isEmpty()) {
            QString old_abs = QCoreApplication::applicationDirPath() + "/" + old_avatar;
            QFile::remove(old_abs);
            // qDebug() << "Old avatar removed:" << old_abs;
        }

        QFile::remove(final_abs);
        if (!QFile::rename(temp_path, final_abs)) {
            send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, FAILED, "Cannot store avatar");
            return;
        }

        QString rel_path = "files/avt/" + final_name;
        db_update(db, "UPDATE users SET avatar_path = ? WHERE id = ?", {rel_path, client_state.id});
        client_state.avatar_path = rel_path;
        // qDebug() << "Avatar updated for user" << client_state.id << "->" << rel_path;

        send_tcp_response_event(client_socket, ASN1_RESPONE_EVENT::RESPONSE_AVATAR_UPLOAD, SUCCESS, rel_path);
        if (client_state.current_voice_room_id > 0) {
            broadcast_room_list();
        }
    }
}

#endif // SERVER_FILE_H
