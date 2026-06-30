#include "MainWindow.h"

#include "app/AppController.h"
#include "player/SyncPlayer.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEasingCurve>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace {
constexpr auto kEncoderSetting = "recorder_encoder";
constexpr auto kBitrateSetting = "recorder_bitrate_kbps";
constexpr auto kResolutionSetting = "recorder_resolution";
constexpr auto kFpsSetting = "recorder_fps";
constexpr auto kNotificationsSetting = "notifications";
constexpr auto kLaunchAtStartupSetting = "launch_at_startup";
constexpr auto kPrivacyGameOnly = "game_only";
constexpr auto kPrivacyGameExternalAudio = "game_external_audio";
constexpr auto kPrivacyDesktop = "desktop";
constexpr auto kPrivacyFullDesktop = "full_desktop";
constexpr auto kStartupRunValue = "VodLink";

QString quotedStartupCommand()
{
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --minimized").arg(executable);
}

QString xmlEscaped(QString value)
{
    value.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    value.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    value.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    value.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    value.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    return value;
}

QString desktopQuoted(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

QString macLaunchAgentPath()
{
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/app.vodlink.VodLink.plist"));
}

QString linuxAutostartPath()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(config.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : config)
        .filePath(QStringLiteral("autostart/vodlink.desktop"));
}

bool launchAtStartupSupported()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

bool launchAtStartupEnabled()
{
#if defined(Q_OS_WIN)
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                     QSettings::NativeFormat);
    const QString value = runKey.value(QString::fromLatin1(kStartupRunValue)).toString();
    return value.contains(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), Qt::CaseInsensitive)
        && value.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive);
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QFile file(macLaunchAgentPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QString content = QString::fromUtf8(file.readAll());
    return content.contains(QCoreApplication::applicationFilePath())
        && content.contains(QStringLiteral("--minimized"));
#elif defined(Q_OS_LINUX)
    QFile file(linuxAutostartPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QString content = QString::fromUtf8(file.readAll());
    return content.contains(QCoreApplication::applicationFilePath())
        && content.contains(QStringLiteral("--minimized"))
        && !content.contains(QStringLiteral("Hidden=true"));
#else
    return false;
#endif
}


constexpr qint64 kThumbnailRefreshWindowMs = 6LL * 60LL * 60LL * 1000LL;
constexpr qint64 kThumbnailEarlyRefreshMs = 2LL * 60LL * 1000LL;
constexpr qint64 kThumbnailLateRefreshMs = 10LL * 60LL * 1000LL;
constexpr qint64 kThumbnailEarlyWindowMs = 60LL * 60LL * 1000LL;

qint64 vodEndedAtMs(const Vod &vod)
{
    if (!vod.startedAt.isValid() || vod.durationMs <= 0) {
        return 0;
    }
    return vod.startedAt.toUTC().toMSecsSinceEpoch() + vod.durationMs;
}

bool shouldRefreshVodThumbnail(const Vod &vod, qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
{
    if (vod.youtubeId.trimmed().isEmpty()) {
        return false;
    }
    const qint64 endedAt = vodEndedAtMs(vod);
    if (endedAt <= 0) {
        // Live/unknown-duration VODs can still receive new generated posters.
        return true;
    }
    return nowMs >= endedAt && (nowMs - endedAt) < kThumbnailRefreshWindowMs;
}

qint64 thumbnailRefreshIntervalMs(const Vod &vod, qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
{
    const qint64 endedAt = vodEndedAtMs(vod);
    if (endedAt <= 0 || (nowMs - endedAt) < kThumbnailEarlyWindowMs) {
        return kThumbnailEarlyRefreshMs;
    }
    return kThumbnailLateRefreshMs;
}

qint64 thumbnailProbeBackoffMs(const Vod &vod, int unchangedCount, qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
{
    const qint64 base = thumbnailRefreshIntervalMs(vod, nowMs);
    const int multiplier = std::clamp(unchangedCount + 1, 1, 4);
    const qint64 cap = vodEndedAtMs(vod) > 0 ? (30LL * 60LL * 1000LL) : (10LL * 60LL * 1000LL);
    return std::min(base * multiplier, cap);
}

QByteArray thumbnailBytesHash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QString thumbnailCacheKey(const Vod &vod, qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
{
    const QString youtubeId = vod.youtubeId.trimmed();
    if (!shouldRefreshVodThumbnail(vod, nowMs)) {
        return youtubeId;
    }
    const qint64 bucket = nowMs / std::max<qint64>(1, thumbnailRefreshIntervalMs(vod, nowMs));
    return QStringLiteral("%1|fresh|%2").arg(youtubeId, QString::number(bucket));
}

QUrl thumbnailUrl(const QString &youtubeId, bool refresh, qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
{
    QUrl url(QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(youtubeId));
    if (refresh) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("vodlink"), QString::number(nowMs / (2LL * 60LL * 1000LL)));
        url.setQuery(query);
    }
    return url;
}

bool setLaunchAtStartupEnabled(bool enabled, QString *error)
{
#if defined(Q_OS_WIN)
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                     QSettings::NativeFormat);
    if (enabled) {
        runKey.setValue(QString::fromLatin1(kStartupRunValue), quotedStartupCommand());
    } else {
        runKey.remove(QString::fromLatin1(kStartupRunValue));
    }
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("Windows rejected the startup registry change.");
        }
        return false;
    }
    return true;
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QString path = macLaunchAgentPath();
    if (!enabled) {
        QFile::remove(path);
        return true;
    }

    QDir dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the macOS LaunchAgents directory.");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not write the macOS startup item.");
        }
        return false;
    }

    QTextStream out(&file);
    out << QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>app.vodlink.VodLink</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>%1</string>\n"
        "    <string>--minimized</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "</dict>\n"
        "</plist>\n").arg(xmlEscaped(QCoreApplication::applicationFilePath()));
    return true;
#elif defined(Q_OS_LINUX)
    const QString path = linuxAutostartPath();
    if (!enabled) {
        QFile::remove(path);
        return true;
    }

    QDir dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the Linux autostart directory.");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not write the Linux autostart entry.");
        }
        return false;
    }

    QTextStream out(&file);
    out << QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=VodLink\n"
        "Comment=Start VodLink minimized\n"
        "Exec=%1 --minimized\n"
        "Icon=vodlink\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n").arg(desktopQuoted(QCoreApplication::applicationFilePath()));
    return true;
#else
    Q_UNUSED(enabled);
    if (error != nullptr) {
        *error = QStringLiteral("Launch on startup is not supported on this platform.");
    }
    return false;
#endif
}

QString cssCard(const QString &extra = {})
{
    return QStringLiteral(
               "background: rgba(15, 21, 32, 0.86);"
               "border: 1px solid rgba(120, 133, 160, 0.18);"
               "border-radius: 16px;")
        + extra;
}

void addShadow(QWidget *widget, int blur = 28, int alpha = 90)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(blur);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(0, 0, 0, alpha));
    widget->setGraphicsEffect(shadow);
}

// Crops a (possibly square) avatar into a circle so profile pictures look like
// Google's round avatars instead of squares.
QPixmap circularPixmap(const QPixmap &src, int size)
{
    QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}

// Largest pixel size (within [minPx, maxPx]) at which text fits maxWidth, so long
// emails shrink to fit the friend row instead of being clipped.
int fittedFontPx(const QString &text, int maxWidth, int maxPx, int minPx)
{
    QFont font;
    for (int px = maxPx; px > minPx; --px) {
        font.setPixelSize(px);
        if (QFontMetrics(font).horizontalAdvance(text) <= maxWidth) {
            return px;
        }
    }
    return minPx;
}

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("color:#8e98ad;"));
    label->setWordWrap(true);
    return label;
}

void makeOpaque(QWidget *widget, const QColor &color = QColor(7, 11, 18))
{
    if (widget == nullptr) {
        return;
    }

    widget->setAttribute(Qt::WA_StyledBackground, true);
    widget->setAutoFillBackground(true);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, color);
    palette.setColor(QPalette::Base, color);
    widget->setPalette(palette);
}

QLabel *sectionLabel(const QString &text, int size = 18)
{
    auto *label = new QLabel(text);
    // Size via stylesheet — the global "QWidget { font-size }" rule wins over QFont.
    label->setStyleSheet(QStringLiteral("color:#f4f7fb; font-size:%1px; font-weight:700;").arg(size));
    return label;
}

QString normalizeTitle(const Vod &vod)
{
    if (!vod.title.trimmed().isEmpty()) {
        return vod.title.trimmed();
    }
    if (!vod.game.trimmed().isEmpty()) {
        return QStringLiteral("%1 VOD").arg(vod.game);
    }
    return QStringLiteral("Untitled VOD");
}

QString ownerText(const Vod &vod)
{
    if (!vod.ownerEmail.trimmed().isEmpty()) {
        if (!vod.ownerName.trimmed().isEmpty()) {
            return vod.ownerName.trimmed();
        }
        return vod.ownerEmail.trimmed();
    }
    if (!vod.accountEmail.trimmed().isEmpty()) {
        return vod.accountEmail.trimmed();
    }
    return QStringLiteral("Local VOD");
}

QString friendDisplayName(const AccountProfile &profile)
{
    // The real Google name only arrives once the friendship is mutual and a shared
    // session is matched (the Worker gates that). Until then, show the email.
    const QString name = profile.displayName.trimmed();
    return name.isEmpty() ? profile.email.trimmed() : name;
}

QString settingOr(const QString &value, const QString &fallback)
{
    return value.trimmed().isEmpty() ? fallback : value.trimmed();
}

// Resolution options offered in Settings: the native pixel size of every connected
// monitor, plus standard resolutions that fit inside the largest one. This way a
// 3440x1440 ultrawide shows up as a real option instead of arbitrary presets.
QSize nativeRecorderResolution()
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (!screens.isEmpty()) {
            screen = screens.first();
        }
    }

    if (screen == nullptr) {
        return QSize(1920, 1080);
    }

    const qreal ratio = screen->devicePixelRatio();
    QSize native(qRound(screen->geometry().width() * ratio),
                 qRound(screen->geometry().height() * ratio));
    if (native.width() < 640 || native.height() < 360) {
        return QSize(1920, 1080);
    }

    native.setWidth(native.width() & ~1);
    native.setHeight(native.height() & ~1);
    return native.isEmpty() ? QSize(1920, 1080) : native;
}

QString nativeRecorderResolutionText()
{
    const QSize size = nativeRecorderResolution();
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

bool parseResolutionText(const QString &text, QSize *out)
{
    const QString normalized = text.trimmed().toLower();
    const QStringList parts = normalized.split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return false;
    }

    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).trimmed().toInt(&widthOk);
    const int height = parts.at(1).trimmed().toInt(&heightOk);
    if (!widthOk || !heightOk || width < 640 || height < 360
        || (width % 2) != 0 || (height % 2) != 0) {
        return false;
    }

    if (out != nullptr) {
        *out = QSize(width, height);
    }
    return true;
}

QString normalizedResolutionText(const QString &text)
{
    QSize size;
    if (!parseResolutionText(text, &size)) {
        return QString();
    }
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QStringList availableResolutions(const QString &saved)
{
    QList<QSize> sizes;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        const qreal ratio = screen->devicePixelRatio();
        const QSize native(qRound(screen->geometry().width() * ratio),
                           qRound(screen->geometry().height() * ratio));
        if (native.width() < 640 || native.height() < 480) {
            continue;
        }
        sizes.append(native);
    }

    const QList<QSize> standard = {
        {3840, 2160}, {3440, 1440}, {2560, 1440}, {2560, 1080},
        {1920, 1080}, {1600, 900},  {1366, 768},  {1280, 720}};
    for (const QSize &size : standard) {
        sizes.append(size);
    }

    QStringList result;
    auto addUnique = [&result](const QSize &size) {
        const QString text = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
        if (!result.contains(text)) {
            result.append(text);
        }
    };
    addUnique(nativeRecorderResolution());
    std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
        return a.width() != b.width() ? a.width() > b.width() : a.height() > b.height();
    });
    for (const QSize &size : sizes) {
        addUnique(size);
    }
    // Never drop the user's saved choice, even if no current monitor matches it.
    if (!saved.trimmed().isEmpty() && !result.contains(saved.trimmed())) {
        result.prepend(saved.trimmed());
    }
    if (result.isEmpty()) {
        result.append(QStringLiteral("1920x1080"));
    }
    return result;
}

// True for codecs that need less bitrate than H.264 for the same quality.
bool isEfficientCodec(const QString &encoderText)
{
    const QString t = encoderText.toLower();
    return t.contains(QStringLiteral("av1")) || t.contains(QStringLiteral("265"))
        || t.contains(QStringLiteral("hevc"));
}

// YouTube's recommended ingest bitrate (kbps) for a resolution/frame rate.
// Uses the H.264 "Recommended" column; for AV1/HEVC it clamps into the lower
// AV1/H.265 min–max range. See YouTube Live encoder settings.
int youtubeQualityTierHeight(const QString &resolution)
{
    const QStringList parts = resolution.split(QLatin1Char('x'));
    const int width = parts.size() == 2 ? parts.at(0).toInt() : 1920;
    const int height = parts.size() == 2 ? parts.at(1).toInt() : 1080;
    if (width <= 0 || height <= 0) {
        return 1080;
    }

    // Same tier logic as AppController/YouTubeLiveClient: exact OBS canvas,
    // YouTube ladder selected by total pixels, rounded up to the first
    // standard 16:9 tier that can contain that many pixels. 2160p is the cap.
    const long long pixels = 1LL * width * height;

    if (pixels <= 1LL * 640 * 360) {
        return 360;
    }
    if (pixels <= 1LL * 854 * 480) {
        return 480;
    }
    if (pixels <= 1LL * 1280 * 720) {
        return 720;
    }
    if (pixels <= 1LL * 1920 * 1080) {
        return 1080;
    }
    if (pixels <= 1LL * 2560 * 1440) {
        return 1440;
    }
    return 2160;
}

int recommendedBitrateKbps(const QString &resolution, int fps, bool efficientCodec)
{
    const int height = youtubeQualityTierHeight(resolution);
    const bool high = fps >= 50;

    int h264 = 12000; // 1080p60 default
    int avMin = 4000;
    int avMax = 10000;
    if (height >= 2160) {
        h264 = high ? 35000 : 30000; avMin = high ? 10000 : 8000; avMax = high ? 40000 : 35000;
    } else if (height >= 1440) {
        h264 = high ? 24000 : 15000; avMin = high ? 6000 : 5000; avMax = high ? 30000 : 25000;
    } else if (height >= 1080) {
        h264 = high ? 12000 : 10000; avMin = high ? 4000 : 3000; avMax = high ? 10000 : 8000;
    } else if (height >= 720) {
        h264 = high ? 6000 : 4000; avMin = 3000; avMax = 8000;
    } else {
        h264 = 4000; avMin = 3000; avMax = 8000;
    }

    if (!efficientCodec) {
        return h264;
    }
    return std::clamp(h264, avMin, avMax);
}


QString privacyLabelForMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QString::fromLatin1(kPrivacyGameExternalAudio)) {
        return QStringLiteral("Game and external audio");
    }
    if (normalized == QString::fromLatin1(kPrivacyDesktop)) {
        return QStringLiteral("Desktop");
    }
    if (normalized == QString::fromLatin1(kPrivacyFullDesktop)) {
        return QStringLiteral("Desktop with external audio");
    }
    return QStringLiteral("Game only");
}

QString privacyModeForLabel(const QString &label)
{
    if (label == QStringLiteral("Game and external audio")) {
        return QString::fromLatin1(kPrivacyGameExternalAudio);
    }
    if (label == QStringLiteral("Desktop")) {
        return QString::fromLatin1(kPrivacyDesktop);
    }
    if (label == QStringLiteral("Desktop with external audio")) {
        return QString::fromLatin1(kPrivacyFullDesktop);
    }
    return QString::fromLatin1(kPrivacyGameOnly);
}

QString normalizedEncoderLabel(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized.contains(QStringLiteral("av1"))) {
        return QStringLiteral("AV1");
    }
    if (normalized.contains(QStringLiteral("hevc")) || normalized.contains(QStringLiteral("h265"))
        || normalized.contains(QStringLiteral("265"))) {
        return QStringLiteral("HEVC");
    }
    if (normalized.contains(QStringLiteral("h264")) || normalized.contains(QStringLiteral("x264"))
        || normalized.contains(QStringLiteral("264"))) {
        return QStringLiteral("H.264");
    }
    return QStringLiteral("H.264");
}

QString noHardwareEncoderChoice()
{
    return QStringLiteral("No hardware encoder found");
}

bool isNoHardwareEncoderChoice(const QString &text)
{
    return text == noHardwareEncoderChoice();
}

QString gpuDescriptionForSettingsProbe()
{
#if defined(Q_OS_WIN)
    QProcess process;
    process.start(QStringLiteral("wmic"), {QStringLiteral("path"), QStringLiteral("win32_VideoController"),
                                           QStringLiteral("get"), QStringLiteral("name")});
    if (process.waitForFinished(1200)) {
        return QString::fromLocal8Bit(process.readAllStandardOutput()).toLower();
    }
#elif defined(Q_OS_LINUX)
    QProcess process;
    process.start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("lspci 2>/dev/null | grep -Ei 'vga|3d|display' || true")});
    if (process.waitForFinished(1200)) {
        return QString::fromLocal8Bit(process.readAllStandardOutput()).toLower();
    }
#endif
    return {};
}

bool probableHardwareAv1EncoderAvailable()
{
    if (qEnvironmentVariableIntValue("VODLINK_FORCE_AV1_SETTINGS") == 1) {
        return true;
    }

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    // Apple currently exposes AV1 decode much more broadly than AV1 encode. Hide
    // AV1 unless a future runtime positively opts in via the env override above.
    return false;
#else
    const QString gpu = gpuDescriptionForSettingsProbe();
    if (gpu.isEmpty()) {
        return false;
    }

    // Keep this deliberately conservative. RTX 30-series cards, including the
    // user's 3070 Ti, have NVENC H.264/HEVC but no AV1 encode. AV1 appears on
    // NVIDIA Ada/Blackwell, Intel Arc, and AMD RDNA3/RX 7000 class hardware.
    return gpu.contains(QStringLiteral("rtx 40"))
        || gpu.contains(QStringLiteral("rtx 50"))
        || gpu.contains(QStringLiteral("geforce rtx 40"))
        || gpu.contains(QStringLiteral("geforce rtx 50"))
        || gpu.contains(QStringLiteral("intel(r) arc"))
        || gpu.contains(QStringLiteral("intel arc"))
        || gpu.contains(QStringLiteral("radeon rx 7"))
        || gpu.contains(QStringLiteral("radeon 780m"))
        || gpu.contains(QStringLiteral("radeon 760m"));
#endif
}

QStringList availableEncoderChoices()
{
    // Do not start libobs from the Settings dialog. Some OBS modules do not like
    // being started/shutdown just for a dropdown probe, and that was causing
    // Settings to crash before the user even recorded anything. Runtime streaming
    // still validates the exact private OBS hardware encoder before going live.
    QStringList choices = {QStringLiteral("H.264"), QStringLiteral("HEVC")};
    if (probableHardwareAv1EncoderAvailable()) {
        choices.append(QStringLiteral("AV1"));
    }
    return choices;
}

class ScopedUpdatesBlocker
{
public:
    explicit ScopedUpdatesBlocker(QWidget *widget)
        : m_widget(widget), m_wasEnabled(widget != nullptr && widget->updatesEnabled())
    {
        if (m_widget != nullptr) {
            m_widget->setUpdatesEnabled(false);
        }
    }

    ~ScopedUpdatesBlocker()
    {
        if (m_widget != nullptr) {
            m_widget->setUpdatesEnabled(m_wasEnabled);
            if (m_wasEnabled) {
                m_widget->update();
            }
        }
    }

private:
    QWidget *m_widget = nullptr;
    bool m_wasEnabled = true;
};

bool vodsOverlap(const Vod &a, const Vod &b)
{
    if (!a.startedAt.isValid() || !b.startedAt.isValid()) {
        return a.youtubeId == b.youtubeId;
    }
    const qint64 aStart = a.startedAt.toMSecsSinceEpoch();
    const qint64 bStart = b.startedAt.toMSecsSinceEpoch();
    constexpr qint64 kUnknownDurationMs = 6 * 60 * 60 * 1000;
    const qint64 aEnd = aStart + (a.durationMs > 0 ? a.durationMs : kUnknownDurationMs);
    const qint64 bEnd = bStart + (b.durationMs > 0 ? b.durationMs : kUnknownDurationMs);
    return aStart <= bEnd && bStart <= aEnd;
}

double clampVodOffset(const Vod &vod, double seconds)
{
    seconds = std::max(0.0, seconds);
    if (vod.durationMs <= 0) {
        // Duration is unknown for still-live friend sessions and for VODs that
        // YouTube has not fully processed yet. Do not collapse those to 1s, or
        // linked VOD sync will always jump near the beginning. The YouTube player
        // will clamp naturally if the requested timestamp is beyond the available
        // media.
        return seconds;
    }
    return std::clamp(seconds, 0.0, static_cast<double>(vod.durationMs) / 1000.0);
}

} // namespace

MainWindow::MainWindow(AppController *controller, QWidget *parent)
    : QMainWindow(parent), m_controller(controller),
      m_avatarNetwork(new QNetworkAccessManager(this))
{
    buildUi();
    setupTray();
    qApp->installEventFilter(this);

    connect(m_controller, &AppController::libraryChanged, this, [this] {
        reloadLibrary();
    });
    connect(m_controller, &AppController::statusChanged, this, &MainWindow::updateStatus);
    connect(m_controller, &AppController::accountChanged, this, &MainWindow::onAccountChanged);
    connect(m_controller, &AppController::friendsChanged, this, &MainWindow::reloadFriends);
    connect(m_controller, &AppController::errorOccurred, this, [this](const QString &message) {
        if (m_hasViewerVod && m_deleteVodButton != nullptr) {
            m_deleteVodButton->setEnabled(m_controller->canDeleteVod(m_viewerVod) || !m_viewerVod.ownerEmail.trimmed().isEmpty());
            m_deleteVodButton->setText(!m_viewerVod.ownerEmail.trimmed().isEmpty() ? QStringLiteral("Remove") : QStringLiteral("Delete"));
        }
        QMessageBox::warning(this, QStringLiteral("VodLink"), message);
    });
    connect(m_controller, &AppController::vodDeleted, this, [this](const QString &youtubeId) {
        if (m_hasViewerVod && m_viewerVod.youtubeId == youtubeId) {
            clearVodViewer();
        }
    });
    connect(m_controller, &AppController::autoRecordEnabledChanged, this, [this](bool enabled) {
        updateAutoRecordLabel();
        if (m_autoRecordAction != nullptr) {
            QSignalBlocker blocker(m_autoRecordAction);
            m_autoRecordAction->setChecked(enabled);
        }
    });
    connect(m_controller, &AppController::lastGameChanged, this, [this] { updateFooterIdentity(); });
    connect(m_controller, &AppController::streamingUnavailable, this, [this](const QString &explanation) {
        QMessageBox::warning(this, QStringLiteral("YouTube live streaming unavailable"), explanation);
    });
    connect(m_controller, &AppController::shareEnabledChanged, this, [this](bool enabled) {
        if (m_shareToggle != nullptr) {
            QSignalBlocker blocker(m_shareToggle);
            m_shareToggle->setChecked(enabled);
        }
        if (m_shareAction != nullptr) {
            QSignalBlocker blocker(m_shareAction);
            m_shareAction->setChecked(enabled);
        }
    });

    reloadLibrary();
    reloadFriends();
    onAccountChanged(m_controller->accountEmail());
    updateFooterIdentity();
    updateAuthGate();
}

void MainWindow::buildUi()
{
    makeOpaque(this);
    setWindowTitle(QStringLiteral("VodLink"));
    setWindowIcon(QIcon(QStringLiteral(":/vodlink.png")));
    resize(1580, 900);
    setMinimumSize(1180, 720);

    setStyleSheet(QStringLiteral(R"CSS(
        QMainWindow { background: #070b12; }
        QStackedWidget#RootStack, QWidget#SetupPage, QWidget#MainPage, QWidget#LibrarySurface, QWidget#VodGridWidget, QWidget#VodContentHost, QWidget#VodLibraryLayer { background: #070b12; }
        QScrollArea#VodScroll, QScrollArea#VodScroll > QWidget, QScrollArea#VodScroll > QWidget > QWidget { background: #070b12; border: 0; }
        QWidget { color: #edf2fb; font-family: "Inter", "Segoe UI", Arial, sans-serif; font-size: 14px; }
        QFrame#AppShell { background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #101722, stop:0.52 #08101a, stop:1 #080b11); border: 1px solid rgba(120, 132, 160, 0.23); border-radius: 20px; }
        QFrame#SidePanel, QFrame#StatCard, QFrame#SoftPanel { background: rgba(13, 18, 28, 0.88); border: 1px solid rgba(125, 138, 167, 0.18); border-radius: 16px; }
        QFrame#ViewerPanel { background: rgba(5, 8, 13, 0.98); border: 1px solid rgba(143, 102, 255, 0.38); border-radius: 18px; }
        QLineEdit, QComboBox, QSpinBox { background: rgba(8, 13, 22, 0.9); border: 1px solid rgba(128, 141, 169, 0.22); border-radius: 18px; padding: 9px 16px; selection-background-color: #7c4dff; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid rgba(143, 102, 255, 0.85); }
        QComboBox:hover, QSpinBox:hover { border: 1px solid rgba(143, 102, 255, 0.5); }
        QComboBox::drop-down { border: 0; width: 28px; }
        QComboBox::down-arrow { image: url(:/chevron-down.svg); width: 14px; height: 14px; }
        QComboBox QAbstractItemView { background: #121a26; border: 1px solid rgba(128, 141, 169, 0.24); border-radius: 12px; padding: 6px; outline: 0; selection-background-color: rgba(124, 77, 255, 0.28); }
        QSpinBox { padding-right: 26px; }
        QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 22px; margin: 3px 4px 0px 0px; border-radius: 6px; background: rgba(124, 77, 255, 0.22); }
        QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 22px; margin: 0px 4px 3px 0px; border-radius: 6px; background: rgba(124, 77, 255, 0.22); }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: rgba(143, 102, 255, 0.45); }
        QSpinBox::up-arrow { image: url(:/chevron-up.svg); width: 11px; height: 11px; }
        QSpinBox::down-arrow { image: url(:/chevron-down.svg); width: 11px; height: 11px; }
        QToolButton::menu-indicator { image: none; width: 0; }
        QListWidget { background: transparent; border: 0; outline: 0; }
        QListWidget::item { padding: 10px 8px; border-radius: 12px; }
        QListWidget::item:selected { background: rgba(124, 77, 255, 0.22); }
        QPushButton, QToolButton { background: rgba(124, 77, 255, 0.95); border: 0; border-radius: 10px; padding: 9px 14px; font-weight: 700; }
        QPushButton:hover, QToolButton:hover { background: #8f66ff; }
        QPushButton:pressed, QToolButton:pressed { background: #6437df; }
        QPushButton:disabled, QToolButton:disabled { background: rgba(52, 58, 73, 0.72); color: #747d91; }
        QPushButton#GhostButton, QToolButton#GhostButton { background: rgba(11, 16, 26, 0.75); border: 1px solid rgba(128, 141, 169, 0.22); font-weight: 600; }
        QPushButton#GhostButton:hover, QToolButton#GhostButton:hover { background: rgba(124, 77, 255, 0.18); border: 1px solid rgba(143, 102, 255, 0.7); }
        QPushButton#DangerButton { background: rgba(255, 68, 90, 0.16); color: #ff6b7d; border: 1px solid rgba(255, 68, 90, 0.32); }
        QPushButton#VodCard { text-align: left; background: rgba(13, 19, 30, 0.96); border: 1px solid rgba(128, 141, 169, 0.18); border-radius: 18px; padding: 0px; }
        QPushButton#VodCard:hover { border: 1px solid rgba(138, 92, 255, 0.78); background: rgba(18, 24, 38, 0.98); }
        QPushButton#VodCard[selected="true"] { border: 2px solid #7c4dff; background: rgba(28, 22, 52, 0.92); }
        QFrame#VodThumbFrame { background: rgba(6, 10, 17, 0.95); border: 1px solid rgba(128, 141, 169, 0.16); border-radius: 12px; }
        QLabel#VodThumb { background: #050811; border-radius: 12px; }
        QLabel#VodDurationBadge { background: rgba(5, 7, 12, 0.86); color: #f8fbff; border-radius: 9px; padding: 3px 8px; font-size: 11px; font-weight: 800; }
        QLabel#VodTitle { color: #f6f8fc; font-size: 15px; font-weight: 800; }
        QLabel#VodGame { color: #b7bfd0; font-size: 13px; font-weight: 600; }
        QLabel#VodMeta { color: #8e98ad; font-size: 12px; font-weight: 600; }
        QScrollArea { border: 0; background: transparent; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 4px; }
        QScrollBar::handle:vertical { background: rgba(127, 139, 168, 0.35); border-radius: 5px; }
        QMenu { background: #121a26; color: #f4f7fb; border: 1px solid rgba(128, 141, 169, 0.24); border-radius: 12px; padding: 8px; }
        QMenu::item { padding: 10px 36px 10px 12px; border-radius: 8px; }
        QMenu::item:selected { background: rgba(124, 77, 255, 0.24); }
        QCheckBox { spacing: 10px; }
        QCheckBox::indicator { width: 20px; height: 20px; border-radius: 6px; border: 1px solid rgba(150, 162, 190, 0.55); background: rgba(8, 13, 22, 0.9); }
        QCheckBox::indicator:hover { border: 1px solid rgba(143, 102, 255, 0.85); }
        QCheckBox::indicator:checked { background: #7c4dff; border: 1px solid #7c4dff; image: url(:/check.svg); }
        QCheckBox::indicator:disabled { border: 1px solid rgba(90, 98, 116, 0.5); background: rgba(40, 45, 58, 0.7); }
    )CSS"));

    auto *outer = new QWidget(this);
    outer->setObjectName(QStringLiteral("MainPage"));
    makeOpaque(outer);
    auto *outerLayout = new QVBoxLayout(outer);
    outerLayout->setContentsMargins(20, 18, 20, 18);

    auto *shell = new QFrame;
    shell->setObjectName(QStringLiteral("AppShell"));
    shell->setAttribute(Qt::WA_StyledBackground, true);
    // Keep the main shell free of QGraphicsEffect. On Windows + high-DPI scaling,
    // effects cache the whole widget tree and can leave stale pixels when pages or
    // the friends drawer are hidden/shown.
    auto *shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(12, 12, 12, 12);
    shellLayout->setSpacing(0);

    auto *main = new QHBoxLayout;
    main->setSpacing(18);
    m_friendsPanel = buildFriendsPanel();
    main->addWidget(m_friendsPanel);
    main->addWidget(buildLibrarySurface(), 1);
    shellLayout->addLayout(main, 1);
    m_friendsPanel->hide(); // start collapsed; opened via the header 👥 button

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(18, 8, 18, 2);
    m_statusLabel = mutedLabel(QStringLiteral("Watching for games"));
    bottom->addWidget(m_statusLabel, 1);
    m_autoRecordLabel = new QLabel;
    m_autoRecordLabel->setStyleSheet(QStringLiteral("color:#8e98ad;"));
    m_autoRecordLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottom->addWidget(m_autoRecordLabel, 0, Qt::AlignRight);
    updateAutoRecordLabel();
    shellLayout->addLayout(bottom);

    outerLayout->addWidget(shell, 1);
    m_mainPage = outer;

    m_setupPage = buildSetupPage();
    m_restorePage = buildRestorePage();
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("RootStack"));
    makeOpaque(m_stack);
    m_stack->addWidget(m_setupPage);
    m_stack->addWidget(m_restorePage);
    m_stack->addWidget(m_mainPage);
    setCentralWidget(m_stack);


    // Give every clickable control the hand cursor (Qt style sheets can't set it).
    const auto buttons = findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons) {
        button->setCursor(Qt::PointingHandCursor);
    }
    const auto combos = findChildren<QComboBox *>();
    for (QComboBox *combo : combos) {
        combo->setCursor(Qt::PointingHandCursor);
    }
}

QWidget *MainWindow::buildSetupPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("SetupPage"));
    makeOpaque(page);
    auto *outerLayout = new QVBoxLayout(page);
    outerLayout->setContentsMargins(20, 18, 20, 18);

    auto *shell = new QFrame;
    shell->setObjectName(QStringLiteral("AppShell"));
    shell->setAttribute(Qt::WA_StyledBackground, true);
    // No drop-shadow effect here either; the setup page shares the same stack area
    // as the library and cached effects can visually bleed between them.
    auto *layout = new QVBoxLayout(shell);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(14);
    layout->addStretch();

    auto *icon = new QLabel;
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QPixmap(QStringLiteral(":/vodlink.png"))
                        .scaled(150, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(icon);

    auto *name = new QLabel(QStringLiteral("Vod<span style='color:#ffffff'>Link</span>"));
    name->setTextFormat(Qt::RichText);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(QStringLiteral("color:#7c4dff; font-size:52px; font-weight:800;"));
    layout->addWidget(name);

    layout->addSpacing(12);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    auto *signIn = new QPushButton(QStringLiteral("Sign in with Google"));
    signIn->setIcon(QIcon(QStringLiteral(":/google.svg")));
    signIn->setIconSize(QSize(20, 20));
    signIn->setCursor(Qt::PointingHandCursor);
    signIn->setMinimumSize(240, 46);
    buttonRow->addWidget(signIn);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);
    layout->addStretch();

    outerLayout->addWidget(shell, 1);
    connect(signIn, &QPushButton::clicked, this, [this] { m_controller->signIn(); });
    return page;
}

QWidget *MainWindow::buildRestorePage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("RestorePage"));
    makeOpaque(page);
    auto *outerLayout = new QVBoxLayout(page);
    outerLayout->setContentsMargins(20, 18, 20, 18);

    auto *shell = new QFrame;
    shell->setObjectName(QStringLiteral("AppShell"));
    shell->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(shell);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(14);
    layout->addStretch();

    auto *icon = new QLabel;
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QPixmap(QStringLiteral(":/vodlink.png"))
                        .scaled(130, 130, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(icon);

    auto *title = new QLabel(QStringLiteral("Restoring Google session…"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:#f4f7fb; font-size:26px; font-weight:800;"));
    layout->addWidget(title);

    auto *subtitle = mutedLabel(QStringLiteral("VodLink found saved credentials and is signing you back in. If this fails, you can reset local data from Settings or sign in again."));
    subtitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(subtitle);
    layout->addStretch();

    outerLayout->addWidget(shell, 1);
    return page;
}

void MainWindow::updateAuthGate()
{
    if (m_stack == nullptr || m_setupPage == nullptr
        || m_restorePage == nullptr || m_mainPage == nullptr) {
        return;
    }

    // There are three different states:
    // 1. No token/account: show the sign-in page.
    // 2. Refresh token exists but GoogleAuth has not produced an account yet:
    //    show a neutral restore page, never the sign-in page.
    // 3. Real signed-in identity exists: show the app.
    //
    // This avoids a startup flicker where "Sign in with Google" is painted first
    // and then visually sticks behind the main page on Windows/high-DPI systems.
    const bool signedIn = m_controller->isSignedIn()
                          && !m_controller->accountEmail().trimmed().isEmpty();
    const bool restoring = !signedIn && m_controller->hasStoredCredentials();

    QWidget *target = signedIn ? m_mainPage : (restoring ? m_restorePage : m_setupPage);
    const int targetIndex = m_stack->indexOf(target);

    if (m_stack->currentIndex() != targetIndex) {
        m_stack->setCurrentIndex(targetIndex);
    }

    // Do not call setVisible() on individual pages: QStackedWidget owns that.
    // Manually hiding pages fought the stack layout on some Windows builds and
    // made stale sign-in/friends pixels more likely.
    target->raise();
    target->updateGeometry();
    target->update();
    target->repaint();
    m_stack->updateGeometry();
    m_stack->update();
    m_stack->repaint();
}

QWidget *MainWindow::buildFriendsPanel()
{
    auto *panel = new QFrame;
    panel->setObjectName(QStringLiteral("SidePanel"));
    panel->setFixedWidth(315);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // Top row: the signed-in Google account (shown like a friend, no delete) and
    // a close button.
    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    m_selfAvatar = new QLabel;
    m_selfAvatar->setFixedSize(40, 40);
    m_selfAvatar->setScaledContents(true);
    top->addWidget(m_selfAvatar);
    m_selfName = new QLabel(QStringLiteral("Not signed in"));
    m_selfName->setStyleSheet(QStringLiteral("color:#f4f7fb; font-weight:600;"));
    top->addWidget(m_selfName, 1);
    auto *close = new QToolButton;
    close->setObjectName(QStringLiteral("GhostButton"));
    close->setText(QStringLiteral("×"));
    close->setStyleSheet(QStringLiteral("font-size:22px; padding:0 0 2px 0;"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(36, 36);
    top->addWidget(close, 0, Qt::AlignVCenter);
    layout->addLayout(top);

    layout->addWidget(sectionLabel(QStringLiteral("Friends"), 18));
    layout->addWidget(new QLabel(QStringLiteral("Add friend by email")));
    auto *addRow = new QHBoxLayout;
    m_friendEmailEdit = new QLineEdit;
    m_friendEmailEdit->setPlaceholderText(QStringLiteral("name@email.com"));
    addRow->addWidget(m_friendEmailEdit, 1);
    auto *addButton = new QPushButton(QStringLiteral("Add"));
    addButton->setCursor(Qt::PointingHandCursor);
    addRow->addWidget(addButton);
    layout->addLayout(addRow);

    m_workerHint = mutedLabel(QString());
    m_workerHint->setStyleSheet(QStringLiteral("color:#ffb86b;"));
    m_workerHint->hide();
    layout->addWidget(m_workerHint);

    auto *friendHeader = new QLabel(QStringLiteral("FRIENDS • 0"));
    friendHeader->setObjectName(QStringLiteral("friendsHeader"));
    friendHeader->setStyleSheet(QStringLiteral("color:#9aa4b8; letter-spacing:1px; font-weight:700;"));
    layout->addWidget(friendHeader);
    m_friendsList = new QListWidget;
    m_friendsList->setIconSize(QSize(44, 44));
    m_friendsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_friendsList->setFocusPolicy(Qt::NoFocus);
    m_friendsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_friendsList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_friendsList->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: 0; outline: 0; }"
                                              "QListWidget::item { padding: 4px 6px; border-radius: 12px; }"
                                              "QListWidget::item:selected { background: rgba(124, 77, 255, 0.22); }"));
    layout->addWidget(m_friendsList, 1);

    m_shareToggle = new QCheckBox(QStringLiteral("Link VODs with mutual friends"));
    m_shareToggle->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_shareToggle);

    connect(close, &QToolButton::clicked, this, [this] { setFriendsPanelVisible(false); });
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addFriendClicked);
    connect(m_friendEmailEdit, &QLineEdit::returnPressed, this, &MainWindow::addFriendClicked);
    connect(m_shareToggle, &QCheckBox::toggled, this, &MainWindow::toggleShare);
    return panel;
}

QWidget *MainWindow::buildLibrarySurface()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("LibrarySurface"));
    makeOpaque(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(16);

    constexpr int kHeaderBoxHeight = 64;
    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Vod<span style='color:#ffffff'>Link</span>"));
    title->setTextFormat(Qt::RichText);
    // Font size must come from the stylesheet: the global "QWidget { font-size }"
    // rule overrides anything set via QFont/setPointSize.
    title->setStyleSheet(QStringLiteral("color:#7c4dff; font-size:48px; font-weight:800;"));
    header->addWidget(title, 1, Qt::AlignVCenter);

    auto *vodCard = buildStatCard(QStringLiteral("0"), QStringLiteral("VODs"), QStringLiteral("▣"));
    vodCard->setFixedHeight(kHeaderBoxHeight);
    header->addWidget(vodCard, 0, Qt::AlignVCenter);
    auto *timeCard = buildStatCard(QStringLiteral("0h"), QStringLiteral("Total Game Time"), QStringLiteral("◷"));
    timeCard->setFixedHeight(kHeaderBoxHeight);
    header->addWidget(timeCard, 0, Qt::AlignVCenter);

    auto *friendsToggle = new QToolButton;
    friendsToggle->setObjectName(QStringLiteral("GhostButton"));
    friendsToggle->setText(QStringLiteral("👥"));
    friendsToggle->setStyleSheet(QStringLiteral("font-size:24px; padding:0 0 2px 0;"));
    friendsToggle->setCursor(Qt::PointingHandCursor);
    friendsToggle->setToolTip(QStringLiteral("Show or hide the friends panel"));
    friendsToggle->setFixedSize(kHeaderBoxHeight, kHeaderBoxHeight);
    header->addWidget(friendsToggle, 0, Qt::AlignVCenter);
    connect(friendsToggle, &QToolButton::clicked, this, [this] {
        if (m_friendsPanel != nullptr) {
            setFriendsPanelVisible(!m_friendsPanel->isVisible());
        }
    });

    m_accountButton = new QToolButton;
    m_accountButton->setObjectName(QStringLiteral("AccountButton"));
    m_accountButton->setPopupMode(QToolButton::InstantPopup);
    m_accountButton->setCursor(Qt::PointingHandCursor);
    m_accountButton->setFixedSize(kHeaderBoxHeight, kHeaderBoxHeight);
    m_accountButton->setIconSize(QSize(54, 54));
    m_accountButton->setStyleSheet(QStringLiteral(
        "QToolButton#AccountButton { background: rgba(11, 16, 26, 0.75); border: 1px solid rgba(128, 141, 169, 0.22); border-radius: %1px; padding: 0; }"
        "QToolButton#AccountButton:hover { border: 1px solid rgba(143, 102, 255, 0.7); background: rgba(124, 77, 255, 0.18); }")
        .arg(kHeaderBoxHeight / 2));
    header->addWidget(m_accountButton, 0, Qt::AlignVCenter);
    layout->addLayout(header);
    setupAccountMenu();

    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText(QStringLiteral("Search VODs..."));
    controls->addWidget(m_searchEdit, 1);

    m_libraryGameFilter = new QComboBox;
    m_libraryGameFilter->setMinimumWidth(150);
    controls->addWidget(m_libraryGameFilter);
    m_sortCombo = new QComboBox;
    m_sortCombo->addItems({QStringLiteral("Sort: Date"), QStringLiteral("Sort: Game"), QStringLiteral("Sort: Duration"), QStringLiteral("Sort: Title")});
    controls->addWidget(m_sortCombo);
    m_orderCombo = new QComboBox;
    m_orderCombo->addItems({QStringLiteral("Order: Newest"), QStringLiteral("Order: Oldest")});
    controls->addWidget(m_orderCombo);
    m_visibilityCombo = new QComboBox;
    m_visibilityCombo->addItems({QStringLiteral("Visibility: All"), QStringLiteral("Mine only"), QStringLiteral("Friends only")});
    controls->addWidget(m_visibilityCombo);
    layout->addLayout(controls);

    auto *contentHost = new QWidget;
    contentHost->setObjectName(QStringLiteral("VodContentHost"));
    makeOpaque(contentHost);
    auto *contentStack = new QGridLayout(contentHost);
    contentStack->setContentsMargins(0, 0, 0, 0);
    contentStack->setSpacing(0);

    auto *libraryLayer = new QWidget;
    libraryLayer->setObjectName(QStringLiteral("VodLibraryLayer"));
    makeOpaque(libraryLayer);
    auto *libraryLayout = new QVBoxLayout(libraryLayer);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(0);

    m_vodScroll = new QScrollArea;
    m_vodScroll->setObjectName(QStringLiteral("VodScroll"));
    m_vodScroll->setWidgetResizable(true);
    makeOpaque(m_vodScroll);
    if (m_vodScroll->viewport() != nullptr) {
        makeOpaque(m_vodScroll->viewport());
    }
    m_vodGridWidget = new QWidget;
    m_vodGridWidget->setObjectName(QStringLiteral("VodGridWidget"));
    makeOpaque(m_vodGridWidget);
    m_vodGridLayout = new QGridLayout(m_vodGridWidget);
    m_vodGridLayout->setContentsMargins(0, 0, 0, 0);
    m_vodGridLayout->setHorizontalSpacing(10);
    m_vodGridLayout->setVerticalSpacing(12);
    m_vodScroll->setWidget(m_vodGridWidget);
    libraryLayout->addWidget(m_vodScroll, 1);
    contentStack->addWidget(libraryLayer, 0, 0);

    m_viewerPanel = buildVodViewer();
    m_viewerPanel->hide();
    contentStack->addWidget(m_viewerPanel, 0, 0);
    m_viewerPanel->raise();
    layout->addWidget(contentHost, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::applyVodFilters);
    connect(m_libraryGameFilter, &QComboBox::currentTextChanged, this, &MainWindow::reloadLibrary);
    connect(m_sortCombo, &QComboBox::currentTextChanged, this, &MainWindow::applyVodFilters);
    connect(m_orderCombo, &QComboBox::currentTextChanged, this, &MainWindow::applyVodFilters);
    connect(m_visibilityCombo, &QComboBox::currentTextChanged, this, &MainWindow::applyVodFilters);
    return page;
}

QWidget *MainWindow::buildVodViewer()
{
    auto *panel = new QFrame;
    panel->setObjectName(QStringLiteral("ViewerPanel"));
    panel->setMinimumSize(760, 520);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *titleRow = new QHBoxLayout;
    m_previewTitle = sectionLabel(QStringLiteral("Select a VOD"), 16);
    titleRow->addWidget(m_previewTitle, 1);
    auto *close = new QToolButton;
    close->setObjectName(QStringLiteral("GhostButton"));
    close->setText(QStringLiteral("×"));
    close->setStyleSheet(QStringLiteral("font-size:22px; padding:0 0 2px 0;"));
    close->setFixedSize(36, 36);
    titleRow->addWidget(close);
    layout->addLayout(titleRow);

    m_syncPlayer = new SyncPlayer(panel);
    m_syncPlayer->setMinimumHeight(360);
    m_syncPlayer->setObjectName(QStringLiteral("EmbeddedVodPlayer"));
    m_syncPlayer->setStyleSheet(QStringLiteral("background:#05080d; border-radius:14px;"));
    layout->addWidget(m_syncPlayer, 2);

    m_previewMeta = mutedLabel(QStringLiteral("Select a VOD from the library to open the viewer."));
    layout->addWidget(m_previewMeta);

    auto *actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);
    m_copyVodLinkButton = new QPushButton(QStringLiteral("Copy link"));
    m_copyVodLinkButton->setObjectName(QStringLiteral("GhostButton"));
    m_copyVodLinkButton->setEnabled(false);
    actionRow->addWidget(m_copyVodLinkButton);
    m_deleteVodButton = new QPushButton(QStringLiteral("Delete"));
    m_deleteVodButton->setObjectName(QStringLiteral("DangerButton"));
    m_deleteVodButton->setEnabled(false);
    actionRow->addWidget(m_deleteVodButton);
    layout->addLayout(actionRow);

    m_participantStrip = new QWidget;
    m_participantLayout = new QHBoxLayout(m_participantStrip);
    m_participantLayout->setContentsMargins(0, 0, 0, 0);
    m_participantLayout->setSpacing(8);
    layout->addWidget(m_participantStrip);

    layout->addStretch();

    connect(close, &QToolButton::clicked, this, [this] {
        clearVodViewer();
    });
    connect(m_copyVodLinkButton, &QPushButton::clicked, this, &MainWindow::copyVodLinkClicked);
    connect(m_deleteVodButton, &QPushButton::clicked, this, &MainWindow::deleteVodClicked);
    return panel;
}

QWidget *MainWindow::buildStatCard(const QString &value, const QString &label, const QString &symbol)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("StatCard"));
    card->setFixedSize(165, 70);
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 10, 14, 10);
    auto *icon = new QLabel(symbol);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(34, 34);
    icon->setStyleSheet(QStringLiteral("background:rgba(124,77,255,0.18); color:#9f7bff; border-radius:10px; font-weight:800;"));
    layout->addWidget(icon);
    auto *texts = new QVBoxLayout;
    auto *valueLabel = new QLabel(value);
    valueLabel->setProperty("statLabel", label);
    QFont valueFont = valueLabel->font();
    valueFont.setBold(true);
    valueFont.setPointSize(14);
    valueLabel->setFont(valueFont);
    texts->addWidget(valueLabel);
    auto *sub = mutedLabel(label);
    sub->setStyleSheet(QStringLiteral("color:#9aa4b8; font-size:12px;"));
    texts->addWidget(sub);
    layout->addLayout(texts);
    if (label == QStringLiteral("VODs")) {
        m_vodsStat = valueLabel;
    } else if (label == QStringLiteral("Total Game Time")) {
        m_watchTimeStat = valueLabel;
    }
    return card;
}


void MainWindow::showInitial(bool minimized)
{
    if (minimized) {
        if (m_tray != nullptr && m_tray->isVisible()) {
            hide();
            return;
        }
        showMinimized();
        return;
    }
    show();
}

void MainWindow::setupAccountMenu()
{
    m_accountMenu = new QMenu(this);
    QAction *settings = m_accountMenu->addAction(QStringLiteral("⚙  Settings"));
    m_accountMenu->addSeparator();
    QAction *signOut = m_accountMenu->addAction(QStringLiteral("↩  Sign out"));
    m_accountButton->setMenu(m_accountMenu);
    connect(settings, &QAction::triggered, this, &MainWindow::openSettingsDialog);
    connect(signOut, &QAction::triggered, this, [this] { m_controller->signOut(); });
}

void MainWindow::updateAutoRecordLabel()
{
    if (m_autoRecordLabel == nullptr) {
        return;
    }
    if (m_controller->isRecording()) {
        m_autoRecordLabel->setToolTip(QString());
        m_autoRecordLabel->setText(m_isStreaming
            ? QStringLiteral("●  <span style='color:#ff6b7d;font-weight:700'>STREAMING</span>")
            : QStringLiteral("●  <span style='color:#ffd36c;font-weight:700'>WAITING</span>"));
        return;
    }
    m_autoRecordLabel->setToolTip(QString());
    const bool on = m_controller->autoRecordEnabled();
    m_autoRecordLabel->setText(
        QStringLiteral("●  Auto-recording: <span style='color:%1;font-weight:700'>%2</span>")
            .arg(on ? QStringLiteral("#6cff6c") : QStringLiteral("#ff6b7d"),
                 on ? QStringLiteral("ON") : QStringLiteral("OFF")));
}

void MainWindow::fadeInWidget(QWidget *widget, int durationMs)
{
    Q_UNUSED(durationMs);
    if (widget == nullptr) {
        return;
    }

    // Avoid QGraphicsOpacityEffect on app panels. On Windows with fractional
    // scaling, Qt may cache the subtree into a stale pixmap, which is exactly the
    // visual corruption we are trying to avoid.
    widget->setGraphicsEffect(nullptr);
    widget->show();
    widget->raise();
    widget->updateGeometry();
    widget->update();
    widget->repaint();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        return true; // swallow hover tooltips app-wide
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setFriendsPanelVisible(bool visible)
{
    if (m_friendsPanel == nullptr || m_friendsPanel->isVisible() == visible) {
        return;
    }

    QWidget *parent = m_friendsPanel->parentWidget();
    m_friendsPanel->setVisible(visible);
    m_friendsPanel->updateGeometry();
    if (parent != nullptr && parent->layout() != nullptr) {
        parent->layout()->invalidate();
        parent->layout()->activate();
    }

    // Do not block updates around this. The old disable/enable path was the source
    // of the "friends panel ghost" on Windows scaling because the newly uncovered
    // library area was never fully repainted.
    const QList<QWidget *> repaintTargets{parent, m_mainPage, m_stack, centralWidget(), this};
    for (QWidget *widget : repaintTargets) {
        if (widget == nullptr) {
            continue;
        }
        widget->updateGeometry();
        widget->update();
        widget->repaint();
    }
}

void MainWindow::openSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("VodLink Settings"));
    dialog.setStyleSheet(styleSheet());
    dialog.setSizeGripEnabled(false);
    auto *layout = new QVBoxLayout(&dialog);
    // Lock the dialog to a fixed size so it can't be dragged wider.
    layout->setSizeConstraint(QLayout::SetFixedSize);
    layout->addWidget(sectionLabel(QStringLiteral("Recorder"), 20));

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(12);

    auto *encoder = new QComboBox;
    encoder->setMinimumWidth(300);
    const QStringList encoderChoices = availableEncoderChoices();
    encoder->addItems(encoderChoices);
    const QString savedEncoder = normalizedEncoderLabel(
        settingOr(m_controller->appSetting(QString::fromLatin1(kEncoderSetting)), QStringLiteral("H.264")));
    const bool hasHardwareEncoder = !encoderChoices.isEmpty() && !isNoHardwareEncoderChoice(encoderChoices.first());
    encoder->setCurrentText(encoderChoices.contains(savedEncoder) ? savedEncoder : encoderChoices.first());
    encoder->setEnabled(hasHardwareEncoder);
    if (hasHardwareEncoder && encoder->currentText() != savedEncoder) {
        m_controller->setAppSetting(QString::fromLatin1(kEncoderSetting), encoder->currentText());
    }
    form->addRow(QStringLiteral("Encoder"), encoder);
    connect(encoder, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!isNoHardwareEncoderChoice(text)) {
            m_controller->setAppSetting(QString::fromLatin1(kEncoderSetting), text);
        }
    });
    if (!hasHardwareEncoder) {
        auto *encoderWarning = new QLabel(QStringLiteral("VodLink did not find a private OBS hardware encoder. Streaming is disabled until NVENC, AMF, QSV, VideoToolbox or VAAPI is bundled/available."));
        encoderWarning->setWordWrap(true);
        encoderWarning->setProperty("muted", true);
        form->addRow(QString(), encoderWarning);
    }

    const QString savedResolution = settingOr(m_controller->appSetting(QString::fromLatin1(kResolutionSetting)), nativeRecorderResolutionText());
    const QString savedFps = settingOr(m_controller->appSetting(QString::fromLatin1(kFpsSetting)), QStringLiteral("60"));

    auto *bitrate = new QSpinBox;
    bitrate->setRange(2500, 40000); // YouTube HEVC/AV1 max range tops out at 40 Mbps.
    bitrate->setSingleStep(500);
    bitrate->setSuffix(QStringLiteral(" Kbps"));
    bool bitrateOk = false;
    const int savedBitrate = m_controller->appSetting(QString::fromLatin1(kBitrateSetting)).toInt(&bitrateOk);
    const int defaultBitrate = recommendedBitrateKbps(savedResolution,
                                                     savedFps.toInt(),
                                                     isEfficientCodec(encoder->currentText()));
    bitrate->setValue((std::clamp)(bitrateOk ? savedBitrate : defaultBitrate, 2500, 40000));
    if (!bitrateOk) {
        m_controller->setAppSetting(QString::fromLatin1(kBitrateSetting), QString::number(bitrate->value()));
    }
    form->addRow(QStringLiteral("Bitrate"), bitrate);
    connect(bitrate, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        m_controller->setAppSetting(QString::fromLatin1(kBitrateSetting), QString::number(value));
    });

    auto *resolution = new QComboBox;
    resolution->setEditable(true);
    resolution->setInsertPolicy(QComboBox::NoInsert);
    resolution->addItems(availableResolutions(savedResolution));
    resolution->setCurrentText(savedResolution);
    if (resolution->lineEdit() != nullptr) {
        resolution->lineEdit()->setPlaceholderText(QStringLiteral("3440x1440"));
    }
    form->addRow(QStringLiteral("Resolution"), resolution);

    auto saveResolution = [this, resolution] {
        const QString normalized = normalizedResolutionText(resolution->currentText());
        if (normalized.isEmpty()) {
            return;
        }
        if (resolution->currentText() != normalized) {
            const QSignalBlocker blocker(resolution);
            resolution->setCurrentText(normalized);
        }
        m_controller->setAppSetting(QString::fromLatin1(kResolutionSetting), normalized);
    };
    connect(resolution, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        const QString normalized = normalizedResolutionText(text);
        if (!normalized.isEmpty()) {
            m_controller->setAppSetting(QString::fromLatin1(kResolutionSetting), normalized);
        }
    });
    if (resolution->lineEdit() != nullptr) {
        connect(resolution->lineEdit(), &QLineEdit::editingFinished, this, saveResolution);
    }

    auto *fps = new QComboBox;
    fps->addItems({QStringLiteral("60"), QStringLiteral("30")});
    fps->setCurrentText(savedFps);
    form->addRow(QStringLiteral("Framerate"), fps);
    connect(fps, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        m_controller->setAppSetting(QString::fromLatin1(kFpsSetting), text);
    });

    auto applyRecommendedBitrate = [this, bitrate, resolution, fps, encoder] {
        const QString normalized = normalizedResolutionText(resolution->currentText());
        if (normalized.isEmpty()) {
            return;
        }
        const int recommended = recommendedBitrateKbps(normalized,
                                                      fps->currentText().toInt(),
                                                      isEfficientCodec(encoder->currentText()));
        const int clamped = (std::clamp)(recommended, bitrate->minimum(), bitrate->maximum());
        {
            const QSignalBlocker blocker(bitrate);
            bitrate->setValue(clamped);
        }
        m_controller->setAppSetting(QString::fromLatin1(kBitrateSetting), QString::number(clamped));
    };

    // Resolution/FPS/encoder changes should snap the bitrate to the matching
    // YouTube recommendation. The user can still adjust the spinbox afterwards;
    // that manual value is what gets used until another quality setting changes.
    connect(resolution, &QComboBox::currentTextChanged, this, [applyRecommendedBitrate](const QString &) {
        applyRecommendedBitrate();
    });
    connect(fps, &QComboBox::currentTextChanged, this, [applyRecommendedBitrate](const QString &) {
        applyRecommendedBitrate();
    });
    connect(encoder, &QComboBox::currentTextChanged, this, [applyRecommendedBitrate](const QString &) {
        applyRecommendedBitrate();
    });

    auto *privacy = new QComboBox;
    privacy->setMinimumWidth(300);
    privacy->addItems({QStringLiteral("Game only"),
                       QStringLiteral("Game and external audio"),
                       QStringLiteral("Desktop"),
                       QStringLiteral("Desktop with external audio")});
    privacy->setCurrentText(privacyLabelForMode(m_controller->privacyMode()));
    form->addRow(QStringLiteral("Privacy"), privacy);
    connect(privacy, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        m_controller->setPrivacyMode(privacyModeForLabel(text));
    });

    auto *autoRecord = new QCheckBox(QStringLiteral("Automatically record supported games"));
    autoRecord->setChecked(m_controller->autoRecordEnabled());
    form->addRow(QStringLiteral("Auto-recording"), autoRecord);
    connect(autoRecord, &QCheckBox::toggled, this, [this](bool on) { m_controller->setAutoRecordEnabled(on); });

    auto *share = new QCheckBox(QStringLiteral("Link VODs with mutual friends"));
    share->setChecked(m_controller->shareEnabled());
    form->addRow(QStringLiteral("Friends"), share);
    connect(share, &QCheckBox::toggled, this, [this](bool on) { m_controller->setShareEnabled(on); });

    auto *notifications = new QCheckBox(QStringLiteral("Show tray notifications"));
    notifications->setChecked(m_controller->appSetting(QString::fromLatin1(kNotificationsSetting), QStringLiteral("1")) == QStringLiteral("1"));
    form->addRow(QStringLiteral("Notifications"), notifications);
    connect(notifications, &QCheckBox::toggled, this, [this](bool on) {
        m_controller->setAppSetting(QString::fromLatin1(kNotificationsSetting), on ? QStringLiteral("1") : QStringLiteral("0"));
    });

    auto *launchStartup = new QCheckBox(QStringLiteral("Launch minimized when Windows starts"));
    launchStartup->setChecked(launchAtStartupEnabled());
    launchStartup->setEnabled(launchAtStartupSupported());
    if (!launchAtStartupSupported()) {
        launchStartup->setToolTip(QStringLiteral("Launch on startup is available on Windows, macOS, and Linux desktop sessions."));
    }
    form->addRow(QStringLiteral("Launch on startup"), launchStartup);
    connect(launchStartup, &QCheckBox::toggled, this, [this, launchStartup](bool on) {
        QString error;
        if (!setLaunchAtStartupEnabled(on, &error)) {
            const QSignalBlocker blocker(launchStartup);
            launchStartup->setChecked(launchAtStartupEnabled());
            QMessageBox::warning(this, QStringLiteral("Startup setting failed"), error);
            return;
        }
        m_controller->setAppSetting(QString::fromLatin1(kLaunchAtStartupSetting), on ? QStringLiteral("1") : QStringLiteral("0"));
    });

    auto *addGameSpacer = new QWidget;
    addGameSpacer->setFixedHeight(10);
    form->addRow(QString(), addGameSpacer);

    auto *addGame = new QPushButton(QStringLiteral("Add game manually"));
    addGame->setObjectName(QStringLiteral("GhostButton"));
    addGame->setMinimumWidth(300);
    addGame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    form->addRow(QString(), addGame);
    connect(addGame, &QPushButton::clicked, this, &MainWindow::addCurrentGameClicked);

    auto *syncVods = new QPushButton(QStringLiteral("Sync YouTube VODs"));
    syncVods->setObjectName(QStringLiteral("GhostButton"));
    syncVods->setMinimumWidth(300);
    syncVods->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    syncVods->setEnabled(m_controller->isSignedIn());
    syncVods->setToolTip(m_controller->isSignedIn()
                             ? QStringLiteral("Manually import VodLink VODs and clips from your YouTube channel.")
                             : QStringLiteral("Sign in with Google before syncing YouTube VODs."));
    form->addRow(QString(), syncVods);
    connect(syncVods, &QPushButton::clicked, this, [this, syncVods] {
        syncVods->setEnabled(false);
        m_controller->syncOwnLibrary();
        QTimer::singleShot(1500, this, [this, syncVods] {
            if (syncVods != nullptr) {
                syncVods->setEnabled(m_controller->isSignedIn());
            }
        });
    });

    auto *resetSpacer = new QWidget;
    resetSpacer->setFixedHeight(6);
    form->addRow(QString(), resetSpacer);

    auto *resetButton = new QPushButton(QStringLiteral("Reset VodLink"));
    resetButton->setObjectName(QStringLiteral("DangerButton"));
    resetButton->setMinimumWidth(300);
    resetButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    resetButton->setToolTip(QStringLiteral("Close VodLink and delete all local app data/cache."));
    form->addRow(QString(), resetButton);
    connect(resetButton, &QPushButton::clicked, this, [this, &dialog] {
        QMessageBox prompt(&dialog);
        prompt.setWindowTitle(QStringLiteral("Reset VodLink?"));
        prompt.setIcon(QMessageBox::Warning);
        prompt.setText(QStringLiteral("Reset VodLink and close the app?"));
        prompt.setInformativeText(QStringLiteral(
            "This deletes all local VodLink data on this computer: sign-in tokens, settings, friends, cached VODs, clips, games, and broken cache.\n\n"
            "It does not delete videos or clips from YouTube."));
        auto *reset = prompt.addButton(QStringLiteral("Reset and close"), QMessageBox::DestructiveRole);
        reset->setObjectName(QStringLiteral("DangerButton"));
        auto *cancel = prompt.addButton(QMessageBox::Cancel);
        prompt.setDefaultButton(cancel);
        prompt.setStyleSheet(styleSheet());
        prompt.exec();
        if (prompt.clickedButton() != reset) {
            return;
        }

        QString error;
        const bool activeCleanup = m_controller->factoryResetLocalData(&error);
        if (!error.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Reset failed"), error);
            return;
        }

        m_quitRequested = true;
        dialog.accept();
        hide();
        QTimer::singleShot(activeCleanup ? 1200 : 0, qApp, [] { QApplication::quit(); });
    });

    layout->addLayout(form);

    auto *closeSpacer = new QWidget;
    closeSpacer->setFixedHeight(24);
    layout->addWidget(closeSpacer);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *closeButton = new QPushButton(QStringLiteral("Close"));
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    // QPushButton enables autoDefault inside dialogs. With no explicit default
    // button, Qt can route Enter/Return from editor widgets such as the bitrate
    // spinbox into the first auto-default push button in tab order, which is the
    // "Add game manually" action. Settings buttons should only fire on an
    // explicit click/Space while focused; Enter in a field should just commit the
    // field's value.
    const auto pushButtons = dialog.findChildren<QPushButton *>();
    for (QPushButton *button : pushButtons) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    const auto clickable = dialog.findChildren<QWidget *>();
    for (QWidget *widget : clickable) {
        if (qobject_cast<QAbstractButton *>(widget) != nullptr || qobject_cast<QComboBox *>(widget) != nullptr) {
            widget->setCursor(Qt::PointingHandCursor);
        }
    }

    dialog.exec();
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    m_tray = new QSystemTrayIcon(QIcon(QStringLiteral(":/vodlink.png")), this);
    m_tray->setToolTip(QStringLiteral("VodLink"));

    auto *menu = new QMenu(this);
    QAction *openAction = menu->addAction(QStringLiteral("Open VodLink"));
    menu->addSeparator();

    m_autoRecordAction = menu->addAction(QStringLiteral("Auto-record games"));
    m_autoRecordAction->setCheckable(true);
    m_autoRecordAction->setChecked(m_controller->autoRecordEnabled());
    m_shareAction = menu->addAction(QStringLiteral("Share VODs with friends"));
    m_shareAction->setCheckable(true);
    m_shareAction->setChecked(m_controller->shareEnabled());
    menu->addSeparator();

    QAction *settingsAction = menu->addAction(QStringLiteral("Settings…"));
    menu->addSeparator();
    QAction *quitAction = menu->addAction(QStringLiteral("Quit"));
    m_tray->setContextMenu(menu);

    connect(openAction, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    connect(m_autoRecordAction, &QAction::toggled, this, [this](bool checked) {
        m_controller->setAutoRecordEnabled(checked);
    });
    connect(m_shareAction, &QAction::toggled, this, [this](bool checked) {
        m_controller->setShareEnabled(checked);
    });
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettingsDialog);
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestAppQuit);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    m_tray->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_quitRequested && m_tray != nullptr && m_tray->isVisible()) {
        hide();
        event->ignore();
        const bool trayNotifications = m_controller->appSetting(QString::fromLatin1(kNotificationsSetting), QStringLiteral("1")) == QStringLiteral("1");
        if (!m_trayMessageShown && trayNotifications) {
            m_trayMessageShown = true;
            m_tray->showMessage(QStringLiteral("VodLink is still running"),
                                QStringLiteral("VodLink keeps watching for games in the background. Quit from the tray icon to exit."),
                                QSystemTrayIcon::Information, 4000);
        }
        return;
    }

    if (!m_quitRequested) {
        event->ignore();
        requestAppQuit();
        return;
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::requestAppQuit()
{
    if (m_quitRequested) {
        return;
    }
    m_quitRequested = true;
    const bool activeCleanup = m_controller != nullptr && m_controller->shutdownForQuit();
    hide();
    // Give the just-created YouTube/Worker cleanup requests a short chance to hit
    // the socket before the process exits. Normal stream finalization still runs
    // asynchronously if the app remains open.
    QTimer::singleShot(activeCleanup ? 1200 : 0, qApp, [] { QApplication::quit(); });
}

void MainWindow::reloadLibrary()
{
    QString error;
    const QString selectedGame = m_libraryGameFilter->currentIndex() <= 0 ? QString() : m_libraryGameFilter->currentText();
    const QString previous = m_libraryGameFilter->currentText();
    const QStringList games = m_controller->games(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Library error"), error);
        return;
    }
    m_libraryGameFilter->blockSignals(true);
    m_libraryGameFilter->clear();
    m_libraryGameFilter->addItem(QStringLiteral("Game: All"));
    for (const QString &game : games) {
        m_libraryGameFilter->addItem(game);
    }
    const int index = m_libraryGameFilter->findText(previous);
    m_libraryGameFilter->setCurrentIndex(index < 0 ? 0 : index);
    m_libraryGameFilter->blockSignals(false);

    m_libraryVods = m_controller->libraryVods(selectedGame, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Library error"), error);
        return;
    }
    applyVodFilters();
    refreshStats();
}

void MainWindow::applyVodFilters()
{
    const QString search = m_searchEdit != nullptr ? m_searchEdit->text().trimmed().toLower() : QString();
    const QString visibility = m_visibilityCombo != nullptr ? m_visibilityCombo->currentText() : QString();
    m_filteredVods.clear();
    for (const Vod &vod : m_libraryVods) {
        if (visibility == QStringLiteral("Mine only") && !vod.isMine()) {
            continue;
        }
        if (visibility == QStringLiteral("Friends only") && vod.isMine()) {
            continue;
        }
        const QString haystack = QStringLiteral("%1 %2 %3 %4")
                                  .arg(normalizeTitle(vod), vod.game, ownerText(vod), vod.youtubeId)
                                  .toLower();
        if (!search.isEmpty() && !haystack.contains(search)) {
            continue;
        }
        m_filteredVods.push_back(vod);
    }

    const QString sort = m_sortCombo != nullptr ? m_sortCombo->currentText() : QStringLiteral("Sort: Date");
    std::sort(m_filteredVods.begin(), m_filteredVods.end(), [sort](const Vod &a, const Vod &b) {
        if (sort.contains(QStringLiteral("Game"))) {
            return a.game.toLower() < b.game.toLower();
        }
        if (sort.contains(QStringLiteral("Duration"))) {
            return a.durationMs < b.durationMs;
        }
        if (sort.contains(QStringLiteral("Title"))) {
            return normalizeTitle(a).toLower() < normalizeTitle(b).toLower();
        }
        return a.startedAt < b.startedAt;
    });
    const bool newest = m_orderCombo == nullptr || m_orderCombo->currentText().contains(QStringLiteral("Newest"));
    if (newest) {
        std::reverse(m_filteredVods.begin(), m_filteredVods.end());
    }

    // Group VODs from the same session (same game, overlapping time) into a single
    // card, preferring the user's own VOD as the representative. The other players'
    // linked VODs stay reachable from the viewer's participant strip.
    QVector<Vod> grouped;
    grouped.reserve(m_filteredVods.size());
    for (const Vod &vod : m_filteredVods) {
        bool merged = false;
        for (Vod &rep : grouped) {
            if (rep.game.compare(vod.game, Qt::CaseInsensitive) == 0 && vodsOverlap(rep, vod)) {
                if (!rep.isMine() && vod.isMine()) {
                    rep = vod; // promote the user's own VOD to the group's card
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            grouped.push_back(vod);
        }
    }
    m_filteredVods = grouped;

    if (m_hasViewerVod) {
        m_selectedVodRow = -1;
        for (int i = 0; i < m_filteredVods.size(); ++i) {
            if (m_filteredVods.at(i).youtubeId == m_viewerVod.youtubeId) {
                m_selectedVodRow = i;
                break;
            }
        }
    }
    rebuildVodGrid();
}

void MainWindow::rebuildVodGrid()
{
    while (QLayoutItem *item = m_vodGridLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }

    for (int i = 0; i < 32; ++i) {
        m_vodGridLayout->setRowStretch(i, 0);
        m_vodGridLayout->setColumnStretch(i, 0);
    }

    if (m_filteredVods.isEmpty()) {
        m_vodGridLayout->setAlignment(Qt::AlignCenter);
        auto *empty = mutedLabel(QStringLiteral("No VODs yet. Start a supported game and VodLink will create a YouTube VOD automatically."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(false);
        empty->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        empty->setMinimumHeight(empty->sizeHint().height());
        m_vodGridLayout->setRowStretch(0, 1);
        m_vodGridLayout->setColumnStretch(0, 1);
        m_vodGridLayout->addWidget(empty, 0, 0, Qt::AlignCenter);
        m_vodGridWidget->updateGeometry();
        m_vodGridWidget->update();
        if (m_vodScroll != nullptr && m_vodScroll->viewport() != nullptr) {
            m_vodScroll->viewport()->updateGeometry();
            m_vodScroll->viewport()->update();
        }
        return;
    }

    m_vodGridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    constexpr int columns = 3;
    for (int i = 0; i < m_filteredVods.size(); ++i) {
        const Vod &vod = m_filteredVods.at(i);
        auto *card = new QPushButton;
        card->setObjectName(QStringLiteral("VodCard"));
        card->setCursor(Qt::PointingHandCursor);
        card->setCheckable(true);
        card->setFlat(true);
        card->setProperty("selected", i == m_selectedVodRow);
        card->setProperty("vodRow", i);
        constexpr int kVodCardWidth = 238;
        constexpr int kVodCardHeight = 264;
        card->setFixedSize(kVodCardWidth, kVodCardHeight);
        card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 10, 10, 12);
        layout->setSpacing(8);

        auto *thumbFrame = new QFrame(card);
        thumbFrame->setObjectName(QStringLiteral("VodThumbFrame"));
        thumbFrame->setFixedSize(kVodCardWidth - 20, 126);
        auto *thumbGrid = new QGridLayout(thumbFrame);
        thumbGrid->setContentsMargins(0, 0, 0, 0);
        thumbGrid->setSpacing(0);

        auto *thumb = new QLabel(thumbFrame);
        thumb->setObjectName(QStringLiteral("VodThumb"));
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setFixedSize(kVodCardWidth - 20, 126);
        thumb->setPixmap(vodPlaceholderIcon(vod.game).pixmap(thumb->size()));
        thumbGrid->addWidget(thumb, 0, 0);

        auto *durationBadge = new QLabel(durationText(vod.durationMs), thumbFrame);
        durationBadge->setObjectName(QStringLiteral("VodDurationBadge"));
        durationBadge->setAlignment(Qt::AlignCenter);
        durationBadge->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        thumbGrid->addWidget(durationBadge, 0, 0, Qt::AlignTop | Qt::AlignRight);
        layout->addWidget(thumbFrame, 0, Qt::AlignHCenter);

        auto *title = new QLabel(normalizeTitle(vod), card);
        title->setObjectName(QStringLiteral("VodTitle"));
        title->setWordWrap(true);
        title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        title->setMaximumHeight(40);
        layout->addWidget(title);

        auto *game = new QLabel(vod.game.trimmed().isEmpty() ? QStringLiteral("YouTube VOD") : vod.game.trimmed(), card);
        game->setObjectName(QStringLiteral("VodGame"));
        game->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        game->setMaximumHeight(18);
        layout->addWidget(game);

        auto *meta = new QLabel(QStringLiteral("%1 • %2").arg(relativeTimeText(vod.startedAt), durationText(vod.durationMs)), card);
        meta->setObjectName(QStringLiteral("VodMeta"));
        meta->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        meta->setWordWrap(false);
        layout->addWidget(meta);

        const QVector<Vod> linkedFriends = linkedFriendVodsForCard(vod);
        if (!linkedFriends.isEmpty()) {
            auto *bottomRow = new QHBoxLayout;
            bottomRow->setContentsMargins(0, 2, 0, 0);
            bottomRow->setSpacing(8);

            auto *friendCount = new QLabel(QStringLiteral("%1 friend%2")
                                                 .arg(linkedFriends.size())
                                                 .arg(linkedFriends.size() == 1 ? QString() : QStringLiteral("s")),
                                             card);
            friendCount->setObjectName(QStringLiteral("VodMeta"));
            bottomRow->addWidget(friendCount, 0, Qt::AlignLeft | Qt::AlignVCenter);
            bottomRow->addStretch(1);

            auto *avatars = new QWidget(card);
            auto *avatarsLayout = new QHBoxLayout(avatars);
            avatarsLayout->setContentsMargins(0, 0, 0, 0);
            avatarsLayout->setSpacing(-6);
            const int avatarCount = std::min(3, linkedFriends.size());
            for (int avatarIndex = 0; avatarIndex < avatarCount; ++avatarIndex) {
                const Vod &friendVod = linkedFriends.at(avatarIndex);
                const QString label = ownerText(friendVod);
                auto *avatar = new QLabel(avatars);
                avatar->setObjectName(QStringLiteral("cardAvatar"));
                avatar->setFixedSize(24, 24);
                avatar->setScaledContents(true);
                avatar->setAlignment(Qt::AlignCenter);
                avatar->setStyleSheet(QStringLiteral("background:#0b101a; border:2px solid #0d131e; border-radius:12px;"));
                avatar->setProperty("profileUrl", friendVod.ownerPictureUrl);
                avatar->setPixmap(m_avatarCache.value(friendVod.ownerPictureUrl,
                                                      fallbackProfileIcon(label)).pixmap(24, 24));
                avatarsLayout->addWidget(avatar);
                requestProfileIcon(friendVod.ownerPictureUrl);
            }
            bottomRow->addWidget(avatars, 0, Qt::AlignRight | Qt::AlignVCenter);
            layout->addLayout(bottomRow);
        }
        layout->addStretch(1);

        card->setStyleSheet(card->styleSheet());
        requestVodThumbnail(vod, card);
        connect(card, &QPushButton::clicked, this, [this, i] { selectVod(i); });
        m_vodGridLayout->addWidget(card, i / columns, i % columns);
    }
    m_vodGridLayout->setRowStretch((m_filteredVods.size() + columns - 1) / columns, 1);
    m_vodGridWidget->updateGeometry();
    m_vodGridWidget->update();
    if (m_vodScroll != nullptr && m_vodScroll->viewport() != nullptr) {
        m_vodScroll->viewport()->update();
    }
}

void MainWindow::updateVodCardSelection()
{
    if (m_vodGridWidget == nullptr) {
        return;
    }

    const auto cards = m_vodGridWidget->findChildren<QAbstractButton *>(QString(), Qt::FindChildrenRecursively);
    for (QAbstractButton *card : cards) {
        if (card == nullptr || card->objectName() != QStringLiteral("VodCard")) {
            continue;
        }
        bool ok = false;
        const int row = card->property("vodRow").toInt(&ok);
        card->setProperty("selected", ok && row == m_selectedVodRow);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    }
}

void MainWindow::selectVod(int row)
{
    if (row < 0 || row >= m_filteredVods.size()) {
        return;
    }
    showVodInViewer(m_filteredVods.at(row), row);
}

void MainWindow::showVodInViewer(const Vod &vod, int selectedGridRow)
{
    showVodInViewerAt(vod, selectedGridRow, -1);
}

void MainWindow::showVodInViewerAt(const Vod &vod, int selectedGridRow, double startSeconds)
{
    m_hasViewerVod = true;
    m_viewerVod = vod;
    m_selectedVodRow = selectedGridRow;
    m_controller->ensureVodEmbeddable(vod);
    if (m_viewerPanel != nullptr) {
        const bool wasHidden = !m_viewerPanel->isVisible();
        m_viewerPanel->show();
        m_viewerPanel->raise();
        m_viewerPanel->setFocus(Qt::OtherFocusReason);
        if (wasHidden) {
            fadeInWidget(m_viewerPanel);
        }
    }
    m_previewTitle->setText(normalizeTitle(vod));
    const bool canDelete = m_controller->canDeleteVod(vod);
    const QString ownership = !vod.ownerEmail.trimmed().isEmpty()
                                  ? QStringLiteral("Friend: %1").arg(ownerText(vod))
                                  : (canDelete ? QStringLiteral("Current account")
                                               : QStringLiteral("Account: %1").arg(ownerText(vod)));
    m_previewMeta->setText(QStringLiteral("%1 · %2 · %3 · %4")
                               .arg(vod.game,
                                    vod.startedAt.toLocalTime().toString(QStringLiteral("MMM d, yyyy h:mm AP")),
                                    ownership,
                                    durationText(vod.durationMs)));
    if (m_deleteVodButton != nullptr) {
        if (!vod.ownerEmail.trimmed().isEmpty()) {
            m_deleteVodButton->setVisible(true);
            m_deleteVodButton->setEnabled(true);
            m_deleteVodButton->setText(QStringLiteral("Remove"));
            m_deleteVodButton->setToolTip(QStringLiteral("Remove this friend's VOD from the local library only."));
        } else {
            m_deleteVodButton->setVisible(canDelete);
            m_deleteVodButton->setEnabled(canDelete);
            m_deleteVodButton->setText(QStringLiteral("Delete"));
            m_deleteVodButton->setToolTip(canDelete
                                              ? QStringLiteral("Delete this VOD from YouTube and remove it from VodLink.")
                                              : QStringLiteral("Only the signed-in owner can delete this YouTube VOD."));
        }
    }
    if (m_copyVodLinkButton != nullptr) {
        m_copyVodLinkButton->setEnabled(!vod.youtubeId.trimmed().isEmpty());
    }
    const double selectedStart = startSeconds >= 0.0
                                     ? clampVodOffset(vod, startSeconds)
                                     : 0.0;
    m_viewerStartSeconds = selectedStart;
    rebuildParticipantVodStrip(vod);
    updateVodCardSelection();
}

double MainWindow::syncedOffsetForLinkedVod(const Vod &sourceVod, const Vod &targetVod,
                                            double sourceOffsetSeconds) const
{
    if (!sourceVod.startedAt.isValid() || !targetVod.startedAt.isValid()) {
        return std::max(0.0, sourceOffsetSeconds);
    }

    const qint64 sourceMomentMs = sourceVod.startedAt.toUTC().toMSecsSinceEpoch()
                                  + static_cast<qint64>(std::max(0.0, sourceOffsetSeconds) * 1000.0);
    const qint64 targetOffsetMs = sourceMomentMs - targetVod.startedAt.toUTC().toMSecsSinceEpoch();
    return clampVodOffset(targetVod, static_cast<double>(targetOffsetMs) / 1000.0);
}

void MainWindow::rebuildParticipantVodStrip(const Vod &vod)
{
    if (m_participantLayout == nullptr) {
        return;
    }
    while (QLayoutItem *item = m_participantLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    QString error;
    QVector<Vod> linked = m_controller->sessionVods(vod.game, &error);
    if (!error.isEmpty()) {
        linked = {vod};
    } else {
        linked.erase(std::remove_if(linked.begin(), linked.end(), [&vod](const Vod &candidate) {
                         return candidate.youtubeId.trimmed().isEmpty() || !vodsOverlap(vod, candidate);
                     }),
                     linked.end());
    }

    // Always include the selected VOD even if the Worker/session cache has not
    // returned it yet, then remove duplicate YouTube ids.
    const auto selectedIt = std::find_if(linked.cbegin(), linked.cend(), [&vod](const Vod &candidate) {
        return candidate.youtubeId == vod.youtubeId;
    });
    if (selectedIt == linked.cend()) {
        linked.push_back(vod);
    }
    QSet<QString> seenIds;
    linked.erase(std::remove_if(linked.begin(), linked.end(), [&seenIds](const Vod &candidate) {
                     const QString id = candidate.youtubeId.trimmed();
                     if (id.isEmpty() || seenIds.contains(id)) {
                         return true;
                     }
                     seenIds.insert(id);
                     return false;
                 }),
                 linked.end());

    std::sort(linked.begin(), linked.end(), [](const Vod &a, const Vod &b) {
        if (a.isMine() != b.isMine()) {
            return a.isMine();
        }
        return ownerText(a).toLower() < ownerText(b).toLower();
    });

    int selectedIndex = 0;
    bool hasSharedVod = false;
    for (int i = 0; i < linked.size(); ++i) {
        if (linked.at(i).youtubeId == vod.youtubeId) {
            selectedIndex = i;
        } else {
            hasSharedVod = true;
        }
    }

    const double selectedOffset = m_viewerStartSeconds;
    if (m_syncPlayer != nullptr) {
        m_syncPlayer->setGroup(linked);
        m_syncPlayer->playIndexAt(selectedIndex, selectedOffset);
    }

    if (m_participantStrip != nullptr) {
        m_participantStrip->setVisible(hasSharedVod);
    }
    if (!hasSharedVod) {
        return;
    }

    auto *label = mutedLabel(QStringLiteral("Linked VODs:"));
    label->setFixedWidth(86);
    m_participantLayout->addWidget(label);

    for (int i = 0; i < linked.size(); ++i) {
        const Vod linkedVod = linked.at(i);
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("GhostButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(42, 42);
        button->setIconSize(QSize(32, 32));
        const QString owner = ownerText(linkedVod);
        const double sourceOffset = m_syncPlayer != nullptr
                                        ? m_syncPlayer->currentTimeSeconds()
                                        : selectedOffset;
        const double targetOffset = syncedOffsetForLinkedVod(vod, linkedVod, sourceOffset);
        button->setToolTip(QStringLiteral("Open %1's VOD at %2")
                               .arg(owner, durationText(static_cast<qint64>(targetOffset * 1000.0))));
        button->setIcon(fallbackProfileIcon(owner));
        if (!linkedVod.ownerPictureUrl.trimmed().isEmpty()) {
            requestProfileIcon(linkedVod.ownerPictureUrl);
            button->setProperty("profileUrl", linkedVod.ownerPictureUrl);
            button->setIcon(m_avatarCache.value(linkedVod.ownerPictureUrl, fallbackProfileIcon(owner)));
        }
        button->setProperty("selected", linkedVod.youtubeId == m_viewerVod.youtubeId);
        button->setStyleSheet(linkedVod.youtubeId == m_viewerVod.youtubeId
                                  ? QStringLiteral("padding:0; border:2px solid #7c4dff; border-radius:12px;")
                                  : QStringLiteral("padding:0;"));
        connect(button, &QToolButton::clicked, this, [this, linkedVod] {
            const double sourceOffset = m_syncPlayer != nullptr
                                            ? m_syncPlayer->currentTimeSeconds()
                                            : m_viewerStartSeconds;
            const double targetOffset = syncedOffsetForLinkedVod(m_viewerVod, linkedVod, sourceOffset);

            int row = -1;
            for (int j = 0; j < m_filteredVods.size(); ++j) {
                if (m_filteredVods.at(j).youtubeId == linkedVod.youtubeId) {
                    row = j;
                    break;
                }
            }
            showVodInViewerAt(linkedVod, row, targetOffset);
        });
        m_participantLayout->addWidget(button);
    }
    m_participantLayout->addStretch(1);
}

void MainWindow::clearVodViewer()
{
    m_hasViewerVod = false;
    m_selectedVodRow = -1;
    m_viewerStartSeconds = 0.0;
    m_previewTitle->setText(QStringLiteral("Select a VOD"));
    m_previewMeta->setText(QStringLiteral("Select a VOD from the library to open the viewer."));
    if (m_syncPlayer != nullptr) {
        m_syncPlayer->clear();
    }
    if (m_copyVodLinkButton != nullptr) {
        m_copyVodLinkButton->setEnabled(false);
    }
    if (m_deleteVodButton != nullptr) {
        m_deleteVodButton->setVisible(true);
        m_deleteVodButton->setText(QStringLiteral("Delete"));
        m_deleteVodButton->setEnabled(false);
    }
    if (m_participantLayout != nullptr) {
        while (QLayoutItem *item = m_participantLayout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
    }
    if (m_viewerPanel != nullptr) {
        m_viewerPanel->hide();
    }
    updateVodCardSelection();
}

void MainWindow::copyVodLinkClicked()
{
    if (!m_hasViewerVod || m_viewerVod.youtubeId.trimmed().isEmpty()) {
        return;
    }
    const double currentOffset = m_syncPlayer != nullptr
                                     ? m_syncPlayer->currentTimeSeconds()
                                     : m_viewerStartSeconds;
    QApplication::clipboard()->setText(youtubeWatchUrl(m_viewerVod.youtubeId, static_cast<int>(std::floor(currentOffset))));
    if (m_copyVodLinkButton != nullptr) {
        m_copyVodLinkButton->setText(QStringLiteral("Copied!"));
        QTimer::singleShot(1500, m_copyVodLinkButton, [this] {
            if (m_copyVodLinkButton != nullptr) {
                m_copyVodLinkButton->setText(QStringLiteral("Copy link"));
            }
        });
    }
}

void MainWindow::deleteVodClicked()
{
    if (!m_hasViewerVod || m_viewerVod.youtubeId.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Delete VOD"), QStringLiteral("Select a VOD first."));
        return;
    }

    if (!m_viewerVod.ownerEmail.trimmed().isEmpty()) {
        const QString owner = ownerText(m_viewerVod);
        if (QMessageBox::question(this, QStringLiteral("Remove friend VOD"),
                                  QStringLiteral("Remove %1's linked VOD from this device? This does not delete anything from YouTube.").arg(owner))
            == QMessageBox::Yes) {
            m_controller->removeFriendVodFromLibrary(m_viewerVod);
        }
        return;
    }

    if (!m_controller->canDeleteVod(m_viewerVod)) {
        return;
    }

    const QString title = normalizeTitle(m_viewerVod);
    QMessageBox prompt(this);
    prompt.setWindowTitle(QStringLiteral("Delete YouTube VOD"));
    prompt.setIcon(QMessageBox::Warning);
    prompt.setText(QStringLiteral("Delete \"%1\" from YouTube and remove it from VodLink?\n\nThis cannot be undone from VodLink.").arg(title));
    auto *deleteButton = prompt.addButton(QStringLiteral("Delete from YouTube"), QMessageBox::DestructiveRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.exec();
    if (prompt.clickedButton() != deleteButton) {
        return;
    }
    m_deleteVodButton->setEnabled(false);
    m_deleteVodButton->setText(QStringLiteral("Deleting…"));
    m_controller->deleteVod(m_viewerVod);
}

void MainWindow::refreshStats()
{
    if (m_vodsStat != nullptr) {
        m_vodsStat->setText(QString::number(m_libraryVods.size()));
    }
    qint64 totalMs = 0;
    for (const Vod &vod : m_libraryVods) {
        totalMs += std::max<qint64>(0, vod.durationMs);
    }
    const qint64 hours = totalMs / (1000 * 60 * 60);
    const qint64 minutes = (totalMs / (1000 * 60)) % 60;
    if (m_watchTimeStat != nullptr) {
        m_watchTimeStat->setText(QStringLiteral("%1h %2m").arg(hours).arg(minutes));
    }
}

void MainWindow::reloadFriends()
{
    m_shareToggle->blockSignals(true);
    m_shareToggle->setChecked(m_controller->shareEnabled());
    m_shareToggle->blockSignals(false);

    QString error;
    const QVector<AccountProfile> friends = m_controller->friendProfiles(&error);
    m_friendsList->clear();
    for (const AccountProfile &profile : friends) {
        auto *item = new QListWidgetItem(m_friendsList);
        QWidget *row = createFriendRow(profile);
        item->setSizeHint(QSize(row->sizeHint().width(), 68));
        m_friendsList->setItemWidget(item, row);
        requestProfileIcon(profile.pictureUrl);
    }

    QLabel *header = m_friendsPanel != nullptr ? m_friendsPanel->findChild<QLabel *>(QStringLiteral("friendsHeader")) : nullptr;
    if (header != nullptr) {
        header->setText(QStringLiteral("FRIENDS • %1").arg(friends.size()));
    }

    if (!m_controller->isWorkerConfigured()) {
        m_workerHint->setText(QStringLiteral("Friend linking is offline because the Worker URL is not configured."));
        m_workerHint->show();
    } else {
        m_workerHint->hide();
    }
}

QVector<Vod> MainWindow::linkedFriendVodsForCard(const Vod &vod) const
{
    QVector<Vod> friends;
    QSet<QString> seenIds;
    const QString selectedId = vod.youtubeId.trimmed();
    for (const Vod &candidate : m_libraryVods) {
        const QString candidateId = candidate.youtubeId.trimmed();
        if (candidateId.isEmpty() || candidateId == selectedId) {
            continue;
        }
        if (candidate.game.compare(vod.game, Qt::CaseInsensitive) != 0 || !vodsOverlap(vod, candidate)) {
            continue;
        }
        if (candidate.ownerEmail.trimmed().isEmpty()) {
            continue;
        }
        if (seenIds.contains(candidateId)) {
            continue;
        }
        seenIds.insert(candidateId);
        friends.push_back(candidate);
    }
    std::sort(friends.begin(), friends.end(), [](const Vod &a, const Vod &b) {
        return a.startedAt < b.startedAt;
    });
    return friends;
}

void MainWindow::onAccountChanged(const QString &email)
{
    updateAuthGate();
    if (!m_controller->isAuthConfigured()) {
        m_accountButton->setEnabled(false);
        m_accountButton->setIcon(fallbackProfileIcon(QStringLiteral("?")));
        m_accountButton->setToolTip(QStringLiteral("Google sign-in is not configured"));
        applyAccountIcon(fallbackProfileIcon(QStringLiteral("?")));
        if (m_selfName != nullptr) {
            m_selfName->setText(QStringLiteral("Sign-in not configured"));
        }
        return;
    }
    m_accountButton->setEnabled(true);
    if (email.isEmpty()) {
        m_accountButton->setProperty("profileUrl", QString());
        m_accountButton->setIcon(fallbackProfileIcon(QStringLiteral("?")));
        m_accountButton->setToolTip(QStringLiteral("Sign in with Google"));
        applyAccountIcon(fallbackProfileIcon(QStringLiteral("?")));
        if (m_selfName != nullptr) {
            m_selfName->setText(QStringLiteral("Not signed in"));
            m_selfName->setToolTip(QString());
        }
        updateFooterIdentity();
        return;
    }
    const QString name = m_controller->accountName();
    const QString picture = m_controller->accountPictureUrl();
    const QString label = name.isEmpty() ? email : name;
    m_accountButton->setToolTip(label);
    m_accountButton->setProperty("profileUrl", picture);
    if (m_selfName != nullptr) {
        m_selfName->setText(label);
        m_selfName->setToolTip(email);
    }
    applyAccountIcon(m_avatarCache.value(picture, fallbackProfileIcon(label)));
    requestProfileIcon(picture);
    updateFooterIdentity();
}

void MainWindow::addFriendClicked()
{
    const QString email = m_friendEmailEdit->text().trimmed();
    if (email.isEmpty()) {
        return;
    }
    m_controller->addFriend(email);
    m_friendEmailEdit->clear();
}

void MainWindow::addCurrentGameClicked()
{
#if defined(Q_OS_WIN)
    const QString filter = QStringLiteral("Windows executables (*.exe);;All files (*)");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QString filter = QStringLiteral("Applications and executables (*.app *);;All files (*)");
#else
    const QString filter = QStringLiteral("Executables (*)");
#endif

    const QString executablePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Add game manually"),
        QDir::homePath(),
        filter);

    if (executablePath.trimmed().isEmpty()) {
        return;
    }

    const QFileInfo file(executablePath);
    const bool isMacAppBundle =
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        file.isDir() && file.suffix().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0;
#else
        false;
#endif

    if (!file.exists() || (!file.isFile() && !isMacAppBundle)) {
        QMessageBox::warning(this, QStringLiteral("Add game manually"),
                             QStringLiteral("That executable does not exist."));
        return;
    }

#if defined(Q_OS_WIN)
    if (file.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, QStringLiteral("Add game manually"),
                             QStringLiteral("Please select the game's .exe file."));
        return;
    }
#elif defined(Q_OS_LINUX)
    if (!file.isExecutable()) {
        QMessageBox::warning(this, QStringLiteral("Add game manually"),
                             QStringLiteral("Please select an executable file."));
        return;
    }
#endif

    bool ok = false;
    QString suggested = file.completeBaseName().trimmed();
    if (suggested.isEmpty()) {
        suggested = file.fileName();
    }

    const QString name = QInputDialog::getText(this, QStringLiteral("Add game manually"),
                                               QStringLiteral("Display name for this game:"),
                                               QLineEdit::Normal, suggested, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }

    m_controller->addUserGame(executablePath, name);
}

void MainWindow::toggleShare(bool enabled)
{
    m_controller->setShareEnabled(enabled);
}


void MainWindow::updateStatus(const QString &message, bool streaming)
{
    Q_UNUSED(streaming);
    // The footer-left shows identity/last game; the recorder/live state lives only
    // in the right-hand label to avoid showing it twice. STREAMING is read from
    // the controller's YouTube-confirmed state, not from generic status messages.
    m_lastStatusMessage = message;
    m_isStreaming = m_controller->isLiveConfirmed();
    updateAutoRecordLabel();
    if (m_tray != nullptr) {
        m_tray->setToolTip(message.isEmpty() ? QStringLiteral("VodLink")
                                             : QStringLiteral("VodLink — %1").arg(message));
    }
}

void MainWindow::updateFooterIdentity()
{
    if (m_statusLabel == nullptr) {
        return;
    }
    const QString game = m_controller->lastGame().trimmed();
    if (!game.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("Last game: %1").arg(game));
        return;
    }
    if (m_controller->isSignedIn() || !m_controller->accountEmail().isEmpty()) {
        const QString name = m_controller->accountName().trimmed();
        const QString who = name.isEmpty() ? m_controller->accountEmail() : name;
        m_statusLabel->setText(QStringLiteral("Signed in as %1").arg(who));
    } else {
        m_statusLabel->setText(QString());
    }
}

QString MainWindow::durationText(qint64 durationMs)
{
    const qint64 total = std::max<qint64>(0, durationMs / 1000);
    const qint64 hours = total / 3600;
    const qint64 minutes = (total / 60) % 60;
    const qint64 seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString MainWindow::relativeTimeText(const QDateTime &when)
{
    if (!when.isValid()) {
        return QStringLiteral("unknown time");
    }
    const qint64 seconds = when.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 120) {
        return QStringLiteral("just now");
    }
    if (seconds < 3600) {
        return QStringLiteral("%1m ago").arg(seconds / 60);
    }
    if (seconds < 86400) {
        return QStringLiteral("%1h ago").arg(seconds / 3600);
    }
    return when.toLocalTime().toString(QStringLiteral("MMM d, yyyy"));
}

QString MainWindow::vodLabel(const Vod &vod)
{
    const QString who = ownerText(vod);
    return QStringLiteral("%1 — %2\n%3 · %4")
        .arg(who, vod.game, vod.startedAt.toLocalTime().toString(QStringLiteral("dd MMM yyyy, HH:mm")), durationText(vod.durationMs));
}

QString MainWindow::cardText(const Vod &vod)
{
    const QString owner = ownerText(vod);
    return QStringLiteral("%1\n%2 · %3\n%4")
        .arg(normalizeTitle(vod), relativeTimeText(vod.startedAt), durationText(vod.durationMs), owner);
}

QString MainWindow::youtubeWatchUrl(const QString &videoId, int startSeconds)
{
    QUrl url(QStringLiteral("https://www.youtube.com/watch"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), videoId);
    if (startSeconds >= 0) {
        query.addQueryItem(QStringLiteral("t"), QStringLiteral("%1s").arg(startSeconds));
    }
    url.setQuery(query);
    return url.toString();
}


QWidget *MainWindow::createFriendRow(const AccountProfile &profile)
{
    const QString display = friendDisplayName(profile);
    const bool hasName = !profile.displayName.trimmed().isEmpty();
    auto *row = new QWidget;
    row->setToolTip(profile.email);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(6, 6, 6, 6);
    rowLayout->setSpacing(10);

    // Avatar: the friend's Google picture once a mutual match supplies it, else a
    // letter badge built from the first character of their email.
    auto *avatar = new QLabel;
    avatar->setObjectName(QStringLiteral("friendAvatar"));
    avatar->setFixedSize(44, 44);
    avatar->setScaledContents(true);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setProperty("profileUrl", profile.pictureUrl);
    avatar->setPixmap(m_avatarCache.value(profile.pictureUrl, fallbackProfileIcon(profile.email)).pixmap(44, 44));
    rowLayout->addWidget(avatar, 0, Qt::AlignVCenter);

    // Width available to the text column: panel (315) minus margins, avatar,
    // trash button, and spacing. Long emails shrink to fit instead of clipping.
    constexpr int kTextWidth = 144;
    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    auto *name = new QLabel(display);
    name->setStyleSheet(QStringLiteral("color:#f4f7fb; font-weight:600; font-size:%1px;")
                            .arg(fittedFontPx(display, kTextWidth, 14, 10)));
    texts->addWidget(name);
    // Only show the email as a subtitle when the primary line is a real name, to
    // avoid showing the email twice.
    if (hasName) {
        auto *sub = new QLabel(profile.email);
        sub->setStyleSheet(QStringLiteral("color:#8e98ad; font-size:%1px;")
                               .arg(fittedFontPx(profile.email, kTextWidth, 12, 9)));
        texts->addWidget(sub);
    }
    rowLayout->addLayout(texts, 1);
    rowLayout->setAlignment(texts, Qt::AlignVCenter);

    auto *trash = new QToolButton;
    trash->setObjectName(QStringLiteral("GhostButton"));
    trash->setIcon(QIcon(QStringLiteral(":/trash.svg")));
    trash->setIconSize(QSize(24, 24));
    trash->setFixedSize(40, 40);
    trash->setStyleSheet(QStringLiteral("padding:0 0 2px 0;"));
    trash->setCursor(Qt::PointingHandCursor);
    trash->setToolTip(QStringLiteral("Remove %1").arg(display));
    const QString email = profile.email;
    connect(trash, &QToolButton::clicked, this, [this, email] {
        m_controller->removeFriend(email);
    });
    rowLayout->addWidget(trash, 0, Qt::AlignVCenter);
    return row;
}

void MainWindow::requestProfileIcon(const QString &pictureUrl)
{
    const QUrl url(pictureUrl);
    const QString host = url.host().toLower();
    if (pictureUrl.isEmpty() || m_avatarCache.contains(pictureUrl) || m_avatarRequests.contains(pictureUrl)
        || url.scheme() != QStringLiteral("https")
        || !(host == QStringLiteral("googleusercontent.com") || host.endsWith(QStringLiteral(".googleusercontent.com")))) {
        return;
    }
    m_avatarRequests.insert(pictureUrl);
    QNetworkRequest request(url);
    request.setTransferTimeout(5000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_avatarNetwork->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, pictureUrl] {
        m_avatarRequests.remove(pictureUrl);
        QPixmap pixmap;
        if (reply->error() == QNetworkReply::NoError) {
            pixmap.loadFromData(reply->readAll());
        }
        reply->deleteLater();
        if (pixmap.isNull()) {
            return;
        }
        QIcon icon(circularPixmap(pixmap, 96));
        m_avatarCache.insert(pictureUrl, icon);
        applyProfileIcon(pictureUrl, icon);
    });
}

void applyVodThumbnailToCard(QAbstractButton *button, const QPixmap &pixmap)
{
    if (button == nullptr || pixmap.isNull()) {
        return;
    }
    if (QLabel *thumb = button->findChild<QLabel *>(QStringLiteral("VodThumb"))) {
        const QSize targetSize = thumb->size().isValid() ? thumb->size() : QSize(214, 120);
        thumb->setPixmap(pixmap.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        return;
    }
    button->setIcon(QIcon(pixmap.scaled(520, 292, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
}

void MainWindow::requestVodThumbnail(const Vod &vod, QAbstractButton *button)
{
    const QString youtubeId = vod.youtubeId.trimmed();
    if (youtubeId.isEmpty() || button == nullptr) {
        return;
    }

    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const bool refresh = shouldRefreshVodThumbnail(vod, nowMs);
    const QString cacheKey = thumbnailCacheKey(vod, nowMs);

    if (m_thumbnailCache.contains(cacheKey)) {
        applyVodThumbnailToCard(button, m_thumbnailCache.value(cacheKey).pixmap(520, 292));
        if (refresh) {
            scheduleVodThumbnailRefresh(vod, button);
        }
        return;
    }

    // While YouTube is still generating better thumbnails after a live ends, keep
    // the latest known thumbnail visible instead of bouncing back to the placeholder.
    if (refresh) {
        const QString prefix = youtubeId + QStringLiteral("|fresh|");
        for (auto it = m_thumbnailCache.constBegin(); it != m_thumbnailCache.constEnd(); ++it) {
            if (it.key().startsWith(prefix) || it.key() == youtubeId) {
                applyVodThumbnailToCard(button, it.value().pixmap(520, 292));
                break;
            }
        }

        const qint64 nextProbeMs = m_thumbnailNextProbeMs.value(youtubeId, 0);
        if (nextProbeMs > nowMs) {
            scheduleVodThumbnailRefresh(vod, button);
            return;
        }
    }

    if (m_thumbnailRequests.contains(cacheKey)) {
        if (refresh) {
            scheduleVodThumbnailRefresh(vod, button);
        }
        return;
    }
    m_thumbnailRequests.insert(cacheKey);

    QPointer<QAbstractButton> target(button);
    QNetworkRequest request(thumbnailUrl(youtubeId, refresh, nowMs));
    request.setTransferTimeout(6000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         refresh ? QNetworkRequest::AlwaysNetwork : QNetworkRequest::PreferCache);
    QNetworkReply *reply = m_avatarNetwork->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 2 * 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, vod, youtubeId, cacheKey, refresh, target] {
        m_thumbnailRequests.remove(cacheKey);
        QByteArray payload;
        QPixmap pixmap;
        if (reply->error() == QNetworkReply::NoError) {
            payload = reply->readAll();
            pixmap.loadFromData(payload);
        }
        reply->deleteLater();
        if (pixmap.isNull()) {
            if (!target.isNull() && refresh) {
                scheduleVodThumbnailRefresh(vod, target);
            }
            return;
        }

        const QByteArray newHash = thumbnailBytesHash(payload);
        const QByteArray previousHash = m_thumbnailHashes.value(youtubeId);
        if (refresh && !previousHash.isEmpty() && previousHash == newHash) {
            const int unchangedCount = m_thumbnailUnchangedCounts.value(youtubeId, 0) + 1;
            m_thumbnailUnchangedCounts.insert(youtubeId, unchangedCount);
            const qint64 nextProbeMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()
                                      + thumbnailProbeBackoffMs(vod, unchangedCount);
            m_thumbnailNextProbeMs.insert(youtubeId, nextProbeMs);
            if (!target.isNull()) {
                scheduleVodThumbnailRefresh(vod, target);
            }
            return;
        }

        m_thumbnailHashes.insert(youtubeId, newHash);
        m_thumbnailUnchangedCounts.insert(youtubeId, 0);
        if (refresh) {
            m_thumbnailNextProbeMs.insert(youtubeId, QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()
                                                     + thumbnailRefreshIntervalMs(vod));
        } else {
            m_thumbnailNextProbeMs.remove(youtubeId);
        }

        const QString prefix = youtubeId + QStringLiteral("|fresh|");
        if (refresh) {
            QList<QString> oldKeys;
            for (auto it = m_thumbnailCache.constBegin(); it != m_thumbnailCache.constEnd(); ++it) {
                if (it.key().startsWith(prefix) && it.key() != cacheKey) {
                    oldKeys.push_back(it.key());
                }
            }
            for (const QString &oldKey : oldKeys) {
                m_thumbnailCache.remove(oldKey);
            }
        }

        QIcon icon(pixmap.scaled(520, 292, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        m_thumbnailCache.insert(cacheKey, icon);
        if (!target.isNull()) {
            applyVodThumbnailToCard(target, icon.pixmap(520, 292));
            if (refresh) {
                scheduleVodThumbnailRefresh(vod, target);
            }
        }
    });
}

void MainWindow::scheduleVodThumbnailRefresh(const Vod &vod, QAbstractButton *button)
{
    if (button == nullptr || !shouldRefreshVodThumbnail(vod)) {
        return;
    }
    if (button->property("thumbnailRefreshScheduled").toBool()) {
        return;
    }
    button->setProperty("thumbnailRefreshScheduled", true);
    QPointer<QAbstractButton> target(button);
    const QString youtubeId = vod.youtubeId.trimmed();
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const qint64 nextProbeMs = m_thumbnailNextProbeMs.value(youtubeId, 0);
    const qint64 baseDelayMs = thumbnailRefreshIntervalMs(vod, nowMs);
    const qint64 probeDelayMs = nextProbeMs > nowMs ? (nextProbeMs - nowMs) : baseDelayMs;
    const int delayMs = static_cast<int>(std::min<qint64>(std::max<qint64>(1000, probeDelayMs),
                                                          std::numeric_limits<int>::max()));
    QTimer::singleShot(delayMs, this, [this, target, vod] {
        if (target.isNull()) {
            return;
        }
        target->setProperty("thumbnailRefreshScheduled", false);
        if (shouldRefreshVodThumbnail(vod)) {
            requestVodThumbnail(vod, target);
        }
    });
}

void MainWindow::applyProfileIcon(const QString &pictureUrl, const QIcon &icon)
{
    if (m_friendsList != nullptr) {
        for (int i = 0; i < m_friendsList->count(); ++i) {
            QWidget *row = m_friendsList->itemWidget(m_friendsList->item(i));
            if (row == nullptr) {
                continue;
            }
            const auto avatars = row->findChildren<QLabel *>(QStringLiteral("friendAvatar"));
            for (QLabel *avatar : avatars) {
                if (avatar->property("profileUrl").toString() == pictureUrl) {
                    avatar->setPixmap(icon.pixmap(40, 40));
                }
            }
        }
    }
    if (m_accountButton != nullptr && m_accountButton->property("profileUrl").toString() == pictureUrl) {
        applyAccountIcon(icon);
    }
    if (m_participantStrip != nullptr) {
        const auto buttons = m_participantStrip->findChildren<QToolButton *>();
        for (QToolButton *button : buttons) {
            if (button->property("profileUrl").toString() == pictureUrl) {
                button->setIcon(icon);
            }
        }
    }
    if (m_vodGridWidget != nullptr) {
        const auto avatars = m_vodGridWidget->findChildren<QLabel *>(QStringLiteral("cardAvatar"), Qt::FindChildrenRecursively);
        for (QLabel *avatar : avatars) {
            if (avatar->property("profileUrl").toString() == pictureUrl) {
                avatar->setPixmap(icon.pixmap(24, 24));
            }
        }
    }
}

void MainWindow::applyAccountIcon(const QIcon &icon)
{
    if (m_accountButton != nullptr) {
        m_accountButton->setIcon(icon);
    }
    if (m_selfAvatar != nullptr) {
        m_selfAvatar->setPixmap(icon.pixmap(40, 40));
    }
}

QIcon MainWindow::fallbackProfileIcon(const QString &text)
{
    QPixmap pixmap(96, 96);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(QStringLiteral("#7c4dff")));
    painter.setPen(QPen(QColor(QStringLiteral("#b998ff")), 4));
    painter.drawEllipse(pixmap.rect().adjusted(4, 4, -4, -4));
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(38);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString initial = text.trimmed().left(1).toUpper();
    painter.drawText(pixmap.rect(), Qt::AlignCenter, initial.isEmpty() ? QStringLiteral("?") : initial);
    return QIcon(pixmap);
}

QIcon MainWindow::vodPlaceholderIcon(const QString &text)
{
    QPixmap pixmap(520, 292);
    QPainter painter(&pixmap);
    QLinearGradient gradient(0, 0, pixmap.width(), pixmap.height());
    gradient.setColorAt(0.0, QColor(QStringLiteral("#20113f")));
    gradient.setColorAt(0.55, QColor(QStringLiteral("#101927")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#07121f")));
    painter.fillRect(pixmap.rect(), gradient);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(QStringLiteral("#9f7bff")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(34);
    painter.setFont(font);
    painter.drawText(pixmap.rect().adjusted(20, 20, -20, -20), Qt::AlignCenter,
                     text.trimmed().isEmpty() ? QStringLiteral("VodLink") : text.trimmed());
    return QIcon(pixmap);
}
