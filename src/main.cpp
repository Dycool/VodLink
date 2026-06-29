#include "app/AppController.h"
#include "app/DebugLog.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPalette>
#include <QPropertyAnimation>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>

namespace {
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
} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    // Keep QStandardPaths app-specific folders from becoming VodLink/VodLink.
    // VodLink stores its own data via AppPaths in a single local root:
    // %LOCALAPPDATA%/VodLink on Windows.
    QCoreApplication::setOrganizationName(QString());
    QCoreApplication::setApplicationName(QStringLiteral("VodLink"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/vodlink.png")));

    const QStringList arguments = QCoreApplication::arguments();
    QString debugLogError;
    if (!DebugLog::initializeFromArguments(arguments, &debugLogError)) {
        QMessageBox::critical(nullptr, QStringLiteral("VodLink debug log failed"), debugLogError);
        return 1;
    }
    DebugLog::write(QStringLiteral("main: QApplication created"));

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
    const int exitCode = application.exec();
    DebugLog::write(QStringLiteral("main: QApplication exited with code %1").arg(exitCode));
    DebugLog::shutdown();
    return exitCode;
}
