#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class ObsRuntime
{
public:
    bool prepare(QString *error);

    [[nodiscard]] QString rootPath() const { return m_rootPath; }
    [[nodiscard]] QString moduleConfigPath() const { return m_moduleConfigPath; }
    [[nodiscard]] QString privateConfigRoot() const { return m_privateConfigRoot; }
    [[nodiscard]] QString profilePath() const { return m_profilePath; }
    [[nodiscard]] QString coreDataPath() const;
    [[nodiscard]] QStringList coreDataPaths() const;
    [[nodiscard]] QStringList moduleBinPaths() const;
    [[nodiscard]] QStringList moduleDataPaths() const;
    [[nodiscard]] QList<QPair<QString, QString>> modulePathPairs() const;

    [[nodiscard]] QByteArray moduleConfigPathUtf8() const;
    [[nodiscard]] QByteArray coreDataPathUtf8() const;
    [[nodiscard]] bool validateRuntimeLayout(QString *error) const;

private:
    bool extractEmbeddedRuntime(QString *error);
#if defined(Q_OS_WIN)
    bool extractNativeWindowsRuntime(QString *error);
#endif
    bool extractResourceDirectory(const QString &resourcePath, const QString &destinationPath,
                                  QString *error);
    bool ensurePrivateConfig(QString *error);
    bool installPrivateEnvironment(QString *error) const;
    bool addWindowsDllSearchPaths(QString *error) const;

    QString m_rootPath;
    QString m_privateConfigRoot;
    QString m_moduleConfigPath;
    QString m_profilePath;
    QString m_cachePath;
    QString m_logPath;
};
