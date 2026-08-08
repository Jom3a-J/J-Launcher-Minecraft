// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMap>
#include <QString>

class ServerProperties
{
public:
    static QMap<QString, QString> load(const QString &path, QString *error = nullptr);
    static bool validate(const QMap<QString, QString> &values, QString *error = nullptr);
    static bool save(const QString &path, const QMap<QString, QString> &values,
                     QString *error = nullptr);
};
