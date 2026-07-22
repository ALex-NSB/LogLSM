#ifndef ACTIVATIONBAR_H
#define ACTIVATIONBAR_H

/*
 * ActivationBar — полоса секторов активации (ТЗ LOGLSMW v2 §2.5).
 * Этап 1: отрисовка состояний; логика шагов — ActivationController (этап 4).
 */

#include <QWidget>
#include <QVector>

class ActivationBar : public QWidget
{
    Q_OBJECT

public:
    enum class SectorState {
        Idle,     // не активировано — красный
        Active,   // выполняется — сплошной жёлтый, держится до завершения
        Done,     // выполнен — зелёный
        Error,    // ошибка — красный (+ стоп)
        Disabled  // функция ещё не назначена — серый, не кликается (21.07.2026)
    };

    explicit ActivationBar(QWidget *parent = nullptr);

    void setSectorCount(int n);                       // default 6
    int  sectorCount() const { return m_states.size(); }
    void setSectorState(int idx, SectorState st);
    void setSectorName(int idx, const QString &name); // tooltip
    void reset();                                     // все Idle

signals:
    void sectorClicked(int idx);                      // клик по сектору (17.07.2026)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override; // клик → sectorClicked(idx)
    bool event(QEvent *event) override;               // tooltip по сектору

private:
    QVector<SectorState> m_states;
    QVector<QString>     m_names;

    int sectorAt(const QPoint &pos) const;
};

#endif // ACTIVATIONBAR_H
