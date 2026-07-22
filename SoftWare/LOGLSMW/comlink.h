#ifndef COMLINK_H
#define COMLINK_H

/*
 * ComLink — обёртка QSerialPort (ТЗ LOGLSMW v2 §6).
 * Заменяет прототипный класс COM (копия в proto_v0/, оригинал в git).
 */

#include <QObject>
#include <QSerialPort>

class ComLink : public QObject
{
    Q_OBJECT

public:
    explicit ComLink(QObject *parent = nullptr);

    bool open(const QString &portName, qint32 baud);
    void close();
    bool isOpen() const { return m_port.isOpen(); }
    QString portName() const { return m_port.portName(); }
    qint32 baudRate() const { return m_port.baudRate(); }

    /** Сменить скорость на уже открытом порту без переоткрытия (для
     *  «горячего» переключения вместе с прошивкой через LtpCmd::SET_BAUD —
     *  см. switchBaudLive() в mainwindow.cpp). */
    bool setBaudRate(qint32 baud) { return m_port.isOpen() && m_port.setBaudRate(baud); }

    bool send(const QByteArray &data);

signals:
    void dataReceived(const QByteArray &data);
    void linkLost(const QString &reason);   // кабель выдернут / ResourceError
    void linkClosed();                      // штатное close() — без авторетрая

private slots:
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);

private:
    QSerialPort m_port;
};

#endif // COMLINK_H
