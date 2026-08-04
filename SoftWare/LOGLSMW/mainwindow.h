#ifndef MAINWINDOW_H
#define MAINWINDOW_H

/*
 * LOGLSMW v2 — этапы 1–2 + вкладки инженерного режима.
 * Ядро: ComLink + DeviceController + PortScanner.
 */

#include <QMainWindow>
#include <QLabel>
#include <QTimer>
#include <QList>
#include <QVector>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>

#include "comlink.h"
#include "devicecontroller.h"
#include "portscanner.h"
#include "activationbar.h"

namespace Ui {
class MainWindow;
}
class QPushButton;
class QCheckBox;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void setServiceMode(bool on);
    void tickPcClock();

    void onScanClicked();
    void onScanPortChecked(const QString &port, PortScanner::Result result);
    void onScanFinished(const QString &foundPort);
    void onLinkLost(const QString &reason);

    void onResponse(quint8 cmd, const QByteArray &payload, quint32 tag);
    void onProtoError(quint8 cmd, quint8 code, const QString &name);
    void onRequestFailed(quint8 cmd);
    void onCounters(quint32 tx, quint32 rx, quint32 crcErrors);

    void setDarkTheme(bool on);   // переключатель темы (меню «Вид»)

private:
    Ui::MainWindow *ui;

    ComLink          *m_link    = nullptr;
    DeviceController *m_dev     = nullptr;
    PortScanner      *m_scanner = nullptr;

    QLabel *lblPort     = nullptr;
    QLabel *lblAddr     = nullptr;
    QLabel *lblCounters = nullptr;
    QLabel *lblMode     = nullptr;

    QTimer m_pcClockTimer;
    QTimer m_devClockTimer;
    QTimer m_retryTimer;        // авторетрай поиска устройства
    bool   m_timeAlarm  = false;
    bool   m_blinkPhase = false;
    // Индикация срабатывания сторожей (17.07.2026): при РОСТЕ счётчика
    // перезапусков значение моргает красным ~10 c (tickPcClock).
    int    m_lastRstTimer = -1;   // -1 = ещё не читали (первый GET_STATS не моргает)
    int    m_lastRstPower = -1;
    int    m_rstTimerBlinkLeft = 0;  // осталось тиков моргания (1 Гц)
    int    m_rstPowerBlinkLeft = 0;
    bool   m_erasing    = false;
    qint64 m_eraseStartMs = 0;

    // Регистратор «пропал» в режиме A (03.07.2026): стоп циклограммы (0x22)
    // ушёл в таймаут — устройство осталось в автомате и спит в Stop2, на
    // LTP не отвечает. Пока флаг взведён, m_devClockTimer не спамит опросом
    // часов/температур (2 команды/с × таймауты), а шлёт редкий PING —
    // первый же успешный ответ (пользователь перещёлкнул питание/тумблер)
    // снимает флаг в onResponse() и возвращает обычные опросы.
    bool m_regAwol = false;

    // Серия «⚠ Страница» (02.07.2026): кнопка стирает spinMemPages страниц
    // подряд от spinMemStartPage (раньше — только одну стартовую, поле
    // «Страниц» игнорировалось, замечено пользователем). Ответы приходят
    // по одному на страницу — итог/разблокировка/пересканирования только
    // после последнего. Счётчики сбрасываются при каждом нажатии кнопки,
    // так что таймаут середины серии не «залипает» на следующую операцию.
    int m_pageEraseLeft  = 0;   // осталось ответов в серии (0 = серии нет)
    int m_pageEraseTotal = 0;   // всего страниц в серии (для итога в журнале)
    int m_pageEraseErr   = 0;   // страниц с ошибкой стирания

    // ── Активация устройства (ТЗ v2 §2.6, §3) ───────────────────────────────
    enum class ActStep {
        Idle,       // не запущена
        Archive,    // сегм 0 — сохранить данные (по флагу)
        Check,      // сегм 1 — проверка устройства (WHO_AM_I + температура)
        ResetWdt,   // сегм 2 — сброс счётчиков рестартов (per-cycle отсчёт)
        SyncTime,   // сегм 3 — синхронизация времени
        Erase,      // сегм 4 — стирание Flash
        TestWrite,  // сегм 5 — тестовая запись/чтение
        SetReady,   // сегм 6 — постановка на готовность (Активация)
        Done,       // завершена успешно
        Error       // ошибка
    };

    struct ActState {
        ActStep step     = ActStep::Idle;
        int     subPhase = 0;   // внутренняя фаза шага
    } m_act;

    // Автосохранение образа на диск в шаге 1 активации (27.07.2026): если на
    // устройстве есть данные — читаем их постранично и пишем единый Intel HEX
    // (тот же формат, что грузит «Образ RG/LOG» — loadImageFromHexFile), затем
    // волна продолжается сама. Отдельный от m_test read-loop, свой тег TagActDump.
    struct ActDumpState {
        bool       running      = false;
        bool       continueWave = false;  // true: после сохранения продолжить волну активации
        quint16    page      = 0;    // следующая запрашиваемая страница
        quint32    pageEnd   = 0;    // первая страница ЗА диапазоном (не вкл.); quint32:
                                     // kFlashTotalPages=65536 НЕ влезает в quint16 (→0, баг)!
        quint16    startPage = 0;    // первая страница образа (для адресов HEX)
        QByteArray buf;              // накопленные сырые байты (256×N)
        QString    path;             // куда пишем .hex
    } m_actDump;

    // Сохранить данные устройства на диск (образ HEX) и сбросить флаг
    // «несохранённые данные». continueWave=true → по завершении продолжить
    // волну активации (шаг 1); false → отдельная кнопка «Сохранить данные».
    void startDataDump(bool continueWave);
    void dataSaveFlow();   // §3.2 политика сохранения: активировано×флаг (см. .cpp)

    // Обновить сегмент 0 ленты по флагу данных + занятости Flash (28.07.2026):
    // красный «Сохранить» (флаг взведён) / жёлтый «Стёрто» (Flash пуста) /
    // зелёный «Сохранено» (данные есть и сохранены). Зовётся из ответа DATA_FLAG
    // и по завершении сканирования памяти (m_firstFreePage).
    void updateSaveSegment();
    void updateActivationState();   // трёхпозиционное состояние: не активирован / между жизнями / идёт жизнь
    bool saveImageToHexFile(const QString &path, quint16 startPage,
                            const QByteArray &buf, QString &errMsg) const;

    // Одиночный клик по сегменту (вне полной автопоследовательности,
    // 21.07.2026): сегмент светится жёлтым (Active) с момента отправки
    // команды и зеленеет (Done) ТОЛЬКО когда пришло подтверждение реального
    // результата — не сразу по факту отправки, как было раньше.
    bool m_actWdtPending  = false;   // сегмент 1 «Сброс WDT», ждём GET_STATS
    bool m_actSyncPending = false;   // сегмент 2 «Синхро время», ждём GET_DATETIME
    qint64 m_actWdtActiveMs  = 0;    // когда пожелтел (мс, epoch) — для мин. видимости
    qint64 m_actSyncActiveMs = 0;

    // Перевести сегмент в финальное состояние, но не раньше, чем через
    // kActMinVisibleMs после того, как он пожелтел (21.07.2026: на реальном
    // железе RESET_STATS/SET_DATETIME отрабатывают за миллисекунды — быстрее
    // одного кадра отрисовки, жёлтый физически не успевал быть виден).
    void activationSetSectorMinDelay(int idx, ActivationBar::SectorState st, qint64 activeSinceMs);

    // ── Температурный прогон ─────────────────────────────────────────────────
    enum class TempRunStep { Idle, Write, Read };
    struct TempRunState {
        bool        running  = false;
        bool        halted   = false;   // вне диапазона — ждём возврата
        TempRunStep step     = TempRunStep::Idle;
        quint16     page     = 0;       // страница для теста
        float       lastTemp = -999.0f; // темп при последней операции (-999=нет данных)
        int         opCount  = 0;
        int         errCount = 0;       // накоплено НЕКОРРЕКТНЫХ БАЙТ за прогон
        quint16     rangeStart = 1;     // начало диапазона страниц (из таблицы)
        int         rangeCount = 1;     // сколько страниц по кругу (spinMemPages)
        quint8      testByte = 0x00;    // байт-шаблон из «Задать» (фикс. на прогон)
        int         passesPerStep = 1;  // «проходов» — страниц на одну температуру
        int         pagesLeftInStep = 0;// осталось страниц в текущей темп-точке
    } m_tempRun;

    QTimer m_tempPollTimer;
    bool   m_tempBlink = false;         // мигание «градусника» активности термотеста

    struct PendingCmd { quint8 cmd; QByteArray payload; quint32 tag; };
    QList<PendingCmd> m_pendingCmds;

    // ── Тестовый цикл (вкладка «Тест памяти») ───────────────────────────────
    enum class TestStep { Idle, Write, Read, Compare, Dump, Done };

    struct MemTestState {
        bool      running    = false;
        int       pageTotal  = 0;    // страниц за один цикл
        int       pageCur    = 0;    // текущая страница (Compare) / derived (Write/Read)
        quint16   pageStart  = 0;
        int       errTotal   = 0;    // страниц/записей с ошибками
        TestStep  step       = TestStep::Idle;
        int       cycleTotal = 1;    // всего циклов
        int       pagesDone  = 0;    // всего страниц обработано (все циклы)
        // «Байт» Факт — реально ПРОЧИТАННЫЙ с устройства байт (первый байт
        // payload последнего успешного FLASH_READ), НЕ эхо поля «Задать»
        // (баг до 02.07.2026: в lblCurByte копировался editTestByte, и
        // колонка «факт» ничего не проверяла). -1 = ещё не читали (в т.ч.
        // во время фазы записи) — ячейка пустая (пробел, соглашение UI).
        // Раньше был ещё lastReadUniform («…» при неоднородной странице) —
        // убран (22.07.2026, по замечанию: не должно быть домысливания,
        // только реально прочитанное значение).
        // Агрегатные `m_test = {...}` (8 инициализаторов) сбрасывают эти
        // поля в дефолты автоматически (C++14 default member init).
        int       lastReadByte    = -1;
    } m_test;

    // Замер реальной скорости операции записи/чтения теста (26.07.2026):
    // засекаем время от нажатия «Запись»/«Чтение» до завершения серии,
    // объём = страниц × циклов × 256 байт, выводим КБ/с в lblMemSpeed.
    QElapsedTimer m_memOpTimer;
    qint64        m_memOpBytes = 0;
    qint64        m_speedBytes = 0;   // объём последней операции для расчёта КБ/с из накопленного времени
    double        m_speedWrKbps = -1.0; // скорость записи (W-фаза замера), <0 = ждём W
    // Замер скорости чтения (27.07.2026): эталон пишется ОДИН раз за сессию на
    // безопасной низкой частоте (с verify), дальше кнопка только читает по
    // готовому на выбранной частоте/режиме и сверяет. m_speedPhase: 0=простой,
    // 1=ждём ответ записи эталона, 2=ждём ответ чтения.
    bool          m_speedRefReady = false;  // эталон записан и сверен в текущей области
    int           m_speedPhase    = 0;
    int           m_speedRefStart = -1;     // область, для которой записан эталон (стр.)
    int           m_speedRefPages = -1;     // при смене Старт/Страниц эталон переписывается

    // «Тихий» автодамп (вход на вкладку / после стирания / после записи
    // образа, 22.07.2026) — memTestDump() АСИНХРОННАЯ (шлёт FLASH_READ,
    // ждёт ответ), поэтому m_test нельзя восстанавливать сразу после
    // вызова (это ломало реальный ход дампа — краш при заходе на вкладку,
    // найдено по факту). Вместо этого — флаг: по факту РЕАЛЬНОГО завершения
    // (в обработчике ответа) панель «Проверка» просто очищается, а не
    // показывает результат дампа как будто это был настоящий тест.
    bool m_silentDump = false;
    // После «Запись»/«Чтение» показываем дамп сразу (без «Прочитать») — но
    // memTestDump() переиспользует m_test.* под СВОЙ проход чтения (диапазон
    // ограничен kAutoDumpMaxPages=64), стирая реальный результат теста
    // (Страниц/Байт/Старт). Найдено по факту (22.07.2026): после записи
    // 100 страниц панель показывала «Страниц 64», «Байт» от дампа, а не от
    // теста. Сохраняем состояние ДО вызова, восстанавливаем, когда дамп
    // реально (асинхронно) завершится — см. m_silentDump рядом по духу.
    MemTestState m_savedTestForDump{};
    bool m_restoreTestAfterDump = false;
    // После завершения «Образ» — подтянуть «Старт»/Задать к НОВОЙ первой
    // свободной странице по факту пересчёта (22.07.2026, по просьбе: чтобы
    // сразу указывал на свободное место для следующей операции). Флаг —
    // пересчёт асинхронный (flashBinSearchStart), значение узнаём позже.
    bool m_syncStartAfterImage = false;

    // Кеш последнего записанного образа (для Сравнить)
    QList<QByteArray> m_imgCache;
    quint16           m_imgStartPage = 1;

    // Прогресс записи образа (RG / LOG)
    int       m_imgPagesTotal = 0;
    int       m_imgPagesDone  = 0;
    QWidget  *m_imgActiveBtn  = nullptr;  // кнопка, запустившая образ
    QString   m_imgBtnLabel;              // исходная надпись кнопки
    QString   m_imgFileName;              // имя файла образа (для лога завершения)

    // Какой образ сейчас фактически лежит в Flash (для подсветки кнопок
    // «Образ RG/LOG» — см. archiveMode → refreshImgButtonsHighlight).
    enum class FlashImageState { Unknown, Empty, Registrator, Logger };
    FlashImageState m_flashImageState = FlashImageState::Unknown;
    void refreshImgButtonsHighlight();
    void probeFlashImageState();   // читает стр.1 и определяет Empty/RG/Logger

    void setOpsEnabled(bool on, QWidget *except = nullptr); // блокировка кнопок операций
    // «Прочитать»/авто-дамп: по умолчанию диапазон = поля «Старт»/«Страниц»
    // (spinMemStartPage/spinMemPages). startOverride/countOverride >=0 — явный
    // диапазон (после загрузки образа показываем весь образ). (17.07.2026)
    void memTestDump(int startOverride = -1, int countOverride = -1);
    void memTestStep();         // выдать следующую команду по state machine
    void memTestUpdateUi();     // обновить индикаторы левого столбца + прогрессбар
    void renderHexDump(quint16 startPage, const QByteArray &buf); // вывод в txtHexDump
    QByteArray m_dumpBuf;        // накопитель байт для «Прочитать» (TestStep::Dump)

    // Загрузка образа из файла Intel HEX (вкладка «Тест памяти», кнопки «Образ RG/LOG»).
    // pages[i] соответствует странице (startPage + i), всегда 256 байт каждая
    // (пропуски адресов в файле заполняются 0xFF — как на чистой NOR Flash).
    bool loadImageFromHexFile(const QString &path, quint16 &startPage,
                               QList<QByteArray> &pages, QString &errMsg) const;
    // Поставить разобранный образ в очередь FLASH_WRITE и завести прогресс-индикацию
    // на кнопке (тот же механизм, что раньше использовался для синтетической генерации).
    void startImageWrite(QPushButton *btn, const QString &label,
                          quint16 startPage, const QList<QByteArray> &pages,
                          const QString &fileName);

    // ── Архив (вкладка «Данные»): чтение журнала из Flash и разбор записей ──
    // Запускается автоматически после записи образа RG/LOG — читает обратно
    // именно тот диапазон страниц, что был записан, разбирает его как
    // Регистратор v2 (24 байта/запись, до 10 записей/страница) или Logger v2
    // (типизированные фреймы по 256 байт) и заполняет виджеты вкладки «Данные»
    // (lblFirstDate/Time, lblLastDate/Time, lblCyclesUsed/Free, lblMaxSpeed/
    // Vibro, plotSpeed/plotVibro) — то есть отображает результат именно
    // загрузки образа, а не «живые» данные с устройства.
    enum class ArchiveMode { Registrator, Logger };

    struct ArchiveState {
        bool        running       = false;
        ArchiveMode mode          = ArchiveMode::Registrator;
        quint16     pageStart     = 1;
        quint16     pageNext      = 1;
        quint16     pageLimit     = 1;   // первая страница ЗА диапазоном (не вкл.)
        // Прошивка (cmdReadMem() в com.c) физически отдаёт не более 1 страницы
        // за один FLASH_READ (буфер на 256 байт) — запрашивать больше смысла
        // нет, ответ всё равно урежется. archiveHandleChunk при этом не
        // доверяет запрошенному числу страниц, а смотрит на фактически
        // пришедшее, так что увеличение этого числа в будущем (после
        // доработки прошивки) ничего не сломает.
        int         chunkPages    = 1;   // страниц за один FLASH_READ
        int         chunkRequested = 0;  // страниц запрошено в текущем чанке (в полёте)
        int         records       = 0;   // завершённых записей/циклов
        int         brokenRecords = 0;   // записей/циклов с CRC=0xFFFF либо не до конца
        quint32     tsFirst       = 0;
        quint32     tsLast        = 0;
        quint32     durationTotal = 0;
        bool        haveDuration  = false;
        float       maxVibro      = 0.0f;   // = max vib1_peak (канал 1, уровень)
        float       maxVib2       = 0.0f;   // max vib2_peak (канал 2, удары/jerk)
        float       maxRpm        = 0.0f;
        bool        haveComplete  = false;  // хотя бы одна завершённая запись/цикл
        bool        trailingOpen  = false;  // последняя запись/цикл — обрыв
        bool        valid         = false;  // архив дочитан хотя бы раз — он, а не GET_STATS,
                                             // теперь владеет lblFirstDate/Time/lblLastDate/Time
        // Для Logger: ts начала текущего разбираемого цикла (для расчёта ts_end по СТОП)
        quint32     curCycleTs    = 0;
        bool        haveCurCycleTs = false;
        QVector<double> plotKeys;
        QVector<double> plotVibro;   // vib1_peak по циклам («пики», график 2)
        QVector<double> plotVibroRms;// vib1_RMS по циклам («уровень», график 1) 19.07
        QVector<double> plotVib2;    // vib2_peak по циклам (канал 2, линия 1) 18.07
        QVector<double> plotRpm;
        QVector<double> plotTs;         // tsStart цикла (для тултипа столбика) 18.07
        QVector<double> plotDuration;   // длительность цикла (с) — столбчатая
        QVector<double> plotTemp;       // температура (°C) по циклам — график 5 (27.07.2026)
                                         // диаграмма «Активное время», 1 столбик = 1 цикл
        QVector<double> plotEpoch;      // поколение часов по циклам (индекс-синхронно
                                         // с plotKeys) — смена = вертикальная линия стыка
                                         // на графиках (02.08.2026)
        // По-записные данные завершённых записей (03.07.2026, для вывода
        // автономного журнала на вкладке «Стенд» — stendShowDeviceLog():
        // агрегатов выше недостаточно, нужны Старт/Стоп/Общее каждого
        // цикла). Индексы синхронны с plot*-векторами (заполняются в той
        // же ветке complete в archiveParseRegistratorPage; Logger их не
        // заполняет). 1000 записей × 12 байт — копейки.
        QVector<quint32> recTs;      // timestamp_start (unix)
        QVector<quint32> recDur;     // duration, с
        QVector<quint32> recTotal;   // duration_total, с
        QVector<quint16> recEpoch;   // поколение часов rec[40..41]: смена между
                                     // соседними записями = точка стыка (часы
                                     // обнулялись после потери питания). 02.08.2026
    } m_arc;

    void archiveStart(ArchiveMode mode, quint16 startPage, int pageCount);
    // Перечитать журнал на полном диапазоне Flash (последний известный режим
    // Регистратор/Logger) — вызывается после стирания и при открытии вкладки
    // «Данные», чтобы она не показывала устаревший кэш предыдущего разбора.
    void archiveRescanFull();
    void archiveRequestChunk();
    void archiveHandleChunk(const QByteArray &data, int pagesInChunk);
    void archiveParseRegistratorPage(const QByteArray &page, int cycleIndex);
    // pageOffset — смещение этой страницы от m_arc.pageStart (для clamp'а
    // pageLimit при встрече незаписанной страницы, см. archiveHandleChunk).
    void archiveParseLoggerPage(const QByteArray &page, int pageOffset);
    void archiveFinish();

    quint8  tempRunTempCmd() const;    // GET_TEMP_* по радио-кнопке прогона
    QString tempRunSensorName() const; // имя выбранного датчика (TMP117/IMU/STM32)
    void   tempRunBeginStep();         // начать блок «проходов» страниц на темп-точке
    void   tempRunStart();
    void   tempRunStop();
    void   tempRunHandleTemp(float t); // вызывается из onResponse при активном прогоне
    void   tempRunDoOp();              // запустить цикл: стереть→записать→прочитать

    bool eventFilter(QObject *obj, QEvent *ev) override;  // дабл-клик «по таймеру» = тест IWDG
    class QCPBars *m_uptimeBars = nullptr;  // столбики «Активного времени» (18.07.2026)
    class QCPBars *m_speedBars  = nullptr;  // столбики «гироскоп» (max об/мин за цикл),
                                            // ось X синхронна с plotUptime (18.07.2026)
    class QCPBars *m_vibBars    = nullptr;  // столбики «акселерометр · уровень» (vib1_peak)
    class QCPBars *m_vib2Bars   = nullptr;  // столбики «акселерометр · удары» (vib2_peak)
    class QCPBars *m_tempBars   = nullptr;  // столбики «температура» (°C по циклам, 27.07.2026)
    QVector<class QCPItemStraightLine*> m_seamItems;   // вертикальные линии стыков времени
                                                       // на графиках «Данных» (02.08.2026)
    // Макс-значения графиков ВЕРТИКАЛЬНО в правом конце (27.07.2026): индексы
    // как kind — 1=обороты, 2=вибрация, 3=удары, 4=температура (0=время не исп.).
    class QCPItemText *m_valLbl[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    class QAction *m_actSimulation = nullptr;   // «Симуляция» в меню «Вид» (27.07.2026)
    // Флаг «есть непрочитанные данные» (0x30, внутр. Flash прибора, 27.07.2026):
    // взведён → блокируем разрушающие операции (стирание и запись), пока не
    // нажали «Считать данные» (сброс). Обновляется ответом DATA_FLAG.
    bool m_dataFlagSet = false;
    bool m_deviceActivated = false;   // ts_activation != 0xFFFFFFFF (из GET_STATS) — для dataSaveFlow
    bool m_dataPresent = false;       // NOR-флаг [1] стр.0: данные физически в NOR (не по скану 0xFF!)
    bool m_dataSaved   = false;       // NOR-флаг [2] стр.0: сохранение выполнено (конец жизни)
    quint32 m_tsActivation = 0xFFFFFFFFu;   // ts последней активации (из GET_STATS) — t0 калибровки от активации
    bool m_saveWasWave = false;   // «Сохранено»: зелёный ТОЛЬКО после волны активации; одиночное сохранение → жёлтый
    // Грубая калибровка RTC («Калибровка» → Часы RTC), 03.08.2026:
    QDateTime m_rtcCalT0;             // метка ПК в момент «Старт» (синхро по границе секунды)
    bool   m_rtcCalActive = false;    // идёт выдержка калибровки
    double m_rtcCurPpm     = 0.0;     // текущая поправка устройства (RTC_CALIB_GET)
    double m_rtcCalNewPpm  = 0.0;     // рассчитанная новая поправка (для «Применить»)
    bool m_serviceMode = false;   // «Сервис»: в операторе доступны только «Считать данные»/«Активировать»
    bool m_vbatBlink   = false;   // фаза моргания сегмента VBAT (заглушка, 28.07.2026)
    void onActivationSectorClicked(int idx);  // клик по сегменту → его операция (17.07.2026)
    void archiveUpdateDashboard();  // перерисовка «Данных» из m_arc (и по живым пушам)
    void archiveDrawSeams();        // вертикальные линии стыков времени на 5 графиках
    void updateCycleScroll();       // синхронизация ползунка истории с осью циклов
    bool stendValidateRanges();     // живая проверка Мин≤Макс: подсветка+блок кнопок
    void stendFillSubSpeeds(const QByteArray &payload);  // 0x29: макс по полкам скорости
    QList<QByteArray> m_regPushOrphans; // ранние пуши без готовой строки (лок-степ):
                                        // не выбрасываем, втягиваем при закрытии
                                        // цикла (19.07.2026, фикс «нет ответа» 1-го)
    void archiveFinishTail();       // лог + границы образа после полного разбора
    void activationStart();
    void activationStop();
    void activationFail(const QString &reason);
    void activationSetSector(int idx, ActivationBar::SectorState st);
    void activationBeginStep(ActStep step);
    void activationHandleResponse(quint8 cmd, const QByteArray &payload);

    void setupCore();
    void setupStatusBar();
    void setupDashboardPlots();
    void setupStend();

    // ── Тема оформления (светлая/тёмная) ────────────────────────────────────
    // Единственный источник цвета — applyStyles(t): и палитра Fusion, и большая
    // таблица стилей строятся из ThemePalette, поэтому переключение тем меняет
    // только её. ThemePalette определена в mainwindow.cpp (вложенная структура).
    struct ThemePalette;
    void applyStyles(const ThemePalette &t);
    void applyTheme(bool dark);     // палитра Fusion + applyStyles() под тему
    void setupThemeMenu();          // пункт меню «Вид → Тёмная тема»
    void restyleThemedPlots();      // перекраска QCustomPlot под тему
    bool m_darkTheme = false;       // текущая тема (грузится из QSettings)

    // ── Стенд (вкладка «Стенд») ──────────────────────────────────────────────
    // Циклограмма работа/простой, см. CLAUDE.md «Стенд (циклограмма
    // испытаний)» и «Алгоритм циклограммы и совмещения журналов». Команды
    // идут на LtpAddr::STEND (0x8C), независимо от адреса регистратора
    // (spinDevAddr/0x8D) — см. DeviceController::enqueueTo().
    enum class StendMode { RandomPM, OnlyPlus, OnlyMinus, NStartStop };
    enum class StendPhase { Idle, Work, Pause };

    // Одна запись истории для правой панели cmpReport.
    // Сторона "Стенд" заполняется сразу при старте цикла.
    // Сторона "Регистратор" — из CMD_CYCLE_PUSH (BENCH) или Flash (FIELD).
    struct StendCycleRecord {
        QDateTime startStend;
        QDateTime stopStend;
        int       speedStend   = 0;  // МАКС заданная скорость цикла, об/мин
        bool      multiSpeed   = false; // >1 смены скорости → сравнение по max_rpm
        QVector<int> speeds;         // заданные скорости полок цикла (стенд знает заранее)
        QVector<int> regSubSpeeds;   // измеренные МАКС по полкам (суб-пуш 0x29)
        int       plannedDurS  = 0;  // плановая длительность (известна при старте)
        bool      inProgress   = false;
        bool      written      = false;  // запись сброшена в файл журнала

        // Данные регистратора
        bool      hasRegData   = false;
        bool      regNoAnswer  = false;  // пуш по циклу так и не пришёл к старту
                                         // следующего цикла → регистратор пропустил
                                         // (правую колонку не заполняем, метка «нет ответа»)
        QDateTime startReg;           // метка времени RTC устройства
        int       speedReg     = 0;   // max_rpm, об/мин
        qint64    durationRegS = 0;   // длительность цикла, с
        qint64    totalRegS    = 0;   // суммарная наработка, с
    };

    struct StendState {
        bool             running     = false;
        StendPhase       phase       = StendPhase::Idle;
        StendMode        mode        = StendMode::RandomPM;
        QVector<int>     steps;        // ступеньки текущей фазы «Работа» (об/мин)
        int              stepIndex    = 0;     // индекс текущего сегмента внутри фазы
        int              triangleIdx  = 0;     // только для NStartStop — позиция в полном ряду ступеней
        int              triangleDir  = 1;     // только для NStartStop — +1 вверх / -1 вниз
        QVector<int>     triangleSteps;         // только для NStartStop — построен один раз на весь прогон
        QDateTime        cycleStartTs;
        int              lastSpeed    = 0;
        qint64           totalStendMs = 0;      // накопленная длительность интервалов (для «Общая»)
        int              groupSize    = 1;      // рабочих циклов в текущей группе (случ. из «Циклов» Мин/Макс)
        int              cycleInGroup = 0;      // сколько циклов группы уже отработано
        QList<StendCycleRecord> history;        // последние циклы для cmpReport (держим 4)
    } m_stend;

    QTimer  m_stendStepTimer;  // переключение ступеньки / окончание фазы work; пауза
    QFile   m_journalFile;     // текущий файл журнала испытания (открыт пока стенд работает)
    qint64    m_journalAccMs   = 0;   // накопленная Стенд-длительность уже записанных циклов (мс)
    qint64    m_journalRegAccS = 0;   // накопленная Рег-длительность уже записанных циклов (с)
    qint64    m_regBaseTotal   = 0;   // totalSec регистратора ДО этого теста (снимается в stendStart)
    qint64    m_lastRegTotS    = 0;   // последнее totS из любого CMD_CYCLE_PUSH (обновляется всегда)
    quint16   m_regFwVersion   = 0;   // версия прошивки регистратора из ответа WHO_AM_I (13.07.2026)
    qint64    m_preTestRegTotS = 0;   // totalSec в памяти ДО RESET_TOTAL — «Память устройства»
    int       m_pushCount      = 0;   // количество CMD_CYCLE_PUSH в текущей сессии
    bool      m_stendActive    = false; // true пока циклограмма работает (кнопка = «Стоп»)
    bool      m_stendKickoffPending = false; // ждём ACK 0x1D перед первой фазой (см. stendKickoff)
    // Сессионные аккумуляторы для циклов, уже вышедших из 4-цикловго окна history.
    // Без них «Общая» в cmpReport сбрасывалась бы к нулю при каждом сдвиге окна.
    qint64    m_sessStendAcc   = 0;   // Стенд: сумма plannedDurS вышедших циклов
    qint64    m_sessRegAcc     = 0;   // Регистратор: сумма durationRegS вышедших (с данными) циклов
    qint64    m_sessRegMatched = 0;   // «Сессия» = сумма durationRegS ТОЛЬКО совпавших
                                      // пушей (обработанных ответов). Нет ответа →
                                      // не добавляем (никаких фантомных времён, 04.07)

    // Бинарный поиск первой свободной страницы NOR Flash (log2(65536)=16 запросов макс.)
    struct BinSearchState {
        bool running  = false;
        int  lo       = 1;
        int  hi       = 65536;  // sentinel: kFlashTotalPages (= "full")
        int  midSent  = 0;      // страница текущего запроса в полёте
        int  step     = 0;      // номер текущего шага (для отображения N/16)
    } m_binSearch;
    int  m_firstFreePage    = -1;  // -1 = не искали / в процессе; 65536 = чип заполнен
    int  m_binSearchRetries = 0;   // число неудачных перезапусков после таймаута
    bool m_binSearchFailed  = false; // Flash не отвечает после всех попыток
    bool m_memAutoDumpPending = false; // авто-дамп при входе на «Тест памяти»:
                                       // ждём завершения двоичного поиска границ,
                                       // затем memTestDump() (17.07.2026)
    int  m_autoDumpStart = -1;         // явный диапазон для отложенного авто-дампа
    int  m_autoDumpCount = -1;         // (после образа — его диапазон; иначе -1 =
                                       // по умолчанию «Старт»/«Страниц»)
    // Живой бар занятости Flash по пушам (12.07.2026): в «Работе» устройство
    // спит, device-scan гейтится → оцениваем занятость от числа записанных
    // циклов (каждый CYCLE_PUSH = 1 запись = 24 байта, kRecordsPerPage/страница).
    int    m_flashLiveBasePage = -1;   // база (свободная стр. на «Старте»)
    qint64 m_flashLiveRecords  = 0;    // записей за сессию (матч-пуши)
    QByteArray m_flashLiveBuf;         // реконструкция содержимого Flash (записи +
                                       // 0xFF-хвосты страниц) для живого HEX-дампа
                                       // в txtHexDump «как Прочитать» (12.07.2026)
    QLabel *m_flashCellLbl  = nullptr; // overlay-подпись номера свободной ячейки
                                       // у края штриховки бара (не встроенный текст)

    void flashBinSearchStart();
    void flashBinSearchSendNext();
    void updateOccupiedLabel();   // «Занято»: Задать=занято стр.(красн.), Факт=осталось стр.(зел.)
    void showMemSpeed(int mode);  // замер скорости контроллером (0=запись,1=чтение), 0x2D
    void flashBinSearchHandlePage(const QByteArray &data);
    void flashBinSearchUpdateUi();

    // noReg=true (03.07.2026, тестовый режим «разбираемся с датчиком»):
    // циклограмма гоняет ТОЛЬКО мотор (0x8C) по обычной программе вкладки
    // «Стенд» — регистратор не трогается вовсе (ни 0x1D, ни 0x22), он
    // остаётся в Service, опросы/мониторинг НЕ подавляются: наблюдаем
    // датчик на графиках «Мониторинга», пока мотор делает холодные старты.
    // Запуск/стоп — кнопкой «Циклограмма» на вкладке «Мониторинг».
    void stendStart(bool noReg = false);
    bool m_stendNoReg = false;
    // Отложенный запуск первой фазы циклограммы (03.07.2026): мотор
    // стартует только ПОСЛЕ ответа регистратора на 0x1D (ACK → автомат
    // взведён до начала вращения; таймаут → предупреждение и тест без
    // регистратора). Без этого команды мотору стояли в очереди за
    // ретраями 0x1D и «Старт» выглядел зависшим. В режиме noReg 0x1D не
    // шлётся — первая фаза стартует сразу.
    void stendKickoff();
    // Сценарий 2 («два разных лога», план 21.06) — офлайн-вид правой панели
    // (03.07.2026): «Из устройства…» даёт колонку «Регистратор» (разбор
    // архива Flash), «Журнал…» — колонку «Стенд» (файл журнала испытания с
    // диска). Когда есть ОБЕ стороны — совмещение (одна точка выравнивания
    // по времени первого цикла файла, дальше по порядку — решение 21.06;
    // если по времени пара не нашлась — по порядку с начала, с пометкой в
    // журнале) и автоматическая колонка «Ошибка» (дельты Интервал/Общее/
    // Скорость; у Старт/Стоп дельты нет — это абсолютное время).
    // m_stendArcToPanel: «Из устройства…» запросила разбор архива —
    // archiveFinish() по завершении вызовет stendShowDeviceLog().
    struct OfflineCycle {
        QDateTime start, stop;
        qint64    intervalS = 0;
        qint64    totalS    = 0;   // «Общее» файла — сессионное накопление
        int       speed     = 0;
    };
    // Модель колонок офлайн-вида (уточнено пользователем 03.07.2026):
    // колонка «Регистратор» — ВСЕГДА только Flash (живые пуши или
    // «Память…»); колонка «Стенд» — ОДИН из источников (или-или-или):
    // живой тест / файл журнала («Журнал…») / файл образа («Образ…» —
    // эталон для проверки целостности записи: образ↔память должны дать
    // нулевые дельты). Загрузка одного источника колонки замещает другой.
    QVector<OfflineCycle> m_offlJournal;         // колонка «Стенд» из файла (журнал ИЛИ образ)
    QString m_offlJrnSrc;                        // подпись источника колонки «Стенд»
    bool    m_offlJrnLifetime = false;           // true: «Общее» в m_offlJournal — lifetime
                                                 // (образ), сравнивать без сессионной базы
    // Сторона «Регистратор» офлайн-вида — отдельное хранилище (03.07.2026),
    // НЕ ссылка на живой m_arc: источника два — «Из устройства…» (копия из
    // m_arc после archiveFinish) и «Из образа…» (разбор записей прямо из
    // .hex-файла, без устройства). Ведущая лента совмещения — эта
    // (приоритет Flash, решение пользователя 03.07): выводятся ВСЕ записи,
    // файл журнала подставляется в «Стенд» только там, где нашлась пара.
    struct OfflineDevRec { quint32 ts = 0, dur = 0, total = 0; int rpm = 0; quint16 epoch = 0; };
    QVector<OfflineDevRec> m_offlDev;
    QString m_offlDevSrc;   // подпись источника записей для сводки в панели
                            // («память устройства» / «образ <файл>»)
    bool stendLoadJournalFile(const QString &path, QString *errOut);
    bool stendLoadImageRecords(const QString &path, QString *errOut); // «Из образа…»
    void stendShowDeviceLog();                   // рендер офлайн-вида (файл ⊕ устройство)
    bool m_stendArcToPanel = false;
    void stendStop(bool manual);
    void stendSetUiRunning(bool running);
    void stendBeginWork();
    void stendBeginPause();
    void stendAdvanceWorkStep();
    void stendNextNStartStop();
    static QVector<int> stendBuildSteps(int n, int minV, int maxV);
    int stendPickNextSpeed(int prev) const;  // выбрать следующую скорость по режиму
    bool sendStendCmd(quint8 cmd, const QByteArray &payload = {});
    void stendHandleResponse(quint8 cmd, const QByteArray &payload);
    void stendRecordCycle();
    void stendFinishCycle();
    void stendRenderReport();
    void stendResolvePendingReg();  // на старте цикла закрыть предыдущую ждущую строку
    void stendFillRegColumn(const QByteArray &payload);  // разобрать CYCLE_PUSH (0x20)
    void stendJournalOpen();                             // создать файл при «Старт»
    void stendJournalWrite(StendCycleRecord &r);         // записать один блок цикла
    void stendUpdateFlashStat();                         // обновить панель «Память устройства»

    // ── Мониторинг (вкладка «Мониторинг») ───────────────────────────────────
    // Периодический опрос GET_AXES_RAW (Acc+Gyro в одной команде) и до 3
    // температур (GET_TEMP_IMU/TMP117/STM), живые скользящие графики
    // plotAcc/plotGyro/plotTemp. Конфигурация датчика (ODR/FS) сознательно
    // НЕ используется в этом проходе (см. CLAUDE.md, сессия 22.06.2026,
    // «начнём со штатных условий») — опрос идёт на текущих настройках IMU
    // (по умолчанию ±2g/±2000dps, LSM6DSO_Init); при «Старт» читаем факт.
    // FS один раз (ACC_GET_FS/GYRO_GET_FS), чтобы перевести raw LSB в
    // физические единицы для графиков, а не угадывать по умолчанию.
    struct MonState {
        bool          running  = false;
        bool          paused   = false;
        QElapsedTimer t0;                  // отсчёт времени графиков от «Старт»/«Очистить», с
        double        windowSec = 10.0;
        int           periodMs  = 50;
        float         accSens_mg   = 0.061f;  // mg/LSB,  дефолт ±2g     (LSM6DSO_Init)
        float         gyroSens_mdps = 70.0f;   // mdps/LSB, дефолт ±2000dps (LSM6DSO_Init)
        QVector<double> tAcc,  axX, axY, axZ;
        QVector<double> tGyro, gyX, gyY, gyZ;
        QVector<double> tTemp[3];           // 0=LSM 1=TMP117 2=STM32
        QVector<double> vTemp[3];
        // Вибрация, посчитанная НА ПК из живого accel (промежуточный отладочный
        // вывод, 13.07.2026): зеркалит firmware HandleSensorData — канал1 = AC
        // модуля |a| (EMA убирает гравитацию), канал2 = jerk |Δ|a||. Показывает
        // вибрацию, пока firmware-запись не доставляет свои значения.
        double vibDc = 0.0; bool vibDcInit = false;   // EMA-оценка |a| (гравитация)
        double vib1Peak = 0.0;                          // пик AC |a| за сессию, g
        double prevAmag = 0.0; bool prevAmagInit = false;
        double vib2Peak = 0.0;                          // пик jerk за сессию, g
    } m_mon;

    QTimer m_monTimer;

    // Авто-калибровка скорости: проход стенда по столбцу «Задано», живой опрос
    // гироскопа на каждой ступени → «Измерено» (03.08.2026).
    struct SpeedCalRun {
        bool  running = false;
        int   curRow  = 0;          // текущая строка таблицы
        int   phase   = 0;          // 0 = разгон/устаканивание, 1 = измерение
        int   phaseMs = 0;          // накоплено мс в текущей фазе
        QVector<double> samples;    // выборки сырой об/мин за окно измерения (усечённое среднее)
        QVector<double> targets;    // заданные скорости ОТМЕЧЕННЫХ строк (параллельно rows)
        QVector<int>    rows;       // индексы строк таблицы (только отмеченные галкой)
        int    cyclesTotal = 1;     // сколько раз пройти весь набор
        int    cycle = 0;           // текущий цикл (0-индекс)
        int    settleMs = 4500;     // разгон/пауза на ступени (из UI)
        int    measMs   = 5000;     // окно измерения на ступени (из UI)
        bool   autoUpdate = false;  // писать коэффициенты в прибор после каждого цикла
    } m_speedCal;
    QTimer m_speedCalTimer;
    void   speedCalAutoStart();
    void   speedCalAutoTick();
    void   speedCalAutoStop(bool finished);
    void   speedCalAutoAccum(const QByteArray &payload);   // накопить rpm из GET_AXES_RAW
    void   speedCalHighlightRow(int row, bool on, int phase); // подсветка текущей ступени
    void   speedCalClearHighlight();
    bool   speedCalWrite(bool confirm, bool reread = true);   // записать коэффициенты измеренных узлов (0x32); reread=false — без переформатирующего чтения (в прогоне)
    void   rtcCalUpdateButtons(int state);   // 0 idle / 1 идёт выдержка / 2 расчёт готов — цвет кнопок RTC
    QTimer m_rtcApplyBlinkTimer;             // моргание «Применить» в состоянии «расчёт готов»
    bool   m_rtcApplyBlinkOn = false;
    int    m_rtcCalUiState = 0;              // 0 idle / 1 выдержка / 2 готово — защита от затирания периодикой

    void   setupMonitor();
    void   monStart();
    void   monPause();
    void   monStop();
    void   monClear();
    void   monExportCsv();
    void   monPoll();                                  // таймер — опрос включённых каналов
    void   monHandleAxes(const QByteArray &payload);
    void   monHandleTemp(quint8 cmd, const QByteArray &payload);
    void   monTrim();                                  // обрезка буферов по «Окну»
    void   monReplot();
    void   monSetUiRunning(bool running);
    int    monPeriodMsFromCombo() const;
    double monWindowSecFromCombo() const;

    void appendLog(const QString &msg);
    void setConnectedUi(bool on, const QString &port = QString());
    qint32 selectedBaud() const;
    quint8 dashboardTempCmd() const;
    void requestCmd(quint8 cmd, const QByteArray &payload = {}, quint32 tag = 0);
    void speedCalSetCell(int row, int col, const QString &text, bool editable = true);
    void speedCalRecompute(int row);                              // Δ% и коэфф. новый по задано/измерено
    void speedCalSetRow(int row, double given, double measured, double workKoef);
};

#endif // MAINWINDOW_H