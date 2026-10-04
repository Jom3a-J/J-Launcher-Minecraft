// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPackStaging.h"

#include "FileSystem.h"
#include "MMCZip.h"

#include <QCoreApplication>
#include <QDir>

namespace ModPlatform::ServerPackStaging {

namespace {
QString listFileName(FileLists::List list)
{
    switch (list) {
        case FileLists::ClientOnly:
            return QStringLiteral("client-only.txt");
        case FileLists::ServerOnly:
            return QStringLiteral("server-only.txt");
        case FileLists::Include:
            return QStringLiteral("include.txt");
        case FileLists::Unknown:
            return QStringLiteral("unknown.txt");
    }
    return {};
}
}  // namespace

QString path(const QString& stagingPath, const QString& fileName)
{
    const QString folder = FS::PathCombine(stagingPath, "server-pack");
    return fileName.isEmpty() ? folder : FS::PathCombine(folder, fileName);
}

QString serverFilesPath(const QString& stagingPath, const QString& relativePath)
{
    const QString folder = FS::PathCombine(stagingPath, "server-pack", "server-files");
    return relativePath.isEmpty() ? folder : FS::PathCombine(folder, relativePath);
}

bool writeProviderMarker(const QString& path, const QString& provider)
{
    FS::ensureFilePathExists(path);
    QFile marker(path);
    const QByteArray contents = provider.toUtf8() + '\n';
    return marker.open(QIODevice::WriteOnly | QIODevice::Text) && marker.write(contents) == contents.size();
}

bool recordProvider(const QString& stagingPath, const QString& provider)
{
    return writeProviderMarker(path(stagingPath, QStringLiteral("provider.txt")), provider);
}

bool recordPublishedServerPack(const QString& stagingPath, const QString& provider)
{
    return writeProviderMarker(path(stagingPath, QStringLiteral("published-server-pack.txt")), provider);
}

QString extractPublishedServerPack(const QString& archivePath, const QString& stagingPath, const QString& provider,
                                   const QString& providerName)
{
    QString failedEntry;
    if (!MMCZip::validateArchive(archivePath, &failedEntry)) {
        return QCoreApplication::translate("ModPlatform::ServerPackStaging",
                                           "The %1 server-pack archive is corrupt (failed integrity check at %2).")
            .arg(providerName,
                 failedEntry.isEmpty() ? QCoreApplication::translate("ModPlatform::ServerPackStaging", "an unknown file")
                                       : failedEntry);
    }
    if (!MMCZip::extractDir(archivePath, serverFilesPath(stagingPath))) {
        return QCoreApplication::translate("ModPlatform::ServerPackStaging", "Failed to extract the %1 server-pack archive.")
            .arg(providerName);
    }
    if (!recordPublishedServerPack(stagingPath, provider)) {
        return QCoreApplication::translate("ModPlatform::ServerPackStaging", "Could not record the downloaded %1 server pack.")
            .arg(providerName);
    }
    return {};
}

bool FileLists::open(const QString& stagingPath, const QList<List>& lists)
{
    close();
    for (const List list : lists) {
        const QString filePath = path(stagingPath, listFileName(list));
        FS::ensureFilePathExists(filePath);
        QFile& file = m_files[list];
        file.setFileName(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            close();
            return false;
        }
    }
    m_open = true;
    return true;
}

void FileLists::add(List list, const QString& relativePath)
{
    QFile& file = m_files[list];
    if (!file.isOpen())
        return;
    file.write(QDir::fromNativeSeparators(relativePath).toUtf8());
    file.write("\n");
}

void FileLists::close()
{
    for (QFile& file : m_files) {
        if (file.isOpen())
            file.close();
    }
    m_open = false;
}

}  // namespace ModPlatform::ServerPackStaging
