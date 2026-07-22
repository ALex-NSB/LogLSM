/**
 * qltp.hpp — LTP (LogLSM Transport Protocol) v1.0 для Qt
 *
 * Формат пакета (до stuffing):
 *   FEND | ADDR | CMD | FLAGS | SEQ[0] | SEQ[1] | LEN[0] | LEN[1] | PAYLOAD | CRC[0] | CRC[1]
 *
 *   SEQ, LEN — little-endian; CRC16 — big-endian.
 *   CRC16-CCITT: poly=0x1021, init=0xFFFF, no reflect, xor=0. Тест: "123456789" -> 0x29B1.
 *   CRC покрывает ADDR..PAYLOAD (сырые байты, до stuffing).
 *
 * Спека: Doc/LogLSM Transport Protocol/LTP_PROTOCOL_v1.0_RU.docx
 */
#ifndef QLTP_H
#define QLTP_H

#include <QObject>
#include <QByteArray>

/* FLAGS (спека §5) */
#define LTP_FLAG_DIR   0x01   /* 0 = запрос хоста, 1 = ответ устройства */
#define LTP_FLAG_ERR   0x02   /* 1 = PAYLOAD содержит 1 байт кода ошибки */

/* Коды ошибок (спека §9.2) */
enum LtpError : quint8 {
    LTP_ERR_OK          = 0x00,
    LTP_ERR_UNKNOWN_CMD = 0x01,
    LTP_ERR_BAD_LEN     = 0x02,
    LTP_ERR_CRC         = 0x03,
    LTP_ERR_BUSY        = 0x04,
    LTP_ERR_FLASH       = 0x05,
    LTP_ERR_IMU         = 0x06,
    LTP_ERR_NO_SERVICE  = 0x07
};

class QLtp : public QObject
{
    Q_OBJECT
public:
    explicit QLtp(QObject *parent = nullptr);

    /** Собрать пакет запроса/ответа (со стартовым FEND и stuffing). */
    static QByteArray build(quint8 cmd, quint8 addr,
                            quint8 flags = 0, quint16 seq = 0,
                            const QByteArray &payload = QByteArray());

    static quint16 crc16(const QByteArray &data);
    static QString errorName(quint8 code);

    void feed(quint8 byte);
    void feed(const QByteArray &bytes);

    quint32 crcErrors() const { return m_crcErrors; }

signals:
    void packetReceived(quint8 addr, quint8 cmd,
                        quint8 flags, quint16 seq, QByteArray payload);

private:
    enum FsmState { WAIT_FEND, READ_HEADER, READ_PAYLOAD, READ_CRC };
    static constexpr int HDR_SIZE = 7;   /* ADDR+CMD+FLAGS+SEQ[2]+LEN[2] */
    static constexpr int MAX_PAYLOAD = 65535;

    void resync();
    void acceptByte(quint8 b);           /* байт после destuffing */
    static void appendStuffed(QByteArray &out, quint8 b);

    FsmState   m_state    = WAIT_FEND;
    bool       m_stuffing = false;
    QByteArray m_header;                 /* 7 байт заголовка */
    QByteArray m_payload;
    QByteArray m_crc;                    /* 2 байта CRC (BE) */
    quint16    m_len      = 0;
    quint32    m_crcErrors = 0;
};

#endif // QLTP_H
