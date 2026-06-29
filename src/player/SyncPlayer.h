#pragma once

#include "library/Vod.h"

#include <QVector>
#include <QWidget>

class PlayerBridge;
class QWebChannel;
class QWebEngineProfile;
class QWebEngineView;

// Plays a "session group" of overlapping VODs (mine + friends') and lets you
// switch between them while staying locked to the same real-world moment. Each
// VOD knows its UTC start; switching seeks the target to
// (activeStart + currentTime) - targetStart, clamped to the target's length.
class SyncPlayer final : public QWidget
{
    Q_OBJECT

public:
    explicit SyncPlayer(QWidget *parent = nullptr);

    // The synced set, sorted by start time. Pass the whole connected component of
    // overlapping VODs for one game. Does not start playback.
    void setGroup(const QVector<Vod> &vods);
    [[nodiscard]] const QVector<Vod> &group() const { return m_group; }

    // Switch to group entry `index`, preserving the current wall-clock position.
    void playIndex(int index);

    // Switch to group entry `index` at an explicit offset in the selected VOD.
    // This is used when opening from a card/link where the caller already mapped
    // a real-world timestamp into that VOD's timeline.
    void playIndexAt(int index, double offsetSeconds);

    [[nodiscard]] int currentIndex() const { return m_current; }
    [[nodiscard]] double currentTimeSeconds() const { return m_currentTime; }
    void clear();

signals:
    void currentChanged(int index);

private slots:
    void onPageReady();
    void onTimeUpdate(double seconds);

private:
    void loadCurrent(double offsetSeconds, const QString &note = {});
    [[nodiscard]] qint64 absolutePositionMs() const;

    QWebEngineProfile *m_profile = nullptr;
    QWebEngineView *m_view = nullptr;
    QWebChannel *m_channel = nullptr;
    PlayerBridge *m_bridge = nullptr;

    QVector<Vod> m_group;
    int m_current = -1;
    double m_currentTime = 0.0;
    bool m_pageReady = false;
    int m_pendingIndex = -1;
    double m_pendingOffsetSeconds = 0.0;
};
