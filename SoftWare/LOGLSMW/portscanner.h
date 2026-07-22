#ifndef PORTSCANNER_H
#define PORTSCANNER_H

/*
 * PortScanner — автопоиск устройства (ТЗ v2 §2.2):
 * перебор COM-портов системы: открыть на заданной скорости → PING на ADDR →
 * таймаут ~200 мс. Закреплённый порт проверяется первым.
 * Асинхронный (UI не блокируется).
 */

#include <QObject>
#include <QSerialPort>
#include <QStringList>
#include <QTimer>

#include "qltp.hpp"

class PortScanner : public QObject
{
    Q_OBJECT

public:
    enum class Result { NoReply, Found, Busy };

    explicit PortScanner(QObject *parent = nullptr);

    /** Запустить поиск. pinnedPort (если задан) проверяется первым.
     *  altAddr (если задан, не 0x00) — дополнительный адрес, на который
     *  тоже отправляется PING на каждом порту: автоподключение находит
     *  порт, если ответил ЛЮБОЙ из двух адресов. Нужно для случая, когда
     *  регистратор физически отключён, а на линии остался только стенд
     *  (0x8C, см. CLAUDE.md «Стенд») — без этого сканер не найдёт порт
     *  вообще, раз регистратор не отвечает. */
    void start(qint32 baud, quint8 addr,
               const QString &pinnedPort = QString(),
               int timeoutMs = 200, int pinnedTimeoutMs = 500, int pinnedRetries = 2,
               quint8 altAddr = 0x00);
    void stop();
    bool isScanning() const { return m_scanning; }
    quint32 bytesReceived()  const { return m_bytesReceived; }
    quint32 crcErrors()      const { return m_ltp ? m_ltp->crcErrors() : 0; }

signals:
    void portChecked(const QString &port, PortScanner::Result result);
    /** foundPort пуст, если устройство не найдено. */
    void finished(const QString &foundPort);

private slots:
    void onReadyRead();
    void onTimeout();
    void onPacket(quint8 addr, quint8 cmd,
                  quint8 flags, quint16 seq, QByteArray payload);

private:
    void tryNext();
    void finishPort(Result r);
    /** Шлёт PING на m_addr сразу, и (если задан altAddr) на m_altAddr —
     *  ОТЛОЖЕННО, через небольшую паузу (см. .cpp). Раньше оба PING уходили
     *  впритык один за другим — на 921600 это иногда роняло приём второго
     *  пакета на стороне стенда (приём по прерыванию байт-за-байтом не
     *  успевал освободиться после релей-логики первого пакета, см.
     *  CLAUDE.md, найдено 22.06.2026). Пауза — дешёвый и безопасный фикс
     *  именно для сценария сканирования (единственное место, где два
     *  разных пакета шлются без ожидания ответа между ними). */
    void sendPings();

    QSerialPort m_port;
    QLtp       *m_ltp = nullptr;
    QTimer      m_timer;

    void retryCurrentOrNext();

    QStringList m_pending;
    QString     m_currentPort;
    QString     m_pinnedPort;
    bool        m_scanning        = false;
    bool        m_gotReply        = false;
    qint32      m_baud            = 921600;
    quint8      m_addr            = 0x8D;
    quint8      m_altAddr         = 0x00;   // 0x00 = не используется
    int         m_timeoutMs       = 200;
    int         m_pinnedTimeoutMs = 500;
    int         m_pinnedRetries   = 2;
    int         m_retriesLeft     = 0;
    quint32     m_bytesReceived   = 0;
};

#endif // PORTSCANNER_H
