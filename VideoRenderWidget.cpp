#include "VideoRenderWidget.h"

#include <QMouseEvent>
#include <QPainter>

VideoRenderWidget::VideoRenderWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
}

void VideoRenderWidget::setFrame(const QImage& frame)
{
    m_currentFrame = frame;
    update();
}

void VideoRenderWidget::setOverlayIcon(const QPixmap& icon)
{
    m_overlayIcon = icon;
    update();
}

void VideoRenderWidget::showOverlay(bool show)
{
    m_showOverlay = show;
    update();
}

void VideoRenderWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (m_currentFrame.isNull()) {
        return;
    }

    QSize targetSize = m_currentFrame.size();
    targetSize.scale(size(), Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), targetSize);
    targetRect.moveCenter(rect().center());
    painter.drawImage(targetRect, m_currentFrame);

    if (m_showOverlay && !m_overlayIcon.isNull()) {
        // Set opacity to make the overlay icon less intrusive (lighter/semi-transparent)
        painter.setOpacity(0.7);

        // Scale icon to be reasonable size (e.g. 20% of min dimension, capped at 128px)
        int iconDim = std::min(width(), height()) * 0.2;
        if (iconDim > 128) iconDim = 128;
        if (iconDim < 48) iconDim = 48;

        QSize iconSize = m_overlayIcon.size();
        iconSize.scale(iconDim, iconDim, Qt::KeepAspectRatio);

        QRect iconRect(QPoint(0, 0), iconSize);
        iconRect.moveCenter(rect().center());

        painter.drawPixmap(iconRect, m_overlayIcon);
        
        // Reset opacity for future drawing operations if any
        painter.setOpacity(1.0);
    }
}

void VideoRenderWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit clicked();
    }
    QWidget::mousePressEvent(event);
}
