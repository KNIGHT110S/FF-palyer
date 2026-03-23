#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QIcon>
#include <QPixmap>

#include "PlaybackController.h"
#include "VideoRenderWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnPlayPause_clicked();
    void on_btnStop_clicked();
    void on_sliderPosition_sliderMoved(int value);
    void onOpenMedia();
    void on_sliderPosition_sliderPressed();
    void on_sliderPosition_sliderReleased();
    void onVideoAreaClicked();

private:
    Ui::MainWindow *ui;
    PlaybackController* m_controller = nullptr;
    VideoRenderWidget* m_videoRenderWidget = nullptr;
    QIcon m_playIcon;
    QIcon m_pauseIcon;
    QPixmap m_pauseOverlayIcon;

    void updatePlayPauseButtonIcon();
};
#endif // MAINWINDOW_H
