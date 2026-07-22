#include "activationbar.h"

#include <QPainter>
#include <QToolTip>
#include <QHelpEvent>
#include <QMouseEvent>

namespace {
const QColor kRed    (0xC0, 0x30, 0x30);
const QColor kGreen  (0x1D, 0x7A, 0x4C);
const QColor kYellow (0xE0, 0xA0, 0x20);
const QColor kGray   (0x50, 0x50, 0x50);
const int    kGap    = 3;   // зазор между секторами, px (эскиз R21)
const int    kRadius = 2;
} // namespace

ActivationBar::ActivationBar(QWidget *parent)
    : QWidget(parent)
{
    setSectorCount(6);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ActivationBar::setSectorCount(int n)
{
    m_states.fill(SectorState::Idle, n);
    m_names.resize(n);
    update();
}

void ActivationBar::setSectorState(int idx, SectorState st)
{
    if (idx < 0 || idx >= m_states.size())
        return;
    m_states[idx] = st;
    update();
}

void ActivationBar::setSectorName(int idx, const QString &name)
{
    if (idx < 0 || idx >= m_names.size())
        return;
    m_names[idx] = name;
}

void ActivationBar::reset()
{
    // Без подписи (функция ещё не назначена) — Disabled, не Idle
    // (21.07.2026: иначе после reset() серые снова становились красными).
    for (int i = 0; i < m_states.size(); ++i)
        m_states[i] = (i < m_names.size() && !m_names[i].isEmpty())
            ? SectorState::Idle : SectorState::Disabled;
    update();
}

int ActivationBar::sectorAt(const QPoint &pos) const
{
    const int n = m_states.size();
    if (n == 0 || width() <= 0)
        return -1;
    const int idx = pos.x() * n / width();
    return (idx >= 0 && idx < n) ? idx : -1;
}

void ActivationBar::mousePressEvent(QMouseEvent *event)
{
    const int idx = sectorAt(event->pos());
    if (idx >= 0 && idx < m_states.size() && m_states[idx] != SectorState::Disabled)
        emit sectorClicked(idx);
    QWidget::mousePressEvent(event);
}

void ActivationBar::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);

    const int n = m_states.size();
    if (n == 0)
        return;

    const qreal w = (qreal(width()) - kGap * (n - 1)) / n;
    qreal x = 0;
    for (int i = 0; i < n; ++i) {
        QColor c;
        switch (m_states[i]) {
        case SectorState::Done:   c = kGreen;  break;
        case SectorState::Active: c = kYellow;  break;
        case SectorState::Disabled: c = kGray;  break;
        case SectorState::Error:
        case SectorState::Idle:   c = kRed;    break;
        }
        p.setBrush(c);
        p.drawRoundedRect(QRectF(x, 0, w, height()), kRadius, kRadius);
        // Подпись ВНУТРИ сегмента (17.07.2026, по запросу) — светлым, с обрезкой
        // по ширине сегмента.
        if (i < m_names.size() && !m_names[i].isEmpty()) {
            p.setPen(QColor(0xF5, 0xF4, 0xF0));
            const int tw = int(w) - 6;
            const QString t = p.fontMetrics().elidedText(m_names[i], Qt::ElideRight, tw);
            p.drawText(QRectF(x + 3, 0, w - 6, height()), Qt::AlignCenter, t);
            p.setPen(Qt::NoPen);
        }
        x += w + kGap;
    }
}

bool ActivationBar::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(event);
        const int idx = sectorAt(he->pos());
        if (idx >= 0 && !m_names[idx].isEmpty())
            QToolTip::showText(he->globalPos(), m_names[idx], this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}
