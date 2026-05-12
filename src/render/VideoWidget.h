#pragma once

#include <QMutex>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QString>
#include <QTimer>

#include "player/PlaybackTypes.h"

// OpenGL 视频显示控件：负责接收 YUV420P 帧并在界面上绘制。
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    // 接收播放器控制器提交的新视频帧，并触发 OpenGL 重绘。
    void presentFrame(const VideoFrameData &frame);
    // 停止播放或切换文件时清空当前画面，回到空状态背景。
    void clearFrame();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private:
    // 创建着色器、上传纹理与重算顶点是渲染链路里的三个核心步骤。
    void createShaderProgram();
    // 把当前 YUV420P 帧上传到三张 OpenGL 纹理。
    void uploadFrame();
    // 根据视频比例和控件比例重算矩形顶点，实现铺满显示区域。
    void updateVertexGeometry();

    QMutex m_frameMutex;
    VideoFrameData m_currentFrame;
    bool m_hasFrame = false;
    bool m_frameDirty = false;

    QOpenGLShaderProgram m_program;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;
    GLuint m_textures[3] = {0, 0, 0};
    bool m_shaderReady = false;
    QString m_shaderError;
    // 空闲动画只在未播放视频时启用，用来填补等待打开媒体时的视觉状态。
    QTimer m_idleAnimationTimer;
    qreal m_idlePhase = 0.0;
};
