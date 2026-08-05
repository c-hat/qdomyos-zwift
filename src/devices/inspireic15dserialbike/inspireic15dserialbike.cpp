#include "inspireic15dserialbike.h"

#include "qzsettings.h"
#include "virtualdevices/virtualbike.h"

#include <QSettings>
inspireic15dserialbike::inspireic15dserialbike(const QString &serialPort, bool noHeartService,
                                               int8_t bikeResistanceOffset, double bikeResistanceGain) {
    m_watt.setType(metric::METRIC_WATT, deviceType());
    Speed.setType(metric::METRIC_SPEED);

    reader = new inspireic15dserialreader(this, serialPort);
    reader->start();

    QSettings settings;
    if (settings.value(QZSettings::virtual_device_enabled, QZSettings::default_virtual_device_enabled).toBool()) {
        emit debug(QStringLiteral("creating receive-only IC15D virtual bike interface..."));
        auto virtualBike = new virtualbike(this, true, noHeartService, bikeResistanceOffset, bikeResistanceGain);
        this->setVirtualDevice(virtualBike, VIRTUAL_DEVICE_MODE::PRIMARY);
    }

    refresh = new QTimer(this);
    connect(refresh, &QTimer::timeout, this, &inspireic15dserialbike::update);
    refresh->start(200);
}

inspireic15dserialbike::~inspireic15dserialbike() {
    if (reader) {
        reader->requestInterruption();
        reader->wait(1000);
    }
}

void inspireic15dserialbike::update() {
    bool portOpen;
    qint64 totalBytes;
    QByteArray lastChunk;
    QString error;
    reader->snapshot(portOpen, totalBytes, lastChunk, error);

    if (portOpen != lastPortOpen || error != lastError) {
        if (portOpen)
            emit debug(QStringLiteral("IC15D serial port opened in receive-only mode"));
        if (!error.isEmpty())
            emit debug(QStringLiteral("IC15D serial error: ") + error);
        lastPortOpen = portOpen;
        lastError = error;
    }
    if (totalBytes != lastReportedBytes) {
        emit debug(QStringLiteral("IC15D serial bytes received: ") + QString::number(totalBytes) +
                   QStringLiteral("; latest: ") + QString::fromLatin1(lastChunk.toHex(' ')));
        lastReportedBytes = totalBytes;
    }

    // Protocol decoding is intentionally deferred until passive captures identify
    // packet boundaries and metric fields. Never act on FTMS control requests.
    requestResistance = -1;
    requestPower = -1;
    requestInclination = -100;
    requestStart = -1;
    requestStop = -1;

    update_metrics(false, watts());
    if (firstUpdate) {
        emit connectedAndDiscovered();
        firstUpdate = false;
    }
}

bool inspireic15dserialbike::connected() { return lastPortOpen; }

uint16_t inspireic15dserialbike::watts() { return static_cast<uint16_t>(m_watt.value()); }

resistance_t inspireic15dserialbike::resistanceFromPowerRequest(uint16_t) {
    return static_cast<resistance_t>(Resistance.value());
}

resistance_t inspireic15dserialbike::pelotonToBikeResistance(int pelotonResistance) {
    return static_cast<resistance_t>(pelotonResistance);
}
