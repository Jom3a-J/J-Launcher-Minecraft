// SPDX-License-Identifier: GPL-3.0-only

// Pins down what the Server Manager page shows and saves, so the page can be split into
// smaller pieces and its data moved without changing what the user sees.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTimer>
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
#include "ui/pages/server/ServerManagerWindow.h"
#include "ui/pages/server/ServerStatusIndicator.h"

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

/*! Answers the page's message boxes while a test runs: "Yes" to questions, otherwise the
 *  default button. Keeps the text of each box it closed.
 */
class MessageBoxAnswerer : public QObject {
public:
    MessageBoxAnswerer()
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            answered.append(box->text());
            if (QAbstractButton* yes = box->button(QMessageBox::Yes)) {
                yes->click();
            } else if (QAbstractButton* button = box->defaultButton()) {
                button->click();
            } else {
                box->accept();
            }
        });
        m_timer.start();
    }

    QStringList answered;

private:
    QTimer m_timer;
};

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

    void statusIndicatorSummarisesActiveServers()
    {
        QCOMPARE(ServerStatusIndicator::summaryFor(0, 0).text, QString());
        QCOMPARE(ServerStatusIndicator::summaryFor(1, 1).text, QStringLiteral("1 server running"));
        QCOMPARE(ServerStatusIndicator::summaryFor(3, 3).text, QStringLiteral("3 servers running"));
        QVERIFY(ServerStatusIndicator::summaryFor(3, 3).allRunning);
        QCOMPARE(ServerStatusIndicator::summaryFor(0, 1).text, QStringLiteral("1 server process active"));
        QCOMPARE(ServerStatusIndicator::summaryFor(1, 2).text, QStringLiteral("2 server processes active"));
        QVERIFY(!ServerStatusIndicator::summaryFor(1, 2).allRunning);
        QVERIFY(ServerStatusIndicator::summaryFor(1, 2).detail.contains("starting, stopping, or downloading"));

        // With only stopped servers the button hides and the action keeps its plain tooltip.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        QVERIFY(manager.createServer(QStringLiteral("Stopped"), QStringLiteral("1.21.1")));
        QWidget window;
        QAction manageServers(&window);
        ServerStatusIndicator indicator(&manager, &manageServers, &window);
        window.show();
        settle();
        QVERIFY(!indicator.isVisible());
        QCOMPARE(indicator.objectName(), QStringLiteral("serverStatusButton"));
        QCOMPARE(manageServers.toolTip(), QStringLiteral("Create and manage local Minecraft servers."));
    }

    void serverManagerWindowOpensOnceAndReusesItself()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        QVERIFY(manager.createServer(QStringLiteral("Alpha"), QStringLiteral("1.21.1")));
        QWidget parent;
        QPointer<QDialog> window;

        ServerManagerWindow::show(nullptr, &parent, window);
        QVERIFY(!window);

        ServerManagerWindow::show(&manager, &parent, window);
        settle();
        QVERIFY(window);
        QCOMPARE(window->objectName(), QStringLiteral("serverManagerWindow"));
        QVERIFY(window->findChild<ServerListPage*>());
        auto* list = child<QListWidget>(*window->findChild<ServerListPage*>(), "serverList");
        QVERIFY(list);
        QCOMPARE(list->count(), 1);

        QDialog* first = window.data();
        ServerManagerWindow::show(&manager, &parent, window);
        settle();
        QCOMPARE(window.data(), first);
        QCOMPARE(parent.findChildren<QDialog*>(QStringLiteral("serverManagerWindow")).size(), 1);

        // Nothing is running, so quitting needs no question.
        QVERIFY(ServerManagerWindow::confirmQuit(&manager, &parent));
        QVERIFY(ServerManagerWindow::confirmQuit(nullptr, &parent));
        window->close();
        settle();
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

    void backupsTabCreatesListsAndRemovesBackups()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Backed up"), QStringLiteral("1.21.1"));
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("world/level.dat"), "world"));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "backupsTab");
        auto* tree = child<QTreeWidget>(page, "backupsTree");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 0);

        MessageBoxAnswerer answerer;
        child<QLineEdit>(page, "backupNameInput")->setText(QStringLiteral("Before test"));
        child<QPushButton>(page, "createBackupButton")->click();
        QCOMPARE(tree->topLevelItemCount(), 1);
        QVERIFY(tree->topLevelItem(0)->text(0).contains(QStringLiteral("Before test")));
        QVERIFY(child<QLineEdit>(page, "backupNameInput")->text().isEmpty());
        QCOMPARE(manager.listServerBackups(server->id()).size(), 1);

        tree->setCurrentItem(tree->topLevelItem(0));
        QVERIFY(child<QPushButton>(page, "restoreBackupButton")->isEnabled());
        child<QPushButton>(page, "removeBackupButton")->click();
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(manager.listServerBackups(server->id()).isEmpty());
    }

    void filesTabBrowsesTheServerFolder()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Browsed"), QStringLiteral("1.21.1"));
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("server.properties"), "motd=hi\n"));
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("world/level.dat"), "world"));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "filesTab");
        auto* tree = child<QTreeWidget>(page, "serverFilesTree");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        QTreeWidgetItem* rootItem = tree->topLevelItem(0);
        QCOMPARE(rootItem->text(0), QStringLiteral("Browsed"));
        QStringList names;
        for (int index = 0; index < rootItem->childCount(); ++index) names << rootItem->child(index)->text(0);
        QVERIFY2(names.contains(QStringLiteral("world")), qPrintable(names.join(", ")));
        QVERIFY(names.contains(QStringLiteral("server.properties")));
        // Folders list first.
        QCOMPARE(names.first(), QStringLiteral("world"));
    }

    void modsTabListsAndTogglesInstalledContent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Modded"), QStringLiteral("1.21.1"),
                                                 QStringLiteral("fabric"));
        QVERIFY(server);
        const QDir mods(server->modsDirectory());
        QVERIFY(writeFile(mods.filePath("sodium-fabric-0.5.8.jar"), "jar"));
        QVERIFY(writeFile(mods.filePath("Jade_1.21-15.1.jar.disabled"), "jar"));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "installedContentTab");
        auto* tree = child<QTreeWidget>(page, "installedContentTree");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        QTreeWidgetItem* jade = tree->topLevelItem(0);
        QCOMPARE(jade->text(0), QStringLiteral("Jade"));
        QCOMPARE(jade->text(1), QStringLiteral("1.21-15.1"));
        QCOMPARE(jade->text(2), QStringLiteral("Disabled"));
        QTreeWidgetItem* sodium = tree->topLevelItem(1);
        QCOMPARE(sodium->text(0), QStringLiteral("sodium fabric"));
        QCOMPARE(sodium->text(1), QStringLiteral("0.5.8"));
        QCOMPARE(sodium->text(2), QStringLiteral("Enabled"));

        tree->setCurrentItem(jade);
        child<QPushButton>(page, "toggleInstalledButton")->click();
        QVERIFY(QFileInfo::exists(mods.filePath("Jade_1.21-15.1.jar")));
        QCOMPARE(tree->topLevelItem(0)->text(2), QStringLiteral("Enabled"));
    }

    void consoleShowsServerOutputAndCopiesErrors()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Chatty"), QStringLiteral("1.21.1"));
        QVERIFY(server);

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "consoleTab");
        auto* console = child<QPlainTextEdit>(page, "consoleOutput");
        QVERIFY(console);
        emit server->outputReceived(QStringLiteral("Done (3.2s)! For help, type \"help\""));
        emit server->errorReceived(QStringLiteral("Something broke"));
        QVERIFY(console->toPlainText().contains(QStringLiteral("Done (3.2s)!")));
        QVERIFY(console->toPlainText().contains(QStringLiteral("[ERROR] Something broke")));

        child<QPushButton>(page, "copyConsoleErrorsButton")->click();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("[ERROR] Something broke"));
    }

    void homeTabSummarizesTheServer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerManager manager(root.path());
        const auto server = manager.createServer(QStringLiteral("Summary"), QStringLiteral("1.20.4"),
                                                 QStringLiteral("paper"));
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->pluginsDirectory()).filePath("EssentialsX-2.20.1.jar"), "jar"));

        ServerListPage page;
        page.setServerManager(&manager);
        settle();
        openTab(page, "overviewTab");
        QCOMPARE(child<QLabel>(page, "overviewStatusValue")->text(), QStringLiteral("Stopped"));
        auto* summary = child<QLabel>(page, "overviewSummaryLabel");
        QVERIFY(summary);
        QVERIFY2(summary->text().contains(QStringLiteral("1.20.4")), qPrintable(summary->text()));
        QVERIFY(summary->text().contains(QStringLiteral("1 plugins")));
    }

private:
    QString m_previousOrganization;
    QString m_previousApplication;
};

QTEST_MAIN(ServerListPageTest)

#include "ServerListPage_test.moc"
