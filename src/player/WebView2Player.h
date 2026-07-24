#pragma once

#include <QStringList>
#include <QWidget>

#include <memory>

class QLabel;
class QJsonObject;
class QResizeEvent;

// Windows host for the exact lite-youtube page validated in the standalone
// proof. Qt WebEngine's Chromium build returns YouTube error 5 for that page,
// while the operating-system WebView2 runtime plays and seeks it correctly.
class WebView2Player final : public QWidget
{
    Q_OBJECT

public:
    explicit WebView2Player(QWidget *parent = nullptr);
    ~WebView2Player() override;

public slots:
    void loadVideo(const QString &videoId, double seekSeconds);
    void seek(double seconds);
    void play();
    void pause();
    void showMessage(const QString &text);
    void cacheVideos(const QStringList &videoIds);

signals:
    void pageReady();
    void timeUpdated(double seconds);
    void playerError(int code);
    void fullscreenToggleRequested();
    void debugMessage(const QString &category, const QString &message);
    void fatalError(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Impl;

    void initialize();
    void finishInitialization();
    void updateBounds();
    void handleWebMessage(const QString &json);
    void sendCommand(const QJsonObject &command);
    void fail(const QString &message);

    std::unique_ptr<Impl> m_impl;
    QLabel *m_status = nullptr;
    QString m_contentFolder;
};
