#ifndef ASN1_PARSER_H
#define ASN1_PARSER_H

#include <QString>
#include <QTcpSocket>
#include <QTcpServer>
#include <QtEndian>
#include <QMap>
#include <libtasn1.h>
#include <stdint.h>
#include "common/types.h"

struct asn1_def {
    asn1_node definitions;
    QMap<uint16_t, std::function<void(QTcpSocket*, asn1_node)>> dispatch_table;

    void on(uint16_t tag, std::function<void(QTcpSocket*, asn1_node)> f){
        dispatch_table[tag] = f;
    }
};

extern struct asn1_def def;

QString asn1_read_string(asn1_node root, const char *name) {
    int len = 0;
    int ret = asn1_read_value(root, name, NULL, &len);

    if (ret != ASN1_MEM_ERROR && ret != ASN1_SUCCESS) return QString();
    if (len <= 0) return QString("");

    QByteArray buffer(len, 0);
    ret = asn1_read_value(root, name, buffer.data(), &len);

    if (ret != ASN1_SUCCESS) return QString();

    return QString::fromUtf8(buffer);
}

QByteArray asn1_read_bytes(asn1_node root, const char *name){
    int len = 0;
    int ret = asn1_read_value(root, name, nullptr, &len);

    if (ret != ASN1_MEM_ERROR && ret != ASN1_SUCCESS) return {};
    if (len <= 0) return {};

    QByteArray buffer(len, Qt::Uninitialized);
    ret = asn1_read_value(root, name, buffer.data(), &len);

    if (ret != ASN1_SUCCESS) return {};

    buffer.resize(len);
    return buffer;
}

uint64_t asn1_read_int(asn1_node root, const char *name) {
    int len = 0;
    int ret;
    ret = asn1_read_value(root, name, NULL, &len);
    if (ret != ASN1_MEM_ERROR && ret != ASN1_SUCCESS) return 0;
    if (len <= 0) return 0;

    QByteArray buffer(len, 0);
    unsigned char *ptr = (unsigned char*)buffer.data();
    ret = asn1_read_value(root, name, ptr, &len);
    if (ret != ASN1_SUCCESS) return 0;

    uint64_t val = 0;

    // Step 2: Handle Sign Extension
    // Only extend if the first bit is 1 AND it's not a 9-byte unsigned value
    if (len < 9 && (ptr[0] & 0x80)) {
        val = (uint64_t)-1; // Fill with 1s for negative numbers
    } else {
        val = 0; // Keep as 0 for positive numbers (including 9-byte ones)
    }

    // Step 3: Reconstruct
    for (int i = 0; i < len; i++) {
        val = (val << 8) | (ptr[i] & 0xFF);
    }

    return val;
}

void asn1_write_unsigned64(asn1_node element, const char* name, uint64_t value) {
    uint64_t be_val = qToBigEndian(value);
    unsigned char *bytes = (unsigned char *)&be_val;

    // If the highest bit of the data is 1, we MUST prepend 0x00
    // to keep it positive in ASN.1
    if (bytes[0] & 0x80) {
        unsigned char buffer[9];
        buffer[0] = 0x00;
        memcpy(&buffer[1], &be_val, 8);
        asn1_write_value(element, name, buffer, 9);
    } else {
        asn1_write_value(element, name, &be_val, 8);
    }
}

void asn1_write_signed64(asn1_node element, const char* name, int64_t value) {
    int64_t be_val = qToBigEndian((uint64_t)value);
    asn1_write_value(element, name, &be_val, 8);
}

void asn1_write_string(asn1_node element, const char* name, QString value) {
    QByteArray utf8 = value.toUtf8();
    asn1_write_value(element, name, utf8.data(), utf8.size());
}

void asn1_write_byte_array(asn1_node element, const char* name, QByteArray value) {
    asn1_write_value(element, name, value.data(), value.size());
}

inline void asn1_write_bool(asn1_node element, const char* name, bool val) {
    asn1_write_value(element, name, val ? "TRUE" : "FALSE", 1);
}

int asn1_client_init(asn1_def &def, QString &asn_file) {
    char errorDesc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    def.definitions = NULL;
    int ret = asn1_parser2tree(asn_file.toUtf8(), &def.definitions, errorDesc);
    // if (ret != ASN1_SUCCESS) printf("%s\n", errorDesc);
    return ret;
}

void asn1_server_send_packet(QTcpSocket *client, asn1_node &element) {
    int ret;
    // DER Encoding
    char errorDesc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    int der_len = 0;

    ret = asn1_der_coding(element, "", NULL, &der_len, errorDesc);

    if (ret != ASN1_MEM_ERROR) {
        // qDebug() << "Failed getting DER size:" << errorDesc;
        return;
    }

    QByteArray asn1_data(der_len, Qt::Uninitialized);
    ret = asn1_der_coding(element, "", asn1_data.data(), &der_len, errorDesc);
    if (ret != ASN1_SUCCESS) {
        // qDebug() << "DER encoding failed:" << errorDesc;
        return;
    }

    asn1_data.resize(der_len);
    QByteArray packet;
    quint16 len = (quint16)asn1_data.size();
    char header[2];
    header[0] = (len >> 8) & 0xFF;
    header[1] = len & 0xFF;

    packet.append(header, 2);      // Add the 2-byte header
    packet.append(asn1_data);     // Add the raw uncompressed ASN.1 data

    client->write(packet);
    client->flush();
}

void asn1_server_read_response(QTcpSocket *client, asn1_def &def) {
    // Switch to a while loop to catch multiple back-to-back packets
    while (client->bytesAvailable() >= 2) {
        char header[2];
        client->peek(header, 2);

        quint16 len = (unsigned char)header[0] << 8 | (unsigned char)header[1];

        // If the complete payload hasn't fully arrived yet, stop and wait for the next signal
        if (client->bytesAvailable() < (len + 2)) return;

        client->read(2); // Skip header
        QByteArray data = client->read(len);

        asn1_node element = NULL;
        char errorDesc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];

        asn1_create_element(def.definitions, "ChatProtocol.ProtocolMessage", &element);

        int ret = asn1_der_decoding(&element, data.constData(), data.size(), errorDesc);
        if (ret != ASN1_SUCCESS) {
            // qDebug() << "Decoding Error:" << errorDesc;
            asn1_delete_structure(&element);
            continue;
        }

        unsigned long tag_value;
        unsigned char class_value;
        int len2;

        ret = asn1_get_tag_der((const unsigned char*)data.constData(),
                               data.size(), &class_value, &len2, &tag_value);

        // qDebug() << "Received";
        if (ret == ASN1_SUCCESS) {
            auto it = def.dispatch_table.find(tag_value);
            if (it != def.dispatch_table.end()){
                it.value()(client, element); // Fire the handler lambda!
            } else {
                // qDebug() << "No handler for tag:" << tag_value;
            }
        } else {
            // qDebug() << "Decoding Error:" << errorDesc;
        }

        asn1_delete_structure(&element);
    }
}

#endif // ASN1_PARSER_H
