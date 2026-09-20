// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QFile>
#include <QString>
#include <memory>

namespace Net {

/*! The scratch file a segmented download assembles its bytes in.
 *
 *  QSaveFile (and therefore PSaveFile) cannot back a segmented download: it hides the temporary
 *  file it writes to, so the file can neither be reopened nor written at arbitrary offsets in a
 *  way the caller can reason about. This is the explicit replacement - one handle, positioned
 *  writes, and an atomic promotion at the end through FS::move().
 *
 *  The part file lives next to its target (<target>.part) so that promotion is a same volume
 *  rename, and it registers the same "<absolute target>." prefix PSaveFile registers with the
 *  Application, so resource scanners skip it exactly as they skip a QSaveFile temporary.
 *
 *  Not thread safe by design: every write comes from the thread that owns the network replies,
 *  which is the single thread Qt delivers readyRead on. See SegmentedDownload's invariants.
 */
class PartFile {
   public:
    /// \a targetPath is the final destination, not the part file.
    explicit PartFile(QString targetPath);
    ~PartFile();

    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;

    /*! Whether a part file can be used for \a targetPath at all.
     *
     *  Windows path limits are the only reason this fails in practice; a caller that gets false
     *  must fall back to an ordinary unsegmented download rather than fail.
     */
    static bool isUsableFor(const QString& targetPath);

    static QString partPathFor(const QString& targetPath);

    QString targetPath() const { return m_targetPath; }
    QString partPath() const { return m_partPath; }

    /// Creates the containing directory and opens the part file, truncating anything left over.
    bool open(QString* error);

    /*! Best effort preallocation to \a total bytes.
     *
     *  Failure is not fatal - it only means the file grows as segments write into it - so this
     *  returns void and logs instead of failing the download.
     */
    void preallocate(qint64 total);

    /// Writes \a data at \a offset. Fails on short writes and on any device error.
    bool writeAt(qint64 offset, const QByteArray& data, QString* error);

    /// Drops everything written so far and rewinds to an empty file.
    bool truncateAll(QString* error);

    bool flush();
    qint64 size() const;
    bool isOpen() const { return m_file && m_file->isOpen(); }

    /*! Atomically replaces the target with the part file.
     *
     *  Uses FS::move(), which is std::filesystem::rename - a replacing MoveFileExW on Windows -
     *  with the launcher's existing copy and delete fallback.
     */
    bool promote(QString* error);

    /*! Closes and deletes the part file. Used for every failure that is not a user abort.
     *
     *  Idempotent, and deliberately so: a failed download discards its part file immediately, but
     *  the sinks that share this object outlive that moment. If a retry or a fallback URL has
     *  already opened a new part file at the same path by the time the old object is destroyed, a
     *  second removal would delete the live download's bytes.
     */
    void discard();

    /*! Closes the part file but leaves it on disk.
     *
     *  A user abort keeps the bytes for the lifetime of the staging directory that holds them.
     */
    void keep();

   private:
    void close();
    void registerWithApplication();
    void unregisterFromApplication();

    QString m_targetPath;
    QString m_partPath;
    QString m_qsavePrefix;
    bool m_registered = false;
    bool m_promoted = false;
    bool m_kept = false;
    bool m_discarded = false;
    std::unique_ptr<QFile> m_file;
};

}  // namespace Net
