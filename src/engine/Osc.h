#pragma once
// OSC control and OSCQuery publication of the whole namespace (composition, layers and groups,
// sources, transport, ISF parameters, color, spatial, effects).
//
//  - OSC over UDP (messages and bundles, address patterns with * ? [] {}): writes values;
//  - OSCQuery (Vidvox proposal) over HTTP on another port: GET / returns the tree as JSON
//    (FULL_PATH, CONTENTS, TYPE, VALUE, RANGE, ACCESS, DESCRIPTION, CLIPMODE), GET /path?VALUE an attribute,
//    GET /?HOST_INFO the server description;
//  - WebSocket on the HTTP port: LISTEN / IGNORE commands, value updates sent as binary OSC messages,
//    OSC messages accepted as binary frames.
//
// Everything is hierarchical and lowercase (layer, group and ISF names aside); a parameter that switches something
// on or off is the child "enable" of what it switches (color/tint/enable, effect/<fx>/enable); the parameters of a shader are under param/.
// Layers are addressed by name, groups contain their members: /layer/<group>/layer/<layer>/opacity.
// Names are made OSC-safe (spaces and reserved characters become '_') and unique among siblings ("_2").
// A locked layer refuses every write except enable, locked and the transport (play, restart, position).
// Writes do not go through the undo stack (show control); edited() tells the interface to refresh.
// When layers, sources or effects change, WebSocket clients receive PATH_CHANGED and fetch the tree again.
// The server is announced by zeroconf (_oscjson._tcp for OSCQuery, _osc._udp for OSC).

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <functional>
#include <map>
#include <vector>

class Engine;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace osc {

struct Message {
    QString address;
    QString types;     // type tags without the leading ','
    QVariantList args; // int, float (double), QString, bool, QByteArray (blob), invalid (nil / impulse)
};

QByteArray encode(const Message &m);
// Decodes a message or a bundle (flattened, nested bundles included). Returns false if malformed.
bool decode(const QByteArray &packet, std::vector<Message> &out);
// OSC 1.0 address pattern matching (* ? [abc] [!a-z] {foo,bar}), segment by segment
bool match(const QString &pattern, const QString &address);
// Name usable as one segment of an OSC address
QString safeName(const QString &name);
// The segment of each effect of a layer (names made safe; a repeated name gets _2, _3…): the same in OSC addresses,
// timeline parameters and snapshot times
QStringList uniqueSegments(const QStringList &names);

} // namespace osc

// One node of the namespace (container or method)
struct OscNode {
    QString path;
    QString type;   // OSC type tags; empty for a container
    int access = 0; // 0 none, 1 read, 2 write, 3 read / write
    QString description;
    QJsonArray range;
    QString clip;   // "both" when the range is enforced
    QStringList children; // segment names, in order
    std::function<QVariantList()> get;
    std::function<bool(const QVariantList &)> set; // false: refused (locked layer, bad value)
};

// The namespace built from the engine state; rebuilt when the structure changes.
class OscNamespace
{
public:
    explicit OscNamespace(Engine *e) : m_e(e) {}

    void refresh();                         // rebuilds if the structure signature changed
    QString signature() const;              // layers, sources, effects and parameters (not values)
    const OscNode *node(const QString &path) const;
    QStringList paths() const;              // every method (node with a type)
    QJsonObject toJson(const QString &path) const; // node and its contents (OSCQuery)
    static QJsonValue jsonValue(const OscNode &n, const QVariant &v, int k);

private:
    void build();
    OscNode &add(const QString &path, const QString &type, int access, const QString &description);
    void addLayer(const QString &prefix, quint64 id);

    Engine *m_e;
    QString m_signature;
    std::map<QString, OscNode> m_nodes;
};

class OscServer : public QObject
{
    Q_OBJECT
public:
    explicit OscServer(Engine *engine, QObject *parent = nullptr);
    ~OscServer() override;

    // Ports 0: chosen by the system (tests). Returns false if a port could not be opened.
    // announce: zeroconf (_oscjson._tcp, _osc._udp)
    bool start(quint16 oscPort, quint16 queryPort, const QString &name = QStringLiteral("Fulskrin"), bool announce = true);
    void stop();
    bool isRunning() const;
    quint16 oscPort() const;
    quint16 queryPort() const;
    QString status() const { return m_status; }

    // Entry points, also used directly by the tests
    int handlePacket(const QByteArray &packet); // number of values written
    bool handleMessage(const osc::Message &m);
    // HTTP answer to a GET (status code and JSON body)
    QByteArray httpGet(const QString &target, int *status);

signals:
    void edited(); // values changed through OSC: the interface refreshes

private:
    struct Client {
        QTcpSocket *socket = nullptr;
        QByteArray buffer;
        bool websocket = false;
        QSet<QString> listening;
        QHash<QString, QVariantList> sent; // last value sent per listened path
    };
    void onUdp();
    void onConnection();
    void onData(QTcpSocket *s);
    void onWsFrame(Client &c, int opcode, const QByteArray &payload);
    void sendWs(QTcpSocket *s, int opcode, const QByteArray &payload);
    void pushListened();
    QJsonObject hostInfo() const;

    Engine *m_e;
    OscNamespace m_ns;
    QUdpSocket *m_udp = nullptr;
    QTcpServer *m_http = nullptr;
    QHash<QTcpSocket *, Client> m_clients;
    QTimer m_listenTimer;
    QString m_name, m_status;
    QString m_treeSignature; // tree last seen by the WebSocket clients (PATH_CHANGED when it changes)
    class Zeroconf *m_zeroconf = nullptr;
};
