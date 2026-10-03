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
