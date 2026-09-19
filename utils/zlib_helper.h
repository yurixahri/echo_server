#ifndef ZLIB_HELPER_H
#define ZLIB_HELPER_H

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <zlib.h>

QByteArray zcompress(const QByteArray &data) {
    uLongf compressedLen = compressBound(data.size());
    QByteArray compressed(compressedLen, 0);

    // 2. Perform raw zlib compression
    int res = compress((Bytef*)compressed.data(), &compressedLen,
                       (const Bytef*)data.data(), data.size());

    if (res != Z_OK) return QByteArray();
    compressed.resize(compressedLen);

    // 3. Build the Packet with Header
    QByteArray finalPacket;
    QDataStream out(&finalPacket, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);

    // Header: The Original Size (4 bytes)
    out << (quint32)data.size();

    // Payload: The Compressed Data
    finalPacket.append(compressed);

    return finalPacket;
}



QByteArray zdecompress(const QByteArray &packet) {
    if (packet.size() < 4) return QByteArray();

    // 1. Extract the Original Size from the first 4 bytes
    QDataStream in(packet);
    in.setByteOrder(QDataStream::BigEndian);
    quint32 originalSize;
    in >> originalSize;

    // 2. Extract the remaining compressed payload
    QByteArray compressedData = packet.mid(4);

    // 3. Allocate EXACTLY the right amount of RAM
    QByteArray decompressed(originalSize, 0);
    uLongf destLen = originalSize;

    // 4. Raw Zlib Uncompress
    int res = uncompress((Bytef*)decompressed.data(), &destLen,
                         (const Bytef*)compressedData.data(), compressedData.size());

    if (res == Z_OK) {
        return decompressed;
    }
    return QByteArray();
}

#endif // ZLIB_HELPER_H
