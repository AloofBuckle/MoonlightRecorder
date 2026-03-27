#include "bitstreammuxer.h"

#include "bitstreamfilemuxer.h"
#include "settings/streamingpreferences.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <climits>
#include <cstring>

#include <opus.h>

#ifdef Q_OS_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

constexpr quint8 kOggHeaderTypeContinuation = 0x01;
constexpr quint8 kOggHeaderTypeBOS = 0x02;
constexpr quint8 kOggHeaderTypeEOS = 0x04;
constexpr qsizetype kMaxQueuedVideoBytes = 128 * 1024 * 1024;
constexpr qsizetype kMaxQueuedAudioBytes = 4 * 1024 * 1024;

BitstreamMuxer::BitstreamMuxer()
    : m_TestOnly(false),
      m_VideoFormat(0),
      m_Width(0),
      m_Height(0),
      m_FrameRate(0),
      m_FileMuxer(nullptr),
      m_Ready(false),
      m_ProcessActive(false),
      m_VideoHeadersSent(false),
      m_SubmittedFrames(0),
      m_LastVideoPresentationTimeUs(0),
      m_AudioConfigValid(false),
      m_AudioHeadersSent(false),
      m_AudioInputClosed(false),
      m_AudioGranulePosition(0),
      m_OggStreamSerial(QRandomGenerator::global()->generate()),
      m_OggPageSequence(0),
      m_Ffmpeg(nullptr),
      m_VideoListenPort(0),
      m_AudioListenPort(0),
      m_VideoListenDescriptor(-1),
      m_AudioListenDescriptor(-1),
      m_VideoSocketDescriptor(-1),
      m_AudioSocketDescriptor(-1),
      m_QueuedVideoBytes(0),
      m_VideoInputClosed(false),
      m_VideoWriterStopRequested(false),
      m_VideoWriterFailed(false),
      m_VideoWriterDropLogged(false),
      m_VideoQueueCond(SDL_CreateCond()),
      m_VideoWriterThread(nullptr),
      m_QueuedAudioBytes(0),
      m_AudioWriterStopRequested(false),
      m_AudioWriterFailed(false),
      m_AudioWriterDropLogged(false),
      m_AudioQueueCond(SDL_CreateCond()),
      m_AudioWriterThread(nullptr),
      m_VideoLock(SDL_CreateMutex()),
      m_AudioLock(SDL_CreateMutex())
{
    SDL_zero(m_AudioConfig);
}

BitstreamMuxer::~BitstreamMuxer()
{
    shutdown();
    SDL_DestroyCond(m_VideoQueueCond);
    SDL_DestroyCond(m_AudioQueueCond);
    SDL_DestroyMutex(m_VideoLock);
    SDL_DestroyMutex(m_AudioLock);
}

void BitstreamMuxer::setVideoConfig(int videoFormat, int width, int height, int frameRate)
{
    m_VideoFormat = videoFormat;
    m_Width = width;
    m_Height = height;
    m_FrameRate = frameRate;

    if (m_FileMuxer != nullptr) {
        m_FileMuxer->setVideoConfig(videoFormat, width, height, frameRate);
    }
}

void BitstreamMuxer::setAudioConfig(const OPUS_MULTISTREAM_CONFIGURATION& opusConfig)
{
    m_AudioConfig = opusConfig;
    m_AudioConfigValid = opusConfig.channelCount > 0;

    if (m_FileMuxer != nullptr) {
        m_FileMuxer->setAudioConfig(opusConfig);
    }
}

bool BitstreamMuxer::initialize(bool testOnly)
{
    m_TestOnly = testOnly;
    m_Ready = false;
    m_ProcessActive = false;
    m_VideoHeadersSent = false;
    m_SubmittedFrames = 0;
    m_LastVideoPresentationTimeUs = 0;
    m_QueuedVideoBytes = 0;
    m_VideoFrameQueue.clear();
    m_VideoInputClosed = false;
    m_VideoWriterStopRequested = false;
    m_VideoWriterFailed = false;
    m_VideoWriterDropLogged = false;
    m_AudioHeadersSent = false;
    m_AudioInputClosed = false;
    m_AudioGranulePosition = 0;
    m_OggPageSequence = 0;
    m_OggStreamSerial = QRandomGenerator::global()->generate();
    m_QueuedAudioBytes = 0;
    m_AudioPacketQueue.clear();
    m_AudioWriterStopRequested = false;
    m_AudioWriterFailed = false;
    m_AudioWriterDropLogged = false;

    if (m_TestOnly) {
        m_Ready = true;
        return true;
    }

    delete m_FileMuxer;
    m_FileMuxer = nullptr;

    const QString outputTarget = getResolvedOutputTarget();
    if (shouldUseInternalFileMuxer(outputTarget)) {
        m_FileMuxer = new BitstreamFileMuxer();
        m_FileMuxer->setVideoConfig(m_VideoFormat, m_Width, m_Height, m_FrameRate);
        if (m_AudioConfigValid) {
            m_FileMuxer->setAudioConfig(m_AudioConfig);
        }

        if (!m_FileMuxer->initialize(outputTarget)) {
            delete m_FileMuxer;
            m_FileMuxer = nullptr;
            return false;
        }

        m_Ready = true;
        return true;
    }

    if (!openFeedServers()) {
        return false;
    }

    if (!openFfmpegProcess()) {
        shutdown();
        return false;
    }

    if (!waitForFeedConnections()) {
        shutdown();
        return false;
    }

    if (!startVideoWriterThread()) {
        shutdown();
        return false;
    }

    if (m_AudioConfigValid && !startAudioWriterThread()) {
        shutdown();
        return false;
    }

    m_Ready = true;
    logFfmpegMessages(true);
    return true;
}

bool BitstreamMuxer::writeVideoFrame(const QByteArray& frameBuffer,
                                     quint64 presentationTimeUs,
                                     quint64 receiveTimeUs,
                                     bool keyFrame,
                                     const QByteArray& codecConfig)
{
    if (m_TestOnly || !m_Ready) {
        return true;
    }

    if (m_FileMuxer != nullptr) {
        return m_FileMuxer->writeVideoFrame(frameBuffer,
                                            presentationTimeUs,
                                            receiveTimeUs,
                                            keyFrame,
                                            codecConfig);
    }

    if (SDL_LockMutex(m_VideoLock) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Bitstream video mutex lock failed: %s",
                     SDL_GetError());
        return false;
    }

    if (m_VideoInputClosed || m_VideoWriterFailed) {
        SDL_UnlockMutex(m_VideoLock);
        return true;
    }

    while (!m_VideoFrameQueue.isEmpty() &&
           (m_QueuedVideoBytes + frameBuffer.size()) > kMaxQueuedVideoBytes) {
        m_QueuedVideoBytes -= m_VideoFrameQueue.head().frameBuffer.size();
        m_VideoFrameQueue.dequeue();
        if (!m_VideoWriterDropLogged) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dropping queued video frames because FFmpeg video sink is lagging");
            m_VideoWriterDropLogged = true;
        }
    }

    m_VideoFrameQueue.enqueue(VideoFramePacket{frameBuffer, presentationTimeUs, receiveTimeUs, keyFrame});
    m_QueuedVideoBytes += frameBuffer.size();
    SDL_CondSignal(m_VideoQueueCond);

    SDL_UnlockMutex(m_VideoLock);

    return true;
}

bool BitstreamMuxer::shouldDropFrameBeforeCaptureStart(bool keyFrame)
{
    if (m_TestOnly || !m_Ready) {
        return false;
    }

    if (m_FileMuxer != nullptr) {
        return m_FileMuxer->shouldDropFrameBeforeCaptureStart(keyFrame);
    }

    return false;
}

bool BitstreamMuxer::writeAudioPacket(const char* sampleData,
                                      int sampleLength,
                                      quint64 receiveTimeUs,
                                      quint32 rtpTimestamp)
{
    if (m_TestOnly || !m_Ready || !m_AudioConfigValid) {
        return true;
    }

    if (m_FileMuxer != nullptr) {
        return m_FileMuxer->writeAudioPacket(QByteArray(sampleData, sampleLength),
                                             receiveTimeUs,
                                             rtpTimestamp);
    }

    if (sampleData == nullptr || sampleLength <= 0) {
        return true;
    }

    if (SDL_LockMutex(m_AudioLock) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Bitstream audio mutex lock failed: %s",
                     SDL_GetError());
        return false;
    }

    if (m_AudioInputClosed || m_AudioWriterFailed) {
        SDL_UnlockMutex(m_AudioLock);
        return true;
    }

    const QByteArray packet(sampleData, sampleLength);
    while (!m_AudioPacketQueue.isEmpty() &&
           (m_QueuedAudioBytes + packet.size()) > kMaxQueuedAudioBytes) {
        m_QueuedAudioBytes -= m_AudioPacketQueue.head().packet.size();
        m_AudioPacketQueue.dequeue();
        if (!m_AudioWriterDropLogged) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dropping queued audio packets because FFmpeg audio sink is lagging");
            m_AudioWriterDropLogged = true;
        }
    }

    m_AudioPacketQueue.enqueue(AudioPacketData{packet, receiveTimeUs, rtpTimestamp});
    m_QueuedAudioBytes += packet.size();
    SDL_CondSignal(m_AudioQueueCond);

    SDL_UnlockMutex(m_AudioLock);

    return true;
}

void BitstreamMuxer::closeAudioInput()
{
    if (m_FileMuxer != nullptr) {
        m_FileMuxer->closeAudioInput();
        return;
    }

    if (m_TestOnly || m_AudioInputClosed) {
        return;
    }

    if (SDL_LockMutex(m_AudioLock) != 0) {
        return;
    }

    m_AudioInputClosed = true;
    SDL_CondBroadcast(m_AudioQueueCond);

    SDL_UnlockMutex(m_AudioLock);

    stopAudioWriterThread(false);
    closeSocketDescriptor(m_AudioSocketDescriptor);
    closeSocketDescriptor(m_AudioListenDescriptor);
}

void BitstreamMuxer::shutdown()
{
    m_Ready = false;
    m_ProcessActive = false;

    if (m_FileMuxer != nullptr) {
        m_FileMuxer->shutdown();
        delete m_FileMuxer;
        m_FileMuxer = nullptr;
    }

    stopVideoWriterThread(false);
    stopAudioWriterThread(false);

    if (SDL_LockMutex(m_AudioLock) == 0) {
        closeSocketDescriptor(m_AudioSocketDescriptor);
        m_AudioInputClosed = true;
        m_AudioPacketQueue.clear();
        m_QueuedAudioBytes = 0;
        SDL_UnlockMutex(m_AudioLock);
    }

    if (SDL_LockMutex(m_VideoLock) == 0) {
        closeSocketDescriptor(m_VideoSocketDescriptor);
        m_VideoInputClosed = true;
        m_VideoFrameQueue.clear();
        m_QueuedVideoBytes = 0;
        SDL_UnlockMutex(m_VideoLock);
    }

    closeSocketDescriptor(m_VideoListenDescriptor);
    closeSocketDescriptor(m_AudioListenDescriptor);
    m_VideoListenPort = 0;
    m_AudioListenPort = 0;

    closeFfmpegProcess();
}

QString BitstreamMuxer::getResolvedOutputTarget() const
{
    const StreamingPreferences* prefs = StreamingPreferences::get();
    QString target = prefs->bitstreamOutputTarget.trimmed();
    if (target.isEmpty()) {
        return QString();
    }

    if (isNetworkTarget(target)) {
        return target;
    }

    QFileInfo targetInfo(target);
    if (target.endsWith('/') || target.endsWith('\\') || (targetInfo.exists() && targetInfo.isDir())) {
        QDir outputDir(targetInfo.exists() && targetInfo.isDir() ? targetInfo.absoluteFilePath()
                                                                 : QDir::cleanPath(target));
        QString resolvedTarget = outputDir.filePath(buildDefaultCaptureName());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Bitstream output directory detected, saving to: %s",
                    qPrintable(resolvedTarget));
        return resolvedTarget;
    }

    if (targetInfo.suffix().isEmpty()) {
        QString resolvedTarget =
                target + "." + StreamingPreferences::recordingContainerExtension(prefs->recordingContainer);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Bitstream output had no file extension, saving to: %s",
                    qPrintable(resolvedTarget));
        return resolvedTarget;
    }

    return target;
}

bool BitstreamMuxer::openFeedServers()
{
    closeSocketDescriptor(m_VideoListenDescriptor);
    closeSocketDescriptor(m_AudioListenDescriptor);
    m_VideoListenPort = 0;
    m_AudioListenPort = 0;

    if (!createFeedListener(m_VideoListenDescriptor, m_VideoListenPort, "video")) {
        return false;
    }

    if (m_AudioConfigValid) {
        if (!createFeedListener(m_AudioListenDescriptor, m_AudioListenPort, "audio")) {
            return false;
        }
    }

    return true;
}

bool BitstreamMuxer::waitForFeedConnections()
{
    if (!acceptFeedConnection(m_VideoListenDescriptor,
                              10000,
                              m_VideoSocketDescriptor,
                              "video")) {
        return false;
    }

    closeSocketDescriptor(m_VideoListenDescriptor);

    if (m_AudioConfigValid && m_AudioListenDescriptor != -1) {
        acceptAudioConnection(0);
    }

    return true;
}

bool BitstreamMuxer::acceptAudioConnection(int timeoutMs)
{
    if (m_AudioSocketDescriptor != -1) {
        return true;
    }

    if (!m_AudioConfigValid || m_AudioListenDescriptor == -1) {
        return false;
    }

    if (!acceptFeedConnection(m_AudioListenDescriptor,
                              timeoutMs,
                              m_AudioSocketDescriptor,
                              "audio")) {
        return false;
    }

    closeSocketDescriptor(m_AudioListenDescriptor);
    return true;
}

bool BitstreamMuxer::createFeedListener(qintptr& listenDescriptor,
                                        quint16& listenPort,
                                        const char* label)
{
    closeSocketDescriptor(listenDescriptor);
    listenPort = 0;

#ifdef Q_OS_WIN32
    SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (rawSocket == INVALID_SOCKET) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create local FFmpeg %s feed socket: %d",
                     label,
                     WSAGetLastError());
        return false;
    }
#else
    const int rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (rawSocket < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create local FFmpeg %s feed socket: %s",
                     label,
                     strerror(errno));
        return false;
    }
#endif

    listenDescriptor = static_cast<qintptr>(rawSocket);

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    const int reuseAddr = 1;
    setsockopt(rawSocket,
               SOL_SOCKET,
               SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddr),
               sizeof(reuseAddr));

    if (::bind(rawSocket,
               reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
#ifdef Q_OS_WIN32
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to bind local FFmpeg %s feed socket: %d",
                     label,
                     WSAGetLastError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to bind local FFmpeg %s feed socket: %s",
                     label,
                     strerror(errno));
#endif
        closeSocketDescriptor(listenDescriptor);
        return false;
    }

    if (::listen(rawSocket, 1) != 0) {
#ifdef Q_OS_WIN32
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to listen on local FFmpeg %s feed socket: %d",
                     label,
                     WSAGetLastError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to listen on local FFmpeg %s feed socket: %s",
                     label,
                     strerror(errno));
#endif
        closeSocketDescriptor(listenDescriptor);
        return false;
    }

    sockaddr_in boundAddress;
    memset(&boundAddress, 0, sizeof(boundAddress));
#ifdef Q_OS_WIN32
    int addressLength = sizeof(boundAddress);
#else
    socklen_t addressLength = sizeof(boundAddress);
#endif
    if (::getsockname(rawSocket,
                      reinterpret_cast<sockaddr*>(&boundAddress),
                      &addressLength) != 0) {
#ifdef Q_OS_WIN32
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to resolve local FFmpeg %s feed port: %d",
                     label,
                     WSAGetLastError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to resolve local FFmpeg %s feed port: %s",
                     label,
                     strerror(errno));
#endif
        closeSocketDescriptor(listenDescriptor);
        return false;
    }

    listenPort = ntohs(boundAddress.sin_port);
    return true;
}

bool BitstreamMuxer::acceptFeedConnection(qintptr listenDescriptor,
                                          int timeoutMs,
                                          qintptr& clientDescriptor,
                                          const char* label)
{
    if (clientDescriptor != -1) {
        return true;
    }

    if (listenDescriptor == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "FFmpeg %s feed listener is unavailable",
                     label);
        return false;
    }

    if (!waitForSocketActivity(listenDescriptor, timeoutMs, label, false)) {
        if (timeoutMs != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "FFmpeg failed to connect to local %s feed",
                         label);
        }
        return false;
    }

#ifdef Q_OS_WIN32
    SOCKET acceptedSocket = accept((SOCKET)listenDescriptor, nullptr, nullptr);
    if (acceptedSocket == INVALID_SOCKET) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed accepting local FFmpeg %s feed socket: %d",
                     label,
                     WSAGetLastError());
        return false;
    }
#else
    const int acceptedSocket = accept((int)listenDescriptor, nullptr, nullptr);
    if (acceptedSocket < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed accepting local FFmpeg %s feed socket: %s",
                     label,
                     strerror(errno));
        return false;
    }
#endif

    clientDescriptor = static_cast<qintptr>(acceptedSocket);
    return configureConnectedSocket(clientDescriptor, label);
}

bool BitstreamMuxer::configureConnectedSocket(qintptr& socketDescriptor, const char* label)
{
    if (socketDescriptor == -1) {
        return false;
    }

    const int enableNoDelay = 1;
#ifdef Q_OS_WIN32
    if (setsockopt((SOCKET)socketDescriptor,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   reinterpret_cast<const char*>(&enableNoDelay),
                   sizeof(enableNoDelay)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to disable Nagle on FFmpeg %s feed socket: %d",
                    label,
                    WSAGetLastError());
    }

    u_long blockingMode = 0;
    if (ioctlsocket((SOCKET)socketDescriptor, FIONBIO, &blockingMode) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to switch FFmpeg %s feed socket to blocking mode: %d",
                     label,
                     WSAGetLastError());
        closeSocketDescriptor(socketDescriptor);
        return false;
    }

    const DWORD timeoutMs = 1000;
    if (setsockopt((SOCKET)socketDescriptor,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs),
                   sizeof(timeoutMs)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to set send timeout on FFmpeg %s feed socket: %d",
                    label,
                    WSAGetLastError());
    }
#else
    if (setsockopt((int)socketDescriptor,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   &enableNoDelay,
                   sizeof(enableNoDelay)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to disable Nagle on FFmpeg %s feed socket: %s",
                    label,
                    strerror(errno));
    }

    const int flags = fcntl((int)socketDescriptor, F_GETFL, 0);
    if (flags >= 0 && fcntl((int)socketDescriptor, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to switch FFmpeg %s feed socket to blocking mode: %s",
                     label,
                     strerror(errno));
        closeSocketDescriptor(socketDescriptor);
        return false;
    }

    timeval timeoutValue;
    timeoutValue.tv_sec = 1;
    timeoutValue.tv_usec = 0;
    if (setsockopt((int)socketDescriptor,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeoutValue,
                   sizeof(timeoutValue)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to set send timeout on FFmpeg %s feed socket: %s",
                    label,
                    strerror(errno));
    }
#endif

    return true;
}

bool BitstreamMuxer::waitForSocketActivity(qintptr socketDescriptor,
                                           int timeoutMs,
                                           const char* label,
                                           bool waitForWrite)
{
    if (socketDescriptor == -1) {
        return false;
    }

    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
#ifdef Q_OS_WIN32
    if (waitForWrite) {
        FD_SET((SOCKET)socketDescriptor, &writeSet);
    }
    else {
        FD_SET((SOCKET)socketDescriptor, &readSet);
    }
#else
    if (waitForWrite) {
        FD_SET((int)socketDescriptor, &writeSet);
    }
    else {
        FD_SET((int)socketDescriptor, &readSet);
    }
#endif

    timeval timeoutValue;
    timeoutValue.tv_sec = qMax(0, timeoutMs) / 1000;
    timeoutValue.tv_usec = (qMax(0, timeoutMs) % 1000) * 1000;

    const int readyCount =
#ifdef Q_OS_WIN32
            select(0,
#else
            select((int)socketDescriptor + 1,
#endif
                   waitForWrite ? nullptr : &readSet,
                   waitForWrite ? &writeSet : nullptr,
                   nullptr,
                   timeoutMs >= 0 ? &timeoutValue : nullptr);

    if (readyCount > 0) {
        return true;
    }
    else if (readyCount == 0) {
        if (timeoutMs != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Timed out waiting for FFmpeg %s feed socket activity",
                        label);
        }
        return false;
    }

#ifdef Q_OS_WIN32
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed waiting on FFmpeg %s feed socket: %d",
                 label,
                 WSAGetLastError());
#else
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed waiting on FFmpeg %s feed socket: %s",
                 label,
                 strerror(errno));
#endif
    return false;
}

bool BitstreamMuxer::startVideoWriterThread()
{
    m_VideoWriterThread = SDL_CreateThread([](void* context) -> int {
        return static_cast<BitstreamMuxer*>(context)->runVideoWriterThread();
    }, "MLVideoMux", this);
    if (m_VideoWriterThread == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create bitstream video writer thread: %s",
                     SDL_GetError());
        return false;
    }

    return true;
}

void BitstreamMuxer::stopVideoWriterThread(bool flushPendingFrames)
{
    if (m_VideoWriterThread == nullptr) {
        return;
    }

    if (SDL_LockMutex(m_VideoLock) == 0) {
        if (!flushPendingFrames) {
            m_VideoWriterStopRequested = true;
            m_VideoFrameQueue.clear();
            m_QueuedVideoBytes = 0;
        }
        m_VideoInputClosed = true;
        SDL_CondBroadcast(m_VideoQueueCond);
        SDL_UnlockMutex(m_VideoLock);
    }

    SDL_WaitThread(m_VideoWriterThread, nullptr);
    m_VideoWriterThread = nullptr;
}

int BitstreamMuxer::runVideoWriterThread()
{
    while (true) {
        VideoFramePacket frame;

        if (SDL_LockMutex(m_VideoLock) != 0) {
            return 0;
        }

        while (!m_VideoWriterStopRequested &&
               m_VideoFrameQueue.isEmpty() &&
               !m_VideoInputClosed) {
            SDL_CondWait(m_VideoQueueCond, m_VideoLock);
        }

        if (m_VideoWriterStopRequested) {
            SDL_UnlockMutex(m_VideoLock);
            break;
        }

        if (m_VideoFrameQueue.isEmpty()) {
            const bool inputClosed = m_VideoInputClosed;
            SDL_UnlockMutex(m_VideoLock);
            if (inputClosed) {
                break;
            }
            continue;
        }

        frame = m_VideoFrameQueue.dequeue();
        m_QueuedVideoBytes -= frame.frameBuffer.size();
        SDL_UnlockMutex(m_VideoLock);

        bool ok = true;
        if (isAv1Bitstream()) {
            if (!m_VideoHeadersSent) {
                ok = writeVideoPayload(buildIvfStreamHeader());
                m_VideoHeadersSent = ok;
            }

            if (ok) {
                ok = writeVideoPayload(buildIvfFrameHeader(frame.frameBuffer.size(),
                                                           frame.presentationTimeUs));
            }
        }

        if (ok) {
            ok = writeVideoPayload(frame.frameBuffer);
        }

        if (ok) {
            if (frame.presentationTimeUs != 0) {
                m_LastVideoPresentationTimeUs = frame.presentationTimeUs;
            }
            m_SubmittedFrames++;
            continue;
        }

        if (m_ProcessActive && !m_VideoWriterStopRequested) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Disabling video recording after FFmpeg video sink write failure");
            if (SDL_LockMutex(m_VideoLock) == 0) {
                m_VideoWriterFailed = true;
                m_VideoFrameQueue.clear();
                m_QueuedVideoBytes = 0;
                SDL_UnlockMutex(m_VideoLock);
            }
        }

        break;
    }

    return 0;
}

bool BitstreamMuxer::startAudioWriterThread()
{
    if (!m_AudioConfigValid) {
        return true;
    }

    m_AudioWriterThread = SDL_CreateThread([](void* context) -> int {
        return static_cast<BitstreamMuxer*>(context)->runAudioWriterThread();
    }, "MLAudioMux", this);
    if (m_AudioWriterThread == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create bitstream audio writer thread: %s",
                     SDL_GetError());
        return false;
    }

    return true;
}

void BitstreamMuxer::stopAudioWriterThread(bool flushPendingPackets)
{
    if (m_AudioWriterThread == nullptr) {
        return;
    }

    if (SDL_LockMutex(m_AudioLock) == 0) {
        if (!flushPendingPackets) {
            m_AudioWriterStopRequested = true;
            m_AudioPacketQueue.clear();
            m_QueuedAudioBytes = 0;
        }
        m_AudioInputClosed = true;
        SDL_CondBroadcast(m_AudioQueueCond);
        SDL_UnlockMutex(m_AudioLock);
    }

    SDL_WaitThread(m_AudioWriterThread, nullptr);
    m_AudioWriterThread = nullptr;
}

int BitstreamMuxer::runAudioWriterThread()
{
    while (true) {
        AudioPacketData packetData;

        if (SDL_LockMutex(m_AudioLock) != 0) {
            return 0;
        }

        while (!m_AudioWriterStopRequested &&
               m_AudioPacketQueue.isEmpty() &&
               !m_AudioInputClosed) {
            SDL_CondWait(m_AudioQueueCond, m_AudioLock);
        }

        if (m_AudioWriterStopRequested) {
            SDL_UnlockMutex(m_AudioLock);
            break;
        }

        if (m_AudioPacketQueue.isEmpty()) {
            const bool inputClosed = m_AudioInputClosed;
            SDL_UnlockMutex(m_AudioLock);
            if (inputClosed) {
                break;
            }
            continue;
        }

        packetData = m_AudioPacketQueue.dequeue();
        m_QueuedAudioBytes -= packetData.packet.size();
        SDL_UnlockMutex(m_AudioLock);

        while (m_AudioSocketDescriptor == -1 && !m_AudioInputClosed && !m_AudioWriterStopRequested) {
            if (acceptAudioConnection(1000)) {
                break;
            }
        }

        if (m_AudioWriterStopRequested || (m_AudioInputClosed && m_AudioSocketDescriptor == -1)) {
            break;
        }

        if (m_AudioSocketDescriptor == -1) {
            continue;
        }

        if (!m_AudioHeadersSent && !sendAudioHeaders()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Disabling audio recording after FFmpeg header write failure");
            if (SDL_LockMutex(m_AudioLock) == 0) {
                m_AudioWriterFailed = true;
                m_AudioPacketQueue.clear();
                m_QueuedAudioBytes = 0;
                SDL_UnlockMutex(m_AudioLock);
            }
            break;
        }

        const quint64 packetDuration = packetDurationInSamples(packetData.packet.constData(),
                                                               packetData.packet.size());
        m_AudioGranulePosition += packetDuration;

        if (!writeAudioPayload(buildOggPage(packetData.packet, 0, m_AudioGranulePosition))) {
            if (m_ProcessActive && !m_AudioInputClosed && !m_AudioWriterStopRequested) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Disabling audio recording after FFmpeg audio sink write failure");
                if (SDL_LockMutex(m_AudioLock) == 0) {
                    m_AudioWriterFailed = true;
                    m_AudioPacketQueue.clear();
                    m_QueuedAudioBytes = 0;
                    SDL_UnlockMutex(m_AudioLock);
                }
            }
            break;
        }
    }

    return 0;
}

bool BitstreamMuxer::openFfmpegProcess()
{
    const QString command = buildFfmpegCommand();
    if (command.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to build FFmpeg command line for bitstream passthrough");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching FFmpeg sink: %s",
                qPrintable(command));

    const QString program = getResolvedFfmpegPath();
    const QStringList arguments = buildFfmpegArguments();
    if (program.isEmpty() || arguments.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to resolve FFmpeg launch arguments");
        return false;
    }

    m_Ffmpeg = new QProcess();
    m_Ffmpeg->setProgram(program);
    m_Ffmpeg->setArguments(arguments);
    m_Ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
    m_Ffmpeg->start(QIODevice::ReadOnly);

    if (!m_Ffmpeg->waitForStarted(10000)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to launch FFmpeg process: %s",
                     qPrintable(m_Ffmpeg->errorString()));
        logFfmpegMessages(true);
        delete m_Ffmpeg;
        m_Ffmpeg = nullptr;
        return false;
    }

    m_ProcessActive = true;
    return true;
}

void BitstreamMuxer::closeFfmpegProcess()
{
    if (m_Ffmpeg == nullptr) {
        return;
    }

    m_ProcessActive = false;

    if (!m_Ffmpeg->waitForFinished(10000)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "FFmpeg sink did not exit cleanly; terminating process");
        m_Ffmpeg->terminate();
        if (!m_Ffmpeg->waitForFinished(5000)) {
            m_Ffmpeg->kill();
            m_Ffmpeg->waitForFinished(5000);
        }
    }

    logFfmpegMessages(true);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FFmpeg sink exited with code %d",
                m_Ffmpeg->exitCode());

    delete m_Ffmpeg;
    m_Ffmpeg = nullptr;
}

QString BitstreamMuxer::buildFfmpegCommand() const
{
    const QString program = getResolvedFfmpegPath();
    const QStringList arguments = buildFfmpegArguments();
    if (program.isEmpty() || arguments.isEmpty()) {
        return QString();
    }

    QStringList commandParts;
    commandParts << shellQuote(program);
    for (const QString& argument : arguments) {
        commandParts << shellQuote(argument);
    }

    return commandParts.join(' ');
}

QStringList BitstreamMuxer::buildFfmpegArguments() const
{
    const StreamingPreferences* prefs = StreamingPreferences::get();
    if (!prefs->enableBitstreamPassthrough || prefs->bitstreamOutputTarget.isEmpty()) {
        return {};
    }

    const QString inputFormat = getInputFormatName();
    if (inputFormat.isEmpty() || m_VideoListenPort == 0) {
        return {};
    }

    const QString outputTarget = getResolvedOutputTarget();
    if (outputTarget.isEmpty()) {
        return {};
    }

    const QString videoInputUrl =
            QString("tcp://127.0.0.1:%1").arg(m_VideoListenPort);

    QStringList arguments = {
        "-hide_banner",
        "-loglevel", "warning",
        "-y",
        "-fflags", "+genpts"
    };

    if (!isAv1Bitstream()) {
        arguments << "-use_wallclock_as_timestamps" << "1";
    }

    arguments << "-f" << inputFormat
              << "-i" << videoInputUrl;

    if (m_AudioConfigValid && m_AudioListenPort != 0) {
        const QString audioInputUrl =
                QString("tcp://127.0.0.1:%1").arg(m_AudioListenPort);
        arguments << "-f" << "ogg"
                  << "-i" << audioInputUrl;
    }

    arguments << "-map" << "0:v:0"
              << "-c:v" << "copy"
              << "-fps_mode:v" << "passthrough";

    if (m_AudioConfigValid && m_AudioListenPort != 0) {
        arguments << "-map" << "1:a:0";

        if (prefs->recordingAudioMode == StreamingPreferences::RAM_AAC_COMPAT) {
            const int audioBitrateKbps = m_AudioConfig.channelCount > 2 ? 384 : 192;
            arguments << "-c:a" << "aac"
                      << "-b:a" << QString("%1k").arg(audioBitrateKbps);
        }
        else {
            arguments << "-c:a" << "copy";
        }
    }
    else {
        arguments << "-an";
    }

    const QString extraArgs = prefs->ffmpegExtraArgs.trimmed();
    if (!extraArgs.isEmpty()) {
        arguments.append(QProcess::splitCommand(extraArgs));
    }
    else {
        const QString muxer = autoOutputMuxer();
        if (!muxer.isEmpty()) {
            arguments << "-f" << muxer;
        }
    }

    arguments << outputTarget;
    return arguments;
}

QString BitstreamMuxer::getInputFormatName() const
{
    if (m_VideoFormat & VIDEO_FORMAT_MASK_H264) {
        return "h264";
    }
    else if (m_VideoFormat & VIDEO_FORMAT_MASK_H265) {
        return "hevc";
    }
    else if (m_VideoFormat & VIDEO_FORMAT_MASK_AV1) {
        return "ivf";
    }

    return QString();
}

QString BitstreamMuxer::getResolvedFfmpegPath() const
{
    const StreamingPreferences* prefs = StreamingPreferences::get();

    if (!prefs->ffmpegPath.isEmpty()) {
        return prefs->ffmpegPath;
    }

    QString envPath = qEnvironmentVariable("ML_FFMPEG_PATH");
    if (!envPath.isEmpty()) {
        return envPath;
    }

    return "ffmpeg";
}

QString BitstreamMuxer::autoOutputMuxer() const
{
    const QString target = StreamingPreferences::get()->bitstreamOutputTarget.toLower();

    if (target.startsWith("rtmp://")) {
        return "flv";
    }
    else if (target.startsWith("rtsp://")) {
        return "rtsp";
    }
    else if (target.startsWith("srt://") || target.startsWith("udp://")) {
        return "mpegts";
    }

    return QString();
}

bool BitstreamMuxer::shouldUseInternalFileMuxer(const QString& outputTarget) const
{
    if (outputTarget.isEmpty() || isNetworkTarget(outputTarget)) {
        return false;
    }

    return StreamingPreferences::get()->recordingAudioMode == StreamingPreferences::RAM_OPUS_COPY;
}

bool BitstreamMuxer::writeVideoPayload(const QByteArray& payload)
{
    return writeSocketPayload(m_VideoSocketDescriptor, payload, "video");
}

bool BitstreamMuxer::writeAudioPayload(const QByteArray& payload)
{
    return writeSocketPayload(m_AudioSocketDescriptor, payload, "audio");
}

bool BitstreamMuxer::writeSocketPayload(qintptr socketDescriptor, const QByteArray& payload, const char* label)
{
    if (payload.isEmpty()) {
        return true;
    }

    if (!m_ProcessActive || socketDescriptor == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "FFmpeg %s feed is not writable",
                     label);
        return false;
    }

    qint64 totalWritten = 0;
    while (totalWritten < payload.size()) {
        const qint64 remaining = payload.size() - totalWritten;
#ifdef Q_OS_WIN32
        const int bytesSent = send((SOCKET)socketDescriptor,
                                   payload.constData() + totalWritten,
                                   (int)qMin<qint64>(remaining, INT_MAX),
                                   0);
        if (bytesSent == SOCKET_ERROR) {
            const int socketError = WSAGetLastError();
            if (socketError == WSAEWOULDBLOCK || socketError == WSAETIMEDOUT) {
                if (waitForSocketActivity(socketDescriptor, 1000, label, true)) {
                    continue;
                }
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed writing %s payload to FFmpeg TCP feed: %d",
                         label,
                         socketError);
            return false;
        }
#else
        const ssize_t bytesSent = send((int)socketDescriptor,
                                       payload.constData() + totalWritten,
                                       remaining,
                                       0);
        if (bytesSent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                if (waitForSocketActivity(socketDescriptor, 1000, label, true)) {
                    continue;
                }
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed writing %s payload to FFmpeg TCP feed: %s",
                         label,
                         strerror(errno));
            return false;
        }
#endif

        if (bytesSent == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "FFmpeg closed the %s TCP feed",
                         label);
            return false;
        }

        totalWritten += bytesSent;
    }

    return true;
}

void BitstreamMuxer::closeSocketDescriptor(qintptr& socketDescriptor)
{
    if (socketDescriptor == -1) {
        return;
    }

#ifdef Q_OS_WIN32
    ::shutdown((SOCKET)socketDescriptor, SD_BOTH);
    closesocket((SOCKET)socketDescriptor);
#else
    ::shutdown((int)socketDescriptor, SHUT_RDWR);
    ::close((int)socketDescriptor);
#endif

    socketDescriptor = -1;
}

void BitstreamMuxer::logFfmpegMessages(bool forceInfo)
{
    if (m_Ffmpeg == nullptr) {
        return;
    }

    const QByteArray processOutput = m_Ffmpeg->readAllStandardOutput();
    if (processOutput.isEmpty()) {
        return;
    }

    for (const QByteArray& rawLine : processOutput.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (forceInfo) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "FFmpeg: %s",
                        line.constData());
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "FFmpeg: %s",
                        line.constData());
        }
    }
}

bool BitstreamMuxer::sendAudioHeaders()
{
    if (!m_AudioConfigValid || m_AudioHeadersSent) {
        return true;
    }

    if (!writeAudioPayload(buildOggPage(buildOpusHeadPacket(), kOggHeaderTypeBOS, 0))) {
        return false;
    }

    if (!writeAudioPayload(buildOggPage(buildOpusTagsPacket(), 0, 0))) {
        return false;
    }

    m_AudioHeadersSent = true;
    return true;
}

QByteArray BitstreamMuxer::buildIvfStreamHeader() const
{
    QByteArray header(32, 0);

    header[0] = 'D';
    header[1] = 'K';
    header[2] = 'I';
    header[3] = 'F';
    header[6] = 32;
    header[8] = 'A';
    header[9] = 'V';
    header[10] = '0';
    header[11] = '1';
    header[12] = static_cast<char>(m_Width & 0xFF);
    header[13] = static_cast<char>((m_Width >> 8) & 0xFF);
    header[14] = static_cast<char>(m_Height & 0xFF);
    header[15] = static_cast<char>((m_Height >> 8) & 0xFF);

    const quint32 timebaseDen = 1000000;
    header[16] = static_cast<char>(timebaseDen & 0xFF);
    header[17] = static_cast<char>((timebaseDen >> 8) & 0xFF);
    header[18] = static_cast<char>((timebaseDen >> 16) & 0xFF);
    header[19] = static_cast<char>((timebaseDen >> 24) & 0xFF);
    header[20] = 1;
    header[21] = 0;
    header[22] = 0;
    header[23] = 0;

    return header;
}

QByteArray BitstreamMuxer::buildIvfFrameHeader(int frameSize, quint64 presentationTimeUs) const
{
    QByteArray header(12, 0);
    const quint32 packetSize = static_cast<quint32>(qMax(0, frameSize));

    header[0] = static_cast<char>(packetSize & 0xFF);
    header[1] = static_cast<char>((packetSize >> 8) & 0xFF);
    header[2] = static_cast<char>((packetSize >> 16) & 0xFF);
    header[3] = static_cast<char>((packetSize >> 24) & 0xFF);

    quint64 pts = presentationTimeUs;
    if (pts == 0) {
        if (m_LastVideoPresentationTimeUs != 0) {
            pts = m_LastVideoPresentationTimeUs + 1;
        }
        else if (m_FrameRate > 0) {
            pts = (m_SubmittedFrames * 1000000ULL) / static_cast<quint64>(m_FrameRate);
        }
        else {
            pts = m_SubmittedFrames;
        }
    }

    for (int i = 0; i < 8; i++) {
        header[4 + i] = static_cast<char>((pts >> (i * 8)) & 0xFF);
    }

    return header;
}

QByteArray BitstreamMuxer::buildOpusHeadPacket() const
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

QByteArray BitstreamMuxer::buildOpusTagsPacket() const
{
    static const QByteArray vendor = QByteArrayLiteral("MoonlightQt Bitstream");

    QByteArray packet;
    packet.reserve(32 + vendor.size());
    packet.append("OpusTags", 8);
    appendLe32(packet, vendor.size());
    packet.append(vendor);
    appendLe32(packet, 0);

    return packet;
}

QByteArray BitstreamMuxer::buildOggPage(const QByteArray& packet,
                                        quint8 headerType,
                                        quint64 granulePosition)
{
    QByteArray segmentTable;
    segmentTable.reserve((packet.size() / 255) + 2);

    int remaining = packet.size();
    while (remaining >= 255) {
        segmentTable.append(char(255));
        remaining -= 255;
    }
    segmentTable.append(char(remaining));

    if (!packet.isEmpty() && (packet.size() % 255) == 0) {
        segmentTable.append(char(0));
    }

    QByteArray page;
    page.reserve(27 + segmentTable.size() + packet.size());
    page.append("OggS", 4);
    page.append(char(0));
    page.append(char(headerType & ~kOggHeaderTypeContinuation));
    appendLe64(page, granulePosition);
    appendLe32(page, m_OggStreamSerial);
    appendLe32(page, m_OggPageSequence);
    appendLe32(page, 0);
    page.append(char(segmentTable.size()));
    page.append(segmentTable);
    page.append(packet);

    const quint32 crc = calculateOggCrc(page);
    page[22] = static_cast<char>(crc & 0xFF);
    page[23] = static_cast<char>((crc >> 8) & 0xFF);
    page[24] = static_cast<char>((crc >> 16) & 0xFF);
    page[25] = static_cast<char>((crc >> 24) & 0xFF);

    m_OggPageSequence++;
    return page;
}

quint64 BitstreamMuxer::packetDurationInSamples(const char* sampleData, int sampleLength) const
{
    const int samples = opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(sampleData),
                                                   sampleLength,
                                                   m_AudioConfig.sampleRate);
    if (samples > 0) {
        return static_cast<quint64>(samples);
    }

    return static_cast<quint64>(qMax(0, m_AudioConfig.samplesPerFrame));
}

bool BitstreamMuxer::isAv1Bitstream() const
{
    return (m_VideoFormat & VIDEO_FORMAT_MASK_AV1) != 0;
}

quint32 BitstreamMuxer::calculateOggCrc(const QByteArray& page)
{
    static quint32 crcTable[256];
    static bool tableInitialized = false;

    if (!tableInitialized) {
        for (quint32 i = 0; i < 256; i++) {
            quint32 r = i << 24;
            for (int j = 0; j < 8; j++) {
                r = (r & 0x80000000U) ? (r << 1) ^ 0x04C11DB7U : (r << 1);
            }
            crcTable[i] = r;
        }
        tableInitialized = true;
    }

    quint32 crc = 0;
    for (unsigned char ch : page) {
        crc = (crc << 8) ^ crcTable[((crc >> 24) & 0xFF) ^ ch];
    }

    return crc;
}

void BitstreamMuxer::appendLe16(QByteArray& buffer, quint16 value)
{
    buffer.append(char(value & 0xFF));
    buffer.append(char((value >> 8) & 0xFF));
}

void BitstreamMuxer::appendLe32(QByteArray& buffer, quint32 value)
{
    buffer.append(char(value & 0xFF));
    buffer.append(char((value >> 8) & 0xFF));
    buffer.append(char((value >> 16) & 0xFF));
    buffer.append(char((value >> 24) & 0xFF));
}

void BitstreamMuxer::appendLe64(QByteArray& buffer, quint64 value)
{
    for (int i = 0; i < 8; i++) {
        buffer.append(char((value >> (i * 8)) & 0xFF));
    }
}

bool BitstreamMuxer::isNetworkTarget(const QString& target)
{
    static const QRegularExpression kUrlSchemeRegex("^[A-Za-z][A-Za-z0-9+.-]*://");
    return kUrlSchemeRegex.match(target).hasMatch();
}

QString BitstreamMuxer::buildDefaultCaptureName()
{
    const QString extension = StreamingPreferences::recordingContainerExtension(
                StreamingPreferences::get()->recordingContainer);
    return QString("Moonlight-%1.%2")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"),
                 extension);
}

QString BitstreamMuxer::shellQuote(const QString& value)
{
    QString escaped = value;

#ifdef Q_OS_WIN32
    escaped.replace("\"", "\"\"");
    return "\"" + escaped + "\"";
#else
    escaped.replace("'", "'\"'\"'");
    return "'" + escaped + "'";
#endif
}
