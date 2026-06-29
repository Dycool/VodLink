#pragma once

#include <QString>
#include <QStringList>

namespace DebugLog
{
// Enables debug logging when arguments contain -debug or --debug. The log file
// is created beside the executable as VodLink-debug.txt and truncated on each
// run. Returns false only when the user explicitly requested debug logging and
// the file could not be opened.
bool initializeFromArguments(const QStringList &arguments, QString *error = nullptr);
void shutdown();

[[nodiscard]] bool enabled();
[[nodiscard]] QString filePath();

void write(const QString &message);
void write(const char *message);
void writeCategory(const QString &category, const QString &message);
}
