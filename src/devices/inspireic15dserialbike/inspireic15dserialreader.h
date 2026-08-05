#ifndef INSPIREIC15DSERIALREADER_H
#define INSPIREIC15DSERIALREADER_H

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

class inspireic15dserialreader : public QThread {
  public:
    explicit inspireic15dserialreader(QObject *parent, const QString &deviceFilename, bool metricPollingEnabled);
    ~inspireic15dserialreader() override;

    void snapshot(bool &portOpen, qint64 &totalBytes, QByteArray &lastChunk, QString &error, int &cadence,
                  int &power, int &resistance, qint64 &validFrames) const;

  protected:
    void run() override;

  private:
    const QString deviceFilename;
    const bool metricPollingEnabled;
    mutable QMutex stateMutex;
    bool portOpen = false;
    qint64 totalBytes = 0;
    QByteArray lastChunk;
    QString error;
    int cadence = -1;
    int power = -1;
    int resistance = -1;
    qint64 validFrames = 0;
};

#endif // INSPIREIC15DSERIALREADER_H
