#pragma once
// Zeroconf (DNS-SD over multicast DNS) announcement of services, so that clients find them without typing
// an address: OSCQuery (_oscjson._tcp) and OSC (_osc._udp).
//  - macOS: the system's Bonjour (dns_sd);
//  - elsewhere: a small mDNS responder (RFC 6762 / 6763) sharing UDP port 5353 with the system's one
//    (Avahi, Windows): answers the PTR / SRV / TXT / A questions about our services and announces them at start;
//    a goodbye (TTL 0) is sent at stop.

#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>

class QUdpSocket;

class Zeroconf : public QObject
{
    Q_OBJECT
public:
    struct Service {
        QString type; // "_oscjson._tcp", "_osc._udp"
        quint16 port = 0;
        QMap<QString, QString> txt;
    };

    explicit Zeroconf(QObject *parent = nullptr);
    ~Zeroconf() override;

    // instance: shown name, e.g. "Fulskrin (studio-mac)"
    bool start(const QString &instance, const QList<Service> &services, QString *err = nullptr);
    void stop();
    bool isRunning() const { return m_running; }
    QString instanceName() const { return m_instance; }

    // mDNS responder (not used on macOS), exposed for the tests
    QByteArray answerQuery(const QByteArray &query, bool *unicast = nullptr) const; // empty: not for us
    QByteArray announcement(bool goodbye) const;
    QString hostName() const { return m_host; }

private:
    void onDatagrams();
    void sendMulticast(const QByteArray &packet);

    bool m_running = false;
    QString m_instance, m_host;
    QList<Service> m_services;
    QList<QHostAddress> m_addresses;
    QUdpSocket *m_socket = nullptr;
    QTimer m_announce;
    int m_announced = 0;
#ifdef Q_OS_MACOS
    std::vector<void *> m_refs; // DNSServiceRef
#endif
};
