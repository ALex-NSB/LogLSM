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
#include <QTableWidget>
#include <QHeaderView>
#include <QBrush>
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
#include <QStringConverter>
#include <QSet>
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
constexpr quint32 TagWriteVerify = 12; // контрольное чтение сразу после записи страницы (22.07.2026)
constexpr quint32 TagSpeedTest = 13; // ответ замера скорости флеша контроллером (0x2D, 26.07.2026)
constexpr quint32 TagActDump   = 14; // постраничное чтение образа для сохранения на диск в шаге 1 активации (27.07.2026)
constexpr quint32 TagRtcCalStop = 15; // контрольное чтение времени при «Стоп» грубой калибровки RTC (03.08.2026)
constexpr quint32 TagSpeedCal   = 16; // авто-калибровка скорости: опрос гироскопа на каждой ступени (03.08.2026)
constexpr quint32 TagSyncCheck  = 17; // проверка опорной точки синхро на устройстве (backup) — состояние кнопок RTC (04.08.2026)
constexpr quint32 TagFw         = 18; // обновление прошивки STM32 через загрузчик: 0x39..0x3D (05.08.2026)
constexpr quint32 TagIflash     = 19; // чтение внутренней Flash STM32 (0x3E) — диагностика (05.08.2026)

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
constexpr int     kRecordBytes     = 48;   // базовая запись цикла (без маркёра слова)
constexpr int     kRecordsPerPage  = 1;    // «1 цикл = 1 слово» (28.07.2026): 1 запись/страница (было 5)

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
constexpr int kRegRecordsPerPage = 1;   // «1 цикл = 1 слово» (28.07.2026), было 5
} // namespace

struct MainWindow::ThemePalette {
    QString bg, card, cardBorder, inputBorder, text, textDim, sectionText;
    QString menuHover, tabSelText, accent, factBg, logText;
};



/* ─────────────────────────────────────────────────────────────────────────────
 * Цвет заголовка окна (06.08.2026).
 *
 * Windows рисует заголовок НЕАКТИВНОГО окна серым по серому — версии прибора и
 * программы в шапке становятся почти нечитаемы. Из Qt это не правится:
 * заголовок рисует система. Но в Windows 11 есть DWM-атрибуты на цвет фона и
 * текста заголовка, и заданные цвета система уже НЕ гасит при потере фокуса.
 *
 * Красим сами: активное окно — синее (как система и рисовала), неактивное —
 * ЧЁРНОЕ, текст белый в обоих. Так шапка читается всегда, а какое окно
 * активно, по-прежнему видно.
 *
 * Функция грузится динамически: на Windows 10 и старше атрибутов нет, вызов
 * просто вернёт ошибку и заголовок останется системным. Ради этого не нужно
 * ни линковать dwmapi, ни трогать сборку.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef Q_OS_WIN
#include <windows.h>
static void applyCaptionColors(WId winId, bool active)
{
    using SetAttr = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttr setAttr = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
            setAttr = reinterpret_cast<SetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    }
    if (!setAttr) return;

    constexpr DWORD DWMWA_CAPTION_COLOR = 35;   // фон заголовка (Windows 11 22000+)
    constexpr DWORD DWMWA_TEXT_COLOR    = 36;   // цвет текста заголовка
    COLORREF caption = active ? RGB(0x00, 0x78, 0xD7)    // активное — синий
                              : RGB(0x00, 0x00, 0x00);   // неактивное — чёрный
    COLORREF text    = RGB(0xFF, 0xFF, 0xFF);            // текст белый всегда
    HWND hwnd = reinterpret_cast<HWND>(winId);
    setAttr(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    setAttr(hwnd, DWMWA_TEXT_COLOR,    &text,    sizeof(text));
}
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
#ifdef Q_OS_WIN
    applyCaptionColors(winId(), true);   // читаемый заголовок в обоих состояниях
    // ⚠ Одного changeEvent(ActivationChange) не хватило: на Windows окно его при
    // уходе фокуса в другое ПРИЛОЖЕНИЕ не всегда получает, и заголовок оставался
    // синим. Ловим состояние приложения целиком — это и есть нужный признак.
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState st) {
                applyCaptionColors(winId(), st == Qt::ApplicationActive);
            });
#endif
    ui->setupUi(this);

    // Фиксированный размер окна (22.07.2026): раньше окно расширялось под
    // самую широкую вкладку («Тест памяти», гекс-дамп без переноса строк) и
    // не сжималось обратно при переключении на более узкие вкладки. Теперь
    // размер зафиксирован под экран пользователя (2560×1440 @175%), а
    // содержимое «Тест памяти» ужато отдельно (см. frameMemInfo в .ui).
    // Диапазон размера окна (22.07.2026): вместо жёсткого фикса — можно
    // потянуть в пределах, чтобы не упираться в край на маленьких
    // мониторах и не растягивать пустоту на больших.
    setFixedSize(800, 650);

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
    setupWorkDir();   // одна папка на дампы, образы и журналы (06.08.2026)
    setupRegistry();  // учёт приборов в той же папке (06.08.2026)
    memTestUpdateUi();   // начальное состояние Факт — пустые ячейки

    // Сегменты активации = операции, ЗАПОЛНЯЕМ СЛЕВА НАПРАВО по мере готовности
    // функций (17.07.2026). Каждый сегмент — кнопка (клик = его операция),
    // отдельных кнопок нет; подпись рисуется ВНУТРИ сегмента (ActivationBar).
    // 0 «Стереть память» (чип NOR) и 1 «Сброс перезапусков» (счётчики iflash) —
    // разные операции: стирание чипа счётчики НЕ трогает. 2..6 — пока без функции.
    // Лента = карта активации. 7 сегментов (02.08.2026): РАБОЧИЕ кнопки слева,
    // ИНДИКАТОРЫ справа. Кнопки (клик только в «Сервис»): Сохранить(0)/Сброс
    // WDT(1)/Синхро время(2). Индикаторы: Датчики(3)/Память(4)/Тест записи(5)/
    // VBAT(6). Сегмент «Активация» УБРАН: статус активации показывает правая
    // панель (запись+дата) и вся зелёная лента; активация запускается кнопкой ▶.
    ui->barActivation->setSectorCount(7);
    ui->barActivation->setSectorName(0, QStringLiteral("Сохранить"));
    ui->barActivation->setSectorName(1, QStringLiteral("Сброс WDT"));      // кнопка: обнуление счётчиков рестартов
    ui->barActivation->setSectorName(2, QStringLiteral("Синхро время"));   // кнопка: SET_DATETIME
    ui->barActivation->setSectorName(3, QStringLiteral("Датчики"));        // индикатор: WHO_AM_I + температура (ок/нет)
    ui->barActivation->setSectorName(4, QStringLiteral("Память"));         // индикатор: стирание/пусто («Чисто»)
    ui->barActivation->setSectorName(5, QStringLiteral("Чтение"));         // индикатор: запись шаблона → чтение/сверка → очистка
    ui->barActivation->setSectorName(6, QStringLiteral("VBAT"));           // индикатор-заглушка (вариант B)
    for (int i = 1; i <= 5; ++i)
        ui->barActivation->setSectorState(i, ActivationBar::SectorState::Disabled);
    // VBAT (6) — заглушка (реальное чтение батареи — в варианте B). Статично
    // серый; МОРГАЕТ жёлтым ТОЛЬКО когда активация завершена (лента зелёная) —
    // напоминание «не забудь проверить батарею перед развёртыванием». В остальных
    // состояниях не мигает (раздражает). 28.07.2026 / VBAT переиндексирован 7→6.
    ui->barActivation->setSectorState(6, ActivationBar::SectorState::Disabled);
    {   auto *vbatTimer = new QTimer(this);
        connect(vbatTimer, &QTimer::timeout, this, [this] {
            if (m_act.step != ActStep::Done) {            // не после активации — не мигаем
                if (m_vbatBlink) { m_vbatBlink = false;
                    ui->barActivation->setSectorState(6, ActivationBar::SectorState::Disabled); }
                return;
            }
            m_vbatBlink = !m_vbatBlink;
            ui->barActivation->setSectorState(6, m_vbatBlink
                ? ActivationBar::SectorState::Active       // жёлтый
                : ActivationBar::SectorState::Disabled);   // серый
        });
        vbatTimer->start(900);
    }
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
        svcLay->setContentsMargins(0, 0, ui->rootLayout->contentsMargins().right() + 10, 0);
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
        // m_fw.running (05.08.2026): идёт заливка прошивки — на линии загрузчик,
        // он знает ровно 0x01/0x02/0x39..0x3D. Любой опрос часов/температур
        // получил бы «неизвестная команда» и засорял журнал, а в очереди мешал
        // бы потоку DATA. Часы замирают на время прошивки — так и надо.
        if (!m_link->isOpen() || m_dev->busy() || m_imgActiveBtn || m_fw.running
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
            if (ui->tabsMain->currentWidget() == ui->tabMemTest
                || ui->tabsMain->currentWidget() == ui->tabMonitor) {
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
                if (ui->tabsMain->currentWidget() == ui->tabDashboard) {
                    // DATA_FLAG (0x30=0) ПЕРВЫМ, затем GET_STATS: «активирован»
                    // карточки берётся из маски флагов (NOR-байт [0]) → должно быть
                    // свежим к моменту разбора GET_STATS. Иначе после «Очистить
                    // журналы» карточка врала «не активирован» при взведённом
                    // флаге (журнал пуст, а флаг стоит). 03.08.2026.
                    m_dev->enqueue(LtpCmd::DATA_FLAG, QByteArray(1, char(0)));
                    m_dev->enqueue(LtpCmd::GET_STATS);
                }
            }
        }
    });

    ui->btnInd->setText(QString()); // без текста — только цветной кружок-индикатор

    // «Симуляция» — демо-режим: заполняет интерфейс образцовыми данными,
    // чтобы показать, как программа выглядит с подключённым устройством и
    // результатами испытания. Обновлено 30.06.2026: теперь заполняет и
    // дашборд, и панель сравнения стенда (ранее — только cmpReport).
    connect(ui->chkSimulation, &QCheckBox::toggled, this, [this](bool on) {
        if (m_actSimulation && m_actSimulation->isChecked() != on) m_actSimulation->setChecked(on);
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
            ui->lblTempCurCaption->setText(QStringLiteral("текущая"));
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
            // Без связи — в заголовке только версия приложения (W); версия
            // прошивки (A) добавится при подключении (WHO_AM_I).
            setWindowTitle(QStringLiteral("LogLSMW %1").arg(qApp->applicationVersion()));
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
        if (cmd == LtpCmd::CYCLE_PUSH) {
            stendFillRegColumn(payload);
            // Прибор записал цикл в Flash → есть несохранённые данные (прошивка
            // взвела флаг стр.122). Красим «Сохранить» в красный СРАЗУ по пушу.
            // Напрямую (не через updateSaveSegment): m_firstFreePage может быть
            // устаревшим («пусто») и дал бы ошибочный «Стёрто». (28.07.2026)
            m_dataFlagSet = true;
            if (m_act.step == ActStep::Idle || m_act.step == ActStep::Done
                    || m_act.step == ActStep::Error) {
                ui->barActivation->setSectorName(0, QStringLiteral("Сохранить"));
                ui->barActivation->setSectorState(0, ActivationBar::SectorState::Idle);
            }
        }
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
            // Опорная точка синхро на устройстве (backup): t0 = это же время (unix).
            // Любая синхронизация задаёт точку → потом «Стоп» посчитает дрейф от неё.
            { QByteArray sr; quint32 ts = quint32(now.toSecsSinceEpoch());
              for (int i = 0; i < 4; ++i) sr.append(char((ts >> (8*i)) & 0xFF));
              requestCmd(LtpCmd::SYNC_REF_SET, sr); }
            requestCmd(LtpCmd::GET_DATETIME, {}, TagSyncTime);   // контрольное чтение
            appendLog(QStringLiteral("[TX] SET_DATETIME ← время ПК (по границе секунды) + опорная точка"));
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

    // «Очистить журналы» (0x27, «Калибровка») — provision: стереть журнал
    // активаций (стр.121) + счётчики рестартов (124..127) → прибор «не
    // активирован», паспорт/калибровки (стр.123) целы. Только Сервис (03.08.2026).
    connect(ui->btnClearJournals, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (QMessageBox::warning(this, QStringLiteral("Очистить журналы"),
                QStringLiteral("Стереть журнал активаций и счётчики рестартов?\n"
                    "Прибор станет «не активирован». Паспорт и калибровки НЕ затрагиваются."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        requestCmd(LtpCmd::CLEAR_JOURNALS, {}, TagManual);
        appendLog(QStringLiteral("[TX] CLEAR_JOURNALS (0x27)"));
        requestCmd(LtpCmd::GET_STATS,   {}, TagManual);   // обновить состояние активации
        requestCmd(LtpCmd::ACT_HISTORY, {}, TagManual);   // обновить список «жизней»
    });

    // «Обновление ПО STM32» («Калибровка», 03.08.2026) — свой бутлоадер (приём
    // образа по LTP + запись во внутр. Flash) ПОКА НЕ реализован. Сейчас работает
    // только выбор файла; заливка — следующий шаг (bootloader-раздел + протокол).
    // «Часы RTC» («Калибровка») — чтение и синхронизация по границе секунды.
    connect(ui->btnRtcSync, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
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
            requestCmd(LtpCmd::SET_DATETIME, p, TagManual);
            requestCmd(LtpCmd::GET_DATETIME, {}, TagSyncTime);   // контроль
            appendLog(QStringLiteral("[TX] RTC ← ПК (по границе секунды)"));
        });
    });

    // «Часы RTC» → грубая калибровка дрейфа (Старт/Стоп/Применить), 03.08.2026.
    connect(ui->btnRtcCalStart, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        requestCmd(LtpCmd::RTC_CALIB_GET, {}, TagManual);   // текущая поправка (для расчёта новой)
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
            requestCmd(LtpCmd::SET_DATETIME, p, TagManual);
            m_rtcCalT0 = QDateTime(now.date(), QTime(now.time().hour(),
                                   now.time().minute(), now.time().second()));  // на границе секунды
            m_rtcCalActive = true;
            // Опорная точка синхро НА УСТРОЙСТВЕ (backup-регистр): t0 = момент синхро
            // (unix). Переживает сон/перезапуск/смену ПК; сбросится только с
            // питанием (тогда и часы сбились → калибровать нельзя, контроль в 0x38).
            { QByteArray sr; quint32 ts = quint32(m_rtcCalT0.toSecsSinceEpoch());
              for (int i = 0; i < 4; ++i) sr.append(char((ts >> (8*i)) & 0xFF));
              requestCmd(LtpCmd::SYNC_REF_SET, sr, TagManual); }
            // Сохраняем в настройки — «Старт» переживёт перезапуск W (выдержка
            // может длиться ночь/сутки, приложение за это время закрывают).
            QSettings st(kOrg, kApp);
            st.setValue(QStringLiteral("rtcCal/active"), true);
            st.setValue(QStringLiteral("rtcCal/t0"), m_rtcCalT0);
            rtcCalUpdateButtons(1);   // «Старт» красная — идёт отсчёт
            ui->lblRtcCalStatus->setText(QStringLiteral("Идёт выдержка… нажмите «Стоп» позже (дольше — точнее)"));
            appendLog(QStringLiteral("[RTC-калибровка] старт, часы синхронизированы"));
        });
    });
    connect(ui->btnRtcCalStop, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        // Сначала запрашиваем опорную точку с УСТРОЙСТВА (backup-регистр). Если
        // валидна — t0 берём оттуда (переживает перезапуск/смену ПК). Если backup
        // сброшен (часы сбились) — валидность 0, и расчёт откажем. Ответ 0x38
        // разбирается в onResponse (тег TagRtcCalStop) → там же цепочка к расчёту.
        requestCmd(LtpCmd::SYNC_REF_GET, {}, TagRtcCalStop);
    });
    // «Стоп от активации» (task #15): t0 = момент активации (ts_activation из
    // GET_STATS). При активации волна синхронизирует часы и пишет ts_activation =
    // PC-время, поэтому активация = длинный «Старт». Дрейф считается тем же
    // обработчиком (TagRtcCalStop), просто m_rtcCalT0 берём из ts_activation.
    connect(ui->btnRtcCalActStop, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_tsActivation == 0xFFFFFFFFu || m_tsActivation == 0u) {
            appendLog(QStringLiteral("⚠ Устройство не активировано — нет t0 (обновите GET_STATS)"));
            requestCmd(LtpCmd::GET_STATS, {}, TagManual);
            return;
        }
        m_rtcCalT0 = QDateTime::fromSecsSinceEpoch(qint64(m_tsActivation));
        m_rtcCalActive = true;
        requestCmd(LtpCmd::RTC_CALIB_GET, {}, TagManual);      // текущая поправка (для расчёта новой)
        requestCmd(LtpCmd::GET_DATETIME, {}, TagRtcCalStop);   // → расчёт
        appendLog(QStringLiteral("[RTC-калибровка] стоп от активации (t0=%1)")
            .arg(m_rtcCalT0.toString(QStringLiteral("yy.MM.dd HH:mm:ss"))));
    });
    connect(ui->btnRtcCalApply, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        float p = float(m_rtcCalNewPpm);
        quint32 u; std::memcpy(&u, &p, 4);
        QByteArray b; for (int i = 0; i < 4; ++i) b.append(char((u >> (8*i)) & 0xFF));
        requestCmd(LtpCmd::RTC_CALIB_SET, b, TagManual);
        appendLog(QStringLiteral("[TX] RTC_CALIB_SET %1 ppm").arg(m_rtcCalNewPpm, 0, 'f', 1));
        regEvent(ui->editSerial->text().trimmed(), QStringLiteral("калибровка RTC"),
                 QStringLiteral("%1 ppm").arg(m_rtcCalNewPpm, 0, 'f', 1));
        requestCmd(LtpCmd::RTC_CALIB_GET, {}, TagManual);   // контроль
        rtcCalUpdateButtons(0);   // применено → всё в исходное
        ui->lblRtcCalStatus->setText(QStringLiteral("Поправка применена. Для контроля прогоните ещё одну выдержку."));
    });
    // Моргание «Применить» в состоянии «расчёт готов» (переключаем фон).
    connect(&m_rtcApplyBlinkTimer, &QTimer::timeout, this, [this] {
        m_rtcApplyBlinkOn = !m_rtcApplyBlinkOn;
        ui->btnRtcCalApply->setStyleSheet(m_rtcApplyBlinkOn
            ? QStringLiteral("background:#9A7B1A;color:#F5F4F0;") : QString());
    });
    // Восстановление незакрытой выдержки: «Старт» мог быть в прошлой сессии W
    // (приложение закрывали за ночь). t0 зафиксирован в настройках → продолжаем.
    {
        QSettings st(kOrg, kApp);
        if (st.value(QStringLiteral("rtcCal/active"), false).toBool()) {
            m_rtcCalT0 = st.value(QStringLiteral("rtcCal/t0")).toDateTime();
            if (m_rtcCalT0.isValid()) {
                m_rtcCalActive = true;
                rtcCalUpdateButtons(1);   // «Старт» красная — выдержка продолжается
                ui->lblRtcCalStatus->setText(QStringLiteral("Выдержка продолжается с %1 — нажмите «Стоп»")
                    .arg(m_rtcCalT0.toString(QStringLiteral("yy.MM.dd HH:mm:ss"))));
            }
        }
    }

    // «Калибровка скорости» («Калибровка») — чтение/дефолт/запись таблицы узлов.
    connect(ui->btnSpeedCalRead, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        requestCmd(LtpCmd::SPEED_CAL_GET, {}, TagManual);
    });
    connect(ui->btnSpeedCalDefault, &QPushButton::clicked, this, [this] {
        // Вкомпилированный дефолт прошивки (kSpeedCalDefault) — для наглядного
        // старта редактирования; на устройство попадёт только по «Записать».
        // Узловые скорости (задано), текущий коэфф = 1 (по умолчанию — без поправки),
        // измерено пусто (заполнит прогон «Калибровать» или ручной ввод).
        static const double nodes[] = { 10,25,50,70,90,115,150,195,245,292,330 };
        const int nn = int(sizeof(nodes)/sizeof(nodes[0]));
        ui->tblSpeedCal->setRowCount(nn);
        { QSignalBlocker b(ui->tblSpeedCal);
          for (int i = 0; i < nn; ++i) {
              speedCalSetCell(i, 0, QString::number(nodes[i], 'f', 1), true);   // задано
              speedCalSetCell(i, 1, QString(),                        false);   // измерено — только прогон
              speedCalSetCell(i, 2, QStringLiteral("1.000"),          false);   // текущий = что в приборе
              speedCalRecompute(i);
          } }
        appendLog(QStringLiteral("[Калибровка] шаблон: узлы, текущий=1 (не записано)"));
    });
    connect(ui->btnSpeedCalWrite, &QPushButton::clicked, this, [this] {
        speedCalWrite(true);
    });
    // Ручная правка «Задано»/«Измерено» → пересчёт Δ% и «Коэфф. новый» в строке.
    connect(ui->tblSpeedCal, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (!it) return;
        // Правка «Задано»/«Измерено» — пересчитать строку целиком.
        if (it->column() == 0 || it->column() == 1) {
            QSignalBlocker b(ui->tblSpeedCal);
            speedCalRecompute(it->row());
            return;
        }
        // Правка «Нового» (06.08.2026). Правят именно его: в прибор уходит
        // ровно этот столбец. «Текущий» — это что в приборе СЕЙЧАС, его правка
        // ничего никуда не отправит.
        //
        // ⚠ НИЧЕГО НЕ СЧИТАЕМ. Пробовали пересчитывать «Δ расч» под введённый
        // коэффициент — убрано: это выглядит как предсказание остаточного
        // отклонения, а предсказать его нельзя, следующее измерение выйдет
        // другим (повторяемость замера гуляет на проценты). Просто гасим
        // дельты: они относились к посчитанному коэффициенту, не к вашему.
        if (it->column() == 4) {
            QSignalBlocker b(ui->tblSpeedCal);
            speedCalSetCell(it->row(), 5, QString(), false);   // Δ расч
        }
    });
    // Авто-калибровка: кнопка-переключатель (Калибровать/Стоп) + таймер прохода.
    connect(ui->btnSpeedCalAuto, &QPushButton::clicked, this, [this] {
        if (m_speedCal.running) speedCalAutoStop(false); else speedCalAutoStart();
    });
    connect(&m_speedCalTimer, &QTimer::timeout, this, &MainWindow::speedCalAutoTick);
    // Общая галка «Все» — отметить/снять все скорости для прогона калибровки.
    connect(ui->chkSpeedCalAll, &QCheckBox::clicked, this, [this](bool on) {
        QSignalBlocker b(ui->tblSpeedCal);
        for (int r = 0; r < ui->tblSpeedCal->rowCount(); ++r)
            if (auto *it = ui->tblSpeedCal->item(r, 0))
                it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    });
    // Таблица калибровки: 5 столбцов делят ширину поровну (без горизонтального
    // скролла), без вертикальной нумерации, заголовки — вправо.
    ui->tblSpeedCal->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tblSpeedCal->horizontalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->tblSpeedCal->verticalHeader()->setVisible(false);
    ui->tblSpeedCal->verticalHeader()->setDefaultSectionSize(30);   // компактнее строки — все узлы влезают
    ui->tblSpeedCal->horizontalHeader()->setStyleSheet(
        QStringLiteral("QHeaderView::section{font-size:8pt;padding:2px;}"));  // мельче — подписи влезают
    // Вкладка «Калибровка»: левый столбец уже, правый (таблица) шире; верхняя
    // строка выше нижней (столбцы/строки задаём кодом — uic не берёт из .ui).
    ui->calibColumns->setColumnStretch(0, 2);
    ui->calibColumns->setColumnStretch(1, 5);
    ui->calibColumns->setRowStretch(0, 3);
    ui->calibColumns->setRowStretch(1, 1);

    // ── Паспорт устройства (стр.123): чтение/запись серийник/вариант/дата ──────
    connect(ui->btnDevInfoRead, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        requestCmd(LtpCmd::PASSPORT_GET, {}, TagManual);
        appendLog(QStringLiteral("[TX] PASSPORT_GET"));
    });
    connect(ui->btnDevInfoWrite, &QPushButton::clicked, this, [this] {
        if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
        // Дата выпуска = день паспортизации, то есть СЕГОДНЯ (06.08.2026).
        // Поле с экрана убрано: вводить руками было нечего, а совпадение с
        // датой записи в реестре делало его дублем. В самом приборе поле
        // остаётся — это часть паспорта.
        const QDate date = QDate::currentDate();
        if (QMessageBox::question(this, QStringLiteral("Запись паспорта"),
                QStringLiteral("Записать паспорт?"))
            != QMessageBox::Yes) return;
        QByteArray serial = ui->editSerial->text().trimmed().toLatin1();
        serial.truncate(15);
        serial.append(QByteArray(16 - serial.size(), '\0'));   // ровно 16 байт, '\0'-паддинг
        const quint16 y  = date.isValid() ? quint16(date.year())  : 0;
        const quint8  mo = date.isValid() ? quint8(date.month()) : 0;
        const quint8  da = date.isValid() ? quint8(date.day())   : 0;
        QByteArray p;
        p.append(serial);                                       // [0..15] серийник
        p.append(char(m_variantCode));   // [16] вариант — из WHO_AM_I датчика, не с экрана
        p.append(char(y & 0xFF)); p.append(char((y >> 8) & 0xFF));             // [17..18] год LE
        p.append(char(mo));                                      // [19] месяц
        p.append(char(da));                                      // [20] день
        // Повторная паспортизация (ТЗ реестра §5): паспорт — замороженный
        // параметр, молча переписывать его нельзя. Пароля пока нет (отложен),
        // поэтому просто подтверждение.
        if (m_passportPresent &&
            QMessageBox::question(this, QStringLiteral("Паспорт уже записан"),
                QStringLiteral("В приборе уже есть паспорт. Перезаписать его?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            appendLog(QStringLiteral("Паспорт: перезапись отменена"));
            return;
        }
        requestCmd(LtpCmd::PASSPORT_SET, p, TagManual);
        appendLog(QStringLiteral("[TX] PASSPORT_SET: %1").arg(ui->editSerial->text().trimmed()));
        requestCmd(LtpCmd::PASSPORT_GET, {}, TagManual);         // контрольное чтение
    });

    connect(ui->btnFwBrowse, &QPushButton::clicked, this, [this] {
        // Куда открывать диалог. По умолчанию Qt открывает каталог СВОЕЙ сборки
        // (Desktop_Qt_..._Debug) — там прошивки нет и не будет, каждый раз
        // пришлось бы идти через полдерева. Подставляем сразу ФАЙЛ, а не папку:
        // тогда образ виден и выбран, достаточно нажать «Открыть». По порядку:
        //   1. уже выбранный файл (обычный повторный заход);
        //   2. запомненный с прошлого раза;
        //   3. свежая сборка прошивки рядом с исходниками LOGLSMW.
        QSettings st(kOrg, kApp);
        QString start = ui->editFwFile->text().trimmed();
        if (start.isEmpty() || !QFileInfo::exists(start))
            start = st.value(QStringLiteral("fwFile")).toString();
        if (start.isEmpty() || !QFileInfo::exists(start)) {
            start.clear();
#ifdef LOGLSMW_SRC_DIR
            // SoftWare/LOGLSMW → ../../Firmware/LOGLSMA/build/<конфигурация>
            const QString fw = QDir(QStringLiteral(LOGLSMW_SRC_DIR "/../../Firmware/LOGLSMA/build"))
                                   .absolutePath();
            for (const QString &cfg : { QStringLiteral("Debug"), QStringLiteral("Release") }) {
                QDir d(fw + QLatin1Char('/') + cfg);
                if (!d.exists()) continue;
                // Имя образа обновления несёт версию (UpLSMA_ГГ.ММ.ДД_ЧЧ.ММ.bin),
                // поэтому ищем по маске и берём самый свежий. Заодно понимаем
                // старое имя LOGLSMA.bin — вдруг рядом лежит от прошлых сборок.
                const auto found = d.entryInfoList(
                    { QStringLiteral("UpLSMA*.bin"), QStringLiteral("LOGLSMA.bin") },
                    QDir::Files, QDir::Time);
                if (!found.isEmpty()) { start = found.first().absoluteFilePath(); break; }
                if (start.isEmpty()) start = d.absolutePath();                 // хотя бы папка
            }
#endif
        }

        const QString fn = QFileDialog::getOpenFileName(this,
            QStringLiteral("Файл прошивки STM32"), start,
            QStringLiteral("Образ обновления (UpLSMA*.bin);;Двоичный образ (*.bin);;Все файлы (*)"));
        if (!fn.isEmpty()) {
            ui->editFwFile->setText(fn);
            st.setValue(QStringLiteral("fwFile"), fn);
        }
    });
    connect(ui->btnFwUpdate, &QPushButton::clicked, this, [this] { fwUpdateStart(); });

    // Чтение внутренней Flash: своя страница + кнопки-закладки на интересные.
    connect(ui->btnIflashRead, &QPushButton::clicked, this,
            [this] { iflashReadStart(ui->spinIflashPage->value()); });
    connect(ui->btnIflashBoot, &QPushButton::clicked, this, [this] { iflashReadStart(113); });
    connect(ui->btnIflashAct,  &QPushButton::clicked, this, [this] { iflashReadStart(121); });
    connect(ui->btnIflashCfg,  &QPushButton::clicked, this, [this] { iflashReadStart(123); });
    connect(ui->btnIflashJrn,  &QPushButton::clicked, this, [this] { iflashReadStart(124); });
    // Пауза после команды «уйти в загрузчик»: прибор отвечает, потом сбрасывается.
    // Спросить его раньше, чем он поднимется, = гарантированный ложный таймаут.
    m_fwWaitTimer.setSingleShot(true);
    connect(&m_fwWaitTimer, &QTimer::timeout, this, [this] {
        if (!m_fw.running) return;
        m_fw.phase = 2;                       // подтверждение: кто на линии?
        requestCmd(LtpCmd::BOOT_ENTER, {}, TagFw);
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

    // Формат записи цикла (28.07.2026): маркёр слова — 0 базовый / 1 уплотнённый /
    // 2 подробный. Запоминаем выбор, при смене сразу шлём прибору (REC_FORMAT 0x31).
    ui->cmbRecFormat->setCurrentIndex(QSettings(kOrg, kApp)
        .value(QStringLiteral("device/recFormat"), 0).toInt());
    connect(ui->cmbRecFormat, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        QSettings(kOrg, kApp).setValue(QStringLiteral("device/recFormat"), idx);
        if (m_link->isOpen()) {
            requestCmd(LtpCmd::REC_FORMAT, QByteArray(1, char(quint8(idx))), TagManual);
            appendLog(QStringLiteral("[TX] формат записи → %1")
                .arg(ui->cmbRecFormat->currentText()));
        }
    });

    // Отображаемый текст SPI/МГц не обновлялся при переключении (жалоба
    // 22.07.2026) — код на эти комбобоксы нигде не влияет, стороннего
    // вмешательства не нашёл, но форсируем repaint() на смену индекса как
    // защиту на случай платформенной особенности QComboBox+QSS.
    // Режим SPI (22.07.2026) — ТОЛЬКО ручная проверка/сравнение в Сервисе
    // (индекс combo 0=SPI/1=SPIx4 совпадает с payload[0] прошивки 1:1).
    ui->comboSpiMode->setCurrentIndex(1);   // дефолт = SPIx4 (максимум); список остаётся для характеризации
    connect(ui->comboSpiMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (idx < 0) return;
        const quint8 payload = quint8(idx);
        requestCmd(LtpCmd::FLASH_SET_SPI_MODE, QByteArray(1, char(payload)), TagManual);
        appendLog(QStringLiteral("[Тест памяти] режим SPI → %1").arg(ui->comboSpiMode->currentText()));
    });
    connect(ui->comboSpiMhz, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (idx < 0) return;
        requestCmd(LtpCmd::FLASH_SET_FREQ, QByteArray(1, char(quint8(idx))), TagManual);
        appendLog(QStringLiteral("[Тест памяти] частота флеша → %1 МГц").arg(ui->comboSpiMhz->currentText()));
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
        memTestDump(ui->spinMemStartPage->value() - 1, qMax(1, ui->spinMemPages->value()));
    });
    // Кнопки стирания: подпись без номера страницы (адрес задаётся в параметрах)
    ui->btnMemErasePage->setText(QStringLiteral("⚠ Страница"));
    ui->btnMemEraseSector->setText(QStringLiteral("⚠ Сектор"));
    ui->btnMemEraseChip->setText(QStringLiteral("⚠ Чип"));

    connect(ui->btnMemErasePage, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_dataFlagSet && !m_test.running) { memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные»")); return; }
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
        const quint16 start = quint16(ui->spinMemStartPage->value() - 1);
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
        if (m_dataFlagSet) { memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные»")); return; }
        if (QMessageBox::question(this, QStringLiteral("Стереть чип"),
                QStringLiteral("Стереть ВСЮ память? Операция до ~30 с."))
            != QMessageBox::Yes) return;
        memLog(QStringLiteral("[Стирание чипа…]"));
        m_eraseStartMs = QDateTime::currentMSecsSinceEpoch();
        setOpsEnabled(false);
        requestCmd(LtpCmd::FLASH_ERASE, {}, TagManual);
    });
    // «Стереть данные» (0x2A) — перенесена из активации в «Тест памяти»
    // (27.07.2026), третья после Запись/Чтение. Блокируется флагом непрочитанных.
    connect(ui->btnMemEraseData, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_dataFlagSet) { memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные» (активация)")); return; }
        if (QMessageBox::question(this, QStringLiteral("Стирание данных"),
                QStringLiteral("Стереть данные регистратора (NOR, со страницы 1)? Служебная "
                    "страница 0 и счётчики перезапусков не затрагиваются."))
            != QMessageBox::Yes) return;
        memLog(QStringLiteral("[Стереть данные…]"));
        requestCmd(LtpCmd::FLASH_ERASE_DATA, {}, TagManual);
    });
    connect(ui->btnMemEraseSector, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        if (m_dataFlagSet) { memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные»")); return; }
        const quint16 sec = quint16((ui->spinMemStartPage->value() - 1) / 16);
        QByteArray p; p.append(char(sec & 0xFF)); p.append(char((sec >> 8) & 0xFF));
        memLog(QStringLiteral("[Стирание сектора %1 (стр. %2–%3)]")
                   .arg(sec).arg(sec * 16).arg(sec * 16 + 15));
        setOpsEnabled(false);
        requestCmd(LtpCmd::FLASH_SECTOR_ERASE, p, TagManual);
    });
    // Запись: первое нажатие — старт, повторное — стоп
    connect(ui->btnMemWrite, &QPushButton::clicked, this, [this, memLog, testByte] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        // Блокировка при непрочитанных данных (27.07.2026): запись разрушительна.
        if (m_dataFlagSet && !(m_test.running && m_test.step == TestStep::Write)) {
            memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные» (активация)"));
            return;
        }
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
        const quint16 start  = quint16(ui->spinMemStartPage->value() - 1);
        // Предупреждение (22.07.2026): страница 0 служебная (зарезервирована,
        // см. FLASH_ERASE_DATA в прошивке — стирание всегда оставляет её
        // нетронутой). Писать туда можно ТОЛЬКО вручную через «Запись»,
        // остальные операции (Термотест, Образ) всегда стартуют минимум со
        // страницы 1 (см. kLogStartPage) и сюда попасть не могут.
        if (start == 0) {
            if (QMessageBox::question(this, QStringLiteral("Запись на служебную страницу"),
                    QStringLiteral("Страница 0 зарезервирована (служебная) — обычно её "
                        "не трогают. Термотест и запись образа туда никогда не "
                        "пишут. Продолжить запись именно сюда?"))
                != QMessageBox::Yes) return;
        }
        // Предупреждение (22.07.2026, по обсуждению): запись НЕ на границе
        // известной свободной страницы (m_firstFreePage) создаёт «остров»
        // занятых страниц посреди свободного места — бинарный поиск
        // (flashBinSearchStart) предполагает Flash заполненной СТРОГО ПОДРЯД
        // от стр.1, и с таким «островом» дальше даёт непредсказуемый
        // результат (следующая «Загрузка» может писать не туда). Тест —
        // произвольный доступ по определению, к «Работе» не относится;
        // просто спрашиваем подтверждение, не блокируем.
        if (m_firstFreePage >= 0 && start > quint16(m_firstFreePage)
            && start > quint16(kLogStartPage)) {
            if (QMessageBox::question(this, QStringLiteral("Запись не на границе"),
                    QStringLiteral("Старт (стр.%1) дальше первой свободной страницы "
                        "(стр.%2) — между ними останется дыра. Это собьёт "
                        "автопоиск свободного места для «Загрузка» образа "
                        "(она может начать писать не туда). Продолжить?")
                        .arg(start + 1).arg(m_firstFreePage + 1))
                != QMessageBox::Yes) return;
        }
        const int     n      = ui->spinMemPages->value();
        const int     cycles = ui->spinMemCycles->value();
        const quint8  b      = testByte();
        m_test = { true, n, 0, start, 0, TestStep::Write, cycles, 0 };
        ui->lblMemSpeed->setText(QStringLiteral(" "));
        ui->btnMemWrite->setText(QStringLiteral("Запись ■"));
        memLog(QStringLiteral("[Запись] стр.%1..%2 <- 0x%3 x256 (%4 стр. × %5 цикл.), "
                               "с контролем чтением после каждой страницы")
                   .arg(start).arg(start + n - 1)
                   .arg(b, 2, 16, QLatin1Char('0')).arg(n).arg(cycles));
        setOpsEnabled(false, ui->btnMemWrite);
        memTestUpdateUi();
        // Пара «записать страницу → сразу прочитать её же обратно» (22.07.2026,
        // по просьбе: раньше «Запись» не проверяла себя вообще, ошибку ловило
        // только отдельное «Чтение»). TagWriteVerify — отдельный тег для этого
        // контрольного чтения, чтобы не путать со standalone «Чтение» (TagManual).
        // При расхождении обработчик FLASH_READ/TagWriteVerify чистит очередь
        // (m_dev->clearQueue()) — дальнейшие страницы уже не запишутся.
        for (int c = 0; c < cycles; ++c)
            for (int i = 0; i < n; ++i) {
                const quint16 pg = quint16(start + i);
                QByteArray p;
                p.append(char(pg & 0xFF)); p.append(char((pg >> 8) & 0xFF));
                p.append(QByteArray(256, char(b)));
                m_dev->enqueue(LtpCmd::FLASH_WRITE, p, TagManual);

                const quint32 addr = quint32(pg) << 8;
                QByteArray rp;
                for (int j = 0; j < 4; ++j) rp.append(char((addr >> (8*j)) & 0xFF));
                for (int j = 0; j < 4; ++j) rp.append(char((256   >> (8*j)) & 0xFF));
                m_dev->enqueue(LtpCmd::FLASH_READ, rp, TagWriteVerify);
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
        const quint16 start  = quint16(ui->spinMemStartPage->value() - 1);
        const int     n      = ui->spinMemPages->value();
        const int     cycles = ui->spinMemCycles->value();
        m_test = { true, n, 0, start, 0, TestStep::Read, cycles, 0 };
        ui->lblMemSpeed->setText(QStringLiteral(" "));
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

    // Кнопка «Скорость» (26.07.2026): запускает замер записи; по ответу
    // цепочка сама запустит замер чтения, итог — W/R в индикаторе.
    connect(ui->btnSpeedTest, &QPushButton::clicked, this, [this, memLog] {
        if (!m_link->isOpen()) { memLog(QStringLiteral("⚠ Нет подключения")); return; }
        // Эталон в КОНЦЕ памяти, размер зависит только от «Страниц» → сменили
        // число страниц → переписать (03.08.2026: «Старт» на тест скорости не влияет).
        if (m_speedRefReady && ui->spinMemPages->value() != m_speedRefPages)
            m_speedRefReady = false;
        if (!m_speedRefReady) {
            // ЭТАЛОН пишем ОДИН раз за сессию на БЕЗОПАСНОЙ низкой частоте
            // (10 МГц, quad) с verify — гарантируем корректную ground-truth в
            // области. По успеху onResponse сам переключит на ВЫБРАННУЮ
            // конфигурацию и запустит чтение по готовому.
            {   const int n = qMax(1, ui->spinMemPages->value());
                const int endStart = int(kFlashTotalPages) - n + 1;   // экранная стр. (адрес+1)
                memLog(QStringLiteral("[Скорость] пишу эталон (10 МГц, quad) в КОНЦЕ памяти, стр.%1..%2…")
                       .arg(endStart).arg(int(kFlashTotalPages))); }
            m_speedPhase = 1;
            requestCmd(LtpCmd::FLASH_SET_SPI_MODE, QByteArray(1, char(1)), TagManual);  // quad
            requestCmd(LtpCmd::FLASH_SET_FREQ,     QByteArray(1, char(3)), TagManual);  // idx3 = 10 МГц
            showMemSpeed(0);   // стирание + запись эталона + verify
        } else {
            // Эталон готов — применяем ВЫБРАННУЮ конфигурацию и только читаем+сверяем.
            m_speedPhase = 2;
            const quint8 spiIdx  = quint8(qMax(0, ui->comboSpiMode->currentIndex()));
            const quint8 freqIdx = quint8(qMax(0, ui->comboSpiMhz->currentIndex()));
            requestCmd(LtpCmd::FLASH_SET_SPI_MODE, QByteArray(1, char(spiIdx)),  TagManual);
            requestCmd(LtpCmd::FLASH_SET_FREQ,     QByteArray(1, char(freqIdx)), TagManual);
            showMemSpeed(1);
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
        // Гейт «есть несохранённые данные» здесь СНЯТ (02.08.2026): запись образа
        // неразрушающая — идёт с первой свободной страницы, старые данные не
        // стираются (см. ниже, m_firstFreePage). Проверка «Сохранить» мешала в
        // сервисе (напр. при загрузке тестового образа со стыками), а данным
        // ничего не грозит. Гейты стирания/термотеста/активации оставлены —
        // они реально разрушительны.

        QSettings st(kOrg, kApp);
        const QString lastDir = st.value(QStringLiteral("memtest/lastImgDir"),
                                          workDir()).toString();
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
        // Пишем с ПЕРВОЙ СВОБОДНОЙ страницы (22.07.2026, по просьбе) — старые
        // данные больше не стираются автоматом, образ дописывается после них.
        // m_firstFreePage поддерживается живым сканированием (flashBinSearchStart,
        // см. подключение/после операций) — если ещё не известно, просим
        // подождать/переподключиться, а не гадаем стартовую страницу.
        if (m_firstFreePage < 0) {
            memLog(QStringLiteral("⚠ Образ: первая свободная страница ещё не известна — "
                                   "подождите сканирование Flash и повторите"));
            return;
        }
        if (m_firstFreePage >= int(kFlashTotalPages)) {
            memLog(QStringLiteral("⚠ Образ: свободного места нет — Flash полностью занята"));
            return;
        }
        if (m_firstFreePage + pages.size() > int(kFlashTotalPages)) {
            memLog(QStringLiteral("⚠ Образ: недостаточно свободного места (нужно %1 стр., "
                                   "доступно %2 стр. с текущей позиции)")
                       .arg(pages.size()).arg(int(kFlashTotalPages) - m_firstFreePage));
            return;
        }
        startPage = quint16(qMax(int(m_firstFreePage), int(kLogStartPage)));
        // «Адрес» (Данные) — реальный адрес операции, как и раньше.
        ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(startPage) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
        // «Старт»/«Страниц» (Проверка) — теперь и образ отображает ход здесь
        // (22.07.2026, по просьбе, тот же принцип, что у термотеста): Задать —
        // первая свободная страница / количество страниц образа, Факт —
        // текущая активная страница / сколько обработано (см. обработчик
        // FLASH_WRITE, тег TagImg).
        ui->spinMemStartPage->setValue(startPage + 1);
        ui->spinMemPages->setValue(int(pages.size()));
        appendLog(QStringLiteral(
            "[Образ] %1 — первая свободная стр.%2, пишу стр.%2..%3 (%4 стр.), "
            "старые данные сохраняются")
                .arg(QFileInfo(path).fileName())
                .arg(startPage + 1)
                .arg(startPage + pages.size())
                .arg(pages.size()));
        startImageWrite(btn, btn->text(), startPage, pages, QFileInfo(path).fileName());
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
        // Термотест пишет во Flash → как стирание/активация, блокируем при
        // взведённом флаге несохранённых данных (28.07.2026, был пропущен гейт).
        if (m_dataFlagSet) { memLog(QStringLiteral("⚠ Есть несохранённые данные — сначала «Сохранить данные»")); return; }
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
        // «Активировать» (27.07.2026): ЗЕЛЁНАЯ ВОЛНА — полная пошаговая
        // активация (блок «Проверка устройства»): Считать данные → Проверка
        // устройства → Синхро время → Стереть → Тестовая запись → Постановка на
        // готовность. Каждый пройденный сегмент зеленеет; финал взводит
        // персистентный флаг активации + ts (стр.121/122). Повторное нажатие
        // (■) во время прогона — стоп.
        if (m_act.step != ActStep::Idle && m_act.step != ActStep::Done
                && m_act.step != ActStep::Error) {
            activationStop();
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Активация"),
                QStringLiteral("Запустить активацию устройства? Будет выполнена проверка "
                    "(считывание/сохранение данных, датчики, время), стирание памяти "
                    "и постановка на готовность."))
            != QMessageBox::Yes) return;
        activationStart();
    });
    // Ширина всех боксов «Проверка» задана явно в .ui (50px, единообразно
    // для обеих колонок) — раньше здесь стоял код, перезаписывавший col-1
    // (QSpinBox/editTestByte) поверх этого значением 90px, из-за чего
    // «Задать» и «Факт» визуально расходились по ширине (22.07.2026, найдено
    // по жалобе на разную ширину столбцов). Тот код был актуален во времена
    // 66px-боксов без явных ограничений в .ui — сейчас не нужен, убран.
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
                    m_silentDump = true;
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
        // «Калибровка»: калибровка скорости + текущая поправка RTC.
        if (m_link->isOpen() && ui->tabsMain->currentWidget() == ui->tabCalibration) {
            m_dev->enqueue(LtpCmd::SPEED_CAL_GET, {}, TagManual);
            m_dev->enqueue(LtpCmd::RTC_CALIB_GET, {}, TagManual);
            m_dev->enqueue(LtpCmd::SYNC_REF_GET, {}, TagSyncCheck);   // состояние опорной точки → кнопки
        }
        // «FLASH STM»: состояние активации (GET_STATS) + история активаций (0x2E).
        if (m_link->isOpen() && ui->tabsMain->currentWidget() == ui->tabFlashStm) {
            m_dev->enqueue(LtpCmd::GET_STATS, {}, TagManual);
            m_dev->enqueue(LtpCmd::ACT_HISTORY, {}, TagManual);
            m_dev->enqueue(LtpCmd::PASSPORT_GET, {}, TagManual);   // паспорт стр.123 → поля
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

// Ячейка таблицы калибровки скорости: число выровнено ВПРАВО (при одинаковом
// числе десятичных знаков в столбце цифры встают под цифрами). editable=false —
// производный/справочный столбец (не редактируется вручную).
void MainWindow::speedCalSetCell(int row, int col, const QString &text, bool editable)
{
    auto *it = new QTableWidgetItem(text);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    if (col == 0) {   // «Задано» — с галкой включения скорости в прогон калибровки
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        auto *old = ui->tblSpeedCal->item(row, 0);   // сохранить прежний выбор строки
        it->setCheckState(old ? old->checkState() : Qt::Checked);
    }
    ui->tblSpeedCal->setItem(row, col, it);
}

// Пересчитать производные столбцы строки по задано(0)/измерено(1):
//   [2] Δ% = (измерено−задано)/задано·100   [4] Коэфф. новый = задано/измерено.
// [3] «Коэфф. в работе» не трогаем — это значение, действующее на устройстве.
void MainWindow::speedCalRecompute(int row)
{
    auto txt = [this, row](int c){ auto *it = ui->tblSpeedCal->item(row, c);
        return it ? it->text().trimmed() : QString(); };
    const double given = txt(0).toDouble();       // задано
    const QString mtxt = txt(1);
    const double meas  = mtxt.toDouble();          // измерено = сырое × текущий
    double cur = txt(2).toDouble(); if (cur <= 0.0) cur = 1.0;   // текущий (кол.2, дефолт 1)
    if (mtxt.isEmpty() || meas <= 0.0 || given <= 0.0) {         // нет измерения → пусто
        speedCalSetCell(row, 3, QString(), false);   // Δ изм
        speedCalSetCell(row, 4, QString(), true);    // новый — пусто, но правится руками
        speedCalSetCell(row, 5, QString(), false);   // Δ расч
        return;
    }
    const double dizm = (meas - given) / given * 100.0;         // Δ изм: остаточное отклонение с ТЕКУЩИМ коэфф
    const double koef = cur * given / meas;                     // новый = текущий×задано/измерено = задано/сырое
    const double raw  = meas / cur;                             // сырое = измерено/текущий
    const double drasch = (raw * koef - given) / given * 100.0; // Δ расч: остаточное с НОВЫМ коэфф (≈0 = минимум)
    speedCalSetCell(row, 3, QString::number(dizm,   'f', 1), false);
    // ⚠ «Новый» РЕДАКТИРУЕМЫЙ (06.08.2026): в прибор уходит именно этот
    // столбец, значит он и есть то, что задаёшь. Прогон просто подставляет
    // сюда своё значение, но его можно поправить или вписать без прогона.
    speedCalSetCell(row, 4, QString::number(koef,   'f', 3), true);
    speedCalSetCell(row, 5, QString::number(drasch, 'f', 1), false);
}

// Заполнить строку целиком: задано/измерено (редактируемые) + коэфф. в работе
// (справочный, с устройства), затем пересчитать производные Δ%/коэфф. новый.
void MainWindow::speedCalSetRow(int row, double given, double measured, double workKoef)
{
    QSignalBlocker b(ui->tblSpeedCal);
    speedCalSetCell(row, 0, QString::number(given,    'f', 1), true);
    speedCalSetCell(row, 1, QString::number(measured, 'f', 1), false);  // измерено — только прогон
    speedCalSetCell(row, 2, QString::number(workKoef, 'f', 3), false);   // текущий
    speedCalRecompute(row);
}

// Записать коэффициенты ИЗМЕРЕННЫХ узлов на устройство (0x32). confirm=false —
// без диалога (авто-обновление после цикла). Возврат true при отправке.
bool MainWindow::speedCalWrite(bool confirm, bool reread)
{
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return false; }
    const int rc = ui->tblSpeedCal->rowCount();
    QByteArray body; int cnt = 0; double prevR = -1e9;
    for (int i = 0; i < rc; ++i) {
        auto cell = [this,i](int c){ auto*it=ui->tblSpeedCal->item(i,c); return it?it->text().trimmed():QString(); };
        const QString mtxt = cell(1);
        const QString ktxt = cell(4);
        if (ktxt.isEmpty()) continue;                   // коэффициента нет — писать нечего

        bool okK=false;
        const float newk = ktxt.toFloat(&okK);
        if (!okK || newk <= 0.f) { appendLog(QStringLiteral("⚠ Строка %1: битый коэффициент").arg(i+1)); return false; }

        float cur = cell(2).toFloat(); if (cur <= 0.f) cur = 1.f;
        const float given = cell(0).toFloat();

        float r;
        if (mtxt.isEmpty()) {
            // Прогона не было, коэффициент вписан руками (06.08.2026) — перенос
            // из журнала, повтор на другом приборе, правка одного узла. Ключ
            // интерполяции выводим из определения k = задано/сырое, то есть
            // сырое = задано/k: узел получается тот же, что дал бы прогон.
            if (given <= 0.f) { appendLog(QStringLiteral("⚠ Строка %1: коэффициент есть, а скорость — нет").arg(i+1)); return false; }
            r = given / newk;
        } else {
            bool okM=false;
            const float meas = mtxt.toFloat(&okM);
            if (!okM || meas <= 0.f) { appendLog(QStringLiteral("⚠ Строка %1: битое измерение").arg(i+1)); return false; }
            r = meas / cur;                             // сырое = измерено/текущий (ключ интерполяции)
        }
        if (r <= prevR) { appendLog(QStringLiteral("⚠ скорость должна строго возрастать (строка %1)").arg(i+1)); return false; }
        prevR = r;
        for (int b=0;b<4;++b) body.append(char((*reinterpret_cast<const quint32*>(&r)    >> (8*b)) & 0xFF));
        for (int b=0;b<4;++b) body.append(char((*reinterpret_cast<const quint32*>(&newk) >> (8*b)) & 0xFF));
        ++cnt;
    }
    if (cnt < 2 || cnt > 16) { appendLog(QStringLiteral("⚠ Нужно 2–16 ИЗМЕРЕННЫХ узлов (сейчас %1)").arg(cnt)); return false; }
    if (confirm && QMessageBox::question(this, QStringLiteral("Запись калибровки"),
            QStringLiteral("Записать %1 узлов калибровки во внутреннюю Flash (стр.123)?").arg(cnt))
        != QMessageBox::Yes) return false;
    QByteArray p; p.append(char(cnt)); p.append(body);
    requestCmd(LtpCmd::SPEED_CAL_SET, p, TagManual);
    appendLog(QStringLiteral("[TX] SPEED_CAL_SET: %1 узлов").arg(cnt));

    // Реестр (06.08.2026): узлы пишем в журнал ЧИСЛАМИ — «задано=коэффициент».
    // Ровно в том виде, в каком их можно перепечатать обратно в таблицу, если
    // прибор придётся калибровать заново после стирания стр.123.
    {
        QStringList nodes;
        for (int i = 0; i < rc; ++i) {
            auto cell = [this,i](int c){ auto*it=ui->tblSpeedCal->item(i,c); return it?it->text().trimmed():QString(); };
            if (cell(4).isEmpty()) continue;
            nodes << QStringLiteral("%1=%2").arg(cell(0), cell(4));
        }
        regEvent(ui->editSerial->text().trimmed(), QStringLiteral("калибровка скорости"),
                 nodes.join(QStringLiteral(" ")));
    }
    if (reread)
        requestCmd(LtpCmd::SPEED_CAL_GET, {}, TagManual);   // контрольное чтение (текущий←новый)
    return true;
}

// ── Авто-калибровка скорости: стенд по столбцу «Задано», живой опрос гироскопа ─
// Время разгона/паузы и измерения — из UI (m_speedCal.settleMs/measMs).
static constexpr int kSpeedCalTickMs   = 100;    // период тика/опроса (чаще → больше выборок)

void MainWindow::speedCalAutoStart()
{
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
    const int n = ui->tblSpeedCal->rowCount();
    if (n < 1) { appendLog(QStringLiteral("⚠ Таблица пуста — задайте скорости в «Задано»")); return; }
    QVector<double> targets; QVector<int> rows;
    for (int i = 0; i < n; ++i) {
        auto *it = ui->tblSpeedCal->item(i, 0);
        if (!it || it->checkState() != Qt::Checked) continue;   // не отмечена — пропускаем
        const double v = it->text().trimmed().toDouble();
        if (v <= 0.0) { appendLog(QStringLiteral("⚠ Строка %1: «Задано» не число").arg(i+1)); return; }
        targets << v; rows << i;
    }
    if (targets.isEmpty()) { appendLog(QStringLiteral("⚠ Не отмечено ни одной скорости")); return; }
    if (QMessageBox::question(this, QStringLiteral("Калибровка скорости"),
            QStringLiteral("Прогнать стенд по %1 отмеченным скоростям и измерить гироскопом?\n"
                           "Мотор будет вращаться.").arg(targets.size())) != QMessageBox::Yes) return;

    m_dev->enqueue(LtpCmd::GYRO_GET_FS, {}, TagMon);   // чувствительность гироскопа (raw→°/с)

    m_speedCal.running = true;
    m_speedCal.targets = targets;
    m_speedCal.rows    = rows;
    m_speedCal.curRow  = 0;
    m_speedCal.phase   = 0;
    m_speedCal.phaseMs = 0;
    m_speedCal.samples.clear();
    m_speedCal.settleMs    = ui->spinCalPause->value()  * 1000;   // пауза/разгон (может быть 0)
    m_speedCal.measMs      = ui->spinCalDwell->value()  * 1000;   // время измерения
    m_speedCal.cyclesTotal = ui->spinCalCycles->value();
    m_speedCal.cycle       = 0;
    m_speedCal.autoUpdate  = ui->chkCalAutoUpdate->isChecked();
    ui->btnSpeedCalAuto->setText(QStringLiteral("Стоп"));
    appendLog(QStringLiteral("[Калибровка] старт: %1 ступеней × %2 циклов%3")
                  .arg(targets.size()).arg(m_speedCal.cyclesTotal)
                  .arg(m_speedCal.autoUpdate ? QStringLiteral(", авто-обновление") : QString()));

    auto sendSpeed = [this](double rpm) {
        const quint16 s = quint16(qRound(rpm));
        QByteArray p(3, 0);
        p[0] = char(s & 0xFF); p[1] = char((s >> 8) & 0xFF); p[2] = char(kStendMicrostepCoef);
        m_dev->enqueueTo(LtpAddr::STEND, LtpCmd::STEND_SPEED, p, TagStend);
        return s;
    };
    const quint16 s0 = sendSpeed(targets[0]);
    appendLog(QStringLiteral("[Калибровка] ступень 1/%1: задано %2 об/мин").arg(targets.size()).arg(s0));
    m_speedCalTimer.start(kSpeedCalTickMs);
}

void MainWindow::speedCalAutoTick()
{
    if (!m_speedCal.running) return;
    m_speedCal.phaseMs += kSpeedCalTickMs;

    // Мигание текущей ступени (~2.5 Гц): видно, где идёт проход.
    { QSignalBlocker b(ui->tblSpeedCal);
      speedCalClearHighlight();
      const bool on = (m_speedCal.phaseMs / 400) % 2 == 0;
      const int trow = (m_speedCal.curRow < m_speedCal.rows.size()) ? m_speedCal.rows[m_speedCal.curRow] : -1;
      speedCalHighlightRow(trow, on, m_speedCal.phase); }

    if (m_speedCal.phase == 0) {                       // разгон/устаканивание/пауза
        if (m_speedCal.phaseMs >= m_speedCal.settleMs) {
            m_speedCal.phase = 1; m_speedCal.phaseMs = 0;
            m_speedCal.samples.clear();
        }
        return;
    }
    // phase 1 — измерение: опрашиваем гироскоп (ответы копит speedCalAutoAccum)
    m_dev->enqueue(LtpCmd::GET_AXES_RAW, {}, TagSpeedCal);
    if (m_speedCal.phaseMs < m_speedCal.measMs) return;

    const int idx = m_speedCal.curRow;                       // индекс в прогоне
    const int row = m_speedCal.rows[idx];                    // фактическая строка таблицы
    // Усечённое среднее: сортируем, отбрасываем по 20% с краёв (выбросы/дрожь
    // разгона) → устойчивая оценка сырой об/мин, лучше повторяемость.
    double raw = 0.0;
    if (!m_speedCal.samples.isEmpty()) {
        std::sort(m_speedCal.samples.begin(), m_speedCal.samples.end());
        const int n = m_speedCal.samples.size();
        const int lo = n / 5, hi = n - n / 5;          // серединные 60%
        double sum = 0.0; int cnt = 0;
        for (int k = lo; k < hi; ++k) { sum += m_speedCal.samples[k]; ++cnt; }
        raw = (cnt > 0) ? sum / cnt : m_speedCal.samples[n/2];
    }
    { QSignalBlocker b(ui->tblSpeedCal);
      auto *ic = ui->tblSpeedCal->item(row, 2);         // текущий (кол.2)
      double cur = ic ? ic->text().trimmed().toDouble() : 0.0; if (cur <= 0.0) cur = 1.0;
      const double meas = raw * cur;                    // измерено = сырое × текущий
      speedCalSetCell(row, 1, QString::number(meas, 'f', 1), false);
      speedCalRecompute(row); }
    appendLog(QStringLiteral("[Калибровка] ступень %1/%2: сырое %3 об/мин (%4 выборок, усеч.)")
                  .arg(idx+1).arg(m_speedCal.targets.size()).arg(raw, 0, 'f', 1).arg(m_speedCal.samples.size()));

    auto sendSpeedIdx = [this](int j) {
        const quint16 s = quint16(qRound(m_speedCal.targets[j]));
        QByteArray p(3, 0);
        p[0] = char(s & 0xFF); p[1] = char((s >> 8) & 0xFF); p[2] = char(kStendMicrostepCoef);
        m_dev->enqueueTo(LtpAddr::STEND, LtpCmd::STEND_SPEED, p, TagStend);
        return s;
    };

    if (idx + 1 >= m_speedCal.targets.size()) {
        // ── Цикл (весь набор) пройден ──
        if (m_speedCal.autoUpdate) {
            // Записать коэффициенты в прибор (без re-read, чтобы не переформатировать
            // таблицу в прогоне) и локально сдвинуть текущий←новый: следующий цикл
            // измеряет уже с обновлённым коэффициентом.
            speedCalWrite(false, false);
            QSignalBlocker b(ui->tblSpeedCal);
            for (int rr : m_speedCal.rows) {
                auto *ik = ui->tblSpeedCal->item(rr, 4);
                if (ik && !ik->text().trimmed().isEmpty()) {
                    speedCalSetCell(rr, 2, ik->text().trimmed(), false);   // текущий = новый
                    speedCalRecompute(rr);
                }
            }
        }
        if (m_speedCal.cycle + 1 >= m_speedCal.cyclesTotal) { speedCalAutoStop(true); return; }
        ++m_speedCal.cycle;                       // следующий цикл — с начала набора
        m_speedCal.curRow = 0;
        m_speedCal.phase = 0; m_speedCal.phaseMs = 0;
        const quint16 s = sendSpeedIdx(0);
        appendLog(QStringLiteral("[Калибровка] цикл %1/%2, ступень 1/%3: задано %4 об/мин")
                      .arg(m_speedCal.cycle+1).arg(m_speedCal.cyclesTotal)
                      .arg(m_speedCal.targets.size()).arg(s));
        return;
    }
    // следующая ступень внутри цикла
    m_speedCal.curRow = idx + 1;
    m_speedCal.phase = 0; m_speedCal.phaseMs = 0;
    const quint16 s = sendSpeedIdx(m_speedCal.curRow);
    appendLog(QStringLiteral("[Калибровка] цикл %1/%2, ступень %3/%4: задано %5 об/мин")
                  .arg(m_speedCal.cycle+1).arg(m_speedCal.cyclesTotal)
                  .arg(m_speedCal.curRow+1).arg(m_speedCal.targets.size()).arg(s));
}

void MainWindow::speedCalAutoStop(bool finished)
{
    if (!m_speedCal.running) return;
    m_speedCalTimer.stop();
    m_speedCal.running = false;
    { QSignalBlocker b(ui->tblSpeedCal); speedCalClearHighlight(); }   // снять подсветку
    m_dev->enqueueTo(LtpAddr::STEND, LtpCmd::STEND_STOP, {}, TagStend);   // остановить мотор
    ui->btnSpeedCalAuto->setText(QStringLiteral("Калибровать"));
    appendLog(finished ? QStringLiteral("[Калибровка] завершено — проверьте таблицу и «Записать»")
                       : QStringLiteral("[Калибровка] прервано"));
}

void MainWindow::speedCalAutoAccum(const QByteArray &payload)
{
    if (!m_speedCal.running || m_speedCal.phase != 1) return;
    if (payload.size() < 7 || quint8(payload[0]) != 0) return;
    const auto *d = reinterpret_cast<const quint8 *>(payload.constData());
    qint16 gx, gy, gz;
    std::memcpy(&gx, d + 1, 2); std::memcpy(&gy, d + 3, 2); std::memcpy(&gz, d + 5, 2);
    double sens = double(m_mon.gyroSens_mdps);          // mdps/LSB
    if (sens <= 0.0) sens = 70.0;                       // fallback ±2000 dps
    const double dps = std::sqrt(double(gx)*gx + double(gy)*gy + double(gz)*gz) * sens / 1000.0;
    m_speedCal.samples.append(dps / 6.0);               // °/с → об/мин (выборка)
}

// Подсветка текущей ступени калибровки: разгон — янтарный, измерение — зелёный.
// on=false — снять (для мигания). Сигналы таблицы блокируем у вызывающего.
void MainWindow::speedCalHighlightRow(int row, bool on, int phase)
{
    if (row < 0 || row >= ui->tblSpeedCal->rowCount()) return;
    const QBrush br = on ? QBrush(phase == 1 ? QColor(0x2E, 0x7D, 0x32)   // измерение
                                             : QColor(0x9A, 0x7B, 0x1A))  // разгон
                         : QBrush();
    for (int c = 0; c < ui->tblSpeedCal->columnCount(); ++c)
        if (auto *it = ui->tblSpeedCal->item(row, c)) it->setBackground(br);
}

void MainWindow::speedCalClearHighlight()
{
    for (int r = 0; r < ui->tblSpeedCal->rowCount(); ++r)
        for (int c = 0; c < ui->tblSpeedCal->columnCount(); ++c)
            if (auto *it = ui->tblSpeedCal->item(r, c)) it->setBackground(QBrush());
}

/* =====================================================================
 *  ОБНОВЛЕНИЕ ПРОШИВКИ STM32 ПО КАБЕЛЮ (05.08.2026)
 *
 *  Порядок: 0x39 приложению → оно отвечает и ПРЫГАЕТ в загрузчик (без сброса:
 *  загрузчик лежит наверху Flash и вектором сброса не владеет) → короткая
 *  пауза → 0x39 ещё раз (теперь отвечает загрузчик, метка 0xB0) →
 *  0x3A BEGIN (размер+CRC, стирание — ответ ДОЛГИЙ) → 0x3B DATA по 256 Б →
 *  0x3C END (загрузчик сверяет CRC по записанной Flash) → 0x3D GO (сброс,
 *  стартует новое приложение).
 *
 *  Обрыв заливки не страшен: прибор остаётся в загрузчике, повторное «Прошить»
 *  продолжает с нуля. Единственное чувствительное место — первая страница
 *  образа (вектора приложения), поэтому она уходит ПОСЛЕДНЕЙ, см.
 *  fwUpdateSendNext. Договор целиком — Firmware/LOGLSMA/App/Inc/boot.h.
 * ===================================================================== */

/* =====================================================================
 *  ЧТЕНИЕ ВНУТРЕННЕЙ FLASH STM32 (05.08.2026) — диагностика
 *
 *  ⚠ Не путать с «Тестом памяти»: там ВНЕШНИЙ NOR (чип данных, страница
 *  256 Б). Здесь память самого МК — страница 2 КБ, и в ней лежит всё
 *  интересное: код приложения (стр.0..112), секция загрузчика (113..120),
 *  журнал активаций (121), паспорт с калибровками (123), журнал рестартов
 *  (124..127). Раньше посмотреть это можно было только программатором.
 *
 *  Страница приезжает восемью кусками по 256 Б — больше в payload LTP не
 *  влезает. Только чтение: записи по этому пути нет.
 * ===================================================================== */
void MainWindow::iflashReadStart(int page)
{
    if (m_iflash.running) return;
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Внутр. Flash: нет связи")); return; }

    m_iflash.running = true;
    m_iflash.page    = page;
    m_iflash.chunk   = 0;
    m_iflash.buf.clear();
    ui->spinIflashPage->setValue(page);
    ui->lblIflashAddr->setText(QStringLiteral("0x%1 — чтение…")
                                   .arg(0x08000000u + quint32(page) * 2048u, 8, 16, QLatin1Char('0')));
    iflashReadNext();
}

void MainWindow::iflashReadNext()
{
    const quint32 addr = 0x08000000u + quint32(m_iflash.page) * 2048u
                       + quint32(m_iflash.chunk) * 256u;
    const quint16 len  = 256;
    QByteArray p;
    for (int i = 0; i < 4; ++i) p.append(char((addr >> (8 * i)) & 0xFF));
    p.append(char(len & 0xFF));
    p.append(char((len >> 8) & 0xFF));
    requestCmd(LtpCmd::IFLASH_READ, p, TagIflash);
}

void MainWindow::iflashHandle(const QByteArray &payload)
{
    if (!m_iflash.running) return;
    if (payload.isEmpty() || quint8(payload[0]) != 0) {
        m_iflash.running = false;
        ui->lblIflashAddr->setText(QStringLiteral("ошибка чтения"));
        appendLog(QStringLiteral("⚠ Внутр. Flash: прибор отказал (код %1)")
                      .arg(payload.isEmpty() ? -1 : int(quint8(payload[0]))));
        return;
    }
    m_iflash.buf.append(payload.mid(1));

    if (++m_iflash.chunk < 8) { iflashReadNext(); return; }

    m_iflash.running = false;
    iflashRender();
}

void MainWindow::iflashRender()
{
    const quint32 base = 0x08000000u + quint32(m_iflash.page) * 2048u;
    const QByteArray &b = m_iflash.buf;

    // Подпись страницы: сразу видно, на что смотришь, без сверки с картой.
    QString what;
    const int pg = m_iflash.page;
    if (pg <= 112)                     what = QStringLiteral("приложение");
    else if (pg <= 120)                what = QStringLiteral("загрузчик (.bootsec)");
    else if (pg == 121)                what = QStringLiteral("журнал активаций");
    else if (pg == 122)                what = QStringLiteral("свободна");
    else if (pg == 123)                what = QStringLiteral("паспорт + калибровки");
    else                               what = QStringLiteral("журнал рестартов");

    ui->lblIflashAddr->setText(QStringLiteral("0x%1 · стр.%2 · %3")
                                   .arg(base, 8, 16, QLatin1Char('0')).arg(pg).arg(what));

    QString out;
    out.reserve(b.size() * 4);
    for (int off = 0; off < b.size(); off += 16) {
        QString hex, txt;
        for (int i = 0; i < 16 && off + i < b.size(); ++i) {
            const quint8 v = quint8(b[off + i]);
            hex += QStringLiteral("%1 ").arg(v, 2, 16, QLatin1Char('0')).toUpper();
            txt += (v >= 0x20 && v < 0x7F) ? QChar(v) : QChar('.');
            if (i == 7) hex += QLatin1Char(' ');
        }
        out += QStringLiteral("%1  %2 %3\n")
                   .arg(base + quint32(off), 8, 16, QLatin1Char('0'))
                   .arg(hex, -50)
                   .arg(txt);
    }

    // Первое слово секции загрузчика — указатель на точку входа. Проверяем
    // прямо здесь: это ровно то, ради чего окно и делалось.
    if (pg == 113 && b.size() >= 4) {
        const quint32 entry = quint32(quint8(b[0])) | (quint32(quint8(b[1])) << 8)
                            | (quint32(quint8(b[2])) << 16) | (quint32(quint8(b[3])) << 24);
        const bool ok = (entry >= 0x08038800u) && (entry < 0x0803C800u) && (entry & 1u);
        out.prepend(QStringLiteral("Точка входа загрузчика: 0x%1 — %2\n\n")
                        .arg(entry, 8, 16, QLatin1Char('0'))
                        .arg(ok ? QStringLiteral("верна")
                                : QStringLiteral("НЕВЕРНА (загрузчик не прошит?)")));
    }

    ui->iflashDump->setPlainText(out);
}

/* CRC32 из zlib (poly 0xEDB88320, init/xor 0xFFFFFFFF) — ровно то, что считает
 * загрузчик. Побитно: файл — сотня килобайт, таблица ради этого не нужна. */
quint32 MainWindow::fwCrc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (char ch : data) {
        crc ^= quint8(ch);
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & quint32(-qint32(crc & 1u)));
    }
    return ~crc;
}

void MainWindow::fwUpdateStart()
{
    if (m_fw.running) {          // кнопка работает и как «отмена»
        fwUpdateFinish(false, QStringLiteral("Отменено оператором."), m_fw.phase >= 2);
        return;
    }
    if (ui->editFwFile->text().isEmpty()) {
        appendLog(QStringLiteral("⚠ Сначала выберите файл прошивки"));
        return;
    }
    if (!m_link->isOpen()) {
        appendLog(QStringLiteral("⚠ Прошивка: нет связи с прибором"));
        return;
    }

    QFile f(ui->editFwFile->text());
    if (!f.open(QIODevice::ReadOnly)) {
        appendLog(QStringLiteral("⚠ Прошивка: не открывается файл %1").arg(f.fileName()));
        return;
    }
    QByteArray img = f.readAll();
    f.close();

    // Область приложения — стр.8..120 внутренней Flash (см. boot.h)
    constexpr int kAppMax = 113 * 2048;
    if (img.isEmpty() || img.size() > kAppMax) {
        appendLog(QStringLiteral("⚠ Прошивка: неподходящий размер %1 Б (допустимо 1..%2)")
                      .arg(img.size()).arg(kAppMax));
        return;
    }

    // Дешёвая проверка «тот ли это файл» вместо заголовка в образе: первые
    // 8 байт .bin — вектор сброса. Начальный SP обязан смотреть в ОЗУ, адрес
    // Reset_Handler — внутрь области приложения (0x08000000..+226 КБ). Голый
    // мусор и образ от другого МК отсекаются здесь же.
    const auto le32 = [&img](int i) {
        return quint32(quint8(img[i])) | (quint32(quint8(img[i + 1])) << 8)
             | (quint32(quint8(img[i + 2])) << 16) | (quint32(quint8(img[i + 3])) << 24);
    };
    const quint32 sp = le32(0), pc = le32(4);
    if (sp < 0x20000000u || sp > 0x2000C000u) {
        appendLog(QStringLiteral("⚠ Прошивка: не похоже на образ LOGLSMA — начальный стек 0x%1 вне ОЗУ")
                      .arg(sp, 8, 16, QLatin1Char('0')));
        return;
    }
    if (pc < 0x08000000u || pc >= quint32(0x08000000u + kAppMax)) {
        appendLog(QStringLiteral("⚠ Прошивка: вектор сброса 0x%1 вне области приложения "
                                 "(образ слинкован не на 0x08000000?)")
                      .arg(pc, 8, 16, QLatin1Char('0')));
        return;
    }

    // Flash L4 пишется двойными словами — добиваем хвост до кратности 8.
    // 0xFF (а не 0x00): это «стёртое» состояние, дешевле для чипа.
    while (img.size() % 8) img.append(char(0xFF));

    // Кнопки задаём вручную: стандартные Yes/No у Qt остаются английскими,
    // русский перевод в сборку не подключён.
    QMessageBox ask(this);
    ask.setIcon(QMessageBox::Question);
    ask.setWindowTitle(QStringLiteral("Обновление прошивки"));
    ask.setText(QStringLiteral("Залить %1\nв прибор (%2 КБ)?")
                    .arg(QFileInfo(ui->editFwFile->text()).fileName())
                    .arg(img.size() / 1024.0, 0, 'f', 1));
    ask.setInformativeText(QStringLiteral(
        "Приложение отдаст управление загрузчику и на время заливки перестанет "
        "отвечать на обычные команды.\n"
        "Обрыв не опасен — прошивку можно залить заново."));
    QPushButton *btnGo = ask.addButton(QStringLiteral("Прошить"), QMessageBox::AcceptRole);
    ask.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
    ask.setDefaultButton(btnGo);
    ask.exec();
    if (ask.clickedButton() != btnGo)
        return;

    m_fw.running = true;
    m_fw.img     = img;
    m_fw.crc     = fwCrc32(img);
    m_fw.offset  = 0;
    m_fw.phase   = 1;
    m_fw.savedTimeoutMs = ui->spinTimeout->value();

    ui->barFwUpdate->setRange(0, img.size());
    ui->barFwUpdate->setValue(0);
    ui->btnFwUpdate->setText(QStringLiteral("Стоп"));
    ui->lblFwUpdateNote->setText(QStringLiteral("Перевожу прибор в загрузчик…"));
    appendLog(QStringLiteral("Прошивка: %1 Б, CRC32 %2 — перевожу прибор в загрузчик")
                  .arg(img.size()).arg(m_fw.crc, 8, 16, QLatin1Char('0')));

    requestCmd(LtpCmd::BOOT_ENTER, {}, TagFw);
}

/* ⚠ ПОРЯДОК ОТПРАВКИ. Первая страница образа (2 КБ) — таблица векторов
 * работающего приложения. Пока она стёрта, прибор не поднимется после снятия
 * питания, и лечить это пришлось бы по SWD. Поэтому шлём её ПОСЛЕДНЕЙ: сначала
 * всё, что с 2048, затем начало. Загрузчик со своей стороны так же оставляет
 * стр.0 нестёртой до первого пакета с началом образа. Итог: опасное окно —
 * стирание одной страницы плюс восемь пакетов, а не вся заливка. */
void MainWindow::fwUpdateSendNext()
{
    if (!m_fw.running) return;

    constexpr int kPage = 2048;                 // страница внутренней Flash
    const int total = m_fw.img.size();
    const int tailStart = qMin(kPage, total);   // граница «хвост | первая страница»

    int off;
    if (m_fw.offset < total - tailStart) {      // ещё идёт хвост (с 2048 и дальше)
        off = tailStart + m_fw.offset;
    } else if (m_fw.offset < total) {           // хвост кончился — добиваем начало
        off = m_fw.offset - (total - tailStart);
    } else {                                    // всё отдано — на сверку
        m_fw.phase = 5;
        ui->lblFwUpdateNote->setText(QStringLiteral("Сверка CRC в приборе…"));
        requestCmd(LtpCmd::BOOT_END, {}, TagFw);
        return;
    }

    // Хвост может кончиться неполным куском (образ кратен 8, но не 256) —
    // считаем ровно до его границы, иначе следующий кусок «съел» бы начало
    // первой страницы. Отсюда же lastChunk: шагать по 256 вслепую нельзя.
    const int limit = (off >= tailStart) ? total : tailStart;
    const int n = qMin(int(LtpCmd::BOOT_CHUNK), limit - off);

    QByteArray p;
    p.append(char(off & 0xFF));  p.append(char((off >> 8) & 0xFF));
    p.append(char((off >> 16) & 0xFF)); p.append(char((off >> 24) & 0xFF));
    p.append(m_fw.img.mid(off, n));
    m_fw.lastChunk = n;
    requestCmd(LtpCmd::BOOT_DATA, p, TagFw);
}

void MainWindow::fwUpdateHandle(quint8 cmd, const QByteArray &payload)
{
    if (!m_fw.running) return;

    const auto code = [&payload]() -> int {
        return payload.isEmpty() ? -1 : int(quint8(payload[0]));
    };
    // Расшифровка кодов ошибок загрузчика (boot.h). Держать в одном порядке!
    const auto errText = [](int c) {
        switch (c) {
        case 1:  return QStringLiteral("Недопустимый размер образа.");
        case 2:  return QStringLiteral("Ошибка стирания Flash.");
        case 3:  return QStringLiteral("Ошибка записи Flash.");
        case 4:  return QStringLiteral("Нарушен порядок пакетов.");
        case 5:  return QStringLiteral("CRC записанного образа не сошёлся.");
        case 6:  return QStringLiteral("Образ принят не полностью.");
        default: return QStringLiteral("Код ошибки %1.").arg(c);
        }
    };

    switch (cmd) {

    case LtpCmd::BOOT_ENTER:
        // Кто ответил, видно по первому байту: загрузчик ставит метку 0xB0,
        // приложение — код ошибки (0 = принял, уходит прыжком). Поэтому
        // повторный запуск после сорвавшейся заливки (прибор УЖЕ в загрузчике)
        // не ждёт лишнюю паузу, а продолжает сразу.
        if (m_fw.phase == 1 && code() != LtpCmd::BOOT_WHOAMI_MARK) {
            if (code() != 0) {       // приложение отказалось передавать управление
                fwUpdateFinish(false, QStringLiteral(
                    "Прибор не принял команду (код %1). Возможно, версия без "
                    "загрузчика.").arg(code()),
                    false);   // прибор работает штатно, загрузчика в нём нет
                return;
            }
            // Прыжок без сброса: загрузчик поднимает свой клок и UART за
            // единицы миллисекунд. Пауза с запасом, но короткая.
            appendLog(QStringLiteral("Прошивка: приложение отдало управление загрузчику…"));
            m_fwWaitTimer.start(400);
            return;
        }
        if (code() != LtpCmd::BOOT_WHOAMI_MARK) {
            fwUpdateFinish(false,
                QStringLiteral("Ответ пришёл не от загрузчика."), false);
            return;
        }
        if (payload.size() >= 6) {
            appendLog(QStringLiteral("Прошивка: на связи загрузчик, версия %1.%2.%3 %4:%5")
                          .arg(quint8(payload[1]), 2, 10, QLatin1Char('0'))
                          .arg(quint8(payload[2]), 2, 10, QLatin1Char('0'))
                          .arg(quint8(payload[3]), 2, 10, QLatin1Char('0'))
                          .arg(quint8(payload[4]), 2, 10, QLatin1Char('0'))
                          .arg(quint8(payload[5]), 2, 10, QLatin1Char('0')));
        }
        {
            // Стирание области приложения — единственный ДОЛГИЙ ответ во всём
            // обмене (страница ~22 мс, их до сотни). Штатные 500 мс тут дают
            // ложный таймаут, поэтому на время заливки поднимаем окно.
            m_dev->setTimeout(15000);
            m_fw.phase = 3;
            ui->lblFwUpdateNote->setText(QStringLiteral("Стирание области приложения…"));
            const quint32 sz = quint32(m_fw.img.size()), crc = m_fw.crc;
            QByteArray p;
            for (int i = 0; i < 4; ++i) p.append(char((sz  >> (8 * i)) & 0xFF));
            for (int i = 0; i < 4; ++i) p.append(char((crc >> (8 * i)) & 0xFF));
            requestCmd(LtpCmd::BOOT_BEGIN, p, TagFw);
        }
        return;

    case LtpCmd::BOOT_BEGIN:
        if (code() != 0) { fwUpdateFinish(false, errText(code())); return; }
        m_dev->setTimeout(2000);       // дальше ответы быстрые, но с запасом
        m_fw.phase = 4;
        ui->lblFwUpdateNote->setText(QStringLiteral("Заливка образа…"));
        fwUpdateSendNext();
        return;

    case LtpCmd::BOOT_DATA:
        if (code() != 0) { fwUpdateFinish(false, errText(code())); return; }
        m_fw.offset = qMin(m_fw.offset + m_fw.lastChunk, m_fw.img.size());
        ui->barFwUpdate->setValue(m_fw.offset);
        fwUpdateSendNext();
        return;

    case LtpCmd::BOOT_END:
        if (code() != 0) { fwUpdateFinish(false, errText(code())); return; }

        /* Главное уже позади: образ записан и прибор сам сверил CRC. Осталось
         * скомандовать «стартуй» — и вот тут ответа МЫ НЕ ЖДЁМ. Прибор
         * отвечает на GO и в ту же миллисекунду уходит в сброс: успел ответ
         * дойти или нет — вопрос гонки, а не успеха. Раньше из-за этого
         * заливка «зависала» на 100 % с кнопкой «Стоп», хотя всё прошло.
         * Поэтому: закрываем операцию как успешную сразу, а факт запуска
         * проверяем нормальным способом — перечитываем версию прибора. */
        appendLog(QStringLiteral("Прошивка: образ записан и сверен, запускаю"));
        // Реестр (06.08.2026): отмечаем, когда и чем обновляли — по имени файла
        // видна залитая версия (UpLSMA_ГГ.ММ.ДД_ЧЧ.ММ.bin). В серии прошивка
        // одна, и «версия ПО» в index.txt — факт о приборе; журнал нужен для
        // отладочных перешивок, когда версия за день меняется не раз.
        regEvent(ui->editSerial->text().trimmed(), QStringLiteral("прошивка обновлена"),
                 QFileInfo(ui->editFwFile->text()).fileName());
        requestCmd(LtpCmd::BOOT_GO, {}, TagFw);
        fwUpdateFinish(true, QString());
        QTimer::singleShot(2500, this, [this] {
            if (m_link->isOpen()) requestCmd(LtpCmd::WHO_AM_I, {}, TagManual);
        });
        return;

    case LtpCmd::BOOT_GO:
        return;                     /* ответ, если успел прийти, уже не нужен */

    default:
        return;
    }
}

void MainWindow::fwUpdateFinish(bool ok, const QString &why, bool inLoader)
{
    if (!m_fw.running) return;
    m_fw.running = false;
    m_fw.phase   = 0;
    m_fwWaitTimer.stop();
    m_dev->setTimeout(m_fw.savedTimeoutMs);
    m_fw.img.clear();

    ui->btnFwUpdate->setText(QStringLiteral("Прошить"));
    if (ok) {
        ui->barFwUpdate->setValue(ui->barFwUpdate->maximum());
        ui->lblFwUpdateNote->setText(QStringLiteral("Готово — прибор запущен с новой прошивкой."));
        appendLog(QStringLiteral("✅ Прошивка обновлена"));
    } else {
        // Подсказка «нажмите ещё раз» имеет смысл, только если прибор реально
        // сидит в загрузчике; иначе повторное нажатие ничего не изменит.
        // Про кабель не пишем: если с ним беда, это и так видно по индикатору
        // связи (06.08.2026).
        ui->lblFwUpdateNote->setText(QStringLiteral("Обновление не прошло. %1%2").arg(why,
            inLoader
            ? QStringLiteral("\nПрибор остался в загрузчике и ждёт повторной заливки — "
                             "просто нажмите «Прошить» ещё раз.")
            : QString()));
        appendLog(QStringLiteral("⚠ Обновление не прошло: %1").arg(why));
    }
}

// Цвет кнопок грубой калибровки RTC — наглядное состояние процесса:
//   0 idle — всё «прозрачное» (обычный вид), «Применить» недоступно;
//   1 идёт выдержка (после «Старт») — «Старт» ЗЕЛЁНАЯ (активна), «Стоп» КРАСНАЯ (жми дальше);
//   2 расчёт готов (после «Стоп») — обе ЗЕЛЁНЫЕ (данные зафиксированы), «Применить» МОРГАЕТ.
// После «Применить» → снова 0 (всё прозрачное).
void MainWindow::rtcCalUpdateButtons(int state)
{
    static const QString green = QStringLiteral("background:#1D7A4C;color:#F5F4F0;");
    static const QString red   = QStringLiteral("background:#8A2E2E;color:#F5F4F0;");
    m_rtcCalUiState = state;
    m_rtcApplyBlinkTimer.stop();
    ui->btnRtcCalApply->setStyleSheet(QString());
    if (state != 1) {
        ui->btnRtcCalStop->setText(QStringLiteral("Стоп"));  // вне выдержки — обычный текст
        ui->lblRtcCurPpm->setText(QStringLiteral("%1 ppm").arg(m_rtcCurPpm, 0, 'f', 1));  // обычная поправка
    }
    if (state == 1) {
        ui->btnRtcCalStart->setStyleSheet(green);
        ui->btnRtcCalStop ->setStyleSheet(red);
        ui->btnRtcCalApply->setEnabled(false);
    } else if (state == 2) {
        ui->btnRtcCalStart->setStyleSheet(green);
        ui->btnRtcCalStop ->setStyleSheet(green);
        ui->btnRtcCalApply->setEnabled(true);
        m_rtcApplyBlinkOn = false;
        m_rtcApplyBlinkTimer.start(500);   // «Применить» моргает — зовёт нажать
    } else {                                // idle — всё прозрачное
        ui->btnRtcCalStart->setStyleSheet(QString());
        ui->btnRtcCalStop ->setStyleSheet(QString());
        ui->btnRtcCalApply->setEnabled(false);
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
    m_dev->enqueue(LtpCmd::DATA_FLAG, QByteArray(1, char(0)));   // прочитать флаг непрочитанных → покрасить сегмент 0
    m_dev->enqueue(LtpCmd::GET_DATETIME);
    m_dev->enqueue(dashboardTempCmd());
    m_dev->enqueue(LtpCmd::GET_TEMP_STM);   // питание VDD сразу при подключении
    m_dev->enqueue(LtpCmd::GET_STATS);       // наработка, даты циклов, перезапуски
    m_dev->enqueue(LtpCmd::REC_FORMAT,        // формат записи цикла из настроек → в прибор
                   QByteArray(1, char(quint8(ui->cmbRecFormat->currentIndex()))));

    // Сразу при подключении — включить Flash и проверить, что в ней лежит
    // (Регистратор/Logger/пусто), чтобы подпись в заголовке окна не ждала
    // перехода на вкладку «Тест памяти».
    m_dev->enqueue(LtpCmd::FLASH_ON, {}, TagManual);
    probeFlashImageState();
    m_binSearchRetries = 0;   // сброс при каждом новом подключении
    flashBinSearchStart();

    // Если при подключении активна вкладка «Калибровка» — сразу подтянуть таблицу
    // скорости/поправку/опорную точку (currentChanged на старте не срабатывает,
    // связь появляется позже → окно оставалось пустым до смены вкладки).
    if (ui->tabsMain->currentWidget() == ui->tabCalibration) {
        m_dev->enqueue(LtpCmd::SPEED_CAL_GET, {}, TagManual);
        m_dev->enqueue(LtpCmd::RTC_CALIB_GET, {}, TagManual);
        m_dev->enqueue(LtpCmd::SYNC_REF_GET, {}, TagSyncCheck);
    }

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

/* =====================================================================
 *  РАБОЧАЯ ПАПКА (06.08.2026)
 *
 *  Одна папка на все файлы программы: дампы данных прибора, образы,
 *  журналы испытаний, дальше — реестр приборов. До этого каждый путь жил
 *  сам по себе: дампы падали в исходники LOGLSMW, журнал стенда — в
 *  «Документы», образы помнили последнюю папку отдельно. Собрать всё в
 *  одном месте удобнее и на своём компьютере, и в сетевой папке, откуда
 *  данные можно прочитать без нашей программы.
 * ===================================================================== */
QString MainWindow::workDir() const
{
    QSettings st(kOrg, kApp);
    QString dir = st.value(QStringLiteral("workDir")).toString();
    if (dir.isEmpty()) {
        // Пока не выбрана — рядом с документами пользователя: писать в
        // исходники программы неправильно, а «Документы» есть всегда.
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/LogLSM");
    }
    QDir().mkpath(dir);
    return dir;
}

void MainWindow::setupWorkDir()
{
    ui->editWorkDir->setText(QDir::toNativeSeparators(workDir()));
    connect(ui->btnWorkDirBrowse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this,
            QStringLiteral("Рабочая папка — дампы, образы, журналы"), workDir());
        if (dir.isEmpty()) return;
        QSettings(kOrg, kApp).setValue(QStringLiteral("workDir"), dir);
        ui->editWorkDir->setText(QDir::toNativeSeparators(dir));
        appendLog(QStringLiteral("Рабочая папка: %1").arg(QDir::toNativeSeparators(dir)));
    });
}

/* =====================================================================
 *  РЕЕСТР ПРИБОРОВ (06.08.2026) — ТЗ actual/device_registry_spec_v1.md
 *
 *  Учёт, а НЕ резервная копия. Паспорт и калибровки живут в приборе;
 *  здесь только запись «какие приборы выпущены» и «что с ними делали».
 *  Восстановление калибровок сознательно не делаем: прибор, вернувшийся
 *  из эксплуатации, нуждается в новой калибровке, а не в прошлогодней.
 *
 *  Два CSV в рабочей папке. Формат выбран ради того, чтобы данные
 *  читались БЕЗ нашей программы — открыл в Excel и всё видно.
 *  Разделитель «;» и BOM: иначе русский Excel не разберёт ни колонки,
 *  ни кириллицу.
 * ===================================================================== */
// ⚠ Расширение .txt, а РАЗМЕТКА внутри — csv-шная (06.08.2026, по просьбе).
// Двойной щелчок открывает блокнот, а не Excel: на этом этапе таблица не нужна,
// нужен читаемый файл. Разделитель «;» и BOM сохранены — когда понадобится,
// файл открывается Excel'ем через «Открыть с помощью» и раскладывается по
// колонкам без всякой возни.
QString MainWindow::regIndexPath()  const { return workDir() + QStringLiteral("/index.txt"); }
QString MainWindow::regEventsPath() const { return workDir() + QStringLiteral("/events.txt"); }

// Экранирование поля CSV: кавычки удваиваем, всё берём в кавычки, если внутри
// есть разделитель, кавычка или перевод строки.
static QString csvField(const QString &s)
{
    QString v = s;
    v.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (v.contains(QLatin1Char(';')) || v.contains(QLatin1Char('"'))) {
        v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        v = QLatin1Char('"') + v + QLatin1Char('"');
    }
    return v;
}

// Дописать строку в CSV; если файла ещё нет — создать с BOM и заголовком.
static bool csvAppend(const QString &path, const QString &header, const QString &line)
{
    const bool fresh = !QFileInfo::exists(path);
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    if (fresh) {
        ts << QChar(0xFEFF);          // BOM — ради русского Excel
        ts << header << '\n';
    }
    ts << line << '\n';
    f.close();
    return true;
}

// Номер прибора: БУКВА ТИПА + четыре цифры, например A0001 (06.08.2026).
// Голая единица в паспорте выглядела недоразумением, а буква заодно сразу
// говорит, что за прибор. Буква берётся из типа, определённого по датчику.
// Нумерация СКВОЗНАЯ, общая для A и B: номер уникален сам по себе, иначе
// A0001 и B0001 были бы разными приборами с одинаковым числом.
//
// ПЕРВЫЙ СВОБОДНЫЙ номер, а не «максимум + 1» (правка 06.08 по факту с железа).
// Сперва было «максимум + 1», и один старый отладочный номер 22334456 в реестре
// утянул нумерацию за собой: после записи прибора №1 программа предлагала
// 22334457. Один случайный номер портил счётчик навсегда. Теперь занятые
// номера просто пропускаются — мусор в реестре больше не задаёт тон.
int MainWindow::regNextSerial() const
{
    QSet<int> used;
    QFile f(regIndexPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        while (!ts.atEnd()) {
            QString first = ts.readLine().section(QLatin1Char(';'), 0, 0)
                                .remove(QChar(0xFEFF)).remove(QLatin1Char('"')).trimmed();
            // Буква типа спереди — отбрасываем, считаем только цифры: нумерация
            // сквозная, A0007 и B0007 занимают ОДИН и тот же номер.
            while (!first.isEmpty() && !first.at(0).isDigit()) first.remove(0, 1);
            bool ok = false;
            const int n = first.toInt(&ok);      // заголовок и мусор просто не число
            if (ok && n > 0) used.insert(n);
        }
        f.close();
    }
    int n = 1;
    while (used.contains(n)) ++n;
    return n;
}

void MainWindow::regEvent(const QString &serial, const QString &what, const QString &detail)
{
    const QString line = QStringLiteral("%1;%2;%3;%4")
        .arg(csvField(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))),
             csvField(serial), csvField(what), csvField(detail));
    if (!csvAppend(regEventsPath(),
                   QStringLiteral("время;номер;событие;подробности"), line))
        appendLog(QStringLiteral("⚠ Реестр: не удалось записать событие в %1").arg(regEventsPath()));
}

// Строка прибора в index.csv. Если номер уже есть — перезаписываем строку
// (прибор один, сведения о нём должны быть в одном месте), остальные не трогаем.
void MainWindow::regUpsertDevice(const QString &serial, const QString &variant,
                                 const QString &relDate, const QString &fw)
{
    // Колонки: номер (в нём уже есть буква типа), версия ПО, дата записи.
    // «Дата выпуска» убрана (06.08) — она совпадает с днём паспортизации,
    // то есть с датой записи, и держать её отдельно незачем. В самом приборе
    // поле остаётся: там это часть паспорта.
    Q_UNUSED(variant); Q_UNUSED(relDate);
    static const QString header = QStringLiteral("номер;версия;дата записи");
    // Версия — одной цепочкой через точки: 26.08.06.00.30. На экране она с
    // пробелом между датой и временем (так читается лучше), но в файле пробел
    // только мешает — версия становится похожа на два разных поля.
    QString fwOne = fw.simplified();
    fwOne.replace(QLatin1Char(' '), QLatin1Char('.'));
    const QString row = QStringLiteral("%1;%2;%3")
        .arg(csvField(serial), csvField(fwOne),
             csvField(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));

    const QString path = regIndexPath();
    QStringList out;
    bool replaced = false;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        while (!ts.atEnd()) {
            const QString line = ts.readLine();
            if (line.trimmed().isEmpty()) continue;
            const QString key = line.section(QLatin1Char(';'), 0, 0)
                                    .remove(QChar(0xFEFF)).remove(QLatin1Char('"')).trimmed();
            if (key == serial) { out << row; replaced = true; }
            else               { out << line; }
        }
        f.close();
    }
    if (!replaced) {
        if (out.isEmpty()) out << header;
        out << row;
    }

    // Пишем целиком через временный файл: так недописанный index.csv не
    // затрёт прежний, если программа упадёт на середине.
    const QString tmp = path + QStringLiteral(".tmp");
    QFile t(tmp);
    if (!t.open(QIODevice::WriteOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("⚠ Реестр: не удалось записать %1").arg(path));
        return;
    }
    { QTextStream ts(&t); ts.setEncoding(QStringConverter::Utf8);
      ts << QChar(0xFEFF);
      for (const QString &l : out) ts << l << '\n'; }
    t.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path))
        appendLog(QStringLiteral("⚠ Реестр: не удалось заменить %1").arg(path));
}

void MainWindow::setupRegistry()
{
    // «Следующий №» — номер из реестра плюс сегодняшняя дата. Оператору
    // остаётся проверить вариант и нажать «Записать».
    connect(ui->btnSerialNext, &QPushButton::clicked, this, [this] {
        const int n = regNextSerial();
        ui->editSerial->setText(QStringLiteral("%1%2")
            .arg(m_variantCode == 0x0B ? QLatin1Char('B') : QLatin1Char('A'))
            .arg(n, 4, 10, QLatin1Char('0')));
        appendLog(QStringLiteral("Реестр: следующий свободный номер %1")
                      .arg(ui->editSerial->text()));
    });
    // «Реестр…» открывает сам список приборов; если его ещё нет (ни одного
    // прибора не записано) — открываем папку, чтобы было видно, куда смотреть.
    connect(ui->btnRegistryOpen, &QPushButton::clicked, this, [this] {
        const QString idx = regIndexPath();
        if (QFileInfo::exists(idx)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(idx));
        } else {
            appendLog(QStringLiteral("Реестр пуст — файл %1 появится после первой записи паспорта")
                          .arg(QDir::toNativeSeparators(idx)));
            QDesktopServices::openUrl(QUrl::fromLocalFile(workDir()));
        }
    });
}

// Индикатор связи, три состояния — см. объявление в mainwindow.h.
// Индикатор — вся кнопка целиком (точка плохо видна).
void MainWindow::setLinkLed(int state)
{
    struct { const char *bg, *br, *tip; } s[] = {
        { "#C03030", "#9A2626", "Нет связи" },
        { "#B8860B", "#8A6508", "Порт открыт, но регистратор не отвечает "
                                "(отозвался только стенд)" },
        { "#1D7A4C", "#16613C", "Регистратор на связи" },
    };
    const int i = qBound(0, state, 2);
    ui->btnInd->setStyleSheet(QStringLiteral(
        "QPushButton#btnInd { background: %1; border: 1px solid %2;"
        " border-radius: 12px; color: rgba(255,255,255,150); font-size: 12px; }")
        .arg(QLatin1String(s[i].bg), QLatin1String(s[i].br)));
    ui->btnInd->setToolTip(QString::fromUtf8(s[i].tip));
}

void MainWindow::setConnectedUi(bool on, const QString &port)
{
    // Порт открылся — это ещё не «связь с регистратором»: на линии может
    // сидеть один стенд. Зелёным индикатор станет в onResponse, когда ответит
    // сам регистратор.
    // ⚠ Признак сбрасывается и при ПОДКЛЮЧЕНИИ тоже: порт новый, кто там на
    // линии — пока неизвестно. Иначе после переподключения индикатор зеленел
    // сразу, помня ответ от прошлого раза (замечено на железе 06.08).
    m_regSeen = false;
    setLinkLed(on ? 1 : 0);

    ui->btnScan->setText(on ? port : QStringLiteral("ВКЛ"));
    // Широкое поле справа от кнопки порта — под СВОДКУ по архиву (наработка и
    // максимумы скорости/удара/температуры, см. «Данные»). Надпись
    // «LOGLSM-регистратор» её затирала при каждом подключении и ничего не
    // сообщала: имя прибора и так в заголовке окна. Оставляем поле пустым
    // (06.08.2026).
    if (on) ui->lblDevCard->clear();
    else    ui->lblDevCard->setText(QStringLiteral("LogLSMW · сервис регистратора"));
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
    // Любой ответ НЕ от стенда — это отозвался регистратор: индикатор зеленеет
    // (06.08.2026). До этого он показывал лишь «порт открыт», и подключённый
    // стенд один давал зелёный свет при молчащем регистраторе.
    if (tag != TagStend && !m_regSeen) {
        m_regSeen = true;
        setLinkLed(2);
    }

    // Ответы стенда (0x8C) разбираются ДО общего switch(cmd) ниже — коды
    // STEND_START/SPEED/STOP (0x04/0x05/0x06) численно совпадают с
    // FLASH_READ_ID/FLASH_ERASE/FLASH_PAGE_ERASE регистратора, иначе ответ
    // стенда попал бы в обработчик флеш-команд (см. devicecontroller.h).
    if (tag == TagStend) {
        stendHandleResponse(cmd, payload);
        return;
    }

    // Заливка прошивки (05.08.2026) — ДО общего switch: коды 0x39..0x3D
    // приложение не знает, отвечает на них загрузчик, и разбирать их вместе
    // с обычными командами незачем.
    if (tag == TagFw) {
        fwUpdateHandle(cmd, payload);
        return;
    }

    // Чтение внутренней Flash (0x3E) — свой разбор, до общего switch.
    if (tag == TagIflash) {
        if (cmd == LtpCmd::IFLASH_READ) iflashHandle(payload);
        return;
    }

    // Постраничное сохранение образа в шаге 1 активации (27.07.2026). Читаем
    // страницы подряд, пока не встретим полностью пустую (0xFF — конец лога:
    // журнал непрерывен) или не упрёмся в конец чипа. Затем пишем единый Intel
    // HEX и продолжаем волну. Сырые байты — формат один для Регистратора и
    // Логгера (декодирует потом отдельный формирователь отчётов).
    if (tag == TagActDump) {
        if (cmd != LtpCmd::FLASH_READ) return;   // ждём только страницы
        const auto *pd = reinterpret_cast<const quint8 *>(payload.constData());
        if (payload.size() < 2 || pd[0] != 0) {
            activationFail(QStringLiteral("сохранение образа: ошибка чтения страницы"));
            return;
        }
        QByteArray page = payload.mid(1);
        if (page.size() < 256) page.append(QByteArray(256 - page.size(), char(0xFF)));
        else if (page.size() > 256) page.truncate(256);
        bool allFF = true;
        for (char c : page) if (quint8(c) != 0xFF) { allFF = false; break; }
        const bool finish = allFF || (int(m_actDump.page) + 1 >= int(m_actDump.pageEnd));
        if (!allFF) m_actDump.buf.append(page);
        if (!finish) {
            ++m_actDump.page;
            const quint32 addr = quint32(m_actDump.page) << 8;
            QByteArray p;
            for (int j = 0; j < 4; ++j) p.append(char((addr >> (8*j)) & 0xFF));
            for (int j = 0; j < 4; ++j) p.append(char((256  >> (8*j)) & 0xFF));
            m_dev->enqueue(LtpCmd::FLASH_READ, p, TagActDump);
            ui->lblActStatus->setText(QStringLiteral("Шаг 1: сохранение образа… стр.%1")
                .arg(int(m_actDump.page) - int(m_actDump.startPage) + 1));
            return;
        }
        m_actDump.running = false;
        const bool wave = m_actDump.continueWave;
        // Пустой образ (все страницы 0xFF): данных нет — писать нечего, но флаг
        // всё равно сбрасываем (нечего терять) и, если в волне, идём дальше.
        if (m_actDump.buf.isEmpty()) {
            appendLog(wave ? QStringLiteral("[ACT] Шаг 1: данных нет — сохранять нечего")
                           : QStringLiteral("[Сохранить] данных нет — сохранять нечего"));
            requestCmd(LtpCmd::DATA_FLAG, QByteArray(1, char(wave ? 2 : 3)), TagManual);
            if (wave) activationBeginStep(ActStep::Check);
            else      requestCmd(LtpCmd::GET_STATS, {}, TagManual);   // обновить «Активация»
            return;
        }
        QString err;
        if (!saveImageToHexFile(m_actDump.path, m_actDump.startPage, m_actDump.buf, err)) {
            if (wave) activationFail(QStringLiteral("сохранение образа: %1").arg(err));
            else      appendLog(QStringLiteral("[Сохранить] ошибка записи файла: %1").arg(err));
            return;
        }
        appendLog(QStringLiteral("[%1] образ сохранён (%2 стр.): %3")
            .arg(wave ? QStringLiteral("ACT") : QStringLiteral("Сохранить"))
            .arg(m_actDump.buf.size() / 256).arg(m_actDump.path));
        // Данные на диске → СОХРАНЕНИЕ РЕЗУЛЬТАТА закрывает «жизнь»: 0x30=3 С ts
        // (прошивка пишет END в стр.121 ТОЛЬКО при наличии ts — раньше слали без
        // ts, и END не писался → история копила одни «начала»). И в волне (при
        // активации сохраняем прошлые данные → END старой жизни, START новой даст
        // шаг 7), и в standalone. 03.08.2026.
        {   QByteArray p; p.append(char(3));
            const quint32 nowTs = quint32(QDateTime::currentSecsSinceEpoch());
            for (int j = 0; j < 4; ++j) p.append(char((nowTs >> (8*j)) & 0xFF));
            requestCmd(LtpCmd::DATA_FLAG, p, TagManual); }
        if (wave) activationBeginStep(ActStep::Check);
        else      requestCmd(LtpCmd::GET_STATS, {}, TagManual);   // обновить «Активация»
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
            // ВАРИАНТ ПРИБОРА — ОПРЕДЕЛЯЕТСЯ САМ, по WHO_AM_I датчика
            // (06.08.2026): 0x6C = LSM6DSO = A, 0x70 = LSM6DSV320X = B. Это
            // железный признак, ошибиться нельзя, поэтому руками его больше не
            // выбирают: поле оставлено как показ, но заблокировано. Раньше
            // оператор мог записать в паспорт вариант, которого в приборе нет.
            // Тип прибора запоминаем, но отдельного поля для него больше нет
            // (06.08.2026): буква типа и так стоит в начале номера — A0001,
            // а сам датчик виден в «Данные» → WHO_AM_I. Код нужен для буквы
            // номера и для байта варианта в паспорте.
            if (id == 0x6C || id == 0x70) {
                m_variantCode       = (id == 0x70) ? 0x0B : 0x0A;
                m_variantFromSensor = true;
            }
            // График 2 «пики» = вторая шкала того же vib1 — виден на ОБОИХ
            // вариантах (19.07.2026, пересмотр: раньше прятали на A). На B здесь
            // естественно лягут пики физического high-g акселерометра.
            // ВЕРСИЯ ПРОШИВКИ РЕГИСТРАТОРА (байты 1-2 LE, если прошивка их шлёт —
            // 13.07.2026). Индивидуальный параметр устройства: меняешь FW_VERSION
            // в com.c → перешил → тут сразу новое число = прошивка обновилась.
            if (payload.size() >= 6) {
                // Версия = ВРЕМЯ СБОРКИ прошивки: [1]=ГГ [2]=ММ [3]=ДД [4]=ЧЧ [5]=ММ.
                // Отображаем ГОД-ПЕРВЫМ: ГГ.ММ.ДД  ЧЧ.ММ — так строка сортируется
                // хронологически (28.07.2026: год-первым верно, откат дневного порядка).
                const int yy = d[1], mm = d[2], dd = d[3], hh = d[4], mi = d[5];
                const QString ver = QStringLiteral("%1.%2.%3  %4.%5")
                    .arg(yy,2,10,QLatin1Char('0')).arg(mm,2,10,QLatin1Char('0'))
                    .arg(dd,2,10,QLatin1Char('0')).arg(hh,2,10,QLatin1Char('0'))
                    .arg(mi,2,10,QLatin1Char('0'));
                ui->lblFwVersion->setText(ver);          // индикатор на «Данные»
                ui->editFwVer->setText(ver);             // и рядом с номером в паспорте
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
                ui->editFwVer->setText(QStringLiteral("нет"));
                appendLog(QStringLiteral("[RX] прошивка не сообщает версию (старая — без поля версии)"));
            }
            // ОБЕ версии — в ЗАГОЛОВКЕ окна, в одну строку: приложение (W) и
            // прошивка (A). Нижнюю панель версии убрали; карточка сверху пустая.
            setWindowTitle(QStringLiteral("LogLSMW %1      ·      %2  %3")
                .arg(qApp->applicationVersion(),
                     ui->lblDevName->text().trimmed(),
                     ui->lblFwVersion->text()));
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

        // Флаг истины по САМИМ ЧАСАМ: если часы устройства сбросились в 2000 (было
        // включение питания), опорная точка калибровки недействительна — сразу
        // деактивируем кнопки (работает и без пересборки A, по обычному GET_DATETIME).
        if (devDt.date().year() < 2020 && m_rtcCalUiState != 0
            && tag != TagRtcCalStop) {
            m_rtcCalActive = false;
            QSettings(kOrg, kApp).remove(QStringLiteral("rtcCal"));
            rtcCalUpdateButtons(0);
            ui->lblRtcCalStatus->setText(QStringLiteral("Часы сбились в 2000 (было включение питания) — опорная точка потеряна. Синхронизируйте заново."));
        }

        // Живой предпросмотр поправки во время выдержки: ЗЕЛЁНАЯ — текущая (в работе),
        // КРАСНАЯ — рассчитанная по накопленному интервалу (сходится с ростом выдержки).
        if (m_rtcCalUiState == 1 && m_rtcCalT0.isValid() && devDt.date().year() >= 2020) {
            const double elPc = double(m_rtcCalT0.secsTo(QDateTime::currentDateTime()));
            if (elPc >= 1.0) {
                const double elDev = double(m_rtcCalT0.secsTo(devDt));
                const double resid = (elDev - elPc) / elPc * 1e6;
                double calc = m_rtcCurPpm - resid;
                if (calc >  488.0) calc =  488.0;
                if (calc < -488.0) calc = -488.0;
                ui->lblRtcCurPpm->setText(QStringLiteral(
                    "<span style='color:#3CB371'>%1</span> &rarr; <span style='color:#D05555'>%2</span> ppm")
                    .arg(m_rtcCurPpm, 0, 'f', 1).arg(calc, 0, 'f', 1));
            }
        }

        // «Стоп» грубой калибровки RTC (03.08.2026): дрейф по интервалу выдержки.
        if (tag == TagRtcCalStop && m_rtcCalActive) {
            m_rtcCalActive = false;
            const double elPc  = double(m_rtcCalT0.secsTo(QDateTime::currentDateTime()));
            const double elDev = double(m_rtcCalT0.secsTo(devDt));
            const double resid = (elPc >= 1.0) ? (elDev - elPc) / elPc * 1e6 : 0.0;
            const double newPpm = m_rtcCurPpm - resid;
            if (devDt.date().year() < 2020) {
                // Часы устройства сбились (2000) во время выдержки — интервал битый.
                rtcCalUpdateButtons(0);
                QSettings(kOrg, kApp).remove(QStringLiteral("rtcCal"));
                ui->lblRtcCalStatus->setText(QStringLiteral("Часы устройства сбились — расчёт невозможен. Синхронизируйте и начните заново."));
            } else if (elPc < 1.0) {
                ui->lblRtcCalStatus->setText(QStringLiteral("Слишком короткая выдержка — повторите с большим интервалом"));
            } else if (std::fabs(newPpm) > 488.0) {
                // Нефизичный результат (RTC умеет ±488 ppm) — интервал/данные битые.
                rtcCalUpdateButtons(0);
                ui->lblRtcCalStatus->setText(QStringLiteral("Нефизичный дрейф %1 ppm — расчёт отклонён. Проверьте синхронизацию и выдержку.")
                    .arg(resid, 0, 'f', 0));
            } else {
                m_rtcCalNewPpm = newPpm;
                ui->lblRtcCalStatus->setText(QStringLiteral("Выдержка %1 c: дрейф %2 ppm → новая поправка %3 ppm")
                    .arg(qint64(elPc)).arg(resid, 0, 'f', 1).arg(m_rtcCalNewPpm, 0, 'f', 1));
                rtcCalUpdateButtons(2);   // «Стоп» зелёная, «Применить» доступно (моргает)
                QSettings(kOrg, kApp).remove(QStringLiteral("rtcCal"));   // выдержка закрыта
                appendLog(QStringLiteral("[RTC-калибровка] выдержка %1 c, дрейф %2 ppm, новая поправка %3 ppm")
                    .arg(qint64(elPc)).arg(resid, 0, 'f', 1).arg(m_rtcCalNewPpm, 0, 'f', 1));
            }
        }

        // Лог только при ручном запросе (btnGetTime / контрольное чтение после btnSyncTime)
        if (tag == TagSyncTime)
            appendLog(QStringLiteral("[RX] GET_DATETIME → %1")
                .arg(devDt.toString(QStringLiteral("HH:mm:ss dd/MM/yyyy"))));

        // ТЗ v2 §2.4: сравнение по ПОЛНОЙ дате-времени, допуск 1 с,
        // порог аварии — из настроек (мин). Фоновый 1 Гц — не логируем.
        const qint64 diff  = QDateTime::currentDateTime().secsTo(devDt); // + спешит
        const qint64 ad    = qAbs(diff);
        const qint64 alarm = qint64(ui->spinTimeAlarm->value()) * 60;

        // Панель «Часы RTC» на «Калибровке» (03.08.2026).
        ui->lblRtcDevTime->setText(devDt.toString(QStringLiteral("HH:mm:ss  dd.MM.yyyy")));
        ui->lblRtcPcTime->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  dd.MM.yyyy")));
        ui->lblRtcDrift->setText(!devDt.isValid()
            ? QStringLiteral("часы недостоверны")
            : QStringLiteral("%1%2 с").arg(diff > 0 ? QStringLiteral("+") : QString()).arg(diff));

        // «Синхро время» = сегмент 2. Резолвим ТОЛЬКО на КОНТРОЛЬНОМ чтении после
        // SET (tag==TagSyncTime) — иначе фоновый GET_DATETIME (~1 Гц) сбросил бы
        // pending до-синхронно и показал бы старое расхождение. (02.08.2026)
        if (m_actSyncPending && tag == TagSyncTime) {
            m_actSyncPending = false;
            activationSetSectorMinDelay(2, (ad <= 1)
                ? ActivationBar::SectorState::Active
                : ActivationBar::SectorState::Error, m_actSyncActiveMs);
        } else if (!m_actSyncPending) {
            // ЖИВОЙ индикатор «Синхро время» (сегмент 2), как WDT: сразу по
            // состоянию, в ОБОИХ режимах — часы в допуске → жёлтый, разошлись/
            // недостоверны → красный. КРИТИЧНО: это же снимает Disabled → сегмент
            // становится кликабельным (ActivationBar::mousePressEvent игнорирует
            // Disabled; из-за этого «Синхро» и не нажималась). Вне активной волны.
            // Клик в операторе блокирован гейтом. Зелёный — только из волны. (02.08.2026)
            const bool waveActive = (m_act.step != ActStep::Idle
                                  && m_act.step != ActStep::Done
                                  && m_act.step != ActStep::Error);
            if (!waveActive) {
                const bool okSync = (devDt.isValid() && ad <= alarm);
                if (!okSync)
                    activationSetSector(2, ActivationBar::SectorState::Idle);   // красный — рассинхрон (всегда)
                else if (m_act.step != ActStep::Done)
                    activationSetSector(2, ActivationBar::SectorState::Active); // жёлтый — синхронно (вне пост-волны)
                // okSync && Done → НЕ трогаем: держим ЗЕЛЁНЫЙ от завершённой волны (03.08.2026)
            }
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
        if (tag == TagSpeedCal)               // авто-калибровка: накопить измеренную об/мин
            speedCalAutoAccum(payload);
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
                ui->lblTempCurCaption->setText(QStringLiteral("текущая"));
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
            } else {
                // m_eraseStartMs уже записан в обработчике кнопки (до отправки команды)
                ui->memReport->appendPlainText(QStringLiteral("Стирание запущено, ожидание…"));
                requestCmd(LtpCmd::FLASH_STATE, {}, TagManual);
            }
        } else {
            if (tag == TagAct) {
                activationFail(QStringLiteral("FLASH_ERASE: ошибка"));
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
                    appendLog(QStringLiteral("[ACT] Шаг 5: чип стёрт"));
                    activationSetSector(4, ActivationBar::SectorState::Done);
                    activationBeginStep(ActStep::TestWrite);
                } else {
                    setOpsEnabled(true);
                    // Время стирания на этом чипе не измеряется достоверно
                    // (рапортует ~0) — не показываем «за N с», просто факт.
                    ui->memReport->appendPlainText(QStringLiteral("Чип стёрт"));
                    // Данных больше нет → перечитать флаг (прошивка A≥13:06 сбросила
                    // его при стирании) и обновить сегмент «Сохранить»→«Сохранено».
                    requestCmd(LtpCmd::DATA_FLAG, QByteArray(1, char(0)), TagManual);
                    // Наработка (totalSec) обнуляется прошивкой при стирании чипа
                    // (com.c) — перечитываем GET_STATS, иначе карточка «работа»
                    // висит со старым (легаси-раздутым) значением. 03.08.2026.
                    requestCmd(LtpCmd::GET_STATS, {}, TagManual);
                    // (Одиночное стирание — из «Тест памяти»; ленту активации не
                    // трогаем: «Стереть» теперь шаг 4 волны, а не сегмент 0.)
                    // Содержимое Flash изменилось — вкладка «Данные» до сих пор
                    // показывает результат предыдущего разбора (m_arc хранит его,
                    // ничего не сбрасывает). Перечитываем журнал заново, чтобы
                    // она отражала реальное (теперь пустое) состояние памяти.
                    archiveRescanFull();
                    // Чип стёрт целиком — Flash точно пуста, кнопки «Образ RG/LOG»
                    // подсветку снимаем сразу, без отдельного запроса.
                    m_flashImageState = FlashImageState::Empty;
                    refreshImgButtonsHighlight();
                    // Чип полностью стёрт — ни одной непустой страницы,
                    // первая свободная = внутренняя стр.0 → «Занято» = 0
                    // (26.07.2026: занято = число непустых страниц от начала).
                    m_firstFreePage = 0;
                    // «Занято»/«Адрес» (22.07.2026) — этот путь идёт в обход
                    // обработчика бинарного поиска (flashBinSearchSendNext), где
                    // обычно обновляются эти поля — обновляем и здесь напрямую,
                    // иначе после полного стирания там оставались старые значения.
                    updateOccupiedLabel();
                    ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(m_firstFreePage) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
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
                        && !m_stendActive && !m_test.running) {
                        m_silentDump = true;
                        memTestDump();
                    }
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
    case LtpCmd::DATA_FLAG: {
        // 03.08.2026: ответ 0x30 стал БИТОВОЙ МАСКОЙ (NOR стр.0 флаги, было булево):
        // [0]=err, [1]=маска: 0x01 активирован (стр.121), 0x02 данные_есть, 0x04 сохранено.
        if (payload.size() < 2) break;
        const quint8 mask = quint8(payload.at(1));
        m_deviceActivated = (mask & 0x01u);   // осн. признак — NOR-байт [1] стр.0
        m_dataPresent = (mask & 0x02u);
        m_dataSaved   = (mask & 0x04u);
        // «Несохранённые данные» = данные в NOR есть И сохранение не выполнено.
        m_dataFlagSet = m_dataPresent && !m_dataSaved;
        // Красный «Сохранить» / зелёный «Сохранено» / жёлтый «Стёрто» (если Flash
        // пуста) — единая логика в updateSaveSegment (флаг + занятость памяти).
        updateSaveSegment();
        updateActivationState();   // «между жизнями» ↔ «идёт жизнь» зависит от флага
        break;
    }
    case LtpCmd::RTC_CALIB_GET: {
        // 0x33: [0]=err, [1..4]=ppm float(LE) — текущая применённая поправка RTC.
        if (payload.size() < 5 || d[0] != 0) break;
        float ppm; std::memcpy(&ppm, d + 1, 4);
        m_rtcCurPpm = double(ppm);
        if (m_rtcCalUiState != 1)   // в выдержке строку ведёт живой апдейт (зел/красн)
            ui->lblRtcCurPpm->setText(QStringLiteral("%1 ppm").arg(double(ppm), 0, 'f', 1));
        break;
    }
    case LtpCmd::SYNC_REF_GET: {
        // 0x38: [0]=err, [1]=valid, [2..5]=t0 u32(LE).
        const bool devValid = (payload.size() >= 6 && d[0] == 0 && d[1] != 0);
        if (tag == TagSyncCheck) {
            // Проверка при подключении/открытии вкладки — устройство = источник
            // истины. Валидно → выдержка идёт (кнопки в состояние 1). Невалидно
            // (backup сброшен = было включение питания, часы сбились) → всё в
            // исходное + стираем устаревший флаг в настройках ПК.
            if (devValid) {
                // Точка на устройстве цела — просто держим t0 актуальным, кнопки
                // НЕ трогаем (проверка не «включает» выдержку сама; её начинает
                // «Старт»/синхро, а состояние кнопок ведёт PC-restore + нажатия).
                quint32 ts; std::memcpy(&ts, d + 2, 4);
                m_rtcCalT0 = QDateTime::fromSecsSinceEpoch(qint64(ts));
                m_rtcCalActive = true;
            } else if (m_rtcCalUiState != 0) {
                // Точки нет (backup сброшен = было включение питания, часы сбились)
                // → ДЕАКТИВИРУЕМ: всё в исходное + стираем устаревший флаг ПК.
                m_rtcCalActive = false;
                QSettings(kOrg, kApp).remove(QStringLiteral("rtcCal"));
                rtcCalUpdateButtons(0);
                ui->lblRtcCalStatus->setText(QStringLiteral("Опорная точка потеряна (было включение питания / часы сбились). Синхронизируйте заново."));
            }
            break;
        }
        if (tag != TagRtcCalStop) break;
        // Ответ на «Стоп»: валидно — t0 с устройства; иначе — откат на сессию/
        // настройки ПК; нет и их — отказ (backup сброшен = часы сбились).
        if (devValid) {
            quint32 ts; std::memcpy(&ts, d + 2, 4);
            m_rtcCalT0 = QDateTime::fromSecsSinceEpoch(qint64(ts));
            m_rtcCalActive = true;
            appendLog(QStringLiteral("[RTC-калибровка] опорная точка с устройства: %1")
                .arg(m_rtcCalT0.toString(QStringLiteral("yy.MM.dd HH:mm:ss"))));
        } else if (!m_rtcCalActive) {
            ui->lblRtcCalStatus->setText(QStringLiteral("Опорная точка потеряна (питание/часы сбились) — нажмите «Старт» или синхронизируйте"));
            appendLog(QStringLiteral("⚠ [RTC-калибровка] нет валидной опорной точки — расчёт отменён"));
            break;
        } else {
            appendLog(QStringLiteral("[RTC-калибровка] устройство без опорной точки — использую t0 сессии"));
        }
        requestCmd(LtpCmd::RTC_CALIB_GET, {}, TagManual);      // свежая текущая поправка
        requestCmd(LtpCmd::GET_DATETIME, {}, TagRtcCalStop);   // → расчёт (обработчик GET_DATETIME)
        break;
    }
    case LtpCmd::SPEED_CAL_GET: {
        // 0x2F: [0]=err, [1]=n, далее n×[ r float(LE) | k float(LE) ].
        if (payload.size() < 2 || d[0] != 0) break;
        const int n = quint8(d[1]);
        ui->tblSpeedCal->setRowCount(0);
        for (int i = 0; i < n && (2 + i*8 + 8) <= payload.size(); ++i) {
            float r, k;
            std::memcpy(&r, d + 2 + i*8,     4);
            std::memcpy(&k, d + 2 + i*8 + 4, 4);
            ui->tblSpeedCal->insertRow(i);
            // с устройства: r = СЫРАЯ об/мин (ключ), k = текущий коэфф.
            // Измерено = сырое×k; задано = сырое×k (Δ=0, «новый»=k — согласованно).
            speedCalSetRow(i, double(r) * double(k), double(r) * double(k), double(k));
        }
        break;
    }
    case LtpCmd::PASSPORT_GET: {
        // 0x35: [0]=err, [1]=valid, [2..17]=serial(16), [18]=variant,
        // [19..20]=year u16(LE), [21]=month, [22]=day.
        if (payload.size() < 23 || d[0] != 0) break;
        const bool valid = (d[1] != 0);
        m_passportPresent = valid;      // для гейта повторной паспортизации
        if (!valid) {
            appendLog(QStringLiteral("[RX] Паспорт не задан (стр.123)"));
            ui->editSerial->setText(QString());
            break;
        }
        char ser[17];
        std::memcpy(ser, d + 2, 16);
        ser[16] = '\0';                                     // серийник — C-строка до '\0'
        ui->editSerial->setText(QString::fromLatin1(ser));
        ui->editSerial->setCursorPosition(0);   // иначе виден хвост длинного номера
        // ⚠ Тип берём с ДАТЧИКА, а не из паспорта: живой WHO_AM_I вернее
        // записанного когда-то. Из паспорта показываем, только пока датчик не
        // опознан; расхождение — повод перезаписать паспорт.
        if (m_variantFromSensor && quint8(d[18]) != m_variantCode)
            appendLog(QStringLiteral("⚠ Паспорт: записанный тип не совпадает с датчиком — "
                                     "перезапишите паспорт"));
        quint16 y; std::memcpy(&y, d + 19, 2);
        const int mo = quint8(d[21]), da = quint8(d[22]);
        const QDate date(y, mo, da);
        // Дату показывать негде (поле убрано) — она уходит в журнал.
        appendLog(QStringLiteral("[RX] Паспорт: %1, дата %2")
                      .arg(ui->editSerial->text(),
                           (y == 0) ? QStringLiteral("не задана")
                           : date.isValid() ? date.toString(QStringLiteral("yyyy-MM-dd"))
                           : QStringLiteral("%1-%2-%3").arg(y,4,10,QLatin1Char('0'))
                                 .arg(mo,2,10,QLatin1Char('0')).arg(da,2,10,QLatin1Char('0'))));
        break;
    }
    case LtpCmd::PASSPORT_SET:
        if (d && d[0] == 0) {
            appendLog(QStringLiteral("[RX] Паспорт записан (стр.123)"));
            // Реестр приборов (06.08.2026): пишем ТОЛЬКО когда прибор
            // подтвердил запись — иначе в списке появлялись бы приборы,
            // которых нет.
            const QString ser = ui->editSerial->text().trimmed();
            if (!ser.isEmpty()) {
                regUpsertDevice(ser,
                    m_variantCode == 0x0B ? QStringLiteral("B") : QStringLiteral("A"),
                    QString(),                      // дата выпуска = дата записи
                    ui->lblFwVersion->text().trimmed());
                regEvent(ser, QStringLiteral("паспорт записан"),
                         QStringLiteral("тип %1")
                             .arg(m_variantCode == 0x0B ? QStringLiteral("B") : QStringLiteral("A")));
                appendLog(QStringLiteral("Реестр: прибор %1 записан").arg(ser));
            }
        } else {
            appendLog(QStringLiteral("[RX] Паспорт: ошибка записи"));
        }
        break;
    case LtpCmd::ACT_HISTORY: {
        // 0x2E: [0]=err, [1..2]=count u16 событий, далее count×[type u8 | ts u32].
        // type 0xF3=начало жизни, 0xF4=конец. Разбиваем на строки «начало → конец»
        // (03.08.2026). «Активаций» = число начал. Панель истории на «FLASH STM».
        if (payload.size() < 3 || d[0] != 0) break;
        quint16 cnt; std::memcpy(&cnt, d + 1, 2);   // число событий; событие = 7 байт
        QStringList lines;
        int lives = 0;
        QString openStart;      // начало текущей ещё не закрытой жизни
        auto fmt = [](quint32 ts) {
            return QDateTime::fromSecsSinceEpoch(qint64(ts))
                       .toString(QStringLiteral("yy.MM.dd HH:mm")); };
        for (int i = 0; i < int(cnt) && (3 + i*7 + 7) <= payload.size(); ++i) {
            const quint8 type = quint8(d[3 + i*7]);
            quint32 ts;  std::memcpy(&ts,  d + 3 + i*7 + 1, 4);
            quint16 rst; std::memcpy(&rst, d + 3 + i*7 + 5, 2);   // [6]=таймер [7]=питание
            // Моноширинный вывод (Consolas): столбцы фиксированной ширины —
            // цифры под цифрами. Метка «таймер»/«питание» дополнена до 7 знаков,
            // счётчик выровнен по правому краю (ширина 3).
            if (type == 0xF3u) {                     // начало
                if (!openStart.isEmpty())            // прошлая осталась открытой
                    lines << QStringLiteral("%1. %2   (идёт)").arg(lives, 2).arg(openStart);
                ++lives;
                openStart = fmt(ts);
            } else if (type == 0xF4u) {              // конец → запись в 2 строки:
                const int rt = rst & 0xFFu, rp = (rst >> 8) & 0xFFu;   // таймер, питание
                //  строка 1: N.  <старт>       таймер <кол-во>
                //  строка 2:     <окончание>   питание <кол-во>
                lines << QStringLiteral("%1. %2   %3 %4").arg(lives, 2)
                             .arg(openStart.isEmpty() ? QStringLiteral("?") : openStart)
                             .arg(QStringLiteral("таймер").leftJustified(7)).arg(rt, 3);
                lines << QStringLiteral("    %1   %2 %3").arg(fmt(ts))
                             .arg(QStringLiteral("питание").leftJustified(7)).arg(rp, 3);
                openStart.clear();
            }
        }
        if (!openStart.isEmpty())                    // последняя жизнь ещё идёт
            lines << QStringLiteral("%1. %2   (идёт)").arg(lives, 2).arg(openStart);
        ui->lblActCount->setText(QString::number(lives));
        ui->actHistory->setPlainText(lives == 0
            ? QStringLiteral("Активаций нет (устройство не активировано).")
            : lines.join(QLatin1Char('\n')));
        break;
    }
    case LtpCmd::FLASH_SPEED_TEST: {
        // Ответ 0x2D — 5 байт: [0]=err, [1..4]=страниц/с (uint32 LE).
        // Двухфазный замер: фаза 1 = запись ЭТАЛОНА (prep, безопасная частота),
        // фаза 2 = ЧТЕНИЕ по готовому эталону на выбранной частоте + сверка.
        if (payload.size() < 5) break;
        const bool    err  = quint8(payload.at(0)) != 0;
        const quint32 pps  = quint32(quint8(payload.at(1)))
                           | (quint32(quint8(payload.at(2))) << 8)
                           | (quint32(quint8(payload.at(3))) << 16)
                           | (quint32(quint8(payload.at(4))) << 24);
        const double  kbps = double(pps) * 256.0 / 1024.0;   // страниц/с × 256 / 1024

        if (m_speedPhase == 1) {
            // ── Ответ на запись ЭТАЛОНА ──
            if (err) {   // эталон не записался/не сверился — дальше идти нельзя
                ui->lblMemSpeed->setText(QStringLiteral("эталон: ошибка"));
                appendLog(QStringLiteral("[Скорость] эталон НЕ записался — стоп (проверь флеш/область)"));
                m_speedPhase = 0;
                m_dev->setTimeout(ui->spinTimeout->value());
                break;
            }
            m_speedRefReady = true;
            m_speedRefStart = ui->spinMemStartPage->value();
            m_speedRefPages = ui->spinMemPages->value();
            appendLog(QStringLiteral("[Скорость] эталон записан и сверен → читаю на выбранной конфигурации"));
            m_speedPhase = 2;
            const quint8 spiIdx  = quint8(qMax(0, ui->comboSpiMode->currentIndex()));
            const quint8 freqIdx = quint8(qMax(0, ui->comboSpiMhz->currentIndex()));
            requestCmd(LtpCmd::FLASH_SET_SPI_MODE, QByteArray(1, char(spiIdx)),  TagManual);
            requestCmd(LtpCmd::FLASH_SET_FREQ,     QByteArray(1, char(freqIdx)), TagManual);
            showMemSpeed(1);
            break;
        }

        // ── Ответ на ЧТЕНИЕ (фаза 2) ──
        if (err) {   // прочитанное не сошлось с эталоном → частота/режим ненадёжны
            ui->lblMemSpeed->setText(QStringLiteral("ошибка (данные)"));
            appendLog(QStringLiteral("[Скорость] чтение не сошлось с эталоном — эта частота/режим для чтения ненадёжны"));
        } else {
            ui->lblMemSpeed->setText(QStringLiteral("%1 КБайт/с").arg(kbps, 0, 'f', 0));
            appendLog(QStringLiteral("[Скорость] чтение %1 КБ/с (данные сверены с эталоном)").arg(kbps, 0, 'f', 0));
        }
        m_speedPhase = 0;
        m_dev->setTimeout(ui->spinTimeout->value());
        break;
    }
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
                    // «Адрес»/Факт — живой текущий адрес записи.
                    ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(m_imgStartPage + m_imgPagesDone - 1) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
                    // «Старт»/«Страниц» (Проверка, Факт) — тот же живой ход,
                    // теперь и для образа (22.07.2026, по просьбе).
                    ui->lblCurActivePage->setText(QString::number(m_imgStartPage + m_imgPagesDone));
                    ui->lblCurPages->setText(QString::number(m_imgPagesDone));
                    // «Занято» — живое обновление по факту записи (22.07.2026,
                    // тот же приём, что у термотеста), без ожидания отдельного
                    // пересканирования Flash.
                    if (m_firstFreePage >= 0
                        && int(m_imgStartPage) + m_imgPagesDone > m_firstFreePage) {
                        m_firstFreePage = int(m_imgStartPage) + m_imgPagesDone;
                        updateOccupiedLabel();
                    }
                } else {
                    if (btn) btn->setText(m_imgBtnLabel);
                    m_imgActiveBtn = nullptr;
                    setOpsEnabled(true);
                    ui->btnMemReadImg->setEnabled(true);
                    // «Адрес»/Факт — итоговый адрес последней записанной страницы.
                    ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(m_imgStartPage + m_imgPagesTotal - 1) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
                    ui->lblCurActivePage->setText(QString::number(m_imgStartPage + m_imgPagesTotal));
                    ui->lblCurPages->setText(QString::number(m_imgPagesTotal));
                    // «Занято» — настоящий пересчёт по завершении (22.07.2026).
                    // «Старт» подтянем к новой первой свободной, когда пересчёт
                    // реально придёт (см. flashBinSearchSendNext).
                    m_syncStartAfterImage = true;
                    flashBinSearchStart();
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
                    // Загружен образ = на приборе появились данные → взводим флаг
                    // «несохранённые» (0x30=4, персистентно) и локально, чтобы
                    // «Сохранить» покраснела. Итоговую покраску даст updateSaveSegment
                    // по завершении сканирования памяти (flashBinSearchStart выше).
                    m_dataFlagSet = true;
                    requestCmd(LtpCmd::DATA_FLAG, QByteArray(1, char(4)), TagManual);
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
            // «Занято» — живое обновление по факту записи (22.07.2026, тот же
            // приём, что у образа/термотеста), если реально писали (не ошибка).
            if (d && d[0] == 0 && m_firstFreePage >= 0) {
                // Позиция ВНУТРИ текущего цикла, а не сквозной pagesDone: при
                // нескольких циклах повторная запись в те же страницы не должна
                // раздувать «Занято» (26.07.2026). pagesDone уже инкрементирован
                // выше, поэтому только что записанная страница в цикле —
                // (pagesDone-1) % pageTotal, первая свободная за ней = +1.
                const int inCycle = (m_test.pagesDone - 1)
                                    % (m_test.pageTotal > 0 ? m_test.pageTotal : 1);
                const int curPage = int(m_test.pageStart) + inCycle + 1;
                if (curPage > m_firstFreePage) {
                    m_firstFreePage = curPage;
                    updateOccupiedLabel();
                }
                // «Адрес» — живой адрес текущей операции записи (26.07.2026,
                // по ходу, как и «Байт»/Факт), не только по завершении серии.
                const int wrPage = int(m_test.pageStart) + inCycle;
                ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(wrPage) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
            }
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
                // memTestDump() переиспользует m_test.* под СВОЙ проход — сохраняем
                // реальный результат теста, восстановим по факту завершения дампа
                // (22.07.2026, по факту: панель показывала «Страниц 64» от дампа
                // вместо настоящих 100/100 от теста).
                m_savedTestForDump = m_test;
                m_restoreTestAfterDump = true;
                memTestDump();
                // «Занято» — настоящий пересчёт по завершении операции
                // (22.07.2026), не только «угадывание» вперёд по ходу —
                // так учитываются и стирания, и любые другие изменения.
                flashBinSearchStart();
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
        if (tag == TagWriteVerify && (payload.size() < 2 || d[0] != 0)) {
            m_dev->clearQueue();
            m_test.running = false;
            m_test.step    = TestStep::Idle;
            ui->btnMemWrite->setText(QStringLiteral("Запись"));
            setOpsEnabled(true);
            memTestUpdateUi();
            appendLog(QStringLiteral(
                "[Запись] контрольное чтение стр.%1 не удалось — останов")
                    .arg(m_test.pageStart + m_test.pagesDone));
            break;
        }
        if (payload.size() >= 2 && d[0] == 0) {
            const QByteArray data = payload.mid(1);
            if (tag == TagArchive) {
                archiveHandleChunk(data, m_arc.chunkRequested);
            } else if (tag == TagWriteVerify) {
                // Контрольное чтение сразу после записи страницы (22.07.2026).
                // Тот же байт ожидания, что и при записи (editTestByte) — но
                // ЗАФИКСИРОВАННЫЙ на момент старта «Запись», а не читаемый
                // заново (правка поля в процессе не должна сбивать сверку, тот
                // же принцип, что у m_tempRun.testByte).
                // «Байт»/Факт — реально прочитанный при контрольном чтении
                // байт (26.07.2026: раньше верификация читала, но в поле не
                // клала → после «Запись» «Байт»/Факт оставался пустым).
                if (!data.isEmpty()) m_test.lastReadByte = quint8(data.at(0));
                bool okB = false;
                const int exp = ui->editTestByte->text().trimmed().toInt(&okB, 16);
                int mism = 0;
                if (okB) for (char c : data) if (quint8(c) != quint8(exp)) ++mism;
                if (mism > 0) m_test.errTotal += mism;
                // Порог остановки — spinMaxErrors («Ошибок», Задать). 0 (пусто,
                // спец.значение) = не останавливаться, просто считать все
                // ошибки до конца (как раньше). N>0 = остановиться, как только
                // накопленный errTotal достигнет N (по просьбе 22.07.2026).
                const int maxErrors = ui->spinMaxErrors->value();
                if (mism > 0 && maxErrors >= 0 && m_test.errTotal >= maxErrors) {
                    memTestUpdateUi();
                    m_dev->clearQueue();   // дальше страницы уже не запишутся
                    m_test.running = false;
                    m_test.step    = TestStep::Idle;
                    ui->btnMemWrite->setText(QStringLiteral("Запись"));
                    setOpsEnabled(true);
                    memTestUpdateUi();
                    appendLog(QStringLiteral(
                        "[Запись] стр.%1: контрольное чтение — расхождений %2/256, "
                        "порог ошибок (%3) достигнут — останов (записано %4 из %5 стр.)")
                            .arg(m_test.pageStart + m_test.pagesDone)
                            .arg(mism).arg(maxErrors).arg(m_test.pagesDone)
                            .arg(m_test.pageTotal * m_test.cycleTotal));
                } else if (mism > 0) {
                    memTestUpdateUi();
                    appendLog(QStringLiteral(
                        "[Запись] стр.%1: контрольное чтение — расхождений %2/256 "
                        "(всего %3%4), продолжаю")
                            .arg(m_test.pageStart + m_test.pagesDone)
                            .arg(mism).arg(m_test.errTotal)
                            .arg(maxErrors >= 0 ? QStringLiteral("/%1").arg(maxErrors) : QString()));
                }
                // Совпало — ничего дополнительно не делаем, прогресс уже
                // считает обработчик FLASH_WRITE (TagManual) для той же
                // страницы; здесь только проверка и возможный останов.
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
                if (!data.isEmpty())
                    m_test.lastReadByte = quint8(data.at(0));

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
                // Во время ВИДИМОГО дампа после «Запись»/«Чтение»
                // (m_restoreTestAfterDump) панель «Проверка» держит замороженный
                // результат теста — не показываем ход самого дампа, иначе Факт
                // проскакивает «ещё один цикл» после «готово» (26.07.2026).
                if (!(m_test.step == TestStep::Dump && m_restoreTestAfterDump))
                    memTestUpdateUi();
                if (m_test.pagesDone >= m_test.pageTotal) {
                    // завершено
                    if (m_test.step == TestStep::Dump) {
                        renderHexDump(m_test.pageStart, m_dumpBuf);
                        ui->memReport->appendPlainText(QStringLiteral(
                            "Дамп готов: стр.%1..%2 (%3 байт)")
                            .arg(m_test.pageStart + 1)
                            .arg(m_test.pageStart + m_test.pageTotal)
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
                    if (m_silentDump) {
                        // «Тихий» автодамп реально завершился ТОЛЬКО СЕЙЧАС
                        // (ответ асинхронный — раньше тут стояло немедленное
                        // восстановление m_test сразу после вызова, что ломало
                        // сам ход дампа и роняло программу при заходе на
                        // вкладку). Панель «Проверка» — просто пусто, а не
                        // цифры дампа (не относящегося к пользовательскому тесту).
                        m_silentDump = false;
                        m_test.pageTotal = 0;
                    } else if (m_restoreTestAfterDump) {
                        // Видимый дамп после «Запись»/«Чтение» реально завершился
                        // ТОЛЬКО СЕЙЧАС — восстанавливаем настоящий результат
                        // теста (не блэнкаем, в отличие от «тихого» случая выше),
                        // 22.07.2026, по факту описанного бага (см. комментарий
                        // у места вызова memTestDump).
                        m_restoreTestAfterDump = false;
                        m_test = m_savedTestForDump;
                    }
                    memTestUpdateUi();
                    return;
                }
                memTestStep();
            } else if (tag == TagAct) {
                // активация: обрабатывается в activationHandleResponse (ниже)
            } else if (tag == TagTempRun) {
                // верификация страницы температурного прогона
                if (m_tempRun.step == TempRunStep::Read) {
                    if (!data.isEmpty())   // «Байт» Факт (см. MemTestState)
                        m_test.lastReadByte = quint8(data.at(0));
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
                    // «Занято» — живое обновление по факту записи (22.07.2026),
                    // без полного скана: термотест только что записал
                    // m_tempRun.page — если это дальше текущей известной
                    // границы, граница и «Занято» сдвигаются сразу.
                    if (m_firstFreePage >= 0 && int(m_tempRun.page) + 1 > m_firstFreePage) {
                        m_firstFreePage = int(m_tempRun.page) + 1;
                        updateOccupiedLabel();
                    }
                    // Блок текущей температурной точки: осталось ли страниц из
                    // «проходов»? Да — сразу пишем/сверяем следующую страницу;
                    // нет — блок завершён, ждём следующего шага °C (17.07.2026).
                    if (--m_tempRun.pagesLeftInStep > 0) {
                        ++m_tempRun.page;                     // следующая страница блока
                        m_tempRun.step   = TempRunStep::Idle; // разрешить tempRunDoOp
                        m_test.pageStart = m_tempRun.page;
                        ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(m_tempRun.page) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
                        memTestUpdateUi();
                        tempRunDoOp();
                    } else {
                        m_tempRun.step   = TempRunStep::Idle;
                        m_test.step      = TestStep::Idle;
                        m_test.pageStart = m_tempRun.page;
                        ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(m_tempRun.page) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
                        memTestUpdateUi();
                        // «Занято» — настоящий пересчёт по завершении блока
                        // страниц (22.07.2026), не только «угадывание».
                        flashBinSearchStart();
                    }
                }
            } else {
                // ручной запрос (btnMemRead) — обновляем прогресс
                if (!data.isEmpty())   // «Байт» Факт (см. MemTestState)
                    m_test.lastReadByte = quint8(data.at(0));
                bool ok = false;
                const int exp = ui->editTestByte->text().trimmed().toInt(&ok, 16);
                int mism = 0;
                if (ok) for (char c : data) if (quint8(c) != quint8(exp)) ++mism;
                m_test.errTotal += mism;   // НЕКОРРЕКТНЫЕ БАЙТЫ, не страницы (17.07.2026)
                ++m_test.pagesDone;
                m_test.pageCur = m_test.pagesDone % (m_test.pageTotal > 0 ? m_test.pageTotal : 1);
                memTestUpdateUi();
                // Порог остановки — тот же spinMaxErrors, что и у «Запись»
                // (22.07.2026, по просьбе: одно поле, должно работать
                // одинаково для обеих кнопок). 0 (пусто) — не останавливаться.
                const int maxErrors = ui->spinMaxErrors->value();
                const bool thresholdHit = (mism > 0 && maxErrors >= 0 && m_test.errTotal >= maxErrors);
                if (thresholdHit) m_dev->clearQueue();   // дальше страницы уже не читаем
                if (thresholdHit || m_test.pagesDone >= m_test.pageTotal * m_test.cycleTotal) {
                    m_test.running = false;
                    m_test.step    = TestStep::Idle;
                    ui->btnMemRead->setText(QStringLiteral("Чтение"));
                    setOpsEnabled(true);
                    memTestUpdateUi();
                    if (thresholdHit)
                        appendLog(QStringLiteral(
                            "[Чтение] порог ошибок (%1) достигнут — останов (прочитано %2 из %3 стр.)")
                                .arg(maxErrors).arg(m_test.pagesDone)
                                .arg(m_test.pageTotal * m_test.cycleTotal));
                    // После чтения — обновить гекс-панель актуальным содержимым
                    // (17.07.2026, по запросу), без нажатия «Прочитать».
                    // memTestDump() переиспользует m_test.* для СВОЕГО прогона
                    // (ограничен kAutoDumpMaxPages) — сохраняем реальный итог
                    // теста, восстановим по факту завершения дампа (22.07.2026,
                    // по факту: панель показывала «Страниц 64» от дампа вместо
                    // настоящего результата чтения, «Ошибок» — зелёный OK
                    // вместо реальных расхождений).
                    m_savedTestForDump = m_test;
                    m_restoreTestAfterDump = true;
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
        // ts_activation (d+13): unix-сек последней активации или 0xFFFFFFFF —
        // показываем в тултипе шкалы активации (27.07.2026).
        {   quint32 tsAct; std::memcpy(&tsAct, d + 13, 4);
            m_tsActivation = tsAct;   // ts из ЖУРНАЛА (t0 калибровки, подпись). НЕ состояние!
            // 03.08.2026: «активирован» = NOR-ФЛАГ [0] (m_deviceActivated из маски
            // 0x30), а НЕ журнал. Флаг и журнал НЕ связаны: «Очистить журналы»
            // стирает записи (ts пропадёт), но флаг остаётся → прибор активирован.
            const bool hasTs = (tsAct != 0xFFFFFFFFu);
            const QString tsTxt = hasTs
                ? QDateTime::fromSecsSinceEpoch(qint64(tsAct)).toString(QStringLiteral("yy.MM.dd HH:mm"))
                : QString();
            // Карточка «Активация» — три состояния по флагам [0]/[2] (03.08.2026):
            //   [0] чист            → не активирован
            //   [0] есть, [2] чист   → активирован (+ дата)
            //   [0] есть, [2] есть   → деактивирован (сохранение выполнено)
            QString actTxt;
            if (!m_deviceActivated)     actTxt = QStringLiteral("не активирован");
            else if (!m_dataSaved)      actTxt = hasTs
                    ? QStringLiteral("активирован\n%1").arg(tsTxt)
                    : QStringLiteral("активирован");
            else                        actTxt = QStringLiteral("деактивирован");
            ui->lblActDate->setText(actTxt);
            updateActivationState();
            ui->lblActTs->setText((m_deviceActivated && hasTs) ? tsTxt : QStringLiteral(" "));
            ui->barActivation->setToolTip(m_deviceActivated
                ? (hasTs ? QStringLiteral("Активировано: %1").arg(tsTxt)
                         : QStringLiteral("Активировано"))
                : QStringLiteral("Не активировано"));
            if (m_act.step == ActStep::Idle)
                ui->barActivation->setSectorState(6, m_deviceActivated
                    ? ActivationBar::SectorState::Done
                    : ActivationBar::SectorState::Disabled); }

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
            // «Сброс WDT» — сегмент 1. Одиночный клик (m_actWdtPending) резолвим
            // в ЛЮБОМ режиме (02.08.2026 — лента видна и в операторе, клик там
            // тоже работает). Живой фоновый индикатор (без клика) красим только
            // в «Сервис» и только вне волны активации.
            if (m_actWdtPending) {
                m_actWdtPending = false;
                activationSetSectorMinDelay(1, target, m_actWdtActiveMs);   // Сброс WDT = сегмент 1
            } else {
                // Живой индикатор в ОБОИХ режимах, сразу по состоянию: рестарты>0 →
                // красный (оператор тоже видит тревогу), иначе жёлтый. Не вмешиваемся
                // только во время активной волны — её шаги ведут цвет; зелёный из волны.
                // Клик в операторе всё равно блокирован гейтом. (02.08.2026)
                const bool waveActive = (m_act.step != ActStep::Idle
                                      && m_act.step != ActStep::Done
                                      && m_act.step != ActStep::Error);
                if (!waveActive) {
                    if (!synced)
                        activationSetSector(1, ActivationBar::SectorState::Idle);   // красный — рестарты (всегда)
                    else if (m_act.step != ActStep::Done)
                        activationSetSector(1, ActivationBar::SectorState::Active); // жёлтый — вне пост-волны
                    // synced && Done → держим ЗЕЛЁНЫЙ от завершённой волны (03.08.2026)
                }
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

    // Чтение внутренней Flash (06.08.2026): ответ с флагом ошибки приходит СЮДА,
    // мимо onResponse, — например когда прошивка старая и команды 0x3E не знает.
    // Без сброса состояние «идёт чтение» висело вечно, и кнопка переставала
    // работать до перезапуска программы.
    if (m_iflash.running && cmd == LtpCmd::IFLASH_READ) {
        m_iflash.running = false;
        ui->lblIflashAddr->setText(code == 0x01
            ? QStringLiteral("прошивка не умеет читать внутр. Flash")
            : QStringLiteral("ошибка чтения"));
    }
}

void MainWindow::onRequestFailed(quint8 cmd)
{
    // GO ответа не ждёт: прибор отвечает и в ту же миллисекунду сбрасывается,
    // так что молчание тут — норма. Операция закрыта ещё на шаге END, поэтому
    // даже строку «[ТАЙМАУТ]» не пишем — она выглядела бы как ошибка сразу
    // после успешной заливки.
    if (cmd == LtpCmd::BOOT_GO) return;

    // Собеседник замолчал — индикатор в жёлтый: порт открыт, а отвечать некому
    // (сняли питание, выдернули кабель, прибор ушёл в сон). 06.08.2026: раньше
    // жёлтый включался только при закрытии порта и при уходе в Stop2, поэтому
    // обесточенный прибор оставался «зелёным». Позеленеет сам от первого же
    // ответа — см. onResponse, ничего нажимать не надо.
    if (m_regSeen) {
        m_regSeen = false;
        setLinkLed(1);
    }

    appendLog(QStringLiteral("[ТАЙМАУТ] cmd=0x%1 — нет ответа после повторов")
                  .arg(cmd, 2, 16, QLatin1Char('0')));

    // Чтение внутренней Flash молчанием — снять «идёт чтение», иначе кнопка
    // «Прочитать» залипнет до перезапуска программы (06.08.2026).
    if (m_iflash.running && cmd == LtpCmd::IFLASH_READ) {
        m_iflash.running = false;
        ui->lblIflashAddr->setText(QStringLiteral("нет ответа"));
    }

    // Заливка прошивки: молчание на любом её шаге — стоп всей операции, иначе
    // прогресс замрёт навсегда, а кнопка останется в состоянии «Стоп».
    // Прибор при этом цел: он в загрузчике и ждёт повторной заливки.
    if (m_fw.running && cmd >= LtpCmd::BOOT_ENTER && cmd < LtpCmd::BOOT_GO) {
        fwUpdateFinish(false, (m_fw.phase <= 2)
            ? QStringLiteral("Прибор не ответил на команду. Возможно, версия без загрузчика.")
            : QStringLiteral("Нет ответа на шаге %1.").arg(m_fw.phase),
            m_fw.phase >= 2);   // на фазе 1 управление ещё не передавали
        return;
    }

    // Одиночный клик по сегменту 1/2 «завис» жёлтым без ответа — таймаут
    // переводит его в ошибку (красный), не оставляя гореть вечно (21.07.2026).
    if (m_actWdtPending && (cmd == LtpCmd::RESET_STATS || cmd == LtpCmd::GET_STATS)) {
        m_actWdtPending = false;
        activationSetSector(1, ActivationBar::SectorState::Error);   // «Сброс WDT» = сегмент 1
    }
    if (m_actSyncPending && (cmd == LtpCmd::SET_DATETIME || cmd == LtpCmd::GET_DATETIME)) {
        m_actSyncPending = false;
        activationSetSector(2, ActivationBar::SectorState::Error);   // «Синхро время» = сегмент 2
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
    // Стирание/термотест зависли в true при таймауте (устройство не отвечает) →
    // СБРАСЫВАЕМ, иначе гейт «Сначала остановите текущую операцию» навсегда
    // блокирует вход в «Сервис» (03.08.2026, после зависания прибора).
    if (m_erasing) {
        m_dev->clearQueue();
        m_erasing = false;
        appendLog(QStringLiteral("⚠ Стирание прервано — устройство не отвечает"));
        setOpsEnabled(true);
    }
    if (m_tempRun.running) {
        m_dev->clearQueue();
        m_tempRun.running = false;
        appendLog(QStringLiteral("⚠ Термотест прерван — устройство не отвечает"));
        setOpsEnabled(true);
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
        // Индикатор обратно в жёлтый: порт открыт, но регистратора не слышно.
        // Зазеленеет сам, как только он отзовётся (onResponse). 06.08.2026.
        m_regSeen = false;
        setLinkLed(1);
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

// Перекраска заголовка при смене фокуса — см. applyCaptionColors выше.
void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
#ifdef Q_OS_WIN
    if (e->type() == QEvent::ActivationChange)
        applyCaptionColors(winId(), isActiveWindow());
#endif
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

    m_serviceMode = on;
    // Лента = карта активации (28.07.2026): сегменты 1..7 — индикаторы, их цвет
    // ведут волна ▶ / GET_STATS / флаг. Здесь принудительно НЕ красим (раньше
    // сегменты 1,2 форсились красным как «сервисные кнопки» — устарело). При
    // выходе из «Сервис» гасим шаги волны 1..6 в серый (индикаторы неактивны вне
    // активации); сегмент 0 «Сохранить данные» ведёт флаг, сегмент 7 VBAT — свой.
    if (!on)
        for (int s = 1; s <= 6; ++s)
            ui->barActivation->setSectorState(s, ActivationBar::SectorState::Disabled);

    // Синхронизируем галки — взаимоисключающее выделение
    QSignalBlocker b1(ui->actGoData), b2(ui->actEngineerMode);
    ui->actGoData->setChecked(!on);
    ui->actEngineerMode->setChecked(on);

    ui->tabsMain->tabBar()->setVisible(on);
    ui->chkSimulation->setVisible(false);   // перенесено в меню «Вид» (27.07.2026)
    if (m_actSimulation) m_actSimulation->setVisible(on);   // «Симуляция» — только в сервисе
    if (!on)
        ui->tabsMain->setCurrentWidget(ui->tabDashboard);
    if (lblMode)
        lblMode->setText(on ? QStringLiteral("🔧 режим: сервис")
                            : QStringLiteral("🔒 режим: оператор"));

    // Вход в сервис → сразу выполнить процедуру «Сохранить» и снять блокировку
    // (02.08.2026, по просьбе: иначе приходится прыгать по вкладкам в поисках
    // «Сохранить данные», а все операции упираются в гейт несохранённых данных).
    // Только при подключении и взведённом флаге; startDataDump сам сбросит флаг
    // по успеху (updateSaveSegment → зелёный «Сохранено»). Отмена диалога —
    // флаг остаётся нетронутым, как и раньше (сознательный отказ сохранять).
    if (on && m_link->isOpen() && m_dataFlagSet) {
        appendLog(QStringLiteral("[Сервис] есть несохранённые данные — политика сохранения…"));
        dataSaveFlow();   // §3.2: активирован→в файл; не активирован→спросить
    }
}

void MainWindow::tickPcClock()
{
    ui->lblPcTime->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    ui->lblStendPcTime->setText(ui->lblPcTime->text());   // дубль на «Стенде» (18.07)

    // Идёт выдержка RTC-калибровки → на кнопке «Стоп» живой накопленный интервал
    // (функционально: видно, сколько уже копится). Вне выдержки — просто «Стоп».
    if (m_rtcCalUiState == 1 && m_rtcCalT0.isValid()) {
        qint64 el = m_rtcCalT0.secsTo(QDateTime::currentDateTime());
        if (el < 0) el = 0;
        const qint64 h = el / 3600;          // суммарные ЧАСЫ (могут быть >24, напр. 602)
        const qint64 m = (el % 3600) / 60, s = el % 60;
        ui->btnRtcCalStop->setText(QStringLiteral("Стоп  %1:%2:%3")
            .arg(h).arg(m,2,10,QLatin1Char('0')).arg(s,2,10,QLatin1Char('0')));
    }

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
    // Панель версии («Параметры регистратора») убрана с «Данные» (27.07.2026):
    // версия прошивки теперь в верхней карточке (lblDevCard), в одну строку с
    // версией приложения. Освобождаем место по вертикали (под график температуры).
    ui->groupDevParams->hide();

    // Надпись «Не активировано» убрана (27.07.2026): статус активации виден по
    // цвету сегментов линейки (все зелёные = активировано), отдельная строка не
    // нужна — панель активации становится ниже.
    ui->lblActStatus->hide();

    // «● Регистратор»/«● Logger» справа в шапке (lblDataFormat) убираем
    // (27.07.2026) — подключение и так видно по индикатору и COM-порту, а место
    // нужно под сводку. Счётчик сторожа остаётся в панели «Перезапуски».
    ui->lblDataFormat->hide();

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
    initMini(ui->plotTempArc, QColor(0x9B, 0x6F, 0xC9));   // температура — фиолетовый (27.07.2026)
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
    m_tempBars = makeBars(ui->plotTempArc, QColor(0x9B, 0x6F, 0xC9));
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

    // Короткие ВЕРТИКАЛЬНЫЕ подписи слева у каждого графика (27.07.2026): вместо
    // горизонтальных подписей снизу — экономит вертикаль (место под 5-й график
    // температуры). Ось Y показывает только повёрнутую подпись, без делений/линий.
    const auto vLabel = [](QCustomPlot *plot, const QString &text, const QColor &col) -> QCPItemText* {
        plot->axisRect()->setMargins(QMargins(14, 2, 2, 2));   // узкий отступ слева под подпись
        plot->yAxis->setVisible(false);
        auto *t = new QCPItemText(plot);
        t->setClipToAxisRect(false);
        t->setColor(col);
        t->setText(text);
        QFont f = t->font(); f.setPointSize(9); f.setBold(true); t->setFont(f);   // жирный — как значения
        t->setRotation(-90);                                   // вертикально, снизу вверх
        t->setPadding(QMargins(0, 0, 0, 0));
        // Якорь — нижний-левый угол области данных: текст начинается от нижней
        // линии графика и идёт вверх, тело подписи уходит в левый отступ
        // (минимальный зазор слева).
        t->position->setType(QCPItemPosition::ptAxisRectRatio);
        t->position->setCoords(0.0, 1.0);
        t->setPositionAlignment(Qt::AlignLeft | Qt::AlignBottom);
        return t;
    };
    vLabel(ui->plotUptime, QStringLiteral("время"),    QColor(0x4C, 0x8B, 0xC9));
    vLabel(ui->plotSpeed,  QStringLiteral("обороты"),  QColor(0x2F, 0x9E, 0x86));
    vLabel(ui->plotVibro,  QStringLiteral("вибрация"), QColor(0xC0, 0x30, 0x30));
    vLabel(ui->plotVibro2, QStringLiteral("удары"),    QColor(0xE0, 0x8A, 0x20));
    if (auto *tL = vLabel(ui->plotTempArc, QStringLiteral("T°C"), QColor(0x9B, 0x6F, 0xC9)))
        tL->position->setCoords(0.0, 0.88);   // нижний график — поднять от скролла
    // Горизонтальные подписи снизу больше не нужны — скрываем (место по вертикали).
    ui->lblUptimeCaption->hide();
    ui->lblMaxSpeedCaption->hide();
    ui->lblMaxVibroCaption->hide();
    ui->lblMaxVibro2Caption->hide();

    // Макс-значение — ВЕРТИКАЛЬНО в ПРАВОМ поле графика через правую ось-подпись
    // (yAxis2, 27.07.2026): QCustomPlot сам рисует её в правом margin, столбики
    // НЕ перекрывают (в отличие от ручного повёрнутого текста). Значение пишем в
    // render через plot->yAxis2->setLabel(...).
    const auto vValue = [](QCustomPlot *plot, const QColor &col) -> QCPItemText* {
        const QMargins m = plot->axisRect()->margins();
        plot->axisRect()->setMargins(QMargins(m.left(), m.top(), 18, m.bottom()));
        auto *t = new QCPItemText(plot);
        t->setClipToAxisRect(false);
        t->setColor(col);
        QFont f = t->font(); f.setPointSize(9); f.setBold(true); t->setFont(f);
        t->setRotation(-90);                          // снизу вверх, как левые подписи
        t->setPadding(QMargins(0, 0, 0, 0));
        t->position->setType(QCPItemPosition::ptViewportRatio);
        t->position->setCoords(1.0, 1.0);             // правый-нижний угол ВИДЖЕТА
        t->setPositionAlignment(Qt::AlignLeft | Qt::AlignBottom);
        return t;
    };
    m_valLbl[0] = vValue(ui->plotUptime,  QColor(0x4C, 0x8B, 0xC9));
    m_valLbl[1] = vValue(ui->plotSpeed,   QColor(0x2F, 0x9E, 0x86));
    m_valLbl[2] = vValue(ui->plotVibro,   QColor(0xD0, 0x50, 0x50));
    m_valLbl[3] = vValue(ui->plotVibro2,  QColor(0xE0, 0x8A, 0x20));
    m_valLbl[4] = vValue(ui->plotTempArc, QColor(0x9B, 0x6F, 0xC9));
    // «Температура» — нижний график, у скролла тесно: поднимаем значение выше.
    if (m_valLbl[4]) m_valLbl[4]->position->setCoords(1.0, 0.88);
    // Прежние горизонтальные метки значений снизу больше не нужны.
    ui->lblMaxSpeed->hide();
    ui->lblMaxVibro->hide();
    ui->lblMaxVibro2->hide();
    ui->lblMaxTemp->hide();

    // Синхронизация осей X ВСЕХ ЧЕТЫРЁХ графиков циклов (время/гироскоп/два
    // акселерометра): зум/прокрутка любого двигает остальные (18.07.2026).
    // setRange глушит рекурсию — rangeChanged не эмитится без изменения.
    {
        const QList<QCustomPlot*> cyclePlots = {
            ui->plotUptime, ui->plotSpeed, ui->plotVibro, ui->plotVibro2, ui->plotTempArc };
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
        case 4: barH = idx < m_arc.plotTemp.size()     ? m_arc.plotTemp[idx]     : 0.0; break;  // температура
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
            l1 += QStringLiteral("   %1").arg(dt.toString(QStringLiteral("yy.MM.dd")));
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
        case 4:  l2 = QStringLiteral("%1°C").arg(barH, 0, 'f', 0); break;

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
    connect(ui->plotTempArc, &QCustomPlot::mouseMove, this,
            [this, barTip](QMouseEvent *ev) { barTip(ui->plotTempArc, ev, 4); });

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
                                          workDir()).toString();
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
                                          workDir()).toString();
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
    // Формат записи цикла (REC_FORMAT 0x31) — ПЕРЕД стартом каждого прогона
    // (02.08.2026). В прошивке s_recFormat живёт только в ОЗУ (персистентности
    // нет), и сброс прибора между подключением и запуском (watchdog/питание/
    // Standby) возвращал формат к базовому по умолчанию → первый прогон писал
    // базу вместо уплотнённой. Досылаем формат из настроек здесь, в очереди ДО
    // START_*, чтобы автомат писал циклы уже в нужном формате при любом прогоне.
    requestCmd(LtpCmd::REC_FORMAT,
               QByteArray(1, char(quint8(ui->cmbRecFormat->currentIndex()))), TagManual);
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
        const quint8 mark = d[0];   // маркёр слова [0]
        if (mark == 0xFF)           // пустое слово = конец журнала
            { if (errOut && out.isEmpty()) *errOut = QStringLiteral("образ пуст"); goto done; }
        // Уплотнённое (0xF3) — до 5 записей 48Б; базовое (0xF5)/подробное (0xF4) — 1.
        for (int slot = 0, nSlots = (mark == 0xF3) ? 5 : 1; slot < nSlots; ++slot) {
            const quint8 *r = d + 1 + slot * 48;   // запись с offset 1 + slot*48
            quint32 ts, dur, tot; float rpm;
            std::memcpy(&ts,  r + 0,  4);
            std::memcpy(&dur, r + 4,  4);
            std::memcpy(&tot, r + 8,  4);
            std::memcpy(&rpm, r + 16, 4);   // rpm_avg (отчётная «Скорость»)
            const quint16 crc = quint16(r[46]) | (quint16(r[47]) << 8);
            if (ts == 0xFFFFFFFFu) {
                if (slot == 0)   // слово с маркёром, но без записи → конец
                    { if (errOut && out.isEmpty()) *errOut = QStringLiteral("образ пуст"); goto done; }
                break;           // конец записей уплотнённого слова
            }
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
        // ТОЧКА СТЫКА (02.08.2026): смена поколения часов между соседними
        // записями = здесь часы обнулялись (потеря питания в поле), абсолютное
        // время ниже отсчитывается от нуля заново и не сопоставимо с записями
        // выше. Разделитель отличается от заголовка дня (═══), чтобы бросался
        // в глаза. Сбрасываем curDay — после разрыва дата условная, печатаем
        // заголовок дня заново. См. clock_epoch_seam_spec_v1.md.
        if (j > 0 && r.epoch != m_offlDev[j - 1].epoch) {
            text += QStringLiteral(
                "╍╍╍ разрыв времени: часы обнулялись (поколение %1 → %2) ╍╍╍\n\n")
                .arg(m_offlDev[j - 1].epoch).arg(r.epoch);
            curDay = QDate();
        }
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
        m_arc.plotEpoch.append(0.0);   // живой пуш — текущая эпоха, стыков нет
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
    // Формат записи: уплотнённый (index 1) пакует 5 записей в слово (страницу),
    // базовый/подробный — 1 запись на слово (28.07.2026).
    const int recsPerWord = (ui->cmbRecFormat->currentIndex() == 1) ? 5 : 1;
    if (m_flashLiveBasePage >= 0) {
        // занятые слова = записей / записей-на-слово (округление вверх).
        const int words = int((m_flashLiveRecords + recsPerWord - 1) / recsPerWord);
        m_firstFreePage = qMin(m_flashLiveBasePage + words, int(kFlashTotalPages));
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
        // Слово (страница) содержит recsPerWord записей: базовый 1/слово (маркёр
        // 0xF5), уплотнённый 5/слово (маркёр 0xF3). Запись → слот 1+slot*48.
        const int wordIdx = int(idx / recsPerWord);   // номер слова (страницы)
        const int slot    = int(idx % recsPerWord);   // слот внутри слова
        const int need = wordIdx * 256 + 256;         // полная страница слова
        if (m_flashLiveBuf.size() < need)
            m_flashLiveBuf.append(QByteArray(need - m_flashLiveBuf.size(), char(0xFF)));
        if (slot == 0)   // открытие слова — маркёр формата в [0]
            m_flashLiveBuf[wordIdx * 256] = char(recsPerWord == 5 ? 0xF3 : 0xF5);
        m_flashLiveBuf.replace(wordIdx * 256 + 1 + slot * 48, 48,
                               fwBuildRecord(startReg, durS, totS, maxRpm));
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
    const QString path = workDir() + QStringLiteral("/LogLSM_stend_") + ts + QStringLiteral(".txt");
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
    // Обновление версии прошивки без переподключения (кнопку ↻ убрали вместе с
    // нижней панелью версии 27.07.2026): шлём WHO_AM_I, ответ обновит заголовок.
    viewMenu->addSeparator();
    QAction *actRefreshFw = viewMenu->addAction(QStringLiteral("Прочитать версию прошивки"));
    connect(actRefreshFw, &QAction::triggered, this, [this] {
        if (!m_link->isOpen()) return;
        requestCmd(LtpCmd::WHO_AM_I);
        appendLog(QStringLiteral("[TX] WHO_AM_I (обновление версии ПО)"));
    });
    // «Симуляция» перенесена из шапки в меню (27.07.2026): пункт-галка управляет
    // скрытым chkSimulation (он остаётся держателем состояния и логики).
    viewMenu->addSeparator();
    m_actSimulation = viewMenu->addAction(QStringLiteral("Симуляция"));
    m_actSimulation->setCheckable(true);
    m_actSimulation->setToolTip(QStringLiteral(
        "Заполнить экран тестовыми данными без подключённого устройства"));
    m_actSimulation->setVisible(false);   // показывается только в «Сервис» (setServiceMode)
    connect(m_actSimulation, &QAction::toggled, this, [this](bool on) {
        if (ui->chkSimulation->isChecked() != on) ui->chkSimulation->setChecked(on);
    });
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
        "  border-radius: 2px; padding: 4px 12px; color: %5; }"
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
        "QPushButton#btnMemErasePage, QPushButton#btnMemEraseSector,"
        "QPushButton#btnMemEraseChip {"
        "  background: #7A3C00; border: 1px solid #B85C00; color: #FFE4B5; font-weight: 600; }"
        "QFrame#frameMemInfo { background: %2; border: 1px solid %3;"
        "  border-radius: 8px; min-width: 80px; }"
        "QLabel#lblCurByte, QLabel#lblCurPages, QLabel#lblCurActivePage,"
        "QLabel#lblCurCycle, QLabel#lblCurErrors,"
        "QLabel#lblImgPages, QLabel#lblImgAddr,"
        "QLabel#lblImgPagesNA {"
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
// Классификация по первому байту слова: 0xFF пусто; Logger — тип фрейма
// 0xF6..0xFE; Регистратор — маркёр 0xF5 (базовая) / 0xF4 (расширенная),
// оба НИЖЕ логгеровского диапазона (формат «1 цикл = 1 слово», 28.07.2026).
// Раньше у Регистратора здесь был случайный младший байт времени (риск
// ложной классификации ~единицы %); теперь маркёр фиксирован — детект точен.
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
    m_binSearch.lo = 0;                       // от внутренней стр.0 (занято = непустые с начала)
    m_binSearch.hi = int(kFlashTotalPages);   // 65536 — sentinel «чип полностью занят»
    flashBinSearchSendNext();
}

// «Занято» (22.07.2026, по обсуждению — работаем страницами, это и есть
// естественная единица Flash: минимальный блок программирования 256 байт;
// «слово» — не отсюда термин, чтобы не путать с де-факто 2 байтами).
// Задать — сколько страниц занято (красный), Факт — сколько ещё свободно
// (зелёный). Источник — m_firstFreePage (живой, поддерживается
// flashBinSearchStart и прямыми обновлениями после стирания/операций).
// Реальная скорость последней операции записи/чтения (26.07.2026):
// объём (страниц×циклов×256) делим на затраченное время, выводим КБ/с.
// Замер скорости флеша (26.07.2026, вариант Б): по завершении «Запись»/
// «Чтение» забираем у устройства накопленное время операций с чипом (0x2D=1)
// — оно копилось по ходу постраничного обмена, ждать ничего не нужно.
// mode здесь только для объёма в разборе ответа. Групповые не вызывают.
// Замер скорости флеша по кнопке «Скорость» (26.07.2026, финал): крупным
// блоком в спец-условиях (обычный постраничный обмен не трогаем). mode 0 —
// замер записи, в ответ на него запускаем замер чтения (mode 1); обе цифры
// копим в m_speedWrKbps и показываем W/R по приходу чтения.
void MainWindow::showMemSpeed(int mode)
{
    if (!m_link->isOpen()) return;
    const quint32 n     = quint32(qMax(1, ui->spinMemPages->value()));
    // 03.08.2026: эталон теста скорости пишем в КОНЕЦ памяти (последние n страниц),
    // а НЕ по «Старт», чтобы не портить служебную стр.0 (флаги) и данные в начале.
    const quint32 start = (n < kFlashTotalPages) ? (kFlashTotalPages - n) : 0u;
    m_speedBytes = qint64(n) * 256;
    QByteArray p;
    p.append(char(quint8(mode)));
    for (int j = 0; j < 4; ++j) p.append(char((start >> (8*j)) & 0xFF));
    for (int j = 0; j < 4; ++j) p.append(char((n     >> (8*j)) & 0xFF));
    // Обе фазы (запись эталона / чтение) идут ~5 с по эталону RTC — таймаут 30 с.
    ui->lblMemSpeed->setText(mode == 0 ? QStringLiteral("эталон…") : QStringLiteral("замер…"));
    m_dev->setTimeout(30000);
    requestCmd(LtpCmd::FLASH_SPEED_TEST, p, TagSpeedTest);
}

void MainWindow::updateOccupiedLabel()
{
    if (m_firstFreePage < 0) {
        ui->lblImgPagesNA->setText(QStringLiteral(" "));
        ui->lblImgPages->setText(QStringLiteral(" "));
        ui->lblImgPagesNA->setStyleSheet(QString());
        ui->lblImgPages->setStyleSheet(QString());
        return;
    }
    // «Занято» = число реально записанных страниц от внутренней стр.0
    // (экранная «1»). НЕ вычитаем kLogStartPage: в принятой нумерации
    // проекта адрес 0 = страница 1, служебной страницы перед данными нет,
    // поэтому вычет давал недосчёт на 1 — «призрак нуля» (26.07.2026).
    const int usedPages = qMax(0, m_firstFreePage);
    const int freePages = qMax(0, int(kFlashTotalPages) - m_firstFreePage);
    ui->lblImgPagesNA->setText(QString::number(usedPages));
    ui->lblImgPages->setText(QString::number(freePages));
    ui->lblImgPagesNA->setStyleSheet(QStringLiteral("color:#C03030;font-weight:600;"));
    ui->lblImgPages->setStyleSheet(QStringLiteral("color:#1D7A4C;font-weight:600;"));
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
        const int used = m_firstFreePage;
        const QString msg = (m_firstFreePage >= int(kFlashTotalPages))
            ? QStringLiteral("Flash: чип заполнен")
            : QStringLiteral("Flash: первая свободная стр. %1 (занято %2 стр.)")
                  .arg(m_firstFreePage + 1).arg(qMax(0, used));
        appendLog(msg);
        // «Занято» (22.07.2026, по просьбе) — сколько страниц занято по ВСЕМ
        // операциям (образ + тест на стенде + т.д.) / сколько ещё свободно.
        // Страницы, десятичное, красный/зелёный — обновляется при каждом
        // пересчёте первой свободной страницы (после записи/стирания/подключения).
        updateOccupiedLabel();
        updateSaveSegment();   // занятость определилась → сегмент 0 (Стёрто/Сохранить/Сохранено)
        // 03.08.2026: старое «самолечение флага» (слать 0x30=2 при пустой Flash)
        // УБРАНО. Под NOR-флаги стр.0 флаг = само состояние памяти (не отдельная
        // стр.122, которая могла залипнуть), рассинхрона нет; а 0x30=2 в новой
        // прошивке = ВЗВОД «сохранение», что здесь было бы ложно.
        // «Старт»/Задать → новая первая свободная (22.07.2026, по просьбе,
        // только после «Образ» — не переписываем чужой ручной ввод «Старт»
        // по любому другому поводу пересчёта).
        if (m_syncStartAfterImage) {
            m_syncStartAfterImage = false;
            ui->spinMemStartPage->setValue(qBound(1, m_firstFreePage + 1, 65536));
        }
        stendUpdateFlashStat();
        // Авто-дамп при входе на «Тест памяти» (17.07.2026): границы теперь
        // свежие → показываем реальное содержимое. Гейты: та же вкладка, не
        // идёт чтение, не активен стенд.
        if (m_memAutoDumpPending) {
            m_memAutoDumpPending = false;
            if (ui->tabsMain->currentWidget() == ui->tabMemTest
                && !m_test.running && !m_stendActive) {
                m_silentDump = true;
                memTestDump(m_autoDumpStart, m_autoDumpCount);  // образ→свой диапазон,
                                                                // иначе Старт/Страниц
            }
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

    const int used  = qBound(0, m_firstFreePage, total);
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

    // Формат «1 цикл = 1 слово» (28.07.2026): маркёр [0] выбирает формат.
    // 0xFF → пустое слово = конец журнала; 0xF5 базовая / 0xF4 расширенная
    // запись (базовая часть одинакова, лежит с offset 1).
    const quint8 mark = d[0];
    if (mark == 0xFF) {
        m_arc.pageLimit = quint16(qMin<quint32>(m_arc.pageLimit, m_arc.pageStart + pageOffset));
        return;
    }
    // Уплотнённое слово (0xF3) — до 5 записей 48Б в слоте; базовое (0xF5) /
    // подробное (0xF4) — 1 запись. Слоты: offset 1 + slot*48.
    const int nSlots = (mark == 0xF3) ? 5 : 1;
    for (int slot = 0; slot < nSlots; ++slot) {
        const quint8 *r = d + 1 + slot * 48;   // запись цикла с offset 1 + slot*48, 48 байт
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
        const int tempC = int(quint8(r[36])) - 60;   // температура: raw = °C+60 (spec §v2)
        quint16 clockEpoch;
        std::memcpy(&clockEpoch, r + 40, 2);          // поколение часов [40..41] (LE)
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
            // Пустой слот. slot 0 — слово с маркёром, но без записи (аномалия/
            // конец журнала) → клампим и выходим. slot>0 (уплотнённое) — конец
            // записей ЭТОГО слова (норма); конец журнала определит пустое (0xFF)
            // следующее слово, а здесь просто прекращаем читать слоты.
            if (slot == 0) {
                m_arc.pageLimit = quint16(qMin<quint32>(m_arc.pageLimit, m_arc.pageStart + pageOffset));
                return;
            }
            break;
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
                m_arc.plotTemp.append(double(tempC));
                m_arc.plotEpoch.append(double(clockEpoch));   // для линий стыков
            }
            // По-записные данные для stendShowDeviceLog() (см. ArchiveState)
            m_arc.recTs.append(tsStart);
            m_arc.recDur.append(duration != 0xFFFFFFFFu ? duration : 0);
            m_arc.recTotal.append(durationTotal != 0xFFFFFFFFu ? durationTotal : 0);
            m_arc.recEpoch.append(clockEpoch);   // индекс-синхронно с recTs (для точек стыка)
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
            m_arc.plotEpoch.append(0.0);   // Logger: поколение не хранится, стыков нет
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
            r.epoch = (i < m_arc.recEpoch.size()) ? m_arc.recEpoch[i] : 0;
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
        lDate->setText(dt.toString(QStringLiteral("yy.MM.dd")));   // ГГ.ММ.ДД — сортируется хронологически
        lTime->setText(dt.toString(QStringLiteral("HH:mm")));
    };
    fmtTs(m_arc.tsFirst, ui->lblFirstDate, ui->lblFirstTime);
    fmtTs(m_arc.tsLast,  ui->lblLastDate,  ui->lblLastTime);

    // «записано» = число записанных циклов (завершённые + оборванные), числом и
    // КРАСНЫМ — как «Занято» в «Тест памяти» (28.07.2026, по просьбе). Слово
    // «оборван» из подписи убрано (обрезалось по ширине) — детализация в тултипе.
    const qint64 usedCycles = qint64(m_arc.records) + m_arc.brokenRecords;
    ui->lblCyclesUsed->setText(QString::number(usedCycles));
    ui->lblCyclesUsed->setStyleSheet(QStringLiteral("color:#C03030;font-weight:600;"));
    ui->lblCyclesUsed->setToolTip(m_arc.brokenRecords > 0
        ? QStringLiteral("завершённых: %1, оборванных: %2")
              .arg(m_arc.records).arg(m_arc.brokenRecords)
        : QString());

    if (m_arc.mode == ArchiveMode::Registrator) {
        // Остаток циклов = свободные слова × записей-на-слово. Упаковку журнала
        // детектим по факту: базовый ≈1 запись/слово, уплотнённый ≈5 (28.07.2026).
        const int usedWords = qMax(0, int(m_arc.pageLimit) - int(kLogStartPage));
        const qint64 usedCyclesLocal = qint64(m_arc.records) + m_arc.brokenRecords;
        const int rpw = (usedWords > 0)
            ? qMax(1, int((usedCyclesLocal + usedWords - 1) / usedWords)) : 1;
        const qint64 freeWords = qMax(0, int(kFlashTotalPages) - int(kLogStartPage) - usedWords);
        ui->lblCyclesFree->setText(QString::number(freeWords * rpw));
    } else {
        // Logger: 1 фрейм = 1 страница без изменений — остаток в страницах.
        const int pagesUsed = int(m_arc.pageLimit) - int(kLogStartPage);
        ui->lblCyclesFree->setText(QString::number(
            qint64(kFlashTotalPages) - kLogStartPage - qMax(0, pagesUsed)));
    }
    // «доступно» — ЗЕЛЁНЫМ, как свободное в «Тест памяти» (28.07.2026).
    ui->lblCyclesFree->setStyleSheet(QStringLiteral("color:#1D7A4C;font-weight:600;"));

    if (m_arc.mode == ArchiveMode::Registrator && m_arc.haveComplete) {
        // «уровень» (RMS) — АВТОМАСШТАБ под реальный максимум уровня, БЕЗ насыщения
        // (02.08.2026, идеология: график 1 не должен упираться в потолок; сильные
        // удары относительно среднего уходят на «пики»/график 2). Метка = верх
        // шкалы уровня в g. «пики» (m_valLbl[3]) — истинный максимум vib1 за журнал.
        double lvlMaxG = 0.0;
        for (double v : m_arc.plotVibroRms) lvlMaxG = qMax(lvlMaxG, v);
        if (lvlMaxG < 200.0) lvlMaxG = 200.0;   // нижний предел (фон не раздуваем в шум)
        if (m_valLbl[2]) m_valLbl[2]->setText(QStringLiteral("%1 g").arg(lvlMaxG * 1.1 / 1000.0, 0, 'f', 2));
        if (m_valLbl[3]) m_valLbl[3]->setText(QStringLiteral("%1 g").arg(double(m_arc.maxVibro) / 1000.0, 0, 'f', 2));
        if (m_valLbl[1]) m_valLbl[1]->setText(QStringLiteral("%1 о/м").arg(double(m_arc.maxRpm), 0, 'f', 0));
        if (m_valLbl[0]) { double dmax = 0.0; for (double v : m_arc.plotDuration) dmax = qMax(dmax, v);
          m_valLbl[0]->setText(QStringLiteral("%1 мин").arg(dmax / 60.0, 0, 'f', 0)); }
        ui->plotUptime->replot(); ui->plotSpeed->replot(); ui->plotVibro->replot(); ui->plotVibro2->replot();
        // Сводка по ВСЕМУ архиву — в верхнюю карточку (27.07.2026): суммарное
        // время + максимумы скорости/удара/температуры за все измерения.
        { double tmx = 0.0; for (double v : m_arc.plotTemp) tmx = qMax(tmx, v);
          const quint32 tot = m_arc.durationTotal;
          ui->lblDevCard->setText(QStringLiteral(
              "<span style='font-size:8pt;color:#AAAAAA;'>работа </span>"
              "<span style='font-size:13pt;font-weight:bold;color:#5B9BD5;'>%1час %2мин</span>"
              "<span style='font-size:8pt;color:#AAAAAA;'>&nbsp;&nbsp;&nbsp;&nbsp;максимальные:&nbsp;&nbsp; скорость </span>"
              "<span style='font-size:13pt;font-weight:bold;color:#35B597;'>%3 об/мин</span>"
              "<span style='font-size:8pt;color:#AAAAAA;'>&nbsp;&nbsp;&nbsp;удар </span>"
              "<span style='font-size:13pt;font-weight:bold;color:#E89A30;'>%4g</span>"
              "<span style='font-size:8pt;color:#AAAAAA;'>&nbsp;&nbsp;&nbsp;температура </span>"
              "<span style='font-size:13pt;font-weight:bold;color:#AB7FD9;'>%5 °C</span>")
              .arg(tot / 3600).arg((tot % 3600) / 60)
              .arg(double(m_arc.maxRpm), 0, 'f', 0)
              .arg(double(m_arc.maxVibro) / 1000.0, 0, 'f', 1)
              .arg(tmx, 0, 'f', 0)); }
    } else {
        for (int i = 0; i <= 4; ++i) if (m_valLbl[i]) m_valLbl[i]->setText(QString());
        ui->plotUptime->replot(); ui->plotSpeed->replot(); ui->plotVibro->replot();
        ui->plotVibro2->replot(); ui->plotTempArc->replot();
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
                // Окно от НАЧАЛА заполненной области (02.08.2026, по просьбе): было
                // «последние 60 циклов» (анкер к концу — графики выводились не
                // сначала). Теперь показываем первые min(n, 60), но не уже 30.
                const double lo = -0.5;
                const double hi = qMax(double(kUptimeMinBars),
                                       qMin(double(n), double(kUptimeDefaultBars))) - 0.5;
                ui->plotUptime->xAxis->setRange(lo, hi);
            }
            ui->plotUptime->replot();
        } else {
            m_uptimeBars->data()->clear();
            ui->plotUptime->replot();
        }
    }

    if (m_arc.mode == ArchiveMode::Registrator && !m_arc.plotKeys.isEmpty()) {
        // ДВА РАЗНЫХ КАНАЛА vib1 (19.07.2026 / правка 02.08.2026):
        //  • График 1 «уровень» = vib1_RMS — среднеквадратичный, НЕ дёргается на
        //    одиночном ударе → АВТОМАСШТАБ под свой максимум (без насыщения; клип
        //    median×2.5 убран по идеологии: уровень должен работать нормально).
        //  • График 2 «пики» = vib1_peak — ловит удар; Y фикс. БОЛЬШОЙ масштаб
        //    (16 g), без ударов сидит у нуля, удар сразу до потолка.
        double lvlMax = 0.0;
        for (double v : m_arc.plotVibroRms) lvlMax = qMax(lvlMax, v);
        if (lvlMax < 200.0) lvlMax = 200.0;   // нижний предел масштаба (фон не в шум)
        if (m_vibBars)  { m_vibBars->setData(m_arc.plotKeys, m_arc.plotVibroRms);
                          ui->plotVibro->yAxis->setRange(0, lvlMax * 1.1);   // автомасштаб уровня
                          ui->plotVibro->replot(); }
        if (m_vib2Bars) { m_vib2Bars->setData(m_arc.plotKeys, m_arc.plotVibro);   // ПИК
                          const double top = qMax(16000.0, double(m_arc.maxVibro) * 1.05);
                          ui->plotVibro2->yAxis->setRange(0, top);
                          ui->plotVibro2->replot(); }
        // График 5 — температура (27.07.2026): столбики от 0 до макс+запас.
        if (m_tempBars) {
            m_tempBars->setData(m_arc.plotKeys, m_arc.plotTemp);
            double tmax = 40.0;
            for (double v : m_arc.plotTemp) tmax = qMax(tmax, v);
            // Небольшой отступ снизу (28.07.2026): низ графика «приседал» к самой
            // границе панели — даём нижнее поле ~8 %, столбики приподнимаются.
            ui->plotTempArc->yAxis->setRange(-tmax * 0.08, tmax * 1.1);
            ui->plotTempArc->replot();
            const QString tmaxTxt = m_arc.plotTemp.isEmpty()
                ? QStringLiteral(" ") : QStringLiteral("%1 °C").arg(tmax, 0, 'f', 0);
            if (m_valLbl[4]) m_valLbl[4]->setText(tmaxTxt);   // вертикально справа
            ui->lblTempMax->setText(tmaxTxt);   // панель «Температура · макс»
        }
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
        if (m_tempBars) m_tempBars->data()->clear();
        ui->plotTempArc->replot();
        if (m_speedBars) m_speedBars->data()->clear();
        ui->plotSpeed->replot();
    }
    archiveDrawSeams();   // вертикальные линии стыков времени поверх столбиков
    updateCycleScroll();
}

// Вертикальные линии стыков времени на 5 графиках «Данных» (02.08.2026): там,
// где меняется поколение часов (plotEpoch) между соседними циклами, — прибор
// терял питание и часы обнулялись, абсолютное время дальше идёт от нуля. Линия
// рисуется по X между соседними столбиками (индекс-координаты, как у QCPBars).
void MainWindow::archiveDrawSeams()
{
    // Снять прежние линии (перерисовка на каждый разбор/живой пуш).
    for (QCPItemStraightLine *ln : m_seamItems)
        if (ln && ln->parentPlot()) ln->parentPlot()->removeItem(ln);
    m_seamItems.clear();

    if (m_arc.mode != ArchiveMode::Registrator) return;
    const int n = qMin(m_arc.plotKeys.size(), m_arc.plotEpoch.size());
    if (n < 2) return;

    QCustomPlot *plots[] = { ui->plotUptime, ui->plotSpeed, ui->plotVibro,
                             ui->plotVibro2, ui->plotTempArc };
    bool any = false;
    for (int i = 1; i < n; ++i) {
        if (m_arc.plotEpoch[i] == m_arc.plotEpoch[i - 1]) continue;
        const double x = (m_arc.plotKeys[i] + m_arc.plotKeys[i - 1]) / 2.0;  // между столбиками
        for (QCustomPlot *p : plots) {
            auto *ln = new QCPItemStraightLine(p);
            ln->point1->setCoords(x, 0.0);
            ln->point2->setCoords(x, 1.0);   // две точки с одним X → вертикаль
            QPen pen(QColor(0xF0, 0xC0, 0x40));   // янтарная пунктирная — «стык»
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(1.4);
            ln->setPen(pen);
            m_seamItems.append(ln);
        }
        any = true;
    }
    if (any)
        for (QCustomPlot *p : plots) p->replot();
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

    // «Размер»/«Загрузка» — НЕ трогаем здесь (22.07.2026, по замечанию: эти
    // поля теперь показывают результат последней ЗАПИСИ ОБРАЗА, а не границы
    // архивного скана — их перезапись отсюда путала, «сбрасывая» правильный
    // адрес записи на что-то другое сразу после загрузки). «Прочитать» и так
    // берёт диапазон из спинбоксов «Старт»/«Страниц», эти подписи ей не нужны.
    ui->btnMemReadImg->setEnabled(m_arc.pageLimit > kLogStartPage);
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
        m_silentDump = false;   // дамп не стартовал — флаг «тихого» не должен зависнуть
        ui->memReport->appendPlainText(QStringLiteral("⚠ Нет подключения"));
        return;
    }
    if (m_test.running) { m_silentDump = false; return; }

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
        start = quint16(ui->spinMemStartPage->value() - 1);
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
        out += QStringLiteral(" %1: ").arg(addr, 6, 16, QLatin1Char('0')).toUpper();
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
                          ui->lblCurByte, ui->lblCurErrors})
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

    // Страница Факт = адрес текущей страницы (22.07.2026: hex, как и «Задать»)
    ui->lblCurActivePage->setText(QString::number(pg + 1));
    ui->lblCurActivePage->setStyleSheet(QString());

    // «Байт» Факт — реально ПРОЧИТАННЫЙ с устройства байт, не эхо поля
    // «Задать» (баг до 02.07.2026 — сюда копировался editTestByte, ячейка
    // «факт» ничего не проверяла, замечено пользователем). Пока ни одного
    // чтения не было (фаза записи, старт теста) — пусто (пробел, см.
    // соглашение UI). «…» убрано (22.07.2026, по замечанию: не должно быть
    // домысливания — только реально прочитанное значение, ничего сверх).
    if (m_test.lastReadByte >= 0) {
        ui->lblCurByte->setText(QStringLiteral("%1")
            .arg(m_test.lastReadByte, 2, 16, QLatin1Char('0')).toUpper());
    } else {
        ui->lblCurByte->setText(QStringLiteral(" "));
    }
    ui->lblCurByte->setStyleSheet(QString());

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
    // Старт термотеста — с ПЕРВОЙ СВОБОДНОЙ страницы (22.07.2026, по
    // просьбе: тот же принцип, что и у «Загрузка» образа — стирание больше
    // не делаем нигде без явной причины, данные накапливаются). Раньше брали
    // «Старт» из таблицы (могло указывать в середину/на уже занятое место).
    if (m_firstFreePage < 0) {
        appendLog(QStringLiteral("⚠ Термотест: первая свободная страница ещё не известна — "
                                  "подождите сканирование Flash и повторите"));
        return;
    }
    if (m_firstFreePage >= int(kFlashTotalPages)) {
        appendLog(QStringLiteral("⚠ Термотест: свободного места нет — Flash полностью занята"));
        return;
    }
    const int passesPerStep = qMax(1, ui->spinMemPages->value());
    if (m_firstFreePage + passesPerStep > int(kFlashTotalPages)) {
        appendLog(QStringLiteral("⚠ Термотест: недостаточно свободного места (нужно %1 стр., "
                                  "доступно %2 стр. с текущей позиции)")
                       .arg(passesPerStep).arg(int(kFlashTotalPages) - m_firstFreePage));
        return;
    }
    // «Загрузка»/Факт — отражаем стартовый (реальный) адрес термотеста
    // (22.07.2026, по просьбе: колонка «Задать» у Загрузка убрана целиком,
    // осталось только «Факт» — сюда и пишем адрес операции).
    const int grpStartAddr = qMax(int(m_firstFreePage), int(kLogStartPage));
    ui->lblImgAddr->setText(QStringLiteral("0x") + QString::number(qint64(grpStartAddr) * 256, 16).rightJustified(8, QLatin1Char('0')).toUpper());
    m_tempRun.running  = true;
    setOpsEnabled(false, ui->btnTempRun);
    m_tempRun.halted   = false;
    m_tempRun.step     = TempRunStep::Idle;
    // Диапазон термотеста — от первой свободной страницы (см. выше), не из
    // таблицы «Старт»/«Страниц» (та осталась только у Записи/Чтения теста).
    // Обход — по кругу в пределах [start, start+count) (инкремент page в
    // обработчике TagTempRun). Раньше шёл по всему чипу и вдобавок выставлял
    // spinMemPages=65536, из-за чего потом вешалось стирание страниц.
    // Старт групповой операции — не ниже kLogStartPage: служебную стр.0
    // (адрес 0) групповые обходят, пишут с адреса стр.1 (26.07.2026).
    const int grpStart = qMax(int(m_firstFreePage), int(kLogStartPage));
    m_tempRun.rangeStart     = quint16(grpStart);
    // «Проходов» — сколько страниц (от «Старт») прогоняется на КАЖДОЙ температурной
    // точке (17.07.2026). Отдельное поле, не завязано на «Страниц» (то — для байт-
    // теста). Блок [Старт, Старт+проходов) пишется/сверяется при каждом шаге °C.
    m_tempRun.passesPerStep   = passesPerStep;
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
    // (22.07.2026: pageTotal = passesPerStep, не 1 — иначе «Страниц»/Факт
    // застревал на «1», деление по модулю 1 всегда даёт 0/1)
    m_test.pageTotal  = passesPerStep;
    m_test.pageStart  = m_tempRun.page;
    m_test.pageCur    = 0;
    m_test.errTotal   = 0;
    m_test.running    = true;
    m_test.step       = TestStep::Idle;
    m_test.cycleTotal = 9999;   // число операций заранее неизвестно
    m_test.pagesDone  = 0;
    m_test.lastReadByte    = -1;   // «Байт» Факт — пусто до первого чтения
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

// Сегмент 0 ленты по флагу данных + занятости Flash (28.07.2026). Приоритет:
// (1) Flash ПУСТА (первая страница чистая) → жёлтый «Стёрто» — данных нет,
//     красный неуместен даже при залипшем флаге; (2) флаг взведён → красный
//     «Сохранить» (есть несохранённые данные, первая страница заполнена);
//     (3) иначе → зелёный «Сохранено» (данные есть и сохранены). Во время волны
// активации сегмент 0 ведёт сама волна — не перебиваем.
void MainWindow::updateSaveSegment()
{
    // Во время ИДУЩЕЙ волны сегмент 0 ведёт активация; после (Idle/Done/Error) —
    // можно обновлять (после активации пустая память → «Стёрто»).
    if (m_act.step != ActStep::Idle && m_act.step != ActStep::Done
            && m_act.step != ActStep::Error) return;
    // 03.08.2026: сегмент 0 — «нужно сохранить результат или нет». КРАСНЫЙ только
    // когда есть НЕСОХРАНЁННЫЕ данные (данные_есть [1] И сохранение [2] чист);
    // иначе (нет данных, напр. после активации на чистом чипе, ИЛИ уже сохранено)
    // — зелёный «Сохранено». Слова «Стёрто» тут нет (содержимое памяти — сегмент
    // «Память» [4]). m_dataFlagSet = m_dataPresent && !m_dataSaved.
    QString name; ActivationBar::SectorState st;
    if (m_dataFlagSet)       { name = QStringLiteral("Сохранить"); st = ActivationBar::SectorState::Idle;     }  // красный — есть несохранённые
    else                     { name = QStringLiteral("Сохранено"); st = m_saveWasWave
                                   ? ActivationBar::SectorState::Done       // зелёный — после волны
                                   : ActivationBar::SectorState::Active; }  // жёлтый — одиночное
    ui->barActivation->setSectorName(0, name);
    ui->barActivation->setSectorState(0, st);
}

// Трёхпозиционное состояние активации (§2.4) для панели «Журналы и обслуживание»
// (FLASH STM). «Между жизнями» = активирован, но флаг несохранённых снят (данные
// сохранены/стёрты, прибор ждёт следующий рабочий цикл) — раньше это состояние
// нигде не показывалось. 03.08.2026.
void MainWindow::updateActivationState()
{
    // «Состояние» на «FLASH STM» — по флагам [0]активирован/[2]сохранение, три
    // чистых состояния (03.08.2026, «между жизнями» — лишний термин, убран):
    //   [0] чист            → не активирован
    //   [0] есть, [2] чист   → активирован
    //   [0] есть, [2] есть   → деактивирован (сохранение выполнено)
    QString s;
    if (!m_deviceActivated)  s = QStringLiteral("не активирован");
    else if (!m_dataSaved)   s = QStringLiteral("активирован");
    else                     s = QStringLiteral("деактивирован");
    if (ui->lblActState) ui->lblActState->setText(s);
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
    // Лента = карта активации (28.07.2026): сегменты 1..7 — ИНДИКАТОРЫ состояния
    // (цвет ведёт волна ▶ / флаг / GET_STATS), клик по ним ничего не запускает —
    // вся активация ТОЛЬКО по кнопке ▶. Кликом работает лишь сегмент 0
    // «Сохранить данные» (сохранение на диск + сброс флага), в т.ч. в операторе.
    // Одиночный клик по сегменту = выполнить ЕГО операцию, сегмент → ЖЁЛТЫЙ
    // (Active) на время; ▶ по-прежнему прогоняет все шаги подряд зелёным
    // (02.08.2026, восстановлено: 28.07 клики отключили, оставив ленту как
    // индикаторы — по просьбе вернули поштучный запуск для 0/2/3). Симуляция —
    // просто демо-зелёный, без реальных команд.
    if (ui->chkSimulation->isChecked()) {
        activationSetSector(idx, ActivationBar::SectorState::Done);
        return;
    }
    // Режимное правило (02.08.2026, окончательно): в ОПЕРАТОРЕ работают только
    // «Сохранить» (0 — посмотреть файл заранее) и активация (кнопка ▶). Рабочие
    // кнопки «Сброс WDT» (1) и «Синхро время» (2) — ТОЛЬКО в «Сервис».
    if ((idx == 1 || idx == 2) && !m_serviceMode) {
        appendLog(QStringLiteral("«Сброс WDT» / «Синхро время» доступны только в режиме «Сервис»"));
        return;
    }
    if (idx == 1) {          // «Сброс WDT» → RESET_STATS (обнулить счётчики рестартов)
        m_actWdtActiveMs = QDateTime::currentMSecsSinceEpoch();
        m_actWdtPending  = true;
        activationSetSector(1, ActivationBar::SectorState::Active);   // жёлтый — идёт
        requestCmd(LtpCmd::RESET_STATS, {}, TagManual);
        requestCmd(LtpCmd::GET_STATS,   {}, TagManual);   // перечитать счётчики → индикатор
        appendLog(QStringLiteral("[TX] Сброс счётчиков рестартов (WDT)"));
        return;
    }
    if (idx == 2) {          // «Синхро время» → SET_DATETIME по границе секунды,
                             // НАПРЯМУЮ (как WDT через requestCmd), НЕ через
                             // btnSyncTime->click(): клик по кнопке проходит только
                             // если она включена, а командные кнопки бывают временно
                             // отключены → синхро уходило вхолостую. Резолв сегмента —
                             // на КОНТРОЛЬНОМ чтении (gate tag==TagSyncTime в
                             // обработчике GET_DATETIME), чтобы фоновый опрос (~1 Гц)
                             // не «съедал» pending. (02.08.2026)
        m_actSyncActiveMs = QDateTime::currentMSecsSinceEpoch();
        m_actSyncPending  = true;
        activationSetSector(2, ActivationBar::SectorState::Active);   // жёлтый — идёт
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
            requestCmd(LtpCmd::SET_DATETIME, p, TagManual);
            requestCmd(LtpCmd::GET_DATETIME, {}, TagSyncTime);   // контроль → сегмент 2
            appendLog(QStringLiteral("[TX] Синхро время ← ПК (по границе секунды)"));
        });
        return;
    }
    if (idx != 0) {
        appendLog(QStringLiteral("Сегмент — индикатор; запускается волной ▶ "
                                 "(поштучно кликом работают «Сохранить»/«Сброс WDT»/«Синхро время»)"));
        return;
    }
    // «Сохранить данные»: политика §3.2 (активирован→в файл принудительно; не
    // активирован→спросить [в файл]/[без файла]/[отмена]). Работает и на красном
    // (первый раз), и на зелёном (ещё раз/другой путь).
    dataSaveFlow();
}

void MainWindow::activationFail(const QString &reason)
{
    int sec = -1;
    switch (m_act.step) {
    case ActStep::Archive:   sec = 0; break;
    case ActStep::Check:     sec = 3; break;   // Проверка → сегмент 3
    case ActStep::ResetWdt:  sec = 1; break;   // Сброс WDT → сегмент 1
    case ActStep::SyncTime:  sec = 2; break;   // Синхро время → сегмент 2
    case ActStep::Erase:     sec = 4; break;
    case ActStep::TestWrite: sec = 5; break;
    case ActStep::SetReady:  sec = -1; break;   // сегмент «Активация» убран — ошибку не красим
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
        // Шаг 1 — по ФЛАГУ: читаем флаг «несохранённые данные» (0x30=0).
        // Взведён → уходим в сохранение (startDataDump); сброшен → данные уже
        // сохранены (или их нет) → сразу следующий пункт.
        ui->lblActStatus->setText(QStringLiteral("Шаг 1: проверка флага данных…"));
        activationSetSector(0, ActivationBar::SectorState::Active);
        requestCmd(LtpCmd::DATA_FLAG, QByteArray(1, char(0)), TagAct);
        break;

    case ActStep::Check:
        ui->lblActStatus->setText(QStringLiteral("Шаг 2: проверка устройства…"));
        activationSetSector(3, ActivationBar::SectorState::Active);   // Проверка = сегмент 3
        requestCmd(LtpCmd::WHO_AM_I, {}, TagAct);
        break;

    case ActStep::ResetWdt:
        ui->lblActStatus->setText(QStringLiteral("Шаг 3: сброс счётчиков рестартов…"));
        activationSetSector(1, ActivationBar::SectorState::Active);   // Сброс WDT = сегмент 1
        requestCmd(LtpCmd::RESET_STATS, {}, TagAct);
        requestCmd(LtpCmd::GET_STATS,   {}, TagAct);   // контроль: счётчики обнулены
        break;

    case ActStep::SyncTime:
        ui->lblActStatus->setText(QStringLiteral("Шаг 4: синхронизация времени…"));
        activationSetSector(2, ActivationBar::SectorState::Active);   // Синхро время = сегмент 2
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
        ui->lblActStatus->setText(QStringLiteral("Шаг 5: стирание памяти…"));
        activationSetSector(4, ActivationBar::SectorState::Active);
        m_eraseStartMs = QDateTime::currentMSecsSinceEpoch();
        requestCmd(LtpCmd::FLASH_ERASE, {}, TagAct);
        break;

    case ActStep::TestWrite:
        ui->lblActStatus->setText(QStringLiteral("Шаг 6: проверка записи…"));
        activationSetSector(5, ActivationBar::SectorState::Active);
        {   QByteArray p;
            p.append(char(0)); p.append(char(0));       // страница 0
            p.append(QByteArray(256, char(0x55)));
            requestCmd(LtpCmd::FLASH_WRITE, p, TagAct);
        }
        break;

    case ActStep::SetReady:
        // Сегмент «Активация» убран (02.08.2026) — шаг выполняется, но своего
        // индикатора не красит: результат виден в правой панели (запись+дата) и
        // по общей зелёной ленте.
        ui->lblActStatus->setText(QStringLiteral("Шаг 7: постановка на готовность…"));
        {   // Поставить ts_activation (0x30=1 пишет ts в стр.121 + побочно флаг
            // стр.122). Следующей фазой (в activationHandleResponse) флаг сбросим
            // (0x30=2) → «Сохранено» зелёный, ts на стр.121 уцелеет.
            QByteArray p; p.append(char(1));
            const quint32 nowTs = quint32(QDateTime::currentSecsSinceEpoch());
            for (int j = 0; j < 4; ++j) p.append(char((nowTs >> (8 * j)) & 0xFF));
            requestCmd(LtpCmd::DATA_FLAG, p, TagAct);
        }
        break;

    case ActStep::Done:
        // Сегменты 1..5 (Сброс WDT/Синхро/Датчики/Память/Тест записи) — зелёные.
        // Сегмент 0 «Сохранено» — ЗЕЛЁНЫЙ (результат волны): взводим m_saveWasWave,
        // чтобы updateSaveSegment красил зелёным, а не жёлтым (одиночным).
        // Флаг покраснеет сам, когда прибор запишет цикл.
        // Сегмент 6 (VBAT) волна не трогает — отдельный индикатор.
        m_saveWasWave = true;
        for (int i = 1; i < 6; ++i)
            activationSetSector(i, ActivationBar::SectorState::Done);
        ui->lblActStatus->setText(QStringLiteral("✓ АКТИВИРОВАН — отключите сервисный кабель"));
        ui->btnActivate->setText(QStringLiteral("▶"));
        appendLog(QStringLiteral("[ACT] Активация завершена"));
        ui->btnTempRun->setEnabled(true);
        requestCmd(LtpCmd::GET_STATS, {}, TagManual);   // обновить панель «Активация» (активирован + ts)
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
        if (cmd == LtpCmd::DATA_FLAG) {
            // [0]=err, [1]=состояние флага (1=взведён, есть несохранённые данные).
            const bool flagSet = (payload.size() >= 2 && quint8(payload.at(1)) == 1);
            if (flagSet) {
                // Данные не сохранены, а шаг 4 (стирание) их сотрёт → уходим в
                // сохранение образа (startDataDump сам сбросит флаг и по успеху
                // продолжит волну). Отказ от файла — стоп без стирания.
                appendLog(QStringLiteral("[ACT] Шаг 1: есть несохранённые данные — сохраняю на диск…"));
                startDataDump(true);
                return;
            }
            // Флаг сброшен: данные уже сохранены (или их нет) → дальше.
            activationSetSector(0, ActivationBar::SectorState::Done);
            appendLog(QStringLiteral("[ACT] Шаг 1: данные сохранены ранее — идём дальше"));
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
            activationSetSector(3, ActivationBar::SectorState::Done);   // Проверка = сегмент 3
            activationBeginStep(ActStep::ResetWdt);
        }
        break;

    case ActStep::ResetWdt:
        // Шаг 3: сброс счётчиков рестартов. RESET_STATS ACK игнорируем, ждём
        // GET_STATS с обнулёнными счётчиками (payload: см. cmdGetStats).
        if (cmd == LtpCmd::GET_STATS) {
            if (payload.size() < 21 || d[0] != 0) {
                activationFail(QStringLiteral("Сброс WDT: нет ответа GET_STATS")); return;
            }
            quint16 rstTimer = quint16(d[17]) | (quint16(d[18]) << 8);
            quint16 rstPower = quint16(d[19]) | (quint16(d[20]) << 8);
            if (rstTimer != 0 || rstPower != 0) {
                activationFail(QStringLiteral("Счётчики рестартов не обнулены (T=%1 P=%2)")
                    .arg(rstTimer).arg(rstPower));
                return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 3: счётчики рестартов обнулены"));
            activationSetSector(1, ActivationBar::SectorState::Done);   // Сброс WDT = сегмент 1
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
            appendLog(QStringLiteral("[ACT] Шаг 4: время синхронизировано, ΔT=%1 с")
                .arg(diff));
            activationSetSector(2, ActivationBar::SectorState::Done);   // Синхро время = сегмент 2
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
            appendLog(QStringLiteral("[ACT] Шаг 6: запись/чтение OK (%1 байт)")
                .arg(data.size()));
            activationSetSector(5, ActivationBar::SectorState::Done);
            activationBeginStep(ActStep::SetReady);
        }
        break;

    case ActStep::SetReady:
        if (cmd == LtpCmd::DATA_FLAG) {
            // 03.08.2026: активация = ТОЛЬКО запись ts в журнал стр.121 (action 1).
            // NOR-флаги [1]данные/[2]сохранение НЕ трогаем: после стирания (шаг
            // Erase) данных нет, флаги чисты. Старый двухфазный «сброс флага»
            // (action 2) УБРАН — в новой прошивке action 2 = ВЗВОД сохранения, что
            // при активации ложно ставило байт «сохранение». Флаг «данные_есть»
            // взведёт сама прошивка, когда прибор запишет цикл.
            if (payload.size() < 1 || quint8(payload.at(0)) != 0) {
                activationFail(QStringLiteral("активация: ошибка ответа устройства")); return;
            }
            appendLog(QStringLiteral("[ACT] Шаг 7: активирован (ts установлен, данные не записаны)"));
            activationBeginStep(ActStep::Done);
        }
        break;

    default:
        break;
    }
}

// Сохранить данные устройства на диск (образ HEX) + сбросить флаг. Общий путь
// для кнопки «Сохранить данные» (continueWave=false) и шага 1 активации
// (continueWave=true — по завершении волна идёт дальше). По завершении
// обработчик TagActDump в onResponse пишет HEX и шлёт 0x30=2 (сброс флага).
// Отказ от выбора файла: в волне — стоп без стирания; в кнопке — ничего не делаем.
// §3.2 ПОЛИТИКА СОХРАНЕНИЯ = (активировано? по ts_activation) × (несохранённые? по флагу).
// Вызывать при наличии несохранённых данных (m_dataFlagSet) — при входе в «Сервис»
// и по клику сегмента 0 «Сохранить». «Между жизнями» (активирован, флаг снят) сюда
// НЕ попадает (гейт m_dataFlagSet у вызывающего) → тишина, папка не забивается.
//  • активирован            → спросить подтверждение [Да→в файл] / [Нет→пропустить];
//  • НЕ активирован         → спросить: [в файл] / [отметить без файла→флаг снят] / [отмена].
// Если данные УЖЕ сохранены (флаг [3]) — dataSaveFlow вообще не вызывается (гейт
// m_dataFlagSet = данные И !сохранено) → в сервис без запроса. 03.08.2026.
void MainWindow::dataSaveFlow()
{
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
    if (m_deviceActivated) {                 // активирован + не сохранено → подтверждение
        // Раньше здесь была ПРИНУДИТЕЛЬНАЯ запись → при каждом заходе в сервис
        // открывался диалог сохранения, плодя десяток файлов. Теперь спрашиваем.
        if (QMessageBox::question(this, QStringLiteral("Несохранённые данные"),
                QStringLiteral("На устройстве есть несохранённые данные. Сохранить дамп в файл?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
            startDataDump(false);
        else
            appendLog(QStringLiteral("[Сервис] сохранение пропущено — данные на устройстве целы"));
        return;
    }
    // Не активирован → выбор пользователя.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Несохранённые данные"));
    box.setText(QStringLiteral("Устройство не активировано, но на нём есть несохранённые данные.\n"
                               "Что сделать?"));
    QPushButton *toFile = box.addButton(QStringLiteral("Сохранить в файл"), QMessageBox::AcceptRole);
    QPushButton *noFile = box.addButton(QStringLiteral("Отметить без файла"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
    box.setDefaultButton(toFile);
    box.exec();
    if (box.clickedButton() == toFile) {
        startDataDump(false);
    } else if (box.clickedButton() == noFile) {   // сброс флага без файла (DATA_FLAG=2)
        requestCmd(LtpCmd::DATA_FLAG, QByteArray(1, char(2)), TagManual);
        appendLog(QStringLiteral("[Сохранить] отмечено «без файла» — флаг снят, файл не создан"));
    } else {
        appendLog(QStringLiteral("[Сохранить] отменено — флаг не тронут"));
    }
}

void MainWindow::startDataDump(bool continueWave)
{
    if (!m_link->isOpen()) { appendLog(QStringLiteral("⚠ Нет подключения")); return; }
    // Рабочая папка пользователя (06.08.2026). Раньше дампы падали в
    // test_dumps внутри исходников LOGLSMW — неудобно и не переносимо.
    const QString baseDir = workDir();
    const QString def = baseDir + QStringLiteral("/dump_%1.hex")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyMMdd_HHmm")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить данные устройства"), def,
        QStringLiteral("Intel HEX (*.hex)"));
    if (path.isEmpty()) {
        if (continueWave) {
            appendLog(QStringLiteral("[ACT] Сохранение отменено — активация остановлена (данные целы)"));
            activationSetSector(0, ActivationBar::SectorState::Idle);   // красный: данные не сохранены
            activationStop();
        } else {
            appendLog(QStringLiteral("[Сохранить] отменено — флаг не тронут"));
        }
        return;
    }
    // Одиночное сохранение → «Сохранено» будет ЖЁЛТЫМ; в волне → ЗЕЛЁНЫМ.
    m_saveWasWave = continueWave;
    activationSetSector(0, ActivationBar::SectorState::Active);   // жёлтый — идёт сохранение
    m_actDump              = ActDumpState{};
    m_actDump.running      = true;
    m_actDump.continueWave = continueWave;
    m_actDump.startPage = quint16(kLogStartPage);           // журнал начинается со стр.1
    m_actDump.page      = quint16(kLogStartPage);
    m_actDump.pageEnd   = kFlashTotalPages;                 // 65536 (quint32!) — самотерминация по пустой (0xFF) странице
    m_actDump.path      = path;
    ui->lblActStatus->setText(continueWave
        ? QStringLiteral("Шаг 1: сохранение образа…")
        : QStringLiteral("Сохранение данных…"));
    const quint32 addr = quint32(m_actDump.page) << 8;
    QByteArray p;
    for (int j = 0; j < 4; ++j) p.append(char((addr >> (8*j)) & 0xFF));
    for (int j = 0; j < 4; ++j) p.append(char((256  >> (8*j)) & 0xFF));
    m_dev->enqueue(LtpCmd::FLASH_READ, p, TagActDump);
}

// Запись образа в Intel HEX (симметрично loadImageFromHexFile): записи
// 04 (extended linear address при смене старших 16 бит), 00 (данные по 16 Б),
// 01 (EOF). Адрес = (startPage<<8)+смещение. buf — сырые байты (кратно 256).
bool MainWindow::saveImageToHexFile(const QString &path, quint16 startPage,
                                    const QByteArray &buf, QString &errMsg) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        errMsg = QStringLiteral("не удалось открыть файл на запись");
        return false;
    }
    QTextStream out(&f);
    auto emitRec = [&out](quint8 len, quint16 addr, quint8 type, const QByteArray &data) {
        quint32 sum = quint32(len) + (addr >> 8) + (addr & 0xFF) + type;
        QString s = QStringLiteral(":%1%2%3")
            .arg(len,  2, 16, QLatin1Char('0'))
            .arg(addr, 4, 16, QLatin1Char('0'))
            .arg(type, 2, 16, QLatin1Char('0'));
        for (char c : data) {
            sum += quint8(c);
            s += QStringLiteral("%1").arg(quint8(c), 2, 16, QLatin1Char('0'));
        }
        const quint8 cc = quint8((~sum + 1) & 0xFF);
        s += QStringLiteral("%1").arg(cc, 2, 16, QLatin1Char('0'));
        out << s.toUpper() << "\r\n";
    };
    const quint32 base = quint32(startPage) << 8;
    quint32 curHigh = 0xFFFFFFFFu;    // заведомо не совпадёт → первый 04-рекорд выйдет
    for (int i = 0; i < buf.size(); i += 16) {
        const quint32 addr = base + quint32(i);
        const quint32 high = addr & 0xFFFF0000u;
        if (high != curHigh) {
            curHigh = high;
            QByteArray hd;
            hd.append(char((high >> 24) & 0xFF));
            hd.append(char((high >> 16) & 0xFF));
            emitRec(2, 0, 0x04, hd);
        }
        const int n = qMin(16, buf.size() - i);
        emitRec(quint8(n), quint16(addr & 0xFFFF), 0x00, buf.mid(i, n));
    }
    emitRec(0, 0, 0x01, QByteArray());
    f.close();
    errMsg.clear();
    return true;
}
