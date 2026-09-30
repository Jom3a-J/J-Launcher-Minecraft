// SPDX-License-Identifier: GPL-3.0-only

// Pins down what the Server Manager page shows and saves, so the page can be split into
// smaller pieces and its data moved without changing what the user sees.

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <QTreeWidget>
#include <QUuid>
#include <QtTest>

#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "ui/pages/server/ServerListPage.h"

namespace {
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

template <typename T>
T* child(const QWidget& page, const char* name)
{
    return page.findChild<T*>(QString::fromLatin1(name));
}

/// Waits for the page's queued refreshes (tab switches and selection changes use singleShot).
void settle()
{
    QTest::qWait(20);
}

void openTab(ServerListPage& page, const char* tabObjectName)
{
    auto* tabs = child<QTabWidget>(page, "serverTabs");
    QWidget* tab = page.findChild<QWidget*>(QString::fromLatin1(tabObjectName));
    QVERIFY(tabs && tab);
    auto* navigation = child<QListWidget>(page, "serverNavigationList");
    QVERIFY(navigation);
    navigation->setCurrentRow(tabs->indexOf(tab));
    settle();
    QCOMPARE(tabs->currentWidget(), tab);
}

void openAutomationTab(ServerListPage& page)
{
    openTab(page, "maintenanceTabs");
    auto* maintenance = child<QTabWidget>(page, "maintenanceTabs");
    QWidget* automation = page.findChild<QWidget*>(QStringLiteral("automationHealthScrollArea"));
    QVERIFY(maintenance && automation);
    maintenance->setCurrentWidget(automation);
    settle();
}

void selectServer(ServerListPage& page, const QString& serverId)
{
    auto* list = child<QListWidget>(page, "serverList");
    QVERIFY(list);
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toString() == serverId) {
            list->setCurrentRow(row);
            settle();
            return;
        }
    }
    QFAIL("The server is not listed.");
}
}  // namespace

class ServerListPageTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        // Server data that the page reads from application settings must not leak between
        // tests or into a real launcher profile.
        m_previousOrganization = QCoreApplication::organizationName();
        m_previousApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherServerListPageTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("ServerListPage_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }

    void cleanup()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
        QCoreApplication::setOrganizationName(m_previousOrganization);
        QCoreApplication::setApplicationName(m_previousApplication);
    }

    void listsServersAndFollowsSelection()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        QVERIFY(manager.createServer(QStringLiteral("Alpha"), QStringLiteral("1.21.1")));
        QVERIFY(manager.createServer(QStringLiteral("Beta"), QStringLiteral("1.21.1")));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();

        auto* list = child<QListWidget>(page, "serverList");
        auto* title = child<QLabel>(page, "detailTitleLabel");
        QVERIFY(list && title);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentRow(), 0);
        for (int row : { 0, 1 }) {
            list->setCurrentRow(row);
            settle();
            const auto server = manager.getServer(list->item(row)->data(Qt::UserRole).toString());
            QVERIFY(server);
            QCOMPARE(title->text(), server->name());
        }
    }

    void searchFiltersServerCards()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        QVERIFY(manager.createServer(QStringLiteral("Alpha"), QStringLiteral("1.21.1")));
        QVERIFY(manager.createServer(QStringLiteral("Beta"), QStringLiteral("1.20.4"), QStringLiteral("fabric")));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        auto* list = child<QListWidget>(page, "serverList");
        auto* search = child<QLineEdit>(page, "serverSearchInput");
        QVERIFY(list && search);

        search->setText(QStringLiteral("beta"));
        QCOMPARE(list->count(), 1);
        search->setText(QStringLiteral("fabric"));
        QCOMPARE(list->count(), 1);
        search->clear();
        QCOMPARE(list->count(), 2);
    }

    void everyWorkspaceTabOpensForASelectedServer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Modded"), QStringLiteral("1.21.1"),
                                                 QStringLiteral("fabric"));
        QVERIFY(server);

        ServerListPage page;
        page.setServerManager(&manager);
        settle();

        auto* tabs = child<QTabWidget>(page, "serverTabs");
        auto* navigation = child<QListWidget>(page, "serverNavigationList");
        QVERIFY(tabs && navigation);
        QCOMPARE(navigation->count(), tabs->count());
        for (int row = 0; row < navigation->count(); ++row) {
            QVERIFY2(tabs->isTabEnabled(row), qPrintable(tabs->tabText(row)));
            navigation->setCurrentRow(row);
            settle();
            QCOMPARE(tabs->currentIndex(), row);
        }
        auto* maintenance = child<QTabWidget>(page, "maintenanceTabs");
        QVERIFY(maintenance);
        for (int index = 0; index < maintenance->count(); ++index) {
            maintenance->setCurrentIndex(index);
            settle();
        }
    }

    void automationSettingsSurviveReopeningThePage()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Scheduled"), QStringLiteral("1.21.1"));
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("world/level.dat"), "world"));

        {
            ServerListPage page;
            page.setServerManager(&manager);
            settle();
            openAutomationTab(page);
            child<QCheckBox>(page, "scheduleEnabledCheck")->setChecked(true);
            auto* action = child<QComboBox>(page, "scheduleActionCombo");
            action->setCurrentIndex(action->findData(QStringLiteral("backup")));
            child<QTimeEdit>(page, "scheduleTimeEdit")->setTime(QTime(4, 30));
            child<QSpinBox>(page, "backupRetentionSpin")->setValue(3);
            child<QSpinBox>(page, "cpuWarningSpin")->setValue(70);
            child<QSpinBox>(page, "ramWarningSpin")->setValue(75);
            child<QSpinBox>(page, "diskWarningSpin")->setValue(5);
            child<QPushButton>(page, "saveAutomationButton")->click();
            QVERIFY(child<QLabel>(page, "automationInfoLabel")->text().contains(QStringLiteral("saved")));
        }

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openAutomationTab(page);
        QVERIFY(child<QCheckBox>(page, "scheduleEnabledCheck")->isChecked());
        QCOMPARE(child<QComboBox>(page, "scheduleActionCombo")->currentData().toString(),
                 QStringLiteral("backup"));
        QCOMPARE(child<QTimeEdit>(page, "scheduleTimeEdit")->time(), QTime(4, 30));
        QCOMPARE(child<QSpinBox>(page, "backupRetentionSpin")->value(), 3);
        QCOMPARE(child<QSpinBox>(page, "cpuWarningSpin")->value(), 70);
        QCOMPARE(child<QSpinBox>(page, "ramWarningSpin")->value(), 75);
        QCOMPARE(child<QSpinBox>(page, "diskWarningSpin")->value(), 5);

        // The saved schedule is what the manager runs, and the page shows the result.
        manager.runDueAutomations(QDateTime(QDate::currentDate(), QTime(4, 30)));
        QCOMPARE(manager.listServerBackups(server->id()).size(), 1);
        auto* history = child<QListWidget>(page, "automationHistoryList");
        QVERIFY(history && history->count() == 1);
        QVERIFY(history->item(0)->text().contains(QStringLiteral("BACKUP")));
    }

    void playerActivityAndKnownPlayersAreShown()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Social"), QStringLiteral("1.21.1"));
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("usercache.json"),
                          R"([{"name":"Steve","uuid":"069a79f4-44e9-4726-a5be-fca90e38aaf5","expiresOn":"2030-01-01 00:00:00 +0000"}])"));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "playersTab");
        auto* players = child<QTreeWidget>(page, "playersTree");
        auto* info = child<QLabel>(page, "playersInfoLabel");
        QVERIFY(players && info);
        QCOMPARE(players->topLevelItemCount(), 1);
        QCOMPARE(players->topLevelItem(0)->text(0), QStringLiteral("Steve"));

        emit server->playerActivity(QStringLiteral("Steve"), true);
        QVERIFY2(info->text().contains(QStringLiteral("Steve joined")), qPrintable(info->text()));
    }

    void recordedCrashIsShownInDiagnostics()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto first = manager.createServer(QStringLiteral("First"), QStringLiteral("1.21.1"));
        const auto second = manager.createServer(QStringLiteral("Second"), QStringLiteral("1.21.1"));
        QVERIFY(first && second);

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        auto* list = child<QListWidget>(page, "serverList");
        const QString selectedId = list->currentItem()->data(Qt::UserRole).toString();
        const auto crashed = selectedId == first->id() ? second : first;

        // Crash the server that is not selected: the selected one would open a modal dialog.
        emit crashed->serverCrashed(QStringLiteral("Server stopped unexpectedly"),
                                    QStringLiteral("java.lang.OutOfMemoryError: Java heap space"));
        selectServer(page, crashed->id());
        openAutomationTab(page);
        auto* diagnostics = child<QLabel>(page, "diagnosticsLabel");
        QVERIFY(diagnostics);
        QVERIFY2(diagnostics->text().startsWith(QStringLiteral("Latest crash:")), qPrintable(diagnostics->text()));
        QVERIFY(diagnostics->text().contains(QStringLiteral("Server stopped unexpectedly")));
        QVERIFY(child<QPushButton>(page, "viewCrashReportButton")->isEnabled());
    }

    void earlierRecordedServerDataIsShownAfterReload()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QString serverId;
        QString rollbackBackup;
        {
            ServerManager first(root.path());
            const auto server = first.createServer(QStringLiteral("Recorded"), QStringLiteral("1.21.1"),
                                                   QStringLiteral("fabric"));
            QVERIFY(server);
            serverId = server->id();
            rollbackBackup = QDir(server->serverDirectory()).filePath("backups/server-before-update");
            QVERIFY(QDir().mkpath(rollbackBackup));
            QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("mods/tracked.jar"), "jar"));
            QVERIFY(first.save());
        }

        // Data recorded by earlier launcher versions, under their settings keys.
        QSettings settings;
        const auto key = [&serverId](const QString& group, const QString& name) {
            return QStringLiteral("%1/%2/%3").arg(group, serverId, name);
        };
        settings.setValue(key("ServerAutomation", "enabled"), true);
        settings.setValue(key("ServerAutomation", "action"), QStringLiteral("restart"));
        settings.setValue(key("ServerAutomation", "time"), QStringLiteral("05:15"));
        settings.setValue(key("ServerAutomation", "retention"), 4);
        settings.setValue(key("ServerAutomation", "history"),
                          QStringList{ QStringLiteral("2026-09-01 10:00:00 — BACKUP: Created backup Nightly.") });
        settings.setValue(key("ServerMonitoring", "cpuWarning"), 66);
        settings.setValue(key("ServerMonitoring", "ramWarning"), 77);
        settings.setValue(key("ServerMonitoring", "diskWarningGb"), 9);
        settings.setValue(key("ServerDiagnostics", "lastCrash"), QStringLiteral("Out of memory on Tuesday"));
        settings.setValue(key("ServerDiagnostics", "details"), QStringLiteral("Full crash details"));
        settings.setValue(key("ServerPlayerHistory", "events"),
                          QStringList{ QStringLiteral("2026-09-01 10:00 — Alex joined") });
        settings.setValue(key("ServerContentSources", "tracked.jar"), QStringLiteral("curseforge:123:456"));
        settings.setValue(key("ServerUpdates", "latestRollbackBackupPath"), rollbackBackup);
        settings.sync();

        ServerManager manager(root.path());
        QVERIFY(manager.load());
        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        selectServer(page, serverId);

        openAutomationTab(page);
        QVERIFY(child<QCheckBox>(page, "scheduleEnabledCheck")->isChecked());
        QCOMPARE(child<QComboBox>(page, "scheduleActionCombo")->currentData().toString(),
                 QStringLiteral("restart"));
        QCOMPARE(child<QTimeEdit>(page, "scheduleTimeEdit")->time(), QTime(5, 15));
        QCOMPARE(child<QSpinBox>(page, "backupRetentionSpin")->value(), 4);
        QCOMPARE(child<QSpinBox>(page, "cpuWarningSpin")->value(), 66);
        QCOMPARE(child<QSpinBox>(page, "ramWarningSpin")->value(), 77);
        QCOMPARE(child<QSpinBox>(page, "diskWarningSpin")->value(), 9);
        auto* history = child<QListWidget>(page, "automationHistoryList");
        QVERIFY(history && history->count() == 1);
        QVERIFY(history->item(0)->text().contains(QStringLiteral("Nightly")));
        QCOMPARE(child<QLabel>(page, "diagnosticsLabel")->text(),
                 QStringLiteral("Latest crash: Out of memory on Tuesday"));

        openTab(page, "playersTab");
        QVERIFY(child<QLabel>(page, "playersInfoLabel")->text().contains(QStringLiteral("Alex joined")));

        openTab(page, "maintenanceTabs");
        child<QTabWidget>(page, "maintenanceTabs")->setCurrentWidget(page.findChild<QWidget*>(QStringLiteral("updatesTab")));
        settle();
        QVERIFY(child<QPushButton>(page, "restoreLatestUpdateBackupButton")->isEnabled());
        child<QPushButton>(page, "checkContentUpdatesButton")->click();
        auto* updates = child<QTreeWidget>(page, "contentUpdatesTree");
        QVERIFY(updates);
        QCOMPARE(updates->topLevelItemCount(), 1);
        QCOMPARE(updates->topLevelItem(0)->text(0), QStringLiteral("tracked.jar"));
        QCOMPARE(updates->topLevelItem(0)->text(1), QStringLiteral("CurseForge"));
        // Without a CurseForge key the page must say so instead of sending a request.
        QVERIFY(updates->topLevelItem(0)->text(2).contains(QStringLiteral("API key")));
    }

private:
    QString m_previousOrganization;
    QString m_previousApplication;
};

QTEST_MAIN(ServerListPageTest)

#include "ServerListPage_test.moc"
