#include "streamingpreferences.h"
#include "utils.h"

#include <QSettings>
#include <QTranslator>
#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QReadWriteLock>
#include <QStandardPaths>
#include <QtMath>

#include <QtDebug>

#ifdef Q_OS_WIN32
#include <shobjidl.h>
#endif

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_RICHPRESENCE "richpresence"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_DETECTNETBLOCKING "detectnetblocking"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_HEVC_RECORDING_IDR_INTERVAL "hevcrecordingidrinterval"
#define SER_RECORDING_OUTPUT_DIRECTORY "recordingoutputdirectory"
#define SER_RECORDING_CONTAINER "recordingcontainer"
#define SER_RECORDING_AUDIO_MODE "recordingaudiomode"
#define SER_LANGUAGE "language"

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

namespace {

QString defaultRecordingOutputDirectory()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).trimmed();
    if (path.isEmpty()) {
        path = QDir::homePath();
    }

    return QDir::toNativeSeparators(path);
}

}

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine)
    : m_QmlEngine(qmlEngine),
      m_BitstreamOverridesActive(false),
      m_SavedWidth(0),
      m_SavedHeight(0),
      m_SavedFps(0),
      m_SavedBitrateKbps(0),
      m_SavedVideoCodecConfig(VCC_AUTO),
      m_SavedEnableHdr(false),
      m_SavedEnableYUV444(false)
{
    reload();
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

QString StreamingPreferences::recordingContainerExtension(StreamingPreferences::RecordingContainer container)
{
    switch (container)
    {
    case RecordingContainer::RC_MP4:
        return "mp4";
    case RecordingContainer::RC_MOV:
        return "mov";
    case RecordingContainer::RC_TS:
        return "ts";
    case RecordingContainer::RC_FLV:
        return "flv";
    case RecordingContainer::RC_MKV:
    default:
        return "mkv";
    }
}

QString StreamingPreferences::recordingContainerDisplayName(StreamingPreferences::RecordingContainer container)
{
    switch (container)
    {
    case RecordingContainer::RC_MP4:
        return "MP4";
    case RecordingContainer::RC_MOV:
        return "MOV";
    case RecordingContainer::RC_TS:
        return "MPEG-TS";
    case RecordingContainer::RC_FLV:
        return "FLV";
    case RecordingContainer::RC_MKV:
    default:
        return "MKV";
    }
}

QString StreamingPreferences::browseForRecordingOutputDirectory()
{
#ifdef Q_OS_WIN32
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr) {
        return QString();
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    const QString title = tr("Choose recording folder");
    dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));

    const QString currentPath = QDir::toNativeSeparators(recordingOutputDirectory.trimmed());
    if (!currentPath.isEmpty()) {
        IShellItem* currentFolder = nullptr;
        hr = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(currentPath.utf16()),
                                         nullptr,
                                         IID_PPV_ARGS(&currentFolder));
        if (SUCCEEDED(hr) && currentFolder != nullptr) {
            dialog->SetFolder(currentFolder);
            currentFolder->Release();
        }
    }

    hr = dialog->Show(nullptr);
    if (FAILED(hr)) {
        dialog->Release();
        return QString();
    }

    IShellItem* selectedFolder = nullptr;
    hr = dialog->GetResult(&selectedFolder);
    if (FAILED(hr) || selectedFolder == nullptr) {
        dialog->Release();
        return QString();
    }

    PWSTR selectedPathRaw = nullptr;
    hr = selectedFolder->GetDisplayName(SIGDN_FILESYSPATH, &selectedPathRaw);

    QString selectedPath;
    if (SUCCEEDED(hr) && selectedPathRaw != nullptr) {
        selectedPath = QDir::toNativeSeparators(QString::fromWCharArray(selectedPathRaw));
        CoTaskMemFree(selectedPathRaw);
    }

    selectedFolder->Release();
    dialog->Release();
    return selectedPath;
#else
    return QString();
#endif
}

void StreamingPreferences::reload()
{
    QSettings settings;

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless we have a slow GPU (which can take advantage of wp_viewporter
    // to reduce GPU load with lower resolution video streams).
    if (WMUtils::isRunningWayland() && !WMUtils::isGpuSlow()) {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
    }
    else {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN;
    }
#endif

    width = settings.value(SER_WIDTH, 1280).toInt();
    height = settings.value(SER_HEIGHT, 720).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    bitrateKbps = settings.value(SER_BITRATE, getDefaultBitrate(width, height, fps, enableYUV444)).toInt();
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    richPresence = settings.value(SER_RICHPRESENCE, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    detectNetworkBlocking = settings.value(SER_DETECTNETBLOCKING, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    enableBitstreamPassthrough = false;
    bitstreamOutputTarget.clear();
    ffmpegPath.clear();
    ffmpegExtraArgs.clear();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    hevcRecordingIdrIntervalMs = settings.value(SER_HEVC_RECORDING_IDR_INTERVAL, 0).toInt();
    if (hevcRecordingIdrIntervalMs < 0) {
        hevcRecordingIdrIntervalMs = 0;
    }
    recordingOutputDirectory =
            QDir::toNativeSeparators(settings.value(SER_RECORDING_OUTPUT_DIRECTORY,
                                                    defaultRecordingOutputDirectory()).toString().trimmed());
    if (recordingOutputDirectory.isEmpty()) {
        recordingOutputDirectory = defaultRecordingOutputDirectory();
    }
    recordingContainer = static_cast<RecordingContainer>(
                settings.value(SER_RECORDING_CONTAINER,
                               static_cast<int>(RecordingContainer::RC_MKV)).toInt());
    if (recordingContainer < RecordingContainer::RC_MKV ||
        recordingContainer > RecordingContainer::RC_FLV) {
        recordingContainer = RecordingContainer::RC_MKV;
    }
    recordingAudioMode = static_cast<RecordingAudioMode>(
                settings.value(SER_RECORDING_AUDIO_MODE,
                               static_cast<int>(RecordingAudioMode::RAM_OPUS_COPY)).toInt());
    if (recordingAudioMode < RecordingAudioMode::RAM_OPUS_COPY ||
        recordingAudioMode > RecordingAudioMode::RAM_AAC_COMPAT) {
        recordingAudioMode = RecordingAudioMode::RAM_OPUS_COPY;
    }
    enableHdr = settings.value(SER_HDR, false).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        // Try to load from the old preference value too
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());


    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }
}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

void StreamingPreferences::configureBitstreamPassthrough(const QString& outputTarget,
                                                         const QString& ffmpegPath,
                                                         const QString& ffmpegExtraArgs,
                                                         int widthOverride,
                                                         int heightOverride,
                                                         int fpsOverride,
                                                         int bitrateOverride,
                                                         int codecOverride,
                                                         bool hdrOverride,
                                                         bool yuv444Override)
{
    saveBitstreamOverrideState();

    enableBitstreamPassthrough = !outputTarget.trimmed().isEmpty();
    bitstreamOutputTarget = outputTarget.trimmed();
    this->ffmpegPath = ffmpegPath.trimmed();
    this->ffmpegExtraArgs = ffmpegExtraArgs.trimmed();
    width = widthOverride;
    height = heightOverride;
    fps = fpsOverride;
    bitrateKbps = bitrateOverride;
    videoCodecConfig = static_cast<VideoCodecConfig>(codecOverride);
    enableHdr = hdrOverride;
    enableYUV444 = yuv444Override;
}

void StreamingPreferences::configureGuiRecordingPassthrough()
{
    QString outputTarget = qEnvironmentVariable("ML_BITSTREAM_OUTPUT").trimmed();
    if (outputTarget.isEmpty()) {
        outputTarget = recordingOutputDirectory.trimmed();
    }
    if (outputTarget.isEmpty()) {
        outputTarget = defaultRecordingOutputDirectory();
    }

    enableBitstreamPassthrough = true;
    bitstreamOutputTarget = outputTarget;

    QString configuredFfmpegPath = qEnvironmentVariable("ML_FFMPEG_PATH").trimmed();
    if (!configuredFfmpegPath.isEmpty()) {
        ffmpegPath = configuredFfmpegPath;
    }
    else {
        ffmpegPath.clear();
    }

    ffmpegExtraArgs = qEnvironmentVariable("ML_FFMPEG_EXTRA_ARGS").trimmed();
}

void StreamingPreferences::clearBitstreamPassthrough()
{
    enableBitstreamPassthrough = false;
    bitstreamOutputTarget.clear();
    ffmpegPath.clear();
    ffmpegExtraArgs.clear();

    restoreBitstreamOverrideState();
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::saveBitstreamOverrideState()
{
    if (m_BitstreamOverridesActive) {
        return;
    }

    m_SavedWidth = width;
    m_SavedHeight = height;
    m_SavedFps = fps;
    m_SavedBitrateKbps = bitrateKbps;
    m_SavedVideoCodecConfig = videoCodecConfig;
    m_SavedEnableHdr = enableHdr;
    m_SavedEnableYUV444 = enableYUV444;
    m_BitstreamOverridesActive = true;
}

void StreamingPreferences::restoreBitstreamOverrideState()
{
    if (!m_BitstreamOverridesActive) {
        return;
    }

    width = m_SavedWidth;
    height = m_SavedHeight;
    fps = m_SavedFps;
    bitrateKbps = m_SavedBitrateKbps;
    videoCodecConfig = m_SavedVideoCodecConfig;
    enableHdr = m_SavedEnableHdr;
    enableYUV444 = m_SavedEnableYUV444;
    m_BitstreamOverridesActive = false;
}

void StreamingPreferences::save()
{
    QSettings settings;
    QString normalizedRecordingOutputDirectory =
            QDir::toNativeSeparators(recordingOutputDirectory.trimmed());
    if (normalizedRecordingOutputDirectory.isEmpty()) {
        normalizedRecordingOutputDirectory = defaultRecordingOutputDirectory();
        recordingOutputDirectory = normalizedRecordingOutputDirectory;
    }

    settings.setValue(SER_WIDTH, width);
    settings.setValue(SER_HEIGHT, height);
    settings.setValue(SER_FPS, fps);
    settings.setValue(SER_BITRATE, bitrateKbps);
    settings.setValue(SER_UNLOCK_BITRATE, unlockBitrate);
    settings.setValue(SER_AUTOADJUSTBITRATE, autoAdjustBitrate);
    settings.setValue(SER_VSYNC, enableVsync);
    settings.setValue(SER_GAMEOPTS, gameOptimizations);
    settings.setValue(SER_HOSTAUDIO, playAudioOnHost);
    settings.setValue(SER_MULTICONT, multiController);
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_QUITAPPAFTER, quitAppAfter);
    settings.setValue(SER_ABSMOUSEMODE, absoluteMouseMode);
    settings.setValue(SER_ABSTOUCHMODE, absoluteTouchMode);
    settings.setValue(SER_FRAMEPACING, framePacing);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_RICHPRESENCE, richPresence);
    settings.setValue(SER_GAMEPADMOUSE, gamepadMouse);
    settings.setValue(SER_PACKETSIZE, packetSize);
    settings.setValue(SER_DETECTNETBLOCKING, detectNetworkBlocking);
    settings.setValue(SER_SHOWPERFOVERLAY, showPerformanceOverlay);
    settings.setValue(SER_AUDIOCFG, static_cast<int>(audioConfig));
    settings.setValue(SER_HDR, enableHdr);
    settings.setValue(SER_YUV444, enableYUV444);
    settings.setValue(SER_VIDEOCFG, static_cast<int>(videoCodecConfig));
    settings.setValue(SER_VIDEODEC, static_cast<int>(videoDecoderSelection));
    settings.setValue(SER_WINDOWMODE, static_cast<int>(windowMode));
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_SWAPMOUSEBUTTONS, swapMouseButtons);
    settings.setValue(SER_MUTEONFOCUSLOSS, muteOnFocusLoss);
    settings.setValue(SER_BACKGROUNDGAMEPAD, backgroundGamepad);
    settings.setValue(SER_REVERSESCROLL, reverseScrollDirection);
    settings.setValue(SER_SWAPFACEBUTTONS, swapFaceButtons);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);
    settings.setValue(SER_HEVC_RECORDING_IDR_INTERVAL, hevcRecordingIdrIntervalMs);
    settings.setValue(SER_RECORDING_OUTPUT_DIRECTORY, normalizedRecordingOutputDirectory);
    settings.setValue(SER_RECORDING_CONTAINER, static_cast<int>(recordingContainer));
    settings.setValue(SER_RECORDING_AUDIO_MODE, static_cast<int>(recordingAudioMode));
}

QUrl StreamingPreferences::pathToFileUrl(const QString& path) const
{
    QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return QUrl();
    }

    return QUrl::fromLocalFile(QDir::fromNativeSeparators(trimmedPath));
}

QString StreamingPreferences::fileUrlToPath(const QUrl& url) const
{
    if (!url.isValid()) {
        return QString();
    }

    QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return QDir::toNativeSeparators(QDir::cleanPath(localPath));
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444)
{
    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}
