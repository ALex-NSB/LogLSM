#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QFont>
#include <QFileInfo>
#include <QCoreApplication>

// ── ВЕРСИЯ LOGLSMW ────────────────────────────────────────────────────────────
// Формат «ГГ.ММ.ДД ЧЧ:ММ» — как у прошивки A (дата пробел время), 18.07.2026.
// Меняется руками при заметных изменениях приложения. (заведено 17.07.2026)
#define LOGLSMW_VERSION "26.08.04  00.33"
// Дизайнерское имя приложения (в отличие от технического LOGLSMW в QSettings).
#define LOGLSMW_BRAND   "LogLSMW"

// Иконка приложения. Приоритет: НАСТОЯЩИЙ файл logo.png/logo.ico из папки
// LOGLSMW (положи его туда — подхватится сам, пересборка не нужна, грузим в
// рантайме). Если файла нет — рисуем монограмму «LS», чтобы не было дефолтного
// квадрата Qt. (17.07.2026)
static QIcon makeAppIcon()
{
    QStringList files;
#ifdef LOGLSMW_SRC_DIR
    files << QStringLiteral(LOGLSMW_SRC_DIR "/logo.png")
          << QStringLiteral(LOGLSMW_SRC_DIR "/logo.ico");
#endif
    files << QCoreApplication::applicationDirPath() + QStringLiteral("/logo.png")
          << QCoreApplication::applicationDirPath() + QStringLiteral("/logo.ico");
    for (const QString &path : files) {
        if (QFileInfo::exists(path)) {
            QIcon ic(path);
            if (!ic.isNull()) return ic;
        }
    }

    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1D, 0x7A, 0x4C));           // зелёный акцент приложения
    p.drawRoundedRect(4, 4, 56, 56, 14, 14);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(30);
    p.setFont(f);
    p.setPen(QColor(0xF5, 0xF4, 0xF0));
    p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("LS"));
    p.end();
    return QIcon(pm);
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // QSettings нужны имена Org/App для запоминания выбора темы
    // (совпадают с kOrg/kApp в mainwindow.cpp). App = ТЕХНИЧЕСКОЕ имя LOGLSMW —
    // не менять, иначе потеряются сохранённые настройки.
    QApplication::setOrganizationName(QStringLiteral("LogLSM"));
    QApplication::setApplicationName(QStringLiteral("LOGLSMW"));
    QApplication::setApplicationVersion(QStringLiteral(LOGLSMW_VERSION));
    QApplication::setWindowIcon(makeAppIcon());

    // Стиль Fusion, палитру и таблицу стилей применяет MainWindow::applyTheme()
    // (единый источник правды). Ручную светлую палитру отсюда убрали — иначе
    // она перекрыла бы тёмную тему.

    MainWindow w;
    // Дизайнерский заголовок окна: «LogLSMW · ГГ.ММ.ДД.ЧЧ.ММ» (перекрывает
    // windowTitle из .ui). Так в панели задач/заголовке видно и бренд, и версию.
    w.setWindowTitle(QStringLiteral(LOGLSMW_BRAND "  " LOGLSMW_VERSION));
    w.show();
    return QApplication::exec();
}
