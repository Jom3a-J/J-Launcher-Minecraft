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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "Application.h"
#include "FileSystem.h"
#include "net/PartFile.h"

namespace {

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

}  // namespace

class PartFileTest final : public QObject {
    Q_OBJECT

   private slots:
    void partFileSitsNextToItsTarget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");

        QCOMPARE(Net::PartFile::partPathFor(target), target + ".part");
        QCOMPARE(QFileInfo(Net::PartFile::partPathFor(target)).absolutePath(), QFileInfo(target).absolutePath());
    }

    void openCreatesMissingFoldersAndTruncates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "nested", "deeper", "pack.zip");

        // Something left over from an earlier attempt must not survive into a fresh download.
        QVERIFY(FS::ensureFilePathExists(Net::PartFile::partPathFor(target)));
        QFile stale(Net::PartFile::partPathFor(target));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("leftovers");
        stale.close();

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        QCOMPARE(part.size(), qint64(0));
    }

    void positionedWritesLandAtTheirOffsets()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        part.preallocate(9);

        // Deliberately out of order, the way concurrent segments arrive.
        QVERIFY(part.writeAt(6, QByteArrayLiteral("ghi"), &error));
        QVERIFY(part.writeAt(0, QByteArrayLiteral("abc"), &error));
        QVERIFY(part.writeAt(3, QByteArrayLiteral("def"), &error));
        QVERIFY(part.flush());

        QCOMPARE(part.size(), qint64(9));
        QVERIFY2(part.promote(&error), qPrintable(error));
        QCOMPARE(readAll(target), QByteArrayLiteral("abcdefghi"));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(target)));
    }

    void preallocationSetsTheSizeUpFront()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        part.preallocate(4096);

        // This is exactly why a size check alone cannot prove a download is complete.
        QCOMPARE(part.size(), qint64(4096));
    }

    void truncateAllDropsEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        part.preallocate(1024);
        QVERIFY(part.writeAt(0, QByteArrayLiteral("stale tail"), &error));
        QVERIFY2(part.truncateAll(&error), qPrintable(error));
        QCOMPARE(part.size(), qint64(0));

        QVERIFY(part.writeAt(0, QByteArrayLiteral("fresh"), &error));
        QVERIFY2(part.promote(&error), qPrintable(error));
        QCOMPARE(readAll(target), QByteArrayLiteral("fresh"));
    }

    void promoteReplacesAnExistingTarget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");
        FS::write(target, QByteArrayLiteral("previous version"));

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        QVERIFY(part.writeAt(0, QByteArrayLiteral("new version"), &error));
        QVERIFY2(part.promote(&error), qPrintable(error));

        QCOMPARE(readAll(target), QByteArrayLiteral("new version"));
    }

    void discardRemovesThePartFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");

        Net::PartFile part(target);
        QString error;
        QVERIFY2(part.open(&error), qPrintable(error));
        QVERIFY(part.writeAt(0, QByteArrayLiteral("half a download"), &error));
        part.discard();

        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(target)));
        QVERIFY(!QFileInfo::exists(target));
    }

    void keepLeavesThePartFileForLater()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");
        const QString partPath = Net::PartFile::partPathFor(target);

        {
            Net::PartFile part(target);
            QString error;
            QVERIFY2(part.open(&error), qPrintable(error));
            QVERIFY(part.writeAt(0, QByteArrayLiteral("aborted halfway"), &error));
            part.keep();
        }

        // A user abort keeps the bytes; the destructor must not undo that.
        QVERIFY(QFileInfo::exists(partPath));
        QCOMPARE(readAll(partPath), QByteArrayLiteral("aborted halfway"));
        QVERIFY(!QFileInfo::exists(target));
    }

    void destructionDiscardsUnpromotedBytes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");
        const QString partPath = Net::PartFile::partPathFor(target);

        {
            Net::PartFile part(target);
            QString error;
            QVERIFY2(part.open(&error), qPrintable(error));
            QVERIFY(part.writeAt(0, QByteArrayLiteral("never finished"), &error));
        }

        QVERIFY(!QFileInfo::exists(partPath));
        QVERIFY(!QFileInfo::exists(target));
    }

    void partFileIsHiddenFromResourceScanners()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = FS::PathCombine(dir.path(), "pack.zip");
        const QString partPath = QFileInfo(Net::PartFile::partPathFor(target)).absoluteFilePath();

        QVERIFY(!APPLICATION->checkQSavePath(partPath));
        {
            Net::PartFile part(target);
            QString error;
            QVERIFY2(part.open(&error), qPrintable(error));
            QVERIFY2(APPLICATION->checkQSavePath(partPath), "the part file was visible to resource scanners while in use");
        }
        QVERIFY(!APPLICATION->checkQSavePath(partPath));
    }

    void refusesAnUnusablyLongTargetPath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(Net::PartFile::isUsableFor(FS::PathCombine(dir.path(), "pack.zip")));

        const QString absurd = FS::PathCombine(dir.path(), QString(240, QLatin1Char('n')) + QStringLiteral(".zip"));
        QVERIFY(!Net::PartFile::isUsableFor(absurd));

        // ...and opening one fails cleanly rather than half creating anything.
        Net::PartFile part(absurd);
        QString error;
        QVERIFY(!part.open(&error));
        QVERIFY(!error.isEmpty());
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dataDirectory;
    if (!dataDirectory.isValid()) {
        return 1;
    }

    QByteArray applicationName(argv[0]);
    QByteArray directoryOption("--dir");
    QByteArray directoryPath = dataDirectory.path().toUtf8();
    char* applicationArguments[] = {
        applicationName.data(), directoryOption.data(), directoryPath.data(), nullptr,
    };
    int applicationArgumentCount = 3;
    Application application(applicationArgumentCount, applicationArguments);
    PartFileTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PartFile_test.moc"
