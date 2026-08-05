#ifndef INSPIREIC15DSERIALREADER_H
#define INSPIREIC15DSERIALREADER_H

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

class inspireic15dserialreader : public QThread {
  public:
    explicit inspireic15dserialreader(QObject *parent, const QString &deviceFilename);
    ~inspireic15dserialreader() override;

    void snapshot(bool &portOpen, qint64 &totalBytes, QByteArray &lastChunk, QString &error) const;

  protected:
    void run() override;

  private:
    const QString deviceFilename;
    mutable QMutex stateMutex;
    bool portOpen = false;
    qint64 totalBytes = 0;
    QByteArray lastChunk;
    QString error;
};

#endif // INSPIREIC15DSERIALREADER_H
