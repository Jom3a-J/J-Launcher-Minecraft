// SPDX-License-Identifier: GPL-3.0-only

#include "ServerFiles.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace ServerFiles {

qint64 directorySize(const QString &directoryPath)
{
    qint64 total = 0;
    QDirIterator iterator(directoryPath, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

QString formatByteSize(qint64 bytes)
{
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1024ll * 1024 * 1024) return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
}

ServerContentFileDetails describeContentFile(const QString &fileName)
{
    ServerContentFileDetails details;
    QString name = fileName;
    const QString disabledSuffix = QStringLiteral(".disabled");
    const QString jarSuffix = QStringLiteral(".jar");
    details.enabled = !name.endsWith(disabledSuffix, Qt::CaseInsensitive);
    if (!details.enabled) name.chop(disabledSuffix.size());
    if (name.endsWith(jarSuffix, Qt::CaseInsensitive)) name.chop(jarSuffix.size());

    // The version starts at the first digit that follows a '-' or '_'.
    int versionStart = -1;
    for (int index = 1; index < name.size(); ++index) {
        if ((name.at(index - 1) == '-' || name.at(index - 1) == '_') && name.at(index).isDigit()) {
            versionStart = index;
            break;
        }
    }

    details.name = versionStart > 0 ? name.left(versionStart - 1) : name;
    details.version = versionStart > 0 ? name.mid(versionStart) : QString();
    details.name.replace('-', ' ');
    details.name.replace('_', ' ');
    return details;
}

}  // namespace ServerFiles
