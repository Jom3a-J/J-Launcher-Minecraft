// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPackImportTransaction.h"

#include "archive/ArchiveReader.h"
#include <archive.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <utility>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr auto SERVER_PACK_TEST_FAILURE_ENV =
    "JLAUNCHER_TEST_SERVER_PACK_IMPORT_FAIL_AFTER";
constexpr qsizetype SERVER_PACK_COPY_BUFFER_SIZE = 1024 * 1024;

struct StagedServerPackFile {
    QString relativePath;
    QString stagedPath;
};

struct ServerPackRollbackFile {
    QString targetPath;
    QString backupPath;
    bool existed = false;
    bool published = false;
};

QString normalizedComparisonPath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(path));
    const bool isWindowsDriveRoot = normalized.size() == 3
        && normalized.at(1) == ':' && normalized.endsWith('/');
    if (normalized.size() > 1 && normalized.endsWith('/') && !isWindowsDriveRoot) {
        normalized.chop(1);
    }
    return normalized;
}

bool pathsEqual(const QString& left, const QString& right)
{
    const Qt::CaseSensitivity sensitivity =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    return normalizedComparisonPath(left).compare(
               normalizedComparisonPath(right), sensitivity) == 0;
}

bool isPathWithin(const QString& rootPath, const QString& candidatePath)
{
    const QString root = normalizedComparisonPath(rootPath);
    const QString candidate = normalizedComparisonPath(candidatePath);
    const Qt::CaseSensitivity sensitivity =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif

    if (candidate.compare(root, sensitivity) == 0) {
        return true;
    }
    if (root == QStringLiteral("/") || root.endsWith('/')) {
        return candidate.startsWith(root, sensitivity);
    }
    return candidate.startsWith(root + '/', sensitivity);
}

bool isLinkOrReparsePoint(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        return true;
    }

#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool copyFileContents(const QString& sourcePath, const QString& targetPath,
                      QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Could not read '%1': %2")
                         .arg(sourcePath, source.errorString());
        }
        return false;
    }

    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QObject::tr("Could not prepare '%1': %2")
                         .arg(targetPath, target.errorString());
        }
        return false;
    }

    QByteArray buffer(SERVER_PACK_COPY_BUFFER_SIZE, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 bytesRead = source.read(buffer.data(), buffer.size());
        if (bytesRead < 0
            || target.write(buffer.constData(), bytesRead) != bytesRead) {
            if (error) {
                *error = QObject::tr("Could not copy '%1'.").arg(sourcePath);
            }
            return false;
        }
    }

    if (!target.commit()) {
        if (error) {
            *error = QObject::tr("Could not commit '%1': %2")
                         .arg(targetPath, target.errorString());
        }
        return false;
    }
    return true;
}

bool validateWindowsPathComponents(const QString& path, QString* unsafeComponent)
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
    for (const QString& component : components) {
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

        QString deviceStem = component.section('.', 0, 0).trimmed();
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

bool normalizeServerPackPath(QString path, QString* normalized,
                             QString* unsafeComponent)
{
    path.replace('\\', '/');
    while (path.startsWith("overrides/")) {
        path.remove(0, QStringLiteral("overrides/").size());
    }
    while (path.startsWith(".minecraft/")) {
        path.remove(0, QStringLiteral(".minecraft/").size());
    }

    if (path.isEmpty() || path.startsWith('/') || QDir::isAbsolutePath(path)
        || path.contains(QStringLiteral(".."))) {
        return false;
    }
    if (path.contains(QChar::Null)) {
        if (unsafeComponent) {
            *unsafeComponent = path;
        }
        return false;
    }
    if (!validateWindowsPathComponents(path, unsafeComponent)) {
        return false;
    }

    const QStringList components = path.split('/', Qt::KeepEmptyParts);
    for (const QString& component : components) {
        if (component == QStringLiteral("..")) {
            return false;
        }
    }

    path = QDir::cleanPath(path);
    if (path.isEmpty() || path == QStringLiteral(".")
        || path.startsWith("../") || path == QStringLiteral("..")) {
        return false;
    }

    *normalized = path;
    return true;
}
}  // namespace

class ServerPackImportTransaction::Private final
{
  public:
    explicit Private(QString serverRoot)
        : m_requestedServerRoot(std::move(serverRoot))
        , m_serverRoot(QDir(m_requestedServerRoot).absolutePath())
    {
    }

    bool stage(const QString& archivePath, QString* error)
    {
        if (!validateServerRoot(error)) {
            return false;
        }

        m_transaction = std::make_unique<QTemporaryDir>();
        if (!m_transaction || !m_transaction->isValid()) {
            if (error) {
                *error = QObject::tr(
                    "Could not create a server-pack transaction directory.");
            }
            return false;
        }

        m_stagingRoot = QDir(m_transaction->path()).filePath("payload");
        if (!QDir().mkpath(m_stagingRoot)) {
            if (error) {
                *error = QObject::tr(
                    "Could not create the server-pack staging directory.");
            }
            return false;
        }

        MMCZip::ArchiveReader archive(archivePath);
        QSet<QString> seenTargets;
        QString stageError;
        int extractedCount = 0;

        const QStringList allowedRoots = {
            "mods/", "config/", "configureddefaults/", "datapacks/",
            "defaultconfigs/", "ftbteambases/", "global_packs/", "kubejs/",
            "openloader/", "patchouli_books/", "resources/", "scripts/",
            "structures/"
        };
        const QStringList allowedRootFiles = {
            "server.properties", "default-server.properties", "server-icon.png"
        };

        const bool parsed = archive.parse([&](MMCZip::ArchiveReader::File* input) {
            auto skipEntry = [&]() {
                if (!input->skip()) {
                    stageError = QObject::tr("Could not read the server-pack archive.");
                    return false;
                }
                return true;
            };

            QString relativePath;
            QString unsafeComponent;
            if (!normalizeServerPackPath(input->filename(), &relativePath,
                                          &unsafeComponent)) {
                if (!unsafeComponent.isEmpty()) {
                    stageError = QObject::tr(
                        "The server-pack contains an unsafe Windows path '%1'.")
                        .arg(input->filename());
                    return false;
                }
                return skipEntry();
            }

            bool allowed = allowedRootFiles.contains(relativePath, Qt::CaseInsensitive);
            if (!allowed) {
                for (const QString& root : allowedRoots) {
                    if (relativePath.startsWith(root)) {
                        allowed = true;
                        break;
                    }
                }
            }
            if (!allowed || !input->isFile()) {
                return skipEntry();
            }

            const QString targetRelativePath =
                relativePath.compare("default-server.properties", Qt::CaseInsensitive) == 0
                ? QStringLiteral("server.properties")
                : relativePath;
            const QString targetKey = targetRelativePath.toCaseFolded();
            if (seenTargets.contains(targetKey)) {
                stageError = QObject::tr(
                    "The server-pack contains more than one file for '%1'.")
                    .arg(targetRelativePath);
                return false;
            }
            seenTargets.insert(targetKey);

            const QString stagedPath =
                QDir(m_stagingRoot).filePath(targetRelativePath);
            if (!QDir().mkpath(QFileInfo(stagedPath).dir().absolutePath())) {
                stageError = QObject::tr(
                    "Could not create staging folders for '%1'.")
                    .arg(relativePath);
                return false;
            }

            int readStatus = ARCHIVE_OK;
            const QByteArray data = input->readAll(&readStatus);
            if (readStatus != ARCHIVE_OK && readStatus != ARCHIVE_EOF) {
                stageError = QObject::tr(
                    "Could not read '%1' from the server-pack archive.")
                    .arg(relativePath);
                return false;
            }

            QSaveFile stagedFile(stagedPath);
            if (!stagedFile.open(QIODevice::WriteOnly)
                || stagedFile.write(data) != data.size()
                || !stagedFile.commit()) {
                stageError = QObject::tr("Could not stage '%1'.").arg(relativePath);
                return false;
            }

            m_files.append({ targetRelativePath, stagedPath });
            ++extractedCount;
            return true;
        });

        if (!parsed) {
            if (error) {
                *error = stageError.isEmpty()
                    ? QObject::tr("Could not open or read the server-pack archive.")
                    : stageError;
            }
            return false;
        }

        if (extractedCount == 0) {
            if (error) {
                *error = QObject::tr(
                    "The archive contains no server files in mods, config, defaultconfigs, kubejs, or scripts.");
            }
            return false;
        }
        return true;
    }

    bool publish(QString* error)
    {
        if (!validateServerRoot(error) || !prepareJournal(error)) {
            return false;
        }

        bool failureInjectionEnabled = false;
        const int failureAfter = qEnvironmentVariableIntValue(
            SERVER_PACK_TEST_FAILURE_ENV, &failureInjectionEnabled);
        const int injectedFailureAfter =
            failureInjectionEnabled && failureAfter > 0 ? failureAfter : -1;

        int publishedCount = 0;
        for (const StagedServerPackFile& file : m_files) {
            if (injectedFailureAfter > 0
                && publishedCount >= injectedFailureAfter) {
                return failAndRollback(
                    QObject::tr("Test failure injected during server-pack publication."),
                    error);
            }

            const QString targetPath = QDir(m_serverRoot).filePath(file.relativePath);
            const QString parentPath = QFileInfo(targetPath).dir().absolutePath();
            QString validationError;
            if (!collectMissingDirectories(parentPath, &validationError)
                || !QDir().mkpath(parentPath)
                || !validateExistingDirectoryPath(parentPath, &validationError)) {
                if (validationError.isEmpty()) {
                    validationError = QObject::tr(
                        "Could not create the server-pack target folder: %1")
                        .arg(parentPath);
                }
                return failAndRollback(validationError, error);
            }
            if (!validateTargetPath(targetPath, &validationError)) {
                return failAndRollback(validationError, error);
            }
            if (!copyFileContents(file.stagedPath, targetPath, &validationError)) {
                return failAndRollback(validationError, error);
            }
            m_rollbackFiles[publishedCount].published = true;
            ++publishedCount;
        }

        QString cleanupError;
        if (!removeTransactionMaterial(&cleanupError)) {
            if (error) {
                *error = QObject::tr(
                             "The server-pack was imported, but transaction cleanup failed. "
                             "Recovery material was preserved at %1: %2")
                             .arg(m_transaction->path(), cleanupError);
            }
            m_transaction->setAutoRemove(false);
            return false;
        }
        return true;
    }

  private:
    bool validateServerRoot(QString* error) const
    {
        if (m_requestedServerRoot.isEmpty()) {
            if (error) {
                *error = QObject::tr(
                    "The server directory must be set before importing a server pack.");
            }
            return false;
        }

        const QFileInfo rootInfo(m_serverRoot);
        if (!rootInfo.exists()) {
            if (error) {
                *error = QObject::tr("The server directory does not exist: %1")
                    .arg(m_serverRoot);
            }
            return false;
        }
        if (!rootInfo.isDir()) {
            if (error) {
                *error = QObject::tr("The server directory is not a directory: %1")
                    .arg(m_serverRoot);
            }
            return false;
        }
        if (isLinkOrReparsePoint(m_serverRoot)) {
            if (error) {
                *error = QObject::tr(
                    "The server directory uses a symbolic link or reparse point: %1")
                    .arg(m_serverRoot);
            }
            return false;
        }
        return true;
    }

    bool validateExistingDirectoryPath(const QString& directoryPath,
                                       QString* error) const
    {
        if (!isPathWithin(m_serverRoot, directoryPath)) {
            if (error) {
                *error = QObject::tr("The server-pack target is outside the server folder.");
            }
            return false;
        }

        QString current = QDir::cleanPath(directoryPath);
        while (true) {
            if (!isPathWithin(m_serverRoot, current)) {
                if (error) {
                    *error = QObject::tr("The server-pack target is outside the server folder.");
                }
                return false;
            }
            if (isLinkOrReparsePoint(current)) {
                if (error) {
                    *error = QObject::tr(
                        "The server-pack target uses a symbolic link or reparse point: %1")
                        .arg(current);
                }
                return false;
            }
            const QFileInfo info(current);
            if (!info.exists() || !info.isDir()) {
                if (error) {
                    *error = QObject::tr(
                        "The server-pack target folder is unavailable: %1")
                        .arg(current);
                }
                return false;
            }
            if (pathsEqual(current, m_serverRoot)) {
                return true;
            }
            const QString parent = QFileInfo(current).dir().absolutePath();
            if (parent == current || !isPathWithin(m_serverRoot, parent)) {
                if (error) {
                    *error = QObject::tr("The server-pack target is outside the server folder.");
                }
                return false;
            }
            current = parent;
        }
    }

    bool collectMissingDirectories(const QString& directoryPath, QString* error)
    {
        if (!isPathWithin(m_serverRoot, directoryPath)) {
            if (error) {
                *error = QObject::tr("The server-pack target is outside the server folder.");
            }
            return false;
        }

        QString current = QDir::cleanPath(directoryPath);
        QStringList missing;
        while (true) {
            if (!isPathWithin(m_serverRoot, current)) {
                if (error) {
                    *error = QObject::tr("The server-pack target is outside the server folder.");
                }
                return false;
            }
            if (isLinkOrReparsePoint(current)) {
                if (error) {
                    *error = QObject::tr(
                        "The server-pack target uses a symbolic link or reparse point: %1")
                        .arg(current);
                }
                return false;
            }

            const QFileInfo info(current);
            if (info.exists()) {
                if (!info.isDir()) {
                    if (error) {
                        *error = QObject::tr(
                            "The server-pack target folder is not a directory: %1")
                            .arg(current);
                    }
                    return false;
                }
                if (!pathsEqual(current, m_serverRoot)) {
                    // An existing ancestor is safe only after its full chain
                    // has been checked as well.
                    if (!validateExistingDirectoryPath(current, error)) {
                        return false;
                    }
                }
                break;
            }

            if (pathsEqual(current, m_serverRoot)) {
                if (error) {
                    *error = QObject::tr("The server directory is unavailable: %1")
                        .arg(m_serverRoot);
                }
                return false;
            }
            missing.prepend(current);

            const QString parent = QFileInfo(current).dir().absolutePath();
            if (parent == current || !isPathWithin(m_serverRoot, parent)) {
                if (error) {
                    *error = QObject::tr(
                        "Could not find a safe parent for the server folder.");
                }
                return false;
            }
            current = parent;
        }

        for (const QString& path : missing) {
            if (!m_createdDirectories.contains(path)) {
                m_createdDirectories.append(path);
            }
        }
        return true;
    }

    bool validateTargetPath(const QString& targetPath, QString* error) const
    {
        if (!isPathWithin(m_serverRoot, targetPath)) {
            if (error) {
                *error = QObject::tr("The server-pack target is outside the server folder.");
            }
            return false;
        }
        if (isLinkOrReparsePoint(targetPath)) {
            if (error) {
                *error = QObject::tr(
                    "The server-pack would write through a symbolic link or reparse point: %1")
                    .arg(targetPath);
            }
            return false;
        }

        const QFileInfo info(targetPath);
        if (info.exists() && !info.isFile()) {
            if (error) {
                *error = QObject::tr(
                    "The server-pack target is not a regular file: %1")
                    .arg(targetPath);
            }
            return false;
        }
        return true;
    }

    bool prepareJournal(QString* error)
    {
        if (!m_transaction || !m_transaction->isValid()) {
            if (error) {
                *error = QObject::tr(
                    "Could not create a server-pack transaction directory.");
            }
            return false;
        }
        if (!QDir().mkpath(QDir(m_transaction->path()).filePath("rollback"))) {
            if (error) {
                *error = QObject::tr(
                    "Could not create the server-pack rollback directory.");
            }
            return false;
        }

        for (int index = 0; index < m_files.size(); ++index) {
            const StagedServerPackFile& file = m_files.at(index);
            const QString targetPath = QDir(m_serverRoot).filePath(file.relativePath);
            const QString parentPath = QFileInfo(targetPath).dir().absolutePath();
            const QFileInfo targetInfo(targetPath);
            if (targetInfo.exists()) {
                if (!validateExistingDirectoryPath(parentPath, error)) {
                    return false;
                }
            } else if (!collectMissingDirectories(parentPath, error)) {
                return false;
            }
            if (!validateTargetPath(targetPath, error)) {
                return false;
            }

            ServerPackRollbackFile rollbackFile;
            rollbackFile.targetPath = targetPath;
            rollbackFile.existed = targetInfo.exists();
            if (rollbackFile.existed) {
                rollbackFile.backupPath = QDir(m_transaction->path())
                    .filePath(QStringLiteral("rollback/%1.bin").arg(index));
                QString backupError;
                if (!copyFileContents(targetPath, rollbackFile.backupPath,
                                      &backupError)) {
                    if (error) {
                        *error = QObject::tr(
                                     "Could not journal '%1' before import: %2")
                                     .arg(targetPath, backupError);
                    }
                    return false;
                }
            }
            m_rollbackFiles.append(std::move(rollbackFile));
        }
        return true;
    }

    bool rollback(QStringList* failures)
    {
        bool complete = true;
        for (auto it = m_rollbackFiles.crbegin();
             it != m_rollbackFiles.crend(); ++it) {
            const ServerPackRollbackFile& rollbackFile = *it;
            if (!rollbackFile.published) {
                continue;
            }
            QString validationError;
            const QString parentPath = QFileInfo(rollbackFile.targetPath)
                .dir().absolutePath();
            if (!validateExistingDirectoryPath(parentPath, &validationError)
                || !validateTargetPath(rollbackFile.targetPath, &validationError)) {
                complete = false;
                if (failures) {
                    failures->append(validationError);
                }
                continue;
            }

            if (rollbackFile.existed) {
                if (!copyFileContents(rollbackFile.backupPath,
                                      rollbackFile.targetPath, &validationError)) {
                    complete = false;
                    if (failures) {
                        failures->append(validationError);
                    }
                }
            } else {
                const QFileInfo targetInfo(rollbackFile.targetPath);
                if (targetInfo.exists()
                    && (!targetInfo.isFile()
                        || !QFile::remove(rollbackFile.targetPath))) {
                    complete = false;
                    if (failures) {
                        failures->append(QObject::tr(
                            "Could not remove newly published '%1'.")
                            .arg(rollbackFile.targetPath));
                    }
                }
            }
        }

        for (auto it = m_createdDirectories.crbegin();
             it != m_createdDirectories.crend(); ++it) {
            const QString& directory = *it;
            if (isLinkOrReparsePoint(directory)) {
                complete = false;
                if (failures) {
                    failures->append(QObject::tr(
                        "Could not remove unsafe created directory '%1'.")
                        .arg(directory));
                }
                continue;
            }

            const QFileInfo info(directory);
            if (!info.exists()) {
                continue;
            }
            if (!info.isDir()) {
                complete = false;
                if (failures) {
                    failures->append(QObject::tr(
                        "Created transaction path is no longer a directory: %1")
                        .arg(directory));
                }
                continue;
            }

            const QDir dir(directory);
            if (!dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                               | QDir::Hidden | QDir::System).isEmpty()) {
                complete = false;
                if (failures) {
                    failures->append(QObject::tr(
                        "Created directory is not empty: %1")
                        .arg(directory));
                }
                continue;
            }
            if (!QDir().rmdir(directory)) {
                complete = false;
                if (failures) {
                    failures->append(QObject::tr(
                        "Could not remove created directory '%1'.")
                        .arg(directory));
                }
            }
        }
        return complete;
    }

    bool removeTransactionMaterial(QString* error)
    {
        if (!m_transaction->remove()) {
            if (error) {
                *error = QObject::tr("Could not remove temporary transaction files.");
            }
            m_transaction->setAutoRemove(false);
            return false;
        }
        return true;
    }

    bool failAndRollback(const QString& reason, QString* error)
    {
        QStringList rollbackFailures;
        if (!rollback(&rollbackFailures)) {
            m_transaction->setAutoRemove(false);
            if (error) {
                *error = reason + QObject::tr(
                    " Rollback was incomplete; recovery material was preserved at %1.")
                    .arg(m_transaction->path());
                if (!rollbackFailures.isEmpty()) {
                    *error += QStringLiteral(" ") + rollbackFailures.join(' ');
                }
            }
            return false;
        }

        QString cleanupError;
        if (!removeTransactionMaterial(&cleanupError)) {
            if (error) {
                *error = reason + QObject::tr(
                    " The live server was rolled back, but recovery material was preserved at %1: %2")
                    .arg(m_transaction->path(), cleanupError);
            }
            return false;
        }

        if (error) {
            *error = reason + QObject::tr(" The live server was rolled back successfully.");
        }
        return false;
    }

    QString m_requestedServerRoot;
    QString m_serverRoot;
    std::unique_ptr<QTemporaryDir> m_transaction;
    QString m_stagingRoot;
    QList<StagedServerPackFile> m_files;
    QList<ServerPackRollbackFile> m_rollbackFiles;
    QStringList m_createdDirectories;
};

ServerPackImportTransaction::ServerPackImportTransaction(QString serverRoot)
    : m_private(std::make_unique<Private>(std::move(serverRoot)))
{
}

ServerPackImportTransaction::~ServerPackImportTransaction() = default;

bool ServerPackImportTransaction::stage(const QString& archivePath, QString* error)
{
    return m_private->stage(archivePath, error);
}

bool ServerPackImportTransaction::publish(QString* error)
{
    return m_private->publish(error);
}
