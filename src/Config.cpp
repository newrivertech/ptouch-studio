#include "Config.h"
#include "Provision.h"
#include "Status.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

namespace ptouch {
namespace {

QString runCommand(const QString &program, const QStringList &args, int timeoutMs = 10000)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs))
        p.kill();
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

} // namespace

QString Config::configFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/config.json");
}

QString systemConfValue(const QString &key)
{
    QFile f(QString::fromLatin1(SystemConfPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#')) || !line.contains(QLatin1Char('=')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (line.left(eq).trimmed() == key)
            return line.mid(eq + 1).trimmed().remove(QLatin1Char('"'));
    }
    return {};
}

QString findPtouchQueue()
{
    const QString out = runCommand(QStringLiteral("lpstat"), {QStringLiteral("-v")});
    // "device for NAME: uri", localised installations use their own wording
    static const QRegularExpression re(QStringLiteral("(?:für|for)\\s+(\\S+?):"));
    QStringList queues;
    auto it = re.globalMatch(out);
    while (it.hasNext())
        queues << it.next().captured(1);

    for (const QString &q : std::as_const(queues)) {
        const QString opts = runCommand(QStringLiteral("lpoptions"),
                                        {QStringLiteral("-p"), q, QStringLiteral("-l")});
        if (opts.contains(QStringLiteral("AutoCut")))   // marks the ptouch driver
            return q;
    }
    return queues.value(0);
}

QString hostFromDeviceUri(const QString &uri)
{
    const QUrl url(uri.trimmed());
    static const QStringList schemes = {QStringLiteral("socket"), QStringLiteral("lpd"),
                                        QStringLiteral("ipp"), QStringLiteral("ipps"),
                                        QStringLiteral("http"), QStringLiteral("https")};
    return schemes.contains(url.scheme().toLower()) ? url.host() : QString();
}

QString queueHost(const QString &queue)
{
    if (queue.isEmpty())
        return {};
    // "device for NAME: socket://192.168.1.50:9100" — the URI is the last word.
    const QString out = runCommand(QStringLiteral("lpstat"), {QStringLiteral("-v"), queue});
    return hostFromDeviceUri(out.section(QLatin1Char(' '), -1));
}

QString modelFromQueueOptions(const QString &options)
{
    // lpoptions prints "… printer-make-and-model='Brother PT-P950NW Foomatic/ptouch-pt' …"
    static const QRegularExpression re(QStringLiteral("printer-make-and-model='([^']*)'"));
    const QString make = re.match(options).captured(1);
    if (!make.contains(QStringLiteral("Brother"), Qt::CaseInsensitive))
        return {};
    return modelFromName(make);
}

QString queueModel(const QString &queue)
{
    if (queue.isEmpty())
        return {};
    return modelFromQueueOptions(runCommand(QStringLiteral("lpoptions"),
                                            {QStringLiteral("-p"), queue}));
}

Config Config::load()
{
    Config c;

    QFile f(configFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        c.printer = o.value(QStringLiteral("printer")).toString();
        c.device = o.value(QStringLiteral("device")).toString(c.device);
        c.mac = o.value(QStringLiteral("mac")).toString();
        c.model = o.value(QStringLiteral("model")).toString();
    }

    if (c.mac.isEmpty())
        c.mac = systemConfValue(QStringLiteral("PTOUCH_MAC"));
    const QString sysDevice = systemConfValue(QStringLiteral("PTOUCH_DEVICE"));
    if (!sysDevice.isEmpty() && c.device.isEmpty())
        c.device = sysDevice;

    const auto env = [](const char *name) { return qEnvironmentVariable(name); };
    if (!env("PTOUCH_PRINTER").isEmpty())
        c.printer = env("PTOUCH_PRINTER");
    if (!env("PTOUCH_DEVICE").isEmpty())
        c.device = env("PTOUCH_DEVICE");
    if (!env("PTOUCH_MAC").isEmpty())
        c.mac = env("PTOUCH_MAC");

    if (c.printer.isEmpty())
        c.printer = findPtouchQueue();

    // USB device numbers move when the printer is replugged or restarted, so a
    // stale path in the configuration should not stop us finding the machine.
    bool askedForModel = false;
    if (!isNetworkDevice(c.device) && !QFileInfo::exists(c.device)) {
        // Ask for the status over the link the jobs travel on. A queue that prints
        // to a host belongs to a printer on the network, and that printer answers
        // over SNMP — even if the same machine is plugged in over USB as well, where
        // its node may not be accessible. Asking for the model first keeps other
        // makes of network printer out of it.
        const QString host = queueHost(c.printer);
        QString model;
        if (!host.isEmpty()) {
            model = networkPrinterModel(host);
            askedForModel = true;
            // A printer that is switched off (they power down by themselves) says
            // nothing. Its queue still names it, and it stays a network printer:
            // falling back to a Bluetooth port would only report a setup that
            // was never meant to exist.
            if (model.isEmpty())
                model = queueModel(c.printer);
        }
        if (!model.isEmpty()) {
            c.device = networkDevice(host);
            if (c.model.isEmpty())
                c.model = model;
        } else {
            const QList<Port> ports = candidatePorts();
            if (!ports.isEmpty())
                c.device = ports.first().path;
        }
    }
    // The model decides how much of the tape the head reaches; on the network it
    // is one question away, and the queue knows it while the printer is off.
    if (c.model.isEmpty() && isNetworkDevice(c.device)) {
        if (!askedForModel)
            c.model = networkPrinterModel(networkHost(c.device));
        if (c.model.isEmpty())
            c.model = queueModel(c.printer);
    }
    return c;
}

void Config::save() const
{
    const QString path = configFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject o;
    o[QStringLiteral("printer")] = printer;
    o[QStringLiteral("device")] = device;
    o[QStringLiteral("mac")] = mac;
    o[QStringLiteral("model")] = model;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson());
}

} // namespace ptouch
