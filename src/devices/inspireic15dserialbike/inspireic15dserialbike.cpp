#include "inspireic15dserialbike.h"

#include "qzsettings.h"
#include "virtualdevices/virtualbike.h"

#include <QSettings>
inspireic15dserialbike::inspireic15dserialbike(const QString &serialPort, bool noHeartService,
                                               int8_t bikeResistanceOffset, double bikeResistanceGain) {
    m_watt.setType(metric::METRIC_WATT, deviceType());
    Speed.setType(metric::METRIC_SPEED);

    QSettings settings;
    const bool metricPolling =
        settings.value(QZSettings::inspire_ic15d_metric_polling,
                       QZSettings::default_inspire_ic15d_metric_polling).toBool();
    reader = new inspireic15dserialreader(this, serialPort, metricPolling);
    reader->start();

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
    int cadence;
    int power;
    int resistance;
    qint64 validFrames;
    reader->snapshot(portOpen, totalBytes, lastChunk, error, cadence, power, resistance, validFrames);

    if (portOpen != lastPortOpen || error != lastError) {
        if (portOpen)
            emit debug(QStringLiteral("IC15D serial port opened at 19200 8-N-1"));
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

    if (validFrames != lastValidFrames) {
        if (cadence >= 0) {
            Cadence = cadence;
            Speed = static_cast<double>(cadence) * 0.37497622;
        }
        if (power >= 0)
            m_watt = power;
        if (resistance >= 0) {
            Resistance = resistance;
            emit resistanceRead(Resistance.value());
        }
        emit debug(QStringLiteral("IC15D metrics: cadence=") + QString::number(cadence) +
                   QStringLiteral(" rpm, power=") + QString::number(power) +
                   QStringLiteral(" W, resistance=") + QString::number(resistance));
        lastValidFrames = validFrames;
    }

    // This adapter never acts on FTMS control requests.
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
