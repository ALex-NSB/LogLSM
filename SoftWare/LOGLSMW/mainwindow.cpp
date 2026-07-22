#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qltp.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QToolTip>
#include <QToolButton>
#include <QSpinBox>
#include <QPushButton>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMap>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QDesktopServices>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextStream>
#include <cmath>
#include <cstring>
#include <limits>
#include <QSyntaxHighlighter>

namespace {
// Подсветка строки-МАКСИМУМА в отчёте полок (19.07.2026): строку с маркером «◄»
// (полка, уходящая в Flash) красим цветом — вместо текстового слова «макс».
class MaxRowHighlighter : public QSyntaxHighlighter {
public:
    explicit MaxRowHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
protected:
    void highlightBlock(const QString &t) override {
        if (t.contains(QChar(0x25C4))) {   // ◄
            QTextCharFormat f;
            f.setForeground(QColor(0x2E, 0xCC, 0x71));
            f.setFontWeight(QFont::Bold);
            // С 10-го символа (21.07.2026): первая строка «Полки» может нести
            // подпись в тех же 10 позициях, что «Скорость»/«Старт» — подпись
            // должна остаться обычным цветом, красим только сами данные.
            const int from = qMin(10, t.length());
            setFormat(from, t.length() - from, f);
        }
    }
};
const char *kMono = "Consolas";
const char *kOrg  = "LogLSM";
const char *kApp  = "LOGLSMW";
constexpr quint32 TagManual  = 1;   // запрос с вкладки «Команды» (не фоновый опрос)
constexpr quint32 TagTest    = 2;   // шаг автоматического тестового цикла
constexpr quint32 TagTempRun = 3;   // шаг температурного прогона
constexpr quint32 TagAct     = 4;   // шаг активации устройства
constexpr quint32 TagImg     = 5;   // запись образа RG/LOG
constexpr quint32 TagArchive = 6;   // чтение журнала из Flash для вкладки «Данные»
constexpr quint32 TagProbe   = 7;   // разовое чтение стр.1 — определить, что лежит в Flash (RG/LOG/пусто)
constexpr quint32 TagStend   = 8;   // команда стенду (0x8C) — START/SPEED/STOP циклограммы
constexpr quint32 TagMon     = 9;   // фоновый опрос вкладки «Мониторинг» (GET_AXES_RAW/GET_TEMP_*)
constexpr quint32 TagSyncTime  = 10; // ручная синхронизация времени (btnSyncTime/btnGetTime) — логировать ответ
constexpr quint32 TagBinSearch = 11; // бинарный поиск первой свободной страницы Flash

// Чувствительность LSM6DSO по факт. полной шкале (ACC_GET_FS/GYRO_GET_FS,
// см. devicecontroller.h) — значения из лежащей в основе ST-библиотеки
// (Firmware/LOGLSMA/App/Inc/lsm6dso.h, LSM6DSO_*_SENSITIVITY_FS_*).
// default ниже = ±2g/±2000dps — ровно то, что ставит LSM6DSO_Init().
float accSensitivityMg(qint32 fsG)
{
    switch (fsG) {
    case 4:  return 0.122f;
    case 8:  return 0.244f;
    case 16: return 0.488f;
    default: return 0.061f;   // 2g
    }
}
float gyroSensitivityMdps(qint32 fsDps)
{
    switch (fsDps) {
    case 125:  return 4.375f;
    case 250:  return 8.750f;
    case 500:  return 17.500f;
    case 1000: return 35.000f;
    default:   return 70.000f; // 2000dps — дефолт прошивки
    }
}

// Дробление шага стенда — задаётся DIP-переключателями на самом драйвере
// ШД, программно не управляется (см. CLAUDE.md «Стенд», поле «Дробление
// шага» намеренно убрано из UI 21.06.2026). Протоколу STEND_SPEED всё равно
// нужен байт коэффициента для расчёта частоты импульсов прошивкой Nucleo
// (f = speed*200*coef/60, coef = 1<<coefficient) — здесь зафиксирован
// константой, должен совпадать с физической установкой DIP на стенде.
// 32 микрошага (1<<5) — текущее значение по умолчанию на стенде.
constexpr quint8 kStendMicrostepCoef = 5;

// Емкость журнала во Flash (P25Q128H: 16 МБ / 256 байт на страницу),
// страница 0 — заголовок устройства, журнал начинается со страницы 1 (0x0100).
constexpr quint16 kLogStartPage    = 1;
constexpr quint32 kFlashTotalPages = 65536;
// Ограничение АВТО-дампа (вход на вкладку / после стирания): читаем не больше
// стольких страниц, чтобы вход был мгновенным и не завис на большом «Страниц»
// (17.07.2026 — при «Страниц»=65536 вход монтировал весь чип без остановки).
// Ручное «Прочитать» читает полный заданный диапазон, без ограничения.
constexpr int     kAutoDumpMaxPages = 64;
constexpr int     kRecordBytes     = 48;   // v2 (12.07.2026), было 24
constexpr int     kRecordsPerPage  = 5;    // v2: 48-байт записи, 5 на 256-байт страницу

// Реплика rtcToSec() прошивки (Data.c) — эпоха 2000, свой подсчёт високосных.
// Нужна, чтобы реконструированный HEX записи совпадал байт-в-байт с чтением.
static quint32 fwRtcToSec(const QDateTime &dt)
{
    static const int dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const int yy = dt.date().year() - 2000;
    const int isLeap = (yy % 4 == 0) ? 1 : 0;
    quint32 days = quint32(yy) * 365u;
    if (yy > 0) days += quint32((yy - 1) / 4) + 1u;
    for (int m = 1; m < dt.date().month(); ++m) {
        days += dpm[m - 1];
        if (m == 2 && isLeap) days += 1u;
    }
    days += quint32(dt.date().day()) - 1u;
    return days * 86400u + quint32(dt.time().hour()) * 3600u
         + quint32(dt.time().minute()) * 60u + quint32(dt.time().second());
}

// CRC16-CCITT (poly 0x1021, init 0xFFFF) — как ltp_crc16 в прошивке.
static quint16 fwCrc16(const quint8 *p, int n)
{
    quint16 crc = 0xFFFFu;
    for (int i = 0; i < n; ++i) {
        crc ^= quint16(p[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

// Реконструкция 48-байтной Flash-записи регистратора v2 (раскладка Data.c
// SaveParamOnEEPROM / data_format_spec_v1.md §«Структура записи v2»). Для
// ЖИВОГО hex-дампа: CMD_CYCLE_PUSH несёт только время+скорость, поэтому vib-
// каналы = 0 (в живом виде вибрация не приходит; реальные значения — при
// чтении Flash в «Данных»). rpm_max=rpm_avg=rpm (одно значение из пуша). LE.
static QByteArray fwBuildRecord(const QDateTime &startTs, quint32 durS,
                                quint32 totS, quint16 rpm)
{
    quint8 rec[kRecordBytes];
    memset(rec, 0, sizeof(rec));
    auto putU32 = [](quint8 *dst, quint32 v){ dst[0]=v; dst[1]=v>>8; dst[2]=v>>16; dst[3]=v>>24; };
    auto putF   = [&](int off, float f){ quint32 b; memcpy(&b,&f,4); putU32(rec+off,b); };
    putU32(rec + 0,  fwRtcToSec(startTs));
    putU32(rec + 4,  durS);
    putU32(rec + 8,  totS);
    putF (12, float(rpm));   // rpm_max
    putF (16, float(rpm));   // rpm_avg
    putF (20, 0.0f);         // vib1_peak (в живом пуше нет)
    putF (24, 0.0f);         // vib1_rms
    putF (28, 0.0f);         // vib2_peak
    putF (32, 0.0f);         // vib2_rms
    // rec[36] temp, rec[37] status = 0
    rec[38] = 2;             // rec_version
    rec[39] = 0x0A;          // variant_flags (A)
    // rec[40..45] reserved = 0
    const quint16 crc = fwCrc16(rec, 46);
    rec[46] = crc & 0xFF; rec[47] = crc >> 8; // CRC LE
    return QByteArray(reinterpret_cast<const char*>(rec), kRecordBytes);
}
// Регистратор запись v2 (48 байт): 5 записей на страницу (256/48=5, 16 байт
// хвоста — резерв). Было 10×24 до 12.07.2026, см. data_format_spec_v1.md.
constexpr int kRegRecordsPerPage = 5;
} // namespace

struct MainWindow::ThemePalette {
    QString bg, card, cardBorder, inputBorder, text, textDim, sectionText;
    QString menuHover, tabSelText, accent, factBg, logText;
};


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setupStatusBar();
    setupDashboardPlots();

	{
    QSettings st(kOrg, kApp);
    m_darkTheme = st.value(QStringLiteral("ui/darkTheme"), false).toBool();
}
setupThemeMenu();
applyTheme(m_darkTheme);
	
	

    // Гекс-дамп («Тест памяти» → «Прочитать») — моноширинный шрифт, чтобы все
    // символы были одной ширины и колонки слов выстраивались строго друг под
    // другом (запрос 20.06.2026). Установка ПОСЛЕ applyStyles() обязательна:
    // глобальное правило "QPlainTextEdit { ... }" в таблице стилей пересчитывает
    // эффективный шрифт виджета при setStyleSheet() и затирает setFont(),
    // установленный раньше (баг найден 20.06.2026 — пробелы между словами
    // «съедались» проп. шрифтом стиля, моноширинный не применялся).
    {
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(10);
        ui->txtHexDump->setFont(mono);
    }

    setupCore();
    setupStend();
    setupMonitor();
    memTestUpdateUi();   // начальное состояние Факт — пустые ячейки

    // Сегменты активации = операции, ЗАПОЛНЯЕМ СЛЕВА НАПРАВО по мере готовности
    // функций (17.07.2026). Каждый сегмент — кнопка (клик = его операция),
    // отдельных кнопок нет; подпись рисуется ВНУТРИ сегмента (ActivationBar).
    // 0 «Стереть память» (чип NOR) и 1 «Сброс перезапусков» (счётчики iflash) —
    // разные операции: стирание чипа счётчики НЕ трогает. 2..6 — пока без функции.
    ui->barActivation->setSectorCount(7);
    ui->barActivation->setSectorName(0, QStringLiteral("Стереть данные"));
    ui->barActivation->setSectorName(1, QStringLiteral("Сброс WDT"));   // счётчики перезапусков
    ui->barActivation->setSectorName(2, QStringLiteral("Синхро время"));
    // 3..6 — функция ещё не назначена (21.07.2026, по замечанию): серый
    // («заблокирован/неактивен»), не красный — чтобы не путать с «Idle».
    for (int i = 3; i < ui->barActivation->sectorCount(); ++i)
        ui->barActivation->setSectorState(i, ActivationBar::SectorState::Disabled);
    // Клик по сегменту — выполнить его операцию отдельно (17.07.2026).
    connect(ui->barActivation, &ActivationBar::sectorClicked,
            this, &MainWindow::onActivationSectorClicked);
    // Тест сторожа (18.07.2026): ДВОЙНОЙ КЛИК по строке «по таймеру» → 0x26
    // (отключить рефреш IWDG) → через ~32 c сброс → счётчик +1, моргание.
    ui->lblRestartTimer->installEventFilter(this);
    ui->lblRestartTimerCaption->installEventFilter(this);

    // Журналы: дашборд — 3 последних строки, вкладка «Лог» — лимит из настроек
    ui->logBrief->setMaximumBlockCount(3);
    ui->logMon->setMaximumBlockCount(3);   // «Мониторинг» — 3 строки (не мельтешит)
    ui->logView->setMaximumBlockCount(ui->spinLogLimit->value());
    // «Данные» — дашборд «регистратор в сервисе» (обзор архива/настройка перед
    // запуском), живой поток (IMU/пуши) сюда не по смыслу (07.07.2026, по
    // запросу). Убираем лог-окно с «Данных» — layout освободит место дашборду;
    // тот же лог остаётся на «Мониторинге» (logView) и на вкладке «Лог».
    // appendLog по-прежнему пишет в logBrief (скрыт) — безвредно.
    ui->logBrief->hide();
    connect(ui->spinLogLimit, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        ui->logView->setMaximumBlockCount(v);
    });
    connect(ui->btnLogClear, &QPushButton::clicked, this, [this] {
        ui->logView->clear();
        ui->logBrief->clear();
        ui->logMon->clear();
    });
    connect(ui->btnLogSave, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Сохранить лог"), {},
            QStringLiteral("Текстовые файлы (*.txt);;Все файлы (*)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            QTextStream(&f) << ui->logView->toPlainText();
    });

    // Режимы: оператор по умолчанию; ключ — этап 5, пока простой переключатель
    // Два прямых пункта меню: Данные ↔ Сервис
    connect(ui->actGoData, &QAction::triggered, this, [this] {
        setServiceMode(false);
    });
    connect(ui->actEngineerMode, &QAction::triggered, this, [this] {
        setServiceMode(true);
    });
    // «Сервис» — СПРАВА в менюбаре (19.07.2026, по запросу: над «Симуляцией»),
    // как corner-кнопка на тот же action (из левого ряда .ui убран).
    {
        auto *btnSvc = new QToolButton(this);
        btnSvc->setDefaultAction(ui->actEngineerMode);
        btnSvc->setAutoRaise(true);
        // Выровнять правый край с «Симуляция» ниже (21.07.2026, по просьбе):
        // corner-widget menubar сидит у самого края окна, а «Симуляция» —
        // внутри rootLayout с его правым отступом. Оборачиваем кнопку в
        // контейнер с тем же отступом справа (берём РЕАЛЬНОЕ значение из
        // rootLayout, а не подобранное число — не «уедет» на другом стиле/DPI).
        auto *svcWrap = new QWidget(this);
        auto *svcLay  = new QHBoxLayout(svcWrap);
        svcLay->setContentsMargins(0, 0, ui->rootLayout->contentsMargins().right(), 0);
        svcLay->addWidget(btnSvc);
        ui->menubar->setCornerWidget(svcWrap, Qt::TopRightCorner);
    }
    setServiceMode(false);

    connect(ui->actExit, &QAction::triggered, this, &QWidget::close);

    // Живые часы ПК на дашборде
    connect(&m_pcClockTimer, &QTimer::timeout, this, &MainWindow::tickPcClock);
    m_pcClockTimer.start(1000);
    tickPcClock();

    // Часы регистратора: опрос 0x1B раз в секунду в SERVICE (ТЗ v2 §2.4);
    // каждый 5-й тик — температура выбранного датчика (cmbTempSource)
    connect(&m_devClockTimer, &QTimer::timeout, this, [this] {
        // Не лезем в очередь, пока идёт стирание/запись образа — см. комментарий
        // у currentChanged выше (тот же класс бага с конкурирующими командами).
        // m_stendActive (03.07.2026): во время циклограммы регистратор ушёл
        // из Service в автомат (CMD_START_REGISTER) и бОльшую часть времени
        // спит в Stop2 — на LTP-запросы не отвечает, каждый опрос часов/
        // температур давал бы таймаут-спам в журнал. Часы «замрут» на время
        // теста — это честнее, чем ложные «нет ответа».
        // m_stendNoReg: циклограмма-без-регистратора (03.07) — устройство в
        // Service и отвечает, опросы не подавляем.
        if (!m_link->isOpen() || m_dev->busy() || m_imgActiveBtn
            || (m_stendActive && !m_stendNoReg))
            return;
        // Регистратор пропал в режиме A (стоп не дошёл) — редкий PING вместо
        // полного набора опросов: первый же ответ снимет m_regAwol в
        // onResponse(), и обычные опросы вернутся сами.
        if (m_regAwol) {
            static int awolTick = 0;
            if (++awolTick % 5 == 0)
                m_dev->enqueue(LtpCmd::PING);
            return;
        }
        m_dev->enqueue(LtpCmd::GET_DATETIME);
        static int tick = 0;
        if (++tick % 5 == 0) {
            if (ui->tabsMain->currentWidget() == ui->tabMemTest) {
                m_dev->enqueue(LtpCmd::GET_TEMP_TMP117);   // все 3 датчика
                m_dev->enqueue(LtpCmd::GET_TEMP_IMU);
                m_dev->enqueue(LtpCmd::GET_TEMP_STM);
            } else {
                m_dev->enqueue(dashboardTempCmd());
                m_dev->enqueue(LtpCmd::GET_TEMP_STM);   // STM32 temp + VDD
                // На «Данные» — ещё и GET_STATS раз в ~5 с: наработка/циклы/
                // перезапуски обновляются САМИ, без ухода-возврата на вкладку
                // (07.07.2026, по запросу: после щелчка питанием счётчик
                // подтягивается автоматически, регистратор через стенд не даёт
                // события переподключения). Счётчик меняется редко — 5 с с запасом.
                if (ui->tabsMain->currentWidget() == ui->tabDashboard)
                    m_dev->enqueue(LtpCmd::GET_STATS);
            }
        }
    });

    ui->btnInd->setText(QString()); // без текста — только цветной кружок-индикатор

    // «Симуляция» — демо-режим: заполняет интерфейс образцовыми данными,
    // чтобы показать, как программа выглядит с подключённым устройством и
    // результатами испытания. Обновлено 30.06.2026: теперь заполняет и
    // дашборд, и панель сравнения стенда (ранее — только cmpReport).
    connect(ui->chkSimulation, &QCheckBox::toggled, this, [this](bool on) {
        auto tighten = [](QPlainTextEdit *e) {
            QTextCursor c(e->document());
            c.select(QTextCursor::Document);
            QTextBlockFormat fmt;
            fmt.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            c.mergeBlockFormat(fmt);
        };
        if (on) {
            // ── Индикатор подключения ─────────────────────────────────────
            ui->btnInd->setStyleSheet(
                QStringLiteral("QPushButton#btnInd { background: #1D7A4C;"
                               " border: 1px solid #16613C; border-radius: 12px;"
                               " color: rgba(255,255,255,160); font-size: 12px; }"));
            ui->btnScan->setText(QStringLiteral("COM3"));
            ui->lblDevCard->clear();   // дубль-надпись убрана (18.07.2026)
            ui->lblWhoAmI->setText(QStringLiteral("0x6C (LSM6DSO)"));

            // ── Дашборд: часы устройства (синхронизированы) ───────────────
            const QDateTime now = QDateTime::currentDateTime();
            ui->lblDevDateTime->setText(now.toString(QStringLiteral("HH:mm:ss  dd/MM/yyyy")));
            ui->lblDevTime->setStyleSheet(QStringLiteral("color:#1D7A4C;"));
            ui->lblDevTime->setText(now.toString(QStringLiteral("HH:mm:ss")));
            ui->lblTimeDiff->setStyleSheet(QString());
            ui->lblTimeDiff->clear();

            // ── Дашборд: температуры и питание ───────────────────────────
            ui->lblTempCur->setText(QStringLiteral("23.4 °C"));
            ui->lblTempCurCaption->setText(QStringLiteral("текущая · LSM"));
            ui->lblTempMax->setText(QStringLiteral("31.7 °C"));
            ui->lblVdda->setText(QStringLiteral("3.6 В"));

            // ── Дашборд: наработка ────────────────────────────────────────
            ui->lblUptime->setText(QStringLiteral("012.37"));

            // ── Дашборд: журнал циклов ────────────────────────────────────
            ui->lblFirstDate->setText(QStringLiteral("23.06.2026"));
            ui->lblFirstTime->setText(QStringLiteral("09:14"));
            ui->lblLastDate->setText(now.toString(QStringLiteral("dd.MM.yyyy")));
            ui->lblLastTime->setText(now.toString(QStringLiteral("HH:mm")));
            ui->lblCyclesUsed->setText(QStringLiteral("1 248"));
            ui->lblCyclesFree->setText(QStringLiteral("63 951"));
            ui->lblMaxSpeed->setText(QStringLiteral("450 об/мин"));
            ui->lblMaxVibro->setText(QStringLiteral("2.3 g"));
            ui->lblMaxVibro2->setText(QStringLiteral("4.1 g"));
            ui->lblRestartTimer->setText(QStringLiteral("3"));
            ui->lblRestartPower->setText(QStringLiteral("12"));

            // ── Стенд: панель сравнения (оба столбца, последние 3 цикла;
            //    заголовок колонок — закреплённый lblCmpHeader) ─
            ui->cmpReport->setPlainText(
                QStringLiteral(
                "Старт     14:31:00     14:31:00\n"
                "Стоп      14:31:20     14:31:21\n"
                "Интервал  00:00:20     00:00:21     +1 секунда\n"
                "Общее     00:07:05     00:07:06     +1 секунда\n"
                "Скорость  180          179          −1 об/мин\n"
                "\n"
                "Старт     14:31:40     14:31:40\n"
                "Стоп      14:32:25     14:32:26\n"
                "Интервал  00:00:45     00:00:46     +1 секунда\n"
                "Общее     00:07:25     00:07:27     +2 секунды\n"
                "Скорость  250          248          −2 об/мин\n"
                "\n"
                "Старт     14:32:35     14:32:35\n"
                "Стоп      14:33:05     14:33:06\n"
                "Интервал  00:00:30     00:00:31     +1 секунда\n"
                "Общее     00:07:55     00:07:58     +3 секунды\n"
                "Скорость  320          317          −3 об/мин\n"
                "\n"
                "Старт     14:33:10     14:33:10\n"
                "Стоп      14:38:10     14:39:40\n"
                "Интервал  00:05:00     00:06:30     +90 секунд\n"
                "Общее     00:08:10     00:08:13     +3 секунды\n"
                "Скорость  400          397          −3 об/мин"));
            tighten(ui->cmpReport);

        } else {
            // ── Сброс: индикатор подключения ─────────────────────────────
            ui->btnInd->setStyleSheet(
                QStringLiteral("QPushButton#btnInd { background: #C03030;"
                               " border: 1px solid #9A2626; border-radius: 12px;"
                               " color: rgba(255,255,255,140); font-size: 12px; }"));
            ui->btnScan->setText(QStringLiteral("ВКЛ"));
            ui->lblDevCard->setText(QStringLiteral("LogLSMW · сервис регистратора"));
            ui->lblWhoAmI->clear();

            // ── Сброс: дашборд ────────────────────────────────────────────
            ui->lblDevDateTime->clear();
            ui->lblDevTime->setStyleSheet(QString());
            ui->lblDevTime->clear();
            ui->lblTimeDiff->setStyleSheet(QString());
            ui->lblTimeDiff->clear();
            ui->lblTempCur->clear();
            ui->lblTempCurCaption->setText(QStringLiteral("текущая"));
            ui->lblTempMax->clear();
            ui->lblVdda->clear();
            ui->lblUptime->clear();
            ui->lblFirstDate->clear(); ui->lblFirstTime->clear();
            ui->lblLastDate->clear();  ui->lblLastTime->clear();
            ui->lblCyclesUsed->clear();
            ui->lblCyclesFree->clear();
            ui->lblMaxSpeed->clear();
            ui->lblMaxVibro->clear();
            ui->lblMaxVibro2->clear();
            ui->lblRestartTimer->clear();
            ui->lblRestartPower->clear();

            // ── Сброс: стенд ──────────────────────────────────────────────
            ui->cmpReport->clear();
        }
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ── Ядро: ComLink + DeviceController + PortScanner (этап 2) ────────────────

void MainWindow::setupCore()
{
    m_link    = new ComLink(this);
    m_dev     = new DeviceController(m_link, this);
    m_scanner = new PortScanner(this);

    m_dev->setTargetAddr(quint8(ui->spinDevAddr->value()));
    m_dev->setTimeout(ui->spinTimeout->value());
    m_dev->setRetries(ui->spinRetries->value());

    connect(m_link, &ComLink::linkLost, this, &MainWindow::onLinkLost);

    connect(m_dev, &DeviceController::responseReady, this, &MainWindow::onResponse);
    connect(m_dev, &DeviceController::errorReceived, this, &MainWindow::onProtoError);
    connect(m_dev, &DeviceController::requestFailed, this, &MainWindow::onRequestFailed);
    connect(m_dev, &DeviceController::countersChanged, this, &MainWindow::onCounters);
    // Unsolicited push от регистратора (BENCH-режим: CMD_CYCLE_PUSH 0x20)
    connect(m_dev, &DeviceController::unsolicitedFromReg,
            this, [this](quint8 cmd, QByteArray payload) {
        if (cmd == LtpCmd::CYCLE_PUSH)
            stendFillRegColumn(payload);
        else if (cmd == LtpCmd::SUBSPEED_PUSH)
            stendFillSubSpeeds(payload);
        else if (cmd == LtpCmd::WDG_KICK) {
            m_rstTimerBlinkLeft = 2;   // kick-метка → короткое моргание «по таймеру»
            appendLog(QStringLiteral("[RX] kick 0x25 — сторож поглажен (RTC)"));
            // Вспышка в ШАПКЕ (видна с любой вкладки, 18.07.2026): индикатор
            // формата «● …» на ~1.5 c загорается ярко-зелёным «● сторож», затем
            // возвращается к обычному виду.
            ui->lblDataFormat->setText(QStringLiteral("● сторож сброс"));
            ui->lblDataFormat->setStyleSheet(
                QStringLiteral("color:#2ECC71; font-weight:700;"));
            QTimer::singleShot(1500, this, [this] { refreshImgButtonsHighlight(); });
        }
    });

    connect(m_scanner, &PortScanner::portChecked, this, &MainWindow::onScanPortChecked);
    connect(m_scanner, &PortScanner::finished, this, &MainWindow::onScanFinished);

    connect(ui->btnScan, &QPushButton::clicked, this, &MainWindow::onScanClicked);

    // Авторетрай: не нашли — через 5 с сканируем снова (однократный singleShot)
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (!m_link->isOpen() && !m_scanner->isScanning())
            onScanClicked();
    });

    // Автоскан при запуске (после event loop — чтобы UI успел показаться)
    QTimer::singleShot(0, this, &MainWindow::onScanClicked);

    // Вкладка «Команды» — через очередь DeviceController
    connect(ui->btnPing, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::PING);
        appendLog(QStringLiteral("[TX] PING"));
    });
    connect(ui->btnWhoAmI, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::WHO_AM_I);
        appendLog(QStringLiteral("[TX] WHO_AM_I"));
    });
    // Кнопка «⟳» рядом с версией ПО на «Данные» (13.07.2026): WHO_AM_I шлётся
    // автоматически только один раз при подключении — без этой кнопки индикатор
    // версии не видел бы перепрошивку без полного переподключения/рестарта.
    connect(ui->btnRefreshFwVersion, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::WHO_AM_I);
        appendLog(QStringLiteral("[TX] WHO_AM_I (обновление версии ПО)"));
    });
    connect(ui->btnGetTime, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::GET_DATETIME, {}, TagSyncTime);
        appendLog(QStringLiteral("[TX] GET_DATETIME"));
    });
    // «⌚» на «Стенде» — та же синхронизация, не ходя в «Команды» (18.07.2026).
    connect(ui->btnStendSyncTime, &QPushButton::clicked,
            ui->btnSyncTime, &QPushButton::click);
    connect(ui->btnSyncTime, &QPushButton::clicked, this, [this] {
        // Синхронизация ПО ГРАНИЦЕ СЕКУНДЫ (18.07.2026): раньше слали целые
        // секунды в произвольный момент внутри секунды ПК → погрешность
        // установки до ~1 c (субсекунды RTC обнуляются). Теперь ждём смены
        // секунды и шлём время СЛЕДУЮЩЕЙ секунды ровно на её границе —
        // остаётся только задержка UART (~мс).
        const int msToEdge = 1000 - QTime::currentTime().msec();
        QTimer::singleShot(msToEdge, this, [this] {
            const QDateTime now = QDateTime::currentDateTime();
            QByteArray p;
            p.append(char(now.date().year() - 2000));
            p.append(char(now.date().month()));
            p.append(char(now.date().day()));
            p.append(char(now.time().hour()));
            p.append(char(now.time().minute()));
            p.append(char(now.time().second()));
            requestCmd(LtpCmd::SET_DATETIME, p);
            requestCmd(LtpCmd::GET_DATETIME, {}, TagSyncTime);   // контрольное чтение
            appendLog(QStringLiteral("[TX] SET_DATETIME ← время ПК (по границе секунды)"));
        });
    });
    connect(ui->btnImuRaw, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::GET_AXES_RAW, {}, TagManual);
        appendLog(QStringLiteral("[TX] GET_AXES_RAW"));
    });
    connect(ui->btnTempLsm, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::GET_TEMP_IMU, {}, TagManual);
        appendLog(QStringLiteral("[TX] GET_TEMP_IMU"));
    });
    connect(ui->btnTempTmp117, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::GET_TEMP_TMP117, {}, TagManual);
        appendLog(QStringLiteral("[TX] GET_TEMP TMP117"));
    });
    connect(ui->btnTempStm, &QPushButton::clicked, this, [this] {
        requestCmd(LtpCmd::GET_TEMP_STM, {}, TagManual);
        appendLog(QStringLiteral("[TX] GET_TEMP_CHIP STM32"));
    });

    // Скорость: последняя использованная, стартовый default 921600 — штатная
    // скорость прошивок Stend/Регистратор с 22.06.2026 (см. CLAUDE.md).
    ui->cmbBaud->setCurrentText(QSettings(kOrg, kApp)
        .value(QStringLiteral("connection/baud"), QStringLiteral("921600")).toString());
    connect(ui->cmbBaud, &QComboBox::currentTextChanged, this, [](const QString &b) {
        QSettings(kOrg, kApp).setValue(QStringLiteral("connection/baud"), b);
    });

    // Датчик температуры дашборда: запоминаем выбор, при смене сразу опрашиваем
    ui->cmbTempSource->setCurrentIndex(QSettings(kOrg, kApp)
        .value(QStringLiteral("dashboard/tempSource"), 0).toInt());
    connect(ui->cmbTempSource, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        QSettings(kOrg, kApp).setValue(QStringLiteral("dashboard/tempSource"), idx);
        ui->lblTempCur->setText(QStringLiteral("— °C"));
        if (m_link->isOpen())
            m_dev->enqueue(dashboardTempCmd());   // мгновенно подхватить новый датчик
    });

    // ── Вкладка «Тест памяти»: базовые операции Flash (тег TagManual) ──────
    // Поле байта: только HEX без префикса, всегда заглавные.
    // Доп. к латинским A-F разрешены кириллические буквы с тех же физических
    // клавиш в раскладке ЙЦУКЕН (A->Ф, B->И, C->С, D->В, E->У, F->А) —
    // замечено 22.06.2026: если забыл переключить раскладку на английскую,
    // буквы просто не вводились (валидатор их отбрасывал, цифры проходили).
    // Теперь принимаются и автоматически заменяются на латинский эквивалент
    // по той же клавише.
    ui->editTestByte->setMaxLength(2);
    ui->editTestByte->setPlaceholderText(QStringLiteral("A5"));
    ui->editTestByte->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9A-Fa-fАФВЕСИафвеси]{0,2}")), ui->editTestByte));
    connect(ui->editTestByte, &QLineEdit::textEdited, this, [this](const QString &t) {
        // Кириллица -> латиница по физической клавише раскладки ЙЦУКЕН
        // (клавиша A -> Ф, B -> И, C -> С, D -> В, E -> У, F -> А).
        static const QMap<QChar, QChar> kCyrillicHexKey = {
            {QChar(u'Ф'), QChar('A')}, {QChar(u'И'), QChar('B')},
            {QChar(u'С'), QChar('C')}, {QChar(u'В'), QChar('D')},
            {QChar(u'У'), QChar('E')}, {QChar(u'А'), QChar('F')},
        };
        QString fixed = t.toUpper();
        for (QChar &c : fixed) {
            const auto it = kCyrillicHexKey.constFind(c);
            if (it != kCyrillicHexKey.constEnd())
                c = it.value();
        }
        if (fixed != t) {
            ui->editTestByte->setText(fixed);
            ui->editTestByte->setCursorPosition(fixed.length());
        }
    });

    auto testByte = [this]() -> quint8 {
        bool ok = false;
        const int v = ui->editTestByte->text().trimmed().toInt(&ok, 16);
        return ok ? quint8(v & 0xFF) : 0x00;
    };
    auto memLog = [this](const QString &t) { ui->memReport->appendPlainText(t); };

    connect(ui->btnMemReadImg, &QPushButton::clicked, this, [this] {
        // Ручное «Прочитать» — читаем ПОЛНЫЙ заданный диапазон «Старт»/«Страниц»
        // (явный override, без ограничения kAutoDumpMaxPages, которое действует
        // только на авто-дамп при входе/после стирания). (17.07.2026)
        memTestDump(ui->spinMemStartPage->value(), qMax(1, ui->spinMemPages->value()));
    });
    // Кнопки стирания: подпись без номера страницы (адрес задаётся в параметрах)
    ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница"));
    ui->btnMemEraseSector->setText(QStringLiteral("⚠ Сектор"));
    ui->btnMemEraseChip->setText(QStringLiteral("⚠ Чип"));

    connect(ui->btnMemErasePage, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        // Идёт серия — второе нажатие ОСТАНАВЛИВАЕТ (17.07.2026). Раньше отмены
        // не было: случайный большой диапазон (термотест tempRunStart оставляет
        // «Страниц»=65536) вешал очередь на десятки тысяч поштучных команд без
        // выхода — «кнопка зависла».
        if (m_pageEraseLeft > 0) {
            m_dev->clearQueue();
            m_pageEraseLeft = 0;
            m_pageEraseErr  = 0;
            ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница"));
            memLog(QStringLiteral("— стирание страниц остановлено —"));
            setOpsEnabled(true);
            return;
        }
        // Стираем «Страниц» страниц подряд от стартовой (02.07.2026 — раньше
        // стиралась только стартовая, поле «Страниц» игнорировалось; та же
        // семантика диапазона, что у «Запись»/«Чтение»: start..start+n-1).
        const quint16 start = quint16(ui->spinMemStartPage->value());
        const int     n     = qMax(1, ui->spinMemPages->value());
        // Предохранитель от поштучного стирания огромного объёма (термотест
        // оставляет «Страниц»=65536 → тысячи медленных команд = зависание).
        // Для большого объёма правильный инструмент — «Чип» (одна команда) или
        // «Сектор» (16 стр.). Порог 256 стр. (16 секторов).
        constexpr int kPageEraseWarn = 256;
        if (n > kPageEraseWarn) {
            const auto r = QMessageBox::warning(this,
                QStringLiteral("Стирание страниц"),
                QStringLiteral("Будет стёрто %1 страниц ПО ОДНОЙ (%1 команд) — "
                    "это долго. Для большого объёма используйте «Чип» (одна "
                    "команда) или «Сектор» (16 стр.). Продолжить поштучно?").arg(n),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (r != QMessageBox::Yes) return;
        }
        m_pageEraseLeft  = n;
        m_pageEraseTotal = n;
        m_pageEraseErr   = 0;
        ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница ■"));
        memLog(n == 1
            ? QStringLiteral("[Стирание страницы %1]").arg(start)
            : QStringLiteral("[Стирание страниц %1..%2]")
                  .arg(start).arg(start + n - 1));
        // Кнопку оставляем живой — для остановки серии (стоп-нажатие выше).
        setOpsEnabled(false, ui->btnMemErasePage);
        for (int i = 0; i < n; ++i) {
            const quint16 pg = quint16(start + i);
            QByteArray p;
            p.append(char(pg & 0xFF)); p.append(char((pg >> 8) & 0xFF));
            requestCmd(LtpCmd::FLASH_PAGE_ERASE, p, TagManual);
        }
    });
    connect(ui->btnMemEraseChip, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (QMessageBox::question(this, QStringLiteral("Стереть чип"),
                QStringLiteral("Стереть ВСЮ память? Операция до ~30 с."))
            != QMessageBox::Yes) return;
        memLog(QStringLiteral("[Стирание чипа…]"));
        m_eraseStartMs = QDateTime::currentMSecsSinceEpoch();
        setOpsEnabled(false);
        requestCmd(LtpCmd::FLASH_ERASE, {}, TagManual);
    });
    connect(ui->btnMemEraseSector, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        const quint16 sec = quint16(ui->spinMemStartPage->value() / 16);
        QByteArray p; p.append(char(sec & 0xFF)); p.append(char((sec >> 8) & 0xFF));
        memLog(QStringLiteral("[Стирание сектора %1 (стр. %2–%3)]")
                   .arg(sec).arg(sec * 16).arg(sec * 16 + 15));
        setOpsEnabled(false);
        requestCmd(LtpCmd::FLASH_SECTOR_ERASE, p, TagManual);
    });
    // Запись: первое нажатие — старт, повторное — стоп
    connect(ui->btnMemWrite, &QPushButton::clicked, this, [this, memLog, testByte] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_test.running && m_test.step == TestStep::Write) {
            // ── СТОП ──
            m_dev->clearQueue();
            m_test.running = false;
            m_test.step    = TestStep::Idle;
            ui->btnMemWrite->setText(QStringLiteral("Запись"));
            memLog(QStringLiteral("— запись остановлена (%1 из %2 стр.) —")
                       .arg(m_test.pagesDone).arg(m_test.pageTotal * m_test.cycleTotal));
            setOpsEnabled(true);
            memTestUpdateUi();
            return;
        }
        // ── СТАРТ ──
        const quint16 start  = quint16(ui->spinMemStartPage->value());
        const int     n      = ui->spinMemPages->value();
        const int     cycles = ui->spinMemCycles->value();
        const quint8  b      = testByte();
        m_test = { true, n, 0, start, 0, TestStep::Write, cycles, 0 };
        ui->btnMemWrite->setText(QStringLiteral("Запись ■"));
        memLog(QStringLiteral("[Запись] стр.%1..%2 <- 0x%3 x256 (%4 стр. × %5 цикл.)")
                   .arg(start).arg(start + n - 1)
                   .arg(b, 2, 16, QLatin1Char('0')).arg(n).arg(cycles));
        setOpsEnabled(false, ui->btnMemWrite);
        memTestUpdateUi();
        for (int c = 0; c < cycles; ++c)
            for (int i = 0; i < n; ++i) {
                const quint16 pg = quint16(start + i);
                QByteArray p;
                p.append(char(pg & 0xFF)); p.append(char((pg >> 8) & 0xFF));
                p.append(QByteArray(256, char(b)));
                m_dev->enqueue(LtpCmd::FLASH_WRITE, p, TagManual);
            }
    });
    // Чтение: первое нажатие — старт, повторное — стоп
    connect(ui->btnMemRead, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_test.running && m_test.step == TestStep::Read) {
            // ── СТОП ──
            m_dev->clearQueue();
            m_test.running = false;
            m_test.step    = TestStep::Idle;
            ui->btnMemRead->setText(QStringLiteral("Чтение"));
            memLog(QStringLiteral("— чтение остановлено (%1 из %2 стр.) —")
                       .arg(m_test.pagesDone).arg(m_test.pageTotal * m_test.cycleTotal));
            setOpsEnabled(true);
            memTestUpdateUi();
            return;
        }
        // ── СТАРТ ──
        const quint16 start  = quint16(ui->spinMemStartPage->value());
        const int     n      = ui->spinMemPages->value();
        const int     cycles = ui->spinMemCycles->value();
        m_test = { true, n, 0, start, 0, TestStep::Read, cycles, 0 };
        ui->btnMemRead->setText(QStringLiteral("Чтение ■"));
        memLog(QStringLiteral("[Чтение] стр.%1..%2 (%3 стр. × %4 цикл.)")
                   .arg(start).arg(start + n - 1).arg(n).arg(cycles));
        setOpsEnabled(false, ui->btnMemRead);
        memTestUpdateUi();
        for (int c = 0; c < cycles; ++c)
            for (int i = 0; i < n; ++i) {
                const quint32 addr = quint32(start + i) << 8;
                QByteArray p;
                for (int j = 0; j < 4; ++j) p.append(char((addr >> (8*j)) & 0xFF));
                for (int j = 0; j < 4; ++j) p.append(char((256   >> (8*j)) & 0xFF));
                m_dev->enqueue(LtpCmd::FLASH_READ, p, TagManual);
            }
    });

    // ── Образ RG / Образ LOG: запись образа в Flash из файла Intel HEX ───────
    // Раньше эти кнопки генерировали образ синтетически прямо в коде C++, по
    // устаревшему формату (30 байт/запись RG, block-float LOG) — он давно
    // расходится со спекой data_format_spec_v1.md (Регистратор/Logger v2).
    // Теперь единственный источник истины — файл (actual/test_dumps/*.hex):
    // правки формата требуют пересборки дампа, а не кода вкладки.
    auto loadImageButton = [this, memLog](QPushButton *btn, const QString &caption,
                                           const QString &suggestedFile) {
        if (m_imgActiveBtn) { memLog(QStringLiteral("⚠ Образ: операция уже выполняется")); return; }
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }

        QSettings st(kOrg, kApp);
        const QString lastDir = st.value(QStringLiteral("memtest/lastImgDir"),
                                          QStringLiteral("actual/test_dumps")).toString();
        const QString defaultPath = QDir(lastDir).filePath(suggestedFile);
        const QString path = QFileDialog::getOpenFileName(
            this, caption, defaultPath,
            QStringLiteral("Intel HEX (*.hex *.ihex);;Все файлы (*)"));
        if (path.isEmpty()) return;   // диалог отменён
        st.setValue(QStringLiteral("memtest/lastImgDir"), QFileInfo(path).absolutePath());

        quint16 startPage = 0;
        QList<QByteArray> pages;
        QString err;
        if (!loadImageFromHexFile(path, startPage, pages, err)) {
            memLog(QStringLiteral("⚠ Образ: %1 — %2").arg(QFileInfo(path).fileName(), err));
            return;
        }
        // Запись образа не подразумевает сохранение старых данных — сначала
        // стираем весь чип целиком (решение 20.06.2026: старые данные на чипе
        // путали разбор архива — см. лишние «оборванные» записи 20.06.2026).
        // Параметры записи сохраняются, startImageWrite() запустится из
        // обработчика FLASH_STATE (тег TagImg) по готовности стирания.
        m_imgActiveBtn        = btn;   // блокирует повторный клик на время стирания+записи
        m_imgPendingBtn       = btn;
        m_imgPendingLabel     = btn->text();
        m_imgPendingStartPage = startPage;
        m_imgPendingPages     = pages;
        m_imgPendingFileName  = QFileInfo(path).fileName();
        // Индикация операции в верхних ячейках (02.07.2026, по запросу):
        // «Задать» — план из файла (страниц/стартовая), ставится сразу;
        // «Факт» — очищается и заполняется по ходу реальной записи
        // (см. обработчик FLASH_WRITE/TagImg): счётчик записанных страниц
        // растёт живьём, по завершении — итоговые значения (как и раньше,
        // их же использует «Прочитать» как диапазон).
        ui->lblImgPagesNA->setText(QString::number(pages.size()));
        ui->lblImgAddrNA->setText(QString::number(startPage));
        ui->lblImgPages->setText(QStringLiteral(" "));
        ui->lblImgAddr->setText(QStringLiteral(" "));
        setOpsEnabled(false);
        btn->setText(QStringLiteral("⌛ стирание…"));
        m_eraseStartMs = QDateTime::currentMSecsSinceEpoch();
        appendLog(QStringLiteral(
            "[Образ] %1 — сначала стираю весь чип (без сохранения старых данных), "
            "затем запись стр.%2..%3 (%4 стр.)")
                .arg(m_imgPendingFileName)
                .arg(startPage)
                .arg(startPage + pages.size() - 1)
                .arg(pages.size()));
        requestCmd(LtpCmd::FLASH_ERASE, {}, TagImg);
    };

    connect(ui->btnImgRG, &QPushButton::clicked, this, [this, loadImageButton] {
        loadImageButton(ui->btnImgRG, QStringLiteral("Образ RG — выбрать файл (.hex)"),
                         QStringLiteral("registrator_dump.hex"));
    });
    connect(ui->btnImgLOG, &QPushButton::clicked, this, [this, loadImageButton] {
        loadImageButton(ui->btnImgLOG, QStringLiteral("Образ LOG — выбрать файл (.hex)"),
                         QStringLiteral("logger_dump.hex"));
    });

    // Термотест
    connect(ui->btnTempRun, &QPushButton::clicked, this, [this, memLog] {
        if (m_tempRun.running) { tempRunStop(); return; }
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        tempRunStart();
    });
    m_tempPollTimer.setSingleShot(false);
    connect(&m_tempPollTimer, &QTimer::timeout, this, [this] {
        // Мигаем «градусником» на кнопке — видно, что термотест ЖИВ и ждёт
        // следующего замера температуры (17.07.2026, по запросу). Таймер
        // крутится только пока идёт прогон (старт/стоп управляют им).
        m_tempBlink = !m_tempBlink;
        // На паузе (температура вне [МИН..МАКС] — например, ещё не остыли ниже
        // МАКС) операции НЕ идут: мигаем «⏳» вместо «🌡», чтобы это было видно
        // (17.07.2026). В рабочем диапазоне — «🌡» (идут запись/сверка).
        const QString g = m_tempRun.halted ? QStringLiteral("⏳")
                                           : QStringLiteral("🌡");
        ui->btnTempRun->setText(m_tempBlink ? (g + QStringLiteral(" Стоп"))
                                            : QStringLiteral("·  Стоп"));
        requestCmd(tempRunTempCmd());  // фоновый опрос температуры
    });

    // ── Кнопка активации ────────────────────────────────────────────────────
    connect(ui->btnActivate, &QPushButton::clicked, this, [this] {
        // Симуляция (17.07.2026): при галочке «Симуляция» ► заливает ВСЮ шкалу
        // зелёным по шагам — демонстрация полного цикла активации без реальных
        // операций с устройством. Реальная активация — при снятой галочке.
        if (ui->chkSimulation->isChecked()) {
            ui->barActivation->reset();
            ui->lblActStatus->setText(QStringLiteral("Активация (симуляция)…"));
            const int nSec = ui->barActivation->sectorCount();
            for (int i = 0; i < nSec; ++i)
                QTimer::singleShot(250 * (i + 1), this, [this, i, nSec] {
                    activationSetSector(i, ActivationBar::SectorState::Done);
                    if (i == nSec - 1)
                        ui->lblActStatus->setText(QStringLiteral("Активировано (симуляция)"));
                });
            return;
        }
        if (m_act.step == ActStep::Idle
                || m_act.step == ActStep::Done
                || m_act.step == ActStep::Error)
            activationStart();
        else
            activationStop();
    });

    // Колонки 1 и 2 в groupMemParams — одинаковая ширина
    // Ограничиваем виджеты col-1, иначе спинбоксы растягиваются и «съедают» col-2
    for (QSpinBox *sb : ui->groupMemParams->findChildren<QSpinBox*>())
        sb->setMaximumWidth(90);
    ui->editTestByte->setMaximumWidth(90);
    ui->memParamsGrid->setColumnStretch(1, 1);
    ui->memParamsGrid->setColumnStretch(2, 1);

    // При открытии вкладки «Тест памяти» — включить питание Flash (0x0E) и
    // проверить, что сейчас лежит в Flash, чтобы подсветить кнопку «Образ
    // RG/LOG», соответствующую уже загруженному образу (а не только что
    // записанному в этой же сессии — см. probeFlashImageState).
    connect(ui->tabsMain, &QTabWidget::currentChanged, this, [this] {
        // Пока идёт стирание/запись образа (m_imgActiveBtn) — не лезем своими
        // FLASH_ON/архивными запросами в очередь поперёк: на медленном
        // FLASH_ERASE (см. devicecontroller.h: «долго!») переключение вкладок
        // раньше забивало очередь конкурирующими командами, и опрос готовности
        // стирания эффективно зависал (баг найден 20.06.2026).
        if (m_imgActiveBtn) return;
        if (m_link->isOpen() && ui->tabsMain->currentWidget() == ui->tabMemTest) {
            m_dev->enqueue(LtpCmd::FLASH_ON, {}, TagManual);
            probeFlashImageState();
            // Авто-обновление гекс-панели при входе (17.07.2026, по запросу):
            // всегда показываем РЕАЛЬНОЕ текущее состояние Flash, а не осевший
            // рендер прошлого «Прочитать» (панель не «живая», отсюда путаница —
            // старый дамп принимали за текущий). Сначала освежаем границы
            // (двоичный поиск), по его завершении flashBinSearchSendNext()
            // дёрнет memTestDump(). Не во время стенд-прогона (устройство спит/
            // пишет) и не поверх уже идущего чтения. «Прочитать» вручную остаётся
            // как есть — это просто дополнительное чтение.
            if (!m_stendActive && !m_test.running) {
                m_memAutoDumpPending = true;
                flashBinSearchStart();
                // Границы уже известны и новый поиск не стартовал — дампим сразу.
                if (!m_binSearch.running && m_firstFreePage >= 0) {
                    m_memAutoDumpPending = false;
                    memTestDump();
                }
            }
        }
        // При открытии вкладки «Данные» — перечитать журнал из Flash, а не
        // показывать кэш последнего разбора (актуально после стирания/записи
        // другого образа, сделанных пока вкладка была не активна). Питание
        // Flash включаем на всякий случай и здесь — если на «Данные» перешли
        // напрямую, не заходя на «Тест памяти», флеш может быть ещё не включен.
        if (m_link->isOpen() && ui->tabsMain->currentWidget() == ui->tabDashboard) {
            m_dev->enqueue(LtpCmd::FLASH_ON, {}, TagManual);
            // Обновляем инфу дашборда при входе на «Данные» (07.07.2026, по
            // запросу): иначе после потери питания/перешивки счётчики/наработка
            // висели на старом до рестарта W. ТОЛЬКО ЧТЕНИЕ. Часы (GET_DATETIME)
            // сюда НЕ добавляем — их и так читает секундный таймер, а установка
            // (SET_DATETIME) остаётся РУЧНОЙ командой («Синхронизировать»).
            m_dev->enqueue(dashboardTempCmd());
            m_dev->enqueue(LtpCmd::GET_TEMP_STM);   // VDD
            m_dev->enqueue(LtpCmd::GET_STATS);      // наработка/циклы/перезапуски
            archiveRescanFull();
        }
        // Панель «Память устройства» живёт на вкладке «Стенд» — при входе
        // на неё освежить занятость (03.07.2026: раньше поиск триггерился
        // только с «Тест памяти»/«Данные», панель показывала устаревшее —
        // например «0 занято» от стирания перед записью образа). Во время
        // циклограммы flashBinSearchStart сам откажется (m_stendActive).
        if (m_link->isOpen() && ui->tabsMain->currentWidget() == ui->tabStend) {
            m_dev->enqueue(LtpCmd::FLASH_ON, {}, TagManual);
            probeFlashImageState();   // «Содержимое» в панели «Память устройства»
            flashBinSearchStart();
        }
    });

    // Настройки → ядро (живое применение)
    connect(ui->spinDevAddr, &QSpinBox::valueChanged, this, [this](int v) {
        m_dev->setTargetAddr(quint8(v));
        lblAddr->setText(QStringLiteral("ADDR 0x%1")
                             .arg(QString::number(v, 16).rightJustified(2, QLatin1Char('0')).toUpper()));
    });
    connect(ui->spinTimeout, &QSpinBox::valueChanged, m_dev, &DeviceController::setTimeout);
    connect(ui->spinRetries, &QSpinBox::valueChanged, m_dev, &DeviceController::setRetries);

    setConnectedUi(false);
}

qint32 MainWindow::selectedBaud() const
{
    return ui->cmbBaud->currentText().toInt();
}

quint8 MainWindow::dashboardTempCmd() const
{
    switch (ui->cmbTempSource->currentIndex()) {
    case 1:  return LtpCmd::GET_TEMP_TMP117;
    case 2:  return LtpCmd::GET_TEMP_STM;
    default: return LtpCmd::GET_TEMP_IMU;
    }
}

// Команда из UI: нет связи -> автоподключение, выполнить после (забывчивость).
void MainWindow::requestCmd(quint8 cmd, const QByteArray &payload, quint32 tag)
{
    if (m_link->isOpen()) {
        m_dev->enqueue(cmd, payload, tag);
        return;
    }
    m_pendingCmds.append({cmd, payload, tag});
    if (!m_scanner->isScanning()) {
        appendLog(QStringLiteral("Нет связи — автоподключение…"));
        onScanClicked();
    }
}

void MainWindow::onScanClicked()
{
    if (m_link->isOpen()) {                      // повторное нажатие = отключение
        m_link->close();
        setConnectedUi(false);
        appendLog(QStringLiteral("Отключено"));
        return;
    }
    if (m_scanner->isScanning()) {
        // Явный клик во время автоскана — отмена, не перезапуск.
        // Кнопка не блокируется (setEnabled(false) убран ниже), поэтому
        // пользователь всегда может остановить затянувшийся скан.
        m_scanner->stop();
        m_retryTimer.stop();
        ui->btnScan->setEnabled(true);
        ui->btnScan->setText(QStringLiteral("ВКЛ"));
        return;
    }
    m_retryTimer.stop();     // явный клик сбрасывает таймер авторетрая

    const QString pinned = QSettings(kOrg, kApp)
                               .value(QStringLiteral("connection/pinnedPort")).toString();
    // setEnabled(false) убран: кнопка остаётся кликабельной во время скана
    // и служит кнопкой отмены (ветка isScanning выше).
    ui->btnScan->setText(QStringLiteral("…"));
    appendLog(QStringLiteral("Поиск устройства (PING 0x%1 / 0x%2, %3 бод)…")
                  .arg(m_dev->targetAddr(), 2, 16, QLatin1Char('0'))
                  .arg(LtpAddr::STEND, 2, 16, QLatin1Char('0'))
                  .arg(selectedBaud()));
    // Пингуем и адрес регистратора, и адрес стенда (0x8C) — порт находится,
    // если ответил ХОТЯ БЫ ОДИН из двух (см. PortScanner::start, altAddr).
    // Нужно для случая, когда регистратор физически отключён от линии и
    // остался только стенд — без этого автоподключение никогда не находило
    // порт (см. CLAUDE.md «Стенд», ограничение замечено 21.06.2026).
    // pinnedRetries=15, pinnedTimeoutMs=1000 — порт остаётся открытым ~15с
    // подряд, без закрытия между попытками. Это предотвращает сброс устройства
    // через ST-Link VCP (каждое open/close может пульсировать NRST), давая
    // контроллеру время завершить инициализацию (tmp117Activ/framActiv) и
    // ответить на PING. Раньше было 2×500мс = 1.5с, этого не хватало.
    m_scanner->start(selectedBaud(), m_dev->targetAddr(), pinned,
                      200, 1000, 15, LtpAddr::STEND);
}

void MainWindow::onScanPortChecked(const QString &port, PortScanner::Result result)
{
    switch (result) {
    case PortScanner::Result::Found:
        appendLog(QStringLiteral("%1 — найдено устройство").arg(port));
        break;
    case PortScanner::Result::Busy:
        appendLog(QStringLiteral("%1 — занят другой программой").arg(port));
        break;
    case PortScanner::Result::NoReply: {
        const quint32 rx  = m_scanner->bytesReceived();
        const quint32 crc = m_scanner->crcErrors();
        QString detail;
        if (rx > 0)
            detail = QStringLiteral(" (rx %1 б, CRC-ош %2)").arg(rx).arg(crc);
        appendLog(QStringLiteral("%1 — нет ответа%2").arg(port, detail));
        break;
    }
    }
}

void MainWindow::onScanFinished(const QString &foundPort)
{
    ui->btnScan->setEnabled(true);

    if (foundPort.isEmpty()) {
        ui->btnScan->setText(QStringLiteral("ВКЛ"));
        setConnectedUi(false);
        if (!m_pendingCmds.isEmpty()) {
            m_pendingCmds.clear();
            appendLog(QStringLiteral("Устройство не найдено — команда отменена"));
        }
        // Авторетрай через 1 с (было 5 с — слишком долго ждать после
        // провального 16-секундного скана: устройство успевает загрузиться
        // за время этого скана, второй проход находит его сразу).
        m_retryTimer.start(1000);
        return;
    }

    if (!m_link->open(foundPort, selectedBaud())) {
        ui->btnScan->setText(QStringLiteral("ВКЛ"));
        setConnectedUi(false);
        appendLog(QStringLiteral("Не удалось открыть %1").arg(foundPort));
        return;
    }

    m_retryTimer.stop();
    QSettings(kOrg, kApp).setValue(QStringLiteral("connection/pinnedPort"), foundPort);
    setConnectedUi(true, foundPort);
    m_lastRegTotS = 0;   // сброс: новое подключение = новый r->totalSec с нуля
    appendLog(QStringLiteral("Подключено: %1 · %2").arg(foundPort).arg(selectedBaud()));

    // Паспорт устройства (начало; полный алгоритм §2.6 — этап 3)
    m_dev->enqueue(LtpCmd::WHO_AM_I);
    m_dev->enqueue(LtpCmd::GET_DATETIME);
    m_dev->enqueue(dashboardTempCmd());
    m_dev->enqueue(LtpCmd::GET_TEMP_STM);   // питание VDD сразу при подключении
    m_dev->enqueue(LtpCmd::GET_STATS);       // наработка, даты циклов, перезапуски

    // Сразу при подключении — включить Flash и проверить, что в ней лежит
    // (Регистратор/Logger/пусто), чтобы подпись в заголовке окна не ждала
    // перехода на вкладку «Тест памяти».
    m_dev->enqueue(LtpCmd::FLASH_ON, {}, TagManual);
    probeFlashImageState();
    m_binSearchRetries = 0;   // сброс при каждом новом подключении
    flashBinSearchStart();

    // (05.07.2026) Авто-чтение архива при подключении УБРАНО: оно лупило по
    // устройству полным чтением журнала (стр.1..65534) прямо на коннекте и,
    // если регистратор молчал/спал, топило лог таймаутами и монополило линию.
    // Чтение архива осталось на СМЕНЕ вкладки «Данные» (setupTabs) — по факту
    // взгляда пользователя, без агрессии на каждом подключении.
    // НО (18.07.2026): если пользователь УЖЕ стоит на «Данные» в момент
    // подключения — событие смены вкладки не придёт, графики оставались пустыми
    // до захода на другую вкладку и обратно. Принцип «по факту взгляда»
    // сохраняем: раз открыта «Данные» — читаем.
    if (ui->tabsMain->currentWidget() == ui->tabDashboard)
        archiveRescanFull();

    for (const PendingCmd &pc : m_pendingCmds)   // команды, нажатые до подключения
        m_dev->enqueue(pc.cmd, pc.payload, pc.tag);
    m_pendingCmds.clear();
}

void MainWindow::onLinkLost(const QString &reason)
{
    setConnectedUi(false);
    appendLog(QStringLiteral("⚠ Связь потеряна: %1").arg(reason));
    m_retryTimer.start(5000);   // автопереподключение
}

void MainWindow::setConnectedUi(bool on, const QString &port)
{
    // Индикатор — вся кнопка целиком (точка плохо видна)
    ui->btnInd->setStyleSheet(on
    ? QStringLiteral("QPushButton#btnInd { background: #1D7A4C;"
                     " border: 1px solid #16613C; border-radius: 12px;"
                     " color: rgba(255,255,255,160); font-size: 12px; }")
    : QStringLiteral("QPushButton#btnInd { background: #C03030;"
                     " border: 1px solid #9A2626; border-radius: 12px;"
                     " color: rgba(255,255,255,140); font-size: 12px; }"));
                         
    ui->btnScan->setText(on ? port : QStringLiteral("ВКЛ"));
    ui->lblDevCard->setText(on
        ? QStringLiteral("LOGLSM-регистратор")
        : QStringLiteral("LogLSMW · сервис регистратора"));
    lblPort->setText(on
        ? QStringLiteral("%1 · %2").arg(port).arg(selectedBaud())
        : QStringLiteral("— · —"));
    if (on) {
        m_devClockTimer.start(1000);
    } else {
        m_devClockTimer.stop();
        m_timeAlarm = false;
        // Отключились — подпись формата журнала в заголовке больше не
        // актуальна (могла поменяться, пока мы были не подключены).
        m_flashImageState = FlashImageState::Unknown;
        refreshImgButtonsHighlight();
        ui->lblDevTime->setStyleSheet(QString());
        ui->lblDevTime->setText(QString());
        ui->lblTimeDiff->setStyleSheet(QString());
        ui->lblTimeDiff->clear();
        ui->lblTempCur->setText(QString());
        ui->lblTempCurCaption->setText(QStringLiteral("текущая"));
        ui->lblVdda->setText(QString());
        // Дашборд — очистка при отключении
        ui->lblUptime->clear();
        ui->lblFirstDate->clear();
        ui->lblFirstTime->clear();
        ui->lblLastDate->clear();
        ui->lblLastTime->clear();
        ui->lblCyclesUsed->clear();
        ui->lblCyclesFree->clear();
        ui->lblMaxSpeed->clear();
        ui->lblMaxVibro->clear();
        ui->lblMaxVibro2->clear();
        ui->lblTempMax->clear();
        ui->lblRestartTimer->clear();
        ui->lblRestartPower->clear();
        // Красная подпись «по таймеру» (тест IWDG) → норма при переподключении:
        // сброс уже случился, рефреш снова включён (18.07.2026).
        ui->lblRestartTimerCaption->setStyleSheet(QString());
        m_lastRstTimer = -1; m_lastRstPower = -1;   // новое устройство/сессия
        // Сбросить бинарный поиск страницы Flash
        m_binSearch = {};
        m_firstFreePage = -1;
        // Сбросить lifetime-счётчики (восстановятся из GET_STATS при следующем подключении)
        m_lastRegTotS    = 0;
        m_preTestRegTotS = 0;
        stendUpdateFlashStat();
        // Остановить прогон при потере связи
        if (m_tempRun.running) tempRunStop();
        // Сбросить активацию при потере связи
        if (m_act.step != ActStep::Idle && m_act.step != ActStep::Done
                && m_act.step != ActStep::Error) {
            m_erasing  = false;
            m_act.step = ActStep::Error;
        }
        ui->barActivation->reset();
        ui->lblActStatus->setText(QStringLiteral("Не активировано"));
        ui->btnActivate->setText(QStringLiteral("▶"));
        ui->btnTempRun->setEnabled(true);
    }
}

// ── Разбор ответов (форматы — прошивка LOGLSMA App/Src/com.c) ──────────────

void MainWindow::onResponse(quint8 cmd, const QByteArray &payload, quint32 tag)
{
    // Ответы стенда (0x8C) разбираются ДО общего switch(cmd) ниже — коды
    // STEND_START/SPEED/STOP (0x04/0x05/0x06) численно совпадают с
    // FLASH_READ_ID/FLASH_ERASE/FLASH_PAGE_ERASE регистратора, иначе ответ
    // стенда попал бы в обработчик флеш-команд (см. devicecontroller.h).
    if (tag == TagStend) {
        stendHandleResponse(cmd, payload);
        return;
    }

    // Любой дошедший сюда ответ — от регистратора (0x8D): если он числился
    // пропавшим в режиме A (m_regAwol после таймаута 0x22), значит вернулся
    // (питание/тумблер перещёлкнули) — возобновляем обычные опросы.
    if (m_regAwol) {
        m_regAwol = false;
        appendLog(QStringLiteral("Регистратор снова на связи — опросы возобновлены"));
        // Панель «Память устройства» могла зафейлиться, пока устройство
        // спало (см. m_binSearchRetries) — устройство вернулось,
        // перезапускаем поиск со свежим бюджетом ретраев.
        if (m_binSearchFailed && !m_binSearch.running) {
            m_binSearchRetries = 0;
            flashBinSearchStart();
        }
    }

    const bool manual = (tag == TagManual);   // ручной запрос со вкладки «Команды»
    const auto *d = reinterpret_cast<const quint8 *>(payload.constData());

    switch (cmd) {
    case LtpCmd::PING:
        appendLog(QStringLiteral("[RX] PING OK"));
        break;

    case LtpCmd::WHO_AM_I:
        if (payload.size() >= 1) {
            const quint8 id = d[0];
            QString imu = (id == 0x6C) ? QStringLiteral("LSM6DSO")
                        : (id == 0x70) ? QStringLiteral("LSM6DSV320X")
                                       : QStringLiteral("неизвестный IMU");
            ui->lblWhoAmI->setText(QStringLiteral("0x%1 (%2)")
                                       .arg(id, 2, 16, QLatin1Char('0')).arg(imu));
            // Имя устройства: ПО УМОЛЧАНИЮ по IMU (fallback для старой прошивки
            // без поля имени); если прошивка пришлёт имя (байты 6+), оно
            // ПЕРЕЗАПИШЕТ это ниже — имя хранится в контроллере (13.07.2026).
            // Написание: финальная буква варианта СТРОЧНАЯ (дизайн-соглашение
            // 13.07.2026) — служит разделителем перед модулями: LogLSMaE00.
            ui->lblDevName->setText(id == 0x70 ? QStringLiteral("LogLSMb")
                                               : QStringLiteral("LogLSMa"));
            // График 2 «пики» = вторая шкала того же vib1 — виден на ОБОИХ
            // вариантах (19.07.2026, пересмотр: раньше прятали на A). На B здесь
            // естественно лягут пики физического high-g акселерометра.
            // ВЕРСИЯ ПРОШИВКИ РЕГИСТРАТОРА (байты 1-2 LE, если прошивка их шлёт —
            // 13.07.2026). Индивидуальный параметр устройства: меняешь FW_VERSION
            // в com.c → перешил → тут сразу новое число = прошивка обновилась.
            if (payload.size() >= 6) {
                // Версия = ВРЕМЯ СБОРКИ прошивки: [1]=ГГ [2]=ММ [3]=ДД [4]=ЧЧ [5]=ММ.
                const int yy = d[1], mm = d[2], dd = d[3], hh = d[4], mi = d[5];
                const QString ver = QStringLiteral("%1.%2.%3  %4.%5")
                    .arg(yy,2,10,QLatin1Char('0')).arg(mm,2,10,QLatin1Char('0'))
                    .arg(dd,2,10,QLatin1Char('0')).arg(hh,2,10,QLatin1Char('0'))
                    .arg(mi,2,10,QLatin1Char('0'));
                ui->lblFwVersion->setText(ver);          // индикатор на «Данные»
                appendLog(QStringLiteral("[RX] прошивка регистратора собрана: %1").arg(ver));
                // Имя устройства ИЗ ПРОШИВКИ (байты 6+, ASCII, null-terminated) —
                // хранится в контроллере. Если пришло — перезаписывает fallback.
                if (payload.size() > 6) {
                    QByteArray nb = payload.mid(6);
                    const int nul = nb.indexOf('\0');
                    if (nul >= 0) nb.truncate(nul);
                    const QString devName = QString::fromLatin1(nb).trimmed();
                    if (!devName.isEmpty()) ui->lblDevName->setText(devName);
                }
            } else {
                ui->lblFwVersion->setText(QStringLiteral("нет (старая прошивка)"));
                appendLog(QStringLiteral("[RX] прошивка не сообщает версию (старая — без поля версии)"));
            }
            // Дублирующую надпись «LOGLSMA-регистратор · ПО от …» в верхней
            // панели убрали (18.07.2026) — имя и версия есть в «Параметрах
            // регистратора» на «Данные», карточка сверху остаётся пустой.
            ui->lblDevCard->clear();
            appendLog(QStringLiteral("[RX] WHO_AM_I 0x%1 — %2")
                          .arg(id, 2, 16, QLatin1Char('0')).arg(imu));
        }
        break;

    case LtpCmd::GET_DATETIME: {  // cod|yy|mm|dd|hh|min|ss
        if (payload.size() < 7 || d[0] != 0)
            break;
        const QDateTime devDt(QDate(2000 + d[1], d[2], d[3]),
                              QTime(d[4], d[5], d[6]));
        ui->lblDevDateTime->setText(
            devDt.toString(QStringLiteral("HH:mm:ss  dd/MM/yyyy")));

        // Лог только при ручном запросе (btnGetTime / контрольное чтение после btnSyncTime)
        if (tag == TagSyncTime)
            appendLog(QStringLiteral("[RX] GET_DATETIME → %1")
                .arg(devDt.toString(QStringLiteral("HH:mm:ss dd/MM/yyyy"))));

        // ТЗ v2 §2.4: сравнение по ПОЛНОЙ дате-времени, допуск 1 с,
        // порог аварии — из настроек (мин). Фоновый 1 Гц — не логируем.
        const qint64 diff  = QDateTime::currentDateTime().secsTo(devDt); // + спешит
        const qint64 ad    = qAbs(diff);
        const qint64 alarm = qint64(ui->spinTimeAlarm->value()) * 60;

        if (m_actSyncPending && tag == TagSyncTime) {
            m_actSyncPending = false;
            // Успех одиночного клика — ОСТАЁТСЯ жёлтым (Active), не Done.
            activationSetSectorMinDelay(2, (ad <= 1)
                ? ActivationBar::SectorState::Active
                : ActivationBar::SectorState::Error, m_actSyncActiveMs);
        }

        m_timeAlarm = (ad > alarm) || !devDt.isValid();
        if (m_timeAlarm) {
            // Часы сбиты (после потери питания идут с 0) — показываем РЕАЛЬНОЕ
            // время регистратора красным, а не заглушку «XX:XX» (17.07.2026, по
            // запросу: «должны показываться новые значения часов»).
            ui->lblDevTime->setStyleSheet(QStringLiteral("color:#C03030;"));
            ui->lblDevTime->setText(devDt.toString(QStringLiteral("HH:mm:ss")));
            ui->lblStendDevTime->setStyleSheet(ui->lblDevTime->styleSheet());
            ui->lblStendDevTime->setText(ui->lblDevTime->text());   // дубль «Стенд»
            ui->lblTimeDiff->clear();   // «разницу» при большой рассинхронизации
                                        // НЕ подписываем (17.07.2026, по запросу)
            break;
        }
        ui->lblDevTime->setText(devDt.toString(QStringLiteral("HH:mm:ss")));
        if (ad <= 1) {                                  // синхронно
            ui->lblDevTime->setStyleSheet(QStringLiteral("color:#1D7A4C;"));
            ui->lblTimeDiff->clear();
        } else {
            ui->lblDevTime->setStyleSheet(QStringLiteral("color:#C03030;"));
            ui->lblTimeDiff->setStyleSheet(QStringLiteral("color:#C03030;"));
            const QString sign = diff > 0 ? QStringLiteral("+") : QStringLiteral("−");
            const qint64 m = ad / 60, s = ad % 60;
            ui->lblTimeDiff->setText(m > 0
                ? QStringLiteral("%1%2 м %3 с").arg(sign).arg(m).arg(s, 2, 10, QLatin1Char('0'))
                : QStringLiteral("%1%2 с").arg(sign).arg(s));
        }
        ui->lblStendDevTime->setStyleSheet(ui->lblDevTime->styleSheet());
        ui->lblStendDevTime->setText(ui->lblDevTime->text());   // дубль «Стенд» (18.07)
        break;
    }

    case LtpCmd::SET_DATETIME:
        appendLog((!payload.isEmpty() && d[0] != 0)
            ? QStringLiteral("[RX] SET_DATETIME: ошибка (код 0x%1)").arg(d[0], 2, 16, QLatin1Char('0'))
            : QStringLiteral("[RX] SET_DATETIME OK"));
        break;

    case LtpCmd::GET_AXES_RAW:   // cod|gyro xyz int16|acc xyz int16
        if (payload.size() >= 13 && d[0] == 0) {
            qint16 gx, gy, gz, ax, ay, az;
            std::memcpy(&gx, d + 1,  2); std::memcpy(&gy, d + 3,  2);
            std::memcpy(&gz, d + 5,  2); std::memcpy(&ax, d + 7,  2);
            std::memcpy(&ay, d + 9,  2); std::memcpy(&az, d + 11, 2);
            ui->lblAxes->setText(QStringLiteral("Acc: %1 %2 %3   Gyro: %4 %5 %6")
                                     .arg(ax).arg(ay).arg(az).arg(gx).arg(gy).arg(gz));
            appendLog(QStringLiteral("[RX] IMU acc(%1,%2,%3) gyro(%4,%5,%6)")
                          .arg(ax).arg(ay).arg(az).arg(gx).arg(gy).arg(gz));
        }
        // Вкладка «Мониторинг»: тот же ответ, независимо от того, что выше
        // уже обновило ярлык на «Команды» — фоновый опрос Мониторинга шлёт
        // эту же команду с тегом TagMon (см. monPoll()).
        if (tag == TagMon && m_mon.running && !m_mon.paused)
            monHandleAxes(payload);
        break;

    case LtpCmd::ACC_GET_FS:    // cod|fullscale_g i32 LE — читается один раз при «Старт» Мониторинга
        if (payload.size() >= 5 && d[0] == 0) {
            qint32 fs = 0;
            std::memcpy(&fs, d + 1, 4);
            m_mon.accSens_mg = accSensitivityMg(fs);
            appendLog(QStringLiteral("[Мониторинг] Acc FS = ±%1g (%2 mg/LSB)")
                          .arg(fs).arg(double(m_mon.accSens_mg), 0, 'f', 3));
        }
        break;

    case LtpCmd::GYRO_GET_FS:   // cod|fullscale_dps i32 LE — читается один раз при «Старт» Мониторинга
        if (payload.size() >= 5 && d[0] == 0) {
            qint32 fs = 0;
            std::memcpy(&fs, d + 1, 4);
            m_mon.gyroSens_mdps = gyroSensitivityMdps(fs);
            appendLog(QStringLiteral("[Мониторинг] Gyro FS = ±%1°/с (%2 mdps/LSB)")
                          .arg(fs).arg(double(m_mon.gyroSens_mdps), 0, 'f', 3));
        }
        break;

    case LtpCmd::GET_TEMP_IMU:
    case LtpCmd::GET_TEMP_TMP117:
    case LtpCmd::GET_TEMP_STM:   // cod|temp float32 [|vdda float32 — только STM 0x11]
        if (payload.size() >= 5 && d[0] == 0) {
            float t = 0.0f;
            std::memcpy(&t, d + 1, 4);
            const QString src = (cmd == LtpCmd::GET_TEMP_IMU)    ? QStringLiteral("IMU")
                              : (cmd == LtpCmd::GET_TEMP_TMP117) ? QStringLiteral("TMP")
                                                                 : QStringLiteral("STM");
            const bool isDashSensor = (cmd == dashboardTempCmd());
            if (isDashSensor) {                // на дашборд — только выбранный датчик
                ui->lblTempCur->setText(QStringLiteral("%1 °C")
                                            .arg(double(t), 0, 'f', 1));
                ui->lblTempCurCaption->setText(QStringLiteral("текущая · %1").arg(src));
            }
            // Вкладка «Команды»: метку трогает только ручной запрос,
            // фоновый опрос (питание/дашборд) её не перетирает.
            if (manual)
                ui->lblTemps->setText(QStringLiteral("%1: %2 °C")
                                          .arg(src).arg(double(t), 0, 'f', 1));

            // Вкладка «Тест памяти»: живые показания каждого датчика (без префикса)
            QLabel *memLbl = (cmd == LtpCmd::GET_TEMP_TMP117) ? ui->lblMemTmpVal
                           : (cmd == LtpCmd::GET_TEMP_IMU)    ? ui->lblMemLsmVal
                                                              : ui->lblMemStmVal;
            memLbl->setText(QStringLiteral("%1 °C").arg(double(t), 0, 'f', 1));

            // Термотест: обрабатываем только от выбранного датчика
            if (m_tempRun.running && cmd == tempRunTempCmd())
                tempRunHandleTemp(t);

            // STM32 (0x11) дополнительно несёт VDD — напряжение питания.
            // Когда STM не выбран как датчик дашборда, в журнал — только VDD,
            // чтобы фоновый опрос питания не выглядел сменой датчика.
            if (cmd == LtpCmd::GET_TEMP_STM && payload.size() >= 9) {
                float v = 0.0f;
                std::memcpy(&v, d + 5, 4);
                ui->lblVdda->setText(QStringLiteral("%1 В").arg(double(v), 0, 'f', 1));
                // Вкладка «Тест памяти»: в варианте A оба канала равны VDD
                ui->lblVddVal->setText(QStringLiteral("%1 В").arg(double(v), 0, 'f', 1));
                ui->lblVbatVal->setText(QStringLiteral("%1 В").arg(double(v), 0, 'f', 1));
                // Лог только для ручных запросов (фоновый опрос не засоряет журнал)
                if (manual)
                    appendLog(QStringLiteral("[RX] STM32: %1 °C, VDD %2 В")
                                  .arg(double(t), 0, 'f', 1).arg(double(v), 0, 'f', 1));
            } else {
                if (manual)
                    appendLog(QStringLiteral("[RX] Temp %1: %2 °C")
                                  .arg(src).arg(double(t), 0, 'f', 1));
            }
            if (tag == TagMon && m_mon.running && !m_mon.paused)
                monHandleTemp(cmd, payload);
        }
        break;

    case LtpCmd::FLASH_READ_ID:
        if (payload.size() >= 4 && d[0] == 0)
            ui->memReport->appendPlainText(QStringLiteral("ID: %1 %2 %3")
                .arg(d[1], 2, 16, QLatin1Char('0')).arg(d[2], 2, 16, QLatin1Char('0'))
                .arg(d[3], 2, 16, QLatin1Char('0')).toUpper());
        break;
    case LtpCmd::FLASH_ERASE:        // ACK мгновенный; готовность ждём опросом WIP
        if (d && d[0]==0) {
            m_erasing = true;
            if (tag == TagAct) {
                // Запускаем WIP-опрос с тегом активации
                appendLog(QStringLiteral("[ACT] Шаг 4: стирание запущено…"));
                QTimer::singleShot(700, this, [this] {
                    if (m_link->isOpen() && m_act.step == ActStep::Erase)
                        m_dev->enqueue(LtpCmd::FLASH_STATE, {}, TagAct);
                });
            } else if (tag == TagImg) {
                // Автостирание перед записью образа (см. m_imgPending*) —
                // WIP-опрос с тегом образа, запись начнётся по готовности.
                appendLog(QStringLiteral("[Образ] стирание чипа запущено, ожидание…"));
                requestCmd(LtpCmd::FLASH_STATE, {}, TagImg);
            } else {
                // m_eraseStartMs уже записан в обработчике кнопки (до отправки команды)
                ui->memReport->appendPlainText(QStringLiteral("Стирание запущено, ожидание…"));
                requestCmd(LtpCmd::FLASH_STATE, {}, TagManual);
            }
        } else {
            if (tag == TagAct) {
                activationFail(QStringLiteral("FLASH_ERASE: ошибка"));
            } else if (tag == TagImg) {
                appendLog(QStringLiteral("[Образ] стирание чипа: ошибка — запись образа отменена"));
                if (m_imgPendingBtn) m_imgPendingBtn->setText(m_imgPendingLabel);
                m_imgActiveBtn = nullptr;
                m_imgPendingBtn = nullptr;
                setOpsEnabled(true);
            } else {
                ui->memReport->appendPlainText(QStringLiteral("Стирание чипа: ошибка"));
            }
        }
        break;
    case LtpCmd::FLASH_STATE:        // [er, WIP] — WIP=1 пока стирается
        if (payload.size() >= 2 && d[0]==0 && m_erasing) {
            const qint64 el = (QDateTime::currentMSecsSinceEpoch() - m_eraseStartMs)/1000;
            if (d[1] != 0) {
                // Ещё стирается
                if (m_act.step == ActStep::Erase) {
                    ui->lblActStatus->setText(
                        QStringLiteral("Шаг 4: стирание… %1 с").arg(el));
                    QTimer::singleShot(700, this, [this] {
                        if (m_link->isOpen() && m_act.step == ActStep::Erase)
                            m_dev->enqueue(LtpCmd::FLASH_STATE, {}, TagAct);
                    });
                } else if (tag == TagImg) {
                    if (m_imgPendingBtn)
                        m_imgPendingBtn->setText(QStringLiteral("⌛ %1 с").arg(el));
                    // Видимый прогресс в «Лог» — без него долгое FLASH_ERASE
                    // (см. devicecontroller.h: «долго!») выглядит как зависание.
                    appendLog(QStringLiteral("[Образ] стирание чипа… %1 с").arg(el));
                    QTimer::singleShot(700, this, [this] {
                        if (m_link->isOpen()) m_dev->enqueue(LtpCmd::FLASH_STATE, {}, TagImg);
                    });
                } else {
                    ui->memReport->appendPlainText(
                        QStringLiteral("  стирание… %1 с").arg(el));
                    QTimer::singleShot(700, this, [this] {
                        if (m_link->isOpen())
                            m_dev->enqueue(LtpCmd::FLASH_STATE, {}, TagManual);
                    });
                }
            } else {
                // Стирание завершено
                m_erasing = false;
                if (m_act.step == ActStep::Erase) {
                    appendLog(QStringLiteral("[ACT] Шаг 4: чип стёрт"));
                    activationSetSector(3, ActivationBar::SectorState::Done);
                    activationBeginStep(ActStep::TestWrite);
                } else if (tag == TagImg) {
                    // Автостирание перед образом готово — теперь действительно
                    // пишем образ из сохранённых m_imgPending* (см. loadImageButton).
                    appendLog(QStringLiteral("[Образ] чип стёрт — начинаю запись образа"));
                    m_flashImageState = FlashImageState::Empty;
                    QPushButton *btn      = m_imgPendingBtn;
                    const QString label   = m_imgPendingLabel;
                    const quint16 sp      = m_imgPendingStartPage;
                    const auto pages      = m_imgPendingPages;
                    const QString fname   = m_imgPendingFileName;
                    m_imgPendingBtn = nullptr;
                    m_imgPendingPages.clear();
                    if (btn) startImageWrite(btn, label, sp, pages, fname);
                } else {
                    setOpsEnabled(true);
                    // Время стирания на этом чипе не измеряется достоверно
                    // (рапортует ~0) — не показываем «за N с», просто факт.
                    ui->memReport->appendPlainText(QStringLiteral("Чип стёрт"));
                    // Сегмент активации «Стереть данные» — ОСТАЁТСЯ жёлтым
                    // по факту одиночного стирания (21.07.2026, по замечанию:
                    // зелёный — только когда пройдёт вся цепочка активации
                    // целиком, единичное срабатывание = жёлтый навсегда).
                    activationSetSector(0, ActivationBar::SectorState::Active);
                    // Содержимое Flash изменилось — вкладка «Данные» до сих пор
                    // показывает результат предыдущего разбора (m_arc хранит его,
                    // ничего не сбрасывает). Перечитываем журнал заново, чтобы
                    // она отражала реальное (теперь пустое) состояние памяти.
                    archiveRescanFull();
                    // Чип стёрт целиком — Flash точно пуста, кнопки «Образ RG/LOG»
                    // подсветку снимаем сразу, без отдельного запроса.
                    m_flashImageState = FlashImageState::Empty;
                    refreshImgButtonsHighlight();
                    // Чип полностью стёрт — первая свободная страница известна точно
                    m_firstFreePage = int(kLogStartPage);
                    // Наработка «Общая» = lifetime с устройства (источник — Flash).
                    // PC-снимок m_lastRegTotS был захвачен раньше и после стирания
                    // «висел» бы старым значением (04.07.2026, «а здесь осталось»).
                    // Сбрасываем снимок и ПЕРЕЗАПРАШИВАЕМ GET_STATS — устройство
                    // сообщит актуальную наработку из Flash (с прошивкой 04.07 после
                    // стирания = 0). GET_STATS сам обновит и «Активное время», и
                    // панель «Наработка» (stendUpdateFlashStat внутри обработчика).
                    m_lastRegTotS    = 0;
                    m_preTestRegTotS = 0;
                    m_pushCount      = 0;
                    stendUpdateFlashStat();   // мгновенно очистить панель
                    if (m_link->isOpen())
                        m_dev->enqueue(LtpCmd::GET_STATS);   // дозаполнить с устройства
                    // Гекс-панель «Тест памяти» — сразу отразить, что память
                    // очистилась (17.07.2026): m_firstFreePage уже = kLogStartPage
                    // (пусто) → memTestDump() попадёт в ветку «память пуста» и
                    // очистит txtHexDump. Гейты как у входа на вкладку.
                    if (ui->tabsMain->currentWidget() == ui->tabMemTest
                        && !m_stendActive && !m_test.running)
                        memTestDump();
                }
            }
        }
        break;
    case LtpCmd::FLASH_PAGE_ERASE:
        // Серия из «Страниц» ответов (см. btnMemErasePage): итог,
        // разблокировка и пересканирования — один раз, после последнего
        // (иначе на каждую страницу гонялись бы archiveRescanFull/
        // probeFlashImageState/flashBinSearchStart поверх ещё идущей серии).
        if (!(d && d[0] == 0)) ++m_pageEraseErr;
        if (m_pageEraseLeft > 0 && --m_pageEraseLeft > 0)
            break;
        setOpsEnabled(true);
        ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница"));   // серия завершена
        if (m_pageEraseErr == 0)
            ui->memReport->appendPlainText(m_pageEraseTotal > 1
                ? QStringLiteral("Стёрто страниц: %1").arg(m_pageEraseTotal)
                : QStringLiteral("Страница стёрта"));
        else
            ui->memReport->appendPlainText(
                QStringLiteral("Стирание стр.: ошибок %1 из %2")
                    .arg(m_pageEraseErr).arg(qMax(m_pageEraseTotal, 1)));
        if (m_pageEraseErr < qMax(m_pageEraseTotal, 1)) { // хоть что-то стёрлось
            archiveRescanFull(); probeFlashImageState();
            // Обновить гекс-панель после стирания страниц/сектора (17.07.2026),
            // если мы на «Тест памяти»: авто-дамп по завершении поиска границ
            // (flashBinSearchSendNext дёрнет memTestDump при m_memAutoDumpPending).
            if (ui->tabsMain->currentWidget() == ui->tabMemTest
                && !m_stendActive && !m_test.running)
                m_memAutoDumpPending = true;
            flashBinSearchStart();
        }
        m_pageEraseErr = 0;
        break;
    case LtpCmd::FLASH_SECTOR_ERASE:
        setOpsEnabled(true);
        ui->memReport->appendPlainText(d && d[0]==0
            ? QStringLiteral("Сектор стёрт") : QStringLiteral("Стирание сект.: ошибка"));
        if (d && d[0]==0) {
            archiveRescanFull(); probeFlashImageState();
            // Обновить гекс-панель после стирания сектора (17.07.2026), если на
            // «Тест памяти»: авто-дамп по завершении поиска границ.
            if (ui->tabsMain->currentWidget() == ui->tabMemTest
                && !m_stendActive && !m_test.running)
                m_memAutoDumpPending = true;
            flashBinSearchStart();
        }
        break;
    case LtpCmd::FLASH_WRITE:
        if (tag == TagImg) {
            ++m_imgPagesDone;
            if (m_imgActiveBtn) {
                auto *btn = qobject_cast<QPushButton*>(m_imgActiveBtn);
                // короткий префикс для индикации прогресса
                const QString pfx = (m_imgActiveBtn == ui->btnImgRG)
                    ? QStringLiteral("RG") : QStringLiteral("LOG");
                if (m_imgPagesDone < m_imgPagesTotal) {
                    if (btn) btn->setText(
                        QStringLiteral("%1 %2/%3 ■").arg(pfx).arg(m_imgPagesDone).arg(m_imgPagesTotal));
                    // Живая индикация в верхних ячейках «Факт» (02.07.2026):
                    // «страниц» — записано на данный момент, «начало» —
                    // подтверждается с первой реально записанной страницы.
                    ui->lblImgPages->setText(QString::number(m_imgPagesDone));
                    ui->lblImgAddr->setText(QString::number(m_imgStartPage));
                } else {
                    if (btn) btn->setText(m_imgBtnLabel);
                    m_imgActiveBtn = nullptr;
                    setOpsEnabled(true);
                    ui->btnMemReadImg->setEnabled(true);
                    // «Страниц»/«Страница» (верхние, факт) — расположение
                    // реально записанного образа (запрос пользователя
                    // 20.06.2026). Отдельно от «Задать» ниже — «Прочитать»
                    // берёт диапазон именно отсюда, а не из тестовых полей.
                    ui->lblImgPages->setText(QString::number(m_imgPagesTotal));
                    ui->lblImgAddr->setText(QString::number(m_imgStartPage));
                    const bool isRg = (btn == ui->btnImgRG);
                    // Явное сообщение о завершении (раньше после записи в
                    // журнале не было видно никакого результата — было неясно,
                    // закончилась ли операция и чем она кончилась).
                    appendLog(QStringLiteral(
                        "[Образ] %1 — запись завершена (%2/%2 стр.): Flash теперь "
                        "содержит %3, как у отработавшего устройства")
                            .arg(m_imgFileName).arg(m_imgPagesTotal)
                            .arg(isRg ? QStringLiteral("Регистратор") : QStringLiteral("Logger")));
                    m_flashImageState = isRg ? FlashImageState::Registrator
                                              : FlashImageState::Logger;
                    refreshImgButtonsHighlight();
                    // Имитация отработавшего устройства — заодно синхронизируем
                    // часы устройства текущим временем ПК, как при реальной
                    // активации (ActStep::SyncTime), иначе RTC будет хранить
                    // время, не соответствующее «возрасту» загруженного образа.
                    {   const QDateTime now = QDateTime::currentDateTime();
                        const QDate nd = now.date(); const QTime nt = now.time();
                        QByteArray p;
                        // Ровно 6 байт — sizeof(RTC_DateTime) в прошивке
                        // (cmdSetDateTime), без байта-кода в начале (это не
                        // ответ, а запрос). Лишний 7-й байт раньше ломал
                        // проверку размера на стороне прошивки (er_badarg),
                        // из-за чего часы реально не выставлялись, хотя лог
                        // ошибочно писал «OK» не глядя на код ответа.
                        p.append(char(nd.year() - 2000));
                        p.append(char(nd.month()));
                        p.append(char(nd.day()));
                        p.append(char(nt.hour()));
                        p.append(char(nt.minute()));
                        p.append(char(nt.second()));
                        appendLog(QStringLiteral("[Образ] синхронизация часов устройства…"));
                        requestCmd(LtpCmd::SET_DATETIME, p, TagImg);
                        // Контрольное чтение сразу же — иначе панель «Время»
                        // покажет «часы сбиты» до следующего тика периодического
                        // опроса (m_devClockTimer), а тик может не успеть до
                        // начала archiveStart() ниже, который держит m_dev
                        // занятым на всё сканирование журнала.
                        requestCmd(LtpCmd::GET_DATETIME, {}, TagImg);
                    }
                    // Образ записан — сразу читаем его обратно и разбираем на
                    // вкладке «Данные», чтобы видеть результат именно этой
                    // загрузки (а не «живые» данные устройства).
                    archiveStart(isRg ? ArchiveMode::Registrator
                                      : ArchiveMode::Logger,
                                 m_imgStartPage, m_imgCache.size());
                    // Панель «Память устройства» после записи образа
                    // (03.07.2026): обработчик FLASH_ERASE выставил
                    // «0 занято» при стирании перед образом, а завершение
                    // записи поиск не перезапускало — панель залипала на
                    // нуле при реально записанных страницах. Перезапуск
                    // (очередь общая — шаги поиска встанут за чтением
                    // архива, это нормально).
                    // Гекс-панель — показать ВЕСЬ записанный образ без нажатия
                    // «Прочитать» (17.07.2026): авто-дамп по завершении поиска
                    // границ, диапазон = ДИАПАЗОН ОБРАЗА (явный override, а не
                    // «Старт»/«Страниц»). Гейты как у входа/стирания.
                    if (ui->tabsMain->currentWidget() == ui->tabMemTest
                        && !m_stendActive && !m_test.running) {
                        m_autoDumpStart = m_imgStartPage;
                        m_autoDumpCount = m_imgPagesTotal;
                        m_memAutoDumpPending = true;
                    }
                    flashBinSearchStart();
                }
            }
        } else if (tag == TagTest) {
            // тестовый цикл: продолжаем (Read или следующая страница)
            memTestStep();
        } else if (tag == TagAct) {
            // активация: обрабатывается в activationHandleResponse (ниже)
        } else if (tag == TagTempRun) {
            if (d && d[0] == 0 && m_tempRun.step == TempRunStep::Write) {
                // Записано — читаем обратно для верификации
                m_tempRun.step = TempRunStep::Read;
                m_test.step    = TestStep::Read;
                memTestUpdateUi();
                const quint32 addr = quint32(m_tempRun.page) << 8;
                QByteArray p;
                for (int i = 0; i < 4; ++i) p.append(char((addr >> (8*i)) & 0xFF));
                for (int i = 0; i < 4; ++i) p.append(char((256 >> (8*i)) & 0xFF));
                requestCmd(LtpCmd::FLASH_READ, p, TagTempRun);
            } else {
                ui->memReport->appendPlainText(QStringLiteral(
                    "⚠ Прогон: ошибка записи стр.%1, останов").arg(m_tempRun.page));
                tempRunStop();
            }
        } else {
            // TagManual: обновляем прогресс в колонке Факт
            if (d && d[0] != 0) ++m_test.errTotal;
            ++m_test.pagesDone;
            m_test.pageCur = m_test.pagesDone % (m_test.pageTotal > 0 ? m_test.pageTotal : 1);
            memTestUpdateUi();
            if (m_test.pagesDone >= m_test.pageTotal * m_test.cycleTotal) {
                m_test.running = false;
                m_test.step    = TestStep::Idle;
                ui->btnMemWrite->setText(QStringLiteral("Запись"));
                setOpsEnabled(true);
                memTestUpdateUi();
                // После записи — сразу показать результат в гекс-панели без
                // нажатия «Прочитать» (17.07.2026). Диапазон — «Старт»/«Страниц»
                // (с ограничением авто-дампа kAutoDumpMaxPages).
                memTestDump();
            }
            if (d && d[0] == 0)
                ui->memReport->appendPlainText(QStringLiteral("Запись OK"));
            else
                ui->memReport->appendPlainText(QStringLiteral("Запись: ошибка"));
        }
        break;
    case LtpCmd::FLASH_READ:
        if (tag == TagArchive && (payload.size() < 2 || d[0] != 0)) {
            appendLog(QStringLiteral("[Архив] ошибка чтения Flash — останов"));
            m_arc.running = false;
            break;
        }
        if (payload.size() >= 2 && d[0] == 0) {
            const QByteArray data = payload.mid(1);
            if (tag == TagArchive) {
                archiveHandleChunk(data, m_arc.chunkRequested);
            } else if (tag == TagProbe) {
                // Определяем, что лежит в Flash, по первому байту стр.1.
                const quint8 b0 = data.isEmpty() ? 0xFF : quint8(data.at(0));
                m_flashImageState = (b0 == 0xFF) ? FlashImageState::Empty
                                  : (b0 >= 0xF6 && b0 <= 0xFE) ? FlashImageState::Logger
                                                                : FlashImageState::Registrator;
                refreshImgButtonsHighlight();
            } else if (tag == TagBinSearch) {
                flashBinSearchHandlePage(data);
            } else if (tag == TagTest) {
                const quint16 pg = quint16(m_test.pageStart + m_test.pageCur);

                // «Байт» Факт — реально прочитанный байт (см. MemTestState)
                if (!data.isEmpty()) {
                    m_test.lastReadByte    = quint8(data.at(0));
                    m_test.lastReadUniform = true;
                    for (char c : data)
                        if (quint8(c) != quint8(m_test.lastReadByte))
                            { m_test.lastReadUniform = false; break; }
                }

                if (m_test.step == TestStep::Dump) {
                    // «Прочитать» — просто накапливаем сырые байты, без сравнения
                    m_dumpBuf.append(data);
                } else {
                    // Compare: байт за байтом с кешем образа
                    const QByteArray &ref = m_imgCache.at(m_test.pageCur);
                    int mism = 0;
                    for (int i = 0; i < qMin(data.size(), ref.size()); ++i)
                        if (data[i] != ref[i]) ++mism;
                    if (mism == 0)
                        ui->memReport->appendPlainText(
                            QStringLiteral("Стр.%1: ОК").arg(pg));
                    else {
                        m_test.errTotal += mism;   // НЕКОРРЕКТНЫЕ БАЙТЫ, не страницы
                                                   // (17.07.2026: тест побайтовый)
                        ui->memReport->appendPlainText(
                            QStringLiteral("Стр.%1: FAIL — %2 байт").arg(pg).arg(mism));
                    }
                }

                ++m_test.pagesDone;
                m_test.pageCur = m_test.pagesDone;
                memTestUpdateUi();
                if (m_test.pagesDone >= m_test.pageTotal) {
                    // завершено
                    if (m_test.step == TestStep::Dump) {
                        renderHexDump(m_test.pageStart, m_dumpBuf);
                        ui->memReport->appendPlainText(QStringLiteral(
                            "Дамп готов: стр.%1..%2 (%3 байт)")
                            .arg(m_test.pageStart)
                            .arg(m_test.pageStart + m_test.pageTotal - 1)
                            .arg(m_dumpBuf.size()));
                    } else {
                        const qint64 totalBytes = qint64(m_test.pageTotal) * 256;
                        ui->memReport->appendPlainText(QStringLiteral(
                            "─────\nОшибок байт: %1 из %2")
                            .arg(m_test.errTotal).arg(totalBytes));
                    }
                    m_test.running = false;
                    m_test.step    = TestStep::Idle;
                    setOpsEnabled(true);
                    memTestUpdateUi();
                    return;
                }
                memTestStep();
            } else if (tag == TagAct) {
                // активация: обрабатывается в activationHandleResponse (ниже)
            } else if (tag == TagTempRun) {
                // верификация страницы температурного прогона
                if (m_tempRun.step == TempRunStep::Read) {
                    if (!data.isEmpty()) {   // «Байт» Факт (см. MemTestState)
                        m_test.lastReadByte    = quint8(data.at(0));
                        m_test.lastReadUniform = true;
                        for (char c : data)
                            if (quint8(c) != quint8(m_test.lastReadByte))
                                { m_test.lastReadUniform = false; break; }
                    }
                    int mism = 0;
                    for (char c : data) if (quint8(c) != m_tempRun.testByte) ++mism;
                    ++m_tempRun.opCount;
                    if (mism > 0) {
                        m_tempRun.errCount += mism;   // некорректные БАЙТЫ (17.07.2026)
                        ui->memReport->appendPlainText(QStringLiteral(
                            "  ✗ Прогон op%1 стр.%2: расхождений %3 байт")
                            .arg(m_tempRun.opCount).arg(m_tempRun.page).arg(mism));
                    } else {
                        ui->memReport->appendPlainText(QStringLiteral(
                            "  ✓ Прогон op%1 стр.%2 OK")
                            .arg(m_tempRun.opCount).arg(m_tempRun.page));
                    }
                    // Живой вывод в гекс-панель по мере записи/сверки (17.07.2026:
                    // «делает запись, но в окно не выводит»). memTestDump тут звать
                    // НЕЛЬЗЯ (столкнётся с машиной термотеста) — рендерим напрямую
                    // из уже прочитанной страницы. m_dumpBuf копит блок текущей
                    // температурной точки (сбрасывается в tempRunBeginStep).
                    m_dumpBuf.append(data);
                    renderHexDump(m_tempRun.rangeStart, m_dumpBuf);
                    m_test.pagesDone = m_tempRun.opCount;
                    m_test.errTotal  = m_tempRun.errCount;
                    // Блок текущей температурной точки: осталось ли страниц из
                    // «проходов»? Да — сразу пишем/сверяем следующую страницу;
                    // нет — блок завершён, ждём следующего шага °C (17.07.2026).
                    if (--m_tempRun.pagesLeftInStep > 0) {
                        ++m_tempRun.page;                     // следующая страница блока
                        m_tempRun.step   = TempRunStep::Idle; // разрешить tempRunDoOp
                        m_test.pageStart = m_tempRun.page;
                        memTestUpdateUi();
                        tempRunDoOp();
                    } else {
                        m_tempRun.step   = TempRunStep::Idle;
                        m_test.step      = TestStep::Idle;
                        m_test.pageStart = m_tempRun.page;
                        memTestUpdateUi();
                    }
                }
            } else {
                // ручной запрос (btnMemRead) — обновляем прогресс
                if (!data.isEmpty()) {   // «Байт» Факт (см. MemTestState)
                    m_test.lastReadByte    = quint8(data.at(0));
                    m_test.lastReadUniform = true;
                    for (char c : data)
                        if (quint8(c) != quint8(m_test.lastReadByte))
                            { m_test.lastReadUniform = false; break; }
                }
                bool ok = false;
                const int exp = ui->editTestByte->text().trimmed().toInt(&ok, 16);
                int mism = 0;
                if (ok) for (char c : data) if (quint8(c) != quint8(exp)) ++mism;
                m_test.errTotal += mism;   // НЕКОРРЕКТНЫЕ БАЙТЫ, не страницы (17.07.2026)
                ++m_test.pagesDone;
                m_test.pageCur = m_test.pagesDone % (m_test.pageTotal > 0 ? m_test.pageTotal : 1);
                memTestUpdateUi();
                if (m_test.pagesDone >= m_test.pageTotal * m_test.cycleTotal) {
                    m_test.running = false;
                    m_test.step    = TestStep::Idle;
                    ui->btnMemRead->setText(QStringLiteral("Чтение"));
                    setOpsEnabled(true);
                    memTestUpdateUi();
                    // После чтения — обновить гекс-панель актуальным содержимым
                    // (17.07.2026, по запросу), без нажатия «Прочитать».
                    memTestDump();
                }
                ui->memReport->appendPlainText(QStringLiteral("Чтение %1 байт: %2")
                    .arg(data.size())
                    .arg(ok ? (mism==0
                               ? QStringLiteral("совпало с 0x%1").arg(exp,2,16,QLatin1Char('0'))
                               : QStringLiteral("расхождений: %1").arg(mism))
                            : QString::fromLatin1(data.left(16).toHex(' '))));
            }
        }
        break;

    case LtpCmd::WDG_TEST:
        // Рефреш IWDG отключён (тест) — подпись «по таймеру» КРАСНАЯ, чтобы
        // было видно состояние «сторож взведён на сброс» (18.07.2026). Норма
        // вернётся при переподключении после сброса (см. очистку лейблов).
        ui->lblRestartTimerCaption->setStyleSheet(
            QStringLiteral("color:#C03030;font-weight:700;"));
        break;
    case LtpCmd::GET_STATS: {
        // payload: cod(1) | total_sec u32 | ts_first u32 | ts_last u32
        //        | ts_activation u32 | restarts_timer u16 | restarts_power u16
        //        | mode u8 | stats_ver u8   — итого 23 байта (ТЗ v2 §7.1)
        // [1..4]: после обновления прошивки = lifetime total_sec из Flash
        //         (был HAL_GetTick/1000, теперь s_reg->totalSec = LoadTotalSec при входе в Service)
        if (payload.size() < 23 || d[0] != 0)
            break;
        quint32 uptime, ts_first, ts_last;
        quint16 rst_timer, rst_power;
        std::memcpy(&uptime,    d + 1,  4);
        std::memcpy(&ts_first,  d + 5,  4);
        std::memcpy(&ts_last,   d + 9,  4);

        // Захватить lifetime-наработку устройства (base для «Общая»/«Сессия»
        // в панели «Наработка»).
        // Обновляем базу ТОЛЬКО когда стенд не работает — иначе повторный GET_STATS
        // во время сессии затёр бы m_preTestRegTotS и «Сессия» обнулилась бы.
        if (!m_stendActive) {
            m_preTestRegTotS = qint64(uptime);
            if (m_lastRegTotS < m_preTestRegTotS)
                m_lastRegTotS = m_preTestRegTotS;  // инициализация до первого CYCLE_PUSH
        }
        stendUpdateFlashStat();
        // d+13: ts_activation (4) — резерв для этапа активации
        std::memcpy(&rst_timer, d + 17, 2);
        std::memcpy(&rst_power, d + 19, 2);

        // Сегмент «Сброс WDT» — ЖИВОЙ индикатор (21.07.2026, по замечанию):
        // синхронизируется с РЕАЛЬНЫМИ счётчиками при КАЖДОМ GET_STATS (он
        // опрашивается фоном раз в ~5 с, см. выше), а не только сразу после
        // клика — иначе новый рестарт (например, после сброса питания) не
        // возвращал бы сегмент в красный, пока не нажать «Сброс WDT» заново.
        {
            const bool synced = (rst_timer == 0 && rst_power == 0);
            const auto target = synced ? ActivationBar::SectorState::Active
                                        : ActivationBar::SectorState::Idle;
            if (m_actWdtPending) {
                m_actWdtPending = false;
                activationSetSectorMinDelay(1, target, m_actWdtActiveMs);
            } else if (m_act.step == ActStep::Idle) {
                // Вне полной автоактивации (21.07.2026) — там сегмент 1
                // означает ДРУГОЙ шаг («проверка устройства»), не трогаем.
                activationSetSector(1, target);
            }
        }

        const quint32 h = uptime / 3600;
        const quint32 m = (uptime % 3600) / 60;

        // Наработка: «37 ч 12 м» — но только пока архив ни разу не отработал
        // (та же причина, что у дат ниже: GET_STATS — счётчик самой прошивки,
        // не видит записи тестового образа; найдено 20.06.2026 — раньше
        // безусловно перезатирало lblUptime даже после archiveFinish(),
        // показывая наработку живого железа вместо duration_total образа).
        if (!m_arc.valid) {
            ui->lblUptime->setText(QStringLiteral("%1.%2").arg(h, 3, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')));
        }

        // Дата/время первого и последнего цикла — но только пока архив (прямое
        // чтение и разбор журнала из Flash, вкладка «Тест памяти» → «Данные»)
        // ни разу не отработал. GET_STATS — это сводка по счётчикам самой
        // прошивки и не видит записи, которые мы кладём в Flash напрямую через
        // FLASH_WRITE при загрузке тестового образа; после первого успешного
        // archiveFinish() он не должен затирать то, что показал разбор архива.
        if (!m_arc.valid) {
            const auto fmtCycleTs = [](quint32 ts, QLabel *lDate, QLabel *lTime) {
                if (ts == 0xFFFFFFFFu) {
                    lDate->setText(QStringLiteral(" "));
                    lTime->setText(QStringLiteral(" "));
                    return;
                }
                const QDateTime dt = QDateTime::fromSecsSinceEpoch(qint64(ts));
                lDate->setText(dt.toString(QStringLiteral("yyyy-MM-dd")));
                lTime->setText(dt.toString(QStringLiteral("HH:mm")));
            };
            fmtCycleTs(ts_first, ui->lblFirstDate, ui->lblFirstTime);
            fmtCycleTs(ts_last,  ui->lblLastDate,  ui->lblLastTime);
        }

        // Счётчики перезапусков
        // Сторож сработал (счётчик вырос с прошлого чтения) → моргаем красным
        // ~10 c (17.07.2026). Первый GET_STATS сессии (-1) моргание не взводит.
        if (m_lastRstTimer >= 0 && rst_timer > m_lastRstTimer) {
            m_rstTimerBlinkLeft = 10;
            // IWDG-сброс состоялся → рефреш снова включён (boot) → снять
            // красную подпись теста (18.07.2026).
            ui->lblRestartTimerCaption->setStyleSheet(QString());
        }
        if (m_lastRstPower >= 0 && rst_power > m_lastRstPower)
            m_rstPowerBlinkLeft = 10;
        m_lastRstTimer = rst_timer;
        m_lastRstPower = rst_power;
        ui->lblRestartTimer->setText(QString::number(rst_timer));
        ui->lblRestartPower->setText(QString::number(rst_power));

        appendLog(QStringLiteral("[RX] GET_STATS: %1 ч %2 м, рестарты T=%3 P=%4")
                      .arg(h).arg(m).arg(rst_timer).arg(rst_power));
        break;
    }

    case LtpCmd::START_REGISTER:
        // ACK на 0x1D при старте циклограммы (03.07.2026). TagAct
        // (активация) обрабатывается отдельным диспетчером ниже — сюда
        // попадает только стендовый запуск (TagManual).
        if (tag == TagManual) {
            appendLog(QStringLiteral(
                "Регистратор: мониторинг вращения запущен — устройство ушло "
                "из сервиса, до остановки теста на LTP-запросы не отвечает "
                "(только пуши циклов)"));
            stendKickoff();   // автомат взведён — можно крутить мотор
        }
        break;

    case LtpCmd::START_TEST:
        // ACK на 0x23 — «Тест» (автомат без сна, устройство остаётся на связи).
        if (tag == TagManual) {
            appendLog(QStringLiteral(
                "Регистратор: ТЕСТ запущен — автомат без сна, на связи; "
                "ждём пуши завершённых циклов (опросы не подавляем)"));
            stendKickoff();   // автомат взведён — можно крутить мотор
        }
        break;

    case LtpCmd::STOP_REGISTER:
        appendLog(QStringLiteral(
            "Регистратор: мониторинг остановлен — устройство снова в сервисе"));
        break;

    default:
        appendLog(QStringLiteral("[RX] cmd=0x%1 len=%2 %3")
                      .arg(cmd, 2, 16, QLatin1Char('0'))
                      .arg(payload.size())
                      .arg(payload.isEmpty()
                               ? QString()
                               : QString::fromLatin1(payload.toHex(' '))));
        break;
    }

    // Диспетчер активации — вызывается после обработки UI-эффектов
    if (tag == TagAct
            && m_act.step != ActStep::Idle
            && m_act.step != ActStep::Done
            && m_act.step != ActStep::Error
            && m_act.step != ActStep::Erase)   // Erase: FLASH_STATE обрабатывает сам
    {
        activationHandleResponse(cmd, payload);
    }
}

void MainWindow::onProtoError(quint8 cmd, quint8 code, const QString &name)
{
    appendLog(QStringLiteral("[ERR] cmd=0x%1 код=0x%2 (%3)")
                  .arg(cmd, 2, 16, QLatin1Char('0'))
                  .arg(code, 2, 16, QLatin1Char('0'))
                  .arg(name));
}

void MainWindow::onRequestFailed(quint8 cmd)
{
    appendLog(QStringLiteral("[ТАЙМАУТ] cmd=0x%1 — нет ответа после повторов")
                  .arg(cmd, 2, 16, QLatin1Char('0')));

    // Одиночный клик по сегменту 1/2 «завис» жёлтым без ответа — таймаут
    // переводит его в ошибку (красный), не оставляя гореть вечно (21.07.2026).
    if (m_actWdtPending && (cmd == LtpCmd::RESET_STATS || cmd == LtpCmd::GET_STATS)) {
        m_actWdtPending = false;
        activationSetSector(1, ActivationBar::SectorState::Error);
    }
    if (m_actSyncPending && (cmd == LtpCmd::SET_DATETIME || cmd == LtpCmd::GET_DATETIME)) {
        m_actSyncPending = false;
        activationSetSector(2, ActivationBar::SectorState::Error);
    }

    // Серия «⚠ Страница» (btnMemErasePage): таймаут одной страницы —
    // прервать серию целиком, иначе кнопки останутся заблокированными
    // (итог/setOpsEnabled(true) печатается только после ПОСЛЕДНЕГО ответа
    // серии, которого при таймауте не будет).
    if (cmd == LtpCmd::FLASH_PAGE_ERASE && m_pageEraseLeft > 0) {
        m_dev->clearQueue();
        ui->memReport->appendPlainText(QStringLiteral(
            "Стирание стр.: таймаут — серия прервана (%1 из %2 не сделано)")
                .arg(m_pageEraseLeft).arg(m_pageEraseTotal));
        m_pageEraseLeft = 0;
        m_pageEraseErr  = 0;
        ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница"));
        setOpsEnabled(true);
    }

    // Стоп мониторинга (0x22) не дошёл — регистратор спит в Stop2 (пауза
    // циклограммы или тест уже кончился и вращения больше нет). Разбудить
    // его может только событие на WKUP-пинах (03.07.2026):
    // Залипание кнопок «Тест памяти» (03.07.2026): операции блокируют UI
    // (setOpsEnabled(false)) до своего завершения, а при финальном таймауте
    // (устройство спит в режиме A / пропало) завершение не наступает никогда
    // — кнопки оставались выключенными до перезапуска программы. Любой
    // финальный таймаут команды текущей операции теперь прерывает её явно.
    if (m_test.running) {
        m_dev->clearQueue();
        m_test.running = false;
        m_test.step    = TestStep::Idle;
        ui->btnMemWrite->setText(QStringLiteral("Запись"));
        ui->btnMemRead->setText(QStringLiteral("Чтение"));
        ui->memReport->appendPlainText(
            QStringLiteral("⚠ Операция прервана: устройство не отвечает"));
        setOpsEnabled(true);
        memTestUpdateUi();
    }
    if (m_imgActiveBtn) {
        auto *btn = qobject_cast<QPushButton*>(m_imgActiveBtn);
        if (btn) btn->setText(btn == ui->btnImgRG ? QStringLiteral("Образ RG")
                                                  : QStringLiteral("Образ LOG"));
        m_imgActiveBtn = nullptr;
        appendLog(QStringLiteral("Образ: операция прервана — устройство не отвечает"));
        setOpsEnabled(true);
    }

    // Чтение архива оборвалось таймаутом (устройство пропало посреди
    // разбора) — раньше m_arc.running оставался взведённым, а кнопка
    // «Память…» — в «⌛ чтение…» навсегда (03.07.2026).
    if (m_arc.running && cmd == LtpCmd::FLASH_READ && !m_binSearch.running) {
        m_arc.running = false;
        appendLog(QStringLiteral("[Архив] таймаут чтения — останов"));
        if (m_stendArcToPanel) {
            m_stendArcToPanel = false;
            ui->btnStendFromDev->setText(QStringLiteral("Память…"));
            ui->btnStendFromDev->setEnabled(true);
        }
    }

    if ((cmd == LtpCmd::START_REGISTER || cmd == LtpCmd::START_TEST)
        && m_stendKickoffPending) {
        appendLog(QStringLiteral(
            "Регистратор не подтвердил запуск (0x%1) — прогон пойдёт без него: "
            "колонка «Регистратор» останется пустой")
                      .arg(cmd, 2, 16, QLatin1Char('0')));
        stendKickoff();   // мотор всё равно крутим — журнал стенда ценен сам по себе
    }

    if (cmd == LtpCmd::STOP_REGISTER) {
        appendLog(QStringLiteral(
            "Регистратор не услышал стоп (спит, Stop2). Варианты: перещёлкнуть "
            "тумблер WKUP1 (если есть) или питание регистратора; либо кратко "
            "запустить/остановить циклограмму заново — во время вращения стоп дойдёт"));
        // Не спамить опросами в спящее устройство — редкий PING вместо
        // часов/температур, пока регистратор не вернётся (см. m_regAwol).
        m_regAwol = true;
    }

    // Если тайм-аут случился при FLASH_READ во время бинарного поиска
    // (прошивка блокируется на стирании страницы Flash до 500 мс) —
    // сбросить состояние и перезапустить, но не более 5 раз.
    if (cmd == LtpCmd::FLASH_READ && m_binSearch.running) {
        m_binSearchRetries++;
        m_binSearch = {};
        m_firstFreePage = -1;
        if (m_binSearchRetries >= 5) {
            m_binSearchFailed = true;
            appendLog(QStringLiteral("Flash: нет ответа после %1 попыток — устройство "
                                     "спит (Stop2)? нужен сброс/переподключение")
                          .arg(m_binSearchRetries));
            flashBinSearchUpdateUi();
        } else {
            appendLog(QStringLiteral("Flash: повтор поиска (попытка %1/5)…")
                          .arg(m_binSearchRetries + 1));
            flashBinSearchUpdateUi();   // показать "—" во время паузы
            QTimer::singleShot(1500, this, [this] { flashBinSearchStart(); });
        }
    }
}

void MainWindow::onCounters(quint32 tx, quint32 rx, quint32 crcErrors)
{
    lblCounters->setText(QStringLiteral("TX %1 · RX %2 · CRC %3")
                             .arg(tx).arg(rx).arg(crcErrors));
}

// ── Журнал ──────────────────────────────────────────────────────────────────

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), msg);
    ui->logBrief->appendPlainText(line);
    ui->logView->appendPlainText(line);
    ui->logMon->appendPlainText(line);   // 3-строчный живой лог на «Мониторинге»
    statusBar()->showMessage(line, 4000);   // видно с любой вкладки
}

// ── Режимы, часы, статусбар, стили (этап 1) ────────────────────────────────

void MainWindow::setServiceMode(bool on)
{
    if (on && (m_test.running || m_tempRun.running || m_erasing)) {
        QMessageBox::warning(this, QStringLiteral("Операция выполняется"),
            QStringLiteral("Сначала остановите текущую операцию."));
        return;
    }
    // TODO: запрос пароля при on==true (этап 5)

    // Синхронизируем галки — взаимоисключающее выделение
    QSignalBlocker b1(ui->actGoData), b2(ui->actEngineerMode);
    ui->actGoData->setChecked(!on);
    ui->actEngineerMode->setChecked(on);

    ui->tabsMain->tabBar()->setVisible(on);
    ui->chkSimulation->setVisible(on);   // только в сервисе (запрос 21.06.2026)
    if (!on)
        ui->tabsMain->setCurrentWidget(ui->tabDashboard);
    if (lblMode)
        lblMode->setText(on ? QStringLiteral("🔧 режим: сервис")
                            : QStringLiteral("🔒 режим: оператор"));
}

void MainWindow::tickPcClock()
{
    ui->lblPcTime->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    ui->lblStendPcTime->setText(ui->lblPcTime->text());   // дубль на «Стенде» (18.07)

    // Часы сбиты: НЕ моргаем «XX:XX» — lblDevTime показывает реальное время
    // регистратора (красным), обновляется периодическим GET_DATETIME, чтобы
    // было видно фактическое значение часов (00:00:xx после потери питания).
    // (17.07.2026)

    // Сторож сработал → значение счётчика моргает красным ~10 c (17.07.2026).
    m_blinkPhase = !m_blinkPhase;
    auto blinkRst = [this](QLabel *l, int &left) {
        if (left > 0) {
            --left;
            l->setStyleSheet(m_blinkPhase
                ? QStringLiteral("color:#C03030;font-weight:700;") : QString());
            if (left == 0) l->setStyleSheet(QString());
        }
    };
    blinkRst(ui->lblRestartTimer, m_rstTimerBlinkLeft);
    blinkRst(ui->lblRestartPower, m_rstPowerBlinkLeft);
}

void MainWindow::setupStatusBar()
{
    lblPort     = new QLabel(QStringLiteral("— · —"), this);
    lblAddr     = new QLabel(QStringLiteral("ADDR 0x8D"), this);
    lblCounters = new QLabel(QStringLiteral("TX 0 · RX 0 · CRC 0"), this);
    lblMode     = new QLabel(this);

    QFont f(kMono);
    f.setPointSize(8);
    for (QLabel *l : { lblPort, lblAddr, lblCounters, lblMode })
        l->setFont(f);

    statusBar()->addWidget(lblPort);
    statusBar()->addWidget(lblAddr);
    statusBar()->addWidget(lblCounters);
    statusBar()->addPermanentWidget(lblMode);
}

void MainWindow::setupDashboardPlots()
{
    // Мини-графики дашборда: без осей и сетки, только линия (эскиз R21)
    const auto initMini = [](QCustomPlot *plot, const QColor &color) {
        plot->xAxis->setVisible(false);
        plot->yAxis->setVisible(false);
        plot->axisRect()->setAutoMargins(QCP::msNone);
        plot->axisRect()->setMargins(QMargins(0, 2, 0, 2));
        plot->addGraph();
        plot->graph(0)->setPen(QPen(color, 1.5));
        plot->setBackground(Qt::transparent);
    };
    initMini(ui->plotSpeed, QColor(0x2F, 0x6F, 0xB0));
    // Акселерометры — тоже СТОЛБИКИ (18.07.2026): выборки дискретные (1 цикл =
    // 1 значение), как время и гироскоп. Канал 1 (уровень) красный, канал 2
    // (удары/jerk; на варианте B — физический high-g accel) оранжевый.
    initMini(ui->plotVibro,  QColor(0xC0, 0x30, 0x30));
    initMini(ui->plotVibro2, QColor(0xE0, 0x8A, 0x20));
    auto makeBars = [](QCustomPlot *plot, QColor c) {
        auto *b = new QCPBars(plot->xAxis, plot->yAxis);
        b->setPen(QPen(c));
        b->setBrush(c);
        b->setWidthType(QCPBars::wtPlotCoords);
        b->setWidth(0.8);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        plot->axisRect()->setRangeDrag(Qt::Horizontal);
        plot->axisRect()->setRangeZoom(Qt::Horizontal);
        return b;
    };
    m_vibBars  = makeBars(ui->plotVibro,  QColor(0xC0, 0x30, 0x30));
    m_vib2Bars = makeBars(ui->plotVibro2, QColor(0xE0, 0x8A, 0x20));
    // График 2 = «пики» ТОГО ЖЕ vib1 в полном масштабе (19.07.2026): удары
    // видны здесь, не сплющивая график 1 «уровень». Виден ВСЕГДА (не про
    // вариант B — это просто вторая шкала одного сигнала).
    ui->lblMaxVibro2Caption->setText(QStringLiteral("акселерометр · пики"));

    // «Активное время»: столбчатая диаграмма длительностей циклов — 1 столбик
    // = 1 завершённый цикл, высота = его длительность (с). Ширина столбиков
    // авто-масштабируется QCustomPlot по количеству циклов: мало циклов — широкие
    // столбики, много — узкие (вплоть до визуального слияния в линии при сотнях
    // записей, это ожидаемо).
    ui->plotUptime->xAxis->setVisible(false);
    ui->plotUptime->yAxis->setVisible(false);
    ui->plotUptime->axisRect()->setAutoMargins(QCP::msNone);
    ui->plotUptime->axisRect()->setMargins(QMargins(0, 2, 0, 2));
    ui->plotUptime->setBackground(Qt::transparent);
    // СТОЛБИКИ равной ширины с зазором (18.07.2026, возврат по запросу — вместо
    // заливки-ступенек 05.07): 1 столбик = 1 цикл, высота = длительность.
    // ШИМ-проблема плотных данных решается МАСШТАБИРОВАНИЕМ: колесо мыши зумит
    // ось циклов, зажатая ЛКМ — прокрутка; по умолчанию видны последние
    // kUptimeDefaultBars циклов (см. рендер). Ширина 0.8 в координатах ключей
    // (зазор 0.2) — на любом зуме столбики равные, с пробелом.
    m_uptimeBars = new QCPBars(ui->plotUptime->xAxis, ui->plotUptime->yAxis);
    m_uptimeBars->setPen(QPen(QColor(0x4C, 0x8B, 0xC9)));
    m_uptimeBars->setBrush(QColor(0x4C, 0x8B, 0xC9));
    m_uptimeBars->setWidthType(QCPBars::wtPlotCoords);
    m_uptimeBars->setWidth(0.8);
    ui->plotUptime->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plotUptime->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plotUptime->axisRect()->setRangeZoom(Qt::Horizontal);
    // «Гироскоп» — тоже СТОЛБИКИ (18.07.2026): высота = МАКС. скорость за цикл,
    // ось X синхронна со столбиками времени (общий зум/прокрутка) — столбик под
    // столбиком, цикл к циклу.
    m_speedBars = new QCPBars(ui->plotSpeed->xAxis, ui->plotSpeed->yAxis);
    // Цвет гироскопа — бирюзовый, отличен от синих столбиков времени (18.07).
    m_speedBars->setPen(QPen(QColor(0x2F, 0x9E, 0x86)));
    m_speedBars->setBrush(QColor(0x2F, 0x9E, 0x86));
    m_speedBars->setWidthType(QCPBars::wtPlotCoords);
    m_speedBars->setWidth(0.8);
    ui->plotSpeed->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plotSpeed->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plotSpeed->axisRect()->setRangeZoom(Qt::Horizontal);
    // Синхронизация осей X ВСЕХ ЧЕТЫРЁХ графиков циклов (время/гироскоп/два
    // акселерометра): зум/прокрутка любого двигает остальные (18.07.2026).
    // setRange глушит рекурсию — rangeChanged не эмитится без изменения.
    {
        const QList<QCustomPlot*> cyclePlots = {
            ui->plotUptime, ui->plotSpeed, ui->plotVibro, ui->plotVibro2 };
        for (QCustomPlot *src : cyclePlots)
            connect(src->xAxis,
                    static_cast<void (QCPAxis::*)(const QCPRange&)>(&QCPAxis::rangeChanged),
                    this, [this, cyclePlots, src](const QCPRange &r) {
                for (QCustomPlot *dst : cyclePlots)
                    if (dst != src) { dst->xAxis->setRange(r); dst->replot(); }
            });
    }
    // Тултипы: время (длительность+старт) / скорость (макс за цикл).
    // Ползунок истории циклов (18.07.2026): таскает окно всех 4 графиков.
    // Ось ↔ ползунок двусторонне; рекурсию глушат QSignalBlocker/сравнение.
    connect(ui->scrCycles, &QScrollBar::valueChanged, this, [this](int v) {
        const double span = ui->plotUptime->xAxis->range().size();
        ui->plotUptime->xAxis->setRange(v - 0.5, v - 0.5 + span);
        ui->plotUptime->replot();   // остальные подтянет синхронизация осей
    });
    connect(ui->plotUptime->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange &) { updateCycleScroll(); });

    // kind: 0 = время, 1 = гироскоп, 2 = акселерометр·уровень, 3 = ·удары.
    auto barTip = [this](QCustomPlot *plot, QMouseEvent *ev, int kind) {
        if (m_arc.plotDuration.isEmpty()) return;
        const int idx = int(plot->xAxis->pixelToCoord(ev->pos().x()) + 0.5);
        if (idx < 0 || idx >= m_arc.plotKeys.size()) { QToolTip::hideText(); return; }
        // Только НА столбике (18.07.2026): курсор выше вершины / ниже нуля —
        // пустое место, тултип не показываем.
        double barH = 0.0;
        switch (kind) {
        case 1: barH = idx < m_arc.plotRpm.size()   ? m_arc.plotRpm[idx]   : 0.0; break;
        case 2: barH = idx < m_arc.plotVibroRms.size() ? m_arc.plotVibroRms[idx] : 0.0; break;  // «уровень» = RMS
        case 3: barH = idx < m_arc.plotVibro.size()    ? m_arc.plotVibro[idx]    : 0.0; break;  // «пики» = peak
        default: barH = m_arc.plotDuration[idx]; break;
        }
        const double yc = plot->yAxis->pixelToCoord(ev->pos().y());
        if (yc < 0.0 || yc > barH) { QToolTip::hideText(); return; }
        // Формат (18.07.2026, по запросу):
        //   стр.1: «Цикл N  ГГ.ММ.ДД» (дата только при осмысленных часах);
        //   стр.2: длительность ММ:СС · старт на регистраторе ЧЧ:ММ (какой есть,
        //          даже после сброса часов — видно 00:xx). Секунды — в логе.
        QDateTime dt;
        if (idx < m_arc.plotTs.size())
            dt = QDateTime(QDate(2000,1,1), QTime(0,0))
                     .addSecs(qint64(m_arc.plotTs[idx]));
        QString l1 = QStringLiteral("Цкл%1").arg(idx + 1);   // «Цкл455» — короче
        // Дату скрываем ТОЛЬКО при реально сброшенных часах (2000–2001 от нуля
        // RTC); всё остальное (в т.ч. даты тестовых образов) показываем как
        // есть (18.07.2026 — прежний фильтр 2020–2050 резал даты образа).
        if (dt.isValid() && dt.date().year() >= 2002)
            l1 += QStringLiteral("   %1").arg(dt.toString(QStringLiteral("dd.MM.yy")));
        // Выравнивание под стр.1 (18.07.2026): «Цкл455␣␣26.10.22» — дата с 9-й
        // позиции; стр.2 «␣07:18␣␣10:53:30» — ведущий пробел + 2 пробела, старт
        // начинается той же колонкой, без разделительной точки.
        // Единый формат ЧЧ:ММ:СС и для длительности, и для старта (18.07.2026).
        const int durS = int(m_arc.plotDuration[idx]);
        QString l2;
        switch (kind) {
        case 1:  l2 = QStringLiteral("%1о/м").arg(barH, 0, 'f', 0); break;  // «308о/м»
        case 2:
        case 3:  l2 = QStringLiteral("%1g").arg(barH / 1000.0, 0, 'f', 2); break; // «1.81g»
        default: l2 = QStringLiteral("%1:%2:%3")
                          .arg(durS / 3600, 2, 10, QLatin1Char('0'))
                          .arg((durS / 60) % 60, 2, 10, QLatin1Char('0'))
                          .arg(durS % 60, 2, 10, QLatin1Char('0')); break;
        }
        if (dt.isValid())
            l2 += QStringLiteral(" %1").arg(dt.toString(QStringLiteral("HH:mm:ss")));
        QToolTip::showText(plot->mapToGlobal(ev->pos()),
                           l1 + QLatin1Char('\n') + l2, plot);
    };
    connect(ui->plotUptime, &QCustomPlot::mouseMove, this,
            [this, barTip](QMouseEvent *ev) { barTip(ui->plotUptime, ev, 0); });
    connect(ui->plotSpeed, &QCustomPlot::mouseMove, this,
            [this, barTip](QMouseEvent *ev) { barTip(ui->plotSpeed, ev, 1); });
    connect(ui->plotVibro, &QCustomPlot::mouseMove, this,
            [this, barTip](QMouseEvent *ev) { barTip(ui->plotVibro, ev, 2); });
    connect(ui->plotVibro2, &QCustomPlot::mouseMove, this,
            [this, barTip](QMouseEvent *ev) { barTip(ui->plotVibro2, ev, 3); });

    // Графики мониторинга: оси и графы настраиваются на этапе 4
    for (QCustomPlot *p : { ui->plotAcc, ui->plotGyro }) {
        p->xAxis->setLabel(QStringLiteral("t, с"));
        p->legend->setVisible(false);
    }
}

// ── Стенд (вкладка «Стенд») ──────────────────────────────────────────────
//
// Циклограмма работа/простой. Реализованы все 4 режима из CLAUDE.md
// («Алгоритм циклограммы и совмещения журналов», сессия 21.06.2026):
// Случайно ±/Только +/Только − (смена скорости без остановки мотора внутри
// фазы «Работа») и N старт/стоп (полный старт-стоп на каждой ступеньке,
// треугольный обход всего ряда — калибровка порога пробуждения
// регистратора, не основной режим). Цикл идёт по кругу до ручного «Стоп».
//
// НЕ реализовано в этом проходе (сознательно отложено — отдельная задача):
// чтение записи регистратора после «Стоп» (правая колонка «Регистратор» в
// cmpReport заполняется через CMD_CYCLE_PUSH от регистратора)
// и запись журнала на диск (см. CLAUDE.md — решено вести в файл, код не
// написан). Локальный таймер циклограммы не ждёт ACK от стенда — таймаут
// команды виден как обычная строка в общем логе (без явной привязки к
// стенду), цикл продолжается по расписанию независимо от него.

void MainWindow::setupStend()
{
    m_stendStepTimer.setSingleShot(true);
    connect(&m_stendStepTimer, &QTimer::timeout, this, [this] {
        if (!m_stend.running)
            return;
        if (m_stend.mode == StendMode::NStartStop) {
            if (m_stend.phase == StendPhase::Work)
                stendFinishCycle();          // конец «Работы» — стоп, затем пауза
            else
                stendNextNStartStop();       // конец «Простоя» — следующая ступенька
        } else {
            if (m_stend.phase == StendPhase::Work)
                stendAdvanceWorkStep();       // следующий сегмент или конец фазы
            else
                stendBeginWork();             // конец «Простоя» — новая фаза работы
        }
    });

    // «Работа» — полный тест (0x1D, регистратор в автомат). Кнопка-
    // переключатель: «Работа» → запуск, «Стоп» → остановка. Пока идёт «Тест»
    // (noReg) — «Работа» не останавливает чужой прогон (гейт по !m_stendNoReg).
    connect(ui->btnStendStart, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            if (!m_stendNoReg) stendStop(true);
            return;
        }
        stendStart(false /* полный тест */);
    });
    // «Тест» — noReg-циклограмма (то же, что «Старт стенда» на «Мониторинге»):
    // регистратор остаётся в Service, 0x1D не шлётся. Переключатель Тест↔Стоп.
    connect(ui->btnStendTest, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            if (m_stendNoReg) stendStop(true);
            return;
        }
        stendStart(true /* noReg */);
    });
    connect(ui->btnStendStop, &QPushButton::clicked, this, [this] { stendStop(true); }); // скрыта, но оставлена для совместимости

    // Продублированный «Старт» на «Мониторинге» (03.07.2026): обычная
    // программа циклограммы (параметры вкладки «Стенд»), но в режиме noReg
    // — только мотор, регистратор не трогается (0x1D нет), остаётся в
    // Service, графики живут. Повторное нажатие — стоп.
    connect(ui->btnMonStendStart, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            if (m_stendNoReg) stendStop(true);
            return;   // обычную циклограмму отсюда не останавливаем
        }
        stendStart(true /* noReg */);
    });
    connect(ui->btnOpenJournal, &QPushButton::clicked, this, [this] {
        if (m_journalFile.fileName().isEmpty()) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_journalFile.fileName()));
    });

    // «Из устройства…» — сценарий 2: прочитать автономный журнал с Flash
    // и вывести в правую панель (03.07.2026, см. stendShowDeviceLog()).
    connect(ui->btnStendFromDev, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            appendLog(QStringLiteral("Стенд: сначала остановите циклограмму"));
            return;
        }
        if (m_arc.running) {
            appendLog(QStringLiteral("Стенд: разбор архива уже идёт — дождитесь"));
            return;
        }
        if (!m_link->isOpen()) {
            appendLog(QStringLiteral("Стенд: нет связи с устройством"));
            return;
        }
        m_stendArcToPanel = true;
        archiveRescanFull();
        if (!m_arc.running) {
            // archiveRescanFull не запустился (идёт тест памяти/стирание) —
            // не оставлять флаг взведённым до случайного чужого разбора.
            m_stendArcToPanel = false;
            appendLog(QStringLiteral("Стенд: чтение журнала не запустилось (устройство занято)"));
            return;
        }
        // Индикация идущего чтения (03.07.2026, «не видно, идёт загрузка
        // или нет») — восстановление в archiveFinish/onRequestFailed.
        ui->btnStendFromDev->setText(QStringLiteral("⌛ чтение…"));
        ui->btnStendFromDev->setEnabled(false);
    });

    // «Образ…» — записи циклов из .hex-файла образа БЕЗ устройства →
    // колонка «Регистратор» (03.07.2026, см. stendLoadImageRecords).
    connect(ui->btnStendFromImg, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            appendLog(QStringLiteral("Стенд: сначала остановите циклограмму"));
            return;
        }
        QSettings st(kOrg, kApp);
        const QString lastDir = st.value(QStringLiteral("memtest/lastImgDir"),
                                          QStringLiteral("actual/test_dumps")).toString();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Образ Регистратора — выбрать файл (.hex)"),
            lastDir, QStringLiteral("Intel HEX (*.hex *.ihex);;Все файлы (*)"));
        if (path.isEmpty()) return;
        st.setValue(QStringLiteral("memtest/lastImgDir"), QFileInfo(path).absolutePath());
        QString err;
        if (!stendLoadImageRecords(path, &err)) {
            appendLog(QStringLiteral("Стенд: образ %1 — %2")
                          .arg(QFileInfo(path).fileName(), err));
            return;
        }
        // Образ занимает колонку «Стенд» (или-или-или с журналом/живым
        // тестом — модель 03.07); колонка «Регистратор» (память) не
        // трогается: образ↔память = проверка целостности записи.
        m_offlJrnSrc = QStringLiteral("образ %1").arg(QFileInfo(path).fileName());
        appendLog(QStringLiteral("Стенд: образ %1 — %2 записей → колонка «Стенд»")
                      .arg(QFileInfo(path).fileName())
                      .arg(m_offlJournal.size()));
        stendShowDeviceLog();
    });

    // «Очистить» — сброс офлайн-вида целиком (записи + файл журнала + панель)
    connect(ui->btnStendOfflineClear, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            appendLog(QStringLiteral("Стенд: идёт циклограмма — панель занята живым тестом"));
            return;
        }
        m_offlDev.clear();
        m_offlJournal.clear();
        m_offlDevSrc.clear();
        m_offlJrnSrc.clear();
        m_offlJrnLifetime = false;
        ui->cmpReport->setPlainText(QStringLiteral(" "));
        appendLog(QStringLiteral("Стенд: офлайн-вид очищен"));
    });

    // «Журнал…» — файл журнала испытания с диска → колонка «Стенд»; при
    // прочитанном устройстве — совмещение + «Ошибка» (см. stendShowDeviceLog).
    connect(ui->btnStendFromFile, &QPushButton::clicked, this, [this] {
        if (m_stend.running) {
            appendLog(QStringLiteral("Стенд: сначала остановите циклограмму"));
            return;
        }
        QSettings st(kOrg, kApp);
        const QString lastDir = st.value(QStringLiteral("stend/lastJournalDir"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Журнал испытания — выбрать файл"), lastDir,
            QStringLiteral("Журнал испытания (LogLSM_stend_*.txt);;Текстовые файлы (*.txt);;Все файлы (*)"));
        if (path.isEmpty()) return;
        st.setValue(QStringLiteral("stend/lastJournalDir"), QFileInfo(path).absolutePath());
        QString err;
        if (!stendLoadJournalFile(path, &err)) {
            appendLog(QStringLiteral("Стенд: журнал %1 — %2")
                          .arg(QFileInfo(path).fileName(), err));
            return;
        }
        m_offlJrnLifetime = false;   // «Общее» журнала — сессионное накопление
        m_offlJrnSrc = QStringLiteral("журнал %1").arg(QFileInfo(path).fileName());
        appendLog(QStringLiteral("Стенд: журнал %1 — %2 циклов → колонка «Стенд»")
                      .arg(QFileInfo(path).fileName())
                      .arg(m_offlJournal.size()));
        stendShowDeviceLog();   // рендер: файл (+ память, если прочитана)
    });

    // Значения по умолчанию
    ui->spinWorkMinMin->setValue(0);  ui->spinWorkMinSec->setValue(10);
    ui->spinWorkMaxMin->setValue(0);  ui->spinWorkMaxSec->setValue(10);
    ui->spinPauseMinMin->setValue(0); ui->spinPauseMinSec->setValue(10);
    ui->spinPauseMaxMin->setValue(0); ui->spinPauseMaxSec->setValue(10);
    new MaxRowHighlighter(ui->cmpReport->document());  // строка «◄» = зелёная

    // Дефолт — простой прогон (19.07.2026): Циклов 1/1, интервалов 1/1 (одна
    // скорость за цикл), скорость 10–300. Усложняется по необходимости.
    ui->spinCyclesMin->setValue(1); ui->spinCyclesMax->setValue(1);
    ui->spinChgMin->setValue(1);
    ui->spinChgMax->setValue(1);
    ui->spinSpdMin->setValue(10);
    ui->spinSpdMax->setValue(300);
    ui->radioModeNStartStop->setChecked(true);

    // ЖИВАЯ валидация Мин≤Макс (19.07.2026): подсветка полей + блок кнопок сразу
    // при вводе, а не при «Старте» (раньше сообщение всплывало «не в тот момент»).
    for (QSpinBox *sb : {
            ui->spinWorkMinMin, ui->spinWorkMinSec, ui->spinWorkMaxMin, ui->spinWorkMaxSec,
            ui->spinPauseMinMin, ui->spinPauseMinSec, ui->spinPauseMaxMin, ui->spinPauseMaxSec,
            ui->spinChgMin, ui->spinChgMax, ui->spinSpdMin, ui->spinSpdMax,
            ui->spinCyclesMin, ui->spinCyclesMax })
        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int){ stendValidateRanges(); });
    stendValidateRanges();   // исходное состояние

    stendSetUiRunning(false);

    // Полоса памяти Flash: светлая часть = занято, тёмный фон = свободно
    if (ui->barFlashMem) {
        ui->barFlashMem->setStyleSheet(
            QStringLiteral(
                "QProgressBar {"
                "  border: 1px solid #555;"
                "  border-radius: 3px;"
                "  background: #1e1e1e;"       /* тёмный = свободно */
                "  text-align: right;"         /* текст вправо — не наезжает на штриховку занятого */
                "  padding-right: 8px;"
                "  color: #fff;"
                "  font-size: 9pt;"
                "  font-weight: bold;"
                "}"
                "QProgressBar::chunk {"
                /* Занято = приглушённая диагональная штриховка (сеточка), а не
                 * сплошной яркий блок — не режет глаз (04.07.2026). spread:repeat
                 * повторяет мелкий период, жёсткие стопы 0.49/0.5 дают чёткие
                 * полоски-штрих. Два близких мягких тона поверх тёмного фона. */
                "  background: qlineargradient(spread:repeat, x1:0, y1:0, x2:0.04, y2:0.04,"
                "    stop:0 #dfe3e8, stop:0.48 #dfe3e8, stop:0.5 #4e535a, stop:1 #4e535a);"
                "  border-radius: 2px;"
                "}"));
        // Номер свободной ячейки рисуем НЕ встроенным текстом бара (он липнет к
        // краю ленты и кажется концом чипа), а overlay-подписью — её позицию
        // в flashBinSearchUpdateUi() двигаем к КРАЮ штриховки. Встроенный текст
        // бара остаётся только под служебные состояния (сканирование…/нет
        // ответа). 05.07.2026, по запросу. Дочерний виджет бара → координаты
        // локальные относительно ленты.
        m_flashCellLbl = new QLabel(ui->barFlashMem);
        m_flashCellLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_flashCellLbl->setStyleSheet(
            QStringLiteral("QLabel{color:#fff; font-weight:bold; background:transparent;}"));
        m_flashCellLbl->hide();
    }
}

// ── Мониторинг (вкладка «Мониторинг») ───────────────────────────────────────
// Живой опрос GET_AXES_RAW (Acc+Gyro вместе) и до 3 температур, скользящие
// графики plotAcc/plotGyro/plotTemp. Реализовано 22.06.2026 — первый проход,
// без конфигурации ODR/FS (см. mainwindow.h). Сценарий, под который писалось:
// проверить стенд «в лоб» — крутить мотор на заданной скорости (об/мин) и
// сверить с измеренной угловой скоростью гироскопа (°/с = об/мин × 6) —
// какая именно ось гироскопа соответствует оси вращения стенда, зависит от
// установки регистратора на стенде, нужно смотреть глазами, какая линия на
// plotGyro отзывается на вращение.
void MainWindow::setupMonitor()
{
    const auto addGraph3 = [](QCustomPlot *plot, const QString &n1, const QString &n2, const QString &n3) {
        plot->addGraph(); plot->graph(0)->setPen(QPen(QColor(0xC0, 0x30, 0x30))); plot->graph(0)->setName(n1);
        plot->addGraph(); plot->graph(1)->setPen(QPen(QColor(0x30, 0x90, 0x30))); plot->graph(1)->setName(n2);
        plot->addGraph(); plot->graph(2)->setPen(QPen(QColor(0x30, 0x50, 0xC0))); plot->graph(2)->setName(n3);
        plot->legend->setVisible(true);
    };
    addGraph3(ui->plotAcc,  QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
    ui->plotAcc->yAxis->setLabel(QStringLiteral("g"));
    addGraph3(ui->plotGyro, QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
    ui->plotGyro->yAxis->setLabel(QStringLiteral("°/с"));
    // График температур убран (03.07.2026) — данные температур по-прежнему
    // опрашиваются (чекбоксы/CSV/индикаторы), просто не рисуются.

    // «Старт» — ТУМБЛЕР Старт/Стоп (07.07.2026, по запросу): один клик запускает,
    // повторный останавливает; надпись Старт↔Стоп меняется в monSetUiRunning().
    // Отдельная маленькая «Стоп» убрана — её функцию взял сам «Старт».
    connect(ui->btnMonStart, &QPushButton::clicked, this, [this] {
        if (m_mon.running) monStop();
        else               monStart();
    });
    connect(ui->btnMonPause, &QPushButton::clicked, this, &MainWindow::monPause);
    ui->btnMonStop->hide();   // убрана; layout сам подберёт место соседям
    // «Старт» и «Пауза» — ФИКСИРОВАННОЙ ширины (07.07.2026): иначе кнопки росли и
    // сжимались под длину надписи («▶ Продолжить» длиннее «⏸ Пауза», «Стоп»
    // короче «Старт»), а из-за stretch в layout за одной тянулась вторая. Фикс-
    // ширина под самую длинную надпись — обе одинаковые и не прыгают. Значение
    // подобрать, если не влезет; Fixed-политика убирает растяжение layout-ом.
    for (QPushButton *b : { ui->btnMonStart, ui->btnMonPause }) {
        b->setMinimumWidth(120);
        b->setMaximumWidth(120);
        QSizePolicy sp = b->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Fixed);
        b->setSizePolicy(sp);
    }
    connect(ui->btnMonClear, &QPushButton::clicked, this, &MainWindow::monClear);
    connect(ui->btnMonCsv,   &QPushButton::clicked, this, &MainWindow::monExportCsv);
    connect(&m_monTimer,     &QTimer::timeout,      this, &MainWindow::monPoll);

    connect(ui->cmbTimeWindow, &QComboBox::currentTextChanged, this, [this](const QString &) {
        m_mon.windowSec = monWindowSecFromCombo();
        monTrim();
        monReplot();
    });
    for (QCheckBox *c : { ui->chkAcc, ui->chkGyro, ui->chkTLsm, ui->chkTTmp, ui->chkTStm })
        connect(c, &QCheckBox::toggled, this, [this](bool) { monReplot(); });

    // Единая сетка управления: левый блок (каналы/Старт/индикаторы/Стоп/Пауза)
    // прижат влево, правый блок (Период/Окно, Очистить/CSV, Старт стенда/
    // обороты) — вправо; слабину забирает СРЕДНЯЯ распорка (колонка 5).
    ui->monTopGrid->setColumnStretch(5, 1);

    monSetUiRunning(false);
}

int MainWindow::monPeriodMsFromCombo() const
{
    const int ms = ui->cmbPollPeriod->currentText().split(QLatin1Char(' ')).value(0).toInt();
    return ms > 0 ? ms : 50;
}

double MainWindow::monWindowSecFromCombo() const
{
    const double s = ui->cmbTimeWindow->currentText().split(QLatin1Char(' ')).value(0).toDouble();
    return s > 0 ? s : 10.0;
}

void MainWindow::monSetUiRunning(bool running)
{
    // Сенсорные чекбоксы (Acc/Gyro/термо) при работе НЕ блокируем (07.07.2026,
    // по запросу): monPoll() читает их состояние вживую на каждом тике, значит
    // датчики можно включать/выключать НА ХОДУ, не останавливая мониторинг
    // (видимость графиков тоже идёт за чекбоксами, см. monRedraw). Блокируем
    // только период опроса — его смена требует перезапуска таймера.
    ui->cmbPollPeriod->setEnabled(!running);
    // «Старт» — тумблер (07.07.2026): всегда активна, надпись Старт↔Стоп.
    ui->btnMonStart->setEnabled(true);
    ui->btnMonStart->setText(running ? QStringLiteral("Стоп") : QStringLiteral("Старт"));
    ui->btnMonPause->setEnabled(running);
    // btnMonStop убрана (скрыта в setupMonitor) — её функцию взял «Старт».
}

void MainWindow::monStart()
{
    if (!m_link->isOpen()) {
        appendLog(QStringLiteral("[Мониторинг] нет связи с устройством"));
        return;
    }
    monClear();   // свежий запуск — буферы и ось времени с нуля
    m_mon.running   = true;
    m_mon.paused    = false;
    m_mon.periodMs  = monPeriodMsFromCombo();
    m_mon.windowSec = monWindowSecFromCombo();
    m_mon.t0.restart();
    monSetUiRunning(true);

    // Факт. полная шкала IMU — один раз при «Старт», для перевода raw LSB
    // в g/°/с ниже (monHandleAxes). Конфигурацию (Set) не трогаем — см.
    // комментарий у setupMonitor()/MonState.
    m_dev->enqueue(LtpCmd::ACC_GET_FS,  {}, TagMon);
    m_dev->enqueue(LtpCmd::GYRO_GET_FS, {}, TagMon);

    m_monTimer.start(m_mon.periodMs);
    appendLog(QStringLiteral("[Мониторинг] старт, период %1 мс, окно %2 с")
                  .arg(m_mon.periodMs).arg(m_mon.windowSec, 0, 'f', 0));
}

void MainWindow::monPause()
{
    if (!m_mon.running)
        return;
    m_mon.paused = !m_mon.paused;
    if (m_mon.paused) {
        m_monTimer.stop();
        ui->btnMonPause->setText(QStringLiteral("▶ Продолжить"));
        appendLog(QStringLiteral("[Мониторинг] пауза"));
    } else {
        m_monTimer.start(m_mon.periodMs);
        ui->btnMonPause->setText(QStringLiteral("⏸ Пауза"));
        appendLog(QStringLiteral("[Мониторинг] продолжение"));
    }
}

void MainWindow::monStop()
{
    if (!m_mon.running)
        return;
    m_monTimer.stop();
    m_mon.running = false;
    m_mon.paused  = false;
    ui->btnMonPause->setText(QStringLiteral("⏸ Пауза"));
    monSetUiRunning(false);
    appendLog(QStringLiteral("[Мониторинг] стоп"));
}

void MainWindow::monClear()
{
    m_mon.tAcc.clear();  m_mon.axX.clear(); m_mon.axY.clear(); m_mon.axZ.clear();
    m_mon.tGyro.clear(); m_mon.gyX.clear(); m_mon.gyY.clear(); m_mon.gyZ.clear();
    for (int i = 0; i < 3; ++i) { m_mon.tTemp[i].clear(); m_mon.vTemp[i].clear(); }
    // Сброс ПК-расчёта вибрации (пики за сессию, EMA гравитации).
    m_mon.vibDc = 0.0; m_mon.vibDcInit = false; m_mon.vib1Peak = 0.0;
    m_mon.prevAmag = 0.0; m_mon.prevAmagInit = false; m_mon.vib2Peak = 0.0;
    if (m_mon.running)
        m_mon.t0.restart();   // «0» на графике — момент последней очистки
    monReplot();
}

void MainWindow::monPoll()
{
    if (!m_mon.running || m_mon.paused)
        return;
    // Регистратор в режиме A (спит/автомат, m_regAwol) или идёт циклограмма
    // (опросы 0x8D во время теста подавлены) — не спамить GET_AXES_RAW
    // каждые 50 мс по недоступному устройству (03.07.2026, замечено в
    // логах: сплошные таймауты 0x1a). Мониторинг «замирает» до
    // возвращения устройства — точки просто не добавляются.
    if (m_regAwol || (m_stendActive && !m_stendNoReg))
        return;
    if (ui->chkAcc->isChecked() || ui->chkGyro->isChecked())
        m_dev->enqueue(LtpCmd::GET_AXES_RAW, {}, TagMon);
    if (ui->chkTLsm->isChecked())
        m_dev->enqueue(LtpCmd::GET_TEMP_IMU, {}, TagMon);
    if (ui->chkTTmp->isChecked())
        m_dev->enqueue(LtpCmd::GET_TEMP_TMP117, {}, TagMon);
    if (ui->chkTStm->isChecked())
        m_dev->enqueue(LtpCmd::GET_TEMP_STM, {}, TagMon);
}

void MainWindow::monHandleAxes(const QByteArray &payload)
{
    if (payload.size() < 13 || quint8(payload[0]) != 0)
        return;
    const auto *d = reinterpret_cast<const quint8 *>(payload.constData());
    qint16 gx, gy, gz, ax, ay, az;
    std::memcpy(&gx, d + 1,  2); std::memcpy(&gy, d + 3,  2); std::memcpy(&gz, d + 5,  2);
    std::memcpy(&ax, d + 7,  2); std::memcpy(&ay, d + 9,  2); std::memcpy(&az, d + 11, 2);

    const double t      = m_mon.t0.elapsed() / 1000.0;
    const double accK   = double(m_mon.accSens_mg)   / 1000.0;  // mg/LSB  → g/LSB
    const double gyroK  = double(m_mon.gyroSens_mdps) / 1000.0; // mdps/LSB → °/с/LSB

    m_mon.tAcc  << t; m_mon.axX << ax * accK;  m_mon.axY << ay * accK;  m_mon.axZ << az * accK;
    m_mon.tGyro << t; m_mon.gyX << gx * gyroK; m_mon.gyY << gy * gyroK; m_mon.gyZ << gz * gyroK;

    // === ПК-РАСЧЁТ ВИБРАЦИИ из живого accel (промежуточный вывод, 13.07.2026) ===
    // Зеркалит firmware HandleSensorData: |a| = √(ax²+ay²+az²) держит ~1g базу,
    // медленный EMA (vibDc) её оценивает, отклонение = вибрация (канал 1);
    // |Δ|a|| между отсчётами = импульсность/удар (канал 2). Так видим вибрацию
    // на ударах прямо сейчас, не дожидаясь firmware-записи.
    const double gX = ax * accK, gY = ay * accK, gZ = az * accK;      // g
    const double amag = std::sqrt(gX*gX + gY*gY + gZ*gZ);
    if (!m_mon.vibDcInit) { m_mon.vibDc = amag; m_mon.vibDcInit = true; }
    const double dev = std::fabs(amag - m_mon.vibDc);
    m_mon.vibDc += (amag - m_mon.vibDc) * 0.05;                       // VIB_DC_ALPHA
    if (dev > m_mon.vib1Peak) m_mon.vib1Peak = dev;
    const double jerk = m_mon.prevAmagInit ? std::fabs(amag - m_mon.prevAmag) : 0.0;
    if (m_mon.prevAmagInit && jerk > m_mon.vib2Peak) m_mon.vib2Peak = jerk;
    m_mon.prevAmag = amag; m_mon.prevAmagInit = true;
    ui->logMon->appendPlainText(
        QStringLiteral("вибр AC=%1 jerk=%2 g | пик1=%3 пик2=%4 g")
            .arg(dev,  0,'f',3).arg(jerk, 0,'f',3)
            .arg(m_mon.vib1Peak, 0,'f',3).arg(m_mon.vib2Peak, 0,'f',3));

    monTrim();
    monReplot();
}

void MainWindow::monHandleTemp(quint8 cmd, const QByteArray &payload)
{
    if (payload.size() < 5 || quint8(payload[0]) != 0)
        return;
    const int idx = (cmd == LtpCmd::GET_TEMP_IMU) ? 0
                  : (cmd == LtpCmd::GET_TEMP_TMP117) ? 1 : 2;
    float t = 0.0f;
    std::memcpy(&t, payload.constData() + 1, 4);
    m_mon.tTemp[idx] << (m_mon.t0.elapsed() / 1000.0);
    m_mon.vTemp[idx] << double(t);

    // Числовой индикатор под соответствующим T-чекбоксом (график температур
    // убран 03.07 — значения показываем здесь).
    QLabel *lbl = (idx == 0) ? ui->lblMonTLsm
                : (idx == 1) ? ui->lblMonTTmp
                             : ui->lblMonTStm;
    lbl->setText(QStringLiteral("%1 °C").arg(double(t), 0, 'f', 1));

    monTrim();
    monReplot();
}

void MainWindow::monTrim()
{
    const double cutoff = (m_mon.t0.elapsed() / 1000.0) - m_mon.windowSec;
    const auto trimN = [cutoff](const QVector<double> &t) {
        int n = 0;
        while (n < t.size() && t[n] < cutoff) ++n;
        return n;
    };
    int n = trimN(m_mon.tAcc);
    if (n > 0) { m_mon.tAcc.remove(0, n); m_mon.axX.remove(0, n); m_mon.axY.remove(0, n); m_mon.axZ.remove(0, n); }
    n = trimN(m_mon.tGyro);
    if (n > 0) { m_mon.tGyro.remove(0, n); m_mon.gyX.remove(0, n); m_mon.gyY.remove(0, n); m_mon.gyZ.remove(0, n); }
    for (int i = 0; i < 3; ++i) {
        n = trimN(m_mon.tTemp[i]);
        if (n > 0) { m_mon.tTemp[i].remove(0, n); m_mon.vTemp[i].remove(0, n); }
    }
}

void MainWindow::monReplot()
{
    const double tNow = m_mon.t0.isValid() ? m_mon.t0.elapsed() / 1000.0 : 0.0;
    const double lo   = qMax(0.0, tNow - m_mon.windowSec);
    const double hi   = qMax(tNow, m_mon.windowSec);

    ui->plotAcc->graph(0)->setData(m_mon.tAcc, m_mon.axX);
    ui->plotAcc->graph(1)->setData(m_mon.tAcc, m_mon.axY);
    ui->plotAcc->graph(2)->setData(m_mon.tAcc, m_mon.axZ);
    for (int i = 0; i < 3; ++i) ui->plotAcc->graph(i)->setVisible(ui->chkAcc->isChecked());
    ui->plotAcc->xAxis->setRange(lo, hi);
    ui->plotAcc->yAxis->rescale(true);
    ui->plotAcc->replot(QCustomPlot::rpQueuedReplot);

    ui->plotGyro->graph(0)->setData(m_mon.tGyro, m_mon.gyX);
    ui->plotGyro->graph(1)->setData(m_mon.tGyro, m_mon.gyY);
    ui->plotGyro->graph(2)->setData(m_mon.tGyro, m_mon.gyZ);
    for (int i = 0; i < 3; ++i) ui->plotGyro->graph(i)->setVisible(ui->chkGyro->isChecked());
    ui->plotGyro->xAxis->setRange(lo, hi);
    ui->plotGyro->yAxis->rescale(true);
    ui->plotGyro->replot(QCustomPlot::rpQueuedReplot);

    // График температур убран (03.07.2026): буферы m_mon.tTemp/vTemp
    // по-прежнему наполняются (CSV-экспорт и числовые индикаторы живы),
    // отрисовки нет.
}

void MainWindow::monExportCsv()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить CSV"), QString(), QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("[Мониторинг] не удалось открыть файл: %1").arg(path));
        return;
    }
    QTextStream out(&f);
    out << "t_s,channel,value\n";
    const auto writeSeries = [&out](const QVector<double> &t, const QVector<double> &v, const QString &name) {
        for (int i = 0; i < t.size(); ++i)
            out << QString::number(t[i], 'f', 3) << ',' << name << ','
                << QString::number(v[i], 'f', 4) << '\n';
    };
    writeSeries(m_mon.tAcc,  m_mon.axX, QStringLiteral("AccX"));
    writeSeries(m_mon.tAcc,  m_mon.axY, QStringLiteral("AccY"));
    writeSeries(m_mon.tAcc,  m_mon.axZ, QStringLiteral("AccZ"));
    writeSeries(m_mon.tGyro, m_mon.gyX, QStringLiteral("GyroX"));
    writeSeries(m_mon.tGyro, m_mon.gyY, QStringLiteral("GyroY"));
    writeSeries(m_mon.tGyro, m_mon.gyZ, QStringLiteral("GyroZ"));
    static const char *names[3] = { "T_LSM", "T_TMP117", "T_STM32" };
    for (int i = 0; i < 3; ++i)
        writeSeries(m_mon.tTemp[i], m_mon.vTemp[i], QString::fromLatin1(names[i]));
    f.close();
    appendLog(QStringLiteral("[Мониторинг] CSV сохранён: %1").arg(path));
}

QVector<int> MainWindow::stendBuildSteps(int n, int minV, int maxV)
{
    QVector<int> steps;
    if (n <= 1) {
        steps.append(minV);
        return steps;
    }
    steps.reserve(n);
    const double span = double(maxV - minV) / double(n - 1);
    for (int i = 0; i < n; ++i)
        steps.append(minV + qRound(span * i));
    return steps;
}

// Выбрать следующую скорость по режиму из текущей позиции prev.
// Только +: растём вверх случайно; дошли до Макс — wrap на Мин.
// Только −: падаем вниз случайно; дошли до Мин — wrap на Макс.
// Случайно ±: любое случайное значение в [Мин, Макс].
int MainWindow::stendPickNextSpeed(int prev) const
{
    const int lo = ui->spinSpdMin->value();
    const int hi = ui->spinSpdMax->value();

    switch (m_stend.mode) {
    case StendMode::OnlyPlus:
        if (prev >= hi)
            return lo;                                          // wrap
        return QRandomGenerator::global()->bounded(qMax(lo, prev + 1), hi + 1);

    case StendMode::OnlyMinus:
        if (prev <= lo || prev == 0)
            return hi;                                          // wrap / init
        return QRandomGenerator::global()->bounded(lo, prev);  // [lo, prev)

    default: // RandomPM
        return QRandomGenerator::global()->bounded(lo, hi + 1);
    }
}

bool MainWindow::sendStendCmd(quint8 cmd, const QByteArray &payload)
{
    if (!m_link->isOpen()) {
        appendLog(QStringLiteral("Стенд: нет связи — команда не отправлена"));
        return false;
    }
    // НАДЁЖНАЯ доставка (09.07.2026) — заменяет fire-and-forget от 07.07.
    // Все команды мотору идут через очередь DeviceController с ACK+ретраями
    // (enqueueTo, тег TagStend), а НЕ одноразовым выстрелом. Причина отката:
    // на общем релеенном (через Nucleo) канале в «Тесте» регистратор в фазе
    // ROTATING заливает линию поллами гироскопа/CYCLE_PUSH, и одноразовые
    // STEND_SPEED/STEND_STOP в этом трафике ТОНУЛИ:
    //   • потерян STEND_SPEED → «показывает скорость, но вращение не включается»
    //     (ярлык скорости обновляется локально ниже, а мотор не крутанул цикл →
    //     регистратору нечего ловить → «нет ответа», видно на 236 об/мин);
    //   • потерян STEND_STOP → «стоп не останавливает мотор».
    // Стенд квитирует все 4 команды (com_interr.c → sendAck), поэтому ACK+ретраи
    // (2 повтора × 500 мс) надёжно добивают пакет. Команды идемпотентны и редки
    // (раз в сегмент/шаг, секунды врозь) — кратковременная занятость канала на
    // квитирование пренебрежима, живой опрос графиков практически не страдает.
    const bool ok = m_dev->enqueueTo(LtpAddr::STEND, cmd, payload, TagStend);

    // Индикатор текущей скорости на «Мониторинге» (03.07.2026): единая точка —
    // сюда проходят ВСЕ команды мотору (циклограмма любого режима).
    // STEND_SPEED — ярлык обновляем сразу (мотор вот-вот раскрутится, показать
    // цель безобидно). «пауза» же по STEND_STOP тут БОЛЬШЕ НЕ ставим: с надёжной
    // доставкой (09.07.2026) команда уходит в очередь, реальная отправка мотору
    // отложена (очередь/ретраи + рампа торможения) — ярлык «пауза» выскакивал бы,
    // пока двигатель ещё крутится. «пауза» переехала в stendHandleResponse (по
    // ACK STEND_STOP = стоп реально доставлен). Полный стоп ярлык гасит отдельно
    // (stendSetUiRunning(false) → " ").
    if (cmd == LtpCmd::STEND_SPEED && payload.size() >= 2) {
        const int rpm = quint8(payload.at(0)) | (quint8(payload.at(1)) << 8);
        ui->lblMonStendSpeed->setText(QStringLiteral("%1 об/мин").arg(rpm));
    }
    return ok;
}

void MainWindow::stendHandleResponse(quint8 cmd, const QByteArray &payload)
{
    const quint8 err = payload.isEmpty() ? 0 : quint8(payload.at(0));
    const QString name = (cmd == LtpCmd::STEND_START) ? QStringLiteral("START")
                        : (cmd == LtpCmd::STEND_SPEED) ? QStringLiteral("SPEED")
                        : (cmd == LtpCmd::STEND_STOP)  ? QStringLiteral("STOP")
                                                        : QStringLiteral("?");
    if (err != 0) {
        appendLog(QStringLiteral("[Стенд] %1: ошибка (код 0x%2)")
                      .arg(name).arg(err, 2, 16, QLatin1Char('0')));
    }
    // «пауза» на индикаторе «Мониторинга» ставим ТОЛЬКО по факту доставки стопа
    // мотору (ACK STEND_STOP), а не при постановке команды в очередь — иначе с
    // надёжной доставкой (очередь/ретраи + рампа) ярлык выскакивал бы, пока
    // двигатель ещё крутится (замечено 09.07.2026). Полный стоп (m_stend.running
    // уже false) → " " (дублирует stendSetUiRunning, безвредно). Межцикловая
    // пауза (running=true) → «пауза». Ошибочный ACK ярлык не трогает — мотор мог
    // не встать, честнее оставить последнюю скорость.
    if (cmd == LtpCmd::STEND_STOP && err == 0) {
        ui->lblMonStendSpeed->setText(m_stend.running ? QStringLiteral("пауза")
                                                      : QStringLiteral(" "));
    }
    // Прочие ACK — без побочных действий: расписание циклограммы ведётся
    // локальным таймером, а не ожиданием ответа стенда (см. шапку).
}

void MainWindow::stendSetUiRunning(bool running)
{
    m_stendActive = running;
    // «Работа» (полный тест) и «Тест» (noReg) — две кнопки-переключателя.
    // Активный вид показывает «Стоп», второй на время прогона блокируется.
    ui->btnStendStart->setText(running && !m_stendNoReg
        ? QStringLiteral("Стоп") : QStringLiteral("Работа"));
    ui->btnStendStart->setEnabled(!running || !m_stendNoReg);
    ui->btnStendTest->setText(running && m_stendNoReg
        ? QStringLiteral("Стоп") : QStringLiteral("Тест"));
    ui->btnStendTest->setEnabled(!running || m_stendNoReg);
    // После остановки — вернуть блок по диапазонам (если Мин>Макс, кнопки
    // должны остаться недоступными, 19.07.2026).
    if (!running) stendValidateRanges();
    // Продублированный «Старт» на «Мониторинге» (03.07.2026): переключатель
    // noReg-циклограммы; при обычной циклограмме — заблокирован.
    ui->btnMonStendStart->setText(running && m_stendNoReg
        ? QStringLiteral("Стоп стенда")
        : QStringLiteral("Старт стенда"));
    ui->btnMonStendStart->setEnabled(!running || m_stendNoReg);
    if (!running)
        ui->lblMonStendSpeed->setText(QStringLiteral(" "));   // соглашение: пробел
    // QList<QWidget*> — явный тип элемента контейнера; в отличие от auto-
    // выводимого initializer_list (ошибка сборки 21.06.2026: конфликт типов
    // QSpinBox*/QRadioButton*), здесь каждый указатель приводится к QWidget*
    // неявно при заполнении списка, без вывода общего auto-типа.
    const QList<QWidget *> widgets = {
        ui->spinWorkMinMin, ui->spinWorkMinSec,
        ui->spinWorkMaxMin, ui->spinWorkMaxSec,
        ui->spinPauseMinMin, ui->spinPauseMinSec,
        ui->spinPauseMaxMin, ui->spinPauseMaxSec,
        ui->spinChgMin, ui->spinChgMax,
        ui->spinSpdMin, ui->spinSpdMax,
        ui->radioModeRandom, ui->radioModeOnlyPlus,
        ui->radioModeOnlyMinus, ui->radioModeNStartStop
    };
    for (QWidget *w : widgets)
        w->setEnabled(!running);
}

// Живая проверка Мин≤Макс для всех пар циклограммы (19.07.2026). Подсвечивает
// неверные поля красным, блокирует кнопки старта и пишет причину в их подсказку.
// Возвращает true, если все пары корректны.
bool MainWindow::stendValidateRanges()
{
    const QString bad  = QStringLiteral("background:#5A2320; border:1px solid #C0403A;");
    const QString ok   = QString();
    auto mark = [&](QSpinBox *a, QSpinBox *b, bool good) {
        a->setStyleSheet(good ? ok : bad);
        b->setStyleSheet(good ? ok : bad);
    };
    QStringList problems;

    const bool workOk = (ui->spinWorkMinMin->value()*60 + ui->spinWorkMinSec->value())
                     <= (ui->spinWorkMaxMin->value()*60 + ui->spinWorkMaxSec->value());
    mark(ui->spinWorkMinSec, ui->spinWorkMaxSec, workOk);
    ui->spinWorkMinMin->setStyleSheet(workOk?ok:bad); ui->spinWorkMaxMin->setStyleSheet(workOk?ok:bad);
    if (!workOk) problems << QStringLiteral("«Время работы»");

    const bool pauseOk = (ui->spinPauseMinMin->value()*60 + ui->spinPauseMinSec->value())
                      <= (ui->spinPauseMaxMin->value()*60 + ui->spinPauseMaxSec->value());
    mark(ui->spinPauseMinSec, ui->spinPauseMaxSec, pauseOk);
    ui->spinPauseMinMin->setStyleSheet(pauseOk?ok:bad); ui->spinPauseMaxMin->setStyleSheet(pauseOk?ok:bad);
    if (!pauseOk) problems << QStringLiteral("«Время простоя»");

    const bool chgOk = ui->spinChgMin->value() <= ui->spinChgMax->value();
    mark(ui->spinChgMin, ui->spinChgMax, chgOk);
    if (!chgOk) problems << QStringLiteral("«Смен скоростей»");

    const bool spdOk = ui->spinSpdMin->value() <= ui->spinSpdMax->value();
    mark(ui->spinSpdMin, ui->spinSpdMax, spdOk);
    if (!spdOk) problems << QStringLiteral("«Скорость»");

    const bool cycOk = ui->spinCyclesMin->value() <= ui->spinCyclesMax->value();
    mark(ui->spinCyclesMin, ui->spinCyclesMax, cycOk);
    if (!cycOk) problems << QStringLiteral("«Циклов»");

    const bool allOk = problems.isEmpty();
    const QString tip = allOk ? QString()
        : QStringLiteral("Минимум больше максимума: %1").arg(problems.join(QStringLiteral(", ")));
    // Блокируем старт только из-за диапазонов (не трогаем блокировку при running —
    // ею заведует stendSetUiRunning). Кнопки на «Стенде» и дубль на «Мониторинге».
    for (QPushButton *b : { ui->btnStendStart, ui->btnStendTest }) {
        if (!m_stend.running) b->setEnabled(allOk);
        b->setToolTip(tip);
    }
    return allOk;
}

void MainWindow::stendStart(bool noReg)
{
    if (m_stend.running)
        return;
    if (!m_link->isOpen()) {
        appendLog(QStringLiteral("Стенд: нет связи — подключитесь перед стартом"));
        return;
    }
    if (!stendValidateRanges())   // подсветка/подсказка уже выставлены живой проверкой
        return;

    m_stendNoReg = noReg;     // ДО stendSetUiRunning — от него зависит текст кнопок
    m_stend = StendState{};   // сброс истории/счётчиков предыдущего прогона
    m_stend.groupSize = 0; m_stend.cycleInGroup = 0;  // 1-й цикл выберет размер группы
    m_sessStendAcc = 0;       // сессионный аккумулятор — с нуля при каждом «Старт»
    m_sessRegAcc   = 0;
    m_sessRegMatched = 0;     // «Сессия» — только обработанные ответы, с нуля
    // База живого дампа (свободная страница) захватывается ЛЕНИВО на первом
    // пуше из РЕАЛЬНОГО m_firstFreePage (device-scan при подключении / после
    // прошлого «Стоп»), а не из накопленной оценки — чтобы инфа была реальной
    // (12.07.2026, по запросу). Между последним сканом и первой записью прогона
    // новых записей нет → значение достоверно.
    m_flashLiveRecords  = 0;
    // База живого дампа: если device-scan уже знает свободную страницу — берём её
    // СРАЗУ и рисуем ПУСТУЮ (все FF) страницу на постоянном месте, чтобы окно не
    // висело пустым до первого цикла (12.07.2026, по запросу — так нагляднее, чем
    // пусто). Иначе (скан ещё идёт, m_firstFreePage=-1 / чип полон) база
    // захватится лениво на 1-м пуше, как раньше.
    if (m_firstFreePage >= 0 && m_firstFreePage < int(kFlashTotalPages)) {
        m_flashLiveBasePage = m_firstFreePage;
        m_flashLiveBuf = QByteArray(256, char(0xFF));   // чистая страница целиком
        if (ui->txtHexDump)
            renderHexDump(quint16(m_flashLiveBasePage), m_flashLiveBuf);
    } else {
        m_flashLiveBasePage = -1;           // -1 = ещё не захвачена (см. первый пуш)
        m_flashLiveBuf.clear();
    }
    m_stend.running = true;
    ui->btnOpenJournal->setEnabled(false);
    stendJournalOpen();
    m_stend.mode = ui->radioModeOnlyPlus->isChecked()    ? StendMode::OnlyPlus
                 : ui->radioModeOnlyMinus->isChecked()   ? StendMode::OnlyMinus
                 : ui->radioModeNStartStop->isChecked()  ? StendMode::NStartStop
                                                          : StendMode::RandomPM;

    stendSetUiRunning(true);
    ui->cmpReport->clear();
    appendLog(QStringLiteral("Стенд: старт циклограммы"));

    // Запустить автомат распознавания вращения на регистраторе (03.07.2026).
    // Прошивка 02.07+: Service() сам вращение больше НЕ распознаёт — без
    // этой команды CMD_CYCLE_PUSH не приходит и колонка «Регистратор»
    // навсегда пустая (так «перестала работать» вкладка после перестройки
    // режимов A/B). Ставится в очередь ПЕРВОЙ — до команд мотору: автомат
    // должен уже ждать вращение, когда стенд начнёт крутить.
    // ⚠ После ACK регистратор уходит из Service в автомат с реальным
    // Stop2 — до конца теста 0x8D НЕ опрашивать (периодические опросы
    // гейтятся по m_stend.running), связь — только входящие CYCLE_PUSH.
    // Мотор НЕ запускается прямо отсюда: первая фаза стартует из
    // stendKickoff() по ACK на 0x1D (или по его таймауту — тогда тест
    // идёт без регистратора). Иначе команды мотору стоят в очереди ЗА
    // ретраями 0x1D (например, «Старт» сразу после «Стоп», когда
    // регистратор ещё переинициализирует UART при входе в Service) и
    // «Старт» выглядит зависшим (замечено на железе 03.07.2026).
    //
    // Запуск/0x1D — В КОНЦЕ функции, после построения программы (ступеньки
    // NStartStop строятся ниже; ранний kickoff в noReg-режиме стартовал по
    // ПУСТОМУ списку ступенек и молча ничего не делал — найдено на железе
    // 03.07.2026: «нажатие ни к чему не привело»).

    if (m_stend.mode == StendMode::NStartStop) {
        // Минимум 2 ступеньки — режим существует именно для обхода Мин..Макс
        // (калибровка порога пробуждения), при N=1 «обход» вырождается в
        // одно постоянное значение и скорость никогда не меняется между
        // циклами (баг замечен 22.06.2026 — «Старт стоп не меняет
        // скорость», N=1 был выбран случайным образом из «Смен скоростей»).
        const int n = qMax(2, QRandomGenerator::global()->bounded(
            ui->spinChgMin->value(), ui->spinChgMax->value() + 1));
        m_stend.triangleSteps = stendBuildSteps(n, ui->spinSpdMin->value(), ui->spinSpdMax->value());
        m_stend.triangleIdx = 0;
        m_stend.triangleDir = 1;
    }

    // Программа готова — запускаем. Обычный режим: 0x1D регистратору, мотор
    // стартует из stendKickoff() по ACK/таймауту (см. комментарий выше по
    // функции). Режим noReg: регистратор не трогается вовсе (0x1D не
    // шлётся, он остаётся в Service, опросы/мониторинг живы — гейты с
    // !m_stendNoReg), мотор стартует сразу.
    m_stendKickoffPending = true;
    if (m_stendNoReg) {
        // «Тест»: регистратор запускает ТОТ ЖЕ автомат, что и «Работа», но
        // БЕЗ сна — остаётся бодрым и на связи. Шлём START_TEST (0x23); мотор
        // стартует из stendKickoff() по ACK (как «Работа» по 0x1D). Опросы
        // 0x8D НЕ подавляем (устройство отвечает — гейты по !m_stendNoReg),
        // пуши CYCLE_PUSH приходят так же → колонка «Регистратор» заполняется.
        appendLog(QStringLiteral(
            "Стенд: ТЕСТ — регистратор без сна, на связи; ждём пуши циклов"));
        requestCmd(LtpCmd::START_TEST, {}, TagManual);
    } else {
        requestCmd(LtpCmd::START_REGISTER, {}, TagManual);
    }
}

// Разбор файла журнала испытания (стенд-сторона офлайн-вида). Формат — тот,
// что пишет stendJournalWrite(): блоки «Метка | значение …» с разделителем
// « | », времена HH:mm:ss без дат; дата берётся из шапки файла («Журнал
// испытания стенда  dd.MM.yyyy HH:mm:ss»), переход через полночь — по
// уменьшению времени старта относительно предыдущего цикла.
bool MainWindow::stendLoadJournalFile(const QString &path, QString *errOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errOut) *errOut = QStringLiteral("не открывается: %1").arg(f.errorString());
        return false;
    }
    QTextStream in(&f);

    QVector<OfflineCycle> out;
    QDate baseDate = QDate::currentDate();   // fallback, если шапки нет
    QTime prevStart;
    OfflineCycle cur;
    bool inBlock = false;

    auto parseHms = [](const QString &s) -> qint64 {
        const QStringList p = s.split(QLatin1Char(':'));
        if (p.size() != 3) return 0;
        return p[0].toLongLong() * 3600 + p[1].toLongLong() * 60 + p[2].toLongLong();
    };
    auto finishBlock = [&]() {
        if (inBlock && cur.start.isValid())
            out.append(cur);
        cur = OfflineCycle{};
        inBlock = false;
    };

    while (!in.atEnd()) {
        const QString line = in.readLine();

        if (line.startsWith(QStringLiteral("Журнал испытания стенда"))) {
            static const QRegularExpression reD(QStringLiteral("(\\d{2})\\.(\\d{2})\\.(\\d{4})"));
            const auto m = reD.match(line);
            if (m.hasMatch())
                baseDate = QDate(m.captured(3).toInt(), m.captured(2).toInt(),
                                 m.captured(1).toInt());
            continue;
        }

        // Два поколения формата (03.07.2026): с 01.07 (вечер) — разделитель
        // « | »; самые ранние файлы 01.07 — колонки выровнены пробелами без
        // разделителя. Разбираем оба: есть «|» — режем по нему, нет — по
        // пробелам (simplified() схлопывает выравнивание; метки — одно
        // слово, значение стенда — второй токен, дельты правее не нужны).
        QString label, v1;
        if (line.contains(QLatin1Char('|'))) {
            const QStringList parts = line.split(QStringLiteral("|"));
            if (parts.size() < 2) { finishBlock(); continue; }
            label = parts[0].trimmed();
            v1    = parts[1].trimmed();
        } else {
            const QStringList t = line.simplified().split(QLatin1Char(' '));
            if (t.size() < 2) { finishBlock(); continue; }   // пустая/разделитель
            label = t[0];
            v1    = t[1];
        }
        if (label == QStringLiteral("Стенд")
            || v1 == QStringLiteral("Стенд")) continue;      // заголовок колонок

        if (label == QStringLiteral("Старт")) {
            finishBlock();
            inBlock = true;
            const QTime t = QTime::fromString(v1, QStringLiteral("HH:mm:ss"));
            if (prevStart.isValid() && t < prevStart)
                baseDate = baseDate.addDays(1);              // перешли полночь
            prevStart = t;
            cur.start = QDateTime(baseDate, t);
        } else if (label == QStringLiteral("Стоп") && inBlock) {
            const QTime t = QTime::fromString(v1, QStringLiteral("HH:mm:ss"));
            cur.stop = QDateTime(t < cur.start.time() ? baseDate.addDays(1) : baseDate, t);
        } else if (label == QStringLiteral("Интервал") && inBlock) {
            cur.intervalS = parseHms(v1);
        } else if ((label == QStringLiteral("Общее")
                    || label == QStringLiteral("Общая")) && inBlock) {
            // «Общая» — метка в журналах до 02.07.2026 (переименована в
            // «Общее», см. CLAUDE.md) — старые файлы тоже читаем.
            cur.totalS = parseHms(v1);
        } else if (label == QStringLiteral("Скорость") && inBlock) {
            cur.speed = v1.toInt();
        }
    }
    finishBlock();

    if (out.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("не найдено ни одного цикла (не журнал испытания?)");
        return false;
    }
    m_offlJournal = out;
    return true;
}

// «Образ…»: заполнить колонку «СТЕНД» офлайн-вида из .hex-файла образа
// (03.07.2026, модель пользователя: образ — эталонное «задание», Flash —
// факт; образ↔память = проверка целостности записи, дельты нулевые).
// Тот же Intel HEX-разбор, что у кнопок «Образ RG/LOG»
// (loadImageFromHexFile), затем те же 48-байтные записи Регистратора v2,
// что в archiveParseRegistratorPage (упрощённо: запись завершена, если
// CRC-поле != 0xFFFF; сам CRC не пересчитываем — файл образа не портится
// в дороге, в отличие от чтения по UART).
bool MainWindow::stendLoadImageRecords(const QString &path, QString *errOut)
{
    quint16 startPage = 0;
    QList<QByteArray> pages;
    QString err;
    if (!loadImageFromHexFile(path, startPage, pages, err)) {
        if (errOut) *errOut = err;
        return false;
    }
    QVector<OfflineCycle> out;
    for (const QByteArray &page : pages) {
        const auto *d = reinterpret_cast<const quint8 *>(page.constData());
        for (int slot = 0; slot < kRegRecordsPerPage; ++slot) {
            const quint8 *r = d + slot * kRecordBytes;   // v2, 48 байт
            quint32 ts, dur, tot; float rpm;
            std::memcpy(&ts,  r + 0,  4);
            std::memcpy(&dur, r + 4,  4);
            std::memcpy(&tot, r + 8,  4);
            std::memcpy(&rpm, r + 16, 4);   // rpm_avg (отчётная «Скорость»)
            const quint16 crc = quint16(r[46]) | (quint16(r[47]) << 8);
            if (ts == 0xFFFFFFFFu)
                { if (errOut && out.isEmpty()) *errOut = QStringLiteral("образ пуст"); goto done; }
            if (crc == 0xFFFF)
                continue;   // оборванная запись — пропускаем, как и парсер архива
            OfflineCycle rec;
            rec.start     = QDateTime::fromSecsSinceEpoch(qint64(ts));
            rec.intervalS = (dur != 0xFFFFFFFFu) ? qint64(dur) : 0;
            rec.stop      = rec.start.addSecs(rec.intervalS);
            rec.totalS    = (tot != 0xFFFFFFFFu) ? qint64(tot) : 0;   // lifetime!
            rec.speed     = int(rpm + 0.5f);
            out.append(rec);
        }
    }
done:
    if (out.isEmpty()) {
        if (errOut && errOut->isEmpty())
            *errOut = QStringLiteral("не найдено ни одной завершённой записи (это образ Регистратора?)");
        return false;
    }
    m_offlJournal     = out;
    m_offlJrnLifetime = true;   // «Общее» здесь — lifetime-наработка записей
    return true;
}

// Рендер офлайн-вида правой панели: файл журнала (колонка «Стенд») ⊕ записи
// устройства/образа (колонка «Регистратор», m_offlDev). Обе стороны есть —
// совмещение по решению 21.06 (одна точка выравнивания в начале, дальше по
// порядку); ведущая лента — ЗАПИСИ УСТРОЙСТВА (приоритет Flash, решение
// 03.07): выводятся все, «Стенд»+«Ошибка» — только у совмещённых. Только
// одна сторона — просто её колонка. Даты — заголовками при смене дня.
void MainWindow::stendShowDeviceLog()
{
    auto hms = [](qint64 s) {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600, 2, 10, QLatin1Char('0'))
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    };
    auto col = [](const QString &s) { return s.leftJustified(13); };
    auto secDelta = [](qint64 d) -> QString {
        if (d == 0) return QStringLiteral("0 секунд");
        const QString sign = d > 0 ? QStringLiteral("+") : QStringLiteral("−");
        const qint64 a = qAbs(d), r10 = a % 10, r100 = a % 100;
        QString unit;
        if      (r10 == 1 && r100 != 11)                            unit = QStringLiteral("секунда");
        else if (r10 >= 2 && r10 <= 4 && (r100 < 10 || r100 >= 20)) unit = QStringLiteral("секунды");
        else                                                          unit = QStringLiteral("секунд");
        return QStringLiteral("%1%2 %3").arg(sign).arg(a).arg(unit);
    };
    // Дельта скорости — в ПРОЦЕНТАХ от заданной стендом (решение 30.06,
    // единый формат с живой панелью stendRenderReport; 03.07 тут по ошибке
    // были об/мин — замечено пользователем).
    auto spdDelta = [](int reg, int stend) -> QString {
        if (stend <= 0) return QStringLiteral("—");
        const double pct = double(reg - stend) / stend * 100.0;
        return pct >= 0 ? QStringLiteral("+%1%").arg(pct, 0, 'f', 1)
                        : QStringLiteral("−%1%").arg(-pct, 0, 'f', 1);
    };

    const int nDev = m_offlDev.size();
    const int nJrn = m_offlJournal.size();
    QString text;
    QDate curDay;
    auto dayHeader = [&](const QDate &d) {
        if (d != curDay) {
            curDay = d;
            text += QStringLiteral("═══ %1 ═══\n").arg(d.toString(QStringLiteral("yyyy-MM-dd")));
        }
    };

    if (nJrn == 0 && nDev == 0) {
        ui->cmpReport->setPlainText(QStringLiteral(" "));
        return;
    }

    // Сводка источников (03.07.2026, «не видно, идёт загрузка или нет») —
    // первой строкой панели: что в какой колонке и сколько.
    if (nJrn > 0)
        text += QStringLiteral("— Стенд: %1, %2 циклов —\n")
            .arg(m_offlJrnSrc.isEmpty() ? QStringLiteral("файл") : m_offlJrnSrc)
            .arg(nJrn);
    if (nDev > 0)
        text += QStringLiteral("— Регистратор: %1, %2 записей —\n")
            .arg(m_offlDevSrc.isEmpty() ? QStringLiteral("память") : m_offlDevSrc)
            .arg(nDev);
    text += QStringLiteral("\n");

    if (nDev == 0) {
        // Только файл журнала — колонка «Стенд»
        for (int i = 0; i < nJrn; ++i) {
            const OfflineCycle &c = m_offlJournal[i];
            dayHeader(c.start.date());
            text += QStringLiteral("Старт     %1\n").arg(c.start.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Стоп      %1\n").arg(c.stop.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Интервал  %1\n").arg(hms(c.intervalS));
            text += QStringLiteral("Общее     %1\n").arg(hms(c.totalS));
            text += QStringLiteral("Скорость  %1\n").arg(c.speed);
            if (i + 1 < nJrn) text += QStringLiteral("\n");
        }
        ui->cmpReport->setPlainText(text);
        appendLog(QStringLiteral("Стенд: файл журнала выведен — %1 циклов").arg(nJrn));
        return;
    }

    // Ведущая лента — записи устройства/образа, ВСЕ (приоритет Flash,
    // решение 03.07): длину вывода задаёт Flash, не файл. Файл журнала (если
    // загружен) подставляется в колонку «Стенд» у совмещённых записей.
    // Точка выравнивания: запись, ближайшая по времени к первому циклу файла
    // (решение 21.06 — одна точка в начале, дальше по порядку); ближе 10
    // минут не нашлось — по порядку с начала, с пометкой в журнале программы.
    int j0 = 0;
    if (nJrn > 0) {
        qint64 best = std::numeric_limits<qint64>::max();
        const qint64 t0 = m_offlJournal[0].start.toSecsSinceEpoch();
        for (int j = 0; j < nDev; ++j) {
            const qint64 d = qAbs(qint64(m_offlDev[j].ts) - t0);
            if (d < best) { best = d; j0 = j; }
        }
        if (best > 600) {
            j0 = 0;
            appendLog(QStringLiteral(
                "Стенд: пары по времени не нашлось (разбег %1 ч) — совмещение по порядку с начала")
                    .arg(best / 3600));
        }
    }
    // База «Общего» устройства: для журнала (сессионные накопления) —
    // наработка ДО первого совмещённого цикла; для образа «Общее» обеих
    // сторон lifetime — сравниваем как есть, без базы (m_offlJrnLifetime).
    const qint64 devBase = m_offlJrnLifetime
        ? 0
        : qint64(m_offlDev[j0].total) - qint64(m_offlDev[j0].dur);

    for (int j = 0; j < nDev; ++j) {
        const OfflineDevRec &r = m_offlDev[j];
        const QDateTime st = QDateTime::fromSecsSinceEpoch(qint64(r.ts));
        const QDateTime sp = st.addSecs(qint64(r.dur));
        dayHeader(st.date());

        const int i = j - j0;                        // индекс пары в файле
        const bool paired = (nJrn > 0 && i >= 0 && i < nJrn);
        if (paired) {
            const OfflineCycle &c = m_offlJournal[i];
            const qint64 devSess = qint64(r.total) - devBase;
            text += QStringLiteral("Старт     %1%2\n")
                .arg(col(c.start.toString(QStringLiteral("HH:mm:ss"))),
                     st.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Стоп      %1%2\n")
                .arg(col(c.stop.toString(QStringLiteral("HH:mm:ss"))),
                     sp.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Интервал  %1%2%3\n")
                .arg(col(hms(c.intervalS)), col(hms(qint64(r.dur))),
                     secDelta(qint64(r.dur) - c.intervalS));
            text += QStringLiteral("Общее     %1%2%3\n")
                .arg(col(hms(c.totalS)), col(hms(devSess)),
                     secDelta(devSess - c.totalS));
            text += QStringLiteral("Скорость  %1%2%3\n")
                .arg(col(QString::number(c.speed)), col(QString::number(r.rpm)),
                     spdDelta(r.rpm, c.speed));
        } else {
            // Пары в файле нет — только колонка «Регистратор»
            text += QStringLiteral("Старт     %1%2\n").arg(col(QString()), st.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Стоп      %1%2\n").arg(col(QString()), sp.toString(QStringLiteral("HH:mm:ss")));
            text += QStringLiteral("Интервал  %1%2\n").arg(col(QString()), hms(qint64(r.dur)));
            text += QStringLiteral("Общее     %1%2\n").arg(col(QString()), hms(qint64(r.total)));
            text += QStringLiteral("Скорость  %1%2\n").arg(col(QString()), QString::number(r.rpm));
        }
        if (j + 1 < nDev) text += QStringLiteral("\n");
    }

    ui->cmpReport->setPlainText(text);
    ui->cmpReport->moveCursor(QTextCursor::Start);   // к началу (точка совмещения)
    ui->cmpReport->ensureCursorVisible();
    appendLog(QStringLiteral("Стенд: офлайн-вид — %1 записей устройства%2")
                  .arg(nDev)
                  .arg(nJrn > 0 ? QStringLiteral(" + файл %1 циклов (совмещение с записи №%2)")
                                      .arg(nJrn).arg(j0 + 1)
                                : QString()));
}

// Отложенный запуск первой фазы циклограммы — см. комментарий в stendStart()
// и объявление в mainwindow.h. Вызывается из onResponse (ACK 0x1D) или
// onRequestFailed (0x1D не подтверждён — тест продолжается без регистратора).
void MainWindow::stendKickoff()
{
    if (!m_stendKickoffPending || !m_stend.running)
        return;
    m_stendKickoffPending = false;
    if (m_stend.mode == StendMode::NStartStop)
        stendNextNStartStop();
    else
        stendBeginWork();
}

void MainWindow::stendStop(bool manual)
{
    if (!m_stend.running)
        return;
    m_stendKickoffPending = false;   // стоп до ACK 0x1D — мотор уже не стартуем
    m_stendStepTimer.stop();
    /* Сбросить очередь незавершённых stend-команд (STEND_SPEED в полёте,
     * ожидающие слоты) — без этого они таймаутят по одной и блокируют
     * вкладку Команды после остановки стенда. */
    m_dev->clearQueue();
    const bool wasWork = (m_stend.phase == StendPhase::Work);
    if (wasWork && manual)
        stendRecordCycle();   // дописать в историю незавершённый цикл как есть
    // ПОРЯДОК ВАЖЕН (09.07.2026). Сначала гасим регистратор, потом мотор:
    // 1) STOP_REGISTER (0x22) — надёжный, с ретраями — уводит автомат регистратора
    //    из бодрой фазы (CONFIRM/ROTATING) обратно в Service. Стоп общий для
    //    «Работы» (0x1D) и «Теста» (0x23). В «Тесте» устройство бодрое → 0x22
    //    доходит без таймаута; идущий цикл прошивка закроет сама (RotationFinishCycle,
    //    с сохранением во Flash). Если «Стоп» нажат в паузе — регистратор спит в
    //    Stop2, 0x22 уйдёт в таймаут (onRequestFailed): повторить стоп во время
    //    вращения или перещёлкнуть тумблер WKUP1 (если есть).
    // 2) STEND_STOP мотору — ТОЛЬКО ПОСЛЕ шага 1: регистратор уже вышел из ROTATING
    //    и перестал заливать общий (релеенный через Nucleo) канал → линия тихая, и
    //    надёжный (очередь, ACK+ретраи — см. sendStendCmd) стоп гарантированно
    //    доходит. До 09.07 STEND_STOP шёл fire-and-forget и во время вращения в
    //    «Тесте» тонул в трафике поллов/CYCLE_PUSH — мотор не вставал, хотя тест
    //    останавливался (симптом «тест стоп, мотор крутится»).
    requestCmd(LtpCmd::STOP_REGISTER, {}, TagManual);
    if (wasWork)
        sendStendCmd(LtpCmd::STEND_STOP);

    m_stend.running = false;
    m_stend.phase = StendPhase::Idle;
    m_regPushOrphans.clear();   // отложенные пуши прогона больше не актуальны
    // Записываем в журнал все незаписанные завершённые циклы
    for (auto &rec : m_stend.history)
        stendJournalWrite(rec);
    if (m_journalFile.isOpen())
        m_journalFile.close();
    // Lifetime total хранится в Flash устройства (durationTotal каждой записи).
    // QSettings не нужен — при следующем подключении GET_STATS вернёт актуальный
    // total_sec напрямую с устройства.
    stendSetUiRunning(false);
    m_stendNoReg = false;   // после обновления UI (текст кнопок от него зависит)
    appendLog(QStringLiteral("Стенд: остановлен"));
    // Поиск границы Flash мог быть отложен (guard в flashBinSearchStart/
    // SendNext) на всё время работы циклограммы — запускаем/возобновляем
    // сейчас, когда трафик стенда больше не мешает.
    flashBinSearchStart();
    // «Данные» после прогона (18.07.2026): во время «Работы» устройство спит и
    // журнал не читается (по дизайну), поэтому вкладка замирала на старом — а
    // после «Стоп» новые циклы уже во Flash. Перечитываем сразу, не заставляя
    // пользователя ходить по вкладкам.
    archiveRescanFull();
}

// ── Режимы Случайно ±/Только +/Только − ─────────────────────────────────

void MainWindow::stendBeginWork()
{
    if (!m_stend.running)
        return;
    m_stend.phase = StendPhase::Work;
    // Лок-степ: перед выводом нового цикла закрываем предыдущую ждущую строку
    // (пуш пришёл → уже заполнена; не пришёл за паузу → «нет ответа»).
    stendResolvePendingReg();

    // ГРУППА рабочих циклов (19.07.2026): циклограмма = группа из N циклов,
    // N — случайно из «Циклов» Мин/Макс. Отработала группа — берём новый размер
    // и начинаем лестницу режима (Только +/−) сначала; крутится до ручного «Стоп».
    if (m_stend.cycleInGroup >= m_stend.groupSize) {
        m_stend.cycleInGroup = 0;
        m_stend.groupSize = qMax(1, QRandomGenerator::global()->bounded(
                                     ui->spinCyclesMin->value(), ui->spinCyclesMax->value() + 1));
        m_stend.lastSpeed = 0;   // новая группа — лестницу режима засеваем от Мин
    }
    m_stend.cycleInGroup++;

    const int durMin = ui->spinWorkMinMin->value() * 60 + ui->spinWorkMinSec->value();
    const int durMax = ui->spinWorkMaxMin->value() * 60 + ui->spinWorkMaxSec->value();
    const int durS   = QRandomGenerator::global()->bounded(durMin, durMax + 1);
    // «Смен скоростей» = количество скоростей за один цикл (1 = постоянная)
    const int n = qMax(1, QRandomGenerator::global()->bounded(
                             ui->spinChgMin->value(), ui->spinChgMax->value() + 1));

    // Скорости полок цикла (19.07.2026 — по спеке «Алгоритм циклограммы»):
    //  • Только +/− = РОВНАЯ лестница Мин..Макс включительно (шаг (hi−lo)/(n−1)),
    //    по возрастанию/убыванию — детерминированно (раньше случайные приращения
    //    с заворотом давали немонотонный ряд 15,20,10 → полки не совпадали);
    //  • Случайно ± = случайные в [Мин,Макс].
    m_stend.steps.clear();
    {
        const int lo = ui->spinSpdMin->value();
        const int hi = ui->spinSpdMax->value();
        if (m_stend.mode == StendMode::OnlyPlus || m_stend.mode == StendMode::OnlyMinus) {
            for (int i = 0; i < n; ++i) {
                const int v = (n <= 1) ? lo
                    : lo + int(qRound(double(hi - lo) * i / (n - 1)));
                m_stend.steps.append(v);
            }
            if (m_stend.mode == StendMode::OnlyMinus)
                std::reverse(m_stend.steps.begin(), m_stend.steps.end());
        } else {
            for (int i = 0; i < n; ++i)
                m_stend.steps.append(QRandomGenerator::global()->bounded(lo, hi + 1));
        }
    }
    m_stend.stepIndex    = 0;
    m_stend.cycleStartTs = QDateTime::currentDateTime();
    m_stend.lastSpeed    = m_stend.steps.first();

    const int firstVal = m_stend.lastSpeed;   // steps.first(), уже записано в lastSpeed

    // STEND_SPEED сам запускает мотор (SetTimPeriod + HAL_TIM_PWM_Start),
    // STEND_START не нужен: после STOP currSpeed=0, START вызвал бы SetSpeed(0).
    QByteArray p(3, 0);
    p[0] = char(quint16(firstVal) & 0xFF);
    p[1] = char((quint16(firstVal) >> 8) & 0xFF);
    p[2] = char(kStendMicrostepCoef);
    sendStendCmd(LtpCmd::STEND_SPEED, p);

    // Многоскоростной цикл: фиксируем МАКСИМАЛЬНУЮ заданную скорость (19.07.2026,
    // регистратор пишет одну запись на весь цикл → сравниваем по max_rpm пуша).
    int maxVal = firstVal;
    for (int v : m_stend.steps) maxVal = qMax(maxVal, v);

    // Показываем левый столбик сразу — Старт, Скорость и плановая длительность известны
    StendCycleRecord rec;
    rec.startStend   = m_stend.cycleStartTs;
    rec.speedStend   = maxVal;
    rec.multiSpeed   = (n > 1);   // сравнение по max_rpm (иначе avg, как откалибровано)
    rec.speeds       = m_stend.steps;   // заданные скорости полок (для многострочного вывода)
    rec.plannedDurS  = durS;
    rec.inProgress   = true;
    m_stend.history.append(rec);
    while (m_stend.history.size() > 200) {   // держим весь прогон (до 200), панель прокручиваемая
        const StendCycleRecord &evicted = m_stend.history.first();
        if (!evicted.inProgress) {
            m_sessStendAcc += evicted.plannedDurS;
            if (evicted.hasRegData) m_sessRegAcc += evicted.durationRegS;
        }
        stendJournalWrite(m_stend.history.first());
        m_stend.history.removeFirst();
    }
    stendRenderReport();

    const int segMs = qMax(1, (durS * 1000) / qMax(1, n));
    m_stendStepTimer.start(segMs);
}

void MainWindow::stendAdvanceWorkStep()
{
    if (!m_stend.running)
        return;
    m_stend.stepIndex++;
    const int n = m_stend.steps.size();

    if (m_stend.stepIndex >= n) {
        stendFinishCycle();
        return;
    }

    // Шаги уже построены в правильном порядке — просто берём следующий
    const int val = m_stend.steps[m_stend.stepIndex];
    m_stend.lastSpeed = val;

    QByteArray p(3, 0);
    p[0] = char(quint16(val) & 0xFF);
    p[1] = char((quint16(val) >> 8) & 0xFF);
    p[2] = char(kStendMicrostepCoef);
    sendStendCmd(LtpCmd::STEND_SPEED, p);

    // Длительность сегмента — равные доли общей длительности фазы, заданной
    // в stendBeginWork(); интервал таймера не менялся с тех пор, переиспользуем.
    m_stendStepTimer.start(m_stendStepTimer.interval());
}

void MainWindow::stendBeginPause()
{
    if (!m_stend.running)
        return;
    m_stend.phase = StendPhase::Pause;
    const int pMin = ui->spinPauseMinMin->value() * 60 + ui->spinPauseMinSec->value();
    const int pMax = ui->spinPauseMaxMin->value() * 60 + ui->spinPauseMaxSec->value();
    const int pS   = QRandomGenerator::global()->bounded(pMin, pMax + 1);
    m_stendStepTimer.start(qMax(1, pS * 1000));
}

// ── Режим N старт/стоп (треугольный обход, калибровка порога) ───────────

void MainWindow::stendNextNStartStop()
{
    if (!m_stend.running || m_stend.triangleSteps.isEmpty())
        return;

    m_stend.phase = StendPhase::Work;
    // Лок-степ: закрываем предыдущую ждущую строку до вывода новой (см.
    // stendResolvePendingReg / stendBeginWork).
    stendResolvePendingReg();
    m_stend.cycleStartTs = QDateTime::currentDateTime();
    const int val = m_stend.triangleSteps[m_stend.triangleIdx];
    m_stend.lastSpeed = val;
    m_stend.steps = { val };
    m_stend.stepIndex = 0;

    QByteArray p(3, 0);
    p[0] = char(quint16(val) & 0xFF);
    p[1] = char((quint16(val) >> 8) & 0xFF);
    p[2] = char(kStendMicrostepCoef);
    sendStendCmd(LtpCmd::STEND_SPEED, p);

    const int durMin = ui->spinWorkMinMin->value() * 60 + ui->spinWorkMinSec->value();
    const int durMax = ui->spinWorkMaxMin->value() * 60 + ui->spinWorkMaxSec->value();
    const int durS   = QRandomGenerator::global()->bounded(durMin, durMax + 1);

    // Показываем левый столбик сразу — всё известно при старте цикла
    StendCycleRecord rec;
    rec.startStend   = m_stend.cycleStartTs;
    rec.speedStend   = val;
    rec.plannedDurS  = durS;
    rec.inProgress   = true;
    m_stend.history.append(rec);
    while (m_stend.history.size() > 200) {   // держим весь прогон (до 200), панель прокручиваемая
        const StendCycleRecord &evicted = m_stend.history.first();
        if (!evicted.inProgress) {
            m_sessStendAcc += evicted.plannedDurS;
            if (evicted.hasRegData) m_sessRegAcc += evicted.durationRegS;
        }
        stendJournalWrite(m_stend.history.first());
        m_stend.history.removeFirst();
    }
    stendRenderReport();

    m_stendStepTimer.start(qMax(1, durS * 1000));

    // Подготовить индекс к следующему шагу (треугольный обход Мин↔Макс)
    if (m_stend.triangleSteps.size() > 1) {
        if (m_stend.triangleIdx + m_stend.triangleDir >= m_stend.triangleSteps.size() ||
            m_stend.triangleIdx + m_stend.triangleDir < 0)
            m_stend.triangleDir = -m_stend.triangleDir;
        m_stend.triangleIdx += m_stend.triangleDir;
    }
}

// ── Завершение цикла: STOP, запись в историю, пауза ──────────────────────

void MainWindow::stendRecordCycle()
{
    // «Циклов» в «Наработке» = сколько раз (циклов) прогналось за эту сессию
    // (04.07.2026). Считаем на КАЖДОЕ завершение цикла (эта функция зовётся
    // один раз на цикл — из stendFinishCycle или ручного stendStop), а не по
    // пушам регистратора (тот дробит → счёт был то завышен, то занижен).
    // m_pushCount переиспользован под этот счётчик (сброс в stendStart).
    m_pushCount++;
    if (!m_stend.history.isEmpty() && m_stend.history.last().inProgress) {
        // Дописываем Стоп/Скорость в уже показанную запись
        StendCycleRecord &rec = m_stend.history.last();
        rec.stopStend  = QDateTime::currentDateTime();
        // speedStend НЕ перезаписываем: при старте туда записана МАКС скорость
        // цикла (многоскоростной), lastSpeed = последняя ступенька — не она.
        rec.inProgress = false;
        m_stend.totalStendMs += rec.startStend.msecsTo(rec.stopStend);
    } else {
        // Страховка: запись не была добавлена при старте — добавляем сейчас
        StendCycleRecord rec;
        rec.startStend = m_stend.cycleStartTs;
        rec.stopStend  = QDateTime::currentDateTime();
        rec.speedStend = m_stend.lastSpeed;
        m_stend.totalStendMs += rec.startStend.msecsTo(rec.stopStend);
        m_stend.history.append(rec);
        while (m_stend.history.size() > 200) {   // держим весь прогон (до 200), панель прокручиваемая
            const StendCycleRecord &evicted = m_stend.history.first();
            if (!evicted.inProgress) {
                m_sessStendAcc += evicted.plannedDurS;
                if (evicted.hasRegData) m_sessRegAcc += evicted.durationRegS;
            }
            m_stend.history.removeFirst();
        }
    }
    stendRenderReport();
    stendUpdateFlashStat();   // «Циклов» в «Наработке» изменился — обновить панель

    // Цикл закрыт → втянуть отложенный РАННИЙ пуш, если был (19.07.2026,
    // фикс «нет ответа» 1-го цикла: пуш приходил до закрытия строки и
    // выбрасывался, хотя запись во Flash жива).
    if (!m_regPushOrphans.isEmpty())
        stendFillRegColumn(m_regPushOrphans.takeFirst());
}

void MainWindow::stendFinishCycle()
{
    // Конец сегмента «Работа» между циклами: стоп мотору идёт надёжно
    // (очередь, ACK+ретраи — см. sendStendCmd, 09.07.2026). Это межцикловый
    // стоп самой циклограммы (не ручной teardown) — регистратор здесь ещё
    // работает, но команда редкая и идемпотентная, канал держится лишь на
    // квитирование, живой опрос практически не страдает.
    sendStendCmd(LtpCmd::STEND_STOP);
    stendRecordCycle();

    if (!m_stend.running)
        return;   // остановлен вручную во время этого вызова — без новой паузы

    // И для обычных режимов, и для N старт/стоп — после «Работы» идёт пауза
    // (см. CLAUDE.md: для N старт/стоп длительность ступеньки — та же логика
    // Мин/Макс «Время работы»/«Время простоя», отдельного поля не нужно).
    stendBeginPause();
}

// Суб-скорости полок (0x29, 19.07.2026) — приходят СРАЗУ после CYCLE_PUSH того
// же цикла (только сервис/тест). Привязываем к самой свежей записи с regData,
// куда только что лёг пуш; в отчёте многоскоростной цикл раскладывается по
// полкам «задано → измерено».
void MainWindow::stendFillSubSpeeds(const QByteArray &payload)
{
    if (payload.size() < 3) return;
    const quint8 *d = reinterpret_cast<const quint8 *>(payload.constData());
    const int cnt = d[0];
    if (cnt < 1 || payload.size() < 1 + 2 * cnt) return;
    QVector<int> subs;
    for (int i = 0; i < cnt; ++i) {
        quint16 v = 0; memcpy(&v, d + 1 + 2 * i, 2);
        subs.append(int(v));
    }
    // Кладём в самую свежую запись, у которой уже есть пуш и ещё нет полок.
    for (int i = m_stend.history.size() - 1; i >= 0; --i) {
        StendCycleRecord &r = m_stend.history[i];
        if (r.hasRegData && r.regSubSpeeds.isEmpty()) {
            r.regSubSpeeds = subs;
            stendRenderReport();
            return;
        }
    }
}

// Лок-степ вывода (04.07.2026, по модели пользователя): следующая строка
// цикла появляется ТОЛЬКО после того, как по предыдущей выведен результат
// регистратора. Момент решения «пришёл/не пришёл» = старт следующего цикла
// (пауза уже прошла — весь её срок регистратору был дан на пуш). Если пуша по
// предыдущей завершённой строке так и нет — фиксируем «нет ответа»
// (регистратор пропустил цикл) и ЗАКРЫВАЕМ строку, чтобы поздний/чужой пуш в
// неё уже не попал — это и убирает съезд колонки «Регистратор» на один цикл.
void MainWindow::stendResolvePendingReg()
{
    for (int i = m_stend.history.size() - 1; i >= 0; --i) {
        StendCycleRecord &r = m_stend.history[i];
        if (r.inProgress) continue;                 // живая строка — не трогаем
        if (r.hasRegData || r.regNoAnswer) return;  // уже закрыта (заполнена/пропуск)
        r.regNoAnswer = true;                       // за всю паузу пуша не было → пропуск
        appendLog(QStringLiteral(
            "Стенд: цикл (скорость %1) — регистратор не ответил, пропуск")
                  .arg(r.speedStend));
        return;                                     // закрываем только самую свежую ждущую
    }
}

/* Разобрать CMD_CYCLE_PUSH (0x20) от регистратора и заполнить правую
 * колонку последней незаполненной записи в истории циклов стенда.
 *
 * Payload 16 байт (LE):
 *   [0..5]   RTC_DateTime (year+2000, month, day, hour, min, sec)
 *   [6..9]   duration_s  uint32
 *   [10..13] total_s     uint32
 *   [14..15] max_rpm     uint16
 */
void MainWindow::stendFillRegColumn(const QByteArray &payload)
{
    if (payload.size() < 16) return;

    const quint8 *d = reinterpret_cast<const quint8 *>(payload.constData());

    QDateTime startReg = QDateTime(
        QDate(2000 + d[0], d[1], d[2]),
        QTime(d[3], d[4], d[5]));

    quint32 durS   = 0, totS = 0;
    quint16 avgRpm = 0, maxRpm = 0;
    memcpy(&durS,   d + 6,  4);
    memcpy(&totS,   d + 10, 4);
    memcpy(&avgRpm, d + 14, 2);                              // [14..15] avg_rpm
    if (payload.size() >= 18) memcpy(&maxRpm, d + 16, 2);   // [16..17] max_rpm (расширенный пуш)
    else maxRpm = avgRpm;
    // «Скорость» регистратора: многоскоростной цикл сравниваем по max_rpm (avg
    // размазан по циклу), одиночный — по avg (как откалибровано). Выбор — при
    // присвоении speedReg по флагу best->multiSpeed (ниже).

    // Всегда обновляем последнее известное totS — оно используется в stendStart()
    // для снятия базовой линии в момент нажатия «Старт».
    m_lastRegTotS = qint64(totS);   // lifetime «Общая» — валиден из каждого пуша

    // ЖИВЫЕ «Данные» во время «Работы» (18.07.2026): расширенный пуш (22 байта,
    // прошивка 17:4x+) несёт max_rpm и пики обоих каналов вибрации (мг) —
    // дописываем цикл в графики дашборда сразу, не дожидаясь «Стоп» (журнал во
    // сне не читается). Полный rescan после «Стоп» всё перечитает точно.
    if (payload.size() >= 22 && durS >= 2 && durS != 0xFFFFFFFFu) {
        quint16 pkRpm = 0, v1p = 0, v2p = 0, v1rms = 0;
        memcpy(&pkRpm, d + 16, 2);
        memcpy(&v1p,   d + 18, 2);
        memcpy(&v2p,   d + 20, 2);
        if (payload.size() >= 24) memcpy(&v1rms, d + 22, 2);  // vib1_RMS «уровень»
        const double idx = double(m_arc.plotKeys.size());
        m_arc.plotKeys.append(idx);
        m_arc.plotDuration.append(double(durS));
        m_arc.plotRpm.append(double(pkRpm));
        m_arc.plotVibro.append(double(v1p));
        m_arc.plotVibroRms.append(double(v1rms ? v1rms : v1p));  // fallback на пик
        m_arc.plotVib2.append(double(v2p));
        // plotTs — секунды от 2000-01-01 (база rtcToSec)
        m_arc.plotTs.append(double(QDateTime(QDate(2000,1,1), QTime(0,0))
                                       .secsTo(startReg)));
        m_arc.maxRpm   = qMax(m_arc.maxRpm,   float(pkRpm));
        m_arc.maxVibro = qMax(m_arc.maxVibro, float(v1p));
        m_arc.maxVib2  = qMax(m_arc.maxVib2,  float(v2p));
        m_arc.durationTotal = totS;
        m_arc.haveDuration  = true;
        archiveUpdateDashboard();   // перерисовать графики/подписи «Данных»
    }

    // Живая занятость Flash: каждый пуш = записанный цикл. В «Тесте» устройство
    // на связи → перезапускаем двоичный поиск, чтобы «Занято/Адрес записи» росли
    // по мере записи, а не застывали (04.07.2026: «застряли на стр. 28»). Гейт
    // внутри flashBinSearchStart пропустит «Работу» (там регистратор спит);
    // guard m_binSearch.running не даёт накладываться на ещё идущий поиск.
    flashBinSearchStart();

    // Позиционный матчинг: первый пришедший пуш → первый незаполненный ЗАВЕРШЁННЫЙ цикл.
    // inProgress-циклы пропускаем: пуш от decel хвоста предыдущего цикла не должен
    // попасть в уже запущенный следующий (иначе durS будет неверным).
    // Временны́е метки не используются для матчинга — работает даже без синхронизации RTC.
    StendCycleRecord *best = nullptr;
    for (int i = 0; i < m_stend.history.size(); ++i) {
        StendCycleRecord &rec = m_stend.history[i];
        // Закрытые «нет ответа» строки (regNoAnswer) пропускаем — пуш в них уже
        // не кладём (иначе столбец «Регистратор» съедет на один цикл).
        if (!rec.hasRegData && !rec.inProgress && !rec.regNoAnswer) { best = &rec; break; }
    }

    // Диагностика в лог: видно что пришло и куда (или отброшено)
    int bestIdx = -1;
    if (best) {
        for (int i = 0; i < m_stend.history.size(); ++i)
            if (&m_stend.history[i] == best) { bestIdx = i; break; }
    }
    appendLog(QStringLiteral("Рег.пуш: durS=%1с totS=%2с → %3")
              .arg(durS).arg(totS)
              .arg(best ? QStringLiteral("цикл#%1").arg(bestIdx + 1)
                        : QStringLiteral("отброшен")));

    if (!best) {
        // РАННИЙ пуш (строка цикла ещё inProgress / не создана) — НЕ выбрасываем
        // (19.07.2026, фикс «нет ответа» 1-го цикла при живой Flash-записи):
        // откладываем и втянем при закрытии цикла (stendRecordCycle). Держим
        // максимум 3 — от накопления мусора при рассинхроне.
        if (durS >= 2 && m_regPushOrphans.size() < 3) {
            m_regPushOrphans.append(payload);
            appendLog(QStringLiteral("Рег.пуш: строки ещё нет — отложен (%1 в очереди)")
                          .arg(m_regPushOrphans.size()));
        }
        // Общую/Сессию всё равно обновляем — lifetime totS из пуша валиден.
        stendUpdateFlashStat();
        stendRenderReport();
        return;  // нет подходящего незаполненного завершённого цикла
    }
    best->hasRegData   = true;
    best->startReg     = startReg;
    best->speedReg     = int(best->multiSpeed ? maxRpm : avgRpm);   // см. regRpm выше
    best->durationRegS = qint64(durS);
    m_sessRegMatched  += qint64(durS);   // «Сессия»: добавляем ТОЛЬКО обработанный ответ
    // totalRegS = сумма durationRegS всех предыдущих видимых циклов + текущий.
    // НЕ берём totS из прошивки: он включает тормозные хвосты мотора в паузах между
    // стенд-циклами (прошивка пушит их, но LOGLSMW выбрасывает → в totS растёт смещение).
    {
        qint64 cumRegS = qint64(durS);
        for (int j = 0; j < m_stend.history.size(); ++j) {
            const StendCycleRecord &c = m_stend.history.at(j);
            if (&c == best) break;
            if (c.hasRegData) cumRegS += c.durationRegS;
        }
        best->totalRegS = cumRegS;
    }
    // Панель «Наработка» — ПОСЛЕ накопления m_sessRegMatched (21.07.2026: раньше
    // звалась строкой выше, до инкремента, — «Сессия» показывала предыдущее
    // значение и «отставала» на один цикл до следующего пуша).
    stendUpdateFlashStat();
    if (!best->inProgress)
        stendJournalWrite(*best);
    stendRenderReport();

    // Живой бар занятости Flash (12.07.2026): каждый матч-пуш = 1 запись во
    // Flash (~kRecordsPerPage на страницу). В «Работе» устройство спит →
    // device-scan (flashBinSearchStart выше) гейтится, бар бы застыл; ведём
    // инкрементальную оценку от числа записей, чтобы «Тест памяти»/панель
    // показывали ЖИВОЕ заполнение памяти данными. В «Тесте»/«Данных» device-scan
    // потом скорректирует до реального значения.
    m_flashLiveRecords++;
    // База захватывается ЛЕНИВО на первом пуше из РЕАЛЬНОГО m_firstFreePage
    // (device-scan при подключении/после «Стоп»), не из накопленной оценки.
    if (m_flashLiveBasePage < 0)
        m_flashLiveBasePage = m_firstFreePage;
    if (m_flashLiveBasePage >= 0) {
        const int est = m_flashLiveBasePage + int(m_flashLiveRecords / kRecordsPerPage);
        m_firstFreePage = qMin(est, int(kFlashTotalPages));
        flashBinSearchUpdateUi();
    }
    // Живой HEX-дамп записей в «Тест памяти» — ОДНОСТРАНИЧНЫЙ вид (12.07.2026,
    // по запросу). Показываем ВСЕГДА одну ПОЛНУЮ страницу (256 байт) целиком, на
    // постоянном месте: пустое = 0xFF (как стёртая Flash), приход записи заменяет
    // FF на своём слоте (слот×24) реальными байтами (реконструкция байт-в-байт с
    // прошивкой, fwBuildRecord). Никакого частичного/растущего вывода: страница
    // выводится сразу целиком. Как страница заполнилась (kRecordsPerPage записей),
    // следующая запись целиком СБРАСЫВАЕТ её в чистую (все FF) и начинает
    // заполнять новую страницу (номер += 1). Видно живое заполнение памяти
    // реальным содержимым, пока устройство спит (без его чтения).
    if (ui->txtHexDump && m_flashLiveBasePage >= 0) {
        constexpr int kMaxLiveDumpPages = 8;   // сколько последних страниц держим на экране
        const qint64 idx = m_flashLiveRecords - 1;                 // 0-based индекс записи
        const int    off = int(idx / kRecordsPerPage) * 256        // абс. смещение от базы:
                         + int(idx % kRecordsPerPage) * kRecordBytes; // страница×256 + слот×48
        // Достраиваем буфер ДО ПОЛНОЙ страницы с этой записью (хвост = 0xFF) — всегда
        // целая страница, не частичный вывод.
        const int need = (off / 256 + 1) * 256;
        if (m_flashLiveBuf.size() < need)
            m_flashLiveBuf.append(QByteArray(need - m_flashLiveBuf.size(), char(0xFF)));
        m_flashLiveBuf.replace(off, kRecordBytes, fwBuildRecord(startReg, durS, totS, maxRpm));
        // Прокручиваемый вид (12.07.2026, по запросу): копим полные страницы,
        // показываем ПОСЛЕДНИЕ kMaxLiveDumpPages — новая (заполняемая) уходит вниз,
        // сверху виден хвост уже заполненной памяти. Буфер храним целиком (память
        // копеечная), но рисуем только хвост — рендер не растёт на длинных прогонах.
        const int pages = m_flashLiveBuf.size() / 256;
        const int shown = qMin(pages, kMaxLiveDumpPages);
        renderHexDump(quint16(m_flashLiveBasePage + pages - shown),
                      m_flashLiveBuf.right(shown * 256));
        ui->txtHexDump->moveCursor(QTextCursor::End);   // новая страница — от самого низа
    }
}

void MainWindow::stendRenderReport()
{
    // Заголовок колонок — закреплённый lblCmpHeader над панелью (03.07.2026),
    // в прокручиваемый текст больше не пишется.
    QString text;
    // Начинаем с сессионных аккумуляторов — они хранят суммы циклов,
    // уже вышедших из видимого 3-циклового окна. Без этого «Общее» сбрасывалась
    // бы в ~0 при каждом сдвиге окна (цикл 4 выталкивает цикл 1, и т.д.).
    qint64 acc    = m_sessStendAcc;   // стенд: сессионная база + текущее окно
    qint64 accReg = m_sessRegAcc;     // регистратор: то же самое
    bool first = true;

    auto hms = [](qint64 s) {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600, 2, 10, QLatin1Char('0'))
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    };

    // Колонки Стенд/Регистратор — ПО ПРАВОМУ краю (19.07.2026): чтобы времена и
    // цифры полок стояли единообразно (раньше времена слева, полки справа —
    // «одни в начале, другие в конце»). Ширина 13.
    auto col = [](const QString &s) { return s.rightJustified(13); };
    // Колонка «Стенд» — на 1 симв. уже (21.07.2026, по просьбе: сдвинуть влево
    // относительно «Регистратор»/«Ошибка», которые остаются на месте — между
    // col1 и col() везде добавлен один явный пробел-компенсатор).
    auto col1 = [](const QString &s) { return s.rightJustified(12); };

    // Дельта в секундах → "+N с" (04.07.2026: короткая «с» вместо секунда/…)
    auto secDelta = [](qint64 d) -> QString {
        if (d == 0) return QStringLiteral("0 с");
        const QString sign = d > 0 ? QStringLiteral("+") : QStringLiteral("−");
        return QStringLiteral("%1%2 с").arg(sign).arg(qAbs(d));
    };

    for (const StendCycleRecord &r : m_stend.history) {
        if (!first) text += QStringLiteral("\n");
        first = false;

        if (r.inProgress) {
            // Цикл идёт: левый столбик из плановых данных, правый — пуст
            const qint64 intervalS = r.plannedDurS;
            const qint64 totalS    = acc + intervalS;  // acc накоплен в секундах
            const QDateTime stopPlan = r.startStend.addSecs(intervalS);
            text += QStringLiteral("Старт     %1\n").arg(col1(r.startStend.toString(QStringLiteral("HH:mm:ss"))));
            text += QStringLiteral("Стоп      %1\n").arg(col1(stopPlan.toString(QStringLiteral("HH:mm:ss"))));
            text += QStringLiteral("Интервал  %1\n").arg(col1(hms(intervalS)));
            text += QStringLiteral("Общее     %1\n").arg(col1(hms(totalS)));
            // Полки известны ЗАРАНЕЕ (стенд их задал) — показываем сразу при
            // старте цикла, измеренный столбец пустой до суб-пуша (19.07.2026).
            if (r.multiSpeed && r.speeds.size() > 1) {
                int maxIdx = 0;
                for (int k = 1; k < r.speeds.size(); ++k)
                    if (r.speeds[k] > r.speeds[maxIdx]) maxIdx = k;
                // Основная строка — плановый максимум, тем же форматом, что и
                // одиночная (измеренный столбец появится после суб-пуша).
                text += QStringLiteral("Скорость  %1\n").arg(col1(QString::number(r.speeds[maxIdx])));
                // Черта во всю ширину строки, от самого начала (21.07.2026, по
                // просьбе) — отделяет доп. блок «Полки» (только для теста) от
                // основной записи.
                text += QString(46, QChar(0x2500)) + QStringLiteral("\n");
                for (int k = 0; k < r.speeds.size(); ++k)
                    text += QStringLiteral("%1%2%3\n")
                        .arg(k == 0 ? QStringLiteral("Полки     ") : QStringLiteral("          "))
                        .arg(QString::number(r.speeds[k]).rightJustified(12))
                        .arg(k == maxIdx ? QStringLiteral("  ◄") : QString());
            } else {
                text += QStringLiteral("Скорость  %1\n").arg(col1(QString::number(r.speedStend)));
            }
        } else {
            // Цикл завершён.
            // Стенд = точно что задано (plannedDurS), накапливаем в секундах — нет дрейфа.
            // Регистратор = реальные данные из CMD_CYCLE_PUSH (durationRegS / totalRegS).
            const qint64 intervalS = r.plannedDurS;
            acc += intervalS;
            const qint64 totalS = acc;
            if (r.hasRegData) accReg += r.durationRegS;  // зеркально acc, без тормозных хвостов

            if (r.regNoAnswer) {
                // Регистратор пропустил цикл (пуш не пришёл за паузу) — правая
                // колонка пустая, метка «нет ответа» в колонке «Ошибка».
                // «Общее» стенда растёт, регистратора — стоит: разрыв виден
                // дальше как реальная нехватка наработки.
                text += QStringLiteral("Старт     %1\n").arg(col1(r.startStend.toString(QStringLiteral("HH:mm:ss"))));
                text += QStringLiteral("Стоп      %1\n").arg(col1(r.stopStend.toString(QStringLiteral("HH:mm:ss"))));
                text += QStringLiteral("Интервал  %1 %2  нет ответа\n").arg(col1(hms(intervalS)), col(QString()));
                text += QStringLiteral("Общее     %1\n").arg(col1(hms(totalS)));
                text += QStringLiteral("Скорость  %1\n").arg(col1(QString::number(r.speedStend)));
            } else if (!r.hasRegData) {
                // Правая колонка ещё не пришла (текущая ждущая строка) — только
                // левая, но той же шириной col(), что и заполненная строка ниже
                // (21.07.2026: раньше без col() — строка была короче и визуально
                // «съезжала» влево, пока не придёт пуш, затем «прыгала» вправо).
                text += QStringLiteral("Старт     %1\n").arg(col1(r.startStend.toString(QStringLiteral("HH:mm:ss"))));
                text += QStringLiteral("Стоп      %1\n").arg(col1(r.stopStend.toString(QStringLiteral("HH:mm:ss"))));
                text += QStringLiteral("Интервал  %1\n").arg(col1(hms(intervalS)));
                text += QStringLiteral("Общее     %1\n").arg(col1(hms(totalS)));
                text += QStringLiteral("Скорость  %1\n").arg(col1(QString::number(r.speedStend)));
            } else {
                // Обе колонки + дельта
                const QDateTime stopReg   = r.startReg.addSecs(r.durationRegS);
                const qint64    dInterval = r.durationRegS - intervalS;
                const qint64    dTotal    = accReg - totalS;  // сумма durationRegS vs сумма plannedDurS
                const double    dSpeedPct = r.speedStend > 0
                    ? double(r.speedReg - r.speedStend) / r.speedStend * 100.0
                    : 0.0;
                const QString   dSpeedStr = dSpeedPct >= 0
                    ? QStringLiteral("+%1%").arg(dSpeedPct, 0, 'f', 1)
                    : QStringLiteral("−%1%").arg(-dSpeedPct, 0, 'f', 1);

                // Старт/Стоп: время «Стенд» (ПК) показываем ВСЕГДА — формат
                // жёсткий, 5 строк на цикл (04.07.2026: «откуда вывод без
                // Старт/Стоп»). Регистратора — только при синхронном RTC
                // (год<2020 = не синхронизирован после прошивки → его метки
                // мусорные (1970), оставляем пусто, но строки не исчезают).
                const bool timeSynced = r.startReg.date().year() >= 2020;
                text += QStringLiteral("Старт     %1 %2\n")
                    .arg(col1(r.startStend.toString(QStringLiteral("HH:mm:ss"))))
                    .arg(timeSynced ? col(r.startReg.toString(QStringLiteral("HH:mm:ss"))) : QString());
                text += QStringLiteral("Стоп      %1 %2\n")
                    .arg(col1(r.stopStend.toString(QStringLiteral("HH:mm:ss"))))
                    .arg(timeSynced ? col(stopReg.toString(QStringLiteral("HH:mm:ss"))) : QString());
                text += QStringLiteral("Интервал  %1 %2  %3\n")
                    .arg(col1(hms(intervalS)))
                    .arg(col(hms(r.durationRegS)))
                    .arg(secDelta(dInterval).rightJustified(8));
                text += QStringLiteral("Общее     %1 %2  %3\n")
                    .arg(col1(hms(totalS)))
                    .arg(col(hms(accReg)))
                    .arg(secDelta(dTotal).rightJustified(8));
                // ОСНОВНАЯ строка — ВСЕГДА «Скорость», тем же форматом, что и у
                // одиночной (21.07.2026, по ТЗ фиксируется максимум за рабочий
                // цикл). r.speedStend/r.speedReg уже несут МАКС (заданный и
                // max_rpm измеренный) — это же значение уходит во Flash;
                // основная запись не меняется независимо от того, была полка
                // одна или несколько.
                text += QStringLiteral("Скорость  %1 %2  %3\n")
                    .arg(col1(QString::number(r.speedStend)))
                    .arg(col(QString::number(r.speedReg)))
                    .arg(dSpeedStr.rightJustified(8));

                if (r.multiSpeed && !r.regSubSpeeds.isEmpty() && !r.speeds.isEmpty()) {
                    // ДОПОЛНИТЕЛЬНО (для тестирования/диагностики стенда) —
                    // полный набор полок «задано → измерено». «Полки» — подпись
                    // ПЕРВОЙ строки данных (не отдельная строка-заголовок), как и
                    // «Скорость»/«Старт» выше — единообразно, без лишней высоты.
                    // Черта во всю ширину строки, от самого начала (21.07.2026).
                    text += QString(46, QChar(0x2500)) + QStringLiteral("\n");
                    // СОПОСТАВЛЕНИЕ ПО ЗНАЧЕНИЮ (19.07.2026): позиционное съезжало,
                    // когда близкие заданные скорости сливались в одну полку
                    // (измеренная полка попадала не в свою строку, +540%). Каждую
                    // ИЗМЕРЕННУЮ полку кладём к БЛИЖАЙШЕЙ по значению заданной
                    // (не занятой); заданные без своей полки (слились) — пустые.
                    const int nc = r.speeds.size();
                    QVector<int> meas(nc, -1);
                    for (int mv : r.regSubSpeeds) {
                        int best = -1, bestD = 1 << 30;
                        for (int k = 0; k < nc; ++k) {
                            if (meas[k] >= 0) continue;
                            const int dd = qAbs(r.speeds[k] - mv);
                            if (dd < bestD) { bestD = dd; best = k; }
                        }
                        if (best >= 0) meas[best] = mv;
                    }
                    // Макс = заданная с наибольшей ИЗМЕРЕННОЙ — та же полка,
                    // что уже показана строкой выше как «Скорость»; здесь просто
                    // помечаем её «◄» среди остальных, для наглядности.
                    int maxIdx = -1, maxV = -1;
                    for (int k = 0; k < nc; ++k)
                        if (meas[k] > maxV) { maxV = meas[k]; maxIdx = k; }
                    for (int k = 0; k < nc; ++k) {
                        const QString sSet  = QString::number(r.speeds[k]);
                        const QString sMeas = meas[k] >= 0 ? QString::number(meas[k]) : QString();
                        QString dStr;
                        if (meas[k] >= 0 && r.speeds[k] > 0) {
                            const double p = double(meas[k] - r.speeds[k]) / r.speeds[k] * 100.0;
                            dStr = (p >= 0 ? QStringLiteral("+%1%").arg(p, 0, 'f', 1)
                                           : QStringLiteral("−%1%").arg(-p, 0, 'f', 1));
                        }
                        const QString mark = (k == maxIdx)
                            ? QStringLiteral("  ◄") : QString();
                        // Подпись только у первой строки («Полки», 10 симв. —
                        // как «Скорость  »/«Старт     » и т.д.), дальше — тот же
                        // отступ без подписи. Цифры и дельта — по правому краю,
                        // теми же ширинами (13/13/8), что и в строке «Скорость»
                        // выше, — колонки совпадают по вертикали.
                        text += QStringLiteral("%1%2 %3  %4%5\n")
                            .arg(k == 0 ? QStringLiteral("Полки     ") : QStringLiteral("          "))
                            .arg(sSet.rightJustified(12))
                            .arg(sMeas.rightJustified(13))
                            .arg(dStr.rightJustified(8)).arg(mark);
                    }
                }
            }
        }
    }
    ui->cmpReport->setPlainText(text);

    QTextCursor c(ui->cmpReport->document());
    c.select(QTextCursor::Document);
    QTextBlockFormat fmt;
    fmt.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
    c.mergeBlockFormat(fmt);

    // Прокрутка к свежему циклу (низ) — во время прогона виден текущий; после
    // «Стоп» рендер не идёт, можно свободно листать вверх весь прогон (до 200
    // циклов держим в истории, 04.07.2026: «должен контролировать весь цикл»).
    ui->cmpReport->moveCursor(QTextCursor::End);
    ui->cmpReport->ensureCursorVisible();
}

// ── Журнал испытания на диске ─────────────────────────────────────────────

void MainWindow::stendJournalOpen()
{
    if (m_journalFile.isOpen())
        m_journalFile.close();
    // Снапшот текущей lifetime-наработки как база «Сессии».
    // m_lastRegTotS уже актуален: либо из GET_STATS при подключении, либо из
    // последнего CYCLE_PUSH. «Сессия» = m_lastRegTotS - m_preTestRegTotS (растёт
    // с каждым новым пушем). «Общая» (панель «Наработка») = m_lastRegTotS (живая
    // наработка устройства).
    // RESET_TOTAL больше не вызывается — нельзя обнулять lifetime в прошивке.
    m_preTestRegTotS = m_lastRegTotS;   // база Сессии = всё что было до «Старт»
    m_pushCount      = 0;
    m_regPushOrphans.clear();   // чистый прогон — без чужих отложенных пушей
    m_journalAccMs   = 0;
    m_journalRegAccS = 0;
    m_regBaseTotal   = 0;
    stendUpdateFlashStat();

    const QString ts   = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                         + QStringLiteral("/LogLSM_stend_") + ts + QStringLiteral(".txt");
    m_journalFile.setFileName(path);
    if (!m_journalFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("Стенд: не удалось создать журнал: ") + path);
        return;
    }
    QTextStream s(&m_journalFile);
    s << QStringLiteral("Журнал испытания стенда  %1\n")
         .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")));
    s << QString(40, QChar('-')) << QStringLiteral("\n\n");
    m_journalFile.flush();
    ui->btnOpenJournal->setEnabled(true);
    appendLog(QStringLiteral("Стенд: журнал → ") + path);
}

// ── Панель «Наработка» (groupUptime) + «Память устройства» (groupFlashStat) ──
// «Наработка»: lifetime-наработка устройства («Общая» — из CYCLE_PUSH.total_s
// / GET_STATS) и нарастание за текущую сессию («Сессия» = Общая − снапшот на
// момент «Старт»). «Память устройства» — отдельная коробка ниже, индикатор
// занятости Flash (flashBinSearchUpdateUi()).
void MainWindow::stendUpdateFlashStat()
{
    if (!ui->lblFlashStat) return;
    auto hms = [](qint64 s) -> QString {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600,        2, 10, QLatin1Char('0'))
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60,          2, 10, QLatin1Char('0'));
    };

    // m_lastRegTotS = lifetime total устройства (из CYCLE_PUSH.total_s / GET_STATS).
    // «Сессия» = сумма ТОЛЬКО обработанных (совпавших) ответов регистратора
    // (m_sessRegMatched) — никаких фантомных времён: нет ответа → не добавляем
    // (04.07.2026; раньше было total_s−снапшот и росло от несовпавших фрагментов).
    const qint64 sessRegTotS = m_sessRegMatched;

    const QString vTotal  = m_lastRegTotS  > 0 ? hms(m_lastRegTotS) : QString();
    const QString vSess   = sessRegTotS    > 0 ? hms(sessRegTotS)   : QString();
    const QString vCycles = m_pushCount   > 0 ? QString::number(m_pushCount) : QString();

    // HTML-таблица: метка слева, значение прижато вправо.
    // Строка «Общая» — грамматически согласуется с заголовком коробки
    // «Наработка» (Наработка: Общая/Сессия/Циклов), отдельно от «Общее»
    // в cmpReport (там согласование с другим подразумеваемым словом —
    // не путать; коробки визуально разделены заголовками).
    const QString text = QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='0'>"
        "<tr><td>Общая:</td><td align='right'>%1</td></tr>"
        "<tr><td>Сессия:</td><td align='right'>%2</td></tr>"
        "<tr><td>Циклов:</td><td align='right'>%3</td></tr>"
        "</table>")
        .arg(vTotal).arg(vSess).arg(vCycles);

    ui->lblFlashStat->setText(text);

    // Полоса памяти (в отдельной коробке «Память устройства»)
    flashBinSearchUpdateUi();
}

void MainWindow::stendJournalWrite(StendCycleRecord &r)
{
    if (!m_journalFile.isOpen() || r.inProgress || r.written)
        return;

    // Фиксированные ширины колонок (в символах, включая trailing пробелы)
    static constexpr int W_LBL  = 10;  // «Интервал  » — самая длинная метка
    static constexpr int W_DATA = 13;  // «HH:mm:ss    » или «300          »

    auto lbl = [](const QString &s) { return s.leftJustified(W_LBL); };
    auto col = [](const QString &s) { return s.leftJustified(W_DATA); };

    auto hms = [](qint64 s) -> QString {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600, 2, 10, QLatin1Char('0'))
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    };
    auto fmtDt = [](const QDateTime &dt) -> QString {
        return dt.toString(QStringLiteral("HH:mm:ss"));
    };
    auto secDelta = [](qint64 d) -> QString {
        if (d == 0) return QStringLiteral("0 с");
        const QString sign = d > 0 ? QStringLiteral("+") : QStringLiteral("−");
        return QStringLiteral("%1%2 с").arg(sign).arg(qAbs(d));
    };

    // Стенд = точно что задано; накопленное m_journalAccMs хранится в мс
    // только для совместимости с типом, реально это целые секунды * 1000.
    const qint64 intervalS = r.plannedDurS;
    const qint64 totalS    = m_journalAccMs / 1000LL + intervalS;

    QTextStream s(&m_journalFile);

    const QString sep = QStringLiteral(" | ");

    // Заголовок: пустая метка-колонка (10 ш.), затем «Стенд»/«Регистратор» над
    // соответствующими данными, «Ошибка» над колонкой дельты (не проваливается
    // без подписи — тот же принцип, что и в живой панели cmpReport).
    s << lbl(QString()) << sep << col(QStringLiteral("Стенд"))
      << sep << col(QStringLiteral("Регистратор")) << sep << QStringLiteral("Ошибка\n");

    if (!r.hasRegData) {
        s << lbl(QStringLiteral("Старт"))    << sep << fmtDt(r.startStend)           << "\n";
        s << lbl(QStringLiteral("Стоп"))     << sep << fmtDt(r.stopStend)            << "\n";
        s << lbl(QStringLiteral("Интервал")) << sep << hms(intervalS)                << "\n";
        s << lbl(QStringLiteral("Общее"))    << sep << hms(totalS)                   << "\n";
        s << lbl(QStringLiteral("Скорость")) << sep << r.speedStend                  << "\n\n";
    } else {
        const QDateTime stopReg   = r.startReg.addSecs(r.durationRegS);
        const qint64    dInterval = r.durationRegS - intervalS;
        // totalRegS = running накопитель (все уже записанные Рег-циклы) + текущий,
        // аналогично m_journalAccMs для Стенда. r.totalRegS — сумма внутри окна истории,
        // и сбрасывается при сдвиге окна — не использовать для файла журнала.
        const qint64    totalRegS = m_journalRegAccS + r.durationRegS;
        const qint64    dTotal    = totalRegS - totalS;
        const double    dSpeedPct = r.speedStend > 0
            ? double(r.speedReg - r.speedStend) / r.speedStend * 100.0 : 0.0;
        const QString   dSpeedStr = dSpeedPct >= 0
            ? QStringLiteral("+%1%").arg(dSpeedPct, 0, 'f', 1)
            : QStringLiteral("−%1%").arg(-dSpeedPct, 0, 'f', 1);

        s << lbl(QStringLiteral("Старт"))
          << sep << col(fmtDt(r.startStend)) << sep << fmtDt(r.startReg)            << "\n";
        s << lbl(QStringLiteral("Стоп"))
          << sep << col(fmtDt(r.stopStend))  << sep << fmtDt(stopReg)               << "\n";
        // Дельта пишется ВСЕГДА, даже при 0 (secDelta(0) → «0 секунд») — иначе
        // колонка «Ошибка» пропадает у части строк и файл выглядит «рваным»
        // (замечено пользователем 02.07: некоторые блоки без третьей колонки).
        s << lbl(QStringLiteral("Интервал"))
          << sep << col(hms(intervalS))      << sep << col(hms(r.durationRegS))
          << sep << secDelta(dInterval)                                             << "\n";
        s << lbl(QStringLiteral("Общее"))
          << sep << col(hms(totalS))         << sep << col(hms(totalRegS))
          << sep << secDelta(dTotal)                                                << "\n";
        s << lbl(QStringLiteral("Скорость"))
          << sep << col(QString::number(r.speedStend))
          << sep << col(QString::number(r.speedReg))
          << sep << dSpeedStr                                                         << "\n\n";
    }
    m_journalFile.flush();
    m_journalAccMs += (qint64)r.plannedDurS * 1000LL;  // хранится как мс, но всегда целые секунды
    if (r.hasRegData) m_journalRegAccS += r.durationRegS;  // Рег-накопитель: только при наличии данных
    r.written = true;
}

void MainWindow::applyTheme(bool dark)
{
    m_darkTheme = dark;

    qApp->setStyle(QStringLiteral("Fusion"));
    QPalette pal;
    if (dark) {
        pal.setColor(QPalette::Window,          QColor(0x2D,0x2D,0x30));
        pal.setColor(QPalette::WindowText,      QColor(0xDC,0xDC,0xDC));
        pal.setColor(QPalette::Base,            QColor(0x25,0x25,0x26));
        pal.setColor(QPalette::AlternateBase,   QColor(0x35,0x35,0x37));
        pal.setColor(QPalette::Text,            QColor(0xDC,0xDC,0xDC));
        pal.setColor(QPalette::Button,          QColor(0x3A,0x3A,0x3D));
        pal.setColor(QPalette::ButtonText,      QColor(0xDC,0xDC,0xDC));
        pal.setColor(QPalette::ToolTipBase,     QColor(0x2D,0x2D,0x30));
        pal.setColor(QPalette::ToolTipText,     QColor(0xDC,0xDC,0xDC));
        pal.setColor(QPalette::PlaceholderText, QColor(0x80,0x80,0x80));
        pal.setColor(QPalette::Highlight,       QColor(0x2F,0x6F,0xB0));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x70,0x70,0x70));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70,0x70,0x70));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x70,0x70,0x70));
    } else {
        pal.setColor(QPalette::Window,          QColor(0xEC,0xEA,0xE4));
        pal.setColor(QPalette::WindowText,      QColor(0x22,0x22,0x22));
        pal.setColor(QPalette::Base,            Qt::white);
        pal.setColor(QPalette::AlternateBase,   QColor(0xF5,0xF4,0xF0));
        pal.setColor(QPalette::Text,            QColor(0x22,0x22,0x22));
        pal.setColor(QPalette::Button,          Qt::white);
        pal.setColor(QPalette::ButtonText,      QColor(0x22,0x22,0x22));
        pal.setColor(QPalette::ToolTipBase,     Qt::white);
        pal.setColor(QPalette::ToolTipText,     QColor(0x22,0x22,0x22));
        pal.setColor(QPalette::PlaceholderText, QColor(0x99,0x99,0x99));
        pal.setColor(QPalette::Highlight,       QColor(0x2F,0x6F,0xB0));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0xAA,0xAA,0xAA));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xAA,0xAA,0xAA));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xAA,0xAA,0xAA));
    }
    qApp->setPalette(pal);

    ThemePalette t;
    if (dark) {
        t.bg="#2D2D30"; t.card="#252526"; t.cardBorder="#3F3F46"; t.inputBorder="#4A4A4F";
        t.text="#DCDCDC"; t.textDim="#B0B0B0"; t.sectionText="#C8C8C8";
        t.menuHover="#3A3A3D"; t.tabSelText="#FFFFFF"; t.accent="#3D8BD4";
        t.factBg="#1E1E1F"; t.logText="#C8C8C8";
    } else {
        t.bg="#ECEAE4"; t.card="#FFFFFF"; t.cardBorder="#E4E2DC"; t.inputBorder="#C8C6C0";
        t.text="#222222"; t.textDim="#666666"; t.sectionText="#444444";
        t.menuHover="#DDD9D0"; t.tabSelText="#222222"; t.accent="#2F6FB0";
        t.factBg="#F5F4F0"; t.logText="#444444";
    }
    applyStyles(t);
    restyleThemedPlots();
}

void MainWindow::setupThemeMenu()
{
    QMenu *viewMenu = new QMenu(QStringLiteral("Вид"), this);
    QAction *actDark = viewMenu->addAction(QStringLiteral("Тёмная тема"));
    actDark->setCheckable(true);
    actDark->setChecked(m_darkTheme);
    // Вставляем «Вид» перед меню «Помощь», чтобы «Помощь» осталась последней.
    // Если меню «Помощь» в .ui называется иначе — поправь menuHelp на своё имя.
    menuBar()->insertMenu(ui->menuHelp->menuAction(), viewMenu);
    connect(actDark, &QAction::toggled, this, &MainWindow::setDarkTheme);
}

void MainWindow::setDarkTheme(bool on)
{
    applyTheme(on);
    QSettings(kOrg, kApp).setValue(QStringLiteral("ui/darkTheme"), on);
    appendLog(on ? QStringLiteral("Тема: тёмная") : QStringLiteral("Тема: светлая"));
}

// Перекраска QCustomPlot под тему. Графики мониторинга (plotAcc/plotGyro/
// plotTemp) имеют видимые оси/сетку — им перекрашиваем фон, оси, подписи,
// сетку и легенду. Мини-графики дашборда фон не трогаем — он прозрачный,
// цвет даёт карточка под ними (меняется через таблицу стилей).
void MainWindow::restyleThemedPlots()
{
    const QColor bg(m_darkTheme ? QColor(0x25,0x25,0x26) : Qt::white);
    const QColor fg(m_darkTheme ? QColor(0xDC,0xDC,0xDC) : QColor(0x22,0x22,0x22));
    const QColor grid(m_darkTheme ? QColor(0x46,0x46,0x49) : QColor(0xC8,0xC8,0xC8));

    for (QCustomPlot *p : { ui->plotAcc, ui->plotGyro }) {
        if (!p) continue;
        p->setBackground(bg);
        for (QCPAxis *ax : { p->xAxis, p->yAxis, p->xAxis2, p->yAxis2 }) {
            ax->setBasePen(QPen(fg));
            ax->setTickPen(QPen(fg));
            ax->setSubTickPen(QPen(fg));
            ax->setTickLabelColor(fg);
            ax->setLabelColor(fg);
            ax->grid()->setPen(QPen(grid, 1, Qt::DotLine));
        }
        if (p->legend) {
            p->legend->setBrush(QBrush(bg));
            p->legend->setTextColor(fg);
            p->legend->setBorderPen(QPen(grid));
        }
        p->replot();
    }
}

void MainWindow::applyStyles(const ThemePalette &t)
{
    // Цвета берутся из ThemePalette t (см. applyTheme) — это и есть переключатель тем.
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget#centralwidget { background: %1; }"
        "QTabWidget::pane { border: none; }"
        "QWidget#tabDashboard, QWidget#tabCommands, QWidget#tabMonitor,"
        "QWidget#tabMemTest, QWidget#tabStend, QWidget#tabLog, QWidget#tabSettings,"
        "QWidget#tabCalibration {"
        "  background: transparent; }"
        "QMenuBar { background: %1; }"
        "QMenuBar::item { padding: 4px 10px; background: transparent; }"
        "QMenuBar::item:selected { background: %7; border-radius: 4px; }"
        "QMenu { background: %2; border: 1px solid %4; color: %5; }"
        "QMenu::item:selected { background: %7; color: %5; }"
        "QTabBar::tab { background: transparent; padding: 5px 14px;"
        "  color: %6; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %8; font-weight: 600;"
        "  border-bottom: 2px solid %9; }"
        "QTabBar::tab:hover { color: %8; }"
        "QPushButton { background: %2; border: 1px solid %4;"
        "  border-radius: 6px; padding: 4px 12px; color: %5; }"
        "QPushButton:hover { background: %7; }"
        "QPushButton:pressed { background: %7; }"
        "QPushButton:checked { background: %7; border-color: %9; }"
        "QPushButton:disabled { color: %6; background: %7; }"
        "QPushButton#btnActivate { padding: 0; border-radius: 2px; font-size: 9px; }"
        "QPushButton#btnInd, QPushButton#btnScan { padding: 0; font-weight: 600; }"
        "QComboBox, QLineEdit, QSpinBox { background: %2;"
        "  border: 1px solid %4; border-radius: 6px; padding: 3px 8px; color: %5; }"
        "QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border-color: %9; }"
        "QComboBox QAbstractItemView { background: %2;"
        "  border: 1px solid %4; selection-background-color: %7;"
        "  selection-color: %5; color: %5; }"
        /* Заголовок группы — ВНУТРИ рамки (03.07.2026, запрос: «Циклограмма
         * испытания» выпадала за край окна). Раньше: subcontrol-origin:
         * margin + margin-top:12 — титул сидел НА линии рамки в margin-зоне
         * и у верхнего края вкладки обрезался. Теперь origin: border +
         * top-смещение — титул опущен внутрь коробки, padding-top группы
         * увеличен, чтобы содержимое не наезжало на него. */
        "QGroupBox { background: %2; border: 1px solid %3;"
        "  border-radius: 8px; margin-top: 4px; padding-top: 16px; color: %5; }"
        "QGroupBox::title { subcontrol-origin: border;"
        "  subcontrol-position: top center; top: 6px;"
        "  padding: 0 6px; color: %6; font-weight: 600; }"
        "QGroupBox#groupMonCtrl { margin-top: 0; padding-top: 4px; }"
        /* Правое окно «Стенда»: поднять содержимое вверх (шапка Стенд/
         * Регистратор/Ошибка на уровень заголовка «Циклограмма» слева) —
         * больше высоты выводу cmpReport, 3-й «Скорость» влезает (04.07.2026) */
        "QGroupBox#groupCompare { margin-top: 0; padding-top: 4px; }"
        "QLabel#lblFlashStatTitle { color: %6; font-weight: 600; }"
        "QFrame#frameResults, QFrame#frameInfo, QFrame#frameActivation,"
        "QFrame#frameDevCard {"
        "  background: %2; border: 1px solid %3; border-radius: 8px; }"
        "QPlainTextEdit { background: %2; border: 1px solid %3;"
        "  border-radius: 8px; color: %11; }"
        "QProgressBar { background: %7; border: none; border-radius: 3px;"
        "  height: 12px; text-align: center; font-size: 9px; color: %6; }"
        "QProgressBar::chunk { background: #1D7A4C; border-radius: 3px; }"
        "QLabel { color: %5; }"
        "QCheckBox, QRadioButton { color: %5; }"
        "QLabel#secTime, QLabel#secCycles, QLabel#secTemp, QLabel#secRestarts,"
        "QLabel#secPower {"
        "  color: %12; font-weight: 600; font-size: 11px;"
        "  border-top: 1px solid %3; padding-top: 5px; }"
        "QLabel#secTime { border-top: none; padding-top: 0; }"
        "QLabel#lblUptimeCaption, QLabel#lblMaxSpeedCaption,"
        "QLabel#lblMaxVibroCaption, QLabel#lblMaxVibro2Caption,"
        "QLabel#lblPcTimeCaption,"
        "QLabel#lblDevTimeCaption, QLabel#lblTimeDiffCaption,"
        "QLabel#lblCyclesUsedCaption, QLabel#lblCyclesFreeCaption,"
        "QLabel#lblTempCurCaption, QLabel#lblTempMaxCaption,"
        "QLabel#lblRestartTimerCaption, QLabel#lblRestartPowerCaption,"
        "QLabel#lblVddaCaption { color: %6; font-size: 11px; }"
        "QLabel#lblMaxSpeed { color: %9; font-weight: 600; }"
        "QLabel#lblMaxVibro { color: #D05050; font-weight: 600; }"
        "QLabel#lblMaxVibro2 { color: #E08A20; font-weight: 600; }"
        "QLabel#lblActStatus { color: #C9A227; font-weight: 600; font-size: 11px; }"
        "QStatusBar { background: %1; color: %6; }"
        "QStatusBar QLabel { color: %6; }"
        "QPushButton#btnMemWrite, QPushButton#btnMemRead {"
        "  background: %7; border: 1px solid %9; color: %5; font-weight: 600; }"
        "QPushButton#btnMemErasePage, QPushButton#btnMemEraseSector,"
        "QPushButton#btnMemEraseChip {"
        "  background: #7A3C00; border: 1px solid #B85C00; color: #FFE4B5; font-weight: 600; }"
        "QFrame#frameMemInfo { background: %2; border: 1px solid %3;"
        "  border-radius: 8px; min-width: 80px; }"
        "QLabel#lblCurByte, QLabel#lblCurPages, QLabel#lblCurActivePage,"
        "QLabel#lblCurSpi, QLabel#lblCurCycle, QLabel#lblCurErrors,"
        "QLabel#lblCurStep, QLabel#lblImgPages, QLabel#lblImgAddr,"
        "QLabel#lblImgPagesNA, QLabel#lblImgAddrNA {"
        "  border: 1px solid %3; border-radius: 6px;"
        "  background: %10; padding: 2px 8px;"
        "  min-width: 50px; max-width: 90px; }"
        /* Скорость программы и индикаторы температуры на «Мониторинге» —
         * компактные «факт»-рамки (без min-width/жирного: место — графикам) */
        "QLabel#lblMonStendSpeed, QLabel#lblMonTLsm,"
        "QLabel#lblMonTTmp, QLabel#lblMonTStm {"
        "  border: 1px solid %3; border-radius: 6px;"
        "  background: %10; padding: 2px 6px; }"
        /* Период/Окно — подпись+список в общей рамке-блоке */
        "QFrame#frameMonPeriod, QFrame#frameMonWindow {"
        "  border: 1px solid %3; border-radius: 6px; }"
        "QGroupBox#groupMemParams QSpinBox::up-button,"
        "QGroupBox#groupMemParams QSpinBox::down-button { width: 0; border: none; }"
        "QToolTip { color: %5; background: %2; border: 1px solid %3; }"
        )
        .arg(t.bg)          // %1
        .arg(t.card)        // %2
        .arg(t.cardBorder)  // %3
        .arg(t.inputBorder) // %4
        .arg(t.text)        // %5
        .arg(t.textDim)     // %6
        .arg(t.menuHover)   // %7
        .arg(t.tabSelText)  // %8
        .arg(t.accent)      // %9
        .arg(t.factBg)      // %10
        .arg(t.logText)     // %11
        .arg(t.sectionText) // %12
    );

    QFont val(kMono);
    val.setBold(true);
    for (QLabel *l : { ui->lblPcTime, ui->lblDevTime, ui->lblTimeDiff,
                       ui->lblCyclesUsed, ui->lblCyclesFree,
                       ui->lblTempCur, ui->lblTempMax,
                       ui->lblRestartTimer, ui->lblRestartPower,
                       ui->lblVdda,
                       ui->lblMaxSpeed, ui->lblMaxVibro,
                       ui->lblMemTmpVal, ui->lblMemLsmVal, ui->lblMemStmVal,
                       ui->lblVddVal,    ui->lblVbatVal })
        l->setFont(val);

    QFont logf(kMono);
    logf.setPointSize(8);
    ui->logBrief->setFont(logf);
    ui->logView->setFont(logf);
    ui->logMon->setFont(logf);

    QFont cmpFont(kMono);
    cmpFont.setPointSize(10);
    ui->cmpReport->setFont(cmpFont);
    ui->lblCmpHeader->setFont(cmpFont);   // тот же моноширинный — колонки совпадают

    // Индикатор связи — цвет зависит от состояния (ставится в setConnectedUi),
    // не от темы. Здесь — стартовый «не подключён» (красный).
    ui->btnInd->setStyleSheet(QStringLiteral(
        "QPushButton#btnInd { background: #C03030;"
       " border: 1px solid #9A2626; border-radius: 12px;"
       " color: rgba(255,255,255,140); font-size: 12px; }"));
}






// ── Тестовый цикл (вкладка «Тест памяти») ───────────────────────────────────

void MainWindow::setOpsEnabled(bool on, QWidget *except)
{
    for (QWidget *w : { (QWidget*)ui->btnMemWrite,  (QWidget*)ui->btnMemRead,
                        (QWidget*)ui->btnMemErasePage, (QWidget*)ui->btnMemEraseSector,
                        (QWidget*)ui->btnMemEraseChip, (QWidget*)ui->btnTempRun,
                        (QWidget*)ui->btnMemReadImg,
                        (QWidget*)ui->btnImgRG, (QWidget*)ui->btnImgLOG })
        w->setEnabled(on || w == except);
}

// Разбор Intel HEX (записи 00=данные, 01=EOF, 04=extended linear address;
// прочие типы — 02/03/05 и т.п. — в наших дампах не встречаются, пропускаются).
// На выходе: pages[i] — 256 байт страницы (startPage + i); пропуски адресов
// в файле заполняются 0xFF (как на чистой NOR Flash — см. README дампов).
bool MainWindow::loadImageFromHexFile(const QString &path, quint16 &startPage,
                                       QList<QByteArray> &pages, QString &errMsg) const
{
    pages.clear();
    startPage = 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errMsg = QStringLiteral("не удалось открыть файл");
        return false;
    }

    QMap<quint32, quint8> flat;   // абсолютный адрес → байт (только записанные байты)
    quint32 extHigh = 0;          // старшие 16 бит адреса (запись типа 04)
    QTextStream in(&f);
    int lineNo = 0;
    bool sawEof = false;

    while (!in.atEnd() && !sawEof) {
        ++lineNo;
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        if (!line.startsWith(QLatin1Char(':'))) {
            errMsg = QStringLiteral("строка %1: запись не начинается с ':'").arg(lineNo);
            return false;
        }
        const QString hex = line.mid(1);
        if (hex.length() < 10 || hex.length() % 2 != 0) {
            errMsg = QStringLiteral("строка %1: некорректная длина записи").arg(lineNo);
            return false;
        }

        bool ok = true;
        auto byteAt = [&](int pos) -> quint8 {
            return quint8(hex.mid(pos, 2).toUInt(&ok, 16));
        };

        const int    len  = byteAt(0);
        const quint8 ah   = byteAt(2);
        const quint8 al   = byteAt(4);
        const quint8 type = byteAt(6);
        if (!ok || hex.length() != 10 + 2 * len) {
            errMsg = QStringLiteral("строка %1: длина записи не совпадает с полем LEN").arg(lineNo);
            return false;
        }

        QByteArray data;
        data.reserve(len);
        for (int i = 0; i < len; ++i) data.append(char(byteAt(8 + 2 * i)));
        const quint8 cc = byteAt(8 + 2 * len);
        if (!ok) {
            errMsg = QStringLiteral("строка %1: не hex-строка").arg(lineNo);
            return false;
        }

        quint32 sum = quint32(len) + ah + al + type + cc;
        for (int i = 0; i < data.size(); ++i) sum += quint8(data.at(i));
        if ((sum & 0xFF) != 0) {
            errMsg = QStringLiteral("строка %1: неверная контрольная сумма").arg(lineNo);
            return false;
        }

        const quint32 addr = extHigh + ((quint32(ah) << 8) | al);
        switch (type) {
        case 0x00:   // данные
            for (int i = 0; i < len; ++i) flat.insert(addr + quint32(i), quint8(data.at(i)));
            break;
        case 0x01:   // конец файла
            sawEof = true;
            break;
        case 0x04:   // extended linear address
            if (len == 2)
                extHigh = (quint32(quint8(data.at(0))) << 24) | (quint32(quint8(data.at(1))) << 16);
            break;
        default:
            break;   // не используется в наших дампах — игнорируем
        }
    }

    if (flat.isEmpty()) {
        errMsg = QStringLiteral("файл не содержит данных (нет записей типа 00)");
        return false;
    }

    const quint16 firstPage = quint16(flat.firstKey() >> 8);
    const quint16 lastPage  = quint16(flat.lastKey()  >> 8);
    startPage = firstPage;

    for (quint32 pg = firstPage; pg <= lastPage; ++pg) {
        QByteArray page(256, char(0xFF));
        for (int off = 0; off < 256; ++off) {
            const auto it = flat.constFind((pg << 8) + quint32(off));
            if (it != flat.constEnd()) page[off] = char(it.value());
        }
        pages.append(page);
    }
    return true;
}

// Поставить разобранный образ в очередь FLASH_WRITE; m_imgCache/m_imgStartPage
// сохраняются для архива (archiveStart), а прогресс на кнопке обновляется в
// onResponse() при каждом подтверждении FLASH_WRITE (TagImg).
void MainWindow::startImageWrite(QPushButton *btn, const QString &label,
                                  quint16 startPage, const QList<QByteArray> &pages,
                                  const QString &fileName)
{
    m_imgCache      = pages;
    m_imgStartPage  = startPage;
    m_imgPagesTotal = pages.size();
    m_imgPagesDone  = 0;
    m_imgActiveBtn  = btn;
    m_imgBtnLabel   = label;
    m_imgFileName   = fileName;
    setOpsEnabled(false);   // обе Образ-кнопки отключены — нет смысла прерывать запись

    const QString pfx = (btn == ui->btnImgRG) ? QStringLiteral("RG") : QStringLiteral("LOG");
    btn->setText(QStringLiteral("%1 0/%2 ■").arg(pfx).arg(m_imgPagesTotal));

    for (int i = 0; i < pages.size(); ++i) {
        const quint16 pg = quint16(startPage + i);
        QByteArray pkt;
        pkt.append(char(pg & 0xFF));
        pkt.append(char((pg >> 8) & 0xFF));
        pkt.append(pages.at(i));
        requestCmd(LtpCmd::FLASH_WRITE, pkt, TagImg);
    }
}

// Подсветка кнопок «Образ RG/LOG» зелёным — какой образ сейчас фактически
// лежит в Flash (по последней успешной записи или по проверке стр.1 при
// заходе на вкладку «Тест памяти», см. probeFlashImageState). Если ничего
// не записано (стр.1 = 0xFF) — обе кнопки обычные, без подсветки.
//
// Заодно обновляет lblDataFormat в самой верхней строке окна — короткую
// подпись «Регистратор» / «Logger», чтобы сразу было видно, с каким
// форматом журнала сейчас работаем, не заходя на вкладку «Тест памяти».
void MainWindow::refreshImgButtonsHighlight()
{
    const QString hi = QStringLiteral(
        "background:#C8E6C9; border:1px solid #2E7D32; color:#1B5E20; font-weight:600;");
    ui->btnImgRG->setStyleSheet(
        m_flashImageState == FlashImageState::Registrator ? hi : QString());
    ui->btnImgLOG->setStyleSheet(
        m_flashImageState == FlashImageState::Logger ? hi : QString());

    switch (m_flashImageState) {
    case FlashImageState::Registrator:
        ui->lblDataFormat->setText(QStringLiteral("● Регистратор"));
        ui->lblDataFormat->setStyleSheet(QStringLiteral("color:#1B5E20; font-weight:600;"));
        break;
    case FlashImageState::Logger:
        ui->lblDataFormat->setText(QStringLiteral("● Logger"));
        ui->lblDataFormat->setStyleSheet(QStringLiteral("color:#1A4F80; font-weight:600;"));
        break;
    case FlashImageState::Empty:
        ui->lblDataFormat->setText(QStringLiteral("Flash пуста"));
        ui->lblDataFormat->setStyleSheet(QStringLiteral("color:#999999;"));
        break;
    case FlashImageState::Unknown:
        ui->lblDataFormat->setText(QString());
        ui->lblDataFormat->setStyleSheet(QString());
        break;
    }
}

// Разовое чтение страницы 1 при заходе на вкладку «Тест памяти» — чтобы
// понять, что сейчас лежит в Flash, и подсветить соответствующую кнопку
// «Образ RG/LOG» (не дожидаясь новой записи в этой же сессии). Эвристика:
// байт [0] страницы — 0xFF, если она вообще не записана (конец журнала);
// иначе для Logger v2 это всегда один из типов фрейма 0xF6..0xFE (см.
// data_format_spec_v1.md), для Регистратора v2 — младший байт unix-времени
// начала цикла, который физически не может совпасть с этим диапазоном для
// дат из разумного будущего/прошлого (риск ложной классификации — единицы
// процентов, это вспомогательная подсказка в UI, а не критичная проверка).
void MainWindow::probeFlashImageState()
{
    if (!m_link->isOpen() || m_arc.running || m_test.running
        || m_tempRun.running || m_erasing || m_imgActiveBtn)
        return;
    const quint32 addr = quint32(kLogStartPage) << 8;
    QByteArray p;
    for (int i = 0; i < 4; ++i) p.append(char((addr >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) p.append(char((256 >> (8 * i)) & 0xFF));
    m_dev->enqueue(LtpCmd::FLASH_READ, p, quint32(TagProbe));
}

// ── Бинарный поиск первой свободной страницы Flash ──────────────────────────
// NOR Flash после стирания = 0xFF. Данные пишутся последовательно с kLogStartPage.
// Инвариант: страницы [kLogStartPage..firstFree-1] — заняты, [firstFree..] — 0xFF.
// Алгоритм: log2(65536) = 16 запросов FLASH_READ максимум.
// Сигнал завершения — вызов stendUpdateFlashStat() + flashBinSearchUpdateUi().
void MainWindow::flashBinSearchStart()
{
    if (!m_link || !m_link->isOpen()) return;
    if (m_binSearch.running) return;   // уже идёт
    // Не конкурировать трафиком только с «Работой» (0x1D): там регистратор
    // спит в Stop2 и на FLASH_READ не ответит → «нет ответа Flash». В «Тесте»
    // (0x23, m_stendNoReg) устройство всё время на связи — поиск разрешён,
    // occupancy обновляется живьём по мере записи (04.07.2026: «застряли на
    // стр. 28»). По завершении любого прогона поиск всё равно дозапустится из
    // stendStop().
    if (m_stendActive && !m_stendNoReg) return;
    // НЕ сбрасываем m_firstFreePage в -1: при повторном поиске (живое обновление
    // в «Тесте») держим последнее значение на экране до прихода нового —
    // иначе панель мигает между данными и «сканирование…» (04.07.2026).
    m_binSearchFailed = false;
    m_binSearch = {};
    m_binSearch.running = true;
    m_binSearch.lo = int(kLogStartPage);      // 1
    m_binSearch.hi = int(kFlashTotalPages);   // 65536 — sentinel «чип полностью занят»
    flashBinSearchSendNext();
}

void MainWindow::flashBinSearchSendNext()
{
    if (!m_binSearch.running) return;
    if (m_stendActive && !m_stendNoReg) {
        // Только «Работа» (0x1D): регистратор спит — приостанавливаем поиск
        // (lo/hi/step сохранены), пробуем снова через секунду, пока не
        // остановится. В «Тесте» устройство отвечает — поиск идёт как обычно.
        QTimer::singleShot(1000, this, [this] { flashBinSearchSendNext(); });
        return;
    }
    if (m_binSearch.lo >= m_binSearch.hi) {
        // Поиск завершён — lo == hi == первая свободная (или 65536 = Flash полна)
        m_firstFreePage = m_binSearch.lo;
        m_binSearch.running = false;
        const int used = m_firstFreePage - int(kLogStartPage);
        const QString msg = (m_firstFreePage >= int(kFlashTotalPages))
            ? QStringLiteral("Flash: чип заполнен")
            : QStringLiteral("Flash: первая свободная стр. %1 (занято %2 стр.)")
                  .arg(m_firstFreePage).arg(qMax(0, used));
        appendLog(msg);
        stendUpdateFlashStat();
        // Авто-дамп при входе на «Тест памяти» (17.07.2026): границы теперь
        // свежие → показываем реальное содержимое. Гейты: та же вкладка, не
        // идёт чтение, не активен стенд.
        if (m_memAutoDumpPending) {
            m_memAutoDumpPending = false;
            if (ui->tabsMain->currentWidget() == ui->tabMemTest
                && !m_test.running && !m_stendActive)
                memTestDump(m_autoDumpStart, m_autoDumpCount);  // образ→свой диапазон,
                                                                // иначе Старт/Страниц
            m_autoDumpStart = m_autoDumpCount = -1;
        }
        return;
    }
    const int mid = (m_binSearch.lo + m_binSearch.hi) / 2;
    m_binSearch.midSent = mid;
    m_binSearch.step++;
    flashBinSearchUpdateUi();   // обновить счётчик шагов до отправки
    const quint32 addr = quint32(mid) << 8;
    QByteArray p;
    for (int i = 0; i < 4; ++i) p.append(char((addr >> (8*i)) & 0xFF));
    for (int i = 0; i < 4; ++i) p.append(char((256  >> (8*i)) & 0xFF));
    m_dev->enqueue(LtpCmd::FLASH_READ, p, TagBinSearch);
}

void MainWindow::flashBinSearchHandlePage(const QByteArray &data)
{
    if (!m_binSearch.running) return;
    // Поиск получает ответы — Flash жива, обнуляем накопленный бюджет
    // ретраев (03.07.2026: раньше сбрасывался только при переподключении,
    // 5 неудач накапливались по разным эпизодам — например, пока
    // устройство спало в режиме A — и панель залипала в «нет ответа
    // Flash» при давно живой Flash).
    m_binSearchRetries = 0;
    // Страница свободна (never written), если все байты = 0xFF
    bool allFree = true;
    for (char b : data) { if (quint8(b) != 0xFF) { allFree = false; break; } }
    if (allFree)
        m_binSearch.hi = m_binSearch.midSent;      // первая свободная ≤ mid
    else
        m_binSearch.lo = m_binSearch.midSent + 1;  // первая свободная > mid
    flashBinSearchSendNext();
}

void MainWindow::flashBinSearchUpdateUi()
{
    if (!ui->barFlashMem) return;
    const int total = int(kFlashTotalPages) - int(kLogStartPage);  // 65535

    ui->barFlashMem->setRange(0, total);

    if (m_binSearchFailed) {
        ui->barFlashMem->setValue(0);
        ui->barFlashMem->setFormat(QStringLiteral("Flash не отвечает"));
        if (m_flashCellLbl) m_flashCellLbl->hide();
        // Явная подсказка вместо пустоты (05.07.2026): чаще всего Flash молчит
        // не из-за питания, а потому что регистратор спит в Stop2 («Работа») —
        // на FLASH_READ отвечать некому. Сброс/переподключение оживит.
        if (ui->lblFlashDetail)
            ui->lblFlashDetail->setText(
                QStringLiteral("устройство не отвечает — возможно, спит (Stop2); "
                               "сброс/переподключение"));
        return;
    }

    // «сканирование…»/«—» показываем ТОЛЬКО пока данных ещё нет (первый поиск).
    // При повторном поиске (живое обновление в «Тесте») m_firstFreePage хранит
    // прошлое значение — рисуем его дальше (ниже), не мигая (04.07.2026).
    if (m_firstFreePage < 0) {
        ui->barFlashMem->setValue(0);
        if (m_flashCellLbl) m_flashCellLbl->hide();
        if (m_binSearch.running && m_binSearch.step > 0)
            ui->barFlashMem->setFormat(
                QStringLiteral("сканирование… %1/16").arg(m_binSearch.step));
        else
            ui->barFlashMem->setFormat(m_binSearch.running
                ? QStringLiteral("сканирование…")
                : QStringLiteral("—"));
        if (ui->lblFlashDetail)
            ui->lblFlashDetail->clear();
        return;
    }

    const int used  = qBound(0, m_firstFreePage - int(kLogStartPage), total);
    const int free_ = total - used;

    // Адаптивный масштаб бара (04.07.2026, по запросу пользователя): при малой
    // занятости полный диапазон 65535 давал бы вечные ~0% — бар бесполезен.
    // «Старшее значение» (максимум бара) берём по ступеням чуть выше занятого,
    // чтобы малая занятость была видна как заметная заливка, растущая по мере
    // записи. Число в баре — абсолютные занятые страницы (не %), чтобы масштаб
    // не вводил в заблуждение; полная картина — в тексте ниже.
    static const int tiers[] = {10, 50, 100, 500, 1000, 5000, 10000, 50000, 65535};
    int scaleMax = total;
    for (int t : tiers) { if (t > used) { scaleMax = qMin(t, total); break; } }
    if (scaleMax < 1) scaleMax = 1;

    ui->barFlashMem->setRange(0, scaleMax);
    ui->barFlashMem->setValue(used);
    ui->barFlashMem->setFormat(QString());   // встроенный текст прячем — рисуем overlay
    // Номер свободной ячейки (первая свободная страница) ставим overlay-подписью
    // СРАЗУ ЗА КРАЕМ ШТРИХОВКИ, а не у правого края ленты — иначе кажется, что
    // число относится к концу чипа. Так видно: это ячейка сразу за занятой
    // зоной, куда ляжет следующая запись (05.07.2026, по запросу).
    if (m_flashCellLbl) {
        m_flashCellLbl->setText(m_firstFreePage >= int(kFlashTotalPages)
            ? QStringLiteral("чип полон") : QString::number(m_firstFreePage));
        m_flashCellLbl->adjustSize();
        const int bw = ui->barFlashMem->width();
        const int bh = ui->barFlashMem->height();
        int x = (scaleMax > 0) ? int((qint64)bw * used / scaleMax) + 4 : 4;
        if (x + m_flashCellLbl->width() > bw - 2)   // не вылезать за правый край
            x = bw - m_flashCellLbl->width() - 2;
        if (x < 2) x = 2;
        m_flashCellLbl->move(x, (bh - m_flashCellLbl->height()) / 2);
        m_flashCellLbl->show();
    }
    // На чипе есть реальная запись → разрешаем «Прочитать» (читает её по
    // границам поиска, даже без тестового образа этой сессии). 05.07.2026.
    if (ui->btnMemReadImg && used > 0 && !m_test.running)
        ui->btnMemReadImg->setEnabled(true);

    if (ui->lblFlashDetail) {
        // Адрес записи = первая свободная страница (куда ляжет следующая запись);
        // байтовый адрес = стр. << 8. Занято/Свободно — от полного объёма Flash.
        // «Записей/Циклов» убрано (сессионный «Циклов» есть слева в «Наработке»).
        // Адрес — ДЕСЯТИЧНЫЙ номер страницы, hex убран; значения выровнены в одну
        // колонку слева (04.07.2026: «hex никто не понимает», «в одну колонку —
        // удобно»). Таблица без width=100% → не тянется к краю, не режется.
        // Подпись — фиксированная ширина (значения выстраиваются в одну колонку),
        // но БЕЗ width=100% и align=right → значения у левого края, не режутся
        // узким окном (04.07.2026: align=right давал «Свободно: 6» за краем окна).
        ui->lblFlashDetail->setText(
            QStringLiteral(
                "<table cellspacing='0' cellpadding='2'>"
                "<tr><td width='100'>Свободно:</td><td>%1 стр.</td></tr>"
                "</table>")
            .arg(free_));
    }
}

// ── Архив (вкладка «Данные») ────────────────────────────────────────────────
// Читает обратно из Flash диапазон [startPage, startPage+pageCount) — именно
// тот, что был только что записан образом RG/LOG — и разбирает его как журнал
// (Регистратор v2 или Logger v2), заполняя вкладку «Данные». Цель — проверить
// корректность упаковки данных и корректность процедуры считывания: вкладка
// «Тест памяти» имитирует запись реальной работы, вкладка «Данные» должна
// корректно отобразить то, что было фактически загружено в Flash.
void MainWindow::archiveRescanFull()
{
    // Полный диапазон журнала — все страницы Flash, кроме страницы 0 (заголовок
    // устройства). Отступаем ещё на одну страницу от самого конца: pageLimit
    // хранится как quint16 и не может представить значение 65536 («одна
    // страница за диапазоном» для последней реальной страницы 65535) — для
    // журнала такого объёма потеря одной страницы на границе не существенна.
    if (!m_link->isOpen() || m_arc.running || m_test.running
        || m_tempRun.running || m_erasing)
        return;
    const ArchiveMode lastMode  = m_arc.mode;
    const int         pageCount = int(kFlashTotalPages) - int(kLogStartPage) - 1;
    archiveStart(lastMode, kLogStartPage, pageCount);
}

void MainWindow::archiveStart(ArchiveMode mode, quint16 startPage, int pageCount)
{
    if (pageCount <= 0) return;

    m_arc = ArchiveState{};
    m_arc.running   = true;
    m_arc.mode      = mode;
    m_arc.pageStart = startPage;
    m_arc.pageNext  = startPage;
    m_arc.pageLimit = quint16(startPage + pageCount);

    appendLog(QStringLiteral("[Архив] чтение журнала: стр.%1..%2 (%3)")
                  .arg(startPage).arg(m_arc.pageLimit - 1)
                  .arg(mode == ArchiveMode::Registrator ? QStringLiteral("Регистратор")
                                                         : QStringLiteral("Logger")));
    archiveRequestChunk();
}

void MainWindow::archiveRequestChunk()
{
    if (!m_arc.running) return;
    if (m_arc.pageNext >= m_arc.pageLimit) {
        archiveFinish();
        return;
    }
    const int pagesLeft = int(m_arc.pageLimit) - int(m_arc.pageNext);
    const int n         = qMin(pagesLeft, m_arc.chunkPages);
    const quint32 addr  = quint32(m_arc.pageNext) << 8;
    const quint32 sz    = quint32(n) * 256;

    QByteArray p;
    for (int i = 0; i < 4; ++i) p.append(char((addr >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) p.append(char((sz   >> (8 * i)) & 0xFF));

    m_arc.chunkRequested = n;
    m_dev->enqueue(LtpCmd::FLASH_READ, p, quint32(TagArchive));
}

void MainWindow::archiveHandleChunk(const QByteArray &data, int pagesInChunk)
{
    if (!m_arc.running) return;

    // ВАЖНО: прошивка (cmdReadMem() в com.c) отвечает на FLASH_READ не более
    // чем одной страницей (256 байт, буфер page[257]) за раз, независимо от
    // того, сколько страниц запрошено в пакете (archiveRequestChunk просит
    // сразу до chunkPages=32 страниц одним FLASH_READ). Раньше код доверял
    // запрошенному pagesInChunk и сдвигал pageNext на него же — в итоге
    // читалась и разбиралась только первая страница каждого «чанка», а
    // оставшиеся ~31/32 страниц журнала тихо пропускались без чтения. Из-за
    // этого Регистратор показывал только первую запись вместо всех, а Logger
    // (где первая страница — это только ЗАГОЛОВОК, без СТОП) не показывал
    // вообще ничего. Теперь ориентируемся на то, сколько страниц фактически
    // пришло (data.size()/256), а не на то, сколько было запрошено.
    const int pagesGot = qMin(pagesInChunk, data.size() / 256);

    if (pagesGot <= 0) {
        // Ничего не пришло — не уходим в бесконечный повтор запроса.
        archiveFinish();
        return;
    }

    for (int i = 0; i < pagesGot; ++i) {
        const QByteArray page = data.mid(i * 256, 256);
        if (page.size() < 256) break;   // защитная проверка — неполный ответ

        if (m_arc.mode == ArchiveMode::Registrator)
            archiveParseRegistratorPage(page, (m_arc.pageNext - m_arc.pageStart) + i);
        else
            archiveParseLoggerPage(page, (m_arc.pageNext - m_arc.pageStart) + i);
    }

    m_arc.pageNext = quint16(m_arc.pageNext + pagesGot);
    archiveRequestChunk();
}

// Регистратор запись v2: 5 записей по 48 байт в одной странице (240 байт,
// 16 байт хвоста — резерв; было 10×24 до 12.07.2026,
// см. data_format_spec_v1.md). pageOffset — смещение этой
// страницы от m_arc.pageStart (как и у archiveParseLoggerPage) — нужно
// только чтобы клампить m_arc.pageLimit, когда встретили первый незаписанный
// слот (дальше — гарантированно пусто, запись всегда строго последовательна).
void MainWindow::archiveParseRegistratorPage(const QByteArray &page, int pageOffset)
{
    const auto *d = reinterpret_cast<const quint8 *>(page.constData());

    for (int slot = 0; slot < kRegRecordsPerPage; ++slot) {
        const quint8 *r = d + slot * kRecordBytes;   // v2, 48 байт (глоб. kRecordBytes)
        quint32 tsStart, duration, durationTotal;
        float   rpmMax, rpmAvg, vib1Peak, vib1Rms, vib2Peak, vib2Rms;
        std::memcpy(&tsStart,       r + 0,  4);
        std::memcpy(&duration,      r + 4,  4);
        std::memcpy(&durationTotal, r + 8,  4);
        std::memcpy(&rpmMax,        r + 12, 4);
        std::memcpy(&rpmAvg,        r + 16, 4);
        std::memcpy(&vib1Peak,      r + 20, 4);
        std::memcpy(&vib1Rms,       r + 24, 4);
        std::memcpy(&vib2Peak,      r + 28, 4);
        std::memcpy(&vib2Rms,       r + 32, 4);
        const quint16 crc = quint16(r[46]) | (quint16(r[47]) << 8);
        // Пока в графики/сводку кладём КАНАЛ 1 пик как «вибрацию» и rpm_max как
        // «скорость» — совместимо с текущим UI. vib1_rms / vib2_peak / vib2_rms
        // доступны для отдельного отображения (follow-up, dual-channel view).
        // Санитизация вибрации (17.07.2026): в журнале с прошлых прогонов и
        // старых форматов встречаются мусорные записи — нефизичный vib
        // декодируется в гигантский («3e34 g») или отрицательный float, задирая
        // максимум и ломая график. Отбрасываем не-конечные, отрицательные и
        // заведомо нефизичные (> 60000 мг ≈ 60 g; предел датчика ±16g) → 0.
        auto saneVib = [](float v) {
            return (qIsFinite(v) && v >= 0.0f && v < 60000.0f) ? v : 0.0f; };
        vib1Peak = saneVib(vib1Peak);
        vib1Rms  = saneVib(vib1Rms);
        vib2Peak = saneVib(vib2Peak);
        vib2Rms  = saneVib(vib2Rms);
        const float maxVibro = vib1Peak;   // мг (пик канала 1) — «акселерометр»
        const float maxRpm   = rpmMax;
        (void)rpmAvg; (void)vib1Rms; (void)vib2Rms;

        if (tsStart == 0xFFFFFFFFu) {
            // Слот не записан — конец журнала (запись всегда строго
            // последовательна, дальше на этой и следующих страницах пусто).
            m_arc.pageLimit = quint16(qMin<quint32>(m_arc.pageLimit, m_arc.pageStart + pageOffset + 1));
            return;
        }

        if (m_arc.records == 0 && m_arc.brokenRecords == 0)
            m_arc.tsFirst = tsStart;

        // duration_total берём из последней записи, где поле физически записано
        // (rule data_format_spec_v1.md §6) — независимо от завершённости записи.
        if (durationTotal != 0xFFFFFFFFu) {
            m_arc.durationTotal = durationTotal;
            m_arc.haveDuration  = true;
        }

        const bool complete = (crc != 0xFFFF);
        if (complete) {
            ++m_arc.records;
            m_arc.haveComplete = true;
            m_arc.trailingOpen = false;
            // timestamp_end больше не хранится отдельно — конец цикла выводится
            // как timestamp_start + duration (data_format_spec_v1.md, 20.06.2026).
            m_arc.tsLast        = (duration != 0xFFFFFFFFu) ? (tsStart + duration) : tsStart;
            m_arc.maxVibro       = qMax(m_arc.maxVibro, maxVibro);
            m_arc.maxVib2        = qMax(m_arc.maxVib2, vib2Peak);
            m_arc.maxRpm         = qMax(m_arc.maxRpm, maxRpm);
            // Фильтр «мусора» (05.07.2026, по запросу): в графики НЕ кладём
            // вырожденные циклы длительностью < 2 с — это фрагменты дробления
            // (durS=0/1), дававшие короткие «иголки» вперемешку с реальными
            // циклами. Индекс графика — по размеру массива (плотно, без дыр),
            // чтобы отфильтрованные записи не оставляли разрывов. Счётчики
            // m_arc.records и по-записные recDur/recTs считают ВСЕ записи.
            if (duration != 0xFFFFFFFFu && duration >= 2u) {
                const double idx = double(m_arc.plotKeys.size());
                m_arc.plotKeys.append(idx);
                m_arc.plotVibro.append(double(maxVibro));
                m_arc.plotVibroRms.append(double(vib1Rms > 0.0f ? vib1Rms : maxVibro));
                m_arc.plotVib2.append(double(vib2Peak));
                m_arc.plotRpm.append(double(maxRpm));
                m_arc.plotTs.append(double(tsStart));
                m_arc.plotDuration.append(double(duration));
            }
            // По-записные данные для stendShowDeviceLog() (см. ArchiveState)
            m_arc.recTs.append(tsStart);
            m_arc.recDur.append(duration != 0xFFFFFFFFu ? duration : 0);
            m_arc.recTotal.append(durationTotal != 0xFFFFFFFFu ? durationTotal : 0);
        } else {
            ++m_arc.brokenRecords;
            m_arc.trailingOpen = true;
            if (tsStart > m_arc.tsLast)
                m_arc.tsLast = tsStart;   // цикл начат, не завершён — это последнее, что известно
            // В отличие от незаписанного слота (return выше) — после
            // прерванной записи устройство при восстановлении могло занять
            // следующий слот этой же страницы (см. п.5а спеки), поэтому
            // разбор продолжается, не останавливаемся здесь.
        }
    }
}

// Logger v2: типизированные фреймы по 256 байт (см. data_format_spec_v1.md).
void MainWindow::archiveParseLoggerPage(const QByteArray &page, int pageOffset)
{
    const auto *d = reinterpret_cast<const quint8 *>(page.constData());
    const quint8 type = d[0];

    if (type == 0xFF) {
        // Незаписанная страница — конец журнала. Подрезаем pageLimit (как и в
        // Регистраторе), иначе при сканировании всего диапазона Flash (после
        // стирания/при заходе на «Данные») чтение продолжится постранично до
        // самого конца чипа — теперь это особенно важно, т.к. прошивка отдаёт
        // только по одной странице за запрос (см. archiveHandleChunk).
        m_arc.pageLimit = quint16(qMin<quint32>(m_arc.pageLimit, m_arc.pageStart + pageOffset));
        return;
    }

    if (type == 0xFE || type == 0xFC || type == 0xFB) {
        // ЗАГОЛОВОК / СТАРТ+Д+T0/T1 — начало цикла
        quint32 ts;
        std::memcpy(&ts, d + 1, 4);
        m_arc.curCycleTs     = ts;
        m_arc.haveCurCycleTs = true;
        if (m_arc.records == 0 && !m_arc.haveComplete)
            m_arc.tsFirst = ts;
        m_arc.trailingOpen = true;   // цикл открыт, пока не встретим СТОП
    } else if (type == 0xF6) {
        // СТОП: duration u32@[242..245], duration_total u32@[246..249]
        quint32 duration, durationTotal;
        std::memcpy(&duration,      d + 242, 4);
        std::memcpy(&durationTotal, d + 246, 4);
        if (durationTotal != 0xFFFFFFFFu) {
            m_arc.durationTotal = durationTotal;
            m_arc.haveDuration  = true;
        }
        ++m_arc.records;
        m_arc.haveComplete = true;
        m_arc.trailingOpen = false;
        if (m_arc.haveCurCycleTs && duration != 0xFFFFFFFFu)
            m_arc.tsLast = m_arc.curCycleTs + duration;
        else if (m_arc.haveCurCycleTs)
            m_arc.tsLast = m_arc.curCycleTs;
        // Logger v2 не хранит MAX_vibration/MAX_RPM — это поля только Регистратора,
        // поэтому m_arc.maxVibro/maxRpm и графики для этого режима не заполняются.
        // Но duration цикла СТОП-фрейм хранит всегда — этого достаточно для
        // столбчатой диаграммы «Активное время» (запрос 20.06.2026: не должно
        // выглядеть «уныло» против Регистратора пустым местом на её месте).
        if (duration != 0xFFFFFFFFu) {
            m_arc.plotKeys.append(double(m_arc.records));
            m_arc.plotDuration.append(double(duration));
        }
    }
    // 0xFD/0xFA/0xF8/0xF7 (страницы данных) для сводки вкладки «Данные» не нужны.
}

void MainWindow::archiveFinish()
{
    m_arc.running = false;

    // Кнопка «Из устройства…» (вкладка «Стенд») ждала этот разбор —
    // скопировать по-записные данные в офлайн-хранилище и вывести
    // (см. stendShowDeviceLog; m_offlDev отдельно от m_arc, т.к. второй
    // источник той же колонки — «Из образа…»).
    if (m_stendArcToPanel) {
        m_stendArcToPanel = false;
        // Вернуть кнопке вид после «⌛ чтение…» (индикация 03.07.2026)
        ui->btnStendFromDev->setText(QStringLiteral("Память…"));
        ui->btnStendFromDev->setEnabled(true);
        // Колонку «Стенд» (журнал/образ) НЕ трогаем — модель 03.07:
        // «Память…» заполняет только свою колонку «Регистратор».
        m_offlDev.clear();
        m_offlDev.reserve(m_arc.recTs.size());
        for (int i = 0; i < m_arc.recTs.size(); ++i) {
            OfflineDevRec r;
            r.ts    = m_arc.recTs[i];
            r.dur   = m_arc.recDur[i];
            r.total = m_arc.recTotal[i];
            r.rpm   = (i < m_arc.plotRpm.size()) ? int(m_arc.plotRpm[i] + 0.5) : 0;
            m_offlDev.append(r);
        }
        m_offlDevSrc = QStringLiteral("память устройства");
        stendShowDeviceLog();
    }
    m_arc.valid   = true;   // с этого момента архив — источник для lblFirstDate/Time/lblLastDate/Time,
                             // GET_STATS (он не видит наши тестовые записи) больше их не перезатирает

    // То же самое для lblUptime («Активное время») — раньше это число всегда
    // шло из живого GET_STATS, даже после разбора архива (найдено 20.06.2026
    // по вопросу «куда делось основное значение для Logger»): показывало
    // наработку самого железа, а не duration_total загруженного образа.
    archiveUpdateDashboard();
    archiveFinishTail();   // лог + границы образа — только после полного разбора
}

// Ползунок истории циклов ↔ ось X (18.07.2026): диапазон = число циклов минус
// видимое окно; ползунок виден только когда есть что прокручивать.
void MainWindow::updateCycleScroll()
{
    const int n = m_arc.plotKeys.size();
    const QCPRange r = ui->plotUptime->xAxis->range();
    const int span = qMax(1, qRound(r.size()));
    QSignalBlocker block(ui->scrCycles);
    ui->scrCycles->setRange(0, qMax(0, n - span));
    ui->scrCycles->setPageStep(span);
    ui->scrCycles->setValue(qRound(r.lower + 0.5));
    ui->scrCycles->setVisible(n > span);
}

// Перерисовка дашборда «Данные» из m_arc (18.07.2026, вынесено из
// archiveFinish): зовётся и после полного разбора архива, и ЖИВЬЁМ на каждый
// расширенный CYCLE_PUSH во время «Работы» (stendFillRegColumn) — вкладка
// пополняется циклами, не дожидаясь «Стоп».
void MainWindow::archiveUpdateDashboard()
{
    if (m_arc.haveDuration) {
        const quint32 h = m_arc.durationTotal / 3600;
        const quint32 m = (m_arc.durationTotal % 3600) / 60;
        ui->lblUptime->setText(QStringLiteral("%1.%2").arg(h, 3, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')));
    } else {
        ui->lblUptime->setText(QStringLiteral(" "));
    }

    const auto fmtTs = [](quint32 ts, QLabel *lDate, QLabel *lTime) {
        if (ts == 0 || ts == 0xFFFFFFFFu) {
            lDate->setText(QStringLiteral(" "));
            lTime->setText(QStringLiteral(" "));
            return;
        }
        // База ts — 2000-01-01 (rtcToSec прошивки), НЕ Unix-эпоха: старый
        // fromSecsSinceEpoch давал «1970-01-01»/«1996-…» (18.07.2026).
        const QDateTime dt = QDateTime(QDate(2000,1,1), QTime(0,0)).addSecs(qint64(ts));
        if (dt.date().year() < 2002) {   // сброшенные часы → дата-мусор, прячем
            lDate->setText(QStringLiteral(" "));
            lTime->setText(dt.toString(QStringLiteral("HH:mm")));
            return;
        }
        lDate->setText(dt.toString(QStringLiteral("dd.MM.yy")));
        lTime->setText(dt.toString(QStringLiteral("HH:mm")));
    };
    fmtTs(m_arc.tsFirst, ui->lblFirstDate, ui->lblFirstTime);
    fmtTs(m_arc.tsLast,  ui->lblLastDate,  ui->lblLastTime);

    ui->lblCyclesUsed->setText(m_arc.brokenRecords > 0
        ? QStringLiteral("%1 (+%2 оборван)").arg(m_arc.records).arg(m_arc.brokenRecords)
        : QString::number(m_arc.records));

    if (m_arc.mode == ArchiveMode::Registrator) {
        // Упаковка по 10 записей/страницу (20.06.2026) — ёмкость и остаток
        // считаются в записях (слотах), а не в страницах, как раньше.
        const qint64 totalSlots = (qint64(kFlashTotalPages) - kLogStartPage) * kRegRecordsPerPage;
        const qint64 usedSlots  = qint64(m_arc.records) + m_arc.brokenRecords;
        ui->lblCyclesFree->setText(QString::number(qMax<qint64>(0, totalSlots - usedSlots)));
    } else {
        // Logger: 1 фрейм = 1 страница без изменений — остаток в страницах.
        const int pagesUsed = int(m_arc.pageLimit) - int(kLogStartPage);
        ui->lblCyclesFree->setText(QString::number(
            qint64(kFlashTotalPages) - kLogStartPage - qMax(0, pagesUsed)));
    }

    if (m_arc.mode == ArchiveMode::Registrator && m_arc.haveComplete) {
        // «уровень» — потолок МАЛОГО масштаба (клип); «пики» — истинный максимум
        // vib1 за журнал (19.07.2026, два графика одного сигнала).
        double clampG = 0.2;
        if (!m_arc.plotVibroRms.isEmpty()) {
            QVector<double> s = m_arc.plotVibroRms;
            std::sort(s.begin(), s.end());
            clampG = qMax(200.0, s[s.size()/2] * 2.5) / 1000.0;
        }
        ui->lblMaxVibro->setText(QStringLiteral("≤%1 g").arg(clampG, 0, 'f', 2));
        ui->lblMaxVibro2->setText(QStringLiteral("%1 g")
            .arg(double(m_arc.maxVibro) / 1000.0, 0, 'f', 2));
        ui->lblMaxSpeed->setText(QStringLiteral("%1 об/мин").arg(double(m_arc.maxRpm), 0, 'f', 1));
    } else {
        ui->lblMaxVibro->setText(QStringLiteral(" "));
        ui->lblMaxVibro2->setText(QStringLiteral(" "));
        ui->lblMaxSpeed->setText(QStringLiteral(" "));
    }

    // «Активное время» — заливка-ступеньки (graph(0), см. setup): 1 ступень =
    // 1 цикл, высота = его длительность. Общая для обоих режимов (Logger тоже
    // знает duration каждого цикла). QCPBars заменён на filled step ради снятия
    // «ШИМ» при плотных данных (05.07.2026).
    if (m_uptimeBars) {
        if (!m_arc.plotKeys.isEmpty() && !m_arc.plotDuration.isEmpty()) {
            m_uptimeBars->setData(m_arc.plotKeys, m_arc.plotDuration);
            ui->plotUptime->yAxis->rescale();
            // Дефолтное окно: последние kUptimeDefaultBars циклов, но НЕ УЖЕ
            // kUptimeMinBars слотов (иначе при 5 циклах столбики раздувались на
            // весь график). Диапазон ставим ТОЛЬКО при изменении числа циклов —
            // переключение вкладок (rescan с тем же n) не сбрасывает пользова-
            // тельский зум/прокрутку (18.07.2026, «масштаб слетает»).
            constexpr int kUptimeDefaultBars = 60;
            constexpr int kUptimeMinBars     = 30;
            const int n = m_arc.plotKeys.size();
            static int s_lastBarsN = -1;
            if (n != s_lastBarsN) {
                s_lastBarsN = n;
                const double hi = qMax(double(n), double(kUptimeMinBars)) - 0.5;
                const double lo = qMax(-0.5, double(n) - kUptimeDefaultBars - 0.5);
                ui->plotUptime->xAxis->setRange(lo, hi);
            }
            ui->plotUptime->replot();
        } else {
            m_uptimeBars->data()->clear();
            ui->plotUptime->replot();
        }
    }

    if (m_arc.mode == ArchiveMode::Registrator && !m_arc.plotKeys.isEmpty()) {
        // ДВА РАЗНЫХ КАНАЛА vib1 (19.07.2026, «уровень не должен меняться на ударе»):
        //  • График 1 «уровень» = vib1_RMS — среднеквадратичный, НЕ реагирует на
        //    одиночный удар; Y фиксирован на малый масштаб (медиана RMS ×2.5).
        //  • График 2 «пики» = vib1_peak — ловит удар; Y фикс. БОЛЬШОЙ масштаб
        //    (16 g), без ударов сидит у нуля, удар сразу до потолка.
        double clamp = 200.0;
        if (!m_arc.plotVibroRms.isEmpty()) {
            QVector<double> s = m_arc.plotVibroRms;
            std::sort(s.begin(), s.end());
            clamp = qMax(200.0, s[s.size() / 2] * 2.5);
        }
        if (m_vibBars)  { m_vibBars->setData(m_arc.plotKeys, m_arc.plotVibroRms);
                          ui->plotVibro->yAxis->setRange(0, clamp);   // RMS, малый масштаб
                          ui->plotVibro->replot(); }
        if (m_vib2Bars) { m_vib2Bars->setData(m_arc.plotKeys, m_arc.plotVibro);   // ПИК
                          const double top = qMax(16000.0, double(m_arc.maxVibro) * 1.05);
                          ui->plotVibro2->yAxis->setRange(0, top);
                          ui->plotVibro2->replot(); }
        // Гироскоп — столбики (18.07.2026); ось X подтянется синхронизацией
        // от plotUptime (setRange ниже по коду рендера uptime).
        if (m_speedBars) {
            m_speedBars->setData(m_arc.plotKeys, m_arc.plotRpm);
            ui->plotSpeed->yAxis->rescale();
            ui->plotSpeed->replot();
        }
    } else {
        // Logger не хранит MAX_vibration/MAX_RPM по циклам (нужны сырые сэмплы
        // + таблицы LSB_base, которые пока не закрыты, см. data_format_spec_v1.md
        // TODO Logger v2 п.1-2) — снимаем графики предыдущего (Регистраторного)
        // прогона, чтобы не оставлять чужие данные на экране.
        if (m_vibBars)  m_vibBars->data()->clear();
        ui->plotVibro->replot();
        if (m_vib2Bars) m_vib2Bars->data()->clear();
        ui->plotVibro2->replot();
        if (m_speedBars) m_speedBars->data()->clear();
        ui->plotSpeed->replot();
    }
    updateCycleScroll();
}

// Хвост archiveFinish (лог + границы образа) — только после ПОЛНОГО разбора
// архива, не на каждый живой пуш (18.07.2026).
void MainWindow::archiveFinishTail()
{
    appendLog(QStringLiteral(
        "[Архив] %1: %2 завершённых записей%3, duration_total=%4%5")
            .arg(m_arc.mode == ArchiveMode::Registrator ? QStringLiteral("Регистратор")
                                                          : QStringLiteral("Logger"))
            .arg(m_arc.records)
            .arg(m_arc.brokenRecords > 0
                     ? QStringLiteral(" (+%1 оборвана)").arg(m_arc.brokenRecords)
                     : QString())
            .arg(m_arc.haveDuration ? QString::number(m_arc.durationTotal) + " с" : QStringLiteral(" "))
            .arg(m_arc.trailingOpen ? QStringLiteral(", последний цикл не завершён") : QString()));

    // «Образ: страниц» / «Образ: начало» — обновляем реальными границами из Flash
    // после завершения архивного сканирования. Раньше эти поля заполнялись ТОЛЬКО
    // после записи тестового образа в текущей сессии, из-за чего «Прочитать» не
    // работала для реальных данных устройства. Теперь работает всегда (01.07.2026).
    if (m_arc.pageLimit > kLogStartPage) {
        ui->lblImgAddr->setText(QString::number(kLogStartPage));
        ui->lblImgPages->setText(QString::number(m_arc.pageLimit - kLogStartPage));
        ui->btnMemReadImg->setEnabled(true);
    } else {
        ui->lblImgAddr->setText(QStringLiteral(" "));
        ui->lblImgPages->setText(QStringLiteral(" "));
        ui->btnMemReadImg->setEnabled(false);
    }
}

// «Прочитать» (замена «Сравнить», решение 20.06.2026): читает Flash в
// диапазоне «Страниц»/«Страница» — верхних строк-«фактов», которые
// заполняются автоматически при записи образа RG/LOG (расположение именно
// этого образа, отдельно от тестовых полей «Задать» ниже) — и выводит
// гекс-дамп в txtHexDump. Не сравнение с кешем, а наглядный просмотр того,
// что реально лежит на чипе.
//
// ⚠ ИЗВЕСТНОЕ ОГРАНИЧЕНИЕ (заметка 20.06.2026, см. CLAUDE.md): lblImgAddr/
// lblImgPages заполняются ТОЛЬКО когда образ записан из самого LOGLSMW в
// текущей сессии (FLASH_WRITE, тег TagImg). Если на чипе реальные данные с
// устройства (привезённого из поля, не из нашей тестовой записи) — эти 2
// строки остаются пустыми, и «Прочитать» не сработает («Нет записанного
// образа…»), хотя на чипе данных полно. Логично для теста записи/чтения, но
// для чтения РЕАЛЬНОГО устройства нужен отдельный путь — скорее всего через
// уже существующий probeFlashImageState()/архивный разбор (он умеет найти
// границы журнала на чипе вне зависимости от нашей сессии) — TODO, не
// сделано.
void MainWindow::memTestDump(int startOverride, int countOverride)
{
    if (!m_link->isOpen()) {
        ui->memReport->appendPlainText(QStringLiteral("⚠ Нет подключения"));
        return;
    }
    if (m_test.running) return;

    quint16 start;
    int     count;
    if (startOverride >= 0 && countOverride > 0) {
        // Явный диапазон — после ЗАГРУЗКИ ОБРАЗА показываем весь образ целиком
        // (17.07.2026, по запросу: «при загрузке образа — его диапазон»).
        start = quint16(startOverride);
        count = countOverride;
    } else {
        // По умолчанию (АВТО-дамп: вход на вкладку / после стирания) объём =
        // поля «Старт»/«Страниц» таблицы «Задать» (17.07.2026), НО не больше
        // kAutoDumpMaxPages — чтобы вход был мгновенным и не завис на большом
        // «Страниц». Полный заданный диапазон читает ручное «Прочитать» (оно
        // вызывает memTestDump с явным override, минуя это ограничение).
        start = quint16(ui->spinMemStartPage->value());
        count = qMin(qMax(1, ui->spinMemPages->value()), kAutoDumpMaxPages);
    }

    m_test.pageTotal  = count;
    m_test.pageStart  = start;
    m_test.pageCur    = 0;
    m_test.errTotal   = 0;
    m_test.running    = true;
    m_test.step       = TestStep::Dump;
    m_test.cycleTotal = 1;
    m_test.pagesDone  = 0;
    m_test.lastReadByte    = -1;   // «Байт» Факт — пусто до первого чтения
    m_test.lastReadUniform = true;
    m_dumpBuf.clear();

    ui->memReport->appendPlainText(QStringLiteral(
        "Чтение для дампа: стр.%1..%2").arg(start).arg(start + count - 1));

    setOpsEnabled(false);
    memTestStep();
}

// Гекс-дамп по словам (2 байта = 4 hex-цифры), разделённым пробелом, по 8
// слов (16 байт) на строку, с адресом начала строки слева (на запрос
// пользователя 20.06.2026: «слово пробел слово... в правой колонке»).
void MainWindow::renderHexDump(quint16 startPage, const QByteArray &buf)
{
    constexpr int wordsPerLine = 8;
    constexpr int pageBytes    = 256;   // 1 страница Flash — граница блока в дампе
    const auto *d = reinterpret_cast<const quint8 *>(buf.constData());
    const int n = buf.size();
    QString out;
    for (int off = 0; off < n; off += wordsPerLine * 2) {
        // Горизонтальная линия перед началом каждой новой страницы (256 байт) —
        // 1 запись Регистратора/1 фрейм Logger = 1 страница, граница страницы —
        // граница смыслового блока в дампе (запрос 20.06.2026).
        if (off > 0 && off % pageBytes == 0)
            out += QString(60, QChar(0x2500)) + QLatin1Char('\n');
        const quint32 addr = (quint32(startPage) << 8) + quint32(off);
        out += QStringLiteral("%1:  ").arg(addr, 6, 16, QLatin1Char('0')).toUpper();
        QStringList words;
        for (int w = 0; w < wordsPerLine; ++w) {
            const int i = off + w * 2;
            if (i >= n) break;
            words << ((i + 1 < n)
                ? QStringLiteral("%1%2").arg(d[i], 2, 16, QLatin1Char('0'))
                                          .arg(d[i + 1], 2, 16, QLatin1Char('0'))
                : QStringLiteral("%1").arg(d[i], 2, 16, QLatin1Char('0')));
        }
        // Разделитель слов — один пробел (компактнее; граница страницы теперь
        // и так видна по горизонтальной линии выше — запрос 20.06.2026).
        out += words.join(QLatin1Char(' ')).toUpper() + QLatin1Char('\n');
    }
    ui->txtHexDump->setPlainText(out);
}

void MainWindow::memTestStep()
{
    if (!m_test.running) return;
    const quint16 pg   = quint16(m_test.pageStart + m_test.pageCur);
    const quint32 addr = quint32(pg) << 8;

    // Для Scan и Compare одинаковый запрос — FLASH_READ страницы
    QByteArray p;
    for (int i = 0; i < 4; ++i) p.append(char((addr >> (8*i)) & 0xFF));
    for (int i = 0; i < 4; ++i) p.append(char((256   >> (8*i)) & 0xFF));
    m_dev->enqueue(LtpCmd::FLASH_READ, p, quint32(TagTest));
}

void MainWindow::memTestUpdateUi()
{
    // Начальное состояние (ни одной операции не запускали) — пустые ячейки
    if (m_test.pageTotal == 0) {
        for (QLabel *l : {ui->lblCurCycle, ui->lblCurPages, ui->lblCurActivePage,
                          ui->lblCurByte, ui->lblCurErrors, ui->lblCurSpi, ui->lblCurStep})
            { l->setText(QStringLiteral(" ")); l->setStyleSheet(QString()); }
        return;
    }

    // После первой операции — всегда показываем актуальные значения,
    // не сбрасываем после остановки (держим последнее состояние).

    // Текущий номер цикла (1-based). Считаем от последней ЗАВЕРШЁННОЙ страницы
    // (pagesDone-1), а не от следующей в очереди — иначе ровно на границе
    // цикла (pagesDone кратно pageTotal) счётчик цикла и «Страниц»/«Страница»
    // ниже мгновенно перескакивали на начало следующего цикла (0/следующий №),
    // не давая увидеть, что цикл реально дошёл до последней страницы
    // (замечено пользователем 02.07 — «нагляднее, если остановятся на этих
    // значениях до следующего цикла»). Теперь значения держатся на последней
    // обработанной странице/цикле, пока не придёт ответ по первой странице
    // СЛЕДУЮЩЕГО цикла.
    const int cycleCur = (m_test.pageTotal > 0 && m_test.pagesDone > 0)
        ? ((m_test.pagesDone - 1) / m_test.pageTotal) + 1
        : (m_test.running ? 1 : 0);
    const int cycleCurClamped = qMin(qMax(cycleCur, 1), m_test.cycleTotal);

    // Страниц в текущем цикле — 1-based (1..pageTotal), держится на pageTotal
    // на последней странице цикла вместо сброса в 0.
    const int pageCurInCycle = (m_test.pageTotal > 0 && m_test.pagesDone > 0)
        ? ((m_test.pagesDone - 1) % m_test.pageTotal) + 1
        : 0;

    // Адрес текущей (последней обработанной) страницы
    const quint16 pg = quint16(m_test.pageStart + qMax(0, pageCurInCycle - 1));

    // Циклов Факт = номер текущего цикла
    if (m_test.step == TestStep::Idle && m_test.pagesDone > 0) {
        ui->lblCurCycle->setText(QStringLiteral("готово"));
    } else {
        ui->lblCurCycle->setText(QString::number(cycleCurClamped));
    }
    ui->lblCurCycle->setStyleSheet(QString());

    // Страниц Факт = страниц обработано в текущем цикле
    ui->lblCurPages->setText(QString::number(pageCurInCycle));
    ui->lblCurPages->setStyleSheet(QString());

    // Страница Факт = адрес текущей страницы
    ui->lblCurActivePage->setText(QString::number(pg));
    ui->lblCurActivePage->setStyleSheet(QString());

    // «Байт» Факт — реально ПРОЧИТАННЫЙ с устройства байт, не эхо поля
    // «Задать» (баг до 02.07.2026 — сюда копировался editTestByte, ячейка
    // «факт» ничего не проверяла, замечено пользователем). Пока ни одного
    // чтения не было (фаза записи, старт теста) — пусто (пробел, см.
    // соглашение UI). Неоднородная страница (дамп реальных данных) —
    // показан первый байт + «…».
    if (m_test.lastReadByte >= 0) {
        QString b = QStringLiteral("%1")
            .arg(m_test.lastReadByte, 2, 16, QLatin1Char('0')).toUpper();
        if (!m_test.lastReadUniform) b += QStringLiteral("…");
        ui->lblCurByte->setText(b);
    } else {
        ui->lblCurByte->setText(QStringLiteral(" "));
    }
    ui->lblCurByte->setStyleSheet(QString());

    ui->lblCurSpi->setText(QString::number(ui->spinMemSpi->value()));
    ui->lblCurSpi->setStyleSheet(QString());

    // Шаг операции (col 1 строки Ошибок)
    switch (m_test.step) {
    case TestStep::Write:   ui->lblCurStep->setText(QStringLiteral("Запись"));   break;
    case TestStep::Read:    ui->lblCurStep->setText(QStringLiteral("Чтение"));   break;
    case TestStep::Compare: ui->lblCurStep->setText(QStringLiteral("Сравн."));   break;
    case TestStep::Dump:    ui->lblCurStep->setText(QStringLiteral("Дамп"));     break;
    case TestStep::Done:    ui->lblCurStep->setText(QStringLiteral("Готово"));   break;
    default:                ui->lblCurStep->setText(QStringLiteral(" "));        break;
    }
    ui->lblCurStep->setStyleSheet(QString());

    const bool hasErr = (m_test.errTotal > 0);
    ui->lblCurErrors->setText(hasErr
        ? QString::number(m_test.errTotal)
        : (m_test.pagesDone > 0 ? QStringLiteral("OK") : QStringLiteral(" ")));
    ui->lblCurErrors->setStyleSheet(hasErr
        ? QStringLiteral("color:#C03030;font-weight:600;")
        : QStringLiteral("color:#1D7A4C;font-weight:600;"));
}

// ── Термотест ─────────────────────────────────────────────────────

quint8 MainWindow::tempRunTempCmd() const
{
    if (ui->radMemTmp->isChecked()) return LtpCmd::GET_TEMP_TMP117;
    if (ui->radMemLsm->isChecked()) return LtpCmd::GET_TEMP_IMU;
    return LtpCmd::GET_TEMP_STM;
}

QString MainWindow::tempRunSensorName() const
{
    if (ui->radMemTmp->isChecked()) return QStringLiteral("TMP117");
    if (ui->radMemLsm->isChecked()) return QStringLiteral("IMU (LSM)");
    return QStringLiteral("STM32");
}

void MainWindow::tempRunStart()
{
    if (m_tempRun.running) return;
    m_tempRun.running  = true;
    setOpsEnabled(false, ui->btnTempRun);
    m_tempRun.halted   = false;
    m_tempRun.step     = TempRunStep::Idle;
    // Диапазон термотеста берём ИЗ ТАБЛИЦЫ (17.07.2026): стартовая страница и
    // число страниц — те же поля «Страница»/«Страниц», что у Записи/Чтения.
    // Обход — по кругу в пределах [start, start+count) (инкремент page в
    // обработчике TagTempRun). Раньше шёл по всему чипу и вдобавок выставлял
    // spinMemPages=65536, из-за чего потом вешалось стирание страниц.
    m_tempRun.rangeStart     = quint16(ui->spinMemStartPage->value());
    // «Проходов» — сколько страниц (от «Старт») прогоняется на КАЖДОЙ температурной
    // точке (17.07.2026). Отдельное поле, не завязано на «Страниц» (то — для байт-
    // теста). Блок [Старт, Старт+проходов) пишется/сверяется при каждом шаге °C.
    m_tempRun.passesPerStep   = qMax(1, ui->spinTPasses->value());
    m_tempRun.pagesLeftInStep = 0;
    m_tempRun.page            = m_tempRun.rangeStart;
    // Байт-шаблон берём из «Задать: Байт» (17.07.2026) — раньше термотест писал
    // захардкоженный 0xA5, игнорируя поле, из-за чего последующее ручное «Чтение»
    // со сверкой по заданному байту давало «расхождение» (записано A5, ждали 00).
    // Фиксируем на весь прогон, чтобы правка поля в процессе не сбивала сверку.
    { bool okB = false; int v = ui->editTestByte->text().trimmed().toInt(&okB, 16);
      m_tempRun.testByte = okB ? quint8(v & 0xFF) : 0x00; }
    m_tempRun.lastTemp = -999.0f;
    m_tempRun.opCount  = 0;
    m_tempRun.errCount = 0;
    // Инициализируем Факт-колонку для отображения хода термотеста
    m_test.pageTotal  = 1;
    m_test.pageStart  = m_tempRun.page;
    m_test.pageCur    = 0;
    m_test.errTotal   = 0;
    m_test.running    = true;
    m_test.step       = TestStep::Idle;
    m_test.cycleTotal = 9999;   // число операций заранее неизвестно
    m_test.pagesDone  = 0;
    m_test.lastReadByte    = -1;   // «Байт» Факт — пусто до первого чтения
    m_test.lastReadUniform = true;
    memTestUpdateUi();
    m_tempBlink = true;
    ui->btnTempRun->setText(QStringLiteral("🌡 Стоп"));
    const quint16 rEnd = quint16(m_tempRun.rangeStart + m_tempRun.passesPerStep - 1);
    ui->memReport->appendPlainText(QStringLiteral(
        "═══ Термотест: старт — датчик %1, блок стр.%2..%3 (%4 стр./точку), "
        "МИН=%5 МАКС=%6 ШАГ=%7 °C ═══")
        .arg(tempRunSensorName())
        .arg(m_tempRun.rangeStart).arg(rEnd).arg(m_tempRun.passesPerStep)
        .arg(ui->spinTMin->value()).arg(ui->spinTMax->value()).arg(ui->spinTStep->value()));
    m_tempPollTimer.start(3000);  // опрос каждые 3 с
    requestCmd(tempRunTempCmd()); // немедленный первый опрос
}

void MainWindow::tempRunStop()
{
    if (!m_tempRun.running) return;
    m_tempRun.running = false;
    m_tempRun.step    = TempRunStep::Idle;
    m_tempPollTimer.stop();
    setOpsEnabled(true);
    ui->btnTempRun->setText(QStringLiteral("Старт ▶"));
    // Обновляем Факт: итоговое состояние термотеста
    m_test.running   = false;
    m_test.step      = TestStep::Idle;
    m_test.pagesDone = m_tempRun.opCount;
    m_test.errTotal  = m_tempRun.errCount;
    memTestUpdateUi();
    ui->memReport->appendPlainText(QStringLiteral(
        "═══ Прогон остановлен: операций=%1, ошибок=%2 байт ═══")
        .arg(m_tempRun.opCount).arg(m_tempRun.errCount));
}

void MainWindow::tempRunHandleTemp(float t)
{
    const float tMax = float(ui->spinTMax->value());
    const float tMin = float(ui->spinTMin->value());
    const float step = float(ui->spinTStep->value());

    const bool outOfRange = (t >= tMax) || (t <= tMin);

    if (outOfRange && !m_tempRun.halted) {
        m_tempRun.halted = true;
        ui->memReport->appendPlainText(QStringLiteral(
            "⚠ Прогон: %1 °C вне допуска [%2..%3] — ожидание")
            .arg(double(t), 0, 'f', 1).arg(ui->spinTMin->value()).arg(ui->spinTMax->value()));
        return;
    }
    if (!outOfRange && m_tempRun.halted) {
        m_tempRun.halted   = false;
        m_tempRun.lastTemp = t;  // сброс базы — шаг считается от точки возврата
        ui->memReport->appendPlainText(QStringLiteral(
            "↩ Прогон: %1 °C — температура в норме, жду шаг %2 °C")
            .arg(double(t), 0, 'f', 1).arg(ui->spinTStep->value()));
        return;
    }
    if (outOfRange) return;  // всё ещё на паузе

    // Не на паузе — проверяем шаг
    if (m_tempRun.lastTemp < -900.0f) {
        // Первое измерение: фиксируем базу и сразу запускаем первую операцию
        m_tempRun.lastTemp = t;
        ui->memReport->appendPlainText(QStringLiteral(
            "Прогон: начальная температура %1 °C, первая операция")
            .arg(double(t), 0, 'f', 1));
        tempRunBeginStep();
        return;
    }
    const float delta = t - m_tempRun.lastTemp;
    if (qAbs(delta) >= step && m_tempRun.step == TempRunStep::Idle) {
        m_tempRun.lastTemp = t;
        ui->memReport->appendPlainText(QStringLiteral(
            "Прогон: %1%2 °C (шаг %3 °C), операция %4")
            .arg(delta > 0 ? QStringLiteral("+") : QString())
            .arg(double(delta), 0, 'f', 1)
            .arg(double(step), 0, 'f', 0)
            .arg(m_tempRun.opCount + 1));
        tempRunBeginStep();
    }
}

void MainWindow::tempRunBeginStep()
{
    // Новая температурная точка — прогоняем БЛОК из passesPerStep страниц,
    // начиная с rangeStart. Каждая страница: запись байта → сверка сразу этой
    // же страницы (продолжение блока — в обработчике сверки FLASH_READ/TagTempRun).
    // (17.07.2026)
    m_tempRun.pagesLeftInStep = qMax(1, m_tempRun.passesPerStep);
    m_tempRun.page = m_tempRun.rangeStart;
    m_dumpBuf.clear();   // новый блок → чистим буфер живого гекс-вывода (17.07.2026)
    tempRunDoOp();
}

void MainWindow::tempRunDoOp()
{
    if (m_tempRun.step != TempRunStep::Idle) return;  // предыдущий цикл не завершён
    m_tempRun.step = TempRunStep::Write;
    m_test.step      = TestStep::Write;
    m_test.pageStart = m_tempRun.page;
    memTestUpdateUi();
    const quint16 pg = m_tempRun.page;
    QByteArray p;
    p.append(char(pg & 0xFF)); p.append(char((pg >> 8) & 0xFF));
    p.append(QByteArray(256, char(m_tempRun.testByte)));   // байт из «Задать»
    requestCmd(LtpCmd::FLASH_WRITE, p, TagTempRun);
}

// ── Активация устройства (ТЗ v2 §2.6, §3) ────────────────────────────────────

void MainWindow::activationSetSector(int idx, ActivationBar::SectorState st)
{
    ui->barActivation->setSectorState(idx, st);
}

// Мин. видимая длительность жёлтого (21.07.2026, см. комментарий в mainwindow.h).
namespace { constexpr qint64 kActMinVisibleMs = 400; }

void MainWindow::activationSetSectorMinDelay(int idx, ActivationBar::SectorState st,
                                              qint64 activeSinceMs)
{
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - activeSinceMs;
    if (elapsed >= kActMinVisibleMs) {
        activationSetSector(idx, st);
        return;
    }
    QTimer::singleShot(int(kActMinVisibleMs - elapsed), this, [this, idx, st] {
        activationSetSector(idx, st);
    });
}

// Клик по отдельному сегменту шкалы активации → выполнить ЕГО операцию отдельно
// (17.07.2026). Общая активация (▶) прогоняет их подряд; здесь — поштучно.
bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    // Тест IWDG (18.07.2026): дабл-клик по «по таймеру» → отключить рефреш
    // сторожа (0x26). Через ~32 c устройство сбросится, счётчик «по таймеру»
    // +1 и заморгает красным — полная проверка тракта сторожа.
    if ((obj == ui->lblRestartTimer || obj == ui->lblRestartTimerCaption)
        && ev->type() == QEvent::MouseButtonDblClick) {
        if (!m_link->isOpen()) return true;
        if (QMessageBox::question(this, QStringLiteral("Тест сторожа IWDG"),
                QStringLiteral("Отключить рефреш сторожа? Устройство СБРОСИТСЯ "
                    "через ~32 с, счётчик «по таймеру» должен вырасти на 1."))
            == QMessageBox::Yes) {
            requestCmd(LtpCmd::WDG_TEST, {}, TagManual);
            appendLog(QStringLiteral("[TX] Тест IWDG — рефреш отключён, ждём сброса ~32 с"));
        }
        return true;
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::onActivationSectorClicked(int idx)
{
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
    // В симуляции клик просто зеленит сектор (демо, без реальных операций).
    if (ui->chkSimulation->isChecked()) {
        activationSetSector(idx, ActivationBar::SectorState::Done);
        return;
    }
    switch (idx) {
    case 0:   // Стереть данные — НОВАЯ команда FLASH_ERASE_DATA (0x2A, 21.07.2026),
              // ОТДЕЛЬНАЯ от FLASH_ERASE (0x05, весь чип). Счётчики (iflash) НЕ трогает.
        // ⚠ ПРОШИВКА: обработчик 0x2A ещё не добавлен в com.c (см. devicecontroller.h) —
        // до этого регистратор ответит LTP_ERR_UNKNOWN_CMD, это ожидаемо, не баг LOGLSMW.
        if (QMessageBox::question(this, QStringLiteral("Стирание данных"),
                QStringLiteral("Стереть данные (NOR, со страницы 1)? Служебная "
                    "страница 0 и счётчики перезапусков (внутренняя Flash STM32) "
                    "НЕ затрагиваются."))
            != QMessageBox::Yes) return;
        activationSetSector(0, ActivationBar::SectorState::Active);   // остаётся жёлтым по завершении (одиночный клик)
        requestCmd(LtpCmd::FLASH_ERASE_DATA, {}, TagManual);
        appendLog(QStringLiteral("[Активация] сегмент 1 — стирание данных (0x2A)"));
        break;
    case 1:   // Сброс перезапусков (журнал iflash).
        if (QMessageBox::question(this, QStringLiteral("Сброс перезапусков"),
                QStringLiteral("Обнулить счётчики «Перезапуски по питанию/таймеру»?"))
            != QMessageBox::Yes) return;
        activationSetSector(1, ActivationBar::SectorState::Active);
        m_actWdtPending   = true;   // остаётся жёлтым по факту GET_STATS ниже (обработчик LtpCmd::GET_STATS)
        m_actWdtActiveMs  = QDateTime::currentMSecsSinceEpoch();
        requestCmd(LtpCmd::RESET_STATS, {}, TagManual);
        requestCmd(LtpCmd::GET_STATS,   {}, TagManual);
        appendLog(QStringLiteral("[Активация] сегмент 2 — сброс перезапусков"));
        break;
    case 2:   // Синхро время = та же ⌚ (по границе секунды). Сегмент —
              // Active сразу, Done только по контрольному чтению (GET_DATETIME
              // с TagSyncTime, оно уже шлётся из btnSyncTime сразу после SET_DATETIME).
        activationSetSector(2, ActivationBar::SectorState::Active);
        m_actSyncPending  = true;
        m_actSyncActiveMs = QDateTime::currentMSecsSinceEpoch();
        ui->btnSyncTime->click();
        appendLog(QStringLiteral("[Активация] сегмент 3 — синхронизация времени"));
        break;
    default:  // Сегменты 3..7 — функция ещё не назначена (заполним слева направо).
        appendLog(QStringLiteral("[Активация] сегмент %1 — функция ещё не назначена")
            .arg(idx + 1));
        break;
    }
}

void MainWindow::activationFail(const QString &reason)
{
    int sec = -1;
    switch (m_act.step) {
    case ActStep::Archive:   sec = 0; break;
    case ActStep::Check:     sec = 1; break;
    case ActStep::SyncTime:  sec = 2; break;
    case ActStep::Erase:     sec = 3; break;
    case ActStep::TestWrite: sec = 4; break;
    case ActStep::SetReady:  sec = 5; break;
    default: break;
    }
    if (sec >= 0) activationSetSector(sec, ActivationBar::SectorState::Error);
    m_erasing  = false;
    m_act.step = ActStep::Error;
    ui->btnActivate->setText(QStringLiteral("▶"));
    ui->lblActStatus->setText(QStringLiteral("✗ %1").arg(reason));
    appendLog(QStringLiteral("[ACT] ОШИБКА: %1").arg(reason));
    // Разблокировать другие операции
    ui->btnTempRun->setEnabled(true);
}

void MainWindow::activationStop()
{
    if (m_erasing) m_erasing = false;
    m_act.step = ActStep::Idle;
    ui->btnActivate->setText(QStringLiteral("▶"));
    ui->lblActStatus->setText(QStringLiteral("Прервано"));
    appendLog(QStringLiteral("[ACT] Активация прервана"));
    ui->btnTempRun->setEnabled(true);
}

void MainWindow::activationStart()
{
    if (!m_link->isOpen()) return;
    if (m_test.running || m_tempRun.running) return;
    m_act = ActState{};
    ui->barActivation->reset();
    ui->btnActivate->setText(QStringLiteral("■"));
    ui->lblActStatus->setText(QStringLiteral("Активация…"));
    appendLog(QStringLiteral("[ACT] Начало активации"));
    // Взаимная блокировка с прогоном
    ui->btnTempRun->setEnabled(false);
    activationBeginStep(ActStep::Archive);
}

void MainWindow::activationBeginStep(ActStep step)
{
    m_act.step     = step;
    m_act.subPhase = 0;

    switch (step) {

    case ActStep::Archive:
        ui->lblActStatus->setText(QStringLiteral("Шаг 1: автоархив…"));
        activationSetSector(0, ActivationBar::SectorState::Active);
        {   QByteArray p;
            const quint32 addr = 0x000100u;    // первая запись лога
            for (int i = 0; i < 4; ++i) p.append(char((addr >> (8*i)) & 0xFF));
            const quint32 sz = 30;
            for (int i = 0; i < 4; ++i) p.append(char((sz >> (8*i)) & 0xFF));
            requestCmd(LtpCmd::FLASH_READ, p, TagAct);
        }
        break;

    case ActStep::Check:
        ui->lblActStatus->setText(QStringLiteral("Шаг 2: проверка устройства…"));
        activationSetSector(1, ActivationBar::SectorState::Active);
        requestCmd(LtpCmd::WHO_AM_I, {}, TagAct);
        break;

    case ActStep::SyncTime:
        ui->lblActStatus->setText(QStringLiteral("Шаг 3: синхронизация времени…"));
        activationSetSector(2, ActivationBar::SectorState::Active);
        {   const QDateTime now = QDateTime::currentDateTime();
            const QDate nd = now.date(); const QTime nt = now.time();
            QByteArray p;
            // Ровно 6 байт — sizeof(RTC_DateTime) в прошивке (cmdSetDateTime);
            // лишний байт-код в начале запроса ломал проверку размера на
            // стороне прошивки (er_badarg) — нашлось 20.06.2026 на образе,
            // тот же баг был и здесь.
            p.append(char(nd.year() - 2000));
            p.append(char(nd.month()));
            p.append(char(nd.day()));
            p.append(char(nt.hour()));
            p.append(char(nt.minute()));
            p.append(char(nt.second()));
            requestCmd(LtpCmd::SET_DATETIME, p, TagAct);
        }
        break;

    case ActStep::Erase:
        ui->lblActStatus->setText(QStringLiteral("Шаг 4: стирание памяти…"));
        activationSetSector(3, ActivationBar::SectorState::Active);
        m_eraseStartMs = QDateTime::currentMSecsSinceEpoch();
        requestCmd(LtpCmd::FLASH_ERASE, {}, TagAct);
        break;

    case ActStep::TestWrite:
        ui->lblActStatus->setText(QStringLiteral("Шаг 5: проверка записи…"));
        activationSetSector(4, ActivationBar::SectorState::Active);
        {   QByteArray p;
            p.append(char(0)); p.append(char(0));       // страница 0
            p.append(QByteArray(256, char(0x55)));
            requestCmd(LtpCmd::FLASH_WRITE, p, TagAct);
        }
        break;

    case ActStep::SetReady:
        ui->lblActStatus->setText(QStringLiteral("Шаг 6: постановка на готовность…"));
        activationSetSector(5, ActivationBar::SectorState::Active);
        requestCmd(LtpCmd::START_REGISTER, {}, TagAct);
        break;

    case ActStep::Done:
        for (int i = 0; i < 6; ++i)
            activationSetSector(i, ActivationBar::SectorState::Done);
        ui->lblActStatus->setText(QStringLiteral("✓ ГОТОВ — отключите сервисный кабель"));
        ui->btnActivate->setText(QStringLiteral("▶"));
        appendLog(QStringLiteral("[ACT] Активация завершена"));
        ui->btnTempRun->setEnabled(true);
        break;

    default:
        break;
    }
}

void MainWindow::activationHandleResponse(quint8 cmd, const QByteArray &payload)
{
    const auto *d = reinterpret_cast<const quint8 *>(payload.constData());

    switch (m_act.step) {

    case ActStep::Archive:
        if (cmd == LtpCmd::FLASH_READ) {
            // Проверяем: если всё 0xFF — Flash пуст; иначе — есть данные
            const QByteArray data = (payload.size() > 1) ? payload.mid(1) : QByteArray();
            bool empty = data.isEmpty();
            if (!empty) {
                empty = true;
                for (char c : data) if (quint8(c) != 0xFF) { empty = false; break; }
            }
            activationSetSector(0, ActivationBar::SectorState::Done);
            appendLog(empty
                ? QStringLiteral("[ACT] Шаг 1: Flash пуст — архив не нужен")
                : QStringLiteral("[ACT] Шаг 1: данные обнаружены (очистка на шаге 4)"));
            activationBeginStep(ActStep::Check);
        }
        break;

    case ActStep::Check:
        if (cmd == LtpCmd::WHO_AM_I) {
            // WHO_AM_I: payload[0] — ID без cod-байта (см. существующий обработчик)
            if (payload.isEmpty()) {
                activationFail(QStringLiteral("WHO_AM_I: нет ответа")); return;
            }
            const quint8 id = d[0];
            if (id != 0x6C && id != 0x70) {
                activationFail(QStringLiteral("WHO_AM_I: неизвестный ID 0x%1")
                    .arg(id, 2, 16, QLatin1Char('0')));
                return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 2: WHO_AM_I 0x%1 OK")
                .arg(id, 2, 16, QLatin1Char('0')));
            // Проверяем датчик температуры
            requestCmd(LtpCmd::GET_TEMP_STM, {}, TagAct);
        } else if (cmd == LtpCmd::GET_TEMP_STM
                || cmd == LtpCmd::GET_TEMP_IMU
                || cmd == LtpCmd::GET_TEMP_TMP117)
        {
            if (payload.size() < 5 || d[0] != 0) {
                activationFail(QStringLiteral("Датчик температуры: нет ответа")); return;
            }
            float t = 0.0f;
            std::memcpy(&t, d + 1, 4);
            appendLog(QStringLiteral("[ACT] Шаг 2: температура %1 °C OK")
                .arg(double(t), 0, 'f', 1));
            activationSetSector(1, ActivationBar::SectorState::Done);
            activationBeginStep(ActStep::SyncTime);
        }
        break;

    case ActStep::SyncTime:
        if (cmd == LtpCmd::SET_DATETIME) {
            // ACK: пустой payload или cod==0 — OK
            if (!payload.isEmpty() && d[0] != 0) {
                activationFail(QStringLiteral("SET_DATETIME: ошибка")); return;
            }
            requestCmd(LtpCmd::GET_DATETIME, {}, TagAct);
        } else if (cmd == LtpCmd::GET_DATETIME) {
            if (payload.size() < 7 || d[0] != 0) {
                activationFail(QStringLiteral("GET_DATETIME: нет ответа")); return;
            }
            const QDateTime devDt(QDate(2000 + d[1], d[2], d[3]),
                                  QTime(d[4], d[5], d[6]));
            const qint64 diff = qAbs(QDateTime::currentDateTime().secsTo(devDt));
            if (diff > 2) {
                activationFail(QStringLiteral("Время не синхронизировано: ΔT=%1 с")
                    .arg(diff));
                return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 3: время синхронизировано, ΔT=%1 с")
                .arg(diff));
            activationSetSector(2, ActivationBar::SectorState::Done);
            activationBeginStep(ActStep::Erase);
        }
        break;

    case ActStep::Erase:
        // FLASH_ERASE ACK и WIP-опрос через FLASH_STATE обрабатываются
        // непосредственно в switch(cmd) в onResponse — здесь не попадаем.
        if (cmd == LtpCmd::FLASH_ERASE && (!payload.isEmpty() && d[0] != 0)) {
            activationFail(QStringLiteral("FLASH_ERASE: ошибка"));
        }
        break;

    case ActStep::TestWrite:
        if (cmd == LtpCmd::FLASH_WRITE) {
            if (payload.isEmpty() || d[0] != 0) {
                activationFail(QStringLiteral("Тестовая запись: ошибка")); return;
            }
            // Читаем обратно для верификации (адрес 0, 256 байт)
            QByteArray p;
            for (int i = 0; i < 4; ++i) p.append(char(0));  // addr = 0
            const quint32 sz = 256;
            for (int i = 0; i < 4; ++i) p.append(char((sz >> (8*i)) & 0xFF));
            requestCmd(LtpCmd::FLASH_READ, p, TagAct);
        } else if (cmd == LtpCmd::FLASH_READ) {
            if (payload.size() < 2 || d[0] != 0) {
                activationFail(QStringLiteral("Тестовое чтение: ошибка")); return;
            }
            const QByteArray data = payload.mid(1);
            int mism = 0;
            for (char c : data) if (quint8(c) != 0x55) ++mism;
            if (mism > 0) {
                activationFail(QStringLiteral("Тестовая страница: расхождений %1").arg(mism));
                return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 5: запись/чтение OK (%1 байт)")
                .arg(data.size()));
            activationSetSector(4, ActivationBar::SectorState::Done);
            activationBeginStep(ActStep::SetReady);
        }
        break;

    case ActStep::SetReady:
        if (cmd == LtpCmd::START_REGISTER) {
            if (!payload.isEmpty() && d[0] != 0) {
                activationFail(QStringLiteral("START_REGISTER: ошибка")); return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 6: устройство поставлено на готовность"));
            activationSetSector(5, ActivationBar::SectorState::Done);
            activationBeginStep(ActStep::Done);
        }
        break;

    default:
        break;
    }
}
