#pragma once

#include "decoder.h"
#include "../bandwidth.h"

#include <QByteArray>

class BitstreamVideoDecoder : public IVideoDecoder
{
public:
    explicit BitstreamVideoDecoder(bool testOnly);
    virtual ~BitstreamVideoDecoder() override;

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual bool isAlwaysFullScreen() override;
    virtual bool isHdrSupported() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override;
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

private:
    void updateOverlayStats(PDECODE_UNIT du);

    bool m_TestOnly;
    int m_VideoFormat;
    int m_Width;
    int m_Height;
    int m_FrameRate;
    quint64 m_SubmittedFrames;
    int m_LastFrameNumber;
    Uint32 m_LastCaptureStartIdrRequestTicks;
    Uint32 m_LastSeekCompatIdrRequestTicks;
    quint64 m_SeekCompatIdrIntervalUs;
    quint64 m_LastKeyframePresentationTimeUs;
    BandwidthTracker m_BwTracker;
    VIDEO_STATS m_ActiveWndVideoStats;
    VIDEO_STATS m_LastWndVideoStats;
};
