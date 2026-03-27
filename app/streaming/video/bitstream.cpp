#include "bitstream.h"

#include "settings/streamingpreferences.h"
#include "videostats.h"
#include "streaming/bitstreammuxer.h"
#include "streaming/session.h"

namespace {

Uint32 hevcSeekCompatIdrCooldownMs(quint64 intervalUs)
{
    if (intervalUs == 0) {
        return 0;
    }

    const Uint32 intervalMs = static_cast<Uint32>(intervalUs / 1000);
    return qMax<Uint32>(250, qMin<Uint32>(1500, intervalMs / 2));
}

QByteArray extractCodecConfig(PDECODE_UNIT du, int videoFormat)
{
    if (du == nullptr || du->bufferList == nullptr) {
        return {};
    }

    const bool isH264 = (videoFormat & VIDEO_FORMAT_MASK_H264) != 0;
    const bool isHevc = (videoFormat & VIDEO_FORMAT_MASK_H265) != 0;
    if (!isH264 && !isHevc) {
        return {};
    }

    QByteArray codecConfig;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        const bool isConfigEntry =
                entry->bufferType == BUFFER_TYPE_SPS ||
                entry->bufferType == BUFFER_TYPE_PPS ||
                (isHevc && entry->bufferType == BUFFER_TYPE_VPS);
        if (!isConfigEntry) {
            break;
        }

        codecConfig.append(entry->data, entry->length);
    }

    return codecConfig;
}

}

BitstreamVideoDecoder::BitstreamVideoDecoder(bool testOnly)
    : m_TestOnly(testOnly),
      m_VideoFormat(0),
      m_Width(0),
      m_Height(0),
      m_FrameRate(0),
      m_SubmittedFrames(0),
      m_LastFrameNumber(0),
      m_LastCaptureStartIdrRequestTicks(0),
      m_LastSeekCompatIdrRequestTicks(0),
      m_SeekCompatIdrIntervalUs(0),
      m_LastKeyframePresentationTimeUs(0)
{
    SDL_zero(m_ActiveWndVideoStats);
    SDL_zero(m_LastWndVideoStats);
}

BitstreamVideoDecoder::~BitstreamVideoDecoder()
{
    Session* session = Session::get();
    if (session != nullptr && session->getBitstreamMuxer() != nullptr) {
        session->getBitstreamMuxer()->shutdown();
    }
}

bool BitstreamVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    m_VideoFormat = params->videoFormat;
    m_Width = params->width;
    m_Height = params->height;
    m_FrameRate = params->frameRate;
    m_SubmittedFrames = 0;
    m_LastFrameNumber = 0;
    m_LastCaptureStartIdrRequestTicks = 0;
    m_LastSeekCompatIdrRequestTicks = 0;
    m_SeekCompatIdrIntervalUs = 0;
    m_LastKeyframePresentationTimeUs = 0;
    SDL_zero(m_ActiveWndVideoStats);
    SDL_zero(m_LastWndVideoStats);

    const int configuredIdrIntervalMs =
            qMax(0, StreamingPreferences::get()->hevcRecordingIdrIntervalMs);
    m_SeekCompatIdrIntervalUs = static_cast<quint64>(configuredIdrIntervalMs) * 1000;

    Session* session = Session::get();
    if (session == nullptr) {
        if (m_TestOnly) {
            return true;
        }
        return false;
    }

    BitstreamMuxer* muxer = session->ensureBitstreamMuxer();
    muxer->setVideoConfig(m_VideoFormat, m_Width, m_Height, m_FrameRate);
    return muxer->initialize(m_TestOnly);
}

bool BitstreamVideoDecoder::isHardwareAccelerated()
{
    return false;
}

bool BitstreamVideoDecoder::isAlwaysFullScreen()
{
    return false;
}

bool BitstreamVideoDecoder::isHdrSupported()
{
    return true;
}

int BitstreamVideoDecoder::getDecoderCapabilities()
{
    return CAPABILITY_DIRECT_SUBMIT;
}

int BitstreamVideoDecoder::getDecoderColorspace()
{
    return COLORSPACE_REC_709;
}

int BitstreamVideoDecoder::getDecoderColorRange()
{
    return COLOR_RANGE_LIMITED;
}

QSize BitstreamVideoDecoder::getDecoderMaxResolution()
{
    return QSize(8192, 8192);
}

int BitstreamVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)
{
    Session* session = Session::get();
    BitstreamMuxer* muxer = session != nullptr ? session->getBitstreamMuxer() : nullptr;
    if (muxer == nullptr) {
        return DR_OK;
    }

    const QByteArray codecConfig = extractCodecConfig(du, m_VideoFormat);
    const bool keyFrame = du->frameType == FRAME_TYPE_IDR || !codecConfig.isEmpty();
    if (keyFrame) {
        m_LastKeyframePresentationTimeUs = du->presentationTimeUs;
    }

    if (muxer->shouldDropFrameBeforeCaptureStart(keyFrame)) {
        const Uint32 now = SDL_GetTicks();
        if (m_LastCaptureStartIdrRequestTicks == 0 ||
            SDL_TICKS_PASSED(now, m_LastCaptureStartIdrRequestTicks + 500)) {
            LiRequestIdrFrame();
            m_LastCaptureStartIdrRequestTicks = now;
        }
        updateOverlayStats(du);
        m_SubmittedFrames++;
        return DR_OK;
    }

    if (!keyFrame &&
        (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0 &&
        m_SeekCompatIdrIntervalUs > 0 &&
        du->presentationTimeUs >= m_LastKeyframePresentationTimeUs + m_SeekCompatIdrIntervalUs) {
        const Uint32 now = SDL_GetTicks();
        const Uint32 cooldownMs = hevcSeekCompatIdrCooldownMs(m_SeekCompatIdrIntervalUs);
        if (m_LastSeekCompatIdrRequestTicks == 0 ||
            SDL_TICKS_PASSED(now, m_LastSeekCompatIdrRequestTicks + cooldownMs)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Requesting periodic HEVC IDR frame for seek compatibility (%u ms interval)",
                        static_cast<unsigned int>(m_SeekCompatIdrIntervalUs / 1000));
            LiRequestIdrFrame();
            m_LastSeekCompatIdrRequestTicks = now;
        }
    }

    QByteArray frameBuffer;
    frameBuffer.reserve(du->fullLength);

    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        frameBuffer.append(entry->data, entry->length);
    }

    if (!muxer->writeVideoFrame(frameBuffer,
                                du->presentationTimeUs,
                                du->receiveTimeUs,
                                keyFrame,
                                codecConfig)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed writing reordered video bitstream to FFmpeg muxer");
        muxer->shutdown();

        SDL_Event event = {};
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        return DR_OK;
    }

    updateOverlayStats(du);
    m_SubmittedFrames++;
    return DR_OK;
}

void BitstreamVideoDecoder::renderFrameOnMainThread()
{
}

void BitstreamVideoDecoder::setHdrMode(bool)
{
}

bool BitstreamVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO)
{
    return true;
}

void BitstreamVideoDecoder::updateOverlayStats(PDECODE_UNIT du)
{
    if (!m_LastFrameNumber) {
        m_ActiveWndVideoStats.measurementStartUs = LiGetMicroseconds();
        m_LastFrameNumber = du->frameNumber;
    }
    else {
        m_ActiveWndVideoStats.networkDroppedFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_ActiveWndVideoStats.totalFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_LastFrameNumber = du->frameNumber;
    }

    m_BwTracker.AddBytes(du->fullLength);

    if (du->frameHostProcessingLatency != 0) {
        if (m_ActiveWndVideoStats.minHostProcessingLatency != 0) {
            m_ActiveWndVideoStats.minHostProcessingLatency =
                    qMin(m_ActiveWndVideoStats.minHostProcessingLatency, du->frameHostProcessingLatency);
        }
        else {
            m_ActiveWndVideoStats.minHostProcessingLatency = du->frameHostProcessingLatency;
        }

        m_ActiveWndVideoStats.maxHostProcessingLatency =
                qMax(m_ActiveWndVideoStats.maxHostProcessingLatency, du->frameHostProcessingLatency);
        m_ActiveWndVideoStats.totalHostProcessingLatency += du->frameHostProcessingLatency;
        m_ActiveWndVideoStats.framesWithHostProcessingLatency++;
    }

    m_ActiveWndVideoStats.receivedFrames++;
    m_ActiveWndVideoStats.decodedFrames++;
    m_ActiveWndVideoStats.renderedFrames++;
    m_ActiveWndVideoStats.totalFrames++;
    m_ActiveWndVideoStats.totalReassemblyTimeUs += (du->enqueueTimeUs - du->receiveTimeUs);

    if (LiGetMicroseconds() <= m_ActiveWndVideoStats.measurementStartUs + 1000000) {
        return;
    }

    VIDEO_STATS lastTwoWndStats = {};
    VideoStatsText::addVideoStats(m_LastWndVideoStats, lastTwoWndStats);
    VideoStatsText::addVideoStats(m_ActiveWndVideoStats, lastTwoWndStats);

    char statsText[1024];
    VideoStatsText::stringifyVideoStats(m_VideoFormat,
                                        m_Width,
                                        m_Height,
                                        lastTwoWndStats,
                                        statsText,
                                        sizeof(statsText));

    if (Session::get() != nullptr && Session::get()->isBitstreamPassthroughMode()) {
        Session::get()->updateRecordingOverlayText(statsText);
    }

    SDL_memcpy(&m_LastWndVideoStats, &m_ActiveWndVideoStats, sizeof(m_ActiveWndVideoStats));
    SDL_zero(m_ActiveWndVideoStats);
    m_ActiveWndVideoStats.measurementStartUs = LiGetMicroseconds();
}
