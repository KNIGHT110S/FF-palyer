#ifndef VIDEOFRAMECONVERTER_H
#define VIDEOFRAMECONVERTER_H

#include <QImage>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class VideoFrameConverter
{
public:
    VideoFrameConverter() = default;
    ~VideoFrameConverter();

    bool initialize(AVCodecContext* codecContext, QString* errorMessage);
    bool convert(const AVFrame* frame, QImage* outImage) const;
    void reset();

private:
    SwsContext* m_swsContext = nullptr;
    AVFrame* m_rgbFrame = nullptr;
    uint8_t* m_rgbBuffer = nullptr;
    int m_rgbBufferSize = 0;
    int m_width = 0;
    int m_height = 0;
};

#endif // VIDEOFRAMECONVERTER_H
