#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QAction>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QPushButton>
#include <QMenu>

namespace {
constexpr int kControlButtonIconSize = 24;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_controller(new PlaybackController(this))
{
    ui->setupUi(this);
    m_playIcon = QIcon(QStringLiteral(":/icons/play.png"));
    m_pauseIcon = QIcon(QStringLiteral(":/icons/pause.png"));
    m_pauseOverlayIcon = QPixmap(QStringLiteral(":/icons/pause_overlay.png"));

    ui->btnPlayPause->setIconSize(QSize(kControlButtonIconSize, kControlButtonIconSize));
    connect(ui->btnPlayPause, &QPushButton::toggled, this, [this](bool) {
        updatePlayPauseButtonIcon();
    });
    updatePlayPauseButtonIcon();

    m_videoRenderWidget = new VideoRenderWidget(this);
    m_videoRenderWidget->setObjectName(QStringLiteral("videoRenderWidget"));
    m_videoRenderWidget->setMinimumSize(ui->videoWidget->minimumSize());
    m_videoRenderWidget->setSizePolicy(ui->videoWidget->sizePolicy());
    m_videoRenderWidget->setOverlayIcon(m_pauseOverlayIcon);
    ui->verticalLayout->replaceWidget(ui->videoWidget, m_videoRenderWidget);
    ui->videoWidget->deleteLater();

    connect(m_videoRenderWidget, &VideoRenderWidget::clicked,
            this, &MainWindow::onVideoAreaClicked);
    connect(m_controller, &PlaybackController::frameReady,
            m_videoRenderWidget, &VideoRenderWidget::setFrame);
    connect(m_controller, &PlaybackController::overlayVisibilityChanged,
            m_videoRenderWidget, &VideoRenderWidget::showOverlay);
    connect(m_controller, &PlaybackController::playbackStateChanged,
            ui->btnPlayPause, &QPushButton::setChecked);
    connect(m_controller, &PlaybackController::durationChanged, this, [this](qint64 durationMs) {
        ui->sliderPosition->setMinimum(0);
        ui->sliderPosition->setMaximum(static_cast<int>(durationMs));
    });
    connect(m_controller, &PlaybackController::positionChanged, this, [this](qint64 positionMs) {
        if (!ui->sliderPosition->isSliderDown()) {
            ui->sliderPosition->setValue(static_cast<int>(positionMs));
        }
    });
    connect(m_controller, &PlaybackController::timeTextChanged,
            ui->labelTime, &QLabel::setText);
    connect(m_controller, &PlaybackController::statusMessageChanged, this, [this](const QString& msg) {
        statusBar()->showMessage(msg);
    });
    connect(m_controller, &PlaybackController::errorOccurred, this, [this](const QString& msg) {
        QMessageBox::warning(this, QStringLiteral("Decoder Error"), msg);
    });

    // Speed Control Button
    auto* btnSpeed = new QPushButton(tr("1.0x"), this);
    btnSpeed->setFlat(true); // Match flat style if needed
    auto* menuSpeed = new QMenu(btnSpeed);
    const QList<double> rates = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    for (double r : rates) {
        auto* action = menuSpeed->addAction(QStringLiteral("%1x").arg(r));
        connect(action, &QAction::triggered, this, [this, r, btnSpeed]() {
            m_controller->setPlaybackRate(r);
            btnSpeed->setText(QStringLiteral("%1x").arg(r));
        });
    }
    btnSpeed->setMenu(menuSpeed);
    
    // Insert into control layout
    if (auto layout = qobject_cast<QBoxLayout*>(ui->btnStop->parentWidget()->layout())) {
        // Insert before the time label (assuming time label is at the end or near end)
        // Or just add it. Let's add it after stop button.
        int index = layout->indexOf(ui->btnStop);
        if (index >= 0) {
            layout->insertWidget(index + 1, btnSpeed);
        } else {
            layout->addWidget(btnSpeed);
        }
    }

    auto* openAction = new QAction(QStringLiteral("Open"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenMedia);
    menuBar()->addAction(openAction);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::updatePlayPauseButtonIcon()
{
    const bool isPlaying = ui->btnPlayPause->isChecked();
    const QIcon& icon = isPlaying ? m_pauseIcon : m_playIcon;
    const QString label = isPlaying ? tr("暂停") : tr("播放");

    ui->btnPlayPause->setIcon(icon);
    ui->btnPlayPause->setToolTip(label);
    ui->btnPlayPause->setAccessibleName(label);
    ui->btnPlayPause->setText(icon.isNull() ? label : QString());
}

void MainWindow::on_btnPlayPause_clicked()
{
    if (ui->btnPlayPause->isChecked()) {
        ui->btnStop->setChecked(false);
        if (!m_controller->setPlaying(true)) {
            ui->btnPlayPause->setChecked(false);
            return;
        }
        return;
    }

    m_controller->setPlaying(false);
}

void MainWindow::on_btnStop_clicked()
{
    m_controller->stop();
    ui->btnStop->setChecked(false);
}

void MainWindow::on_sliderPosition_sliderMoved(int value)
{
    m_controller->updateSeekPosition(static_cast<qint64>(value));
}

void MainWindow::onOpenMedia()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Video File"),
        QString(),
        QStringLiteral("Video Files (*.mp4 *.mkv *.avi *.mov *.flv *.webm)")
        );
    if (path.isEmpty()) {
        return;
    }
    ui->btnStop->setChecked(false);
    m_controller->openMedia(path);
}

void MainWindow::on_sliderPosition_sliderPressed()
{
    m_controller->beginSeek();
}

void MainWindow::on_sliderPosition_sliderReleased()
{
    m_controller->endSeek(static_cast<qint64>(ui->sliderPosition->value()));
}

void MainWindow::onVideoAreaClicked()
{
    if (!m_controller->isMediaLoaded()) {
        return;
    }
    ui->btnPlayPause->click();
}
