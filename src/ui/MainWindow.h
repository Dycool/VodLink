#pragma once

#include "library/Vod.h"

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QSet>
#include <QVector>

class AppController;
class QAction;
class QAbstractButton;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QNetworkAccessManager;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QSpinBox;
class QSystemTrayIcon;
class QTimer;
class SyncPlayer;
class QToolButton;
class QWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AppController *controller, QWidget *parent = nullptr);
    void showInitial(bool minimized);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Suppresses hover tooltips on clickable widgets app-wide.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void reloadLibrary();
    void reloadFriends();
    void updateStatus(const QString &message, bool streaming);
    void onAccountChanged(const QString &email);
    void addFriendClicked();
    void addCurrentGameClicked();
    void toggleShare(bool enabled);
    void openSettingsDialog();
    void deleteVodClicked();
    void copyVodLinkClicked();

private:
    void buildUi();
    QWidget *buildSetupPage();
    QWidget *buildRestorePage();
    void updateAuthGate();
    QWidget *buildFriendsPanel();
    QWidget *buildLibrarySurface();
    QWidget *buildVodViewer();
    QWidget *buildStatCard(const QString &value, const QString &label, const QString &symbol);
    void setupTray();
    void setupAccountMenu();
    void requestAppQuit();
    void setFriendsPanelVisible(bool visible);
    void updateAutoRecordLabel();
    void updateFooterIdentity();
    static void fadeInWidget(QWidget *widget, int durationMs = 180);
    void applyVodFilters();
    void rebuildVodGrid();
    void updateVodCardSelection();
    void selectVod(int row);
    void showVodInViewer(const Vod &vod, int selectedGridRow);
    void showVodInViewerAt(const Vod &vod, int selectedGridRow, double startSeconds);
    void rebuildParticipantVodStrip(const Vod &vod);
    double syncedOffsetForLinkedVod(const Vod &sourceVod, const Vod &targetVod, double sourceOffsetSeconds) const;
    void clearVodViewer();
    void refreshStats();
    QWidget *createFriendRow(const AccountProfile &profile);
    QVector<Vod> linkedFriendVodsForCard(const Vod &vod) const;
    void requestProfileIcon(const QString &pictureUrl);
    void requestVodThumbnail(const Vod &vod, QAbstractButton *button);
    void scheduleVodThumbnailRefresh(const Vod &vod, QAbstractButton *button);
    void applyProfileIcon(const QString &pictureUrl, const QIcon &icon);
    void applyAccountIcon(const QIcon &icon);
    static QIcon fallbackProfileIcon(const QString &text);
    static QIcon vodPlaceholderIcon(const QString &text);
    static QString durationText(qint64 durationMs);
    static QString relativeTimeText(const QDateTime &when);
    static QString vodLabel(const Vod &vod);
    static QString cardText(const Vod &vod);
    static QString youtubeWatchUrl(const QString &videoId, int startSeconds = -1);

    AppController *m_controller;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_setupPage = nullptr;
    QWidget *m_restorePage = nullptr;
    QWidget *m_mainPage = nullptr;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_autoRecordLabel = nullptr;
    bool m_isStreaming = false;
    QString m_lastStatusMessage;
    QToolButton *m_accountButton = nullptr;
    QMenu *m_accountMenu = nullptr;

    QWidget *m_friendsPanel = nullptr;
    QLabel *m_selfAvatar = nullptr;
    QLabel *m_selfName = nullptr;
    QLineEdit *m_friendEmailEdit = nullptr;
    QListWidget *m_friendsList = nullptr;
    QCheckBox *m_shareToggle = nullptr;
    QLabel *m_workerHint = nullptr;

    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_libraryGameFilter = nullptr;
    QComboBox *m_sortCombo = nullptr;
    QComboBox *m_orderCombo = nullptr;
    QComboBox *m_visibilityCombo = nullptr;
    QScrollArea *m_vodScroll = nullptr;
    QWidget *m_vodGridWidget = nullptr;
    QGridLayout *m_vodGridLayout = nullptr;
    QVector<Vod> m_libraryVods;
    QVector<Vod> m_filteredVods;
    int m_selectedVodRow = -1;
    bool m_hasViewerVod = false;
    Vod m_viewerVod;

    QWidget *m_viewerPanel = nullptr;
    QLabel *m_previewTitle = nullptr;
    QLabel *m_previewMeta = nullptr;
    QWidget *m_participantStrip = nullptr;
    QHBoxLayout *m_participantLayout = nullptr;
    SyncPlayer *m_syncPlayer = nullptr;
    QPushButton *m_copyVodLinkButton = nullptr;
    QPushButton *m_deleteVodButton = nullptr;

    QLabel *m_vodsStat = nullptr;
    QLabel *m_watchTimeStat = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_autoRecordAction = nullptr;
    QAction *m_shareAction = nullptr;
    bool m_trayMessageShown = false;
    bool m_quitRequested = false;
    QNetworkAccessManager *m_avatarNetwork = nullptr;
    QHash<QString, QIcon> m_avatarCache;
    QSet<QString> m_avatarRequests;
    QHash<QString, QIcon> m_thumbnailCache;
    QSet<QString> m_thumbnailRequests;
    QHash<QString, QByteArray> m_thumbnailHashes;
    QHash<QString, int> m_thumbnailUnchangedCounts;
    QHash<QString, qint64> m_thumbnailNextProbeMs;
    double m_viewerStartSeconds = 0.0;
};
