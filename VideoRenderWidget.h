#ifndef VIDEORENDERWIDGET_H
#define VIDEORENDERWIDGET_H

#include <QImage>
#include <QPixmap>
#include <QWidget>

class VideoRenderWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoRenderWidget(QWidget* parent = nullptr);
    void setOverlayIcon(const QPixmap& icon);
    void showOverlay(bool show);

signals:
    void clicked();

public slots:
    void setFrame(const QImage& frame);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QImage m_currentFrame;
    QPixmap m_overlayIcon;
    bool m_showOverlay = false;
};

#endif
