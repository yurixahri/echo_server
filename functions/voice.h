#ifndef SERVER_VOICE_H
#define SERVER_VOICE_H

#include <QUdpSocket>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>
#include <QCoreApplication>
#include <QDebug>
#include "common/types.h"

struct voice_peer_t {
    QHostAddress address;
    quint16 port = 0;
    uint64_t user_id = 0;
    uint64_t room_id = 0;
    qint64 last_active = 0;
    bool is_mute = false;
    bool is_deaf = false;
};

inline QUdpSocket* voice_udp_server = nullptr;
inline QMap<uint64_t, voice_peer_t> voice_peers;
inline QMap<uint64_t, QSet<uint64_t>> voice_room;
inline QMap<uint64_t, uint64_t> voice_authorized_users;
inline QMap<uint64_t, bool> voice_user_can_speak;
inline QMutex voice_auth_mutex;
constexpr int VOICE_MAX_AUDIO_PAYLOAD = 1200;

void broadcast_room_list();

inline void voice_trigger_room_list_broadcast() {
    QCoreApplication *app = QCoreApplication::instance();
    if (app) {
        QMetaObject::invokeMethod(app, []() {
            broadcast_room_list();
        }, Qt::QueuedConnection);
    }
}

inline void voice_room_add_user(uint64_t room_id, uint64_t user_id, bool can_speak = true) {
    if (room_id == 0 || user_id == 0) return;
    QMutexLocker locker(&voice_auth_mutex);

    for (auto it = voice_room.begin(); it != voice_room.end(); ) {
        if (it.key() != room_id) {
            it.value().remove(user_id);
            if (it.value().isEmpty()) {
                it = voice_room.erase(it);
                continue;
            }
        }
        ++it;
    }
    voice_room[room_id].insert(user_id);
    voice_authorized_users[user_id] = room_id;
    voice_user_can_speak[user_id] = can_speak;
}

inline void voice_room_remove_user(uint64_t room_id, uint64_t user_id) {
    QMutexLocker locker(&voice_auth_mutex);
    if (voice_room.contains(room_id)) {
        voice_room[room_id].remove(user_id);
        if (voice_room[room_id].isEmpty()) voice_room.remove(room_id);
    }
    if (voice_peers.contains(user_id) && voice_peers[user_id].room_id == room_id) {
        voice_peers.remove(user_id);
    }
    if (voice_authorized_users.value(user_id) == room_id) {
        voice_authorized_users.remove(user_id);
    }
    voice_user_can_speak.remove(user_id);
}

inline void voice_room_remove_user(uint64_t user_id) {
    QMutexLocker locker(&voice_auth_mutex);
    for (auto it = voice_room.begin(); it != voice_room.end(); ) {
        it.value().remove(user_id);
        if (it.value().isEmpty()) {
            it = voice_room.erase(it);
        } else {
            ++it;
        }
    }
    voice_peers.remove(user_id);
    voice_authorized_users.remove(user_id);
    voice_user_can_speak.remove(user_id);
}

inline void handle_voice_datagrams() {
    if (!voice_udp_server) return;

    while (voice_udp_server->hasPendingDatagrams()) {
        QNetworkDatagram datagram = voice_udp_server->receiveDatagram();
        QByteArray data = datagram.data();
        if (data.size() < (int)sizeof(voice_packet_header_t)) continue;
        if (data.size() > (int)sizeof(voice_packet_header_t) + VOICE_MAX_AUDIO_PAYLOAD) continue;

        const voice_packet_header_t *hdr = reinterpret_cast<const voice_packet_header_t*>(data.constData());
        if (hdr->magic != 0x4543484F) continue;

        uint64_t uid = hdr->user_id;
        uint64_t rid = hdr->room_id;
        int payload_size = data.size() - sizeof(voice_packet_header_t);
        if (hdr->payload_size != payload_size) continue;
        if (hdr->packet_type == VOICE_DATA &&
            (payload_size == 0 || payload_size > VOICE_MAX_AUDIO_PAYLOAD)) continue;
        if (hdr->packet_type != VOICE_PING && hdr->packet_type != VOICE_STATE &&
            hdr->packet_type != VOICE_LEAVE && hdr->packet_type != VOICE_DATA) continue;

        QMutexLocker locker(&voice_auth_mutex);
        if (voice_authorized_users.value(uid) != rid ||
            !voice_room.contains(rid) || !voice_room[rid].contains(uid)) {
            continue;
        }

        // Update peer record
        bool is_new_peer = !voice_peers.contains(uid);
        voice_peer_t &peer = voice_peers[uid];
        peer.address = datagram.senderAddress();
        peer.port = datagram.senderPort();
        peer.user_id = uid;
        peer.room_id = rid;
        peer.last_active = QDateTime::currentSecsSinceEpoch();
        bool prev_mute = peer.is_mute;
        bool prev_deaf = peer.is_deaf;
        peer.is_mute = (hdr->is_mute != 0);
        peer.is_deaf = (hdr->is_deaf != 0);

        if (is_new_peer || hdr->packet_type == VOICE_STATE) {
            if (is_new_peer || prev_mute != peer.is_mute || prev_deaf != peer.is_deaf) {
                voice_trigger_room_list_broadcast();
            }
            if (hdr->packet_type == VOICE_STATE) continue;
        }

        if (hdr->packet_type == VOICE_LEAVE) {
            if (voice_room.contains(rid)) {
                voice_room[rid].remove(uid);
                if (voice_room[rid].isEmpty()) voice_room.remove(rid);
            }
            voice_peers.remove(uid);
            voice_authorized_users.remove(uid);
            voice_trigger_room_list_broadcast();
            continue;
        }

        if (hdr->packet_type == VOICE_DATA) {
            if (peer.is_mute || !voice_user_can_speak.value(uid, true)) continue;

            qint64 now_sec = QDateTime::currentSecsSinceEpoch();
            const QSet<uint64_t> room_users = voice_room.value(rid);
            for (uint64_t dest_uid : room_users) {
                if (!voice_peers.contains(dest_uid)) continue;
                const voice_peer_t &dest = voice_peers[dest_uid];
                if (dest.user_id != uid && dest.room_id == rid && !dest.is_deaf && dest.port > 0 &&
                    (now_sec - dest.last_active <= 15)) {
                    voice_udp_server->writeDatagram(data, dest.address, dest.port);
                }
            }
        }
    }
}

struct voice_server_worker_t {
    QThread thread;
    QObject *context = nullptr;

    voice_server_worker_t() {
        context = new QObject();
        context->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, context, &QObject::deleteLater);
        thread.start(QThread::HighPriority);
    }

    ~voice_server_worker_t() {
        if (thread.isRunning()) {
            QMetaObject::invokeMethod(context, []() {
                if (voice_udp_server) {
                    voice_udp_server->close();
                    delete voice_udp_server;
                    voice_udp_server = nullptr;
                }
            }, Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        }
    }
};

inline voice_server_worker_t& voice_server_worker() {
    static voice_server_worker_t worker;
    return worker;
}

inline bool init_voice_server(quint16 port = 5555) {
    voice_server_worker_t &worker = voice_server_worker();
    bool bound = false;

    QMetaObject::invokeMethod(worker.context, [port, &bound]() {
        if (voice_udp_server) {
            voice_udp_server->close();
            delete voice_udp_server;
            voice_udp_server = nullptr;
        }

        voice_udp_server = new QUdpSocket();
        if (!voice_udp_server->bind(QHostAddress::Any, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            qDebug() << "Failed to bind Voice UDP server on port" << port << ":" << voice_udp_server->errorString();
            delete voice_udp_server;
            voice_udp_server = nullptr;
            bound = false;
            return;
        }

        QObject::connect(voice_udp_server, &QUdpSocket::readyRead, handle_voice_datagrams);
        qDebug() << "Voice UDP server listening on dedicated thread at port" << port << "...";
        bound = true;
    }, Qt::BlockingQueuedConnection);

    return bound;
}

#endif // SERVER_VOICE_H
