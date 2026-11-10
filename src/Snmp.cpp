#include "Snmp.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QStringList>

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace ptouch {
namespace snmp {
namespace {

constexpr int TagNull = 0x05;
constexpr int TagOid = 0x06;
constexpr int TagSequence = 0x30;
constexpr int TagGetRequest = 0xA0;
constexpr int TagGetResponse = 0xA2;

QByteArray tlv(int tag, const QByteArray &content)
{
    QByteArray out;
    out.append(char(tag));
    const int n = content.size();
    if (n < 0x80) {
        out.append(char(n));
    } else if (n < 0x100) {
        out.append(char(0x81));
        out.append(char(n));
    } else {
        out.append(char(0x82));
        out.append(char(n >> 8));
        out.append(char(n & 0xFF));
    }
    return out + content;
}

QByteArray integer(quint32 value)
{
    // Always four bytes plus a leading zero when the top bit is set, so the
    // number can never be read as negative.
    QByteArray bytes;
    if (value & 0x80000000u)
        bytes.append('\0');
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.append(char((value >> shift) & 0xFF));
    return tlv(TagInteger, bytes);
}

QByteArray encodeOid(const QString &oid)
{
    const QStringList parts = oid.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return {};
    QList<quint32> arcs;
    for (const QString &part : parts) {
        bool ok = false;
        arcs << part.toUInt(&ok);
        if (!ok)
            return {};
    }
    if (arcs.at(0) > 2 || arcs.at(1) > 39)
        return {};

    QByteArray out;
    out.append(char(arcs.at(0) * 40 + arcs.at(1)));
    for (int i = 2; i < arcs.size(); ++i) {
        // Base 128, most significant group first, top bit set on all but the last.
        quint32 arc = arcs.at(i);
        QByteArray groups;
        groups.prepend(char(arc & 0x7F));
        while (arc >>= 7)
            groups.prepend(char(0x80 | (arc & 0x7F)));
        out += groups;
    }
    return out;
}

// Reads one tag-length-value triple at `pos` and moves past it.
bool readTlv(const QByteArray &data, int &pos, int &tag, QByteArray &content)
{
    if (pos + 2 > data.size())
        return false;
    tag = static_cast<unsigned char>(data.at(pos++));
    int length = static_cast<unsigned char>(data.at(pos++));
    if (length & 0x80) {
        const int count = length & 0x7F;
        if (count == 0 || count > 2 || pos + count > data.size())
            return false;
        length = 0;
        for (int i = 0; i < count; ++i)
            length = (length << 8) | static_cast<unsigned char>(data.at(pos++));
    }
    if (pos + length > data.size())
        return false;
    content = data.mid(pos, length);
    pos += length;
    return true;
}

bool readInteger(const QByteArray &data, int &pos, quint32 &value)
{
    int tag = 0;
    QByteArray content;
    if (!readTlv(data, pos, tag, content) || tag != TagInteger || content.size() > 5)
        return false;
    value = 0;
    for (const char c : content)
        value = (value << 8) | static_cast<unsigned char>(c);
    return true;
}

Reply failure(const QString &error)
{
    Reply r;
    r.error = error;
    return r;
}

} // namespace

QByteArray encodeGetRequest(const QString &oid, quint32 requestId, const QByteArray &community)
{
    const QByteArray encodedOid = encodeOid(oid);
    if (encodedOid.isEmpty())
        return {};

    const QByteArray binding = tlv(TagSequence, tlv(TagOid, encodedOid) + tlv(TagNull, {}));
    const QByteArray pdu = tlv(TagGetRequest,
                               integer(requestId)
                               + tlv(TagInteger, QByteArray(1, '\0'))   // error status
                               + tlv(TagInteger, QByteArray(1, '\0'))   // error index
                               + tlv(TagSequence, binding));
    return tlv(TagSequence,
               tlv(TagInteger, QByteArray(1, '\0'))                     // version 1
               + tlv(TagOctetString, community)
               + pdu);
}

Reply decodeResponse(const QByteArray &packet)
{
    const QString malformed = QStringLiteral("malformed SNMP reply");
    int pos = 0, tag = 0;
    QByteArray message, community, pdu, bindings, binding, oid;
    quint32 version = 0, errorStatus = 0, errorIndex = 0;
    Reply r;

    if (!readTlv(packet, pos, tag, message) || tag != TagSequence)
        return failure(malformed);
    pos = 0;
    if (!readInteger(message, pos, version)
        || !readTlv(message, pos, tag, community) || tag != TagOctetString
        || !readTlv(message, pos, tag, pdu) || tag != TagGetResponse)
        return failure(malformed);
    pos = 0;
    if (!readInteger(pdu, pos, r.requestId) || !readInteger(pdu, pos, errorStatus)
        || !readInteger(pdu, pos, errorIndex)
        || !readTlv(pdu, pos, tag, bindings) || tag != TagSequence)
        return failure(malformed);
    if (errorStatus != 0) {
        // 2 is noSuchName: the device answers SNMP but does not know this value —
        // which is what any printer other than a P-touch says to the status OID.
        r.error = errorStatus == 2 ? QStringLiteral("the device does not offer this value")
                                   : QStringLiteral("SNMP error %1").arg(errorStatus);
        return r;
    }
    pos = 0;
    if (!readTlv(bindings, pos, tag, binding) || tag != TagSequence)
        return failure(malformed);
    pos = 0;
    if (!readTlv(binding, pos, tag, oid) || tag != TagOid
        || !readTlv(binding, pos, r.tag, r.value))
        return failure(malformed);
    r.ok = true;
    return r;
}

Reply get(const QString &host, const QString &oid, int timeoutMs)
{
    QByteArray community = qgetenv("PTOUCH_SNMP_COMMUNITY");
    if (community.isEmpty())
        community = QByteArrayLiteral("public");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *addresses = nullptr;
    const int rc = ::getaddrinfo(host.toLocal8Bit().constData(), "161", &hints, &addresses);
    if (rc != 0 || !addresses)
        return failure(QStringLiteral("%1: %2").arg(host, QString::fromLocal8Bit(::gai_strerror(rc))));

    const int fd = ::socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    if (fd < 0 || ::connect(fd, addresses->ai_addr, addresses->ai_addrlen) != 0) {
        const QString error = QStringLiteral("%1: %2").arg(host,
                                  QString::fromLocal8Bit(std::strerror(errno)));
        if (fd >= 0)
            ::close(fd);
        ::freeaddrinfo(addresses);
        return failure(error);
    }
    ::freeaddrinfo(addresses);

    Reply reply = failure(QStringLiteral("%1 does not answer over SNMP").arg(host));
    // UDP loses packets without saying so; ask twice before giving up.
    for (int attempt = 0; attempt < 2 && !reply.ok; ++attempt) {
        const quint32 requestId = QRandomGenerator::global()->bounded(1u, 0x7FFFFFFFu);
        const QByteArray request = encodeGetRequest(oid, requestId, community);
        if (request.isEmpty()) {
            reply = failure(QStringLiteral("invalid OID %1").arg(oid));
            break;
        }
        if (::send(fd, request.constData(), size_t(request.size()), 0) < 0)
            continue;       // e.g. "connection refused" reported for the previous datagram

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, int(timeoutMs - timer.elapsed())) <= 0)
                break;
            char buffer[1500];
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            const Reply decoded = decodeResponse(QByteArray(buffer, int(n)));
            if (decoded.requestId != 0 && decoded.requestId != requestId)
                continue;   // a late answer to an earlier question
            reply = decoded;
            break;
        }
        if (!reply.ok && reply.requestId != 0)
            break;          // the device answered, just not with a value
    }
    ::close(fd);
    return reply;
}

} // namespace snmp
} // namespace ptouch
