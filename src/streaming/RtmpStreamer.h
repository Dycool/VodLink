#pragma once

#include "streaming/obs/ObsRuntime.h"

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <memory>

enum class VideoColorMode { Sdr709, HdrPQ };

class RtmpStreamer final : public QObject
{
    Q_OBJECT

public:
    enum class CaptureMode { GameWindow, FullDesktop };
    enum class AudioCaptureSource { GameOnly, System };

    explicit RtmpStreamer(QObject *parent = nullptr);
    ~RtmpStreamer() override;

    void setRecorderSettings(const QString &encoderPreference, int bitrateKbps,
                             const QSize &outputSize, int fps);
    // Streamlabs/OBS-style lifecycle: initialize libobs once while the app is
    // idle, then reuse that runtime for every game session. This moves the
    // native OBS startup/module-load/GPU init work away from game detection.
    bool warmUp(QString *error);
    void shutdownRuntime();
    bool start(const QUrl &ingestUrl, CaptureMode mode, AudioCaptureSource audioSource,
               const QStringList &windowHints, QString *error);
    void stop();
    [[nodiscard]] bool isStreaming() const;

signals:
    void started();
    void stopped();
    void failed(const QString &message);
    // User-safe recorder telemetry. Never includes the YouTube stream key.
    void diagnosticsChanged(const QString &message, bool localPacketsWritten);

private slots:
    void emitDiagnostics();
    void handleOutputStopped(int code);
    void updateFocusGateOverlay();

private:
    struct ObsHandles;

    bool initializeObs(QString *error);
    bool configureObsVideoAudio(QString *error);
    void shutdownObsRuntime();
    bool createScene(CaptureMode mode, AudioCaptureSource audioSource, QString *error);
    bool createEncoders(QString *error);
    bool createOutput(const QUrl &ingestUrl, QString *error);
    bool startOutput(QString *error);
    void ensureCaptureSourceReady();
    void cleanupObs();
    void releaseObsObjects();

    [[nodiscard]] QString diagnosticSummary() const;
    [[nodiscard]] QString activeVideoSourceId() const;
    [[nodiscard]] QString activeAudioSourceId() const;
    [[nodiscard]] QString activeVideoEncoderId() const;
    [[nodiscard]] QString activeAudioEncoderId() const;

    ObsRuntime m_runtime;
    std::unique_ptr<ObsHandles> m_obs;

    QUrl m_ingestUrl;
    CaptureMode m_captureMode = CaptureMode::FullDesktop;
    AudioCaptureSource m_audioSource = AudioCaptureSource::GameOnly;
    QStringList m_windowHints;
    QString m_encoderPreference = QStringLiteral("H.264");
    int m_bitrateKbps = 12000;
    QSize m_outputSize = QSize(1920, 1080);
    int m_fps = 60;
    VideoColorMode m_colorMode = VideoColorMode::Sdr709;

    bool m_running = false;
    bool m_stopping = false;
    bool m_startedSignalSent = false;
    bool m_obsStarted = false;
    bool m_runtimeVideoConfigured = false;
    QSize m_runtimeOutputSize;
    int m_runtimeFps = 0;
    VideoColorMode m_runtimeColorMode = VideoColorMode::Sdr709;
    QTimer m_diagnosticsTimer;
    QTimer m_focusGateTimer;
    bool m_focusGateEnabled = false;
    bool m_focusGateBlack = false;
    QStringList m_captureFallbackCandidates;
    QString m_captureWindowSpec;
    QString m_captureMonitorId;
    int m_captureFallbackIndex = -1;
    int m_captureEmptyFrameChecks = 0;
    quint64 m_renderFramesBaseline = 0;
    quint64 m_renderLagBaseline = 0;
    quint32 m_encodedFramesBaseline = 0;
    quint32 m_encoderSkippedBaseline = 0;
};

using CaptureMode = RtmpStreamer::CaptureMode;
using AudioCaptureSource = RtmpStreamer::AudioCaptureSource;
