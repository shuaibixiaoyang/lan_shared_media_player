#include "render/VideoWidget.h"

#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QPainter>
#include <QPainterPath>
#include <QMutexLocker>
#include <QVector2D>
#include <QDebug>
#include <QTimer>

#include <cmath>

namespace
{
// 顶点着色器：负责把矩形顶点与纹理坐标直接传给片元阶段。
constexpr const char *kVertexShaderSource = R"(
#version 120
attribute vec2 position;
attribute vec2 texCoord;

varying vec2 vTexCoord;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vTexCoord = texCoord;
}
)";

// 片元着色器：把三张 Y/U/V 纹理合成为最终 RGB 像素。
constexpr const char *kFragmentShaderSource = R"(
#version 120
varying vec2 vTexCoord;

uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;

void main()
{
    float y = texture2D(texY, vTexCoord).r;
    float u = texture2D(texU, vTexCoord).r - 0.5;
    float v = texture2D(texV, vTexCoord).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    gl_FragColor = vec4(r, g, b, 1.0);
}
)";
}

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAutoFillBackground(false);

    // 没有视频帧时持续做轻量动态背景，避免播放器区域显得完全静止。
    m_idleAnimationTimer.setInterval(32);
    connect(&m_idleAnimationTimer, &QTimer::timeout, this, [this]() {
        m_idlePhase += 0.015;
        if (m_idlePhase > 6.28318530718) {
            m_idlePhase = 0.0;
        }

        if (!m_hasFrame) {
            update();
        }
    });
    m_idleAnimationTimer.start();
}

VideoWidget::~VideoWidget()
{
    makeCurrent();

    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_ebo != 0) {
        glDeleteBuffers(1, &m_ebo);
    }
    glDeleteTextures(3, m_textures);

    doneCurrent();
}

void VideoWidget::presentFrame(const VideoFrameData &frame)
{
    QMutexLocker locker(&m_frameMutex);
    m_currentFrame = frame;
    m_hasFrame = true;
    m_frameDirty = true;
    update();
}

void VideoWidget::clearFrame()
{
    QMutexLocker locker(&m_frameMutex);
    m_currentFrame = {};
    m_hasFrame = false;
    m_frameDirty = false;
    update();
}

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);

    createShaderProgram();

    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    glGenTextures(3, m_textures);

    for (GLuint texture : m_textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    updateVertexGeometry();
}

void VideoWidget::resizeGL(int, int)
{
    updateVertexGeometry();
}

void VideoWidget::paintGL()
{
    // 有视频帧时使用 OpenGL shader 把 YUV 转成 RGB 绘制。
    // 没有视频帧时用 QPainter 绘制空状态欢迎背景。
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_shaderReady) {
        QPainter painter(this);
        painter.setPen(Qt::red);
        painter.drawText(rect(), Qt::AlignCenter, m_shaderError.isEmpty() ? tr("OpenGL shader initialization failed") : m_shaderError);
        return;
    }

    bool hasFrame = false;
    {
        QMutexLocker locker(&m_frameMutex);
        hasFrame = m_hasFrame;
    }

    if (!hasFrame) {
        // 空状态下使用 QPainter 绘制欢迎占位页，而不是走 OpenGL 视频纹理流程。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.fillRect(rect(), QColor("#f5f9ff"));

        QLinearGradient overlay(0, 0, width(), height());
        overlay.setColorAt(0.0, QColor(248, 252, 255, 248));
        overlay.setColorAt(0.42, QColor(236, 245, 255, 236));
        overlay.setColorAt(1.0, QColor(246, 240, 255, 244));
        painter.fillRect(rect(), overlay);

        const qreal blobAX = width() * 0.18 + std::sin(m_idlePhase * 0.9) * 34.0;
        const qreal blobAY = height() * 0.22 + std::cos(m_idlePhase * 0.7) * 22.0;
        QRadialGradient blobA(QPointF(blobAX, blobAY), width() * 0.24);
        blobA.setColorAt(0.0, QColor(93, 161, 255, 94));
        blobA.setColorAt(0.58, QColor(93, 161, 255, 24));
        blobA.setColorAt(1.0, QColor(93, 161, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(blobA);
        painter.drawEllipse(QPointF(blobAX, blobAY), width() * 0.24, width() * 0.24);

        const qreal blobBX = width() * 0.82 + std::cos(m_idlePhase * 0.8) * 42.0;
        const qreal blobBY = height() * 0.72 + std::sin(m_idlePhase * 1.1) * 30.0;
        QRadialGradient blobB(QPointF(blobBX, blobBY), width() * 0.20);
        blobB.setColorAt(0.0, QColor(108, 225, 255, 78));
        blobB.setColorAt(0.58, QColor(108, 225, 255, 18));
        blobB.setColorAt(1.0, QColor(108, 225, 255, 0));
        painter.setBrush(blobB);
        painter.drawEllipse(QPointF(blobBX, blobBY), width() * 0.20, width() * 0.20);

        QRectF badgeRect(width() * 0.11, height() * 0.11, width() * 0.34, height() * 0.14);
        painter.setBrush(QColor(255, 255, 255, 208));
        painter.drawRoundedRect(badgeRect, 20, 20);

        QFont badgeFont = painter.font();
        badgeFont.setPointSize(11);
        badgeFont.setBold(true);
        painter.setFont(badgeFont);
        painter.setPen(QColor("#4179e9"));
        painter.drawText(badgeRect.adjusted(20, 14, -20, -14), Qt::AlignLeft | Qt::AlignTop, tr("LUMA PLAYER  •  READY"));

        QFont titleFont = painter.font();
        titleFont.setPointSize(30);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(QColor("#112543"));
        painter.drawText(QRectF(0, height() * 0.34, width(), 48), Qt::AlignCenter, tr("Drop into a softer playback flow"));

        QFont subtitleFont = painter.font();
        subtitleFont.setPointSize(13);
        subtitleFont.setBold(false);
        painter.setFont(subtitleFont);
        painter.setPen(QColor("#6a7c99"));
        painter.drawText(QRectF(width() * 0.22, height() * 0.45, width() * 0.56, 40),
                         Qt::AlignCenter,
                         tr("Open a local video and let FFmpeg, Qt audio, and OpenGL take over."));

        const QPointF center(width() / 2.0, height() * 0.63);
        const qreal pulse = 42.0 + std::sin(m_idlePhase * 1.8) * 5.0;
        painter.setBrush(QColor(255, 255, 255, 224));
        painter.setPen(QPen(QColor(92, 139, 255, 120), 2));
        painter.drawEllipse(center, pulse, pulse);

        QPainterPath playPath;
        playPath.moveTo(center.x() - 10, center.y() - 16);
        playPath.lineTo(center.x() + 18, center.y());
        playPath.lineTo(center.x() - 10, center.y() + 16);
        playPath.closeSubpath();

        QLinearGradient playGradient(center.x() - 18, center.y() - 16, center.x() + 18, center.y() + 16);
        playGradient.setColorAt(0.0, QColor("#4e8fff"));
        playGradient.setColorAt(1.0, QColor("#56d4ff"));
        painter.fillPath(playPath, playGradient);

        QRectF tipRect(width() * 0.33, height() * 0.75, width() * 0.34, 40);
        painter.setBrush(QColor(255, 255, 255, 182));
        painter.drawRoundedRect(tipRect, 18, 18);
        painter.setPen(QColor("#28425f"));
        painter.drawText(tipRect, Qt::AlignCenter, tr("Open File  •  Play  •  Enjoy"));
        return;
    }

    uploadFrame();

    m_program.bind();
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    const int positionLocation = m_program.attributeLocation("position");
    const int texCoordLocation = m_program.attributeLocation("texCoord");
    if (positionLocation < 0 || texCoordLocation < 0) {
        qWarning() << "[VideoWidget] shader attributes not found"
                   << "positionLocation=" << positionLocation
                   << "texCoordLocation=" << texCoordLocation;
        m_program.release();
        return;
    }

    m_program.enableAttributeArray(positionLocation);
    m_program.setAttributeBuffer(positionLocation, GL_FLOAT, 0, 2, 4 * sizeof(float));
    m_program.enableAttributeArray(texCoordLocation);
    m_program.setAttributeBuffer(texCoordLocation, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);

    m_program.setUniformValue("texY", 0);
    m_program.setUniformValue("texU", 1);
    m_program.setUniformValue("texV", 2);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    m_program.disableAttributeArray(positionLocation);
    m_program.disableAttributeArray(texCoordLocation);
    m_program.release();
}

void VideoWidget::createShaderProgram()
{
    m_shaderReady = false;
    m_shaderError.clear();

    // 着色器编译/链接失败时保留错误信息，方便直接绘制到控件中央。
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSource)) {
        m_shaderError = tr("Vertex shader compile failed");
        qWarning() << "[VideoWidget]" << m_shaderError << m_program.log();
        return;
    }

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShaderSource)) {
        m_shaderError = tr("Fragment shader compile failed");
        qWarning() << "[VideoWidget]" << m_shaderError << m_program.log();
        return;
    }

    if (!m_program.link()) {
        m_shaderError = tr("Shader link failed");
        qWarning() << "[VideoWidget]" << m_shaderError << m_program.log();
        return;
    }

    m_shaderReady = true;
    qDebug() << "[VideoWidget] shader program linked successfully";
}

void VideoWidget::uploadFrame()
{
    // 解码线程交给 UI 的视频帧已经是紧凑 YUV420P。
    // 这里分别上传 Y/U/V 三个平面，片元着色器再合成 RGB。
    QMutexLocker locker(&m_frameMutex);
    if (!m_frameDirty || !m_hasFrame) {
        return;
    }

    const int frameWidth = m_currentFrame.width;
    const int frameHeight = m_currentFrame.height;
    if (frameWidth <= 0 || frameHeight <= 0) {
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 三个平面分别上传到独立纹理，渲染时再在 shader 中完成 YUV -> RGB 转换。
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frameWidth, frameHeight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_currentFrame.yPlane.constData());

    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frameWidth / 2, frameHeight / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_currentFrame.uPlane.constData());

    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frameWidth / 2, frameHeight / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_currentFrame.vPlane.constData());

    m_frameDirty = false;
    // updateVertexGeometry() 也会访问同一把锁，所以这里先解锁再重算顶点，避免死锁。
    locker.unlock();
    updateVertexGeometry();
}

void VideoWidget::updateVertexGeometry()
{
    QMutexLocker locker(&m_frameMutex);

    float imageAspect = 16.0f / 9.0f;
    if (m_hasFrame && m_currentFrame.height > 0) {
        imageAspect = static_cast<float>(m_currentFrame.width) / static_cast<float>(m_currentFrame.height);
    }

    const float widgetAspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : imageAspect;

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    // 采用 cover 策略铺满显示区域，宁可轻微裁切，也不保留黑边。
    if (widgetAspect > imageAspect) {
        scaleY = widgetAspect / imageAspect;
    } else {
        scaleX = imageAspect / widgetAspect;
    }

    const float vertices[] = {
        -scaleX, -scaleY, 0.0f, 1.0f,
         scaleX, -scaleY, 1.0f, 1.0f,
         scaleX,  scaleY, 1.0f, 0.0f,
        -scaleX,  scaleY, 0.0f, 0.0f
    };
    const GLuint indices[] = {0, 1, 2, 2, 3, 0};

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}
