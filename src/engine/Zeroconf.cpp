#include "Zeroconf.h"

#include <QHostInfo>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QUdpSocket>
#include <QtEndian>
#include <vector>

#ifdef Q_OS_MACOS
#include <arpa/inet.h>
#include <dns_sd.h>
#endif

static const QHostAddress kGroup(QStringLiteral("224.0.0.251"));
static constexpr quint16 kPort = 5353;
enum { TypeA = 1, TypePtr = 12, TypeTxt = 16, TypeSrv = 33, TypeAny = 255 };
static constexpr quint32 kHostTtl = 120, kServiceTtl = 4500;

Zeroconf::Zeroconf(QObject *parent) : QObject(parent), m_announce(this)
{
    // Announced twice, one second apart (RFC 6762 §8.3)
    m_announce.setInterval(1000);
    connect(&m_announce, &QTimer::timeout, this, [this] {
        sendMulticast(announcement(false));
        if (++m_announced >= 2) m_announce.stop();
    });
}

Zeroconf::~Zeroconf() { stop(); }

bool Zeroconf::start(const QString &instance, const QList<Service> &services, QString *err)
{
    stop();
    m_instance = instance.left(63);
    m_services = services;
#ifdef Q_OS_MACOS
    for (const Service &s : services) {
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        for (auto it = s.txt.begin(); it != s.txt.end(); ++it) {
            const QByteArray v = it.value().toUtf8();
            TXTRecordSetValue(&txt, it.key().toUtf8().constData(), uint8_t(v.size()), v.constData());
        }
        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType e =
            DNSServiceRegister(&ref, 0, 0, m_instance.toUtf8().constData(), s.type.toUtf8().constData(), nullptr, nullptr,
                               htons(s.port), TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr);
        TXTRecordDeallocate(&txt);
        if (e != kDNSServiceErr_NoError) {
            if (err) *err = QStringLiteral("Bonjour registration failed (%1)").arg(e);
            stop();
            return false;
        }
        m_refs.push_back(ref);
    }
    m_running = true;
    return true;
#else
    // Own host name, so as not to compete with the system's
    QString host = QHostInfo::localHostName().section('.', 0, 0);
    host.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]")), QStringLiteral("-"));
    m_host = QStringLiteral("fulskrin-%1").arg(host.isEmpty() ? QStringLiteral("host") : host).left(63);
    m_addresses.clear();
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) m_addresses << a;
    if (m_addresses.isEmpty()) m_addresses << QHostAddress(QHostAddress::LocalHost);

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, kPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (err) *err = QStringLiteral("mDNS port 5353 unavailable (%1)").arg(m_socket->errorString());
        stop();
        return false;
    }
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    m_socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1); // clients on this machine too
    bool joined = false;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces())
        if ((i.flags() & QNetworkInterface::IsUp) && (i.flags() & QNetworkInterface::CanMulticast))
            joined |= m_socket->joinMulticastGroup(kGroup, i);
    if (!joined) joined = m_socket->joinMulticastGroup(kGroup);
    if (!joined) {
        if (err) *err = QStringLiteral("mDNS: could not join the multicast group");
        stop();
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &Zeroconf::onDatagrams);
    m_running = true;
    m_announced = 0;
    sendMulticast(announcement(false));
    m_announce.start();
    return true;
#endif
}

void Zeroconf::stop()
{
#ifdef Q_OS_MACOS
    for (void *r : m_refs) DNSServiceRefDeallocate(DNSServiceRef(r));
    m_refs.clear();
#else
    m_announce.stop();
    if (m_socket && m_running) sendMulticast(announcement(true)); // goodbye
    delete m_socket;
    m_socket = nullptr;
#endif
    m_running = false;
}

// ---------------------------------------------------------------------------
// DNS messages
// ---------------------------------------------------------------------------

namespace {

void putLabels(QByteArray &b, const QStringList &labels)
{
    for (const QString &label : labels) {
        const QByteArray l = label.toUtf8().left(63);
        b.append(char(l.size()));
        b.append(l);
    }
    b.append('\0');
}

template <typename T> void putBE(QByteArray &b, T v)
{
    char raw[sizeof(T)];
    qToBigEndian(v, raw);
    b.append(raw, int(sizeof(T)));
}

struct Record {
    QByteArray name; // encoded
    quint16 type;
    bool flush;      // cache-flush bit (unique records)
    quint32 ttl;
    QByteArray rdata;
};

QByteArray encode(const QStringList &labels)
{
    QByteArray b;
    putLabels(b, labels);
    return b;
}

QByteArray message(quint16 id, const QList<Record> &answers, const QList<Record> &additional)
{
    QByteArray b;
    putBE<quint16>(b, id);
    putBE<quint16>(b, 0x8400); // response, authoritative
    putBE<quint16>(b, 0);
    putBE<quint16>(b, quint16(answers.size()));
    putBE<quint16>(b, 0);
    putBE<quint16>(b, quint16(additional.size()));
    for (const QList<Record> *list : {&answers, &additional})
        for (const Record &r : *list) {
            b.append(r.name);
            putBE<quint16>(b, r.type);
            putBE<quint16>(b, quint16(0x0001 | (r.flush ? 0x8000 : 0)));
            putBE<quint32>(b, r.ttl);
            putBE<quint16>(b, quint16(r.rdata.size()));
            b.append(r.rdata);
        }
    return b;
}

// Name at `pos` (compression pointers followed), lowercase, dot-separated; pos moves past it
QString readName(const QByteArray &b, int &pos, bool *ok)
{
    QStringList labels;
    int p = pos, jumps = 0;
    bool jumped = false;
    while (true) {
        if (p >= b.size()) {
            *ok = false;
            return {};
        }
        const quint8 len = quint8(b[p]);
        if (len == 0) {
            ++p;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= b.size() || ++jumps > 16) {
                *ok = false;
                return {};
            }
            const int target = ((len & 0x3F) << 8) | quint8(b[p + 1]);
            if (!jumped) pos = p + 2;
            jumped = true;
            p = target;
            continue;
        }
        if (p + 1 + len > b.size()) {
            *ok = false;
            return {};
        }
        labels << QString::fromUtf8(b.constData() + p + 1, len).toLower();
        p += 1 + len;
    }
    if (!jumped) pos = p;
    return labels.join('.');
}

} // namespace

QByteArray Zeroconf::announcement(bool goodbye) const
{
    QList<Record> answers;
    const QByteArray host = encode({m_host, "local"});
    for (const Service &s : m_services) {
        const QStringList typeLabels = (s.type + ".local").split('.', Qt::SkipEmptyParts);
        const QByteArray type = encode(typeLabels);
        const QByteArray inst = encode(QStringList{m_instance} + typeLabels);
        answers << Record{type, TypePtr, false, goodbye ? 0 : kServiceTtl, inst};
        QByteArray srv;
        putBE<quint16>(srv, 0);
        putBE<quint16>(srv, 0);
        putBE<quint16>(srv, s.port);
        srv.append(host);
        answers << Record{inst, TypeSrv, true, goodbye ? 0 : kHostTtl, srv};
        QByteArray txt;
        for (auto it = s.txt.begin(); it != s.txt.end(); ++it) {
            const QByteArray kv = (it.key() + '=' + it.value()).toUtf8().left(255);
            txt.append(char(kv.size()));
            txt.append(kv);
        }
        if (txt.isEmpty()) txt.append('\0');
        answers << Record{inst, TypeTxt, true, goodbye ? 0 : kServiceTtl, txt};
        answers << Record{encode({"_services", "_dns-sd", "_udp", "local"}), TypePtr, false, goodbye ? 0 : kServiceTtl, type};
    }
    for (const QHostAddress &a : m_addresses) {
        QByteArray ip;
        putBE<quint32>(ip, a.toIPv4Address());
        answers << Record{host, TypeA, true, goodbye ? 0 : kHostTtl, ip};
    }
    return message(0, answers, {});
}

QByteArray Zeroconf::answerQuery(const QByteArray &q, bool *unicast) const
{
    if (q.size() < 12) return {};
    const quint16 id = qFromBigEndian<quint16>(q.constData());
    const quint16 flags = qFromBigEndian<quint16>(q.constData() + 2);
    if (flags & 0x8000) return {}; // a response, not a question
    const int count = qFromBigEndian<quint16>(q.constData() + 4);
    int pos = 12;
    const QString hostName = (m_host + ".local").toLower();
    QList<Record> answers, additional;
    bool wantsUnicast = false;
    auto hostRecords = [&](QList<Record> &out) {
        for (const QHostAddress &a : m_addresses) {
            QByteArray ip;
            putBE<quint32>(ip, a.toIPv4Address());
            out << Record{encode({m_host, "local"}), TypeA, true, kHostTtl, ip};
        }
    };
    for (int k = 0; k < count; ++k) {
        bool ok = true;
        const QString name = readName(q, pos, &ok);
        if (!ok || pos + 4 > q.size()) return {};
        const quint16 type = qFromBigEndian<quint16>(q.constData() + pos);
        const quint16 cls = qFromBigEndian<quint16>(q.constData() + pos + 2);
        pos += 4;
        wantsUnicast |= cls & 0x8000;
        const bool any = type == TypeAny;
        if (name == "_services._dns-sd._udp.local" && (type == TypePtr || any)) {
            for (const Service &s : m_services)
                answers << Record{encode({"_services", "_dns-sd", "_udp", "local"}), TypePtr, false, kServiceTtl,
                                  encode((s.type + ".local").split('.', Qt::SkipEmptyParts))};
        }
        if (name == hostName && (type == TypeA || any)) hostRecords(answers);
        for (const Service &s : m_services) {
            const QStringList typeLabels = (s.type + ".local").split('.', Qt::SkipEmptyParts);
            const QString typeName = (s.type + ".local").toLower();
            const QString instName = (m_instance + '.' + s.type + ".local").toLower();
            const QByteArray inst = encode(QStringList{m_instance} + typeLabels);
            QByteArray srv;
            putBE<quint16>(srv, 0);
            putBE<quint16>(srv, 0);
            putBE<quint16>(srv, s.port);
            srv.append(encode({m_host, "local"}));
            QByteArray txt;
            for (auto it = s.txt.begin(); it != s.txt.end(); ++it) {
                const QByteArray kv = (it.key() + '=' + it.value()).toUtf8().left(255);
                txt.append(char(kv.size()));
                txt.append(kv);
            }
            if (txt.isEmpty()) txt.append('\0');
            const Record srvRec{inst, TypeSrv, true, kHostTtl, srv}, txtRec{inst, TypeTxt, true, kServiceTtl, txt};
            if (name == typeName && (type == TypePtr || any)) {
                answers << Record{encode(typeLabels), TypePtr, false, kServiceTtl, inst};
                additional << srvRec << txtRec;
                hostRecords(additional);
            }
            if (name == instName) {
                if (type == TypeSrv || any) answers << srvRec;
                if (type == TypeTxt || any) answers << txtRec;
                if (type == TypeSrv || any) hostRecords(additional);
            }
        }
    }
    if (answers.isEmpty()) return {};
    if (unicast) *unicast = wantsUnicast;
    return message(id, answers, additional);
}

void Zeroconf::onDatagrams()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_socket->receiveDatagram();
        bool unicast = false;
        const QByteArray reply = answerQuery(d.data(), &unicast);
        if (reply.isEmpty()) continue;
        // Legacy (one-shot) queries come from another port: answered directly; others on the group
        if (d.senderPort() != kPort || unicast) m_socket->writeDatagram(reply, d.senderAddress(), quint16(d.senderPort()));
        if (d.senderPort() == kPort) sendMulticast(reply);
    }
}

void Zeroconf::sendMulticast(const QByteArray &packet)
{
    if (!m_socket) return;
    bool sent = false;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || !(i.flags() & QNetworkInterface::CanMulticast)) continue;
        m_socket->setMulticastInterface(i);
        sent |= m_socket->writeDatagram(packet, kGroup, kPort) > 0;
    }
    if (!sent) m_socket->writeDatagram(packet, kGroup, kPort);
}
