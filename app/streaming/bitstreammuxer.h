#pragma once

#include "SDL_compat.h"

#include <Limelight.h>

#include <QByteArray>
#include <QProcess>
#include <QQueue>
#include <QString>
#include <QStringList>

class BitstreamFileMuxer;

class BitstreamMuxer
{
public:
    BitstreamMuxer();
    ~BitstreamMuxer();

    void setVideoConfig(int videoFormat, int width, int height, int frameRate);
    void setAudioConfig(const OPUS_MULTISTREAM_CONFIGURATION& opusConfig);
    bool initialize(bool testOnly);
    bool shouldDropFrameBeforeCaptureStart(bool keyFrame);
    bool writeVideoFrame(const QByteArray& frameBuffer,
                         quint64 presentationTimeUs,
                         quint64 receiveTimeUs,
                         bool keyFrame,
                         const QByteArray& codecConfig);
    bool writeAudioPacket(const char* sampleData,
                          int sampleLength,
                          quint64 receiveTimeUs,
                          quint32 rtpTimestamp);
    void closeAudioInput();
    void shutdown();

    QString getResolvedOutputTarget() const;

private:
    struct VideoFramePacket {
        QByteArray frameBuffer;
        quint64 presentationTimeUs;
        quint64 receiveTimeUs;
        bool keyFrame;
    };

    struct AudioPacketData {
        QByteArray packet;
        quint64 receiveTimeUs;
        quint32 rtpTimestamp;
    };

    bool openFfmpegProcess();
    void closeFfmpegProcess();
    bool openFeedServers();
    bool waitForFeedConnections();
    bool acceptAudioConnection(int timeoutMs);
    bool createFeedListener(qintptr& listenDescriptor, quint16& listenPort, const char* label);
    bool acceptFeedConnection(qintptr listenDescriptor,
                              int timeoutMs,
                              qintptr& clientDescriptor,
                              const char* label);
    bool configureConnectedSocket(qintptr& socketDescriptor, const char* label);
    bool waitForSocketActivity(qintptr socketDescriptor,
                               int timeoutMs,
                               const char* label,
                               bool waitForWrite);
    bool startAudioWriterThread();
    void stopAudioWriterThread(bool flushPendingPackets);
    int runAudioWriterThread();
    bool startVideoWriterThread();
    void stopVideoWriterThread(bool flushPendingFrames);
    int runVideoWriterThread();
    QString buildFfmpegCommand() const;
    QStringList buildFfmpegArguments() const;
    QString getInputFormatName() const;
    QString getResolvedFfmpegPath() const;
    QString autoOutputMuxer() const;
    bool shouldUseInternalFileMuxer(const QString& outputTarget) const;
    bool writeVideoPayload(const QByteArray& payload);
    bool writeAudioPayload(const QByteArray& payload);
    bool writeSocketPayload(qintptr socketDescriptor, const QByteArray& payload, const char* label);
    void closeSocketDescriptor(qintptr& socketDescriptor);
    void logFfmpegMessages(bool forceInfo = false);
    bool sendAudioHeaders();
    QByteArray buildIvfStreamHeader() const;
    QByteArray buildIvfFrameHeader(int frameSize, quint64 presentationTimeUs) const;
    QByteArray buildOpusHeadPacket() const;
    QByteArray buildOpusTagsPacket() const;
    QByteArray buildOggPage(const QByteArray& packet, quint8 headerType, quint64 granulePosition);
    quint64 packetDurationInSamples(const char* sampleData, int sampleLength) const;
    bool isAv1Bitstream() const;
    static quint32 calculateOggCrc(const QByteArray& page);
    static void appendLe16(QByteArray& buffer, quint16 value);
    static void appendLe32(QByteArray& buffer, quint32 value);
    static void appendLe64(QByteArray& buffer, quint64 value);
    static bool isNetworkTarget(const QString& target);
    static QString buildDefaultCaptureName();
    static QString shellQuote(const QString& value);

    bool m_TestOnly;
    int m_VideoFormat;
    int m_Width;
    int m_Height;
    int m_FrameRate;
    BitstreamFileMuxer* m_FileMuxer;
    bool m_Ready;
    bool m_ProcessActive;
    bool m_VideoHeadersSent;
    quint64 m_SubmittedFrames;
    quint64 m_LastVideoPresentationTimeUs;
    bool m_AudioConfigValid;
    OPUS_MULTISTREAM_CONFIGURATION m_AudioConfig;
    bool m_AudioHeadersSent;
    bool m_AudioInputClosed;
    quint64 m_AudioGranulePosition;
    quint32 m_OggStreamSerial;
    quint32 m_OggPageSequence;
    QProcess* m_Ffmpeg;
    quint16 m_VideoListenPort;
    quint16 m_AudioListenPort;
    qintptr m_VideoListenDescriptor;
    qintptr m_AudioListenDescriptor;
    qintptr m_VideoSocketDescriptor;
    qintptr m_AudioSocketDescriptor;
    QQueue<VideoFramePacket> m_VideoFrameQueue;
    qsizetype m_QueuedVideoBytes;
    bool m_VideoInputClosed;
    bool m_VideoWriterStopRequested;
    bool m_VideoWriterFailed;
    bool m_VideoWriterDropLogged;
    SDL_cond* m_VideoQueueCond;
    SDL_Thread* m_VideoWriterThread;
    QQueue<AudioPacketData> m_AudioPacketQueue;
    qsizetype m_QueuedAudioBytes;
    bool m_AudioWriterStopRequested;
    bool m_AudioWriterFailed;
    bool m_AudioWriterDropLogged;
    SDL_cond* m_AudioQueueCond;
    SDL_Thread* m_AudioWriterThread;
    SDL_mutex* m_VideoLock;
    SDL_mutex* m_AudioLock;
};
