// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QFile>
#include <QList>
#include <QString>

#include <array>

/*! The files a modpack install leaves in <staging>/server-pack so the matching server can be
 *  built from it. ServerPackCompatibility and ServerModpackContent read them back. */
namespace ModPlatform::ServerPackStaging {

/// <staging>/server-pack, or a file inside it.
QString path(const QString& stagingPath, const QString& fileName = {});
/// <staging>/server-pack/server-files, where a server pack's own files go, or a file inside it.
QString serverFilesPath(const QString& stagingPath, const QString& relativePath = {});
/// Writes "<provider>\n" to a marker file such as provider.txt. False if it cannot be written whole.
bool writeProviderMarker(const QString& path, const QString& provider);
/// Writes server-pack/provider.txt, naming the platform the pack came from.
bool recordProvider(const QString& stagingPath, const QString& provider);
/// Writes server-pack/published-server-pack.txt: server-files holds the platform's own server pack.
bool recordPublishedServerPack(const QString& stagingPath, const QString& provider);

/*! Checks a downloaded server-pack archive, unpacks it into server-files and records it as the
 *  provider's published pack. Returns an error message naming providerName, or an empty string. */
QString extractPublishedServerPack(const QString& archivePath, const QString& stagingPath, const QString& provider,
                                   const QString& providerName);

/// The lists that sort a pack's files for the server, one relative path per line.
class FileLists {
   public:
    enum List { ClientOnly, ServerOnly, Include, Unknown };

    /// Creates the given lists, empty. False if one of them cannot be written.
    bool open(const QString& stagingPath, const QList<List>& lists);
    bool isOpen() const { return m_open; }
    /// Adds relativePath to a list opened by open(); does nothing otherwise.
    void add(List list, const QString& relativePath);
    void close();

   private:
    std::array<QFile, 4> m_files;
    bool m_open = false;
};

}  // namespace ModPlatform::ServerPackStaging
