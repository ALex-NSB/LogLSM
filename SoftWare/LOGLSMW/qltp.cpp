#include "qltp.hpp"

#define FEND  quint8(0xC0)
#define FESC  quint8(0xDB)
#define TFEND quint8(0xDC)
#define TFESC quint8(0xDD)

QLtp::QLtp(QObject *parent) : QObject(parent)
{
    m_header.reserve(HDR_SIZE);
    m_crc.reserve(2);
}

/* ── CRC16-CCITT: poly=0x1021, init=0xFFFF, no reflect, xor=0 ── */
quint16 QLtp::crc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (char c : data) {
        crc ^= quint16(quint8(c)) << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

QString QLtp::errorName(quint8 code)
{
    switch (code) {
    case LTP_ERR_OK:          return QStringLiteral("ERR_OK");
    case LTP_ERR_UNKNOWN_CMD: return QStringLiteral("ERR_UNKNOWN_CMD");
    case LTP_ERR_BAD_LEN:     return QStringLiteral("ERR_BAD_LEN");
    case LTP_ERR_CRC:         return QStringLiteral("ERR_CRC");
    case LTP_ERR_BUSY:        return QStringLiteral("ERR_BUSY");
    case LTP_ERR_FLASH:       return QStringLiteral("ERR_FLASH");
    case LTP_ERR_IMU:         return QStringLiteral("ERR_IMU");
    case LTP_ERR_NO_SERVICE:  return QStringLiteral("ERR_NO_SERVICE");
    default: return QStringLiteral("ERR_0x%1").arg(code, 2, 16, QLatin1Char('0'));
    }
}

void QLtp::appendStuffed(QByteArray &out, quint8 b)
{
    if (b == FEND)      { out.append(char(FESC)); out.append(char(TFEND)); }
    else if (b == FESC) { out.append(char(FESC)); out.append(char(TFESC)); }
    else                  out.append(char(b));
}

QByteArray QLtp::build(quint8 cmd, quint8 addr,
                       quint8 flags, quint16 seq, const QByteArray &payload)
{
    /* Сырой блок ADDR..PAYLOAD — по нему считается CRC */
    QByteArray raw;
    raw.reserve(HDR_SIZE + payload.size());
    raw.append(char(addr));
    raw.append(char(cmd));
    raw.append(char(flags));
    raw.append(char(seq & 0xFF));            // SEQ little-endian
    raw.append(char(seq >> 8));
    quint16 len = quint16(payload.size());
    raw.append(char(len & 0xFF));            // LEN little-endian
    raw.append(char(len >> 8));
    raw.append(payload);

    quint16 crc = crc16(raw);

    QByteArray pkt;
    pkt.reserve(1 + 2 * (raw.size() + 2));
    pkt.append(char(FEND));                  // стартовый FEND не экранируется
    for (char c : raw)
        appendStuffed(pkt, quint8(c));
    appendStuffed(pkt, quint8(crc >> 8));    // CRC big-endian
    appendStuffed(pkt, quint8(crc & 0xFF));
    return pkt;
}

/* ── FSM-парсер ── */

void QLtp::resync()
{
    m_state    = WAIT_FEND;
    m_stuffing = false;
    m_header.clear();
    m_payload.clear();
    m_crc.clear();
}

void QLtp::feed(quint8 b)
{
    /* FEND — безусловная точка синхронизации */
    if (b == FEND) {
        resync();
        m_state = READ_HEADER;
        return;
    }

    if (m_state == WAIT_FEND)
        return;

    /* Destuffing до FSM */
    if (m_stuffing) {
        m_stuffing = false;
        if      (b == TFEND) b = FEND;
        else if (b == TFESC) b = FESC;
        else { resync(); return; }           // некорректный escape
    } else if (b == FESC) {
        m_stuffing = true;
        return;
    }

    acceptByte(b);
}

void QLtp::acceptByte(quint8 b)
{
    switch (m_state)
    {
    case READ_HEADER:
        m_header.append(char(b));
        if (m_header.size() == HDR_SIZE) {
            const quint8 *h = reinterpret_cast<const quint8*>(m_header.constData());
            m_len = quint16(h[5] | (h[6] << 8));            // LEN little-endian
            m_state = (m_len > 0) ? READ_PAYLOAD : READ_CRC;
        }
        break;

    case READ_PAYLOAD:
        m_payload.append(char(b));
        if (m_payload.size() == int(m_len))
            m_state = READ_CRC;
        break;

    case READ_CRC:
        m_crc.append(char(b));
        if (m_crc.size() == 2) {
            const quint8 *h = reinterpret_cast<const quint8*>(m_header.constData());
            const quint8 *c = reinterpret_cast<const quint8*>(m_crc.constData());
            quint16 rxCrc   = quint16((c[0] << 8) | c[1]);  // big-endian
            quint16 calc    = crc16(m_header + m_payload);

            if (calc == rxCrc) {
                quint8  addr  = h[0];
                quint8  cmd   = h[1];
                quint8  flags = h[2];
                quint16 seq   = quint16(h[3] | (h[4] << 8)); // little-endian
                QByteArray payload = m_payload;
                resync();
                emit packetReceived(addr, cmd, flags, seq, payload);
                return;
            }
            m_crcErrors++;
            resync();
        }
        break;

    default:
        resync();
        break;
    }
}

void QLtp::feed(const QByteArray &bytes)
{
    for (char b : bytes)
        feed(quint8(b));
}
