// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPaths.h"

#include <QDir>
#include <QStringList>

namespace ServerPaths {

QString normalizedRelativePath(QString path)
{
    path = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
    while (path.startsWith(QStringLiteral("./"))) {
        path.remove(0, 2);
    }
    return path;
}

bool isSafeRelativePath(const QString &path)
{
    const QString normalized = normalizedRelativePath(path);
    return !normalized.isEmpty() && normalized != QStringLiteral("..")
        && !normalized.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(normalized);
}

bool hasValidWindowsNames(const QString &path, QString *unsafeComponent)
{
    static const QStringList reservedDevices = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")
    };

    const QStringList components = path.split('/', Qt::KeepEmptyParts);
    for (const QString &component : components) {
        if (component.isEmpty()) {
            continue;
        }

        bool invalid = component == QStringLiteral(".");
        for (const QChar character : component) {
            const ushort code = character.unicode();
            if (code < 0x20 || (code >= 0x7f && code <= 0x9f)
                || QStringLiteral("<>:\"|?*").contains(character)) {
                invalid = true;
                break;
            }
        }
        if (component.endsWith('.') || component.endsWith(' ')) {
            invalid = true;
        }

        const QString deviceStem = component.section('.', 0, 0).trimmed();
        if (reservedDevices.contains(deviceStem, Qt::CaseInsensitive)) {
            invalid = true;
        }

        if (invalid) {
            if (unsafeComponent) {
                *unsafeComponent = component;
            }
            return false;
        }
    }
    return true;
}

}  // namespace ServerPaths
