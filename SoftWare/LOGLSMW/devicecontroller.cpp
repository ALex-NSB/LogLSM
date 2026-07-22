#include "devicecontroller.h"

DeviceController::DeviceController(ComLink *link, QObject *parent)
    : QObject(parent)
    , m_link(link)
    , m_ltp(new QLtp(this))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &DeviceController::onTimeout);

    connect(m_link, &ComLink::dataReceived, this, [this](const QByteArray &d) {
        m_ltp->feed(d);
    });
    connect(m_ltp, &QLtp::packetReceived, this, &DeviceController::onPacket);

    // Оба сигнала — аварийный разрыв (linkLost) и штатное close() (linkClosed) —
    // сбрасывают состояние DeviceController. Без этого m_inFlight=true блокирует
    // pump() при переподключении, пока не истечёт таймер старой команды.
    connect(m_link, &ComLink::linkLost, this, [this](const QString &) {
        m_timer.stop();
        m_inFlight = false;
        clearQueue();
    });
    connect(m_link, &ComLink::linkClosed, this, [this] {
        m_timer.stop();
        m_inFlight = false;
        clearQueue();
    });
}

bool DeviceController::enqueue(quint8 cmd, const QByteArray &payload, quint32 tag)
{
    return enqueueTo(m_targetAddr, cmd, payload, tag);
}

bool DeviceController::enqueueTo(quint8 addr, quint8 cmd, const QByteArray &payload, quint32 tag)
{
    if (!m_link->isOpen())
        return false;

    Request r;
    r.addr         = addr;
    r.cmd          = cmd;
    r.payload      = payload;
    r.seq          = m_seq++;
    r.attemptsLeft = m_retries + 1;
    r.tag          = tag;
    m_queue.enqueue(r);
    pump();
    return true;
}

void DeviceController::clearQueue()
{
    m_queue.clear();
    // Запрос, уже находящийся "в полёте" (m_inFlight), тоже нужно отменить —
    // не только очистить хвост очереди. Раньше это делали вручную только в
    // linkLost/linkClosed (см. конструктор) — тот же паттерн нужен и здесь:
    // без сброса m_inFlight следующий enqueue()/pump() ничего не отправит,
    // пока не истечёт таймер СТАРОГО запроса (до ~1.5 с с повторами), а если
    // за это время пользователь успел нажать "Старт" ещё раз — новые команды
    // просто зависают в очереди без единой попытки передачи (баг найден
    // 02.07.2026 на кнопке "Чтение" вкладки «Тест памяти»: клик-стоп-клик
    // оставлял тест висеть без единого ответа/таймаута в логе).
    if (m_inFlight) {
        m_timer.stop();
        m_inFlight = false;
    }
}

void DeviceController::pump()
{
    if (m_inFlight || m_queue.isEmpty() || !m_link->isOpen())
        return;
    m_current  = m_queue.dequeue();
    m_inFlight = true;
    transmit();
}

void DeviceController::transmit()
{
    m_current.attemptsLeft--;
    const QByteArray pkt = QLtp::build(m_current.cmd, m_current.addr,
                                       0 /*DIR=0*/, m_current.seq,
                                       m_current.payload);
    m_link->send(pkt);
    m_tx++;
    emit countersChanged(m_tx, m_rx, m_ltp->crcErrors());
    m_timer.start(m_timeoutMs);
}

void DeviceController::sendFireAndForget(quint8 addr, quint8 cmd, const QByteArray &payload)
{
    if (!m_link->isOpen())
        return;
    // Прямая отправка, БЕЗ очереди и БЕЗ ожидания ACK (m_inFlight/таймер не
    // трогаем) — канал сразу свободен для очередных команд (07.07.2026).
    const QByteArray pkt = QLtp::build(cmd, addr, 0 /*DIR=0*/, m_seq++, payload);
    m_link->send(pkt);
    m_tx++;
    emit countersChanged(m_tx, m_rx, m_ltp->crcErrors());
}

void DeviceController::onPacket(quint8 addr, quint8 cmd,
                                quint8 flags, quint16 seq, QByteArray payload)
{
    // Echo-защита: свои запросы (DIR=0) игнорируем
    if (!(flags & LTP_FLAG_DIR))
        return;

    // Базовый фильтр линии: интересуют только наши адреса — регистратор
    // (m_targetAddr, настраивается в UI) и стенд (LtpAddr::STEND, 0x8C,
    // фиксированный). Чужой адрес — точно не наш ответ.
    if (addr != m_targetAddr && addr != LtpAddr::STEND)
        return;

    m_rx++;
    emit countersChanged(m_tx, m_rx, m_ltp->crcErrors());

    // Unsolicited push от регистратора (BENCH-режим): перехватываем до
    // SEQ-фильтра, т.к. у push нет парного запроса и SEQ заведомо не совпадёт.
    if (addr == m_targetAddr
        && (cmd == LtpCmd::CYCLE_PUSH || cmd == LtpCmd::WDG_KICK
            || cmd == LtpCmd::SUBSPEED_PUSH)) {
        emit unsolicitedFromReg(cmd, payload);
        return;
    }

    // SEQ+ADDR+CMD-матчинг: поздние/чужие ответы — игнор (ТЗ v1 §5.2).
    // ADDR проверяем отдельно от m_targetAddr, т.к. m_current.addr может
    // быть адресом стенда (enqueueTo) — коды команд стенда совпадают по
    // значению с кодами команд регистратора (см. devicecontroller.h), без
    // этой проверки ответ с одного адреса мог бы случайно подтвердить
    // запрос к другому (SEQ уникален глобально и так защищает, но проверка
    // адреса — дополнительная подстраховка, не лишняя).
    if (!m_inFlight || seq != m_current.seq || cmd != m_current.cmd || addr != m_current.addr)
        return;

    m_timer.stop();

    // FLAGS.ERR проверяется до разбора PAYLOAD
    if (flags & LTP_FLAG_ERR) {
        const quint8 code = payload.isEmpty() ? 0xFF : quint8(payload.at(0));
        emit errorReceived(cmd, code, QLtp::errorName(code));
    } else {
        emit responseReady(cmd, payload, m_current.tag);
    }
    finishCurrent();
}

void DeviceController::onTimeout()
{
    if (!m_inFlight)
        return;

    if (m_current.attemptsLeft > 0) {
        transmit();              // повтор с тем же SEQ
        return;
    }
    const quint8 cmd = m_current.cmd;
    finishCurrent();
    emit requestFailed(cmd);
}

void DeviceController::finishCurrent()
{
    m_inFlight = false;
    pump();
}
