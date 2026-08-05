#include "inspireic15dserialreader.h"

#include <QDebug>
#include <QMutexLocker>

#ifndef Q_OS_WIN
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#endif

inspireic15dserialreader::inspireic15dserialreader(QObject *parent, const QString &deviceFilename)
    : QThread(parent), deviceFilename(deviceFilename) {}

inspireic15dserialreader::~inspireic15dserialreader() {
    requestInterruption();
    wait(1000);
}

void inspireic15dserialreader::snapshot(bool &isOpen, qint64 &bytes, QByteArray &chunk,
                                        QString &currentError) const {
    QMutexLocker locker(&stateMutex);
    isOpen = portOpen;
    bytes = totalBytes;
    chunk = lastChunk;
    currentError = error;
}

void inspireic15dserialreader::run() {
#ifdef Q_OS_WIN
    QMutexLocker locker(&stateMutex);
    error = QStringLiteral("IC15D internal serial diagnostics are not supported on Windows");
#else
    // Diagnostic mode is deliberately passive: read-only open, no chmod, no
    // termios changes, no flush, no line-discipline ioctl, and no writes.
    const QByteArray filename = deviceFilename.toLocal8Bit();
    const int fd = open(filename.constData(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        QMutexLocker locker(&stateMutex);
        error = QString::fromLocal8Bit(strerror(errno));
        qWarning() << "IC15D serial: unable to open" << deviceFilename << error;
        return;
    }

    {
        QMutexLocker locker(&stateMutex);
        portOpen = true;
        error.clear();
    }
    qInfo() << "IC15D serial: receive-only capture opened" << deviceFilename;

    while (!isInterruptionRequested()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        timeval timeout = {0, 200000};
        const int selected = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (selected < 0) {
            if (errno == EINTR)
                continue;
            QMutexLocker locker(&stateMutex);
            error = QString::fromLocal8Bit(strerror(errno));
            break;
        }
        if (selected == 0)
            continue;

        char buffer[256];
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            const QByteArray received(buffer, static_cast<int>(count));
            {
                QMutexLocker locker(&stateMutex);
                totalBytes += count;
                lastChunk = received;
            }
            qInfo().noquote() << "IC15D serial RX (no decode):" << received.toHex(' ');
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            QMutexLocker locker(&stateMutex);
            error = QString::fromLocal8Bit(strerror(errno));
            break;
        }
    }

    close(fd);
    QMutexLocker locker(&stateMutex);
    portOpen = false;
#endif
}
