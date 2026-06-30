#include "RtmpStreamer.h"

#include "app/DebugLog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QPair>
#include <QProcessEnvironment>
#include <QSize>
#include <QStringList>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <QFileInfo>
#endif

extern "C" {
#include <callback/calldata.h>
#include <callback/signal.h>
#include <obs.h>
#include <obs-module.h>
#include <obs-properties.h>
#include <util/base.h>
#include <util/bmem.h>
}

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_6.h>
#endif

namespace {
struct ObsDataDeleter
{
    void operator()(obs_data_t *data) const
    {
        obs_data_release(data);
    }
};
using ObsDataPtr = std::unique_ptr<obs_data_t, ObsDataDeleter>;

struct ObsPropertiesDeleter
{
    void operator()(obs_properties_t *properties) const
    {
        obs_properties_destroy(properties);
    }
};
using ObsPropertiesPtr = std::unique_ptr<obs_properties_t, ObsPropertiesDeleter>;

QByteArray utf8(const QString &value)
{
    return value.toUtf8();
}

bool envFlagEnabled(const char *name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

float envFloatOrDefault(const char *name, float fallback, float minValue, float maxValue)
{
    const QByteArray raw = qgetenv(name).trimmed();
    if (raw.isEmpty()) {
        return fallback;
    }
    bool ok = false;
    const float parsed = raw.toFloat(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(parsed, minValue, maxValue);
}

QString qstr(const char *value)
{
    return QString::fromUtf8(value == nullptr ? "" : value);
}

QString captureModeName(RtmpStreamer::CaptureMode mode)
{
    return mode == RtmpStreamer::CaptureMode::FullDesktop
               ? QStringLiteral("Desktop")
               : QStringLiteral("FocusGatedDesktop");
}

QString audioSourceName(RtmpStreamer::AudioCaptureSource source)
{
    return source == RtmpStreamer::AudioCaptureSource::GameOnly
               ? QStringLiteral("GameOnly")
               : QStringLiteral("SystemWithMic");
}

QString obsLogLevelName(int level)
{
    switch (level) {
    case LOG_DEBUG:
        return QStringLiteral("debug");
    case LOG_INFO:
        return QStringLiteral("info");
    case LOG_WARNING:
        return QStringLiteral("warning");
    case LOG_ERROR:
        return QStringLiteral("error");
    default:
        return QStringLiteral("level%1").arg(level);
    }
}

void vodlinkObsLogHandler(int level, const char *format, va_list args, void *)
{
    if (!DebugLog::enabled()) {
        return;
    }

    char stackBuffer[4096];
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), format == nullptr ? "" : format, argsCopy);
    va_end(argsCopy);

    QString message;
    if (needed >= 0 && static_cast<size_t>(needed) >= sizeof(stackBuffer)) {
        QByteArray dynamic;
        dynamic.resize(needed + 1);
        std::vsnprintf(dynamic.data(), static_cast<size_t>(dynamic.size()), format == nullptr ? "" : format, args);
        message = QString::fromUtf8(dynamic.constData());
    } else {
        message = QString::fromUtf8(stackBuffer);
    }

    DebugLog::writeCategory(QStringLiteral("OBS/%1").arg(obsLogLevelName(level)), message.trimmed());
}

QString redactedIngestDescription(const QUrl &url)
{
    return QStringLiteral("%1://%2%3/<redacted-stream-key>")
        .arg(url.scheme().isEmpty() ? QStringLiteral("rtmp") : url.scheme(),
             url.host().isEmpty() ? QStringLiteral("unknown-host") : url.host(),
             url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString());
}

QByteArray obsDataSearchPath(const QString &path)
{
    QString normalized = QDir::cleanPath(path.trimmed());
    if (normalized.isEmpty()) {
        return {};
    }

    // libobs' platform fallback paths include a trailing separator, and the
    // deprecated obs_add_data_path() search helper expects the same shape when
    // it concatenates the requested effect file name. Without this, OBS ends up
    // checking e.g. ".../data/libobsdefault.effect" and every core shader is
    // reported missing even though the files are present in the private runtime.
    if (!normalized.endsWith(QLatin1Char('/')) && !normalized.endsWith(QLatin1Char('\\'))) {
        normalized.append(QLatin1Char('/'));
    }
    return QFile::encodeName(normalized);
}

void addObsDataPathNoWarning(const QString &path)
{
    const QByteArray encodedPath = obsDataSearchPath(path);
    if (encodedPath.isEmpty()) {
        return;
    }
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    obs_add_data_path(encodedPath.constData());
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

QString obsCoreDataFileReport(const ObsRuntime &runtime)
{
    const QStringList required = {
        QStringLiteral("default.effect"),
        QStringLiteral("default_rect.effect"),
        QStringLiteral("opaque.effect"),
        QStringLiteral("solid.effect"),
        QStringLiteral("repeat.effect"),
        QStringLiteral("format_conversion.effect"),
        QStringLiteral("bicubic_scale.effect"),
        QStringLiteral("lanczos_scale.effect"),
        QStringLiteral("area.effect"),
        QStringLiteral("bilinear_lowres_scale.effect"),
        QStringLiteral("premultiplied_alpha.effect"),
    };

    QStringList lines;
    lines.append(QStringLiteral("coreDataPaths=[%1]")
                     .arg(runtime.coreDataPaths().isEmpty()
                              ? QStringLiteral("none")
                              : runtime.coreDataPaths().join(QStringLiteral(", "))));

    for (const QString &file : required) {
        const QByteArray name = QFile::encodeName(file);
        char *found = obs_find_data_file(name.constData());
        if (found == nullptr) {
            lines.append(QStringLiteral("missing %1").arg(file));
            continue;
        }
        const QString foundPath = QFile::decodeName(found);
        bfree(found);
        lines.append(QStringLiteral("%1=%2")
                         .arg(file, QDir::toNativeSeparators(foundPath)));
    }
    return lines.join(QStringLiteral("; "));
}

bool verifyObsCoreDataFiles(const ObsRuntime &runtime, QString *error)
{
    const QStringList required = {
        QStringLiteral("default.effect"),
        QStringLiteral("default_rect.effect"),
        QStringLiteral("opaque.effect"),
        QStringLiteral("solid.effect"),
        QStringLiteral("repeat.effect"),
        QStringLiteral("format_conversion.effect"),
        QStringLiteral("bicubic_scale.effect"),
        QStringLiteral("lanczos_scale.effect"),
        QStringLiteral("area.effect"),
        QStringLiteral("bilinear_lowres_scale.effect"),
        QStringLiteral("premultiplied_alpha.effect"),
    };

    QStringList missing;
    for (const QString &file : required) {
        const QByteArray name = QFile::encodeName(file);
        char *found = obs_find_data_file(name.constData());
        if (found == nullptr) {
            missing.append(file);
        } else {
            bfree(found);
        }
    }

    if (!missing.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime is missing libobs graphics data files: %1. %2")
                         .arg(missing.join(QStringLiteral(", ")), obsCoreDataFileReport(runtime));
        }
        DebugLog::writeCategory(QStringLiteral("OBS"),
                                QStringLiteral("core data verification failed: %1")
                                    .arg(missing.join(QStringLiteral(", "))));
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("core data verification ok"));
    return true;
}

bool splitRtmpUrl(const QUrl &url, QString *server, QString *key)
{
    const QString text = url.toString(QUrl::FullyEncoded).trimmed();
    const int slash = text.lastIndexOf(QLatin1Char('/'));
    if (slash <= QStringLiteral("rtmp://x").size() || slash >= text.size() - 1) {
        return false;
    }
    *server = text.left(slash);
    *key = text.mid(slash + 1);
    return server->startsWith(QStringLiteral("rtmp://"), Qt::CaseInsensitive)
        || server->startsWith(QStringLiteral("rtmps://"), Qt::CaseInsensitive);
}

QStringList videoSourceCandidates(RtmpStreamer::CaptureMode mode)
{
    Q_UNUSED(mode);
#if defined(Q_OS_WIN)
    // Game privacy modes intentionally use desktop capture too. OBS Game Capture
    // is the path that frequently goes black with anti-cheat titles / GPU
    // mismatch. Privacy is enforced by a black focus gate on top of the desktop
    // source instead of by trying to hook the game renderer.
    return {QStringLiteral("monitor_capture")};
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return {QStringLiteral("screen_capture"), QStringLiteral("display_capture")};
#else
    return {QStringLiteral("pipewire-desktop-capture-source"), QStringLiteral("xshm_input")};
#endif
}

QStringList audioSourceCandidates(RtmpStreamer::AudioCaptureSource source)
{
#if defined(Q_OS_WIN)
    if (source == RtmpStreamer::AudioCaptureSource::GameOnly) {
        // Streamlabs-style application audio: capture only the target process'
        // Windows process loopback audio. Never fall back to system audio for
        // this privacy mode; if application audio cannot bind, fail clearly.
        return {QStringLiteral("wasapi_process_output_capture")};
    }
    return {QStringLiteral("wasapi_output_capture")};
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    // Modern OBS/macOS ScreenCaptureKit sources can carry per-window/app audio
    // directly on the capture source. For full desktop/external audio, also try
    // the ScreenCaptureKit audio source id used by newer OBS builds.
    if (source == RtmpStreamer::AudioCaptureSource::GameOnly) {
        return {};
    }
    return {QStringLiteral("screen_capture_audio_capture")};
#else
    if (source == RtmpStreamer::AudioCaptureSource::GameOnly) {
        // PipeWire may expose application audio as part of the capture source.
        return {};
    }
    return {QStringLiteral("pulse_output_capture")};
#endif
}

QStringList microphoneSourceCandidates(RtmpStreamer::AudioCaptureSource source)
{
    // Only the modes that intentionally include external/system audio should
    // include the default microphone. Game-only audio stays app-audio-only.
    if (source == RtmpStreamer::AudioCaptureSource::GameOnly) {
        return {};
    }
#if defined(Q_OS_WIN)
    return {QStringLiteral("wasapi_input_capture")};
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return {QStringLiteral("coreaudio_input_capture"),
            QStringLiteral("coreaudio_input_capture_v2")};
#else
    return {QStringLiteral("pulse_input_capture")};
#endif
}

float captureSourceVolumeMultiplier(const QString &sourceId, RtmpStreamer::AudioCaptureSource source)
{
    if (source == RtmpStreamer::AudioCaptureSource::GameOnly) {
        return 1.0F;
    }

    // OBS' per-process WASAPI source tends to come in close to the game's own
    // peak level, while default desktop/mic capture follows Windows/system
    // device volume and can sound much quieter on YouTube. Give the external
    // audio path a moderate default lift, with env overrides for power users.
    if (sourceId == QStringLiteral("wasapi_input_capture")
        || sourceId == QStringLiteral("pulse_input_capture")
        || sourceId.startsWith(QStringLiteral("coreaudio_input_capture"))) {
        return envFloatOrDefault("VODLINK_MIC_AUDIO_GAIN", 1.6F, 0.1F, 6.0F);
    }

    return envFloatOrDefault("VODLINK_SYSTEM_AUDIO_GAIN", 2.0F, 0.1F, 6.0F);
}

enum class VideoCodecChoice { H264, HEVC, AV1 };

QString labelForCodec(VideoCodecChoice codec)
{
    switch (codec) {
    case VideoCodecChoice::AV1:
        return QStringLiteral("AV1");
    case VideoCodecChoice::HEVC:
        return QStringLiteral("HEVC");
    case VideoCodecChoice::H264:
    default:
        return QStringLiteral("H.264");
    }
}

VideoCodecChoice codecFromPreference(const QString &preference)
{
    const QString pref = preference.toLower();
    if (pref.contains(QStringLiteral("av1"))) {
        return VideoCodecChoice::AV1;
    }
    if (pref.contains(QStringLiteral("hevc")) || pref.contains(QStringLiteral("h265"))
        || pref.contains(QStringLiteral("h.265")) || pref.contains(QStringLiteral("265"))) {
        return VideoCodecChoice::HEVC;
    }
    return VideoCodecChoice::H264;
}

VideoCodecChoice codecFromEncoderId(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    if (id.contains(QStringLiteral("av1"))) {
        return VideoCodecChoice::AV1;
    }
    if (id.contains(QStringLiteral("hevc")) || id.contains(QStringLiteral("h265"))
        || id.contains(QStringLiteral("h.265")) || id.contains(QStringLiteral("h265"))
        || id.contains(QStringLiteral("265"))) {
        return VideoCodecChoice::HEVC;
    }
    return VideoCodecChoice::H264;
}

QString videoEncoderDisplayName(const QString &encoderId, VideoColorMode colorMode)
{
    const QString label = labelForCodec(codecFromEncoderId(encoderId));
    return colorMode == VideoColorMode::HdrPQ
               ? QStringLiteral("VodLink %1 HDR").arg(label)
               : QStringLiteral("VodLink %1").arg(label);
}

QStringList hardwareEncoderIdsForCodec(VideoCodecChoice codec)
{
    // Platform-gated priority. Never probe impossible backends on a platform:
    // - Windows: NVENC -> AMF -> QSV
    // - macOS: VideoToolbox only
    // - Linux/AppImage: NVENC -> VAAPI -> QSV
    // Software encoders such as obs_x264/generic ffmpeg HEVC/AV1 are deliberately absent.
#if defined(Q_OS_WIN)
    const QStringList h264 = {QStringLiteral("jim_nvenc"), QStringLiteral("ffmpeg_nvenc"),
                              QStringLiteral("h264_texture_amf"), QStringLiteral("obs_qsv11")};
    const QStringList hevc = {QStringLiteral("jim_hevc_nvenc"), QStringLiteral("ffmpeg_hevc_nvenc"),
                              QStringLiteral("h265_texture_amf"), QStringLiteral("obs_qsv11_hevc")};
    const QStringList av1 = {QStringLiteral("jim_av1_nvenc"), QStringLiteral("ffmpeg_av1_nvenc"),
                             QStringLiteral("av1_texture_amf"), QStringLiteral("obs_qsv11_av1")};
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QStringList h264 = {QStringLiteral("com.apple.videotoolbox.videoencoder.ave.avc"),
                              QStringLiteral("com.apple.videotoolbox.videoencoder.avc")};
    const QStringList hevc = {QStringLiteral("com.apple.videotoolbox.videoencoder.ave.hevc"),
                              QStringLiteral("com.apple.videotoolbox.videoencoder.hevc")};
    const QStringList av1 = {QStringLiteral("com.apple.videotoolbox.videoencoder.ave.av1"),
                             QStringLiteral("com.apple.videotoolbox.videoencoder.av1")};
#else
    const QStringList h264 = {QStringLiteral("ffmpeg_nvenc"), QStringLiteral("vaapi_h264"),
                              QStringLiteral("obs_qsv11")};
    const QStringList hevc = {QStringLiteral("ffmpeg_hevc_nvenc"), QStringLiteral("vaapi_hevc"),
                              QStringLiteral("obs_qsv11_hevc")};
    const QStringList av1 = {QStringLiteral("ffmpeg_av1_nvenc"), QStringLiteral("vaapi_av1"),
                             QStringLiteral("obs_qsv11_av1")};
#endif
    switch (codec) {
    case VideoCodecChoice::AV1:
        return av1;
    case VideoCodecChoice::HEVC:
        return hevc;
    case VideoCodecChoice::H264:
    default:
        return h264;
    }
}

bool isNvencEncoder(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    return id.contains(QStringLiteral("nvenc"));
}

bool isAmfEncoder(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    return id.contains(QStringLiteral("_amf")) || id.contains(QStringLiteral("amf"));
}

bool isQsvEncoder(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    return id.contains(QStringLiteral("qsv"));
}

bool isVideoToolboxEncoder(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    return id.contains(QStringLiteral("videotoolbox"));
}

bool isVaapiEncoder(const QString &encoderId)
{
    const QString id = encoderId.toLower();
    return id.contains(QStringLiteral("vaapi"));
}

QStringList registeredObsInputIds()
{
    QStringList result;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_input_types(index, &id); ++index) {
        const QString sourceId = qstr(id);
        if (!sourceId.isEmpty() && !result.contains(sourceId)) {
            result.append(sourceId);
        }
    }
    return result;
}

QStringList registeredObsEncoderIds()
{
    QStringList result;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
        const QString encoderId = qstr(id);
        if (!encoderId.isEmpty() && !result.contains(encoderId)) {
            result.append(encoderId);
        }
    }
    return result;
}

QStringList registeredObsOutputIds()
{
    QStringList result;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_output_types(index, &id); ++index) {
        const QString outputId = qstr(id);
        if (!outputId.isEmpty() && !result.contains(outputId)) {
            result.append(outputId);
        }
    }
    return result;
}

QStringList registeredObsServiceIds()
{
    QStringList result;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_service_types(index, &id); ++index) {
        const QString serviceId = qstr(id);
        if (!serviceId.isEmpty() && !result.contains(serviceId)) {
            result.append(serviceId);
        }
    }
    return result;
}

QStringList filterRegisteredEncoders(const QStringList &orderedCandidates, const QStringList &registeredIds)
{
    QStringList result;
    for (const QString &id : orderedCandidates) {
        if (registeredIds.contains(id) && !result.contains(id)) {
            result.append(id);
        }
    }
    return result;
}

QStringList hardwareEncoderIdsForCodec(VideoCodecChoice codec, const QStringList &registeredIds)
{
    return filterRegisteredEncoders(hardwareEncoderIdsForCodec(codec), registeredIds);
}

QStringList hardwareEncoderChoicesForRegistry(const QStringList &registeredIds)
{
    QStringList result;
    for (VideoCodecChoice codec : {VideoCodecChoice::H264, VideoCodecChoice::HEVC, VideoCodecChoice::AV1}) {
        if (!hardwareEncoderIdsForCodec(codec, registeredIds).isEmpty()) {
            result.append(labelForCodec(codec));
        }
    }
    return result;
}

QStringList videoEncoderCandidates(const QString &preference, VideoColorMode colorMode)
{
    const QStringList registered = registeredObsEncoderIds();
    const VideoCodecChoice requested = colorMode == VideoColorMode::HdrPQ
                                           ? VideoCodecChoice::HEVC
                                           : codecFromPreference(preference);

    QStringList result = hardwareEncoderIdsForCodec(requested, registered);
    if (!result.isEmpty()) {
        return result;
    }

    // Old settings may reference a codec that is not available on this GPU, for
    // example AV1 on RTX 30-series. Fall back to the first available hardware
    // codec, but never to software x264/ffmpeg encoders.
    if (colorMode != VideoColorMode::HdrPQ) {
        for (VideoCodecChoice codec : {VideoCodecChoice::H264, VideoCodecChoice::HEVC, VideoCodecChoice::AV1}) {
            result = hardwareEncoderIdsForCodec(codec, registered);
            if (!result.isEmpty()) {
                return result;
            }
        }
    }

    return {};
}

QStringList audioEncoderCandidates()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return {QStringLiteral("CoreAudio_AAC"), QStringLiteral("ffmpeg_aac")};
#else
    return {QStringLiteral("ffmpeg_aac"), QStringLiteral("libfdk_aac")};
#endif
}

constexpr int kObsWindowPriorityExe = 2; // OBS WINDOW_PRIORITY_EXE

#if defined(Q_OS_WIN)
QString normalizedExeHint(QString hint)
{
    hint = hint.trimmed().toLower();
    if (hint.isEmpty()) {
        return {};
    }

    hint.replace(QLatin1Char('/'), QLatin1Char('\\'));
    hint = QFileInfo(hint).fileName().toLower();
    if (hint.isEmpty() || hint.contains(QLatin1Char(' ')) && !hint.endsWith(QStringLiteral(".exe"))) {
        return {};
    }
    if (!hint.endsWith(QStringLiteral(".exe")) && hint.contains(QLatin1Char('.'))) {
        return {};
    }
    if (!hint.endsWith(QStringLiteral(".exe"))) {
        hint += QStringLiteral(".exe");
    }
    return hint;
}

QStringList executableHints(const QStringList &windowHints)
{
    QStringList result;
    for (const QString &hint : windowHints) {
        const QString exe = normalizedExeHint(hint);
        if (!exe.isEmpty() && !result.contains(exe, Qt::CaseInsensitive)) {
            result.append(exe);
        }
    }
    return result;
}

QString obsProcessAudioSelectorFromExecutableHints(const QStringList &windowHints)
{
    const QStringList exeHints = executableHints(windowHints);
    if (exeHints.isEmpty()) {
        return {};
    }

    // Application Audio ultimately resolves its selector with
    // WINDOW_PRIORITY_EXE. A game process can be detected several seconds
    // before its top-level window appears in OBS' property list (CS2 commonly
    // does this). Preserve the executable identity in a valid OBS selector so
    // win-wasapi's reconnect loop can bind as soon as the window exists.
    // The placeholder title/class are ignored for executable-priority matches.
    QString executable = exeHints.first();
    executable.replace(QStringLiteral("#"), QStringLiteral("#22"));
    executable.replace(QStringLiteral(":"), QStringLiteral("#3A"));
    return QStringLiteral("VodLink:VodLink:%1").arg(executable);
}

QString decodeObsWindowField(QString value)
{
    // OBS' util/windows/window-helpers.c encodes '#' first and ':' second.
    // Its decoder reverses ':' first, then '#'.  Keep the same order so the
    // executable comparison uses exactly the same window value OBS sources use.
    value.replace(QStringLiteral("#3A"), QStringLiteral(":"));
    value.replace(QStringLiteral("#22"), QStringLiteral("#"));
    return value;
}

struct ObsWindowSelector
{
    QString value;        // exact OBS property value: title:class:exe
    QString displayName;  // exact OBS property label: [exe]: title
    QString title;
    QString className;
    QString executable;
    QString sourceId;
};

bool parseObsWindowSelector(const char *rawValue, const char *rawName,
                            const QString &sourceId, ObsWindowSelector *out)
{
    if (rawValue == nullptr || out == nullptr) {
        return false;
    }

    const QString value = QString::fromUtf8(rawValue).trimmed();
    const QStringList parts = value.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() != 3) {
        return false;
    }

    ObsWindowSelector selector;
    selector.value = value;
    selector.displayName = QString::fromUtf8(rawName == nullptr ? rawValue : rawName);
    selector.title = decodeObsWindowField(parts.at(0));
    selector.className = decodeObsWindowField(parts.at(1));
    selector.executable = decodeObsWindowField(parts.at(2)).toLower();
    selector.sourceId = sourceId;
    if (selector.executable.isEmpty() || selector.className.isEmpty()) {
        return false;
    }

    *out = selector;
    return true;
}

QList<ObsWindowSelector> obsWindowSelectorsForSource(const QString &sourceId, QStringList *details)
{
    QList<ObsWindowSelector> result;
    const QByteArray id = utf8(sourceId);
    ObsPropertiesPtr properties(obs_get_source_properties(id.constData()));
    if (!properties) {
        if (details != nullptr) {
            details->append(QStringLiteral("%1: OBS returned no source properties")
                                .arg(sourceId));
        }
        return result;
    }

    obs_property_t *windowProperty = obs_properties_get(properties.get(), "window");
    if (windowProperty == nullptr) {
        if (details != nullptr) {
            details->append(QStringLiteral("%1: OBS source has no 'window' property")
                                .arg(sourceId));
        }
        return result;
    }

    const size_t count = obs_property_list_item_count(windowProperty);
    if (details != nullptr) {
        details->append(QStringLiteral("%1: OBS listed %2 capturable window(s)")
                            .arg(sourceId)
                            .arg(static_cast<qulonglong>(count)));
    }

    for (size_t index = 0; index < count; ++index) {
        ObsWindowSelector selector;
        if (!parseObsWindowSelector(obs_property_list_item_string(windowProperty, index),
                                    obs_property_list_item_name(windowProperty, index),
                                    sourceId, &selector)) {
            continue;
        }
        result.append(selector);
    }
    return result;
}

QString resolveObsWindowSpecFromProperties(const QStringList &sourceIds,
                                           const QStringList &windowHints,
                                           QString *diagnostic)
{
    const QStringList exeHints = executableHints(windowHints);
    QStringList details;
    if (exeHints.isEmpty()) {
        if (diagnostic != nullptr) {
            *diagnostic = QStringLiteral("no executable hints; raw hints=[%1]")
                              .arg(windowHints.join(QStringLiteral(", ")));
        }
        return {};
    }

    details.append(QStringLiteral("exe hints=[%1]").arg(exeHints.join(QStringLiteral(", "))));
    QList<ObsWindowSelector> allSelectors;
    for (const QString &sourceId : sourceIds) {
        allSelectors += obsWindowSelectorsForSource(sourceId, &details);
    }

    for (const ObsWindowSelector &selector : allSelectors) {
        if (exeHints.contains(selector.executable, Qt::CaseInsensitive)) {
            if (diagnostic != nullptr) {
                details.append(QStringLiteral("selected %1 from %2 with OBS value '%3'")
                                   .arg(selector.displayName, selector.sourceId, selector.value));
                *diagnostic = details.join(QStringLiteral("; "));
            }
            return selector.value;
        }
    }

    QStringList seen;
    for (const ObsWindowSelector &selector : allSelectors) {
        const QString label = QStringLiteral("%1 via %2")
                                  .arg(selector.displayName, selector.sourceId);
        if (!seen.contains(label)) {
            seen.append(label);
        }
    }
    details.append(QStringLiteral("no OBS-listed window matched the target exe; OBS windows=[%1]")
                       .arg(seen.isEmpty() ? QStringLiteral("none") : seen.join(QStringLiteral(" | "))));
    if (diagnostic != nullptr) {
        *diagnostic = details.join(QStringLiteral("; "));
    }
    return {};
}

struct ObsMonitorSelector
{
    QString value;       // exact OBS monitor_id value, never DUMMY
    QString displayName; // exact OBS property label
};

QList<ObsMonitorSelector> obsMonitorSelectorsForSource(const QString &sourceId, QStringList *details)
{
    QList<ObsMonitorSelector> result;
    const QByteArray id = utf8(sourceId);
    ObsPropertiesPtr properties(obs_get_source_properties(id.constData()));
    if (!properties) {
        if (details != nullptr) {
            details->append(QStringLiteral("%1: OBS returned no source properties")
                                .arg(sourceId));
        }
        return result;
    }

    obs_property_t *monitorProperty = obs_properties_get(properties.get(), "monitor_id");
    if (monitorProperty == nullptr) {
        if (details != nullptr) {
            details->append(QStringLiteral("%1: OBS source has no 'monitor_id' property")
                                .arg(sourceId));
        }
        return result;
    }

    const size_t count = obs_property_list_item_count(monitorProperty);
    if (details != nullptr) {
        details->append(QStringLiteral("%1: OBS listed %2 display(s)")
                            .arg(sourceId)
                            .arg(static_cast<qulonglong>(count)));
    }

    for (size_t index = 0; index < count; ++index) {
        const char *rawValue = obs_property_list_item_string(monitorProperty, index);
        if (rawValue == nullptr) {
            continue;
        }
        const QString value = QString::fromUtf8(rawValue).trimmed();
        if (value.isEmpty() || value == QStringLiteral("DUMMY")) {
            continue;
        }

        ObsMonitorSelector selector;
        selector.value = value;
        const char *rawName = obs_property_list_item_name(monitorProperty, index);
        selector.displayName = QString::fromUtf8(rawName == nullptr ? rawValue : rawName);
        if (selector.displayName.isEmpty()) {
            selector.displayName = value;
        }
        result.append(selector);
    }
    return result;
}

QString resolveObsMonitorIdFromProperties(QString *diagnostic)
{
    QStringList details;
    const QList<ObsMonitorSelector> monitors = obsMonitorSelectorsForSource(QStringLiteral("monitor_capture"), &details);
    if (monitors.isEmpty()) {
        details.append(QStringLiteral("no real OBS monitor_id entries; refusing to use DUMMY display"));
        if (diagnostic != nullptr) {
            *diagnostic = details.join(QStringLiteral("; "));
        }
        return {};
    }

    const ObsMonitorSelector &selected = monitors.first();
    details.append(QStringLiteral("selected display '%1' with OBS monitor_id '%2'")
                       .arg(selected.displayName, selected.value));
    if (diagnostic != nullptr) {
        *diagnostic = details.join(QStringLiteral("; "));
    }
    return selected.value;
}
#else
QString resolveObsWindowSpecFromProperties(const QStringList &, const QStringList &, QString *)
{
    return {};
}

QString resolveObsMonitorIdFromProperties(QString *)
{
    return {};
}
#endif

ObsDataPtr makeVideoSourceSettings(const QString &sourceId, RtmpStreamer::CaptureMode mode,
                                   RtmpStreamer::AudioCaptureSource audioSource,
                                   VideoColorMode colorMode, const QString &obsWindowSpec,
                                   const QString &obsMonitorId)
{
    ObsDataPtr settings(obs_data_create());
    obs_data_set_bool(settings.get(), "capture_cursor", true);
    obs_data_set_bool(settings.get(), "cursor", true);
    obs_data_set_bool(settings.get(), "compatibility", false);
#if defined(Q_OS_WIN)
    // Streamlabs-style split: video capture and application/system audio are
    // separate OBS sources. Keeping audio out of the video source avoids hidden
    // fallbacks and duplicate audio paths.
    obs_data_set_bool(settings.get(), "capture_audio", false);
#else
    obs_data_set_bool(settings.get(), "capture_audio", audioSource == RtmpStreamer::AudioCaptureSource::GameOnly);
#endif
    obs_data_set_bool(settings.get(), "capture_audio_only", false);
    obs_data_set_string(settings.get(), "color_space", colorMode == VideoColorMode::HdrPQ ? "rec2100pq" : "rec709");

    if (sourceId == QStringLiteral("game_capture")) {
        // OBS Game Capture uses capture_mode values "any_fullscreen",
        // "window", and "hotkey".  Streamlabs/OBS default to any fullscreen
        // and enable the anti-cheat compatibility hook by default.
        obs_data_set_string(settings.get(), "capture_mode", obsWindowSpec.isEmpty() ? "any_fullscreen" : "window");
        obs_data_set_bool(settings.get(), "capture_cursor", false);
        obs_data_set_bool(settings.get(), "anti_cheat_hook",
                          !envFlagEnabled("VODLINK_DISABLE_OBS_ANTI_CHEAT_HOOK"));
        obs_data_set_bool(settings.get(), "capture_audio", false);
        obs_data_set_int(settings.get(), "hook_rate", 1);
        obs_data_set_bool(settings.get(), "capture_overlays", false);
        obs_data_set_bool(settings.get(), "limit_framerate", false);
        if (!obsWindowSpec.isEmpty()) {
            const QByteArray hint = utf8(obsWindowSpec);
            obs_data_set_string(settings.get(), "window", hint.constData());
            obs_data_set_int(settings.get(), "priority", kObsWindowPriorityExe);
        }
    } else if (sourceId == QStringLiteral("window_capture")) {
        if (!obsWindowSpec.isEmpty()) {
            const QByteArray hint = utf8(obsWindowSpec);
            obs_data_set_string(settings.get(), "window", hint.constData());
            obs_data_set_int(settings.get(), "priority", kObsWindowPriorityExe);
        }
        obs_data_set_int(settings.get(), "method", 2);
    } else if (sourceId == QStringLiteral("monitor_capture")) {
        obs_data_set_bool(settings.get(), "capture_cursor", true);
        obs_data_set_int(settings.get(), "method", 0); // OBS auto: with a real monitor_id this chooses DXGI on normal desktop GPUs.
        if (!obsMonitorId.isEmpty()) {
            const QByteArray monitor = utf8(obsMonitorId);
            obs_data_set_string(settings.get(), "monitor_id", monitor.constData());
        }
    } else if (sourceId == QStringLiteral("screen_capture")) {
        // Both game privacy modes and desktop modes capture the display. Game
        // modes get their privacy from the focus-gate overlay, not from window
        // capture. This avoids the black-frame path seen with game/window hooks.
        obs_data_set_string(settings.get(), "type", "display_capture");
        obs_data_set_bool(settings.get(), "show_cursor", true);
        obs_data_set_bool(settings.get(), "capture_audio", audioSource == RtmpStreamer::AudioCaptureSource::GameOnly);
    } else if (sourceId == QStringLiteral("display_capture")) {
        obs_data_set_bool(settings.get(), "show_cursor", true);
    }
    return settings;
}

ObsDataPtr makeAudioSourceSettings(const QString &sourceId, const QString &obsWindowSpec)
{
    ObsDataPtr settings(obs_data_create());
    if (sourceId == QStringLiteral("wasapi_output_capture")
        || sourceId == QStringLiteral("wasapi_input_capture")) {
        obs_data_set_string(settings.get(), "device_id", "default");
        // OBS defaults WASAPI sources to device timestamps. In VodLink's private
        // auto-record pipeline that can push a large initial audio buffer while
        // the scene is being activated; use OBS wall-clock timing for stable
        // one-shot capture startup instead.
        obs_data_set_bool(settings.get(), "use_device_timing", false);
    } else if (sourceId == QStringLiteral("wasapi_process_output_capture")) {
        if (!obsWindowSpec.isEmpty()) {
            const QByteArray hint = utf8(obsWindowSpec);
            obs_data_set_string(settings.get(), "window", hint.constData());
            obs_data_set_int(settings.get(), "priority", kObsWindowPriorityExe);
        }
    } else if (sourceId.startsWith(QStringLiteral("pulse_"))
               || sourceId.startsWith(QStringLiteral("coreaudio_input_capture"))) {
        obs_data_set_string(settings.get(), "device_id", "default");
    }
    return settings;
}

ObsDataPtr makeVideoEncoderSettings(const QString &encoderId, int bitrateKbps, int fps,
                                     VideoColorMode colorMode)
{
    ObsDataPtr settings(obs_data_create());
    const bool hdr = colorMode == VideoColorMode::HdrPQ;

    // YouTube Live compatibility remains strict: CBR, 2-second GOP, progressive
    // canvas and square pixels. The performance policy is intentionally not
    // "max quality at any cost": keep the GPU encoder in a quality-balanced mode
    // so the game keeps frame time headroom. Full-res multipass and lookahead can
    // cost measurable GPU time, so they are not the default.
    obs_data_set_int(settings.get(), "bitrate", bitrateKbps);
    obs_data_set_int(settings.get(), "keyint_sec", 2);
    obs_data_set_int(settings.get(), "keyint", (std::max)(1, fps * 2));
    obs_data_set_int(settings.get(), "gop_size", (std::max)(1, fps * 2));
    obs_data_set_string(settings.get(), "rate_control", "CBR");
    obs_data_set_string(settings.get(), "rc", "cbr");

    const VideoCodecChoice codec = codecFromEncoderId(encoderId);
    obs_data_set_string(settings.get(), "profile", hdr ? "main10"
                                                        : codec == VideoCodecChoice::H264 ? "high" : "main");
    // Prefer live frame pacing over compression efficiency. B-frames add reorder
    // delay and can make 60fps desktop/game capture feel uneven on busy GPUs.
    obs_data_set_int(settings.get(), "bf", 0);
    obs_data_set_int(settings.get(), "bframes", 0);
    obs_data_set_int(settings.get(), "refs", 1);
    obs_data_set_int(settings.get(), "ref", 1);
    obs_data_set_bool(settings.get(), "cabac", true);
    obs_data_set_string(settings.get(), "entropy_coding", "cabac");
    obs_data_set_bool(settings.get(), "repeat_headers", true);
    obs_data_set_bool(settings.get(), "closed_gop", true);

    // Backend-specific quality/performance knobs. Unknown keys are ignored by OBS
    // encoders, but keeping them backend-grouped avoids cross-platform nonsense
    // such as trying VideoToolbox settings on Windows or AMF settings on macOS.
    if (isNvencEncoder(encoderId)) {
        // Prioritize frame pacing over expensive quality passes. At 3440x1440
        // 60fps while the game is also using the GPU, quarter-res multipass and
        // temporal AQ can be enough to make the ingest look stuttery even when
        // OBS reports the stream as active. Keep CBR/keyframes strict, but use a
        // one-pass balanced live preset.
        obs_data_set_string(settings.get(), "preset", "performance");
        obs_data_set_string(settings.get(), "preset2", "p1");
        obs_data_set_string(settings.get(), "tune", "ull");
        obs_data_set_string(settings.get(), "multipass", "disabled");
        obs_data_set_string(settings.get(), "multiPass", "disabled");
        obs_data_set_bool(settings.get(), "psycho_aq", false);
        obs_data_set_bool(settings.get(), "spatial_aq", false);
        obs_data_set_bool(settings.get(), "temporal_aq", false);
        obs_data_set_bool(settings.get(), "lookahead", false);
        obs_data_set_int(settings.get(), "gpu", 0);
    } else if (isAmfEncoder(encoderId)) {
        obs_data_set_string(settings.get(), "preset", "speed");
        obs_data_set_string(settings.get(), "quality_preset", "speed");
        obs_data_set_string(settings.get(), "usage", "lowlatency");
        obs_data_set_bool(settings.get(), "preanalysis", false);
        obs_data_set_bool(settings.get(), "vbaq", false);
        obs_data_set_bool(settings.get(), "lookahead", false);
    } else if (isQsvEncoder(encoderId)) {
        obs_data_set_string(settings.get(), "target_usage", "speed");
        obs_data_set_string(settings.get(), "usage", "speed");
        obs_data_set_bool(settings.get(), "low_power", true);
        obs_data_set_bool(settings.get(), "mbbrc", false);
        obs_data_set_bool(settings.get(), "lookahead", false);
    } else if (isVideoToolboxEncoder(encoderId)) {
        obs_data_set_bool(settings.get(), "allow_sw", false);
        obs_data_set_bool(settings.get(), "realtime", true);
        obs_data_set_bool(settings.get(), "use_b_frames", false);
        obs_data_set_string(settings.get(), "quality", "balanced");
    } else if (isVaapiEncoder(encoderId)) {
        obs_data_set_bool(settings.get(), "low_power", true);
        obs_data_set_int(settings.get(), "quality", 8);
        obs_data_set_bool(settings.get(), "lookahead", false);
    }

    obs_data_set_bool(settings.get(), "hdr", hdr);
    obs_data_set_string(settings.get(), "color_format", hdr ? "p010" : "nv12");
    obs_data_set_string(settings.get(), "colorspace", hdr ? "rec2100hlg" : "rec709");
    obs_data_set_string(settings.get(), "color_space", hdr ? "rec2100hlg" : "rec709");
    obs_data_set_string(settings.get(), "transfer_characteristics", hdr ? "arib-std-b67" : "bt709");
    obs_data_set_string(settings.get(), "color_primaries", hdr ? "bt2020" : "bt709");
    obs_data_set_string(settings.get(), "matrix_coefficients", hdr ? "bt2020nc" : "bt709");
    obs_data_set_string(settings.get(), "range", "partial");
    return settings;
}

ObsDataPtr makeAudioEncoderSettings()
{
    ObsDataPtr settings(obs_data_create());
    // YouTube recommends 128 Kbps AAC for stereo audio.
    obs_data_set_int(settings.get(), "bitrate", 128);
    return settings;
}


bool envForcesHdr()
{
    const QByteArray value = qgetenv("VODLINK_FORCE_HDR").trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool platformHdrLikelyEnabled()
{
    if (envForcesHdr()) {
        return true;
    }
#if defined(Q_OS_WIN)
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) {
                continue;
            }
            DXGI_OUTPUT_DESC1 desc = {};
            if (SUCCEEDED(output6->GetDesc1(&desc))
                && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                && desc.MaxLuminance > 300.0f) {
                return true;
            }
        }
    }
#endif
    return false;
}

VideoColorMode autoVideoColorMode(RtmpStreamer::CaptureMode mode)
{
    Q_UNUSED(mode);
    return platformHdrLikelyEnabled() ? VideoColorMode::HdrPQ : VideoColorMode::Sdr709;
}

#if defined(Q_OS_WIN)
QString win32ErrorMessage(DWORD code)
{
    if (code == ERROR_SUCCESS) {
        return QStringLiteral("no Windows error");
    }
    wchar_t *buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, 0,
                                        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString message = length > 0 && buffer != nullptr
                          ? QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed()
                          : QStringLiteral("Windows error %1").arg(code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

QString runtimeFileStatus(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return QStringLiteral("missing %1").arg(QDir::toNativeSeparators(path));
    }
    return QStringLiteral("found %1 (%2 bytes)")
        .arg(QDir::toNativeSeparators(path))
        .arg(info.size());
}

QString windowsGraphicsRuntimeReport(const ObsRuntime &runtime)
{
    const QDir root(runtime.rootPath());
    const QStringList important = {
        root.filePath(QStringLiteral("bin/64bit/obs.dll")),
        root.filePath(QStringLiteral("bin/64bit/libobs-d3d11.dll")),
        root.filePath(QStringLiteral("bin/64bit/libobs-opengl.dll")),
        root.filePath(QStringLiteral("bin/64bit/d3dcompiler_47.dll")),
        root.filePath(QStringLiteral("bin/64bit/dxcompiler.dll")),
        root.filePath(QStringLiteral("bin/64bit/dxil.dll")),
    };
    QStringList lines;
    for (const QString &path : important) {
        lines.append(runtimeFileStatus(path));
    }
    return lines.join(QStringLiteral("; "));
}

bool preflightObsGraphicsModule(const ObsRuntime &runtime, const char *moduleName, QString *details)
{
    const QString dllName = QString::fromLatin1(moduleName) + QStringLiteral(".dll");
    const QString path = QDir(runtime.rootPath()).filePath(QStringLiteral("bin/64bit/%1").arg(dllName));
    if (!QFileInfo::exists(path)) {
        if (details != nullptr) {
            *details = QStringLiteral("%1 is missing. %2")
                           .arg(QDir::toNativeSeparators(path), windowsGraphicsRuntimeReport(runtime));
        }
        return false;
    }

    HMODULE module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (module == nullptr) {
        const DWORD code = GetLastError();
        if (details != nullptr) {
            *details = QStringLiteral("Could not load %1: %2. %3")
                           .arg(QDir::toNativeSeparators(path), win32ErrorMessage(code),
                                windowsGraphicsRuntimeReport(runtime));
        }
        return false;
    }
    FreeLibrary(module);
    if (details != nullptr) {
        *details = QStringLiteral("%1 preflight loaded successfully.")
                       .arg(QDir::toNativeSeparators(path));
    }
    return true;
}

QList<uint32_t> windowsDxgiAdapterIndices()
{
    using Microsoft::WRL::ComPtr;
    struct AdapterChoice
    {
        uint32_t index = 0;
        quint64 dedicatedVideoMemory = 0;
        quint64 dedicatedSystemMemory = 0;
        quint64 sharedSystemMemory = 0;
        QString name;
    };

    QList<AdapterChoice> choices;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return QList<uint32_t>{0};
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        AdapterChoice choice;
        choice.index = static_cast<uint32_t>(adapterIndex);
        choice.dedicatedVideoMemory = static_cast<quint64>(desc.DedicatedVideoMemory);
        choice.dedicatedSystemMemory = static_cast<quint64>(desc.DedicatedSystemMemory);
        choice.sharedSystemMemory = static_cast<quint64>(desc.SharedSystemMemory);
        choice.name = QString::fromWCharArray(desc.Description);
        choices.append(choice);
    }

    std::sort(choices.begin(), choices.end(), [](const AdapterChoice &a, const AdapterChoice &b) {
        // OBS game/window capture should render on the high-performance GPU when
        // Windows exposes both an iGPU and a dGPU. Ranking by dedicated VRAM puts
        // NVIDIA/AMD/Arc adapters before Intel/UMA adapters while still keeping
        // every adapter as fallback if the preferred one cannot initialize.
        if (a.dedicatedVideoMemory != b.dedicatedVideoMemory) {
            return a.dedicatedVideoMemory > b.dedicatedVideoMemory;
        }
        if (a.dedicatedSystemMemory != b.dedicatedSystemMemory) {
            return a.dedicatedSystemMemory > b.dedicatedSystemMemory;
        }
        if (a.sharedSystemMemory != b.sharedSystemMemory) {
            return a.sharedSystemMemory > b.sharedSystemMemory;
        }
        return a.index < b.index;
    });

    QStringList report;
    QList<uint32_t> result;
    for (const AdapterChoice &choice : choices) {
        result.append(choice.index);
        report.append(QStringLiteral("#%1 %2 dedicatedVRAM=%3MB shared=%4MB")
                          .arg(choice.index)
                          .arg(choice.name.trimmed().isEmpty() ? QStringLiteral("unknown") : choice.name.trimmed())
                          .arg(choice.dedicatedVideoMemory / (1024 * 1024))
                          .arg(choice.sharedSystemMemory / (1024 * 1024)));
    }
    DebugLog::writeCategory(QStringLiteral("OBS/GPU"),
                            QStringLiteral("DXGI adapter preference order: %1")
                                .arg(report.isEmpty() ? QStringLiteral("none") : report.join(QStringLiteral("; "))));

    if (result.isEmpty()) {
        result.append(0);
    }
    return result;
}
#endif

int resetObsVideoWithFallback(obs_video_info baseInfo, const ObsRuntime &runtime, QString *details)
{
    QStringList attempts;
    int lastResult = -1;
#if defined(Q_OS_WIN)
    const QList<uint32_t> adapters = windowsDxgiAdapterIndices();
    const struct Candidate {
        const char *module;
        QList<uint32_t> adapterIndices;
    } candidates[] = {
        {"libobs-d3d11", adapters},
        {"libobs-opengl", QList<uint32_t>{0}},
    };

    for (const Candidate &candidate : candidates) {
        QString preflight;
        if (!preflightObsGraphicsModule(runtime, candidate.module, &preflight)) {
            attempts.append(QStringLiteral("%1 preflight failed: %2")
                                .arg(QString::fromLatin1(candidate.module), preflight));
            continue;
        }
        attempts.append(preflight);
        for (uint32_t adapter : candidate.adapterIndices) {
            obs_video_info info = baseInfo;
            info.graphics_module = candidate.module;
            info.adapter = adapter;
            int result = obs_reset_video(&info);
            lastResult = result;
            attempts.append(QStringLiteral("obs_reset_video(%1, adapter %2, format %3, gpu_conversion %4) -> %5")
                                .arg(QString::fromLatin1(candidate.module))
                                .arg(adapter)
                                .arg(static_cast<int>(info.output_format))
                                .arg(info.gpu_conversion ? QStringLiteral("true") : QStringLiteral("false"))
                                .arg(result));
            if (result == OBS_VIDEO_SUCCESS) {
                if (details != nullptr) {
                    *details = attempts.join(QStringLiteral("; "));
                }
                return result;
            }

            // Some Windows GPUs/drivers are picky about creating the initial
            // NV12 GPU-conversion chain.  OBS itself can still render an RGB
            // canvas and let encoders/plugins negotiate conversion later.  This
            // fallback keeps game start from failing outright on those systems.
            if (baseInfo.output_format != VIDEO_FORMAT_P010) {
                obs_video_info safeInfo = baseInfo;
                safeInfo.graphics_module = candidate.module;
                safeInfo.adapter = adapter;
                safeInfo.output_format = VIDEO_FORMAT_BGRA;
                safeInfo.colorspace = VIDEO_CS_SRGB;
                safeInfo.range = VIDEO_RANGE_FULL;
                safeInfo.gpu_conversion = false;
                result = obs_reset_video(&safeInfo);
                lastResult = result;
                attempts.append(QStringLiteral("obs_reset_video(%1, adapter %2, safe BGRA, gpu_conversion false) -> %3")
                                    .arg(QString::fromLatin1(candidate.module))
                                    .arg(adapter)
                                    .arg(result));
                if (result == OBS_VIDEO_SUCCESS) {
                    if (details != nullptr) {
                        *details = attempts.join(QStringLiteral("; "));
                    }
                    return result;
                }
            }
        }
    }
#else
    const int result = obs_reset_video(&baseInfo);
    lastResult = result;
    attempts.append(QStringLiteral("obs_reset_video(%1) -> %2")
                        .arg(QString::fromLatin1(baseInfo.graphics_module == nullptr ? "default" : baseInfo.graphics_module))
                        .arg(result));
    if (result == OBS_VIDEO_SUCCESS) {
        if (details != nullptr) {
            *details = attempts.join(QStringLiteral("; "));
        }
        return result;
    }
#endif
    if (details != nullptr) {
        *details = attempts.join(QStringLiteral("; "));
    }
    return lastResult;
}

#if defined(Q_OS_WIN)
QString windowsObsModulePath(const QString &binDir, const QString &moduleName)
{
    return QDir(binDir).filePath(moduleName + QStringLiteral(".dll"));
}

QString obsModuleDataPath(const QString &dataRoot, const QString &moduleName)
{
    QString path = dataRoot;
    if (path.contains(QStringLiteral("%module%"))) {
        path.replace(QStringLiteral("%module%"), moduleName);
        return path;
    }

    const QString moduleSpecific = QDir(path).filePath(moduleName);
    return QDir(moduleSpecific).exists() ? moduleSpecific : path;
}

bool loadObsModuleByName(const ObsRuntime &runtime, const QString &moduleName,
                         QStringList *details)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("manual module probe %1").arg(moduleName));
    const QList<QPair<QString, QString>> pairs = runtime.modulePathPairs();
    for (const auto &pair : pairs) {
        const QString modulePath = windowsObsModulePath(pair.first, moduleName);
        if (!QFileInfo::exists(modulePath)) {
            continue;
        }

        const QString dataPath = obsModuleDataPath(pair.second, moduleName);
        const QByteArray modulePathUtf8 = QFile::encodeName(modulePath);
        const QByteArray dataPathUtf8 = QFile::encodeName(dataPath);
        obs_module_t *module = nullptr;
        const int openResult = obs_open_module(&module, modulePathUtf8.constData(),
                                               dataPathUtf8.constData());
        if (details != nullptr) {
            details->append(QStringLiteral("obs_open_module(%1, data=%2) -> %3")
                                .arg(QDir::toNativeSeparators(modulePath),
                                     QDir::toNativeSeparators(dataPath))
                                .arg(openResult));
        }
        if (openResult != MODULE_SUCCESS || module == nullptr) {
            continue;
        }

        const bool initOk = obs_init_module(module);
        if (details != nullptr) {
            details->append(QStringLiteral("obs_init_module(%1) -> %2")
                                .arg(moduleName, initOk ? QStringLiteral("true")
                                                        : QStringLiteral("false")));
        }
        if (initOk) {
            return true;
        }
    }

    if (details != nullptr) {
        details->append(QStringLiteral("%1 was not loaded from any private OBS module path under %2")
                            .arg(moduleName, QDir::toNativeSeparators(runtime.rootPath())));
    }
    return false;
}
#endif

bool loadVodLinkObsModules(const ObsRuntime &runtime, QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("load modules: begin"));
    QStringList details;

    for (const auto &pair : runtime.modulePathPairs()) {
        const QByteArray bin = QFile::encodeName(pair.first);
        const QByteArray data = QFile::encodeName(pair.second);
        obs_add_module_path(bin.constData(), data.constData());
        details.append(QStringLiteral("obs_add_module_path(%1, data=%2)")
                           .arg(QDir::toNativeSeparators(pair.first),
                                QDir::toNativeSeparators(pair.second)));
    }

    struct obs_module_failure_info failureInfo = {};
    obs_load_all_modules2(&failureInfo);
    obs_log_loaded_modules();
    obs_post_load_modules();

    if (failureInfo.count > 0) {
        for (size_t index = 0; index < failureInfo.count; ++index) {
            details.append(QStringLiteral("failed module: %1")
                               .arg(qstr(failureInfo.failed_modules[index])));
        }
    }
    obs_module_failure_info_free(&failureInfo);

    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("load modules: %1").arg(details.join(QStringLiteral("; "))));

    Q_UNUSED(error);
    return true;
}

obs_sceneitem_t *addSourceToSceneItem(obs_scene_t *scene, obs_source_t *source, const QSize &canvasSize)
{
    if (scene == nullptr || source == nullptr) {
        return nullptr;
    }
    obs_sceneitem_t *item = obs_scene_add(scene, source);
    if (item == nullptr) {
        return nullptr;
    }
    struct vec2 bounds = {};
    bounds.x = static_cast<float>(canvasSize.width());
    bounds.y = static_cast<float>(canvasSize.height());
    obs_sceneitem_set_bounds(item, &bounds);
    // Preserve the full captured frame inside the user-selected canvas. This is
    // deliberately SCALE_INNER, not SCALE_OUTER: ultrawide/game captures must
    // never be cropped just to fill a mismatched player surface. If the source
    // and canvas ratios differ, showing small bars is safer than cutting UI off.
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
    // obs_scene_add() returns the scene-owned item without incrementing its
    // reference. Releasing it here destroys the item while it remains linked
    // from the scene, causing a use-after-free when the canvas is activated.
    return item;
}

bool addSourceToScene(obs_scene_t *scene, obs_source_t *source, const QSize &canvasSize)
{
    return addSourceToSceneItem(scene, source, canvasSize) != nullptr;
}

bool currentObsVideoInfoForCanvas(const QSize &canvasSize, int fps, VideoColorMode colorMode,
                                  obs_video_info *info, QString *details)
{
    if (info == nullptr) {
        return false;
    }

    *info = {};
    if (!obs_get_video_info(info)) {
        if (details != nullptr) {
            *details = QStringLiteral("obs_get_video_info failed");
        }
        return false;
    }

    // The canvas must explicitly match the user-selected recorder resolution.
    // OBS' canvas API creates a dedicated video mix from this structure; do
    // not rely on the legacy default canvas warning/fallback path.
    info->fps_num = static_cast<uint32_t>(fps);
    info->fps_den = 1;
    info->base_width = static_cast<uint32_t>(canvasSize.width());
    info->base_height = static_cast<uint32_t>(canvasSize.height());
    info->output_width = static_cast<uint32_t>(canvasSize.width());
    info->output_height = static_cast<uint32_t>(canvasSize.height());
    info->scale_type = OBS_SCALE_BICUBIC;

    if (colorMode == VideoColorMode::HdrPQ) {
        info->output_format = VIDEO_FORMAT_P010;
        info->colorspace = VIDEO_CS_2100_HLG;
        info->range = VIDEO_RANGE_PARTIAL;
        info->gpu_conversion = true;
    } else if (info->output_format == VIDEO_FORMAT_NONE) {
        info->output_format = VIDEO_FORMAT_NV12;
        info->colorspace = VIDEO_CS_709;
        info->range = VIDEO_RANGE_PARTIAL;
        info->gpu_conversion = true;
    }

    if (details != nullptr) {
        *details = QStringLiteral("canvas video info size=%1x%2 fps=%3 format=%4 colorspace=%5 range=%6 gpu_conversion=%7")
                       .arg(info->base_width)
                       .arg(info->base_height)
                       .arg(info->fps_num)
                       .arg(static_cast<int>(info->output_format))
                       .arg(static_cast<int>(info->colorspace))
                       .arg(static_cast<int>(info->range))
                       .arg(info->gpu_conversion ? QStringLiteral("true") : QStringLiteral("false"));
    }
    return true;
}


ObsDataPtr makeBlackOverlaySettings(const QSize &canvasSize)
{
    ObsDataPtr settings(obs_data_create());
    obs_data_set_int(settings.get(), "width", canvasSize.width());
    obs_data_set_int(settings.get(), "height", canvasSize.height());
    obs_data_set_int(settings.get(), "color", 0xFF000000);
    return settings;
}

QString normalizedHintExecutable(const QString &hint)
{
    QString value = hint.trimmed().toLower();
    if (value.isEmpty()) {
        return {};
    }
    value = QFileInfo(value).fileName().toLower();
    if (!value.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return {};
    }
    return value;
}

#if defined(Q_OS_WIN)
bool foregroundMatchesWindowHints(const QStringList &windowHints, QString *details)
{
    HWND window = GetForegroundWindow();
    if (window == nullptr) {
        if (details != nullptr) {
            *details = QStringLiteral("no foreground window");
        }
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        if (details != nullptr) {
            *details = QStringLiteral("foreground window has no process id");
        }
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        if (details != nullptr) {
            *details = QStringLiteral("cannot open foreground process");
        }
        return false;
    }
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    const bool gotPath = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!gotPath) {
        if (details != nullptr) {
            *details = QStringLiteral("cannot read foreground process path");
        }
        return false;
    }

    const QString foregroundExe = QFileInfo(QString::fromWCharArray(path, static_cast<int>(size))).fileName().toLower();
    QStringList executableHints;
    for (const QString &hint : windowHints) {
        const QString exe = normalizedHintExecutable(hint);
        if (!exe.isEmpty() && !executableHints.contains(exe)) {
            executableHints.append(exe);
        }
    }

    const bool matched = executableHints.contains(foregroundExe);
    if (details != nullptr) {
        *details = QStringLiteral("foreground=%1 hints=[%2]")
                       .arg(foregroundExe.isEmpty() ? QStringLiteral("<empty>") : foregroundExe,
                            executableHints.isEmpty() ? QStringLiteral("none") : executableHints.join(QStringLiteral(", ")));
    }
    return matched;
}
#else
bool foregroundMatchesWindowHints(const QStringList &windowHints, QString *details)
{
    Q_UNUSED(windowHints);
    if (details != nullptr) {
        *details = QStringLiteral("focus gate unsupported on this platform; leaving desktop visible");
    }
    return true;
}
#endif

void outputStoppedCallback(void *data, calldata_t *cd)
{
    auto *streamer = static_cast<RtmpStreamer *>(data);
    const int code = static_cast<int>(calldata_int(cd, "code"));
    QMetaObject::invokeMethod(streamer, "handleOutputStopped", Qt::QueuedConnection, Q_ARG(int, code));
}
} // namespace

struct RtmpStreamer::ObsHandles
{
    obs_canvas_t *canvas = nullptr;
    obs_scene_t *scene = nullptr;
    obs_source_t *videoSource = nullptr;
    obs_sceneitem_t *videoItem = nullptr;
    obs_source_t *audioSource = nullptr;
    obs_source_t *micSource = nullptr;
    obs_source_t *focusOverlaySource = nullptr;
    obs_sceneitem_t *focusOverlayItem = nullptr;
    obs_encoder_t *videoEncoder = nullptr;
    obs_encoder_t *audioEncoder = nullptr;
    obs_service_t *service = nullptr;
    obs_output_t *output = nullptr;
    QString videoSourceId;
    QString audioSourceId;
    QString micSourceId;
    QString videoEncoderId;
    QString audioEncoderId;
    VideoColorMode colorMode = VideoColorMode::Sdr709;
    bool hdrRequested = false;
};

RtmpStreamer::RtmpStreamer(QObject *parent)
    : QObject(parent)
{
    m_diagnosticsTimer.setInterval(2000);
    connect(&m_diagnosticsTimer, &QTimer::timeout, this, &RtmpStreamer::emitDiagnostics);

    m_focusGateTimer.setInterval(250);
    connect(&m_focusGateTimer, &QTimer::timeout, this, &RtmpStreamer::updateFocusGateOverlay);
}

RtmpStreamer::~RtmpStreamer()
{
    stop();
    shutdownObsRuntime();
}

void RtmpStreamer::setRecorderSettings(const QString &encoderPreference, int bitrateKbps,
                                       const QSize &outputSize, int fps)
{
    const QString nextEncoder = encoderPreference.trimmed().isEmpty()
                                    ? QStringLiteral("H.264")
                                    : encoderPreference.trimmed();
    const int nextBitrate = (std::clamp)(bitrateKbps, 2500, 40000);
    QSize nextOutputSize = outputSize.isValid() ? outputSize : QSize(1920, 1080);
    nextOutputSize.setWidth((std::max)(640, nextOutputSize.width() & ~1));
    nextOutputSize.setHeight((std::max)(360, nextOutputSize.height() & ~1));
    const int nextFps = (fps == 30) ? 30 : 60;

    const bool videoConfigChanged = m_outputSize != nextOutputSize || m_fps != nextFps;
    m_encoderPreference = nextEncoder;
    m_bitrateKbps = nextBitrate;
    m_outputSize = nextOutputSize;
    m_fps = nextFps;

    DebugLog::writeCategory(QStringLiteral("streamer"),
                            QStringLiteral("settings encoder=%1 bitrate=%2 size=%3x%4 fps=%5 videoConfigChanged=%6")
                                .arg(m_encoderPreference)
                                .arg(m_bitrateKbps)
                                .arg(m_outputSize.width())
                                .arg(m_outputSize.height())
                                .arg(m_fps)
                                .arg(videoConfigChanged ? QStringLiteral("true") : QStringLiteral("false")));

    if (videoConfigChanged && m_obsStarted && !m_running) {
        m_runtimeVideoConfigured = false;
    }
}


bool RtmpStreamer::warmUp(QString *error)
{
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("warmUp begin"));
    m_colorMode = autoVideoColorMode(m_captureMode);
    const bool ok = initializeObs(error);
    DebugLog::writeCategory(QStringLiteral("streamer"),
                            QStringLiteral("warmUp %1%2")
                                .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"),
                                     (!ok && error != nullptr) ? QStringLiteral(": %1").arg(*error) : QString()));
    return ok;
}

void RtmpStreamer::shutdownRuntime()
{
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("shutdownRuntime"));
    stop();
    shutdownObsRuntime();
}

bool RtmpStreamer::start(const QUrl &ingestUrl, CaptureMode mode, AudioCaptureSource audioSource,
                         const QStringList &windowHints, QString *error)
{
    if (m_running) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink is already streaming.");
        }
        return false;
    }

    m_ingestUrl = ingestUrl;
    m_captureMode = mode;
    m_audioSource = audioSource;
    m_windowHints = windowHints;
    m_stopping = false;
    m_startedSignalSent = false;

    m_colorMode = autoVideoColorMode(mode);
    m_focusGateEnabled = mode == CaptureMode::GameWindow;
    m_focusGateBlack = false;
    DebugLog::writeCategory(QStringLiteral("streamer"),
                            QStringLiteral("start begin ingest=%1 mode=%2 audio=%3 hints=[%4] colorMode=%5")
                                .arg(redactedIngestDescription(ingestUrl), captureModeName(mode),
                                     audioSourceName(audioSource), windowHints.join(QStringLiteral(", ")))
                                .arg(static_cast<int>(m_colorMode)));

    if (!initializeObs(error)) {
        DebugLog::writeCategory(QStringLiteral("streamer"),
                                QStringLiteral("start failed at initializeObs: %1")
                                    .arg(error == nullptr ? QString() : *error));
        cleanupObs();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start stage ok: initializeObs"));

    if (!createScene(mode, audioSource, error)) {
        DebugLog::writeCategory(QStringLiteral("streamer"),
                                QStringLiteral("start failed at createScene: %1")
                                    .arg(error == nullptr ? QString() : *error));
        cleanupObs();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start stage ok: createScene"));

    if (!createEncoders(error)) {
        DebugLog::writeCategory(QStringLiteral("streamer"),
                                QStringLiteral("start failed at createEncoders: %1")
                                    .arg(error == nullptr ? QString() : *error));
        cleanupObs();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start stage ok: createEncoders"));

    if (!createOutput(ingestUrl, error)) {
        DebugLog::writeCategory(QStringLiteral("streamer"),
                                QStringLiteral("start failed at createOutput: %1")
                                    .arg(error == nullptr ? QString() : *error));
        cleanupObs();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start stage ok: createOutput"));

    if (!startOutput(error)) {
        DebugLog::writeCategory(QStringLiteral("streamer"),
                                QStringLiteral("start failed at startOutput: %1")
                                    .arg(error == nullptr ? QString() : *error));
        cleanupObs();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start stage ok: startOutput"));

    m_running = true;
    m_startedSignalSent = true;
    m_diagnosticsTimer.start();
    emit started();
    emitDiagnostics();
    DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("start complete"));
    return true;
}

void RtmpStreamer::stop()
{
    DebugLog::writeCategory(QStringLiteral("streamer"),
                            QStringLiteral("stop requested running=%1 stopping=%2 hasObs=%3")
                                .arg(m_running ? QStringLiteral("true") : QStringLiteral("false"),
                                     m_stopping ? QStringLiteral("true") : QStringLiteral("false"),
                                     m_obs ? QStringLiteral("true") : QStringLiteral("false")));
    if (!m_running && !m_obs) {
        return;
    }
    if (m_stopping) {
        return;
    }

    m_stopping = true;
    m_diagnosticsTimer.stop();
    m_focusGateTimer.stop();

    if (m_obs && m_obs->output != nullptr && obs_output_active(m_obs->output)) {
        // Do not force-stop a healthy RTMP output.  force_stop() tears the socket
        // down immediately and can drop the final GOP/audio packets, which makes
        // YouTube archives miss the last seconds.  Ask OBS to stop gracefully and
        // keep the Qt event loop alive until OBS emits its output stop signal.
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("graceful obs_output_stop requested"));
        QEventLoop waitLoop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(this, &RtmpStreamer::stopped, &waitLoop, &QEventLoop::quit, Qt::QueuedConnection);
        connect(&timeout, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
        obs_output_stop(m_obs->output);
        timeout.start(10000);
        waitLoop.exec();

        if (!m_running) {
            return;
        }

        DebugLog::writeCategory(QStringLiteral("OBS"),
                                QStringLiteral("graceful stop timed out; forcing OBS output stop"));
        if (m_obs && m_obs->output != nullptr && obs_output_active(m_obs->output)) {
            obs_output_force_stop(m_obs->output);
            QEventLoop forceLoop;
            QTimer forceTimeout;
            forceTimeout.setSingleShot(true);
            connect(this, &RtmpStreamer::stopped, &forceLoop, &QEventLoop::quit, Qt::QueuedConnection);
            connect(&forceTimeout, &QTimer::timeout, &forceLoop, &QEventLoop::quit);
            forceTimeout.start(2500);
            forceLoop.exec();
            if (!m_running) {
                return;
            }
        }
    }

    cleanupObs();
    const bool wasRunning = m_running;
    m_running = false;
    m_stopping = false;
    if (wasRunning || m_startedSignalSent) {
        emit stopped();
    }
    m_startedSignalSent = false;
}

bool RtmpStreamer::isStreaming() const
{
    return m_running;
}

bool RtmpStreamer::initializeObs(QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("initializeObs begin"));
    if (!m_runtime.prepare(error)) {
        DebugLog::writeCategory(QStringLiteral("OBS"),
                                QStringLiteral("runtime prepare failed: %1")
                                    .arg(error == nullptr ? QString() : *error));
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("runtime prepared root=%1 moduleConfig=%2 privateConfig=%3")
                                .arg(QDir::toNativeSeparators(m_runtime.rootPath()),
                                     QDir::toNativeSeparators(m_runtime.moduleConfigPath()),
                                     QDir::toNativeSeparators(m_runtime.privateConfigRoot())));

    if (m_obsStarted && obs_initialized()) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("libobs already started by VodLink"));
        if (!m_runtimeVideoConfigured || m_runtimeOutputSize != m_outputSize
            || m_runtimeFps != m_fps || m_runtimeColorMode != m_colorMode) {
            return configureObsVideoAudio(error);
        }
        return true;
    }

    if (obs_initialized() && !m_obsStarted) {
        if (error != nullptr) {
            *error = QStringLiteral("libobs was already initialized outside VodLink's streamer. Refusing to mix OBS runtimes.");
        }
        return false;
    }

    // Streamlabs-style ordering: register libobs data paths before obs_startup().
    // This keeps obs_find_data_file("default.effect") and friends resolving from
    // VodLink's private runtime during graphics initialization.
    if (DebugLog::enabled()) {
        base_set_log_handler(vodlinkObsLogHandler, nullptr);
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("OBS log handler installed"));
    }

    const QStringList coreDataPaths = m_runtime.coreDataPaths();
    for (const QString &path : coreDataPaths) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("obs_add_data_path %1").arg(QDir::toNativeSeparators(path)));
        addObsDataPathNoWarning(path);
    }

    const QByteArray moduleConfig = m_runtime.moduleConfigPathUtf8();
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("calling obs_startup config=%1").arg(QDir::toNativeSeparators(m_runtime.moduleConfigPath())));
    if (!obs_startup("en-US", moduleConfig.constData(), nullptr)) {
        if (error != nullptr) {
            *error = QStringLiteral("libobs startup failed using VodLink's private runtime at %1")
                         .arg(m_runtime.rootPath());
        }
        return false;
    }
    m_obsStarted = true;
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("obs_startup ok"));

    if (!verifyObsCoreDataFiles(m_runtime, error)) {
        shutdownObsRuntime();
        return false;
    }

    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("core data verified"));

    if (!configureObsVideoAudio(error)) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("configure video/audio failed: %1").arg(error == nullptr ? QString() : *error));
        shutdownObsRuntime();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("configure video/audio ok"));

    if (!loadVodLinkObsModules(m_runtime, error)) {
        shutdownObsRuntime();
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("initializeObs complete"));
    return true;
}

bool RtmpStreamer::configureObsVideoAudio(QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("configureObsVideoAudio begin size=%1x%2 fps=%3 colorMode=%4")
                                .arg(m_outputSize.width()).arg(m_outputSize.height()).arg(m_fps).arg(static_cast<int>(m_colorMode)));
    obs_video_info videoInfo = {};
#if !defined(Q_OS_WIN)
    videoInfo.graphics_module = "libobs-opengl";
#endif
    videoInfo.fps_num = static_cast<uint32_t>(m_fps);
    videoInfo.fps_den = 1;
    videoInfo.base_width = static_cast<uint32_t>(m_outputSize.width());
    videoInfo.base_height = static_cast<uint32_t>(m_outputSize.height());
    videoInfo.output_width = static_cast<uint32_t>(m_outputSize.width());
    videoInfo.output_height = static_cast<uint32_t>(m_outputSize.height());
    videoInfo.output_format = (m_colorMode == VideoColorMode::HdrPQ) ? VIDEO_FORMAT_P010 : VIDEO_FORMAT_NV12;
    // Google recommends Rec.709 for SDR. For OBS HDR, YouTube's current
    // guide says P010 plus Rec.2100 PQ or HLG, with HLG recommended.
    videoInfo.colorspace = (m_colorMode == VideoColorMode::HdrPQ) ? VIDEO_CS_2100_HLG : VIDEO_CS_709;
    videoInfo.range = VIDEO_RANGE_PARTIAL;
    videoInfo.scale_type = OBS_SCALE_BICUBIC;
    videoInfo.adapter = 0;
    videoInfo.gpu_conversion = true;

    QString videoResetDetails;
    const int videoResult = resetObsVideoWithFallback(videoInfo, m_runtime, &videoResetDetails);
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("obs_reset_video result=%1 details=%2").arg(videoResult).arg(videoResetDetails));
    if (videoResult != OBS_VIDEO_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS video initialization failed (%1). Private runtime: %2. Details: %3. Core data: %4")
                         .arg(videoResult)
                         .arg(QDir::toNativeSeparators(m_runtime.rootPath()),
                              videoResetDetails.isEmpty() ? QStringLiteral("no details") : videoResetDetails,
                              obsCoreDataFileReport(m_runtime));
        }
        return false;
    }

    obs_audio_info audioInfo = {};
    // YouTube's recommended advanced settings list 44.1 kHz for stereo
    // audio and 48 kHz for 5.1. VodLink currently streams stereo.
    // Streaming services and WASAPI devices are normally 48 kHz. Keeping OBS at
    // 48 kHz avoids the immediate 44.1 -> 48 kHz mismatch seen when desktop
    // audio starts during full-desktop capture activation.
    audioInfo.samples_per_sec = 48000;
    audioInfo.speakers = SPEAKERS_STEREO;
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("calling obs_reset_audio samples=%1 speakers=%2").arg(audioInfo.samples_per_sec).arg(static_cast<int>(audioInfo.speakers)));
    if (!obs_reset_audio(&audioInfo)) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS audio initialization failed.");
        }
        return false;
    }

    m_runtimeVideoConfigured = true;
    m_runtimeOutputSize = m_outputSize;
    m_runtimeFps = m_fps;
    m_runtimeColorMode = m_colorMode;
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("configureObsVideoAudio complete"));
    return true;
}

void RtmpStreamer::shutdownObsRuntime()
{
    releaseObsObjects();
    m_obs.reset();
    if (m_obsStarted && obs_initialized()) {
        obs_shutdown();
    }
    m_obsStarted = false;
    m_runtimeVideoConfigured = false;
    m_runtimeOutputSize = QSize();
    m_runtimeFps = 0;
    m_runtimeColorMode = VideoColorMode::Sdr709;
}

bool RtmpStreamer::createScene(CaptureMode mode, AudioCaptureSource audioSource, QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("createScene begin mode=%1 audio=%2 hints=[%3]")
                                .arg(captureModeName(mode), audioSourceName(audioSource), m_windowHints.join(QStringLiteral(", "))));
    m_obs = std::make_unique<ObsHandles>();
    m_obs->colorMode = m_colorMode;
    m_obs->hdrRequested = m_colorMode == VideoColorMode::HdrPQ;

    obs_video_info canvasVideoInfo = {};
    QString canvasDetails;
    if (!currentObsVideoInfoForCanvas(m_outputSize, m_fps, m_colorMode, &canvasVideoInfo, &canvasDetails)) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not read the active video configuration for VodLink's canvas. Details: %1")
                         .arg(canvasDetails);
        }
        return false;
    }

    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("creating private canvas %1").arg(canvasDetails));
    m_obs->canvas = obs_canvas_create_private("VodLink Canvas", &canvasVideoInfo, PROGRAM | EPHEMERAL);
    if (m_obs->canvas == nullptr || !obs_canvas_has_video(m_obs->canvas)) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create VodLink's private canvas at %1x%2.")
                         .arg(m_outputSize.width()).arg(m_outputSize.height());
        }
        return false;
    }

    m_obs->scene = obs_canvas_scene_create(m_obs->canvas, "VodLink Private Scene");
    if (m_obs->scene == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create VodLink's private scene on the private canvas.");
        }
        return false;
    }

    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("private canvas and scene ready size=%1x%2")
                                .arg(m_outputSize.width()).arg(m_outputSize.height()));

    const QStringList registeredInputs = registeredObsInputIds();
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("registered inputs=[%1]").arg(registeredInputs.join(QStringLiteral(", "))));
    QString obsWindowSpec;
    QString obsAudioWindowSpec;
    QString obsMonitorId;
#if defined(Q_OS_WIN)
    if (audioSource == AudioCaptureSource::GameOnly
        && !registeredInputs.contains(QStringLiteral("wasapi_process_output_capture"))) {
        if (error != nullptr) {
            *error = QStringLiteral("Game only needs OBS Application Audio support (wasapi_process_output_capture), but VodLink's private OBS runtime did not register it. Rebuild the dynamic package with win-wasapi.dll and its data folder.");
        }
        return false;
    }

    if (audioSource == AudioCaptureSource::GameOnly) {
        QString diagnostic;
        const QStringList selectorSources = {QStringLiteral("wasapi_process_output_capture")};
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("application-audio selector sources=[%1]").arg(selectorSources.join(QStringLiteral(", "))));
        obsAudioWindowSpec = resolveObsWindowSpecFromProperties(selectorSources, m_windowHints, &diagnostic);
        for (int attempt = 1; attempt <= 20 && obsAudioWindowSpec.isEmpty(); ++attempt) {
            QThread::msleep(250);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QString retryDiagnostic;
            obsAudioWindowSpec = resolveObsWindowSpecFromProperties(selectorSources, m_windowHints, &retryDiagnostic);
            if (!obsAudioWindowSpec.isEmpty()) {
                DebugLog::writeCategory(QStringLiteral("OBS"),
                                        QStringLiteral("application-audio selector resolved after retry %1: %2 details=%3")
                                            .arg(attempt)
                                            .arg(obsAudioWindowSpec, retryDiagnostic));
                break;
            }
            diagnostic = retryDiagnostic;
        }
        if (obsAudioWindowSpec.isEmpty()) {
            obsAudioWindowSpec = obsProcessAudioSelectorFromExecutableHints(m_windowHints);
            if (!obsAudioWindowSpec.isEmpty()) {
                DebugLog::writeCategory(
                    QStringLiteral("OBS"),
                    QStringLiteral("Application Audio target not listed yet; binding by detected executable with selector '%1'")
                        .arg(obsAudioWindowSpec));
            }
        }
        if (obsAudioWindowSpec.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("Game only could not identify the game's executable for OBS Application Audio. Add the game manually with its real .exe, or try Game and external audio. Details: %1")
                             .arg(diagnostic);
            }
            return false;
        }
    }

    // Game modes also use desktop capture now; the visual privacy guarantee is
    // the black focus gate, not a fragile game/window hook. Desktop modes use
    // the same real monitor selector without the focus gate.
    {
        QString diagnostic;
        obsMonitorId = resolveObsMonitorIdFromProperties(&diagnostic);
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("monitor selector result=%1 details=%2")
                                .arg(obsMonitorId.isEmpty() ? QStringLiteral("<empty>") : obsMonitorId, diagnostic));
        if (obsMonitorId.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("OBS did not list a real display for desktop capture. VodLink refused to use the OBS DUMMY monitor. Details: %1")
                             .arg(diagnostic);
            }
            return false;
        }
    }
#endif
    for (const QString &sourceId : videoSourceCandidates(mode)) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("trying video source %1").arg(sourceId));
        if (!registeredInputs.contains(sourceId)) {
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("video source %1 not registered").arg(sourceId));
            continue;
        }
        ObsDataPtr settings = makeVideoSourceSettings(sourceId, mode, audioSource, m_colorMode, obsWindowSpec, obsMonitorId);
        const QByteArray id = utf8(sourceId);
        m_obs->videoSource = obs_source_create_private(id.constData(), "VodLink Video", settings.get());
        m_obs->videoItem = addSourceToSceneItem(m_obs->scene, m_obs->videoSource, m_outputSize);
        if (m_obs->videoSource != nullptr && m_obs->videoItem != nullptr) {
#if defined(Q_OS_WIN)
            obs_source_set_audio_mixers(m_obs->videoSource, 0);
#else
            obs_source_set_audio_mixers(m_obs->videoSource,
                                        audioSource == AudioCaptureSource::GameOnly ? 1 : 0);
#endif
            obs_source_set_monitoring_type(m_obs->videoSource, OBS_MONITORING_TYPE_NONE);
            m_obs->videoSourceId = sourceId;
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("video source created %1").arg(sourceId));
            break;
        }
        m_obs->videoItem = nullptr;
        if (m_obs->videoSource != nullptr) {
            obs_source_release(m_obs->videoSource);
            m_obs->videoSource = nullptr;
        }
    }

    if (m_obs->videoSource == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create a desktop capture source from VodLink's private plugins.");
        }
        return false;
    }

    if (mode == CaptureMode::GameWindow) {
        QString overlayId;
        for (const QString &candidate : {QStringLiteral("color_source_v3"),
                                         QStringLiteral("color_source_v2"),
                                         QStringLiteral("color_source")}) {
            if (registeredInputs.contains(candidate)) {
                overlayId = candidate;
                break;
            }
        }
        if (!overlayId.isEmpty()) {
            ObsDataPtr settings = makeBlackOverlaySettings(m_outputSize);
            const QByteArray overlaySourceId = utf8(overlayId);
            m_obs->focusOverlaySource = obs_source_create_private(overlaySourceId.constData(), "VodLink Focus Privacy Mask", settings.get());
            m_obs->focusOverlayItem = addSourceToSceneItem(m_obs->scene, m_obs->focusOverlaySource, m_outputSize);
            if (m_obs->focusOverlaySource == nullptr || m_obs->focusOverlayItem == nullptr) {
                DebugLog::writeCategory(QStringLiteral("OBS"),
                                        QStringLiteral("focus mask color source failed; falling back to hiding the desktop source"));
                if (m_obs->focusOverlaySource != nullptr) {
                    obs_source_release(m_obs->focusOverlaySource);
                    m_obs->focusOverlaySource = nullptr;
                }
                m_obs->focusOverlayItem = nullptr;
            }
        } else {
            DebugLog::writeCategory(QStringLiteral("OBS"),
                                    QStringLiteral("no OBS color source registered; focus gate will black out by hiding the desktop source"));
        }

        if (m_obs->focusOverlayItem == nullptr && m_obs->videoItem == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("OBS could not create a Game privacy black-screen gate.");
            }
            return false;
        }

        m_focusGateEnabled = true;
        // Start black by default. The first focus poll will reveal the desktop
        // immediately when the target game is already foreground, but if the
        // user starts while alt-tabbed we never leak the desktop for a frame.
        m_focusGateBlack = true;
        if (m_obs->focusOverlayItem != nullptr) {
            obs_sceneitem_set_visible(m_obs->focusOverlayItem, true);
        } else if (m_obs->videoItem != nullptr) {
            obs_sceneitem_set_visible(m_obs->videoItem, false);
        }
        updateFocusGateOverlay();
        m_focusGateTimer.start();
    }

    for (const QString &sourceId : audioSourceCandidates(audioSource)) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("trying audio source %1").arg(sourceId));
        if (!registeredInputs.contains(sourceId)) {
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("audio source %1 not registered").arg(sourceId));
            continue;
        }
        ObsDataPtr settings = makeAudioSourceSettings(sourceId, obsAudioWindowSpec);
        const QByteArray id = utf8(sourceId);
        m_obs->audioSource = obs_source_create_private(id.constData(), "VodLink Audio", settings.get());
        if (m_obs->audioSource != nullptr && addSourceToScene(m_obs->scene, m_obs->audioSource, m_outputSize)) {
            const float volume = captureSourceVolumeMultiplier(sourceId, audioSource);
            obs_source_set_volume(m_obs->audioSource, volume);
            obs_source_set_audio_mixers(m_obs->audioSource, 1);
            obs_source_set_monitoring_type(m_obs->audioSource, OBS_MONITORING_TYPE_NONE);
            m_obs->audioSourceId = sourceId;
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("audio source created %1 volume=%2").arg(sourceId).arg(volume));
            break;
        }
        if (m_obs->audioSource != nullptr) {
            obs_source_release(m_obs->audioSource);
            m_obs->audioSource = nullptr;
        }
    }

    for (const QString &sourceId : microphoneSourceCandidates(audioSource)) {
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("trying microphone source %1").arg(sourceId));
        if (!registeredInputs.contains(sourceId)) {
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("microphone source %1 not registered").arg(sourceId));
            continue;
        }
        ObsDataPtr settings = makeAudioSourceSettings(sourceId, {});
        const QByteArray id = utf8(sourceId);
        m_obs->micSource = obs_source_create_private(id.constData(), "VodLink Microphone", settings.get());
        if (m_obs->micSource != nullptr && addSourceToScene(m_obs->scene, m_obs->micSource, m_outputSize)) {
            const float volume = captureSourceVolumeMultiplier(sourceId, audioSource);
            obs_source_set_volume(m_obs->micSource, volume);
            obs_source_set_audio_mixers(m_obs->micSource, 1);
            obs_source_set_monitoring_type(m_obs->micSource, OBS_MONITORING_TYPE_NONE);
            m_obs->micSourceId = sourceId;
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("microphone source created %1 volume=%2").arg(sourceId).arg(volume));
            break;
        }
        if (m_obs->micSource != nullptr) {
            obs_source_release(m_obs->micSource);
            m_obs->micSource = nullptr;
        }
    }

#if defined(Q_OS_WIN)
    if (audioSource == AudioCaptureSource::GameOnly && m_obs->audioSource == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create Game only Application Audio from VodLink's private plugins. Switch to Game and external audio or Desktop with external audio for this game.");
        }
        return false;
    }
#endif

    obs_source_t *sceneSource = obs_scene_get_source(m_obs->scene);
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("setting canvas channel 0 to scene source"));
    obs_canvas_set_channel(m_obs->canvas, 0, sceneSource);
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("canvas channel 0 set"));
    // obs_scene_get_source() returns the scene's borrowed source pointer. The
    // canvas takes its own reference in obs_canvas_set_channel(); releasing the
    // borrowed pointer here drops the caller-owned scene reference instead and
    // makes cleanup destroy the source while obs_canvas_remove_source() is
    // still detaching it.
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("createScene complete canvas=%1x%2 video=%3 audio=%4 mic=%5 focusGate=%6")
                            .arg(m_outputSize.width()).arg(m_outputSize.height())
                            .arg(m_obs->videoSourceId,
                                 m_obs->audioSourceId.isEmpty() ? QStringLiteral("none") : m_obs->audioSourceId,
                                 m_obs->micSourceId.isEmpty() ? QStringLiteral("none") : m_obs->micSourceId,
                                 m_focusGateEnabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    return true;
}

bool RtmpStreamer::createEncoders(QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("createEncoders begin preference=%1 colorMode=%2")
                            .arg(m_encoderPreference).arg(static_cast<int>(m_colorMode)));
    const QStringList candidates = videoEncoderCandidates(m_encoderPreference, m_colorMode);
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("video encoder candidates=[%1]").arg(candidates.join(QStringLiteral(", "))));
    for (const QString &encoderId : candidates) {
        ObsDataPtr settings = makeVideoEncoderSettings(encoderId, m_bitrateKbps, m_fps, m_colorMode);
        const QByteArray id = utf8(encoderId);
        const QByteArray name = utf8(videoEncoderDisplayName(encoderId, m_colorMode));
        m_obs->videoEncoder = obs_video_encoder_create(id.constData(), name.constData(), settings.get(), nullptr);
        if (m_obs->videoEncoder != nullptr) {
            video_t *canvasVideo = m_obs->canvas != nullptr ? obs_canvas_get_video(m_obs->canvas) : nullptr;
            if (canvasVideo == nullptr) {
                DebugLog::writeCategory(QStringLiteral("OBS"),
                                        QStringLiteral("video encoder %1 has no canvas video; releasing")
                                            .arg(encoderId));
                obs_encoder_release(m_obs->videoEncoder);
                m_obs->videoEncoder = nullptr;
                continue;
            }
            obs_encoder_set_video(m_obs->videoEncoder, canvasVideo);
            m_obs->videoEncoderId = encoderId;
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("video encoder created %1").arg(encoderId));
            break;
        }
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("video encoder failed %1").arg(encoderId));
    }
    if (m_obs->videoEncoder == nullptr) {
        if (error != nullptr) {
            const QStringList choices = hardwareEncoderChoicesForRegistry(registeredObsEncoderIds());
            *error = m_colorMode == VideoColorMode::HdrPQ
                         ? QStringLiteral("OBS detected HDR output but no private hardware HEVC/Main10 encoder is available. HDR will not use software fallback; bundle/update NVENC, AMF, QSV, VideoToolbox or VAAPI HEVC support in VodLink's OBS runtime.")
                         : QStringLiteral("OBS could not create a private hardware video encoder for %1. Available hardware codecs: %2. VodLink intentionally refuses software x264/FFmpeg fallback.")
                               .arg(m_encoderPreference, choices.isEmpty() ? QStringLiteral("none") : choices.join(QStringLiteral(", ")));
        }
        return false;
    }

    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("audio encoder candidates=[%1]").arg(audioEncoderCandidates().join(QStringLiteral(", "))));
    for (const QString &encoderId : audioEncoderCandidates()) {
        ObsDataPtr settings = makeAudioEncoderSettings();
        const QByteArray id = utf8(encoderId);
        m_obs->audioEncoder = obs_audio_encoder_create(id.constData(), "VodLink AAC", settings.get(), 0, nullptr);
        if (m_obs->audioEncoder != nullptr) {
            obs_encoder_set_audio(m_obs->audioEncoder, obs_get_audio());
            m_obs->audioEncoderId = encoderId;
            DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("audio encoder created %1").arg(encoderId));
            break;
        }
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("audio encoder failed %1").arg(encoderId));
    }
    if (m_obs->audioEncoder == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create an AAC encoder. Bundle ffmpeg-aac/CoreAudio AAC support in VodLink's private runtime.");
        }
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("createEncoders complete video=%1 audio=%2").arg(m_obs->videoEncoderId, m_obs->audioEncoderId));
    return true;
}

bool RtmpStreamer::createOutput(const QUrl &ingestUrl, QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("createOutput begin ingest=%1").arg(redactedIngestDescription(ingestUrl)));
    QString server;
    QString key;
    if (!splitRtmpUrl(ingestUrl, &server, &key)) {
        if (error != nullptr) {
            *error = QStringLiteral("YouTube returned an invalid RTMP ingest URL.");
        }
        return false;
    }

    if (!registeredObsServiceIds().contains(QStringLiteral("rtmp_common"))) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS RTMP service type rtmp_common is unavailable. Bundle/load rtmp-services in VodLink's private runtime.");
        }
        return false;
    }
    if (!registeredObsOutputIds().contains(QStringLiteral("rtmp_output"))) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS RTMP output type rtmp_output is unavailable. Bundle/load obs-outputs in VodLink's private runtime.");
        }
        return false;
    }

    ObsDataPtr serviceSettings(obs_data_create());
    const QByteArray serverUtf8 = utf8(server);
    const QByteArray keyUtf8 = utf8(key);
    obs_data_set_string(serviceSettings.get(), "service", "Custom");
    obs_data_set_string(serviceSettings.get(), "server", serverUtf8.constData());
    obs_data_set_string(serviceSettings.get(), "key", keyUtf8.constData());
    obs_data_set_bool(serviceSettings.get(), "use_auth", false);
    const VideoCodecChoice outputCodec = codecFromEncoderId(m_obs->videoEncoderId);
    const bool needsEnhancedRtmp = m_colorMode == VideoColorMode::HdrPQ
                                   || outputCodec == VideoCodecChoice::HEVC
                                   || outputCodec == VideoCodecChoice::AV1;
    if (needsEnhancedRtmp) {
        // Prefer YouTube's enhanced RTMP path for HEVC/AV1 and for HEVC HDR when
        // the bundled OBS runtime supports it. If the runtime lacks the setting,
        // OBS will ignore it and the normal output creation error path remains.
        obs_data_set_bool(serviceSettings.get(), "enhanced_broadcast", true);
        obs_data_set_bool(serviceSettings.get(), "enhanced_rtmp", true);
        obs_data_set_string(serviceSettings.get(), "protocol", "RTMPS");
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("creating OBS service rtmp_common server=%1 enhanced=%2")
                            .arg(server, needsEnhancedRtmp ? QStringLiteral("true") : QStringLiteral("false")));
    m_obs->service = obs_service_create("rtmp_common", "VodLink YouTube", serviceSettings.get(), nullptr);
    if (m_obs->service == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create the RTMP service. Bundle/load rtmp-services in VodLink's private runtime.");
        }
        return false;
    }

    ObsDataPtr outputSettings(obs_data_create());
    obs_data_set_string(outputSettings.get(), "bind_ip", "default");
    if (needsEnhancedRtmp) {
        obs_data_set_bool(outputSettings.get(), "enhanced_rtmp", true);
    }
    if (m_colorMode == VideoColorMode::HdrPQ) {
        obs_data_set_bool(outputSettings.get(), "hdr", true);
        obs_data_set_string(outputSettings.get(), "video_format", "p010");
        obs_data_set_string(outputSettings.get(), "color_space", "rec2100hlg");
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("creating OBS output rtmp_output"));
    m_obs->output = obs_output_create("rtmp_output", "VodLink RTMP", outputSettings.get(), nullptr);
    if (m_obs->output == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("OBS could not create the RTMP output. Bundle obs-outputs in VodLink's private runtime.");
        }
        return false;
    }

    obs_output_set_service(m_obs->output, m_obs->service);
    obs_output_set_video_encoder(m_obs->output, m_obs->videoEncoder);
    obs_output_set_audio_encoder(m_obs->output, m_obs->audioEncoder, 0);
    obs_output_set_reconnect_settings(m_obs->output, 5, 2);
    obs_output_set_preferred_size(m_obs->output,
                                  static_cast<uint32_t>(m_outputSize.width()),
                                  static_cast<uint32_t>(m_outputSize.height()));

    signal_handler_t *handler = obs_output_get_signal_handler(m_obs->output);
    signal_handler_connect(handler, "stop", outputStoppedCallback, this);
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("createOutput complete preferredSize=%1x%2")
                            .arg(m_outputSize.width()).arg(m_outputSize.height()));
    return true;
}

bool RtmpStreamer::startOutput(QString *error)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("calling obs_output_start"));
    if (!obs_output_start(m_obs->output)) {
        const QString lastError = qstr(obs_output_get_last_error(m_obs->output)).trimmed();
        if (error != nullptr) {
            *error = lastError.isEmpty()
                         ? QStringLiteral("OBS could not start the RTMP stream.")
                         : QStringLiteral("OBS could not start the RTMP stream: %1").arg(lastError);
        }
        DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("obs_output_start failed: %1").arg(error == nullptr ? QString() : *error));
        return false;
    }
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("obs_output_start returned true"));
    return true;
}

void RtmpStreamer::cleanupObs()
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("cleanupObs running=%1 hasObs=%2")
                            .arg(m_running ? QStringLiteral("true") : QStringLiteral("false"), m_obs ? QStringLiteral("true") : QStringLiteral("false")));
    if (m_obs && m_obs->output != nullptr && obs_output_active(m_obs->output)) {
        obs_output_force_stop(m_obs->output);
    }
    releaseObsObjects();
    m_obs.reset();
}

void RtmpStreamer::releaseObsObjects()
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("releaseObsObjects"));
    m_focusGateTimer.stop();
    m_focusGateEnabled = false;
    m_focusGateBlack = false;
    if (!m_obs) {
        return;
    }

    if (m_obs->canvas != nullptr) {
        obs_canvas_set_channel(m_obs->canvas, 0, nullptr);
    } else {
        obs_set_output_source(0, nullptr);
    }

    if (m_obs->output != nullptr) {
        signal_handler_t *handler = obs_output_get_signal_handler(m_obs->output);
        if (handler != nullptr) {
            signal_handler_disconnect(handler, "stop", outputStoppedCallback, this);
        }
        obs_output_release(m_obs->output);
        m_obs->output = nullptr;
    }
    if (m_obs->service != nullptr) {
        obs_service_release(m_obs->service);
        m_obs->service = nullptr;
    }
    if (m_obs->audioEncoder != nullptr) {
        obs_encoder_release(m_obs->audioEncoder);
        m_obs->audioEncoder = nullptr;
    }
    if (m_obs->videoEncoder != nullptr) {
        obs_encoder_release(m_obs->videoEncoder);
        m_obs->videoEncoder = nullptr;
    }
    if (m_obs->focusOverlaySource != nullptr) {
        obs_source_release(m_obs->focusOverlaySource);
        m_obs->focusOverlaySource = nullptr;
        m_obs->focusOverlayItem = nullptr;
    } else {
        m_obs->focusOverlayItem = nullptr;
    }
    if (m_obs->micSource != nullptr) {
        obs_source_release(m_obs->micSource);
        m_obs->micSource = nullptr;
    }
    if (m_obs->audioSource != nullptr) {
        obs_source_release(m_obs->audioSource);
        m_obs->audioSource = nullptr;
    }
    if (m_obs->videoSource != nullptr) {
        obs_source_release(m_obs->videoSource);
        m_obs->videoSource = nullptr;
    }
    m_obs->videoItem = nullptr;
    if (m_obs->scene != nullptr) {
        if (m_obs->canvas != nullptr) {
            obs_canvas_scene_remove(m_obs->scene);
        }
        obs_scene_release(m_obs->scene);
        m_obs->scene = nullptr;
    }
    if (m_obs->canvas != nullptr) {
        obs_canvas_release(m_obs->canvas);
        m_obs->canvas = nullptr;
    }
}

void RtmpStreamer::emitDiagnostics()
{
    emit diagnosticsChanged(diagnosticSummary(), m_obs && m_obs->output != nullptr
                                                 && obs_output_get_total_bytes(m_obs->output) > 0);
}

void RtmpStreamer::handleOutputStopped(int code)
{
    DebugLog::writeCategory(QStringLiteral("OBS"), QStringLiteral("output stopped code=%1 running=%2 stopping=%3")
                            .arg(code)
                            .arg(m_running ? QStringLiteral("true") : QStringLiteral("false"),
                                 m_stopping ? QStringLiteral("true") : QStringLiteral("false")));
    if (!m_running) {
        return;
    }

    const bool intentional = m_stopping || code == OBS_OUTPUT_SUCCESS;
    m_diagnosticsTimer.stop();
    m_focusGateTimer.stop();
    releaseObsObjects();
    m_obs.reset();
    m_running = false;
    m_stopping = false;
    m_startedSignalSent = false;

    if (!intentional) {
        emit failed(QStringLiteral("OBS RTMP output stopped unexpectedly (code %1).").arg(code));
    }
    emit stopped();
}


void RtmpStreamer::updateFocusGateOverlay()
{
    if (!m_focusGateEnabled || !m_obs) {
        return;
    }
    if (m_obs->focusOverlayItem == nullptr && m_obs->videoItem == nullptr) {
        return;
    }

    QString details;
    const bool focused = foregroundMatchesWindowHints(m_windowHints, &details);
    const bool shouldBlack = !focused;
    if (shouldBlack == m_focusGateBlack) {
        return;
    }

    m_focusGateBlack = shouldBlack;
    if (m_obs->focusOverlayItem != nullptr) {
        obs_sceneitem_set_visible(m_obs->focusOverlayItem, shouldBlack);
    } else if (m_obs->videoItem != nullptr) {
        // Fallback for minimal OBS runtimes that do not ship a color source:
        // hiding every video item leaves the private OBS canvas black, while
        // audio continues. This keeps Game only usable without exposing the
        // desktop when the game is not focused.
        obs_sceneitem_set_visible(m_obs->videoItem, !shouldBlack);
    }
    DebugLog::writeCategory(QStringLiteral("OBS"),
                            QStringLiteral("focus privacy gate %1 via %2 (%3)")
                                .arg(shouldBlack ? QStringLiteral("black") : QStringLiteral("clear"),
                                     m_obs->focusOverlayItem != nullptr ? QStringLiteral("overlay") : QStringLiteral("video-source-visibility"),
                                     details));
}

QString RtmpStreamer::diagnosticSummary() const
{
    if (!m_obs || m_obs->output == nullptr) {
        return QStringLiteral("local: OBS output not running");
    }

    const int frames = obs_output_get_total_frames(m_obs->output);
    const int dropped = obs_output_get_frames_dropped(m_obs->output);
    const quint64 bytes = static_cast<quint64>(obs_output_get_total_bytes(m_obs->output));
    const float congestion = obs_output_get_congestion(m_obs->output);
    const QString programAudio = activeAudioSourceId().isEmpty()
                                     ? QStringLiteral("none/private-source-audio")
                                     : activeAudioSourceId();
    const QString microphoneAudio = (!m_obs || m_obs->micSourceId.isEmpty())
                                      ? QStringLiteral("none")
                                      : m_obs->micSourceId;
    return QStringLiteral("local: OBS RTMP active; color=%10 video=%1 audio=%2 mic=%3 encoder=%4/%5 frames=%6 dropped=%7 bytes=%8 congestion=%9%")
        .arg(activeVideoSourceId().isEmpty() ? QStringLiteral("unknown") : activeVideoSourceId(),
             programAudio,
             microphoneAudio,
             activeVideoEncoderId().isEmpty() ? QStringLiteral("unknown") : activeVideoEncoderId(),
             activeAudioEncoderId().isEmpty() ? QStringLiteral("unknown") : activeAudioEncoderId())
        .arg(frames)
        .arg(dropped)
        .arg(bytes)
        .arg(qRound(congestion * 100.0f))
        .arg(m_colorMode == VideoColorMode::HdrPQ ? QStringLiteral("HDR/P010/Rec.2100 HLG")
                                                  : QStringLiteral("SDR/NV12/Rec.709"));
}

QString RtmpStreamer::activeVideoSourceId() const
{
    return m_obs ? m_obs->videoSourceId : QString();
}

QString RtmpStreamer::activeAudioSourceId() const
{
    return m_obs ? m_obs->audioSourceId : QString();
}

QString RtmpStreamer::activeVideoEncoderId() const
{
    return m_obs ? m_obs->videoEncoderId : QString();
}

QString RtmpStreamer::activeAudioEncoderId() const
{
    return m_obs ? m_obs->audioEncoderId : QString();
}
