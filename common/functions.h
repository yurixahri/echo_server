#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "utils/asn1_parser.h"


inline void send_tcp_response_event(QTcpSocket* client_socket, uint16_t event, uint16_t status, QString message = ""){
    asn1_node element;
    int ret = asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);
    if (ret != ASN1_SUCCESS) {
        // printf("Failed to create ASN.1 element!\n");
        return;
    }
    asn1_write_value(element, "", "response", 1);
    asn1_write_unsigned64(element, "response.event", event);
    asn1_write_unsigned64(element, "response.status", status);
    if (message.isEmpty()) {
        asn1_write_value(element, "response.message", NULL, 0);
    } else {
        QByteArray message_utf8 = message.toUtf8();
        asn1_write_value(element, "response.message", message_utf8.data(), message_utf8.size());
    }
    asn1_server_send_packet(client_socket, element);
}



#endif // FUNCTIONS_H
