#include "comlink.h"

ComLink::ComLink(QObject *parent)
    : QObject(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &ComLink::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &ComLink::onError);
}

bool ComLink::open(const QString &portName, qint32 baud)
{
    if (m_port.isOpen())
        m_port.close();

    m_port.setPortName(portName);
    if (!m_port.open(QIODevice::ReadWrite))
        return false;

    m_port.setBaudRate(baud);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    // DTR=false — ST-Link VCP при переходе DTR HIGH может пульсировать NRST на
    // таргете; отключаем сразу после открытия порта (то же самое уже сделано
    // в PortScanner::tryNext — здесь симметрично, чтобы m_link->open() не
    // сбрасывал устройство каждый раз при подключении).
    m_port.setDataTerminalReady(false);
    m_port.clear();
    return true;
}

void ComLink::close()
{
    if (m_port.isOpen()) {
        m_port.close();
        /* linkClosed (не linkLost!) — штатное отключение пользователем.
         * DeviceController слушает оба сигнала для очистки очереди, но
         * авторетрай в mainwindow подключён ТОЛЬКО к linkLost (ResourceError).
         * Иначе close() → linkLost → авторетрай → бесконечный цикл
         * переподключений при каждом нажатии «Отключить». */
        emit linkClosed();
    }
}

bool ComLink::send(const QByteArray &data)
{
    if (!m_port.isOpen())
        return false;
    return m_port.write(data) == data.size();
}

void ComLink::onReadyRead()
{
    const QByteArray data = m_port.readAll();
    if (!data.isEmpty())
        emit dataReceived(data);
}

void ComLink::onError(QSerialPort::SerialPortError error)
{
    // ResourceError = устройство пропало с шины (выдернут кабель)
    if (error == QSerialPort::ResourceError && m_port.isOpen()) {
        m_port.close();
        emit linkLost(QStringLiteral("устройство снято с линии"));
    }
}
