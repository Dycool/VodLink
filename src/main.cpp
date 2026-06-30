#include "app/AppController.h"
#include "app/DebugLog.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPalette>
#include <QPropertyAnimation>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <ShObjIdl_core.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif
#include <QStyleFactory>

#if defined(Q_OS_WIN)
#include <string>
// Ask NVIDIA Optimus and AMD PowerXpress to start VodLink on the dedicated GPU.
// This affects the current process early enough for Qt WebEngine/OBS module load,
// while the registry preference below makes Windows remember the same choice for
// future launches.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {
#ifndef VODLINK_BUILD_COMMIT
#define VODLINK_BUILD_COMMIT "dev"
#endif
#ifndef VODLINK_RELEASE_TAG
#define VODLINK_RELEASE_TAG ""
#endif

#if defined(Q_OS_WIN)
QByteArray configureQtWebEngineHardwareVideoDecoding()
{
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS").trimmed();

    // Hardware video decoding is Chromium's default on Windows. Remove the
    // opt-out if it was inherited from the user's environment, and allow the
    // D3D11 decoder on GPUs that Chromium's conservative blocklist rejects.
    flags.replace("--disable-accelerated-video-decode", "");
    for (const QByteArray &required : {
             QByteArrayLiteral("--ignore-gpu-blocklist"),
             QByteArrayLiteral("--enable-gpu-rasterization"),
             QByteArrayLiteral("--enable-zero-copy")}) {
        if (!flags.contains(required)) {
            if (!flags.isEmpty() && !flags.endsWith(' ')) {
                flags.append(' ');
            }
            flags.append(required);
        }
    }
    flags = flags.simplified();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    return flags;
}
#endif

// A dark palette so widgets that fall back to default platform colors (QDialog,
// QMessageBox, QScrollArea viewports, combo-box popups, …) render dark instead of
// the platform white. The per-widget stylesheet still styles the chrome on top.
QPalette darkPalette()
{
    QPalette palette;
    const QColor base(8, 13, 22);
    const QColor window(11, 17, 28);
    const QColor text(237, 242, 251);
    const QColor disabled(116, 125, 145);
    const QColor accent(124, 77, 255);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(255, 107, 125));
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::PlaceholderText, disabled);

    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return palette;
}

constexpr auto kSingleInstanceKey = "VodLinkSingleInstance";

// Returns true if another VodLink instance is already running. When one is found,
// it optionally pokes the existing instance so it can surface its window.
bool anotherInstanceRunning(bool requestShow)
{
    QLocalSocket socket;
    socket.connectToServer(QString::fromLatin1(kSingleInstanceKey));
    if (socket.waitForConnected(200)) {
        socket.write(requestShow ? "show" : "noop");
        socket.flush();
        socket.waitForBytesWritten(200);
        socket.disconnectFromServer();
        return true;
    }
    return false;
}

#if defined(Q_OS_WIN)
bool preferHighPerformanceGpuForFutureLaunches(QString *error)
{
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (exePath.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink executable path is empty.");
        }
        return false;
    }

    HKEY key = nullptr;
    const wchar_t *subKey = L"SOFTWARE\\Microsoft\\DirectX\\UserGpuPreferences";
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("RegCreateKeyExW(UserGpuPreferences) failed: %1").arg(result);
        }
        return false;
    }

    const std::wstring valueName = exePath.toStdWString();
    const wchar_t value[] = L"GpuPreference=2;"; // 2 = Windows Graphics Settings: High performance.
    result = RegSetValueExW(key, valueName.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE *>(value),
                            static_cast<DWORD>(sizeof(value)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("RegSetValueExW(UserGpuPreferences) failed: %1").arg(result);
        }
        return false;
    }
    return true;
}

void scheduleReleaseUpdateCheck(QApplication *application, AppController *controller,
                                bool startMinimized)
{
    const QString currentTag = QString::fromUtf8(VODLINK_RELEASE_TAG).trimmed();
    if (currentTag.isEmpty()) {
        DebugLog::writeCategory(QStringLiteral("updater"),
                                QStringLiteral("unreleased build %1; public release check disabled")
                                    .arg(QString::fromUtf8(VODLINK_BUILD_COMMIT)));
        return;
    }

    auto *network = new QNetworkAccessManager(application);
    QTimer::singleShot(5000, network, [network, application, controller, currentTag, startMinimized] {
        QNetworkRequest request(QUrl(QStringLiteral(
            "https://api.github.com/repos/Dycool/VodLink/releases/latest")));
        request.setRawHeader(QByteArrayLiteral("Accept"),
                             QByteArrayLiteral("application/vnd.github+json"));
        request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("VodLink-Updater"));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *metadataReply = network->get(request);
        QObject::connect(metadataReply, &QNetworkReply::finished, network,
                         [network, application, controller, currentTag, startMinimized, metadataReply] {
            const QByteArray metadata = metadataReply->readAll();
            const auto metadataError = metadataReply->error();
            metadataReply->deleteLater();
            if (metadataError != QNetworkReply::NoError) {
                DebugLog::writeCategory(QStringLiteral("updater"),
                                        QStringLiteral("latest release check failed"));
                return;
            }
            const QJsonObject release = QJsonDocument::fromJson(metadata).object();
            const QString latestTag = release.value(QStringLiteral("tag_name")).toString().trimmed();
            if (latestTag.isEmpty() || latestTag == currentTag) {
                return;
            }
            if (controller->isRecording()) {
                DebugLog::writeCategory(QStringLiteral("updater"),
                                        QStringLiteral("update deferred until the next launch because a stream is active"));
                return;
            }

            QJsonObject installerAsset;
            for (const QJsonValue &value : release.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString()
                    == QStringLiteral("VodLink-Windows-x64.exe")) {
                    installerAsset = asset;
                    break;
                }
            }
            const QUrl downloadUrl(installerAsset.value(QStringLiteral("browser_download_url")).toString());
            const QString digest = installerAsset.value(QStringLiteral("digest")).toString();
            if (!downloadUrl.isValid() || !digest.startsWith(QStringLiteral("sha256:"))) {
                DebugLog::writeCategory(QStringLiteral("updater"),
                                        QStringLiteral("latest release has no verifiable Windows installer"));
                return;
            }

            QNetworkRequest downloadRequest(downloadUrl);
            downloadRequest.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("VodLink-Updater"));
            downloadRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                         QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *downloadReply = network->get(downloadRequest);
            QObject::connect(downloadReply, &QNetworkReply::finished, network,
                             [application, controller, latestTag, digest, startMinimized, downloadReply] {
                const QByteArray installer = downloadReply->readAll();
                const auto downloadError = downloadReply->error();
                downloadReply->deleteLater();
                const QString actualDigest = QStringLiteral("sha256:")
                    + QString::fromLatin1(QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex());
                if (downloadError != QNetworkReply::NoError
                    || actualDigest.compare(digest, Qt::CaseInsensitive) != 0) {
                    DebugLog::writeCategory(QStringLiteral("updater"),
                                            QStringLiteral("release installer download or digest verification failed"));
                    return;
                }
                QString safeTag = latestTag;
                safeTag.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
                const QString directory = QDir(QStandardPaths::writableLocation(
                    QStandardPaths::TempLocation)).filePath(QStringLiteral("VodLink-Updater/%1").arg(safeTag));
                if (!QDir().mkpath(directory)) return;
                const QString installerPath = QDir(directory).filePath(QStringLiteral("VodLink-Windows-x64.exe"));
                QFile file(installerPath);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    || file.write(installer) != installer.size()) {
                    return;
                }
                file.close();
                QStringList updaterArguments{QStringLiteral("--update-background")};
                if (startMinimized) {
                    updaterArguments.append(QStringLiteral("--launch-minimized"));
                }
                if (QProcess::startDetached(installerPath, updaterArguments)) {
                    DebugLog::writeCategory(QStringLiteral("updater"),
                                            QStringLiteral("handing off update from %1 to %2")
                                                .arg(QString::fromUtf8(VODLINK_RELEASE_TAG), latestTag));
                    controller->shutdownForQuit();
                    application->quit();
                }
            });
        });
    });
}
#endif
} // namespace

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    // Qt WebEngine reads Chromium flags while QApplication is being created.
    // Setting them afterwards is too late to affect its GPU/media processes.
    const QByteArray webEngineFlags = configureQtWebEngineHardwareVideoDecoding();
#endif
    QApplication application(argc, argv);
    // Keep QStandardPaths app-specific folders from becoming VodLink/VodLink.
    // VodLink stores its own data via AppPaths in a single local root:
    // %LOCALAPPDATA%/VodLink on Windows.
    QCoreApplication::setOrganizationName(QString());
    QCoreApplication::setApplicationName(QStringLiteral("VodLink"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("VodLink"));
#if defined(Q_OS_WIN)
    SetCurrentProcessExplicitAppUserModelID(L"VodLink.VodLink");
#endif
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/vodlink.png")));

    const QStringList arguments = QCoreApplication::arguments();
    QString debugLogError;
    if (!DebugLog::initializeFromArguments(arguments, &debugLogError)) {
        QMessageBox::critical(nullptr, QStringLiteral("VodLink debug log failed"), debugLogError);
        return 1;
    }
    DebugLog::write(QStringLiteral("main: QApplication created"));
#if defined(Q_OS_WIN)
    DebugLog::writeCategory(QStringLiteral("GPU"),
                            QStringLiteral("Qt WebEngine Chromium flags=%1")
                                .arg(QString::fromUtf8(webEngineFlags)));
    QString gpuPreferenceError;
    if (preferHighPerformanceGpuForFutureLaunches(&gpuPreferenceError)) {
        DebugLog::writeCategory(QStringLiteral("GPU"),
                                QStringLiteral("Windows high-performance GPU preference is set for future VodLink launches"));
    } else {
        DebugLog::writeCategory(QStringLiteral("GPU"),
                                QStringLiteral("Could not set Windows high-performance GPU preference: %1")
                                    .arg(gpuPreferenceError));
    }
#endif

    const bool startMinimized = arguments.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)
        || arguments.contains(QStringLiteral("--startup"), Qt::CaseInsensitive);

    // Fusion renders consistently across platforms and honors the palette below,
    // unlike the native Windows style which ignores many palette roles.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }
    QApplication::setPalette(darkPalette());

    // Only one VodLink may run at a time: a second launch hands off to the first
    // (which raises its window) and exits.
    if (anotherInstanceRunning(!startMinimized)) {
        DebugLog::write(QStringLiteral("main: another VodLink instance is already running; exiting"));
        DebugLog::shutdown();
        return 0;
    }
    QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));
    QLocalServer instanceServer;
    instanceServer.listen(QString::fromLatin1(kSingleInstanceKey));

    // Closing the window minimises VodLink to the system tray instead of exiting,
    // so it keeps watching for games in the background. Quit from the tray menu.
    QApplication::setQuitOnLastWindowClosed(false);

    AppController controller;
    QString error;
    DebugLog::write(QStringLiteral("main: initializing AppController"));
    if (!controller.initialize(&error)) {
        DebugLog::writeCategory(QStringLiteral("error"), QStringLiteral("AppController initialize failed: %1").arg(error));
        QMessageBox::critical(nullptr, QStringLiteral("VodLink could not start"), error);
        DebugLog::shutdown();
        return 1;
    }
    DebugLog::write(QStringLiteral("main: AppController initialized"));

    MainWindow window(&controller);
    window.resize(1180, 760);
    if (startMinimized) {
        window.showInitial(true);
    } else {
        window.setWindowOpacity(0.0);
        window.showInitial(false);

        auto *fadeIn = new QPropertyAnimation(&window, "windowOpacity", &window);
        fadeIn->setDuration(220);
        fadeIn->setStartValue(0.0);
        fadeIn->setEndValue(1.0);
        fadeIn->setEasingCurve(QEasingCurve::OutCubic);
        fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // A second launch connects to instanceServer to ask us to come to the front.
    QObject::connect(&instanceServer, &QLocalServer::newConnection, &window, [&instanceServer, &window] {
        while (QLocalSocket *client = instanceServer.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::readyRead, &window, [client, &window] {
                const QByteArray command = client->readAll().trimmed();
                if (command == QByteArrayLiteral("show")) {
                    window.showNormal();
                    window.raise();
                    window.activateWindow();
                }
            });
            QObject::connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        }
    });

    DebugLog::write(QStringLiteral("main: startMonitoring"));
    controller.startMonitoring();
#if defined(Q_OS_WIN)
    scheduleReleaseUpdateCheck(&application, &controller, startMinimized);
#endif
    const int exitCode = application.exec();
    DebugLog::write(QStringLiteral("main: QApplication exited with code %1").arg(exitCode));
    DebugLog::shutdown();
    return exitCode;
}
