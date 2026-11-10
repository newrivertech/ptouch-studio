// A single SNMPv1 GET — all a networked P-touch needs to be asked for its status.
//
// Network models (PT-P950NW, PT-P900W, PT-P750W …) take print data on TCP 9100
// but do not answer `ESC i S` there. The same 32 status bytes are published over
// SNMP instead, so one request and one reply over UDP replace the serial dialogue.
// That is too little to justify a dependency on net-snmp: the request is built and
// the reply taken apart by hand.
#pragma once

#include <QByteArray>
#include <QString>

namespace ptouch {
namespace snmp {

// ASN.1 tags that turn up in a reply.
constexpr int TagInteger = 0x02;
constexpr int TagOctetString = 0x04;

struct Reply {
    bool ok = false;
    quint32 requestId = 0;
    int tag = 0;          // type of the returned value
    QByteArray value;     // its raw content
    QString error;
};

// The datagram for "GET oid". Empty when the OID cannot be encoded.
QByteArray encodeGetRequest(const QString &oid, quint32 requestId,
                            const QByteArray &community = QByteArrayLiteral("public"));

// Takes a GetResponse apart and returns the value of its first variable.
Reply decodeResponse(const QByteArray &packet);

// Sends the request and waits for the matching reply. `host` is a name or an
// address; the community comes from PTOUCH_SNMP_COMMUNITY, default "public".
Reply get(const QString &host, const QString &oid, int timeoutMs = 1500);

} // namespace snmp
} // namespace ptouch
