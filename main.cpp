#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDebug>
#include "utils/asn1_parser.h"
#include "db/db.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    struct asn1_def def;
    int ret;

    ret = asn1_client_init(def, "D:/FILE/qt/echo_test_client/ChatProtocol.asn");
    if (ret != ASN1_SUCCESS) {
        printf("Failed to load ASN.1 definitions!\n");
        return 1;
    }

    if (!initialize_database()) {
        qDebug() << "CRITICAL: Database initialization failed. Shutting down.";
        return -1;
    }

    QTcpServer server;

    QObject::connect(&server, &QTcpServer::newConnection, [&server, &def]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *client_socket = server.nextPendingConnection();

            // Performance optimization for fast messaging
            client_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            qDebug() << "New incoming plain TCP connection from Nginx / Client:" << client_socket->peerAddress().toString();

            // Direct mapping of your exact read function to the socket event
            QObject::connect(client_socket, &QTcpSocket::readyRead, [client_socket, &def]() {
                asn1_server_read_response(client_socket, def);

                int ret;
                asn1_node element = NULL;
                ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
                asn1_write_value(element, "", "response", 1);
                asn1_write_unsigned64(element, "response.status", 200);
                asn1_server_send_packet(client_socket, element);
            });

            // Cleanup when the connection ends
            QObject::connect(client_socket, &QTcpSocket::disconnected, [client_socket]() {
                qDebug() << "Client disconnected.";
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
