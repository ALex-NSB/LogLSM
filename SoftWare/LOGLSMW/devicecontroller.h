#ifndef DEVICECONTROLLER_H
#define DEVICECONTROLLER_H

/*
 * DeviceController — ядро LOGLSMW (ТЗ v2 §6, ТЗ v1 §5.2):
 *  - очередь запросов: один запрос в полёте, следующий после ответа/таймаута;
 *  - SEQ-матчинг: принимается только ответ с SEQ и CMD текущего запроса;
 *  - таймаут + повторы; после исчерпания — requestFailed;
 *  - фильтры: DIR=1 (echo-защита), ADDR устройства, FLAGS.ERR до PAYLOAD.
 *
 * UI не знает про LTP — только сигналы этого класса.
 * Типизированные сигналы (axesReady и т.п.) — этап 3, пока cmd+payload.
 */

#include <QObject>
#include <QQueue>
#include <QTimer>

#include "comlink.h"
#include "qltp.hpp"

/* Команды LTP (прошивка LOGLSMA App/Src/com.c) */
namespace LtpCmd {
constexpr quint8 PING             = 0x01;
constexpr quint8 WHO_AM_I         = 0x02;
constexpr quint8 FLASH_READ_ID    = 0x04;
constexpr quint8 FLASH_ERASE      = 0x05;   // стирание всего чипа (долго!)
constexpr quint8 FLASH_PAGE_ERASE = 0x06;   // payload: pg_num uint16 LE
constexpr quint8 FLASH_READ       = 0x07;   // payload: addr u32 LE + size u32 LE → данные
constexpr quint8 FLASH_WRITE      = 0x08;   // payload: pg_num u16 LE + данные (≤256)
constexpr quint8 FLASH_STATE      = 0x09;   // → er, WIP (занятость)
constexpr quint8 FLASH_ON         = 0x0E;
constexpr quint8 FLASH_OFF        = 0x0F;
constexpr quint8 GET_TEMP_TMP117  = 0x0A;
constexpr quint8 GET_TEMP_IMU     = 0x10;
constexpr quint8 GET_TEMP_STM     = 0x11;
/* Конфигурация IMU (com.c, см. таблицу CMD[] 0x12..0x19) — добавлены
 * 22.06.2026 для вкладки «Мониторинг» (нужен ACC_GET_FS/GYRO_GET_FS, чтобы
 * перевести raw LSB в физические единицы). SetOdr/GetOdr в прошивке —
 * заглушки (er_not_impl, см. com.c::cmdAccSetOdr и др.), не использовать. */
constexpr quint8 ACC_SET_ODR      = 0x12;   // прошивка: не реализовано (er_not_impl)
constexpr quint8 ACC_SET_FS       = 0x13;   // payload: fullscale_g  i32 LE (2/4/8/16)
constexpr quint8 GYRO_SET_ODR     = 0x14;   // прошивка: не реализовано (er_not_impl)
constexpr quint8 GYRO_SET_FS      = 0x15;   // payload: fullscale_dps i32 LE (125/250/500/1000/2000)
constexpr quint8 ACC_GET_ODR      = 0x16;   // прошивка: не реализовано (er_not_impl)
constexpr quint8 ACC_GET_FS       = 0x17;   // → cod|fullscale_g i32 LE
constexpr quint8 GYRO_GET_ODR     = 0x18;   // прошивка: не реализовано (er_not_impl)
constexpr quint8 GYRO_GET_FS      = 0x19;   // → cod|fullscale_dps i32 LE
constexpr quint8 GET_AXES_RAW     = 0x1A;   // → cod|gyro xyz i16 LE|acc xyz i16 LE
constexpr quint8 GET_DATETIME     = 0x1B;
constexpr quint8 SET_DATETIME     = 0x1C;
constexpr quint8 START_REGISTER   = 0x1D;
constexpr quint8 GET_STATS         = 0x1E;   // ТЗ v2 §7.1 (в прошивке — TODO)
constexpr quint8 FLASH_SECTOR_ERASE = 0x1F;  // payload: sec_num uint16 LE (сектор 4 КБ)

/* Unsolicited push от регистратора — отправляется прошивкой после каждого
 * завершённого цикла вращения (BENCH-режим, WKUP1=1).
 * Payload 16 байт: start_ts(6) | duration_s u32 | total_s u32 | max_rpm u16 */
constexpr quint8 CYCLE_PUSH         = 0x20;

/* Сброс r->totalSec на регистраторе.
 * Отправляется при старте циклограммы стенда (stendStart / stendJournalOpen),
 * чтобы предстартовое вращение мотора не смещало поле «Общая». */
constexpr quint8 RESET_TOTAL        = 0x21;

/* Остановка автомата SLEEP->CONFIRM->ROTATING (режим A) на регистраторе и
 * возврат его в Service (03.07.2026). Пара к START_REGISTER (0x1D):
 * 0x1D шлётся при старте циклограммы (после него регистратор уходит из
 * Service в автомат с реальным Stop2 и на LTP-запросы НЕ отвечает — только
 * шлёт CYCLE_PUSH после каждого цикла), 0x22 — при остановке. Доходит
 * только в «бодрых» фазах автомата (CONFIRM/ROTATING — прошивка зовёт
 * ComPoll попутно); в паузах (Stop2) регистратор глух — команда уйдёт в
 * таймаут, тогда прошивку вернёт фронт WKUP1 (тумблер, если есть) или
 * повторный стоп во время следующего вращения. */
constexpr quint8 STOP_REGISTER      = 0x22;

/* «Тест» (04.07.2026) — запуск ТОГО ЖЕ автомата, что и START_REGISTER, но
 * БЕЗ сна: регистратор остаётся бодрым и на связи (в фазе SLEEP не уходит в
 * Stop2, ловит фронт INT1/WKUP2 флагом WUF2 на ходу). Отличие от 0x1D для
 * ПК: опросы 0x8D во время теста НЕ подавляем (устройство отвечает), но так
 * же ждём CYCLE_PUSH после каждого цикла → колонка «Регистратор». Стоп —
 * общий STOP_REGISTER (0x22). */
constexpr quint8 START_TEST         = 0x23;
constexpr quint8 RESET_STATS        = 0x24;   // сброс счётчиков перезапусков (журнал iflash)
constexpr quint8 WDG_KICK           = 0x25;   // unsolicited: «сторож поглажен» (Сервис, ~1 Гц)
constexpr quint8 WDG_TEST           = 0x26;   // тест IWDG: отключить рефреш → сброс ~32 c
constexpr quint8 CLEAR_JOURNALS     = 0x27;   // «Очистить журналы»: стереть активации(121)+рестарты(124..127), паспорт(123) цел
constexpr quint8 SUBSPEED_PUSH      = 0x29;   // unsolicited: макс по полкам скорости (сервис/тест)

/* «Стереть данные» (21.07.2026, по замечанию) — НОВАЯ команда, ОТДЕЛЬНАЯ от
 * FLASH_ERASE (0x05, стирает ВЕСЬ чип). Страница 0 — служебная информация
 * (паспорт устройства и т.п.), реальные данные регистратора — со страницы 1.
 * FLASH_ERASE_DATA должна стирать чип СО страницы 1, оставляя страницу 0
 * нетронутой. ⚠ ПРОШИВКА: ЕЩЁ НЕ РЕАЛИЗОВАНА (нет обработчика в com.c) —
 * до появления там ответит LTP_ERR_UNKNOWN_CMD. Без payload, как FLASH_ERASE. */
constexpr quint8 FLASH_ERASE_DATA   = 0x2A;

/* «Режим SPI» (22.07.2026, по просьбе) — ТОЛЬКО для ручной проверки/сравнения
 * в Сервисе (не для «Работы» — там автовыбор экономичного режима прошивкой).
 * payload[0]: 0 = SPI (standart_mode), 1 = SPIx4 (quadspi_mode). SPIx2
 * (dual_mode) сознательно исключён — в прошивке есть в enum, но нигде не
 * реализован; понадобится, возможно, только при большом составе периферии
 * в некоторых конфигурациях — доделаем тогда же. */
constexpr quint8 FLASH_SET_SPI_MODE = 0x2B;

/* Частота флеша (26.07.2026) — только Сервис (устройство разогнано до 80 МГц).
 * payload[0]: индекс 0..4 → 80/40/20/10/5 МГц (делитель QSPI от 80). */
constexpr quint8 FLASH_SET_FREQ     = 0x2C;

/* Замер скорости флеша контроллером (26.07.2026) — только Сервис.
 * payload: [0..3] стартовая страница, [4..7] число страниц (uint32 LE).
 * ответ: [0]=err, [1..4]=мкс записи, [5..8]=мкс чтения (uint32 LE). */
constexpr quint8 FLASH_SPEED_TEST   = 0x2D;
constexpr quint8 ACT_HISTORY        = 0x2E;   // история активаций: список ts «жизней» со стр.121
constexpr quint8 SPEED_CAL_GET      = 0x2F;   // калибровка скорости: чтение таблицы узлов (стр.123)
constexpr quint8 SPEED_CAL_SET      = 0x32;   // калибровка скорости: запись таблицы узлов (стр.123)
constexpr quint8 RTC_CALIB_GET      = 0x33;   // калибровка RTC: чтение поправки ppm
constexpr quint8 RTC_CALIB_SET      = 0x34;   // калибровка RTC: запись поправки ppm (стр.123)
constexpr quint8 PASSPORT_GET       = 0x35;   // паспорт: чтение серийник/вариант/дата (стр.123)
constexpr quint8 PASSPORT_SET       = 0x36;   // паспорт: запись серийник/вариант/дата (стр.123)
/* Флаг «есть непрочитанные данные» во внутр. Flash (стр.122, прошивка 27.07.2026).
 * payload[0]: 0=прочитать, 1=взвести, 2=сбросить. ответ: [0]=err, [1]=состояние
 * (1=взведён/непрочитано, 0=сброшен/прочитано). */
constexpr quint8 DATA_FLAG          = 0x30;
/* Формат записи цикла (маркёр слова): payload[0] = 0 базовый / 1 уплотнённый /
 * 2 подробный. Прошивка запоминает и пишет соответствующий маркёр [0] слова
 * (0xF5/0xF3/0xF4). Ответ: [0]=err. 28.07.2026. */
constexpr quint8 REC_FORMAT         = 0x31;

/* Команды стенда (прошивка Stend/Stend LOGLSM, com_interr.c/StepDriver.c) —
 * адресуются НЕ регистратору, а Nucleo (LtpAddr::STEND, 0x8C). Коды
 * совпадают по значению с FLASH_READ_ID/FLASH_ERASE/FLASH_PAGE_ERASE выше —
 * это не конфликт, т.к. LTP-команда скоуплена адресом пакета, но поэтому
 * ответы по тегу TagStend разбираются ДО общего switch(cmd) в onResponse(),
 * не через него (см. mainwindow.cpp).
 */
constexpr quint8 STEND_START = 0x04;   // без payload
constexpr quint8 STEND_SPEED = 0x05;   // payload: speed_rpm u16 LE + coefficient u8
constexpr quint8 STEND_STOP  = 0x06;   // без payload
} // namespace LtpCmd

/* Адреса устройств на линии LTP (см. CLAUDE.md, раздел «Стенд»):
 * регистратор настраивается в UI (spinDevAddr, обычно 0x8D), стенд — фиксиро-
 * ванный адрес протокола, аппаратно не меняется. */
namespace LtpAddr {
constexpr quint8 STEND = 0x8C;
}

class DeviceController : public QObject
{
    Q_OBJECT

public:
    explicit DeviceController(ComLink *link, QObject *parent = nullptr);

    void setTargetAddr(quint8 addr) { m_targetAddr = addr; }
    quint8 targetAddr() const { return m_targetAddr; }
    void setTimeout(int ms) { m_timeoutMs = ms; }
    void setRetries(int n) { m_retries = n; }

    /** Поставить запрос в очередь. tag — пользовательская метка, возвращается
     *  в responseReady (различать фоновый опрос и ручные команды).
     *  Возвращает false, если линия закрыта. */
    bool enqueue(quint8 cmd, const QByteArray &payload = QByteArray(), quint32 tag = 0);

    /** То же самое, но с явным адресом получателя — НЕ меняет m_targetAddr
     *  (используется для стенда, 0x8C, пока основной адрес остаётся
     *  адресом регистратора, например 0x8D). */
    bool enqueueTo(quint8 addr, quint8 cmd, const QByteArray &payload = QByteArray(), quint32 tag = 0);

    /* «Выстрелил-и-забыл» (07.07.2026): шлёт пакет НАПРЯМУЮ, не ставя в очередь
     * и НЕ ожидая ACK (без m_inFlight/таймера). Для команд циклограммы стенду —
     * расписание их и так не ждёт, а ожидание ACK на STEND_STOP держало
     * единственный канал и тормозило опрос графиков на стопе. Пришедший ACK
     * просто игнорируется (нет in-flight для матчинга). */
    void sendFireAndForget(quint8 addr, quint8 cmd, const QByteArray &payload = QByteArray());

    void clearQueue();
    bool busy() const { return m_inFlight; }

    quint32 txCount() const { return m_tx; }
    quint32 rxCount() const { return m_rx; }
    quint32 crcErrors() const { return m_ltp->crcErrors(); }

signals:
    void responseReady(quint8 cmd, const QByteArray &payload, quint32 tag);
    void errorReceived(quint8 cmd, quint8 code, const QString &name); // FLAGS.ERR
    void requestFailed(quint8 cmd);                                   // таймаут+повторы
    void countersChanged(quint32 tx, quint32 rx, quint32 crcErrors);

    /** Unsolicited-пакет от регистратора (DIR=1, но SEQ не совпадает ни с
     *  одним запросом в очереди). Сейчас: только CYCLE_PUSH (0x20). */
    void unsolicitedFromReg(quint8 cmd, QByteArray payload);

private slots:
    void onPacket(quint8 addr, quint8 cmd,
                  quint8 flags, quint16 seq, QByteArray payload);
    void onTimeout();

private:
    struct Request {
        quint8     addr;     // адрес получателя ЭТОГО запроса (см. enqueueTo)
        quint8     cmd;
        QByteArray payload;
        quint16    seq;
        int        attemptsLeft;
        quint32    tag;
    };

    void pump();             // отправить следующий запрос, если линия свободна
    void transmit();         // отправить m_current (повторно — тот же SEQ)
    void finishCurrent();    // запрос завершён, продолжить очередь

    ComLink *m_link;
    QLtp    *m_ltp;
    QTimer   m_timer;

    QQueue<Request> m_queue;
    Request  m_current{};
    bool     m_inFlight   = false;
    quint8   m_targetAddr = 0x8D;
    quint16  m_seq        = 0;
    int      m_timeoutMs  = 500;
    int      m_retries    = 2;
    quint32  m_tx = 0;
    quint32  m_rx = 0;
};

#endif // DEVICECONTROLLER_H
