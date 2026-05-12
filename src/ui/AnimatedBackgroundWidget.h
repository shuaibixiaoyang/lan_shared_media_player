#pragma once

#include <QTimer>
#include <QWidget>

// 负责主界面的流动背景氛围层，与视频内容本身无关。
class AnimatedBackgroundWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AnimatedBackgroundWidget(QWidget *parent = nullptr);

protected:
    // 绘制主窗口背后的动态渐变、光斑和曲线背景。
    void paintEvent(QPaintEvent *event) override;

private:
    QTimer m_animationTimer;
    qreal m_phase = 0.0;
};
