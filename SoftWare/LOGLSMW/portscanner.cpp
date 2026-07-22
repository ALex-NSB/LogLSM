#include "portscanner.h"

#include <QSerialPortInfo>

PortScanner::PortScanner(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &PortScanner::onTimeout);
    connect(&m_port, &QSerialPort::readyRead, this, &PortScanner::onReadyRead);
}

void PortScanner::start(qint32 baud, quint8 addr,
                        const QString &pinnedPort, int timeoutMs,
                        int pinnedTimeoutMs, int pinnedRetries,
                        quint8 altAddr)
{
    if (m_scanning)
        return;

    m_baud            = baud;
    m_addr            = addr;
    m_altAddr         = altAddr;
    m_timeoutMs       = timeoutMs;
    m_pinnedPort      = pinnedPort;
    m_pinnedTimeoutMs = pinnedTimeoutMs;
    m_pinnedRetries   = pinnedRetries;
    m_bytesReceived   = 0;

    m_pending.clear();
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos)
        m_pending.append(info.portName());

    // Закреплённый порт — первым (ТЗ §2.2)
    if (!pinnedPort.isEmpty() && m_pending.contains(pinnedPort)) {
        m_pending.removeAll(pinnedPort);
        m_pending.prepend(pinnedPort);
    }

    m_scanning = true;
    tryNext();
}

void PortScanner::stop()
{
    m_timer.stop();
    if (m_port.isOpen())
        m_port.close();
    m_scanning = false;
    m_pending.clear();
}

void PortScanner::tryNext()
{
    if (m_pending.isEmpty()) {
        m_scanning = false;
        emit finished(QString());
        return;
    }

    m_currentPort   = m_pending.takeFirst();
    m_gotReply      = false;
    m_bytesReceived = 0;

    // Счётчик повторов: для закреплённого порта — m_pinnedRetries, иначе 0
    m_retriesLeft = (m_currentPort == m_pinnedPort) ? m_pinnedRetries : 0;

    // Свежий парсер на каждый порт — мусор предыдущего не мешает
    delete m_ltp;
    m_ltp = new QLtp(this);
    connect(m_ltp, &QLtp::packetReceived, this, &PortScanner::onPacket);

    m_port.setPortName(m_currentPort);
    if (!m_port.open(QIODevice::ReadWrite)) {
        emit portChecked(m_currentPort, Result::Busy);
        tryNext();
        return;
    }

    m_port.setBaudRate(m_baud);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    // DTR=false — ST-Link VCP при смене DTR может пульсировать NRST на
    // таргете; отключаем явно, чтобы открытие порта не сбрасывало устройство.
    m_port.setDataTerminalReady(false);
    m_port.clear();

    sendPings();

    const int tms = (m_currentPort == m_pinnedPort) ? m_pinnedTimeoutMs : m_timeoutMs;
    m_timer.start(tms);
}

void PortScanner::sendPings()
{
    m_port.write(QLtp::build(0x01 /*PING*/, m_addr, 0, 0));
    m_port.flush();
    if (m_altAddr == 0x00)
        return;
    // Небольшая пауза перед вторым PING — см. комментарий в .h. 5 мс с
    // запасом перекрывают время на обработку первого пакета на стенде
    // (на 921600 сам пакет передаётся ~110 мкс, проблема была в обработке
    // ISR, не в пропускной способности линии).
    QTimer::singleShot(5, this, [this] {
        if (m_port.isOpen()) {
            m_port.write(QLtp::build(0x01 /*PING*/, m_altAddr, 0, 0));
            m_port.flush();
        }
    });
}

void PortScanner::onReadyRead()
{
    const QByteArray data = m_port.readAll();
    m_bytesReceived += quint32(data.size());
    if (m_ltp)
        m_ltp->feed(data);
}

void PortScanner::onPacket(quint8 addr, quint8 cmd,
                           quint8 flags, quint16 seq, QByteArray payload)
{
    Q_UNUSED(seq)
    Q_UNUSED(payload)

    if (!(flags & LTP_FLAG_DIR))      // echo собственного PING
        return;
    if (cmd != 0x01)
        return;
    if (addr != m_addr && !(m_altAddr != 0x00 && addr == m_altAddr))
        return;

    m_gotReply = true;
    m_timer.stop();
    finishPort(Result::Found);
}

void PortScanner::onTimeout()
{
    // Повторная попытка для закрепленного порта (порт остается открытым)
    if (m_retriesLeft > 0) {
        m_retriesLeft--;

        // Свежий парсер - сбрасываем мусор в RX-буфере
        delete m_ltp;
        m_ltp = new QLtp(this);
        connect(m_ltp, &QLtp::packetReceived, this, &PortScanner::onPacket);

        m_port.clear();
        sendPings();
        m_timer.start(m_pinnedTimeoutMs);
        return;
    }
    finishPort(Result::NoReply);
}

void PortScanner::finishPort(Result r)
{
    if (m_port.isOpen())
        m_port.close();
    emit portChecked(m_currentPort, r);

    if (r == Result::Found) {
        m_scanning = false;
        m_pending.clear();
        emit finished(m_currentPort);
        return;
    }
    tryNext();
}
