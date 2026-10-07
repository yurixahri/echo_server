#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDebug>
#include "utils/asn1_parser.h"
#include "functions/user.h"
#include "db/db.h"

QMap<QTcpSocket*, client_state_t> client_state_map;
QSqlDatabase db;
struct asn1_def def;

int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);

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

    QTcpServer server;

    QObject::connect(&server, &QTcpServer::newConnection, [&server, &def]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *client_socket = server.nextPendingConnection();
            if (!client_socket) return;
            client_state_map.insert(client_socket, client_state_t{});

            client_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

            QObject::connect(client_socket, &QTcpSocket::readyRead, [client_socket, &def]() {
                asn1_server_read_response(client_socket, def);
            });

            QObject::connect(client_socket, &QTcpSocket::disconnected, [client_socket]() {
                qDebug() << "client_socket disconnected.";
                client_state_map.remove(client_socket);
                client_socket->deleteLater();
            });
        }
    });


    if (server.listen(QHostAddress::Any, 5555)) {
        qDebug() << "Server listening on port 5555...";
    } else {
        qDebug() << "Server error starting up:" << server.errorString();
    }

    return QCoreApplication::exec();
}
