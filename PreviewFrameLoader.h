#ifndef PREVIEWFRAMELOADER_H
#define PREVIEWFRAMELOADER_H

#include <QObject>
#include <QImage>
#include <QString>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/avstring.h>
}

#include "VideoFrameConverter.h"

class PreviewFrameLoader : public QObject
{
    Q_OBJECT

public:
    explicit PreviewFrameLoader(QObject* parent = nullptr);
    ~PreviewFrameLoader() override;

    bool openMedia(const QString& path);
    void close();
    bool loadPreviewFrame(qint64 targetMs);
    void requestPreviewFrame(qint64 targetMs);
    void cancelPendingPreview();

signals:
    void previewImageReady(const QImage& image, qint64 ptsMs);
    void errorOccurred(const QString& message);

private:
    static QString ffErr2Str(int err);
    qint64 framePtsToMs(const AVFrame* frame) const;
    bool loadPreviewFrameInternal(qint64 targetMs, QImage* outImage, qint64* outPtsMs);
    void previewLoop();
    void stopPreviewWorker();

    AVFormatContext* m_formatContext = nullptr;
    AVCodecContext* m_videoCodecContext = nullptr;
    int m_videoStreamIndex = -1;
    VideoFrameConverter m_frameConverter;
    std::thread m_previewThread;
    std::mutex m_previewMutex;
    std::condition_variable m_previewCondition;
    bool m_stopRequested = false;
    bool m_requestPending = false;
    qint64 m_requestMs = -1;
    uint64_t m_requestSerial = 0;
};

#endif // PREVIEWFRAMELOADER_H
