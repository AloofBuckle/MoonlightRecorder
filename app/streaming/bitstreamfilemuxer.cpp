#include "bitstreamfilemuxer.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLibrary>
#include <QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <opus.h>

namespace {

constexpr qsizetype kMaxPendingAudioBytes = 4 * 1024 * 1024;
constexpr qsizetype kMaxPendingVideoBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaxConfigProbeBytes = 1024 * 1024;

struct AvformatApi
{
    using AvformatAllocOutputContext2 = int (*)(AVFormatContext**, const AVOutputFormat*, const char*, const char*);
    using AvformatNewStream = AVStream* (*)(AVFormatContext*, const AVCodec*);
    using AvioOpen2 = int (*)(AVIOContext**, const char*, int, const AVIOInterruptCB*, AVDictionary**);
    using AvioClosep = int (*)(AVIOContext**);
    using AvformatWriteHeader = int (*)(AVFormatContext*, AVDictionary**);
    using AvInterleavedWriteFrame = int (*)(AVFormatContext*, AVPacket*);
    using AvWriteTrailer = int (*)(AVFormatContext*);
    using AvformatFreeContext = void (*)(AVFormatContext*);
    using AvformatNetworkInit = int (*)();

    bool load()
    {
        if (loaded) {
            return allocOutputContext2 != nullptr;
        }

        loaded = true;

        QStringList candidates = {
            QStringLiteral("avformat-62"),
            QStringLiteral("avformat-lav-62"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/avformat-62.dll"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/avformat-lav-62.dll"),
            QStringLiteral("C:/Program Files/TeamSpeak/avformat-62.dll"),
            QStringLiteral("C:/Program Files/MPC-HC/LAVFilters64/avformat-lav-62.dll"),
        };

        for (const QString& candidate : candidates) {
            library.setFileName(candidate);
            if (library.load()) {
                break;
            }
        }

        if (!library.isLoaded()) {
            return false;
        }

        allocOutputContext2 = reinterpret_cast<AvformatAllocOutputContext2>(library.resolve("avformat_alloc_output_context2"));
        newStream = reinterpret_cast<AvformatNewStream>(library.resolve("avformat_new_stream"));
        avioOpen2 = reinterpret_cast<AvioOpen2>(library.resolve("avio_open2"));
        avioClosep = reinterpret_cast<AvioClosep>(library.resolve("avio_closep"));
        writeHeader = reinterpret_cast<AvformatWriteHeader>(library.resolve("avformat_write_header"));
        interleavedWriteFrame = reinterpret_cast<AvInterleavedWriteFrame>(library.resolve("av_interleaved_write_frame"));
        writeTrailer = reinterpret_cast<AvWriteTrailer>(library.resolve("av_write_trailer"));
        freeContext = reinterpret_cast<AvformatFreeContext>(library.resolve("avformat_free_context"));
        networkInit = reinterpret_cast<AvformatNetworkInit>(library.resolve("avformat_network_init"));

        return allocOutputContext2 != nullptr &&
               newStream != nullptr &&
               avioOpen2 != nullptr &&
               avioClosep != nullptr &&
               writeHeader != nullptr &&
               interleavedWriteFrame != nullptr &&
               writeTrailer != nullptr &&
               freeContext != nullptr &&
               networkInit != nullptr;
    }

    QLibrary library;
    bool loaded = false;
    AvformatAllocOutputContext2 allocOutputContext2 = nullptr;
    AvformatNewStream newStream = nullptr;
    AvioOpen2 avioOpen2 = nullptr;
    AvioClosep avioClosep = nullptr;
    AvformatWriteHeader writeHeader = nullptr;
    AvInterleavedWriteFrame interleavedWriteFrame = nullptr;
    AvWriteTrailer writeTrailer = nullptr;
    AvformatFreeContext freeContext = nullptr;
    AvformatNetworkInit networkInit = nullptr;
};

AvformatApi& avformatApi()
{
    static AvformatApi api;
    return api;
}

AVRational microsecondTimeBase()
{
    return AVRational{1, 1000000};
}

bool videoCodecNeedsConfig(int videoFormat)
{
    return (videoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265)) != 0;
}

}

BitstreamFileMuxer::BitstreamFileMuxer()
    : m_VideoFormat(0),
      m_Width(0),
      m_Height(0),
      m_FrameRate(0),
      m_AudioConfigValid(false),
      m_Lock(SDL_CreateMutex()),
      m_Initialized(false),
      m_HeaderWritten(false),
      m_AudioInputClosed(false),
      m_VideoEpochEstablished(false),
      m_VideoPresentationEpochUs(0),
      m_FirstVideoPresentationUs(-1),
      m_AudioEpochEstablished(false),
      m_FirstAudioRtpTimestamp(0),
      m_FirstAudioPresentationUs(0),
      m_NextAudioPtsUs(0),
      m_AudioPtsInitialized(false),
      m_PendingVideoBytes(0),
      m_PendingAudioBytes(0),
      m_PendingAudioDropLogged(false),
      m_HaveDeferredVideoPacket(false),
      m_LastVideoPacketDurationUs(0),
      m_OutputContext(nullptr),
      m_VideoStream(nullptr),
      m_AudioStream(nullptr),
      m_VideoBsf(nullptr)
{
    SDL_zero(m_AudioConfig);
}

BitstreamFileMuxer::~BitstreamFileMuxer()
{
    shutdown();
    SDL_DestroyMutex(m_Lock);
}

void BitstreamFileMuxer::setVideoConfig(int videoFormat, int width, int height, int frameRate)
{
    m_VideoFormat = videoFormat;
    m_Width = width;
    m_Height = height;
    m_FrameRate = frameRate;
}

void BitstreamFileMuxer::setAudioConfig(const OPUS_MULTISTREAM_CONFIGURATION& opusConfig)
{
    m_AudioConfig = opusConfig;
    m_AudioConfigValid = opusConfig.channelCount > 0;

    if (m_AudioConfigValid && m_Initialized && SDL_LockMutex(m_Lock) == 0) {
        createAudioStreamLocked();
        SDL_UnlockMutex(m_Lock);
    }
}

bool BitstreamFileMuxer::initialize(const QString& outputTarget)
{
    m_OutputTarget = outputTarget;
    m_Initialized = false;
    m_HeaderWritten = false;
    m_AudioInputClosed = false;
    m_VideoEpochEstablished = false;
    m_VideoPresentationEpochUs = 0;
    m_FirstVideoPresentationUs = -1;
    m_AudioEpochEstablished = false;
    m_FirstAudioRtpTimestamp = 0;
    m_FirstAudioPresentationUs = 0;
    m_NextAudioPtsUs = 0;
    m_AudioPtsInitialized = false;
    m_PendingVideoBytes = 0;
    m_PendingAudioBytes = 0;
    m_PendingAudioDropLogged = false;
    m_ConfigProbeData.clear();
    m_ExplicitCodecConfig.clear();
    m_PendingVideoPackets.clear();
    m_PendingAudioPackets.clear();
    m_DeferredVideoPacket = PendingVideoPacket{};
    m_HaveDeferredVideoPacket = false;
    m_LastVideoPacketDurationUs = 0;

    if (!loadOutputLibraries()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load libavformat runtime for local bitstream capture");
        return false;
    }

    AvformatApi& api = avformatApi();
    api.networkInit();

    const QByteArray formatName = outputFormatName().toUtf8();
    int err = api.allocOutputContext2(&m_OutputContext,
                                      nullptr,
                                      formatName.isEmpty() ? nullptr : formatName.constData(),
                                      m_OutputTarget.toUtf8().constData());
    if (err < 0 || m_OutputContext == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed creating output context for %s: %s",
                     qPrintable(m_OutputTarget),
                     qPrintable(avErrorString(err)));
        return false;
    }

    m_OutputContext->flags |= AVFMT_FLAG_AUTO_BSF;

    if (!(m_OutputContext->oformat->flags & AVFMT_NOFILE)) {
        err = api.avioOpen2(&m_OutputContext->pb,
                            m_OutputTarget.toUtf8().constData(),
                            AVIO_FLAG_WRITE,
                            nullptr,
                            nullptr);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed opening output file %s: %s",
                         qPrintable(m_OutputTarget),
                         qPrintable(avErrorString(err)));
            shutdown();
            return false;
        }
    }

    if (!initializeVideoFilter()) {
        shutdown();
        return false;
    }

    if (m_AudioConfigValid && !createAudioStreamLocked()) {
        shutdown();
        return false;
    }

    m_Initialized = true;
    return true;
}

bool BitstreamFileMuxer::shouldDropFrameBeforeCaptureStart(bool keyFrame)
{
    if (!m_Initialized || !videoCodecNeedsConfig(m_VideoFormat) || keyFrame) {
        return false;
    }

    if (SDL_LockMutex(m_Lock) != 0) {
        return false;
    }

    const bool shouldDrop = (m_VideoStream == nullptr && !m_HeaderWritten);
    SDL_UnlockMutex(m_Lock);
    return shouldDrop;
}

bool BitstreamFileMuxer::writeVideoFrame(const QByteArray& frameBuffer,
                                         quint64 presentationTimeUs,
                                         quint64 receiveTimeUs,
                                         bool keyFrame,
                                         const QByteArray& codecConfig)
{
    if (!m_Initialized || frameBuffer.isEmpty()) {
        return true;
    }

    if (SDL_LockMutex(m_Lock) != 0) {
        return false;
    }

    if (!m_VideoEpochEstablished && receiveTimeUs >= presentationTimeUs) {
        m_VideoPresentationEpochUs = static_cast<qint64>(receiveTimeUs - presentationTimeUs);
        m_VideoEpochEstablished = true;
    }
    if (m_FirstVideoPresentationUs < 0) {
        m_FirstVideoPresentationUs = static_cast<qint64>(presentationTimeUs);
    }

    if (!codecConfig.isEmpty()) {
        m_ExplicitCodecConfig = codecConfig;
    }

    if (videoCodecNeedsConfig(m_VideoFormat) && m_ConfigProbeData.size() < kMaxConfigProbeBytes) {
        const qsizetype remainingCapacity = kMaxConfigProbeBytes - m_ConfigProbeData.size();
        if (!codecConfig.isEmpty()) {
            m_ConfigProbeData.append(codecConfig.constData(),
                                     qMin<qsizetype>(codecConfig.size(), remainingCapacity));
        }

        if (m_ConfigProbeData.size() < kMaxConfigProbeBytes) {
            const qsizetype frameRemainingCapacity = kMaxConfigProbeBytes - m_ConfigProbeData.size();
            m_ConfigProbeData.append(frameBuffer.constData(),
                                     qMin<qsizetype>(frameBuffer.size(), frameRemainingCapacity));
        }
    }

    AVPacket* inputPacket = av_packet_alloc();
    if (inputPacket == nullptr) {
        SDL_UnlockMutex(m_Lock);
        return false;
    }

    int err = av_new_packet(inputPacket, frameBuffer.size());
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed allocating video packet: %s",
                     qPrintable(avErrorString(err)));
        av_packet_free(&inputPacket);
        SDL_UnlockMutex(m_Lock);
        return false;
    }

    memcpy(inputPacket->data, frameBuffer.constData(), frameBuffer.size());
    const qint64 rebasedVideoPtsUs =
            qMax<qint64>(0, static_cast<qint64>(presentationTimeUs) - m_FirstVideoPresentationUs);
    inputPacket->pts = rebasedVideoPtsUs;
    inputPacket->dts = rebasedVideoPtsUs;
    inputPacket->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;

    err = av_bsf_send_packet(m_VideoBsf, inputPacket);
    av_packet_free(&inputPacket);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed submitting video packet to bitstream filter: %s",
                     qPrintable(avErrorString(err)));
        SDL_UnlockMutex(m_Lock);
        return false;
    }

    const bool ok = writeVideoPacketsLocked();
    SDL_UnlockMutex(m_Lock);
    return ok;
}

bool BitstreamFileMuxer::writeAudioPacket(const QByteArray& packet,
                                          quint64 receiveTimeUs,
                                          quint32 rtpTimestamp)
{
    if (!m_Initialized || packet.isEmpty() || !m_AudioConfigValid) {
        return true;
    }

    const quint64 durationSamples = packetDurationInSamples(packet);

    if (SDL_LockMutex(m_Lock) != 0) {
        return false;
    }

    bool ok = true;
    if (!m_VideoEpochEstablished || !m_HeaderWritten) {
        while (!m_PendingAudioPackets.isEmpty() &&
               (m_PendingAudioBytes + packet.size()) > kMaxPendingAudioBytes) {
            m_PendingAudioBytes -= m_PendingAudioPackets.head().packet.size();
            m_PendingAudioPackets.dequeue();
            if (!m_PendingAudioDropLogged) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Dropping queued audio packets while waiting for video timestamps");
                m_PendingAudioDropLogged = true;
            }
        }

        m_PendingAudioPackets.enqueue(PendingAudioPacket{packet,
                                                         receiveTimeUs,
                                                         rtpTimestamp,
                                                         durationSamples});
        m_PendingAudioBytes += packet.size();
    }
    else {
        ok = writeAudioPacketLocked(packet, receiveTimeUs, rtpTimestamp, durationSamples);
    }

    SDL_UnlockMutex(m_Lock);
    return ok;
}

void BitstreamFileMuxer::closeAudioInput()
{
    if (SDL_LockMutex(m_Lock) == 0) {
        m_AudioInputClosed = true;
        SDL_UnlockMutex(m_Lock);
    }
}

void BitstreamFileMuxer::shutdown()
{
    if (SDL_LockMutex(m_Lock) != 0) {
        return;
    }

    if (m_VideoBsf != nullptr && m_HeaderWritten) {
        flushVideoFilterLocked();
    }

    if (m_OutputContext != nullptr && m_HeaderWritten) {
        avformatApi().writeTrailer(m_OutputContext);
    }

    if (m_VideoBsf != nullptr) {
        av_bsf_free(&m_VideoBsf);
    }

    if (m_OutputContext != nullptr) {
        if (m_OutputContext->pb != nullptr && !(m_OutputContext->oformat->flags & AVFMT_NOFILE)) {
            avformatApi().avioClosep(&m_OutputContext->pb);
        }
        avformatApi().freeContext(m_OutputContext);
        m_OutputContext = nullptr;
    }

    m_VideoStream = nullptr;
    m_AudioStream = nullptr;
    m_Initialized = false;
    m_HeaderWritten = false;
    m_AudioInputClosed = true;
    m_VideoEpochEstablished = false;
    m_FirstVideoPresentationUs = -1;
    m_AudioEpochEstablished = false;
    m_FirstAudioRtpTimestamp = 0;
    m_FirstAudioPresentationUs = 0;
    m_NextAudioPtsUs = 0;
    m_AudioPtsInitialized = false;
    m_ConfigProbeData.clear();
    m_ExplicitCodecConfig.clear();
    m_PendingVideoPackets.clear();
    m_PendingVideoBytes = 0;
    m_PendingAudioPackets.clear();
    m_PendingAudioBytes = 0;
    m_DeferredVideoPacket = PendingVideoPacket{};
    m_HaveDeferredVideoPacket = false;
    m_LastVideoPacketDurationUs = 0;

    SDL_UnlockMutex(m_Lock);
}

bool BitstreamFileMuxer::loadOutputLibraries()
{
    return avformatApi().load();
}

bool BitstreamFileMuxer::initializeVideoFilter()
{
    const AVCodecID codecId = static_cast<AVCodecID>(codecIdForVideoFormat(m_VideoFormat));
    if (codecId == AV_CODEC_ID_NONE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unsupported video codec for internal capture: 0x%x",
                     m_VideoFormat);
        return false;
    }

    const QByteArray filterChain =
            (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0 ?
                QByteArrayLiteral("extract_extradata,hevc_metadata=aud=insert:tick_rate=0/1") :
                QByteArrayLiteral("extract_extradata");

    int err = av_bsf_list_parse_str(filterChain.constData(), &m_VideoBsf);
    if (err < 0 || m_VideoBsf == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed creating video bitstream filter chain '%s': %s",
                     filterChain.constData(),
                     qPrintable(avErrorString(err)));
        return false;
    }

    m_VideoBsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    m_VideoBsf->par_in->codec_id = codecId;
    m_VideoBsf->par_in->width = m_Width;
    m_VideoBsf->par_in->height = m_Height;
    m_VideoBsf->time_base_in = microsecondTimeBase();

    err = av_bsf_init(m_VideoBsf);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed initializing video bitstream filter: %s",
                     qPrintable(avErrorString(err)));
        av_bsf_free(&m_VideoBsf);
        return false;
    }

    return true;
}

bool BitstreamFileMuxer::createAudioStreamLocked()
{
    if (!m_AudioConfigValid || m_AudioStream != nullptr) {
        return true;
    }

    m_AudioStream = avformatApi().newStream(m_OutputContext, nullptr);
    if (m_AudioStream == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed creating audio stream for local capture");
        return false;
    }

    m_AudioStream->time_base = AVRational{1, qMax(1, m_AudioConfig.sampleRate)};

    AVCodecParameters* codecParameters = m_AudioStream->codecpar;
    codecParameters->codec_type = AVMEDIA_TYPE_AUDIO;
    codecParameters->codec_id = AV_CODEC_ID_OPUS;
    codecParameters->sample_rate = m_AudioConfig.sampleRate;
    av_channel_layout_default(&codecParameters->ch_layout, m_AudioConfig.channelCount);

    const QByteArray opusHead = buildOpusHeadPacket();
    codecParameters->extradata =
            static_cast<uint8_t*>(av_mallocz(opusHead.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (codecParameters->extradata == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed allocating Opus extradata for local capture");
        return false;
    }

    memcpy(codecParameters->extradata, opusHead.constData(), opusHead.size());
    codecParameters->extradata_size = opusHead.size();

    return true;
}

bool BitstreamFileMuxer::createVideoStreamLocked(const QByteArray& fallbackExtradata)
{
    if (m_VideoStream != nullptr) {
        return true;
    }

    m_VideoStream = avformatApi().newStream(m_OutputContext, nullptr);
    if (m_VideoStream == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed creating video stream for local capture");
        return false;
    }

    m_VideoStream->time_base = microsecondTimeBase();
    // These capture files are written from reordered packet timestamps and may
    // be effectively VFR even when the negotiated stream advertises a higher
    // display refresh rate (for example 240 Hz). Advertising a fixed frame rate
    // here causes Matroska/HEVC files to carry misleading default durations that
    // VLC's seeking logic handles poorly. Let timestamps drive playback/seeking.
    m_VideoStream->avg_frame_rate = AVRational{0, 1};
    m_VideoStream->r_frame_rate = AVRational{0, 1};

    int err = avcodec_parameters_copy(m_VideoStream->codecpar, m_VideoBsf->par_out);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed copying video codec parameters: %s",
                     qPrintable(avErrorString(err)));
        return false;
    }

    m_VideoStream->codecpar->codec_tag = 0;
    m_VideoStream->codecpar->width = m_Width;
    m_VideoStream->codecpar->height = m_Height;
    if (m_VideoStream->codecpar->extradata_size == 0 && !fallbackExtradata.isEmpty()) {
        m_VideoStream->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(fallbackExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (m_VideoStream->codecpar->extradata != nullptr) {
            memcpy(m_VideoStream->codecpar->extradata,
                   fallbackExtradata.constData(),
                   fallbackExtradata.size());
            m_VideoStream->codecpar->extradata_size = fallbackExtradata.size();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Local capture injected fallback codec config: %d bytes",
                        fallbackExtradata.size());
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Local capture video stream prepared: codec=%d extradata=%d bytes",
                m_VideoStream->codecpar->codec_id,
                m_VideoStream->codecpar->extradata_size);
    return true;
}

bool BitstreamFileMuxer::ensureHeaderWrittenLocked()
{
    if (m_HeaderWritten) {
        return true;
    }

    if (m_OutputContext == nullptr || m_VideoStream == nullptr) {
        return false;
    }

    int err = avformatApi().writeHeader(m_OutputContext, nullptr);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed writing local capture header: %s",
                     qPrintable(avErrorString(err)));
        return false;
    }

    m_HeaderWritten = true;
    return true;
}

bool BitstreamFileMuxer::flushPendingVideoPacketsLocked()
{
    while (!m_PendingVideoPackets.isEmpty()) {
        const PendingVideoPacket packet = m_PendingVideoPackets.dequeue();
        m_PendingVideoBytes -= packet.packet.size();
        if (!queueOrWriteVideoPacketLocked(packet)) {
            return false;
        }
    }

    return true;
}

bool BitstreamFileMuxer::writeVideoPacketLocked(const PendingVideoPacket& packet, qint64 durationUs)
{
    AVPacket* outputPacket = av_packet_alloc();
    if (outputPacket == nullptr) {
        return false;
    }

    const int err = av_new_packet(outputPacket, packet.packet.size());
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed allocating output video packet: %s",
                     qPrintable(avErrorString(err)));
        av_packet_free(&outputPacket);
        return false;
    }

    memcpy(outputPacket->data, packet.packet.constData(), packet.packet.size());
    outputPacket->pts = packet.ptsUs;
    outputPacket->dts = packet.dtsUs;
    outputPacket->duration = qMax<qint64>(0, durationUs);
    outputPacket->flags = packet.flags;
    outputPacket->stream_index = m_VideoStream->index;
    av_packet_rescale_ts(outputPacket, microsecondTimeBase(), m_VideoStream->time_base);

    const int writeErr = avformatApi().interleavedWriteFrame(m_OutputContext, outputPacket);
    av_packet_free(&outputPacket);
    if (writeErr < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed writing video packet to local capture: %s",
                     qPrintable(avErrorString(writeErr)));
        return false;
    }

    return true;
}

bool BitstreamFileMuxer::queueOrWriteVideoPacketLocked(const PendingVideoPacket& packet)
{
    if (!m_HaveDeferredVideoPacket) {
        m_DeferredVideoPacket = packet;
        m_HaveDeferredVideoPacket = true;
        return true;
    }

    qint64 durationUs = packet.ptsUs - m_DeferredVideoPacket.ptsUs;
    if (durationUs <= 0) {
        durationUs = m_LastVideoPacketDurationUs;
    }
    if (durationUs <= 0 && m_FrameRate > 0) {
        durationUs = 1000000 / m_FrameRate;
    }
    if (durationUs <= 0) {
        durationUs = 1;
    }

    if (!writeVideoPacketLocked(m_DeferredVideoPacket, durationUs)) {
        return false;
    }

    m_LastVideoPacketDurationUs = durationUs;
    m_DeferredVideoPacket = packet;
    return true;
}

bool BitstreamFileMuxer::flushDeferredVideoPacketLocked()
{
    if (!m_HaveDeferredVideoPacket) {
        return true;
    }

    qint64 durationUs = m_LastVideoPacketDurationUs;
    if (durationUs <= 0 && m_FrameRate > 0) {
        durationUs = 1000000 / m_FrameRate;
    }
    if (durationUs <= 0) {
        durationUs = 1;
    }

    const bool ok = writeVideoPacketLocked(m_DeferredVideoPacket, durationUs);
    m_DeferredVideoPacket = PendingVideoPacket{};
    m_HaveDeferredVideoPacket = false;
    return ok;
}

bool BitstreamFileMuxer::flushPendingAudioPacketsLocked()
{
    while (!m_PendingAudioPackets.isEmpty()) {
        const PendingAudioPacket packet = m_PendingAudioPackets.dequeue();
        m_PendingAudioBytes -= packet.packet.size();
        if (!writeAudioPacketLocked(packet.packet,
                                    packet.receiveTimeUs,
                                    packet.rtpTimestamp,
                                    packet.durationSamples)) {
            return false;
        }
    }

    return true;
}

bool BitstreamFileMuxer::queuePendingVideoPacketLocked(const AVPacket* packet)
{
    if (packet == nullptr || packet->data == nullptr || packet->size <= 0) {
        return true;
    }

    while (!m_PendingVideoPackets.isEmpty() &&
           (m_PendingVideoBytes + packet->size) > kMaxPendingVideoBytes) {
        m_PendingVideoBytes -= m_PendingVideoPackets.head().packet.size();
        m_PendingVideoPackets.dequeue();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Dropping queued video packets while waiting for codec configuration");
    }

    PendingVideoPacket pendingPacket;
    pendingPacket.packet = QByteArray(reinterpret_cast<const char*>(packet->data), packet->size);
    pendingPacket.ptsUs = packet->pts;
    pendingPacket.dtsUs = packet->dts;
    pendingPacket.durationUs = packet->duration;
    pendingPacket.flags = packet->flags;
    m_PendingVideoPackets.enqueue(pendingPacket);
    m_PendingVideoBytes += pendingPacket.packet.size();
    return true;
}

bool BitstreamFileMuxer::writeVideoPacketsLocked()
{
    while (true) {
        AVPacket* filteredPacket = av_packet_alloc();
        if (filteredPacket == nullptr) {
            return false;
        }

        int err = av_bsf_receive_packet(m_VideoBsf, filteredPacket);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            av_packet_free(&filteredPacket);
            return true;
        }
        else if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed receiving filtered video packet: %s",
                         qPrintable(avErrorString(err)));
            av_packet_free(&filteredPacket);
            return false;
        }

        QByteArray fallbackExtradata;
        const bool haveCodecConfig =
                !videoCodecNeedsConfig(m_VideoFormat) ||
                m_VideoBsf->par_out->extradata_size > 0 ||
                !(fallbackExtradata = m_ExplicitCodecConfig).isEmpty() ||
                !(fallbackExtradata = buildCodecConfigExtradata()).isEmpty();

        if (m_VideoStream == nullptr) {
            if (!haveCodecConfig) {
                const bool queued = queuePendingVideoPacketLocked(filteredPacket);
                av_packet_free(&filteredPacket);
                if (!queued) {
                    return false;
                }
                continue;
            }

            if (!createVideoStreamLocked(fallbackExtradata)) {
                av_packet_free(&filteredPacket);
                return false;
            }
        }

        if (!ensureHeaderWrittenLocked()) {
            av_packet_free(&filteredPacket);
            return false;
        }

        if (!flushPendingVideoPacketsLocked()) {
            av_packet_free(&filteredPacket);
            return false;
        }

        if (!flushPendingAudioPacketsLocked()) {
            av_packet_free(&filteredPacket);
            return false;
        }

        PendingVideoPacket pendingPacket;
        pendingPacket.packet = QByteArray(reinterpret_cast<const char*>(filteredPacket->data),
                                          filteredPacket->size);
        pendingPacket.ptsUs = filteredPacket->pts;
        pendingPacket.dtsUs = filteredPacket->dts;
        pendingPacket.durationUs = filteredPacket->duration;
        pendingPacket.flags = filteredPacket->flags;

        av_packet_free(&filteredPacket);
        if (!queueOrWriteVideoPacketLocked(pendingPacket)) {
            return false;
        }
    }
}

bool BitstreamFileMuxer::writeAudioPacketLocked(const QByteArray& packet,
                                                quint64 receiveTimeUs,
                                                quint32 rtpTimestamp,
                                                quint64 durationSamples)
{
    if (m_AudioStream == nullptr || m_AudioInputClosed) {
        return true;
    }

    const qint64 audioPresentationUs = mapAudioPresentationUs(receiveTimeUs, rtpTimestamp);
    const qint64 packetDurationUs =
            av_rescale_q(static_cast<qint64>(durationSamples),
                         AVRational{1, qMax(1, m_AudioConfig.sampleRate)},
                         microsecondTimeBase());
    qint64 packetPtsUs = audioPresentationUs;

    if (!m_AudioPtsInitialized) {
        packetPtsUs = qMax<qint64>(0, packetPtsUs);
        m_NextAudioPtsUs = packetPtsUs;
        m_AudioPtsInitialized = true;
    }

    packetPtsUs = qMax(packetPtsUs, m_NextAudioPtsUs);
    m_NextAudioPtsUs = packetPtsUs + qMax<qint64>(1, packetDurationUs);

    AVPacket* audioPacket = av_packet_alloc();
    if (audioPacket == nullptr) {
        return false;
    }

    int err = av_new_packet(audioPacket, packet.size());
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed allocating audio packet: %s",
                     qPrintable(avErrorString(err)));
        av_packet_free(&audioPacket);
        return false;
    }

    memcpy(audioPacket->data, packet.constData(), packet.size());
    audioPacket->pts = packetPtsUs;
    audioPacket->dts = packetPtsUs;
    audioPacket->duration = qMax<qint64>(1, packetDurationUs);
    audioPacket->stream_index = m_AudioStream->index;
    av_packet_rescale_ts(audioPacket,
                         microsecondTimeBase(),
                         m_AudioStream->time_base);

    err = avformatApi().interleavedWriteFrame(m_OutputContext, audioPacket);
    av_packet_free(&audioPacket);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed writing audio packet to local capture: %s",
                     qPrintable(avErrorString(err)));
        return false;
    }

    return true;
}

bool BitstreamFileMuxer::flushVideoFilterLocked()
{
    if (m_VideoBsf == nullptr) {
        return true;
    }

    int err = av_bsf_send_packet(m_VideoBsf, nullptr);
    if (err < 0 && err != AVERROR_EOF) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed flushing video bitstream filter: %s",
                    qPrintable(avErrorString(err)));
        return false;
    }

    if (!writeVideoPacketsLocked()) {
        return false;
    }

    return flushDeferredVideoPacketLocked();
}

quint64 BitstreamFileMuxer::packetDurationInSamples(const QByteArray& packet) const
{
    const int samples = opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()),
                                                   packet.size(),
                                                   m_AudioConfig.sampleRate);
    if (samples > 0) {
        return static_cast<quint64>(samples);
    }

    return static_cast<quint64>(qMax(0, m_AudioConfig.samplesPerFrame));
}

qint64 BitstreamFileMuxer::mapAudioReceiveTimeToPresentationUs(quint64 receiveTimeUs) const
{
    if (!m_VideoEpochEstablished || receiveTimeUs <= static_cast<quint64>(m_VideoPresentationEpochUs)) {
        return 0;
    }

    qint64 mappedUs = static_cast<qint64>(receiveTimeUs) - m_VideoPresentationEpochUs;
    if (m_FirstVideoPresentationUs > 0) {
        mappedUs -= m_FirstVideoPresentationUs;
    }

    return qMax<qint64>(0, mappedUs);
}

qint64 BitstreamFileMuxer::mapAudioPresentationUs(quint64 receiveTimeUs, quint32 rtpTimestamp)
{
    const qint64 receiveMappedUs = mapAudioReceiveTimeToPresentationUs(receiveTimeUs);

    if (!m_AudioEpochEstablished) {
        m_AudioEpochEstablished = true;
        m_FirstAudioRtpTimestamp = rtpTimestamp;
        m_FirstAudioPresentationUs = qMax<qint64>(0, receiveMappedUs);
        return m_FirstAudioPresentationUs;
    }

    const quint32 rtpDelta = rtpTimestamp - m_FirstAudioRtpTimestamp;
    const qint64 rtpMappedUs =
            m_FirstAudioPresentationUs + static_cast<qint64>(rtpDelta) * 1000;

    if (receiveMappedUs <= 0) {
        return rtpMappedUs;
    }

    // Use the RTP clock as the primary timeline once anchored, while preserving
    // monotonicity if the receive-based estimate is somehow ahead.
    return qMax(rtpMappedUs, qMin(receiveMappedUs, m_NextAudioPtsUs));
}

QByteArray BitstreamFileMuxer::buildOpusHeadPacket() const
{
    QByteArray packet;
    packet.reserve(32);
    packet.append("OpusHead", 8);
    packet.append(char(1));
    packet.append(char(m_AudioConfig.channelCount));
    appendLe16(packet, 0);
    appendLe32(packet, static_cast<quint32>(m_AudioConfig.sampleRate));
    appendLe16(packet, 0);

    if (m_AudioConfig.channelCount <= 2) {
        packet.append(char(0));
    }
    else {
        packet.append(char(1));
        packet.append(char(m_AudioConfig.streams));
        packet.append(char(m_AudioConfig.coupledStreams));
        packet.append(reinterpret_cast<const char*>(m_AudioConfig.mapping),
                      m_AudioConfig.channelCount);
    }

    return packet;
}

QString BitstreamFileMuxer::outputFormatName() const
{
    const QString suffix = QFileInfo(m_OutputTarget).suffix().toLower();
    if (suffix == "mkv") {
        return QStringLiteral("matroska");
    }
    else if (suffix == "webm") {
        return QStringLiteral("webm");
    }
    else if (suffix == "mp4" || suffix == "m4v") {
        return QStringLiteral("mp4");
    }
    else if (suffix == "mov") {
        return QStringLiteral("mov");
    }
    else if (suffix == "ts" || suffix == "m2ts") {
        return QStringLiteral("mpegts");
    }
    else if (suffix == "flv") {
        return QStringLiteral("flv");
    }

    return QStringLiteral("matroska");
}

QByteArray BitstreamFileMuxer::buildCodecConfigExtradata() const
{
    if (m_ConfigProbeData.isEmpty()) {
        return {};
    }

    QList<QByteArray> parameterSets;
    const QByteArray startCode("\x00\x00\x00\x01", 4);
    const char* data = m_ConfigProbeData.constData();
    const int size = m_ConfigProbeData.size();

    auto findStartCode = [&](int offset) -> int {
        for (int i = offset; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1) {
                    return i;
                }
                if ((i + 3) < size && data[i + 2] == 0 && data[i + 3] == 1) {
                    return i;
                }
            }
        }

        return -1;
    };

    int nalStart = findStartCode(0);
    while (nalStart >= 0) {
        int startCodeLength = (data[nalStart + 2] == 1) ? 3 : 4;
        const int payloadStart = nalStart + startCodeLength;
        int nextNalStart = findStartCode(payloadStart);
        if (nextNalStart < 0) {
            nextNalStart = size;
        }

        if (payloadStart < nextNalStart) {
            const int payloadSize = nextNalStart - payloadStart;
            int nalType = -1;
            if ((m_VideoFormat & VIDEO_FORMAT_MASK_H264) != 0) {
                nalType = data[payloadStart] & 0x1F;
                if (nalType == 7 || nalType == 8) {
                    parameterSets.append(startCode + QByteArray(data + payloadStart, payloadSize));
                }
            }
            else if ((m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0 && payloadSize >= 2) {
                nalType = (static_cast<unsigned char>(data[payloadStart]) >> 1) & 0x3F;
                if (nalType == 32 || nalType == 33 || nalType == 34) {
                    parameterSets.append(startCode + QByteArray(data + payloadStart, payloadSize));
                }
            }
        }

        nalStart = nextNalStart;
    }

    QByteArray extradata;
    for (const QByteArray& nal : parameterSets) {
        extradata.append(nal);
    }

    return extradata;
}

int BitstreamFileMuxer::codecIdForVideoFormat(int videoFormat)
{
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return AV_CODEC_ID_H264;
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return AV_CODEC_ID_HEVC;
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        return AV_CODEC_ID_AV1;
    }

    return AV_CODEC_ID_NONE;
}

QString BitstreamFileMuxer::avErrorString(int errorCode)
{
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errorBuffer, sizeof(errorBuffer));
    return QString::fromUtf8(errorBuffer);
}

void BitstreamFileMuxer::appendLe16(QByteArray& buffer, quint16 value)
{
    buffer.append(char(value & 0xFF));
    buffer.append(char((value >> 8) & 0xFF));
}

void BitstreamFileMuxer::appendLe32(QByteArray& buffer, quint32 value)
{
    buffer.append(char(value & 0xFF));
    buffer.append(char((value >> 8) & 0xFF));
    buffer.append(char((value >> 16) & 0xFF));
    buffer.append(char((value >> 24) & 0xFF));
}
