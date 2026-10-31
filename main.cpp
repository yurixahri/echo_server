#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDebug>
#include "utils/asn1_parser.h"
#include "functions/user.h"
#include "functions/room.h"
#include "functions/file.h"
#include "functions/role.h"
#include "db/db.h"

QMap<QTcpSocket*, client_state_t> client_state_map;
QSqlDatabase db;
struct asn1_def def;
qint64 server_start_timestamp = 0;

int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);
    server_start_timestamp = QDateTime::currentSecsSinceEpoch();

    quint16 port = 5555;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            bool ok = false;
            uint val = QString::fromLocal8Bit(argv[++i]).toUInt(&ok);
            if (ok && val > 0 && val <= 65535) {
                port = (quint16)val;
            }
        } else if (arg.startsWith("-p") && arg.length() > 2) {
            bool ok = false;
            uint val = arg.mid(2).toUInt(&ok);
            if (ok && val > 0 && val <= 65535) {
                port = (quint16)val;
            }
        }
    }

    int ret;
    QString ans1_path = QCoreApplication::applicationDirPath()+"/ChatProtocol.asn";
    ret = asn1_client_init(def, ans1_path);
    if (ret != ASN1_SUCCESS) {
        printf("Failed to load ASN.1 definitions!\n");
        return 1;
    }

    if (!initialize_database()) {
        qDebug() << "CRITICAL: Database initialization failed. Shutting down.";
        return -1;
    }

    def.on(ASN1_EVENT::CLIENT_CHECK_ID, check_user_id);
    def.on(ASN1_EVENT::NEW_USER_PROOF, check_new_user_proof);
    def.on(ASN1_EVENT::CLAIM_ADMIN_PRIVILEGE, claim_admin_privilege);
    def.on(ASN1_EVENT::CREATE_ROOM, handle_create_room);
    def.on(ASN1_EVENT::JOIN_ROOM, handle_join_room);
    def.on(ASN1_EVENT::LEAVE_ROOM, handle_leave_room);
    def.on(ASN1_EVENT::CHAT, handle_chat_message);
    def.on(ASN1_EVENT::DELETE_MSG, handle_delete_msg);
    def.on(ASN1_EVENT::UPDATE_SERVER_SETTINGS, handle_update_server_settings);
    def.on(ASN1_EVENT::FILE_UPLOAD_CHUNK, handle_file_upload_chunk);
    def.on(ASN1_EVENT::FILE_DOWNLOAD_REQ, handle_file_download_req);
    def.on(ASN1_EVENT::HISTORY_REQ, handle_history_req);
    def.on(ASN1_EVENT::GET_MY_INFO, handle_get_my_info);
    def.on(ASN1_EVENT::AVATAR_UPLOAD_CHUNK, handle_avatar_upload_chunk);
    def.on(ASN1_EVENT::GET_ROLES_REQ, handle_get_roles);
    def.on(ASN1_EVENT::CREATE_ROLE, handle_create_role);
    def.on(ASN1_EVENT::UPDATE_ROLE_PERMS, handle_update_role_perms);
    def.on(ASN1_EVENT::DELETE_ROLE, handle_delete_role);
    def.on(ASN1_EVENT::GET_USER_ROLES_REQ, handle_get_user_roles);
    def.on(ASN1_EVENT::ASSIGN_USER_ROLES, handle_assign_user_roles);
    def.on(ASN1_EVENT::KICK_USER, handle_kick_user);
    def.on(ASN1_EVENT::UPDATE_ROOM, handle_update_room);
    def.on(ASN1_EVENT::POKE_USER, handle_poke_user);
    def.on(ASN1_EVENT::GET_MEMBERS_REQ, handle_get_members);
    def.on(ASN1_EVENT::GET_MEMBER_DETAIL_REQ, handle_get_member_detail);

    QTcpServer server;

    QObject::connect(&server, &QTcpServer::newConnection, [&server]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *client_socket = server.nextPendingConnection();
            if (!client_socket) return;
            client_state_map.insert(client_socket, client_state_t{});

            client_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

            QObject::connect(client_socket, &QTcpSocket::readyRead, [client_socket]() {
                asn1_server_read_response(client_socket, def);
            });

            QObject::connect(client_socket, &QTcpSocket::disconnected, [client_socket]() {
                // qDebug() << "client_socket disconnected.";
                if (client_state_map.contains(client_socket)) {
                    uint64_t uid = client_state_map[client_socket].id;
                    uint64_t v_rid = client_state_map[client_socket].current_voice_room_id;
                    client_state_map.remove(client_socket);
                    voice_room_remove_user(uid);
                    if (v_rid > 0) {
                        broadcast_room_list();
                    }
                } else {
                    client_state_map.remove(client_socket);
                }
                client_socket->deleteLater();
            });
        }
    });

    init_voice_server(port);

    if (server.listen(QHostAddress::Any, port)) {
        qDebug() << "Server listening on port" << port << "...";
    } else {
        qDebug() << "Server error starting up:" << server.errorString();
    }

    return QCoreApplication::exec();
}
