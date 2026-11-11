// Which printer, which queue, which RFCOMM port.
// Nothing is hard-wired: environment > user configuration > system configuration.
#pragma once

#include <QString>

namespace ptouch {

struct Config {
    QString printer;                                  // CUPS queue
    // Where the status is asked for: a device node, or snmp://host on the network.
    QString device = QStringLiteral("/dev/rfcomm0");
    QString mac;                                      // Bluetooth address
    QString model;                                    // e.g. PT-P710BT

    static Config load();
    void save() const;

    static QString configFilePath();
};

// First CUPS queue that uses the ptouch driver.
QString findPtouchQueue();

// Host a queue sends its jobs to (socket://, lpd://, ipp://); empty for queues on
// a local port.
QString queueHost(const QString &queue);
QString hostFromDeviceUri(const QString &uri);

// P-touch model a queue was set up for, from its make-and-model entry; empty for
// other printers. Known even while the printer is switched off.
QString queueModel(const QString &queue);
QString modelFromQueueOptions(const QString &lpoptionsOutput);

// Values from /etc/ptouch-studio/rfcomm.conf, written by the system setup.
QString systemConfValue(const QString &key);

constexpr const char *SystemConfPath = "/etc/ptouch-studio/rfcomm.conf";

} // namespace ptouch
