#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

struct GameDefinition
{
    QString name;
    QStringList processNames;
};

inline bool operator==(const GameDefinition &left, const GameDefinition &right)
{
    return left.name == right.name && left.processNames == right.processNames;
}

Q_DECLARE_METATYPE(GameDefinition)
