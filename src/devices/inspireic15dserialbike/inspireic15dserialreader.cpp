#include "inspireic15dserialreader.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>

#ifndef Q_OS_WIN
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

inspireic15dserialreader::inspireic15dserialreader(QObject *parent, const QString &deviceFilename,
                                                   bool metricPollingEnabled)
    : QThread(parent), deviceFilename(deviceFilename), metricPollingEnabled(metricPollingEnabled) {}

inspireic15dserialreader::~inspireic15dserialreader() {
    requestInterruption();
    wait(1000);
}

void inspireic15dserialreader::snapshot(bool &isOpen, qint64 &bytes, QByteArray &chunk, QString &currentError,
                                        int &currentCadence, int &currentPower, int &currentResistance,
                                        qint64 &frames) const {
    QMutexLocker locker(&stateMutex);
    isOpen = portOpen;
    bytes = totalBytes;
    chunk = lastChunk;
    currentError = error;
    currentCadence = cadence;
    currentPower = power;
    currentResistance = resistance;
    frames = validFrames;
}

void inspireic15dserialreader::run() {
#ifdef Q_OS_WIN
    QMutexLocker locker(&stateMutex);
    error = QStringLiteral("IC15D internal serial diagnostics are not supported on Windows");
#else
    // Polling is opt-in. With it disabled this remains a read-only diagnostic.
    const QByteArray filename = deviceFilename.toLocal8Bit();
    const int accessMode = metricPollingEnabled ? O_RDWR : O_RDONLY;
    const int fd = open(filename.constData(), accessMode | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        QMutexLocker locker(&stateMutex);
        error = QString::fromLocal8Bit(strerror(errno));
        qWarning() << "IC15D serial: unable to open" << deviceFilename << error;
        return;
    }

    termios options;
    if (tcgetattr(fd, &options) != 0) {
        QMutexLocker locker(&stateMutex);
        error = QStringLiteral("Unable to read serial settings: ") + QString::fromLocal8Bit(strerror(errno));
        close(fd);
        return;
    }
    cfmakeraw(&options);
    cfsetispeed(&options, B19200);
    cfsetospeed(&options, B19200);
    options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
#ifdef CRTSCTS
    options.c_cflag &= ~CRTSCTS;
#endif
    options.c_cflag |= CS8 | CLOCAL | CREAD;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        QMutexLocker locker(&stateMutex);
        error = QStringLiteral("Unable to configure 19200 8-N-1: ") +
                QString::fromLocal8Bit(strerror(errno));
        close(fd);
        return;
    }

    {
        QMutexLocker locker(&stateMutex);
        portOpen = true;
        error.clear();
    }
    qInfo() << "IC15D serial: opened at 19200 8-N-1; metric polling" << metricPollingEnabled
            << deviceFilename;

    const QByteArray readCommands[] = {
        QByteArray::fromHex("e845533032e2f6"), // Stock handshake.
        QByteArray::fromHex("f54136f6"),       // Cadence (RPM).
        QByteArray::fromHex("f5493ef6"),       // Resistance.
        QByteArray::fromHex("f54439f6"),       // Instantaneous power.
        QByteArray::fromHex("f5493ef6")        // Resistance, matching the stock cycle.
    };
    int commandIndex = 0;
    QElapsedTimer pollTimer;
    pollTimer.start();
    QByteArray pending;

    while (!isInterruptionRequested()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        timeval timeout = {0, 30000};
        const int selected = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (selected < 0) {
            if (errno == EINTR)
                continue;
            QMutexLocker locker(&stateMutex);
            error = QString::fromLocal8Bit(strerror(errno));
            break;
        }
        if (selected > 0) {
            char buffer[256];
            const ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                const QByteArray received(buffer, static_cast<int>(count));
                pending.append(received);
                {
                    QMutexLocker locker(&stateMutex);
                    totalBytes += count;
                    lastChunk = received;
                }
                qInfo().noquote() << "IC15D serial RX:" << received.toHex(' ');

                while (!pending.isEmpty()) {
                    int start = -1;
                    for (int i = 0; i < pending.size(); ++i) {
                        const quint8 byte = static_cast<quint8>(pending.at(i));
                        if (byte == 0xf1 || byte == 0xf3) {
                            start = i;
                            break;
                        }
                    }
                    if (start < 0) {
                        pending.clear();
                        break;
                    }
                    if (start > 0)
                        pending.remove(0, start);
                    if (pending.size() < 3)
                        break;

                    const bool negativeAcknowledgement = static_cast<quint8>(pending.at(0)) == 0xf3;
                    const int payloadLength = negativeAcknowledgement ? 0 : static_cast<quint8>(pending.at(2));
                    const int frameSize = negativeAcknowledgement ? 5 : payloadLength + 5;
                    if (payloadLength > 32) {
                        pending.remove(0, 1);
                        continue;
                    }
                    if (pending.size() < frameSize)
                        break;

                    const QByteArray frame = pending.left(frameSize);
                    pending.remove(0, frameSize);
                    if (negativeAcknowledgement) {
                        qWarning().noquote() << "IC15D serial NAK:" << frame.toHex(' ');
                        continue;
                    }
                    if (static_cast<quint8>(frame.at(frameSize - 1)) != 0xf6)
                        continue;
                    quint8 checksum = 0;
                    for (int i = 0; i <= payloadLength + 2; ++i)
                        checksum = static_cast<quint8>(checksum + static_cast<quint8>(frame.at(i)));
                    if (checksum != static_cast<quint8>(frame.at(payloadLength + 3)))
                        continue;

                    int value = 0;
                    bool validValue = payloadLength > 0;
                    for (int i = 3; i < payloadLength + 3; ++i) {
                        const quint8 digit = static_cast<quint8>(frame.at(i));
                        if (digit < '0' || digit > '9') {
                            validValue = false;
                            break;
                        }
                        value = (value * 10) + (digit - '0');
                    }
                    if (!validValue)
                        continue;

                    const quint8 type = static_cast<quint8>(frame.at(1));
                    {
                        QMutexLocker locker(&stateMutex);
                        if (type == 'A')
                            cadence = value;
                        else if (type == 'D')
                            power = value;
                        else if (type == 'I')
                            resistance = value;
                        else
                            continue;
                        ++validFrames;
                    }
                    qInfo() << "IC15D metric" << QChar(type) << value;
                }
            } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                QMutexLocker locker(&stateMutex);
                error = QString::fromLocal8Bit(strerror(errno));
                break;
            }
        }

        if (metricPollingEnabled && pollTimer.elapsed() >= 200) {
            const QByteArray &command = readCommands[commandIndex];
            const ssize_t written = write(fd, command.constData(), static_cast<size_t>(command.size()));
            if (written != command.size()) {
                QMutexLocker locker(&stateMutex);
                error = QStringLiteral("Metric query write failed: ") + QString::fromLocal8Bit(strerror(errno));
                break;
            }
            qInfo().noquote() << "IC15D metric query TX:" << command.toHex(' ');
            commandIndex = (commandIndex + 1) % (sizeof(readCommands) / sizeof(readCommands[0]));
            pollTimer.restart();
        }
    }

    close(fd);
    QMutexLocker locker(&stateMutex);
    portOpen = false;
#endif
}
