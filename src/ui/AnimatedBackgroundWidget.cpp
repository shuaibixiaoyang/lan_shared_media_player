#include "ui/AnimatedBackgroundWidget.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

// 背景动画保持为纯绘制逻辑，避免把装饰效果与业务 UI 混到一起。
AnimatedBackgroundWidget::AnimatedBackgroundWidget(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    m_animationTimer.setInterval(32);
    connect(&m_animationTimer, &QTimer::timeout, this, [this]() {
        m_phase += 0.012;
        if (m_phase > 6.28318530718) {
            m_phase = 0.0;
        }
        update();
    });
    m_animationTimer.start();
}

void AnimatedBackgroundWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    // 这里主要通过渐变、丝带曲线和光斑叠加出柔和的动态背景。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal w = width();
    const qreal h = height();
    const qreal slow = m_phase;
    const qreal driftX = std::sin(slow * 0.65) * 72.0;
    const qreal driftY = std::cos(slow * 0.48) * 54.0;

    QLinearGradient base(-120 + driftX, -80 + driftY, w + 160 - driftX * 0.5, h + 120 + driftY);
    base.setColorAt(0.00, QColor("#f8fcff"));
    base.setColorAt(0.22, QColor("#eaf6ff"));
    base.setColorAt(0.48, QColor("#f7f1ff"));
    base.setColorAt(0.72, QColor("#fff4ec"));
    base.setColorAt(1.00, QColor("#fbfdff"));
    painter.fillRect(rect(), base);

    const auto drawRibbon = [&](qreal baseY, qreal amplitude, qreal speed, const QColor &left,
                                const QColor &middle, const QColor &right, qreal opacityScale) {
        QPainterPath ribbon;
        const qreal y0 = h * baseY + std::sin(slow * speed) * amplitude;
        ribbon.moveTo(-120, y0);
        ribbon.cubicTo(w * 0.18, h * (baseY - 0.12) + std::cos(slow * (speed + 0.2)) * amplitude,
                       w * 0.42, h * (baseY + 0.10) + std::sin(slow * (speed + 0.4)) * amplitude,
                       w * 0.66, h * (baseY - 0.04) + std::cos(slow * (speed + 0.1)) * amplitude);
        ribbon.cubicTo(w * 0.86, h * (baseY - 0.13) + std::sin(slow * (speed + 0.35)) * amplitude,
                       w * 1.02, h * (baseY + 0.08) + std::cos(slow * (speed + 0.25)) * amplitude,
                       w + 120, h * baseY + std::sin(slow * speed + 1.2) * amplitude);
        ribbon.lineTo(w + 120, h * (baseY + 0.18));
        ribbon.cubicTo(w * 0.82, h * (baseY + 0.26),
                       w * 0.48, h * (baseY + 0.12),
                       -120, h * (baseY + 0.23));
        ribbon.closeSubpath();

        QLinearGradient gradient(0, h * (baseY - 0.1), w, h * (baseY + 0.25));
        QColor l = left;
        QColor m = middle;
        QColor r = right;
        l.setAlphaF(l.alphaF() * opacityScale);
        m.setAlphaF(m.alphaF() * opacityScale);
        r.setAlphaF(r.alphaF() * opacityScale);
        gradient.setColorAt(0.0, l);
        gradient.setColorAt(0.48, m);
        gradient.setColorAt(1.0, r);
        painter.fillPath(ribbon, gradient);
    };

    drawRibbon(0.10, 24.0, 0.72, QColor(142, 198, 255, 72), QColor(255, 255, 255, 104),
               QColor(255, 209, 184, 68), 1.0);
    drawRibbon(0.45, 30.0, 0.54, QColor(198, 232, 255, 70), QColor(230, 218, 255, 78),
               QColor(255, 238, 218, 70), 0.92);
    drawRibbon(0.78, 26.0, 0.62, QColor(255, 255, 255, 96), QColor(180, 225, 255, 58),
               QColor(255, 220, 201, 72), 0.95);

    const auto drawGlow = [&](qreal cx, qreal cy, qreal radius, const QColor &color, qreal dx, qreal dy) {
        const QPointF center(w * cx + std::sin(slow * dx) * radius * 0.16,
                             h * cy + std::cos(slow * dy) * radius * 0.12);
        QRadialGradient glow(center, radius);
        QColor inner = color;
        QColor mid = color;
        QColor edge = color;
        inner.setAlpha(80);
        mid.setAlpha(28);
        edge.setAlpha(0);
        glow.setColorAt(0.0, inner);
        glow.setColorAt(0.55, mid);
        glow.setColorAt(1.0, edge);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(center, radius, radius * 0.72);
    };

    drawGlow(0.12, 0.24, w * 0.22, QColor("#78c7ff"), 0.92, 0.76);
    drawGlow(0.84, 0.18, w * 0.18, QColor("#ffd7bd"), 0.65, 0.88);
    drawGlow(0.76, 0.82, w * 0.24, QColor("#a9e8ff"), 0.78, 0.62);
    drawGlow(0.43, 0.58, w * 0.20, QColor("#d8ccff"), 0.52, 0.70);

    painter.setPen(QPen(QColor(255, 255, 255, 70), 1.2));
    for (int i = 0; i < 5; ++i) {
        QPainterPath line;
        const qreal y = h * (0.20 + i * 0.15) + std::sin(slow * 0.8 + i) * 10.0;
        line.moveTo(-80, y);
        line.cubicTo(w * 0.22, y - 26.0,
                     w * 0.46, y + 30.0,
                     w * 0.72, y - 12.0);
        line.cubicTo(w * 0.88, y - 34.0,
                     w * 1.02, y + 18.0,
                     w + 80, y);
        painter.drawPath(line);
    }

    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 14; ++i) {
        const qreal t = slow * 0.9 + i * 0.73;
        const qreal x = w * (0.06 + (i % 7) * 0.145) + std::sin(t) * 18.0;
        const qreal y = h * (0.18 + (i / 7) * 0.46) + std::cos(t * 1.17) * 18.0;
        painter.setBrush(QColor(255, 255, 255, 72));
        painter.drawEllipse(QPointF(x, y), 2.4, 2.4);
    }
}
