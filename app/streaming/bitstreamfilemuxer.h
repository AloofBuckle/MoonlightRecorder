#pragma once

#include "SDL_compat.h"

#include <Limelight.h>

#include <QByteArray>
#include <QQueue>
#include <QString>

struct AVBSFContext;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

class BitstreamFileMuxer
{
public:
    BitstreamFileMuxer();
    ~BitstreamFileMuxer();

    void setVideoConfig(int videoFormat, int width, int height, int frameRate);
    void setAudioConfig(const OPUS_MULTISTREAM_CONFIGURATION& opusConfig);

    bool initialize(const QString& outputTarget);
    bool shouldDropFrameBeforeCaptureStart(bool keyFrame);
    bool writeVideoFrame(const QByteArray& frameBuffer,
                         quint64 presentationTimeUs,
                         quint64 receiveTimeUs,
                         bool keyFrame,
                         const QByteArray& codecConfig);
    bool writeAudioPacket(const QByteArray& packet,
                          quint64 receiveTimeUs,
                          quint32 rtpTimestamp);
    void closeAudioInput();
    void shutdown();

private:
    struct PendingVideoPacket {
        QByteArray packet;
        qint64 ptsUs;
        qint64 dtsUs;
        qint64 durationUs;
        int flags;
    };

    struct PendingAudioPacket {
        QByteArray packet;
        quint64 receiveTimeUs;
        quint32 rtpTimestamp;
        quint64 durationSamples;
    };

    bool loadOutputLibraries();
    bool initializeVideoFilter();
    bool createAudioStreamLocked();
    bool createVideoStreamLocked(const QByteArray& fallbackExtradata);
    bool ensureHeaderWrittenLocked();
    bool flushPendingVideoPacketsLocked();
    bool flushPendingAudioPacketsLocked();
    bool queuePendingVideoPacketLocked(const AVPacket* packet);
    bool queueOrWriteVideoPacketLocked(const PendingVideoPacket& packet);
    bool writeVideoPacketLocked(const PendingVideoPacket& packet, qint64 durationUs);
    bool flushDeferredVideoPacketLocked();
    bool writeVideoPacketsLocked();
    bool writeAudioPacketLocked(const QByteArray& packet,
                                quint64 receiveTimeUs,
                                quint32 rtpTimestamp,
                                quint64 durationSamples);
    bool flushVideoFilterLocked();
    quint64 packetDurationInSamples(const QByteArray& packet) const;
    qint64 mapAudioReceiveTimeToPresentationUs(quint64 receiveTimeUs) const;
    qint64 mapAudioPresentationUs(quint64 receiveTimeUs, quint32 rtpTimestamp);
    QByteArray buildOpusHeadPacket() const;
    QString outputFormatName() const;
    QByteArray buildCodecConfigExtradata() const;

    static int codecIdForVideoFormat(int videoFormat);
    static QString avErrorString(int errorCode);
    static void appendLe16(QByteArray& buffer, quint16 value);
    static void appendLe32(QByteArray& buffer, quint32 value);

    QString m_OutputTarget;
    int m_VideoFormat;
    int m_Width;
    int m_Height;
    int m_FrameRate;
    bool m_AudioConfigValid;
    OPUS_MULTISTREAM_CONFIGURATION m_AudioConfig;
    SDL_mutex* m_Lock;
    bool m_Initialized;
    bool m_HeaderWritten;
    bool m_AudioInputClosed;
    bool m_VideoEpochEstablished;
    qint64 m_VideoPresentationEpochUs;
    qint64 m_FirstVideoPresentationUs;
    bool m_AudioEpochEstablished;
    quint32 m_FirstAudioRtpTimestamp;
    qint64 m_FirstAudioPresentationUs;
    qint64 m_NextAudioPtsUs;
    bool m_AudioPtsInitialized;
    qsizetype m_PendingVideoBytes;
    qsizetype m_PendingAudioBytes;
    bool m_PendingAudioDropLogged;
    QByteArray m_ConfigProbeData;
    QByteArray m_ExplicitCodecConfig;
    QQueue<PendingVideoPacket> m_PendingVideoPackets;
    QQueue<PendingAudioPacket> m_PendingAudioPackets;
    PendingVideoPacket m_DeferredVideoPacket;
    bool m_HaveDeferredVideoPacket;
    qint64 m_LastVideoPacketDurationUs;
    AVFormatContext* m_OutputContext;
    AVStream* m_VideoStream;
    AVStream* m_AudioStream;
    AVBSFContext* m_VideoBsf;
};
