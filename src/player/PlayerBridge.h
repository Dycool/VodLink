#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// Bridges C++ and the YouTube IFrame player running inside the QWebEngineView.
// C++ drives playback by emitting the *Requested signals (the page connects to
// them); the page reports progress back through the public slots.
class PlayerBridge final : public QObject
{
    Q_OBJECT

public:
    explicit PlayerBridge(QObject *parent = nullptr);

    [[nodiscard]] double currentTime() const { return m_currentTime; }
    [[nodiscard]] bool isPageReady() const { return m_pageReady; }

    // C++ control surface.
    void loadVideo(const QString &videoId, double seekSeconds);
    void seek(double seconds);
    void play();
    void pause();
    void showMessage(const QString &text);
    void cacheVideos(const QStringList &videoIds);

public slots:
    // Invoked from JavaScript.
    void ready();
    void onTimeUpdate(double seconds);
    void debugLog(const QString &category, const QString &message);

signals:
    // To the page.
    void loadRequested(const QString &videoId, double seek);
    void seekRequested(double seconds);
    void playRequested();
    void pauseRequested();
    void messageRequested(const QString &text);
    void cacheRequested(const QStringList &videoIds);

    // To C++ consumers.
    void pageReady();
    void timeUpdated(double seconds);

private:
    double m_currentTime = 0.0;
    bool m_pageReady = false;
};
