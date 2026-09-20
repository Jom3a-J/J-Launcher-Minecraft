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

#include "net/PartFile.h"

#include <QFileInfo>

#include "FileSystem.h"
#include "logs/Privacy.h"
#include "net/Logging.h"

#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#endif

namespace Net {

namespace {
/*! Longest absolute part file path we are willing to create.
 *
 *  MAX_PATH is 260 including the terminator; staying a little under it leaves room for the
 *  ".part" suffix we add and for any long path quirk in the components below us. A caller that
 *  cannot use a part file falls back to an ordinary download, so this is a capability check and
 *  never an error.
 */
constexpr int MaxPartPathLength = 250;
}  // namespace

PartFile::PartFile(QString targetPath) : m_targetPath(std::move(targetPath))
{
    m_partPath = partPathFor(m_targetPath);
    // Same prefix PSaveFile registers, so the resource scanners that already skip QSaveFile
    // temporaries skip the part file too.
    m_qsavePrefix = QFileInfo(m_targetPath).absoluteFilePath() + QLatin1Char('.');
}

PartFile::~PartFile()
{
    if (m_promoted || m_kept) {
        close();
        unregisterFromApplication();
        return;
    }
    // Anything that did not reach promote() and did not explicitly ask to keep its bytes is
    // scratch, and scratch must not outlive the download that created it.
    discard();
}

QString PartFile::partPathFor(const QString& targetPath)
{
    return targetPath + QStringLiteral(".part");
}

bool PartFile::isUsableFor(const QString& targetPath)
{
    if (targetPath.isEmpty())
        return false;
    const QString absolute = QFileInfo(partPathFor(targetPath)).absoluteFilePath();
    return absolute.size() <= MaxPartPathLength;
}

void PartFile::registerWithApplication()
{
#if defined(LAUNCHER_APPLICATION)
    if (m_registered)
        return;
    if (auto app = APPLICATION_DYN) {
        app->addQSavePath(m_qsavePrefix);
        m_registered = true;
    }
#endif
}

void PartFile::unregisterFromApplication()
{
#if defined(LAUNCHER_APPLICATION)
    if (!m_registered)
        return;
    m_registered = false;
    if (auto app = APPLICATION_DYN) {
        app->removeQSavePath(m_qsavePrefix);
    }
#endif
}

bool PartFile::open(QString* error)
{
    const auto fail = [&error](const QString& reason) {
        if (error)
            *error = reason;
        qCCritical(taskNetLogC) << "PartFile:" << Privacy::sanitizeText(reason);
        return false;
    };

    if (!isUsableFor(m_targetPath)) {
        return fail(QObject::tr("The download path is too long for a staged download."));
    }
    if (!FS::ensureFilePathExists(m_partPath)) {
        return fail(QObject::tr("Could not create the folder for %1.").arg(Privacy::sanitizePath(m_targetPath)));
    }

    registerWithApplication();

    m_file = std::make_unique<QFile>(m_partPath);
    // ReadWrite rather than WriteOnly: Truncate still clears a leftover file, but the handle can
    // also be used to read back what was written without reopening.
    if (!m_file->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        const QString reason = QObject::tr("Could not open %1 for writing: %2")
                                   .arg(Privacy::sanitizePath(m_partPath), m_file->errorString());
        m_file.reset();
        unregisterFromApplication();
        return fail(reason);
    }
    return true;
}

void PartFile::preallocate(qint64 total)
{
    if (!isOpen() || total <= 0)
        return;
    if (!m_file->resize(total)) {
        // Not fatal: the file simply grows as the segments write into it.
        qCDebug(taskNetLogC) << "PartFile: could not preallocate" << total << "bytes:" << m_file->errorString();
    }
}

bool PartFile::writeAt(qint64 offset, const QByteArray& data, QString* error)
{
    if (!isOpen()) {
        if (error)
            *error = QObject::tr("The staged download file is not open.");
        return false;
    }
    if (data.isEmpty())
        return true;

    if (!m_file->seek(offset)) {
        if (error)
            *error = QObject::tr("Could not seek to %1 in %2: %3")
                         .arg(QString::number(offset), Privacy::sanitizePath(m_partPath), m_file->errorString());
        return false;
    }
    if (m_file->write(data) != data.size()) {
        if (error)
            *error = QObject::tr("Failed writing into %1: %2")
                         .arg(Privacy::sanitizePath(m_partPath), m_file->errorString());
        return false;
    }
    return true;
}

bool PartFile::truncateAll(QString* error)
{
    if (!isOpen()) {
        if (error)
            *error = QObject::tr("The staged download file is not open.");
        return false;
    }
    if (!m_file->resize(0) || !m_file->seek(0)) {
        if (error)
            *error = QObject::tr("Could not reset %1: %2")
                         .arg(Privacy::sanitizePath(m_partPath), m_file->errorString());
        return false;
    }
    return true;
}

bool PartFile::flush()
{
    return isOpen() ? m_file->flush() : false;
}

qint64 PartFile::size() const
{
    if (m_file && m_file->isOpen())
        return m_file->size();
    return QFileInfo(m_partPath).size();
}

void PartFile::close()
{
    if (m_file) {
        if (m_file->isOpen()) {
            m_file->flush();
            m_file->close();
        }
        m_file.reset();
    }
}

bool PartFile::promote(QString* error)
{
    if (m_file && m_file->isOpen() && !m_file->flush()) {
        if (error)
            *error = QObject::tr("Failed writing into %1: %2")
                         .arg(Privacy::sanitizePath(m_partPath), m_file->errorString());
        return false;
    }
    close();

    if (!FS::move(m_partPath, m_targetPath)) {
        if (error)
            *error = QObject::tr("Could not move the finished download into place at %1.")
                         .arg(Privacy::sanitizePath(m_targetPath));
        return false;
    }
    m_promoted = true;
    unregisterFromApplication();
    return true;
}

void PartFile::discard()
{
    close();
    // Only the first discard removes anything. Whatever sits at that path afterwards belongs to
    // whoever opened it next - a retry of this download, or the next fallback URL for the same
    // file - and is none of this object's business.
    if (!m_promoted && !m_discarded && !m_partPath.isEmpty() && QFileInfo::exists(m_partPath)) {
        if (!QFile::remove(m_partPath)) {
            qCDebug(taskNetLogC) << "PartFile: could not remove" << Privacy::sanitizePath(m_partPath);
        }
    }
    m_discarded = true;
    unregisterFromApplication();
}

void PartFile::keep()
{
    m_kept = true;
    close();
    unregisterFromApplication();
}

}  // namespace Net
