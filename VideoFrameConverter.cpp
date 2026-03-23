#include "VideoFrameConverter.h"

namespace {
QString ffErr2Str(int err)
{
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
}

VideoFrameConverter::~VideoFrameConverter()
{
    reset();
}

bool VideoFrameConverter::initialize(AVCodecContext* codecContext, QString* errorMessage)
{
    reset();

    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Video decoder context is invalid. Cannot initialize color conversion.");
        }
        return false;
    }

    m_swsContext = sws_getContext(codecContext->width,
                                  codecContext->height,
                                  codecContext->pix_fmt,
                                  codecContext->width,
                                  codecContext->height,
                                  AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR,
                                  nullptr,
                                  nullptr,
                                  nullptr);
    if (!m_swsContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create color conversion context.");
        }
        return false;
    }

    m_rgbFrame = av_frame_alloc();
    if (!m_rgbFrame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate RGB frame.");
        }
        reset();
        return false;
    }

    m_rgbBufferSize = av_image_get_buffer_size(AV_PIX_FMT_BGRA,
                                               codecContext->width,
                                               codecContext->height,
                                               1);
    if (m_rgbBufferSize <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to calculate RGB buffer size.");
        }
        reset();
        return false;
    }

    m_rgbBuffer = static_cast<uint8_t*>(av_malloc(m_rgbBufferSize));
    if (!m_rgbBuffer) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate RGB buffer.");
        }
        reset();
        return false;
    }

    const int fillRet = av_image_fill_arrays(m_rgbFrame->data,
                                             m_rgbFrame->linesize,
                                             m_rgbBuffer,
                                             AV_PIX_FMT_BGRA,
                                             codecContext->width,
                                             codecContext->height,
                                             1);
    if (fillRet < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to initialize RGB frame data: %1")
                                .arg(ffErr2Str(fillRet));
        }
        reset();
        return false;
    }

    m_width = codecContext->width;
    m_height = codecContext->height;
    return true;
}

bool VideoFrameConverter::convert(const AVFrame* frame, QImage* outImage) const
{
    if (!frame || !outImage || !m_swsContext || !m_rgbFrame || !m_rgbBuffer || m_height <= 0) {
        return false;
    }

    const int scaleRet = sws_scale(m_swsContext,
                                   frame->data,
                                   frame->linesize,
                                   0,
                                   m_height,
                                   m_rgbFrame->data,
                                   m_rgbFrame->linesize);
    if (scaleRet <= 0) {
        return false;
    }

    QImage wrapped(m_rgbFrame->data[0],
                   m_width,
                   m_height,
                   m_rgbFrame->linesize[0],
                   QImage::Format_ARGB32);
    *outImage = wrapped.copy();
    return !outImage->isNull();
}

void VideoFrameConverter::reset()
{
    if (m_rgbBuffer) {
        av_free(m_rgbBuffer);
        m_rgbBuffer = nullptr;
    }
    m_rgbBufferSize = 0;

    if (m_rgbFrame) {
        av_frame_free(&m_rgbFrame);
        m_rgbFrame = nullptr;
    }

    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }

    m_width = 0;
    m_height = 0;
}
