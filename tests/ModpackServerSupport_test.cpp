// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <utility>

#include "modplatform/ServerSupport.h"
#include "modplatform/ServerSupportRequestQueue.h"
#include "modplatform/ftb/FTBPackInstallTask.h"
#include "ui/pages/modplatform/technic/TechnicModel.h"

// Last, because its macros clash with names in the launcher headers.
#ifdef Q_OS_WIN
#include <windows.h>
#endif

class ModpackServerSupportTest final : public QObject {
    Q_OBJECT
   private slots:
    void curseForgeLatestFiles()
    {
        auto parse = [](QByteArray json) { return QJsonDocument::fromJson(json).object(); };
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{"serverPackFileId":42}]})")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{"isServerPack":true}]})")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{}]})")),
                 ModPlatform::ServerSupport::ClientDerived);
    }

    void technicServerUrls()
    {
        QCOMPARE(ModPlatform::technicServerSupport(QUrl("https://servers.technicpack.net/Technic/servers/tekkitmain/Tekkit_Server_v1.2.9g-2.zip")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::technicServerSupport(QUrl("https://www.gtnewhorizons.com/downloads/")),
                 ModPlatform::ServerSupport::Website);
        QCOMPARE(ModPlatform::technicServerSupport({}), ModPlatform::ServerSupport::ClientDerived);
    }

    void ftbProbeStatuses()
    {
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(200, false), ModPlatform::ServerSupport::Official);
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(404, true), ModPlatform::ServerSupport::ClientDerived);
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(0, true), ModPlatform::ServerSupport::Unknown);
    }

    void ftbInstallerMustBeSignedByFtb()
    {
#ifdef Q_OS_WIN
        // The Qt library this test runs on carries a real signature from another publisher.
        HMODULE qtCore = nullptr;
        QVERIFY(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&qVersion), &qtCore));
        wchar_t modulePath[MAX_PATH];
        const DWORD length = GetModuleFileNameW(qtCore, modulePath, MAX_PATH);
        QVERIFY(length > 0 && length < MAX_PATH);
        const QString signedByQt = QString::fromWCharArray(modulePath, static_cast<int>(length));

        QString error;
        const bool trusted = FTB::verifyTrustedWindowsExecutable(signedByQt, QStringLiteral("The QT Company Oy"), &error);
        if (!trusted && error.contains(QStringLiteral("signature verification"))) {
            QSKIP(qPrintable(QStringLiteral("This Qt build carries no trusted signature: %1").arg(error)));
        }
        QVERIFY2(trusted, qPrintable(error));

        error.clear();
        QVERIFY(!FTB::verifyTrustedWindowsExecutable(signedByQt, QString::fromLatin1(FTB::ServerInstallerSigner), &error));
        QVERIFY2(error.contains(QStringLiteral("signed by \"The QT Company Oy\" instead of \"Feed The Beast Ltd\"")),
                 qPrintable(error));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString unsignedFile = directory.filePath(QStringLiteral("unsigned.exe"));
        QFile file(unsignedFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("not a signed program") > 0);
        file.close();
        error.clear();
        QVERIFY(!FTB::verifyTrustedWindowsExecutable(unsignedFile, QString::fromLatin1(FTB::ServerInstallerSigner), &error));
        QVERIFY2(error.contains(QStringLiteral("signature verification")), qPrintable(error));
#else
        QSKIP("Installer signatures are checked on Windows only.");
#endif
    }

    void providerQueueLimits()
    {
        QObject owner;
        auto checkLimit = [&owner](int limit, int count) {
            using Queue = ModPlatform::ServerSupportRequestQueue<int>;
            Queue queue(limit);
            QList<Queue::Completion> completions;
            int peak = 0;
            for (int i = 0; i < count; ++i) {
                queue.request(QString::number(i), &owner,
                    [&queue, &completions, &peak](Queue::Completion complete) {
                        peak = qMax(peak, queue.activeCount());
                        completions.append(std::move(complete));
                        return Queue::Cancel{};
                    }, [](Queue::Result) {});
            }
            while (!completions.isEmpty()) completions.takeFirst()(Queue::Result{ 0 });
            QCOMPARE(peak, limit);
            QCOMPARE(queue.activeCount(), 0);
        };
        QCOMPARE(FTB::ServerPackProbeConcurrency, 1);
        QCOMPARE(Technic::PackDetailsConcurrency, 4);
        checkLimit(FTB::ServerPackProbeConcurrency, 4);
        checkLimit(Technic::PackDetailsConcurrency, 7);
    }

    void providerQueueCachesCompletedKeys()
    {
        using Queue = ModPlatform::ServerSupportRequestQueue<ModPlatform::ServerSupport>;
        Queue queue(1);
        QObject owner;
        int starts = 0;
        ModPlatform::ServerSupport result = ModPlatform::ServerSupport::Unknown;
        Queue::Completion complete;
        auto starter = [&starts, &complete](Queue::Completion done) {
            ++starts;
            complete = std::move(done);
            return Queue::Cancel{};
        };
        queue.request("same", &owner, starter, [&result](Queue::Result value) { result = *value; });
        complete(ModPlatform::ServerSupport::Official);
        QCOMPARE(starts, 1);
        queue.request("same", &owner, starter, [&result](Queue::Result value) { result = *value; });
        QCoreApplication::processEvents();
        QCOMPARE(starts, 1);
        QCOMPARE(result, ModPlatform::ServerSupport::Official);
    }

    void providerQueueResetDropsPendingOwner()
    {
        using Queue = ModPlatform::ServerSupportRequestQueue<int>;
        Queue queue(1);
        QObject activeOwner;
        QObject pendingOwner;
        QList<Queue::Completion> completions;
        int starts = 0;
        auto starter = [&completions, &starts](Queue::Completion done) {
            ++starts;
            completions.append(std::move(done));
            return Queue::Cancel{};
        };
        queue.request("active", &activeOwner, starter, [](Queue::Result) {});
        queue.request("pending", &pendingOwner, starter, [](Queue::Result) {});
        queue.cancelOwner(&pendingOwner);
        QCOMPARE(starts, 1);
        QCOMPARE(queue.pendingCount(), 0);
        completions.takeFirst()(Queue::Result{ 1 });
        QCOMPARE(starts, 1);
        QCOMPARE(queue.activeCount(), 0);
    }

    void providerQueueCancellingActiveOwnerReleasesItsSlot()
    {
        using Queue = ModPlatform::ServerSupportRequestQueue<int>;
        Queue queue(1);
        QObject firstOwner;
        QObject secondOwner;
        QList<Queue::Completion> completions;
        int starts = 0;
        int cancels = 0;
        auto starter = [&completions, &starts, &cancels](Queue::Completion done) {
            ++starts;
            completions.append(std::move(done));
            return Queue::Cancel{ [&cancels] { ++cancels; } };
        };

        queue.request("first", &firstOwner, starter, [](Queue::Result) {});
        QCOMPARE(queue.activeCount(), 1);
        queue.cancelOwner(&firstOwner);
        QCOMPARE(cancels, 1);
        QCOMPARE(queue.activeCount(), 0);

        // The freed slot must be usable straight away, even with a concurrency of one.
        bool secondAnswered = false;
        queue.request("second", &secondOwner, starter, [&secondAnswered](Queue::Result) { secondAnswered = true; });
        QCOMPARE(starts, 2);
        QCOMPARE(queue.activeCount(), 1);

        // A late completion of the cancelled request must not release a second slot.
        completions.first()(Queue::Result{ 1 });
        QCOMPARE(queue.activeCount(), 1);
        QVERIFY(!secondAnswered);

        completions.last()(Queue::Result{ 2 });
        QVERIFY(secondAnswered);
        QCOMPARE(queue.activeCount(), 0);
    }
};

QTEST_GUILESS_MAIN(ModpackServerSupportTest)
#include "ModpackServerSupport_test.moc"
