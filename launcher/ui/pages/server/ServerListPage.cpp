/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ServerListPage.h"
#include "ui_ServerListPage.h"
#include "archive/ArchiveReader.h"
#include "Application.h"
#include "server/ServerManager.h"
#include "server/ServerInstance.h"
#include "server/ServerDownloader.h"
#include "server/ServerDiagnostics.h"
#include "server/ServerContentUpdater.h"
#include "server/ServerModpackInstaller.h"
#include "server/ServerPlayerAccess.h"
#include "ui/dialogs/CreateServerDialog.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/dialogs/ResourceDownloadDialog.h"
#include "ui/pages/server/ServerSettingsPage.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "modplatform/flame/FlameAPI.h"
#include "BuildConfig.h"
#include "InstanceList.h"
#include "QObjectPtr.h"
#include "settings/INISettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "logs/Privacy.h"
#include <QMessageBox>
#include <QAbstractButton>
#include <QFileDialog>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QColor>
#include <QStyle>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QLocale>
#include <QMenu>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QClipboard>
#include <QApplication>
#include <QInputDialog>
#include <QSpinBox>
#include <QStorageInfo>
#include <QEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimeEdit>
#include <QThread>
#include <QTemporaryDir>
#include <QProgressBar>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>

namespace {
struct ProcessSnapshot {
    quint64 cpuMilliseconds = 0;
    qint64 workingSetBytes = 0;
};

quint64 fileTimeMilliseconds(const FILETIME &fileTime)
{
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart / 10000;
}

bool readProcessSnapshot(qint64 processId, ProcessSnapshot *snapshot)
{
    if (!snapshot || processId <= 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(processId));
    if (!process) return false;

    FILETIME creation, exitTime, kernel, user;
    const bool haveTimes = GetProcessTimes(process, &creation, &exitTime, &kernel, &user);
    using GetProcessMemoryInfoFunction = BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static GetProcessMemoryInfoFunction getProcessMemoryInfo = nullptr;
    static bool memoryFunctionLoaded = false;
    if (!memoryFunctionLoaded) {
        HMODULE psapi = GetModuleHandleW(L"psapi.dll");
        if (!psapi) psapi = LoadLibraryW(L"psapi.dll");
        if (psapi) getProcessMemoryInfo = reinterpret_cast<GetProcessMemoryInfoFunction>(GetProcAddress(psapi, "GetProcessMemoryInfo"));
        memoryFunctionLoaded = true;
    }

    PROCESS_MEMORY_COUNTERS_EX memoryCounters = {};
    memoryCounters.cb = sizeof(memoryCounters);
    const bool haveMemory = getProcessMemoryInfo
        && getProcessMemoryInfo(process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memoryCounters), sizeof(memoryCounters));
    CloseHandle(process);
    if (!haveTimes || !haveMemory) return false;

    snapshot->cpuMilliseconds = fileTimeMilliseconds(kernel) + fileTimeMilliseconds(user);
    snapshot->workingSetBytes = static_cast<qint64>(memoryCounters.WorkingSetSize);
    return true;
}
}
#endif

namespace {
QString formatByteSize(qint64 bytes)
{
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1024ll * 1024 * 1024) return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
}

QNetworkRequest updateRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "JLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

QNetworkRequest curseForgeUpdateRequest(const QUrl &url)
{
    QNetworkRequest request = updateRequest(url);
    request.setRawHeader("x-api-key", APPLICATION->getFlameAPIKey().toUtf8());
    return request;
}

void showContentUpdate(QTreeWidgetItem *item,
                       const ServerContentUpdateCandidate &update)
{
    if (!item) return;
    if (!update.available && !update.upToDate) {
        item->setText(2, QObject::tr("No compatible update"));
    } else if (update.upToDate) {
        item->setText(2, QObject::tr("Up to date"));
    } else {
        item->setText(2, QObject::tr("Update available: %1").arg(update.versionNumber));
        item->setData(0, Qt::UserRole, update.url.toString());
        item->setData(0, Qt::UserRole + 3, update.fileName);
        item->setData(0, Qt::UserRole + 4, static_cast<int>(update.hashAlgorithm));
        item->setData(0, Qt::UserRole + 5, update.expectedHash);
        item->setData(0, Qt::UserRole + 6, update.versionId);
    }
}

QStringList installedContentDetails(const QFileInfo &file)
{
    QString filename = file.fileName();
    const bool disabled = filename.endsWith(".disabled", Qt::CaseInsensitive);
    if (disabled) filename.chop(QString(".disabled").size());
    if (filename.endsWith(".jar", Qt::CaseInsensitive)) filename.chop(QString(".jar").size());

    int versionStart = -1;
    for (int index = 1; index < filename.size(); ++index) {
        if ((filename.at(index - 1) == '-' || filename.at(index - 1) == '_') && filename.at(index).isDigit()) {
            versionStart = index;
            break;
        }
    }

    QString name = versionStart > 0 ? filename.left(versionStart - 1) : filename;
    const QString version = versionStart > 0 ? filename.mid(versionStart) : QObject::tr("—");
    name.replace('-', ' ');
    name.replace('_', ' ');
    return { name, version, disabled ? QObject::tr("Disabled") : QObject::tr("Enabled") };
}

QIcon launcherIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    const QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull()) {
        return icon;
    }

    // Keep this surface in the launcher's icon family even when a custom or
    // incomplete theme does not provide an icon. The blue pack is the product
    // default; platform icons are only the final safety fallback.
    const QIcon defaultIcon(
        QStringLiteral(":/icons/pe_blue/scalable/%1.svg").arg(name));
    return defaultIcon.isNull()
        ? QApplication::style()->standardIcon(fallback)
        : defaultIcon;
}

QIcon serverCardIcon()
{
    return launcherIcon("server", QStyle::SP_ComputerIcon);
}

void applyMutedLabelPalette(QLabel *label)
{
    if (!label) return;
    QPalette labelPalette = label->palette();
    const QColor foreground = labelPalette.color(QPalette::WindowText);
    const QColor background = label->parentWidget()
        ? label->parentWidget()->palette().color(QPalette::Window)
        : labelPalette.color(QPalette::Window);
    const QColor muted(
        (foreground.red() * 2 + background.red()) / 3,
        (foreground.green() * 2 + background.green()) / 3,
        (foreground.blue() * 2 + background.blue()) / 3);
    labelPalette.setColor(QPalette::WindowText, muted);
    labelPalette.setColor(QPalette::Text, muted);
    label->setPalette(labelPalette);
}

QIcon serverFileIcon(const QFileInfo &file)
{
    if (file.isDir()) {
        const QString name = file.fileName().toLower();
        if (name == "world" || name == "world_nether" || name == "world_the_end") {
            return launcherIcon("worlds", QStyle::SP_DirIcon);
        }
        return launcherIcon("viewfolder", QStyle::SP_DirIcon);
    }

    const QString suffix = file.suffix().toLower();
    const QString name = file.fileName().toLower();
    if (suffix == "jar") {
        return launcherIcon(name.contains("server") || name.contains("paper") || name.contains("purpur")
                                ? "server" : "loadermods", QStyle::SP_FileIcon);
    }
    if (suffix == "zip" || suffix == "rar" || suffix == "7z" || suffix == "gz") {
        return launcherIcon("jarmods", QStyle::SP_FileIcon);
    }
    if (suffix == "properties" || suffix == "json" || suffix == "toml" || suffix == "yml" || suffix == "yaml") {
        return launcherIcon("settings", QStyle::SP_FileIcon);
    }
    if (suffix == "log") return launcherIcon("log", QStyle::SP_FileIcon);
    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg") return launcherIcon("screenshots", QStyle::SP_FileIcon);
    if (suffix == "java") return launcherIcon("java", QStyle::SP_FileIcon);
    return launcherIcon("notes", QStyle::SP_FileIcon);
}

QString structuredCrashDetails(const std::shared_ptr<ServerInstance> &server, const QString &message,
                               const QString &rawLog)
{
    if (!server) return Privacy::sanitizeText(rawLog, 8192);
    const QString loader = server->loaderVersion().isEmpty()
        ? server->loaderType()
        : server->loaderType() + " " + server->loaderVersion();
    const QString serverContentDirectory = server->contentDirectory();
    const QStringList content = serverContentDirectory.isEmpty()
        ? QStringList()
        : QDir(serverContentDirectory).entryList(
              QStringList() << "*.jar" << "*.jar.disabled",
              QDir::Files, QDir::Name | QDir::IgnoreCase);
    QStringList finalLines;
    const QStringList allLines = rawLog.split('\n', Qt::SkipEmptyParts);
    for (int index = qMax(0, allLines.size() - 25); index < allLines.size(); ++index) {
        finalLines << Privacy::sanitizeText(allLines.at(index), 8192);
    }

    QStringList report;
    const QString relevantLine = Privacy::sanitizeText(
        ServerDiagnostics::crashRelevantLine(rawLog), 1000);
    report << QObject::tr("Crash summary")
           << QObject::tr("Time: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
           << QObject::tr("Message: %1").arg(Privacy::sanitizeText(message))
           << QObject::tr("Likely cause: %1").arg(ServerDiagnostics::crashCauseExplanation(
                  ServerDiagnostics::classifyCrash(rawLog)))
           << QObject::tr("Reported error: %1").arg(relevantLine.isEmpty()
                                                        ? QObject::tr("No specific error line was found.")
                                                        : relevantLine)
           << QObject::tr("Minecraft: %1").arg(server->version())
           << QObject::tr("Server type: %1").arg(loader)
           << QObject::tr("Java: %1").arg(server->javaPath().isEmpty()
                                              ? QObject::tr("system default")
                                              : Privacy::sanitizePath(server->javaPath()))
           << QObject::tr("Memory: %1 MiB minimum / %2 MiB maximum").arg(server->minMemory()).arg(server->maxMemory())
           << QObject::tr("Installed content (%1): %2").arg(content.size()).arg(content.isEmpty() ? QObject::tr("none") : content.join(", "))
           << QString()
           << QObject::tr("Final server log lines:")
           << (finalLines.isEmpty() ? QObject::tr("No server output was captured.") : finalLines.join('\n'));
    return report.join('\n');
}

QString crashSummary(const QString& message, const QString& rawLog)
{
    const QString relevantLine = Privacy::sanitizeText(
        ServerDiagnostics::crashRelevantLine(rawLog), 1000);
    QString summary = QObject::tr("%1 — %2\nLikely cause: %3")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
             Privacy::sanitizeText(message),
             ServerDiagnostics::crashCauseExplanation(
                  ServerDiagnostics::classifyCrash(rawLog)));
    if (!relevantLine.isEmpty()) {
        summary += QObject::tr("\nServer reported: %1").arg(relevantLine);
    }
    return summary;
}

QStringList modIdsFromJar(const QString& path)
{
    QStringList identifiers;
    MMCZip::ArchiveReader fabricArchive(path);
    if (const auto metadata = fabricArchive.goToFile(QStringLiteral("fabric.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            metadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject object = document.object();
            identifiers << object.value(QStringLiteral("id")).toString().toLower();
            for (const QJsonValue& provided :
                 object.value(QStringLiteral("provides")).toArray()) {
                identifiers << provided.toString().toLower();
            }
        }
    }

    MMCZip::ArchiveReader quiltArchive(path);
    if (const auto metadata = quiltArchive.goToFile(QStringLiteral("quilt.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            metadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            identifiers << document.object()
                               .value(QStringLiteral("quilt_loader"))
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString()
                               .toLower();
        }
    }

    for (const QString& metadataPath : {
             QStringLiteral("META-INF/mods.toml"),
             QStringLiteral("META-INF/neoforge.mods.toml") }) {
        MMCZip::ArchiveReader forgeArchive(path);
        if (const auto metadata = forgeArchive.goToFile(metadataPath)) {
            const QString contents = QString::fromUtf8(metadata->readAll());
            static const QRegularExpression modIdExpression(
                QStringLiteral(R"((?im)^\s*modId\s*=\s*[\"']([a-z0-9_.-]+)[\"'])"));
            auto matches = modIdExpression.globalMatch(contents);
            while (matches.hasNext()) {
                identifiers << matches.next().captured(1).toLower();
            }
        }
    }
    identifiers.removeAll(QString());
    identifiers.removeDuplicates();
    return identifiers;
}

QStringList suspectedModFiles(const std::shared_ptr<ServerInstance>& server,
                              const QString& rawLog)
{
    if (!server) return {};
    const QStringList suspectedIds = ServerDiagnostics::suspectedModIds(rawLog);
    if (suspectedIds.isEmpty()) return {};

    QStringList matches;
    const QDir directory(server->modsDirectory());
    for (const QFileInfo& jar : directory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        const QStringList ids = modIdsFromJar(jar.absoluteFilePath());
        for (const QString& suspectedId : suspectedIds) {
            if (ids.contains(suspectedId, Qt::CaseInsensitive)) {
                matches << jar.absoluteFilePath();
                break;
            }
        }
    }
    return matches;
}

void showServerFailureDialog(QWidget *parent,
                             const std::shared_ptr<ServerInstance> &server,
                             const QString &message, const QString &rawLog)
{
    const QString likelyCause = ServerDiagnostics::crashCauseExplanation(
        ServerDiagnostics::classifyCrash(rawLog));
    const QString reportedError = Privacy::sanitizeText(
        ServerDiagnostics::crashRelevantLine(rawLog), 1000);

    QMessageBox dialog(parent);
    dialog.setWindowTitle(QObject::tr("Server Failure"));
    dialog.setIcon(QMessageBox::Critical);
    dialog.setText(QObject::tr("The server stopped before it was ready or ended unexpectedly."));

    QString explanation = QObject::tr("Likely cause: %1").arg(likelyCause);
    if (!reportedError.isEmpty()) {
        explanation += QObject::tr("\n\nServer reported:\n%1").arg(reportedError);
    }
    const QStringList suspectFiles = suspectedModFiles(server, rawLog);
    if (!suspectFiles.isEmpty()) {
        explanation += QObject::tr(
            "\n\nThe log identifies this installed mod as the likely cause: %1. "
            "You can disable it on this server and retry; the client copy is not changed.")
                           .arg(QFileInfo(suspectFiles.constFirst()).fileName());
    }
    explanation += QObject::tr("\n\nChoose Show Details to view the server version, Java runtime, installed content, and final log lines.");
    dialog.setInformativeText(explanation);
    dialog.setDetailedText(structuredCrashDetails(server, message, rawLog));
    dialog.setStandardButtons(QMessageBox::Close);
    QAbstractButton *disableAndRetryButton = nullptr;
    if (!suspectFiles.isEmpty()) {
        disableAndRetryButton = dialog.addButton(
            QObject::tr("Disable and Retry"), QMessageBox::ActionRole);
    }
    dialog.exec();

    if (disableAndRetryButton && dialog.clickedButton() == disableAndRetryButton) {
        QStringList failures;
        QStringList disabledNames;
        for (const QString& source : suspectFiles) {
            const QString destination = source + QStringLiteral(".disabled");
            if (QFileInfo::exists(destination) || !QFile::rename(source, destination)) {
                failures << QFileInfo(source).fileName();
            } else {
                disabledNames << QFileInfo(source).fileName();
            }
        }
        if (!failures.isEmpty()) {
            QMessageBox::warning(
                parent, QObject::tr("Could Not Disable Mod"),
                QObject::tr("These files could not be disabled: %1")
                    .arg(failures.join(QStringLiteral(", "))));
            return;
        }
        if (!disabledNames.isEmpty() && server) {
            QTimer::singleShot(0, server.get(), [server]() { server->start(); });
        }
    }
}
}

ServerListPage::ServerListPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ServerListPage)
{
    QElapsedTimer performanceTimer;
    performanceTimer.start();
    ui->setupUi(this);
    m_serverTrackingContext = new QObject(this);
    ui->serverListPanel->setMinimumWidth(264);
    ui->serverListPanel->setMaximumWidth(328);
    ui->serverSplitter->setSizes(QList<int>() << 292 << 888);
    ui->serverList->setSpacing(6);
    applyServerVisualHierarchy();

    // Do not present a creation action as preselected when this page opens.
    setFocusPolicy(Qt::StrongFocus);
    setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [this]() { setFocus(Qt::OtherFocusReason); });

    m_undoDeleteButton = new QPushButton(tr("Undo Delete"), this);
    m_undoDeleteButton->setObjectName(QStringLiteral("undoDeleteButton"));
    m_undoDeleteButton->setIcon(launcherIcon("refresh", QStyle::SP_ArrowBack));
    m_undoDeleteButton->setEnabled(false);
    m_undoDeleteButton->hide();
    ui->headerLayout->insertWidget(ui->headerLayout->count() - 2, m_undoDeleteButton);

    m_emptyServerListWidget = new QWidget(ui->serverListPanel);
    m_emptyServerListWidget->setObjectName("serverEmptyState");
    auto *emptyListLayout = new QVBoxLayout(m_emptyServerListWidget);
    emptyListLayout->addStretch();
    auto *emptyListIcon = new QLabel(m_emptyServerListWidget);
    emptyListIcon->setPixmap(serverCardIcon().pixmap(58, 58));
    emptyListIcon->setAlignment(Qt::AlignCenter);
    emptyListLayout->addWidget(emptyListIcon);
    auto *emptyListTitle = new QLabel(tr("No servers yet"), m_emptyServerListWidget);
    emptyListTitle->setObjectName(QStringLiteral("emptyServerListTitle"));
    QFont emptyTitleFont = emptyListTitle->font();
    emptyTitleFont.setBold(true);
    emptyListTitle->setFont(emptyTitleFont);
    emptyListTitle->setAlignment(Qt::AlignCenter);
    emptyListLayout->addWidget(emptyListTitle);
    auto *emptyListHint = new QLabel(tr("Create a server to manage it from the launcher."), m_emptyServerListWidget);
    emptyListHint->setObjectName(QStringLiteral("emptyServerListHint"));
    emptyListHint->setWordWrap(true);
    emptyListHint->setAlignment(Qt::AlignCenter);
    applyMutedLabelPalette(emptyListHint);
    emptyListLayout->addWidget(emptyListHint);
    auto *emptyListCreateButton = new QPushButton(tr("Create Server"), m_emptyServerListWidget);
    emptyListCreateButton->setObjectName(QStringLiteral("emptyCreateServerButton"));
    emptyListCreateButton->setProperty("role", "primary");
    emptyListCreateButton->setIcon(launcherIcon("new", QStyle::SP_FileDialogNewFolder));
    emptyListCreateButton->setIconSize(QSize(20, 20));
    emptyListCreateButton->setMinimumSize(132, 36);
    emptyListLayout->addWidget(emptyListCreateButton, 0, Qt::AlignCenter);
    emptyListLayout->addStretch();
    ui->serverListPanelLayout->addWidget(m_emptyServerListWidget, 1);
    connect(emptyListCreateButton, &QPushButton::clicked, this, &ServerListPage::onCreateServer);

    m_emptyDetailWidget = new QWidget(ui->serverDetailPanel);
    m_emptyDetailWidget->setObjectName("serverDetailEmptyState");
    auto *emptyDetailLayout = new QVBoxLayout(m_emptyDetailWidget);
    emptyDetailLayout->addStretch();
    auto *emptyDetailTitle = new QLabel(tr("Select a server"), m_emptyDetailWidget);
    emptyDetailTitle->setFont(emptyTitleFont);
    emptyDetailTitle->setAlignment(Qt::AlignCenter);
    emptyDetailLayout->addWidget(emptyDetailTitle);
    auto *emptyDetailHint =
        new QLabel(tr("Choose a server from the list to view status, console, files, backups, and settings."),
                   m_emptyDetailWidget);
    emptyDetailHint->setWordWrap(true);
    emptyDetailHint->setAlignment(Qt::AlignCenter);
    applyMutedLabelPalette(emptyDetailHint);
    emptyDetailLayout->addWidget(emptyDetailHint);
    emptyDetailLayout->addStretch();
    ui->serverDetailLayout->insertWidget(1, m_emptyDetailWidget, 1);
    ui->overviewInfoLabel->hide();
    ui->overviewDetailsGroup->hide();
    ui->overviewBackupsCard->hide();
    ui->overviewWorldsCard->hide();
    ui->overviewStorageCard->hide();
    ui->overviewMemoryCard->setTitle(tr("Uptime"));
    ui->overviewContentCard->setTitle(tr("Storage"));

    auto *summaryGroup = new QGroupBox(tr("Server Summary"), ui->overviewTab);
    summaryGroup->setObjectName("overviewSummaryGroup");
    auto *summaryLayout = new QVBoxLayout(summaryGroup);
    m_overviewSummaryLabel = new QLabel(tr("No server data available."), summaryGroup);
    m_overviewSummaryLabel->setObjectName("overviewSummaryLabel");
    m_overviewSummaryLabel->setWordWrap(true);
    m_overviewSummaryLabel->setTextFormat(Qt::RichText);
    m_overviewSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLayout->addWidget(m_overviewSummaryLabel);
    ui->overviewTabLayout->insertWidget(2, summaryGroup);

    auto *liveUsageGroup = new QGroupBox(tr("Live Resource Use"), ui->overviewTab);
    liveUsageGroup->setObjectName("overviewLiveUsageGroup");
    auto *liveUsageLayout = new QGridLayout(liveUsageGroup);
    liveUsageLayout->addWidget(new QLabel(tr("CPU"), liveUsageGroup), 0, 0);
    m_overviewCpuBar = new QProgressBar(liveUsageGroup);
    m_overviewCpuBar->setRange(0, 100);
    liveUsageLayout->addWidget(m_overviewCpuBar, 0, 1);
    liveUsageLayout->addWidget(new QLabel(tr("RAM"), liveUsageGroup), 1, 0);
    m_overviewRamBar = new QProgressBar(liveUsageGroup);
    liveUsageLayout->addWidget(m_overviewRamBar, 1, 1);
    m_overviewUpdatedLabel = new QLabel(tr("Waiting for live data."), liveUsageGroup);
    m_overviewUpdatedLabel->setAlignment(Qt::AlignRight);
    applyMutedLabelPalette(m_overviewUpdatedLabel);
    liveUsageLayout->addWidget(m_overviewUpdatedLabel, 2, 0, 1, 2);
    ui->overviewTabLayout->insertWidget(3, liveUsageGroup);
    m_liveStatisticsTimer.setInterval(2000);
    connect(&m_liveStatisticsTimer, &QTimer::timeout, this, [this]() {
        if (ui->serverTabs->currentWidget() == ui->overviewTab) refreshLiveStatistics();
    });
    m_liveStatisticsTimer.start();

    m_exportProfileButton = new QPushButton(tr("Export Profile"), this);
    m_importProfileButton = new QPushButton(tr("Import Profile"), this);
    ui->filesActions->insertWidget(2, m_exportProfileButton);
    ui->filesActions->insertWidget(3, m_importProfileButton);

    auto *consoleHeader = new QHBoxLayout();
    auto *consoleTitle = new QLabel(tr("Live output"), ui->consoleTab);
    consoleTitle->setObjectName("consoleTitleLabel");
    QFont consoleTitleFont = consoleTitle->font();
    consoleTitleFont.setBold(true);
    consoleTitle->setFont(consoleTitleFont);
    auto *consoleHint = new QLabel(tr("Server output and commands"), ui->consoleTab);
    consoleHint->setObjectName("consoleHintLabel");
    consoleHeader->addWidget(consoleTitle);
    consoleHeader->addWidget(consoleHint);
    consoleHeader->addStretch();
    ui->consoleTabLayout->insertLayout(0, consoleHeader);

    auto *consoleTools = new QHBoxLayout();
    m_consoleSearchInput = new QLineEdit(ui->consoleTab);
    m_consoleSearchInput->setObjectName("consoleSearchInput");
    m_consoleSearchInput->setPlaceholderText(tr("Search console output…"));
    m_findConsoleButton = new QPushButton(tr("Find Next"), ui->consoleTab);
    m_findConsoleButton->setObjectName("findConsoleButton");
    m_copyConsoleErrorsButton = new QPushButton(tr("Copy Errors"), ui->consoleTab);
    m_copyConsoleErrorsButton->setObjectName("copyConsoleErrorsButton");
    m_pauseConsoleScrollCheck = new QCheckBox(tr("Pause auto-scroll"), ui->consoleTab);
    m_clearConsoleButton = new QPushButton(tr("Clear View"), ui->consoleTab);
    m_exportConsoleButton = new QPushButton(tr("Export Log"), ui->consoleTab);
    consoleTools->addWidget(m_consoleSearchInput, 1);
    consoleTools->addWidget(m_findConsoleButton);
    consoleTools->addWidget(m_copyConsoleErrorsButton);
    consoleTools->addWidget(m_pauseConsoleScrollCheck);
    consoleTools->addWidget(m_clearConsoleButton);
    consoleTools->addWidget(m_exportConsoleButton);
    ui->consoleTabLayout->insertLayout(2, consoleTools);
    ui->consoleOutput->document()->setMaximumBlockCount(5000);

    ui->installedContentTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->installedContentTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->installedContentTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->installedContentTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    m_updatesNetwork = new QNetworkAccessManager(this);
    m_maintenanceTab = new QTabWidget(ui->serverTabs);
    m_maintenanceTab->setDocumentMode(true);
    m_updatesTab = new QWidget(m_maintenanceTab);
    auto *updatesLayout = new QVBoxLayout(m_updatesTab);
    m_updatesInfoLabel = new QLabel(tr("Keep the server stopped while applying updates."), m_updatesTab);
    m_updatesInfoLabel->setWordWrap(true);
    updatesLayout->addWidget(m_updatesInfoLabel);
    auto *softwareGroup = new QGroupBox(tr("Server Software"), m_updatesTab);
    auto *softwareLayout = new QGridLayout(softwareGroup);
    auto *softwareDescription = new QLabel(
        tr("Update the current Minecraft version's server build, or explicitly change the Minecraft version."),
        softwareGroup);
    softwareDescription->setWordWrap(true);
    softwareLayout->addWidget(softwareDescription, 0, 0, 1, 3);
    m_updateServerSoftwareButton = new QPushButton(tr("Update Current Build"), softwareGroup);
    m_changeMinecraftVersionButton = new QPushButton(tr("Change Minecraft Version"), softwareGroup);
    m_restoreLatestUpdateBackupButton = new QPushButton(tr("Restore Latest Update Backup"), softwareGroup);
    m_updateServerSoftwareButton->setObjectName(QStringLiteral("updateServerSoftwareButton"));
    m_changeMinecraftVersionButton->setObjectName(QStringLiteral("changeMinecraftVersionButton"));
    m_restoreLatestUpdateBackupButton->setObjectName(
        QStringLiteral("restoreLatestUpdateBackupButton"));
    m_restoreLatestUpdateBackupButton->setToolTip(tr("Restore the rollback backup made before the most recent server software update."));
    softwareLayout->addWidget(m_updateServerSoftwareButton, 1, 0);
    softwareLayout->addWidget(m_changeMinecraftVersionButton, 1, 1);
    softwareLayout->addWidget(m_restoreLatestUpdateBackupButton, 1, 2);
    updatesLayout->addWidget(softwareGroup);
    auto *contentGroup = new QGroupBox(tr("Mod & Plugin Updates"), m_updatesTab);
    auto *contentLayout = new QVBoxLayout(contentGroup);
    m_contentUpdatesTree = new QTreeWidget(contentGroup);
    m_contentUpdatesTree->setColumnCount(3);
    m_contentUpdatesTree->setHeaderLabels({tr("Installed file"), tr("Source"), tr("Update status")});
    m_contentUpdatesTree->setAlternatingRowColors(true);
    contentLayout->addWidget(m_contentUpdatesTree);
    auto *contentActions = new QHBoxLayout();
    m_checkContentUpdatesButton = new QPushButton(tr("Check for Updates"), contentGroup);
    m_setupCurseForgeButton = new QPushButton(tr("Set Up CurseForge"), contentGroup);
    m_installContentUpdateButton = new QPushButton(tr("Install Selected Update"), contentGroup);
    m_checkContentUpdatesButton->setObjectName(QStringLiteral("checkContentUpdatesButton"));
    m_setupCurseForgeButton->setObjectName(QStringLiteral("setupCurseForgeButton"));
    m_installContentUpdateButton->setObjectName(QStringLiteral("installContentUpdateButton"));
    contentActions->addWidget(m_checkContentUpdatesButton);
    contentActions->addWidget(m_setupCurseForgeButton);
    contentActions->addWidget(m_installContentUpdateButton);
    contentActions->addStretch();
    contentLayout->addLayout(contentActions);
    updatesLayout->addWidget(contentGroup, 1);
    m_maintenanceTab->addTab(m_updatesTab, tr("Updates"));

    m_playersTab = new QWidget(ui->serverTabs);
    auto *playersLayout = new QVBoxLayout(m_playersTab);
    m_playersInfoLabel = new QLabel(tr("Known players are read from usercache.json. Stop the server before changing whitelist, operator, or ban entries."), m_playersTab);
    m_playersInfoLabel->setObjectName(QStringLiteral("playersInfoLabel"));
    m_playersInfoLabel->setWordWrap(true);
    playersLayout->addWidget(m_playersInfoLabel);
    m_playersTree = new QTreeWidget(m_playersTab);
    m_playersTree->setObjectName(QStringLiteral("playersTree"));
    m_playersTree->setColumnCount(5);
    m_playersTree->setHeaderLabels({tr("Player"), tr("UUID"), tr("Whitelisted"), tr("Operator"), tr("Banned")});
    m_playersTree->setAlternatingRowColors(true);
    m_playersTree->setSelectionMode(QAbstractItemView::SingleSelection);
    playersLayout->addWidget(m_playersTree, 1);
    auto *playerActions = new QGridLayout();
    m_refreshPlayersButton = new QPushButton(tr("Refresh"), m_playersTab);
    m_whitelistPlayerButton = new QPushButton(tr("Whitelist"), m_playersTab);
    m_opPlayerButton = new QPushButton(tr("Make Operator"), m_playersTab);
    m_banPlayerButton = new QPushButton(tr("Ban"), m_playersTab);
    m_kickPlayerButton = new QPushButton(tr("Kick"), m_playersTab);
    m_removePlayerAccessButton = new QPushButton(tr("Remove Access"), m_playersTab);
    m_viewPlayerHistoryButton = new QPushButton(tr("Activity History"), m_playersTab);
    m_exportPlayerHistoryButton = new QPushButton(tr("Export History"), m_playersTab);
    m_refreshPlayersButton->setObjectName(QStringLiteral("refreshPlayersButton"));
    m_whitelistPlayerButton->setObjectName(QStringLiteral("whitelistPlayerButton"));
    m_opPlayerButton->setObjectName(QStringLiteral("opPlayerButton"));
    m_banPlayerButton->setObjectName(QStringLiteral("banPlayerButton"));
    m_kickPlayerButton->setObjectName(QStringLiteral("kickPlayerButton"));
    m_removePlayerAccessButton->setObjectName(QStringLiteral("removePlayerAccessButton"));
    playerActions->addWidget(m_refreshPlayersButton, 0, 0);
    playerActions->addWidget(m_whitelistPlayerButton, 0, 1);
    playerActions->addWidget(m_opPlayerButton, 0, 2);
    playerActions->addWidget(m_banPlayerButton, 0, 3);
    playerActions->addWidget(m_kickPlayerButton, 1, 0);
    playerActions->addWidget(m_removePlayerAccessButton, 1, 1);
    playerActions->addWidget(m_viewPlayerHistoryButton, 1, 2);
    playerActions->addWidget(m_exportPlayerHistoryButton, 1, 3);
    for (int column = 0; column < 4; ++column) {
        playerActions->setColumnStretch(column, 1);
    }
    playersLayout->addLayout(playerActions);
    ui->serverTabs->insertTab(ui->serverTabs->indexOf(ui->settingsTab), m_playersTab, tr("Players"));

    auto *automationScroll = new QScrollArea(m_maintenanceTab);
    automationScroll->setObjectName(QStringLiteral("automationHealthScrollArea"));
    automationScroll->setWidgetResizable(true);
    automationScroll->setFrameShape(QFrame::NoFrame);
    m_automationTab = automationScroll;
    auto *automationContent = new QWidget(automationScroll);
    automationScroll->setWidget(automationContent);
    auto *automationLayout = new QVBoxLayout(automationContent);
    automationLayout->setContentsMargins(10, 10, 10, 10);
    m_automationInfoLabel = new QLabel(tr("Schedule one daily maintenance action. A scheduled backup runs only while the server is stopped."), automationContent);
    m_automationInfoLabel->setWordWrap(true);
    automationLayout->addWidget(m_automationInfoLabel);
    auto *scheduleGroup = new QGroupBox(tr("Daily Schedule & Backup Retention"), automationContent);
    auto *scheduleForm = new QFormLayout(scheduleGroup);
    m_scheduleEnabledCheck = new QCheckBox(tr("Enable daily schedule"), scheduleGroup);
    m_scheduleActionCombo = new QComboBox(scheduleGroup);
    m_scheduleActionCombo->addItem(tr("Start"), "start");
    m_scheduleActionCombo->addItem(tr("Stop"), "stop");
    m_scheduleActionCombo->addItem(tr("Restart"), "restart");
    m_scheduleActionCombo->addItem(tr("Backup"), "backup");
    m_scheduleTimeEdit = new QTimeEdit(QTime::currentTime(), scheduleGroup);
    m_scheduleTimeEdit->setDisplayFormat("HH:mm");
    m_backupRetentionSpin = new QSpinBox(scheduleGroup);
    m_backupRetentionSpin->setRange(0, 100);
    m_backupRetentionSpin->setSpecialValueText(tr("Keep all backups"));
    m_backupRetentionSpin->setToolTip(tr("Automatic backups beyond this number are removed, oldest first. 0 keeps all."));
    m_gracefulStopTimeoutSpin = new QSpinBox(scheduleGroup);
    m_gracefulStopTimeoutSpin->setRange(5, 120);
    m_gracefulStopTimeoutSpin->setSuffix(tr(" seconds"));
    m_gracefulStopTimeoutSpin->setToolTip(tr("How long to wait after sending the stop command before terminating an unresponsive server."));
    m_autoRestartCheck = new QCheckBox(tr("Restart automatically after a crash (5-second delay)"), scheduleGroup);
    scheduleForm->addRow(m_scheduleEnabledCheck);
    scheduleForm->addRow(tr("Action:"), m_scheduleActionCombo);
    scheduleForm->addRow(tr("Time:"), m_scheduleTimeEdit);
    scheduleForm->addRow(tr("Keep automatic backups:"), m_backupRetentionSpin);
    scheduleForm->addRow(tr("Graceful stop timeout:"), m_gracefulStopTimeoutSpin);
    scheduleForm->addRow(m_autoRestartCheck);
    auto *scheduleActions = new QHBoxLayout();
    m_saveAutomationButton = new QPushButton(tr("Save Automation"), scheduleGroup);
    m_runAutomationButton = new QPushButton(tr("Run Selected Action Now"), scheduleGroup);
    scheduleActions->addWidget(m_saveAutomationButton);
    scheduleActions->addWidget(m_runAutomationButton);
    scheduleActions->addStretch();
    scheduleForm->addRow(scheduleActions);
    automationLayout->addWidget(scheduleGroup);
    auto *alertsGroup = new QGroupBox(tr("Monitoring Warning Thresholds"), automationContent);
    auto *alertsForm = new QFormLayout(alertsGroup);
    m_cpuWarningSpin = new QSpinBox(alertsGroup);
    m_cpuWarningSpin->setRange(50, 100);
    m_cpuWarningSpin->setSuffix("%");
    m_cpuWarningSpin->setToolTip(tr("CPU use at or above this value is shown as a warning."));
    m_ramWarningSpin = new QSpinBox(alertsGroup);
    m_ramWarningSpin->setRange(50, 100);
    m_ramWarningSpin->setSuffix("%");
    m_ramWarningSpin->setToolTip(tr("Configured RAM use at or above this value is shown as a warning."));
    m_diskWarningSpin = new QSpinBox(alertsGroup);
    m_diskWarningSpin->setRange(1, 1000);
    m_diskWarningSpin->setSuffix(tr(" GB free"));
    m_diskWarningSpin->setToolTip(tr("Free disk space at or below this value is shown as a warning."));
    alertsForm->addRow(tr("CPU warning:"), m_cpuWarningSpin);
    alertsForm->addRow(tr("RAM warning:"), m_ramWarningSpin);
    alertsForm->addRow(tr("Disk warning:"), m_diskWarningSpin);
    automationLayout->addWidget(alertsGroup);
    auto *historyGroup = new QGroupBox(tr("Automation Activity"), automationContent);
    auto *historyLayout = new QVBoxLayout(historyGroup);
    m_automationHistoryList = new QListWidget(historyGroup);
    m_automationHistoryList->setSelectionMode(QAbstractItemView::NoSelection);
    m_automationHistoryList->setAlternatingRowColors(true);
    m_automationHistoryList->setToolTip(tr("The 50 most recent scheduled or manually run maintenance actions for this server."));
    m_automationHistoryList->setMinimumHeight(140);
    historyLayout->addWidget(m_automationHistoryList);
    automationLayout->addWidget(historyGroup);
    auto *diagnosticsGroup = new QGroupBox(tr("Crash Diagnostics"), automationContent);
    auto *diagnosticsLayout = new QVBoxLayout(diagnosticsGroup);
    m_diagnosticsLabel = new QLabel(tr("No crash report recorded for this server."), diagnosticsGroup);
    m_diagnosticsLabel->setObjectName(QStringLiteral("diagnosticsLabel"));
    m_diagnosticsLabel->setWordWrap(true);
    m_viewCrashReportButton = new QPushButton(tr("View Latest Crash Report"), diagnosticsGroup);
    m_viewCrashReportButton->setObjectName(QStringLiteral("viewCrashReportButton"));
    diagnosticsLayout->addWidget(m_diagnosticsLabel);
    diagnosticsLayout->addWidget(m_viewCrashReportButton, 0, Qt::AlignLeft);
    automationLayout->addWidget(diagnosticsGroup);
    automationLayout->addStretch();
    m_maintenanceTab->addTab(m_automationTab, tr("Automation & Health"));
    setupServerNavigation();
    connect(ui->serverTabs, &QTabWidget::currentChanged, this, [this](int) {
        QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
    });
    connect(m_maintenanceTab, &QTabWidget::currentChanged, this, [this](int) {
        if (ui->serverTabs->currentWidget() == m_maintenanceTab) {
            QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
        }
    });
    m_automationTimer.setInterval(30000);
    connect(&m_automationTimer, &QTimer::timeout, this, [this]() {
        if (!m_serverManager) return;
        const QTime now = QTime::currentTime();
        const QString date = QDate::currentDate().toString(Qt::ISODate);
        QSettings settings;
        for (const auto &server : m_serverManager->getAllServers()) {
            const QString prefix = QString("ServerAutomation/%1/").arg(server->id());
            if (!settings.value(prefix + "enabled", false).toBool()) continue;
            const QTime scheduled = QTime::fromString(settings.value(prefix + "time").toString(), "HH:mm");
            if (!scheduled.isValid() || scheduled.hour() != now.hour() || scheduled.minute() != now.minute()) continue;
            if (settings.value(prefix + "lastRun").toString() == date) continue;
            settings.setValue(prefix + "lastRun", date);
            runAutomation(server, settings.value(prefix + "action", "start").toString(), settings.value(prefix + "retention", 0).toInt());
        }
    });
    m_automationTimer.start();

    connect(ui->createServerButton, &QPushButton::clicked, this, &ServerListPage::onCreateServer);
    connect(ui->createFromModpackButton, &QPushButton::clicked, this, &ServerListPage::onInstallModpack);
    connect(ui->startServerButton, &QPushButton::clicked, this, &ServerListPage::onStartServer);
    connect(ui->stopServerButton, &QPushButton::clicked, this, &ServerListPage::onStopServer);
    connect(ui->restartServerButton, &QPushButton::clicked, this, &ServerListPage::onRestartServer);
    connect(ui->deleteServerButton, &QPushButton::clicked, this, &ServerListPage::onDeleteServer);
    connect(m_undoDeleteButton, &QPushButton::clicked, this, &ServerListPage::onUndoDelete);
    connect(ui->openFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenFolder);
    connect(ui->browseModsButton, &QPushButton::clicked, this, &ServerListPage::onBrowseMods);
    connect(ui->addLocalContentButton, &QPushButton::clicked, this, &ServerListPage::onAddLocalContent);
    connect(ui->toggleInstalledButton, &QPushButton::clicked, this, &ServerListPage::onToggleInstalledContent);
    connect(ui->removeInstalledButton, &QPushButton::clicked, this, &ServerListPage::onRemoveInstalledContent);
    connect(ui->openContentFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenContentFolder);
    connect(ui->createBackupButton, &QPushButton::clicked, this, &ServerListPage::onCreateBackup);
    connect(ui->restoreBackupButton, &QPushButton::clicked, this, &ServerListPage::onRestoreBackup);
    connect(ui->openBackupsFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenBackupsFolder);
    connect(ui->removeBackupButton, &QPushButton::clicked, this, &ServerListPage::onRemoveBackup);
    connect(ui->refreshBackupsButton, &QPushButton::clicked, this, &ServerListPage::refreshServerBackups);
    connect(ui->backupsTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(ui->refreshInstalledButton, &QPushButton::clicked, this, &ServerListPage::refreshInstalledContent);
    connect(ui->refreshOverviewButton, &QPushButton::clicked, this, [this]() {
        refreshOverview();
        refreshLiveStatistics();
    });
    connect(ui->installedContentTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(ui->refreshFilesButton, &QPushButton::clicked, this, &ServerListPage::refreshServerFiles);
    connect(ui->importPackButton, &QPushButton::clicked, this, &ServerListPage::onImportServerPack);
    connect(ui->sendCommandButton, &QPushButton::clicked, this, &ServerListPage::onSendCommand);
    connect(m_exportProfileButton, &QPushButton::clicked, this, &ServerListPage::onExportProfile);
    connect(m_importProfileButton, &QPushButton::clicked, this, &ServerListPage::onImportProfile);
    connect(m_saveAutomationButton, &QPushButton::clicked, this, &ServerListPage::onSaveAutomation);
    connect(m_runAutomationButton, &QPushButton::clicked, this, &ServerListPage::onRunAutomationNow);
    connect(m_viewCrashReportButton, &QPushButton::clicked, this, &ServerListPage::onViewCrashReport);
    connect(m_findConsoleButton, &QPushButton::clicked, this, &ServerListPage::onFindConsole);
    connect(m_copyConsoleErrorsButton, &QPushButton::clicked, this, &ServerListPage::onCopyConsoleErrors);
    connect(m_clearConsoleButton, &QPushButton::clicked, ui->consoleOutput, &QPlainTextEdit::clear);
    connect(m_exportConsoleButton, &QPushButton::clicked, this, [this]() {
        QString suggestedName = tr("server-console.log");
        if (m_serverManager && !m_selectedServerId.isEmpty()) {
            const auto server = m_serverManager->getServer(m_selectedServerId);
            if (server) {
                suggestedName = server->name() + "-console.log";
            }
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Server Console"), suggestedName, tr("Log files (*.log);;Text files (*.txt)"));
        if (path.isEmpty()) {
            return;
        }
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text)
            || output.write(ui->consoleOutput->toPlainText().toUtf8()) < 0) {
            QMessageBox::warning(this, tr("Export Console"), tr("Could not write the selected log file."));
        }
    });
    connect(m_consoleSearchInput, &QLineEdit::returnPressed, this, &ServerListPage::onFindConsole);
    connect(m_updateServerSoftwareButton, &QPushButton::clicked, this, &ServerListPage::onUpdateServerSoftware);
    connect(m_changeMinecraftVersionButton, &QPushButton::clicked, this, &ServerListPage::onChangeMinecraftVersion);
    connect(m_restoreLatestUpdateBackupButton, &QPushButton::clicked, this, &ServerListPage::onRestoreLatestUpdateBackup);
    connect(m_checkContentUpdatesButton, &QPushButton::clicked, this, &ServerListPage::onCheckContentUpdates);
    connect(m_setupCurseForgeButton, &QPushButton::clicked, this, [this]() {
        APPLICATION->ShowGlobalSettings(this, QStringLiteral("apis"));
        updateUI();
        if (APPLICATION->capabilities() & Application::SupportsFlame) {
            m_updatesInfoLabel->setText(
                tr("CurseForge is enabled. Check for updates again to include CurseForge mods."));
        }
    });
    connect(m_installContentUpdateButton, &QPushButton::clicked, this, &ServerListPage::onInstallContentUpdate);
    connect(m_refreshPlayersButton, &QPushButton::clicked, this, &ServerListPage::onRefreshPlayers);
    connect(m_whitelistPlayerButton, &QPushButton::clicked, this, &ServerListPage::onWhitelistPlayer);
    connect(m_opPlayerButton, &QPushButton::clicked, this, &ServerListPage::onOpPlayer);
    connect(m_banPlayerButton, &QPushButton::clicked, this, &ServerListPage::onBanPlayer);
    connect(m_kickPlayerButton, &QPushButton::clicked, this, &ServerListPage::onKickPlayer);
    connect(m_removePlayerAccessButton, &QPushButton::clicked, this, &ServerListPage::onRemovePlayerAccess);
    connect(m_viewPlayerHistoryButton, &QPushButton::clicked, this, &ServerListPage::onViewPlayerHistory);
    connect(m_exportPlayerHistoryButton, &QPushButton::clicked, this, &ServerListPage::onExportPlayerHistory);
    connect(m_playersTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(m_contentUpdatesTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(ui->serverList, &QListWidget::currentRowChanged, this, &ServerListPage::onServerSelectionChanged);
    connect(ui->serverSearchInput, &QLineEdit::textChanged, this, [this](const QString &) { updateServerList(); });
    connect(ui->serverFilesTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        populateServerFileItem(item);
    });
    connect(ui->serverFilesTree, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *item, int) {
        if (!item) return;
        const QFileInfo file(item->data(0, Qt::UserRole).toString());
        if (file.isDir()) {
            item->setExpanded(!item->isExpanded());
        } else if (file.exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
        }
    });

    protectInputFromWheel(this);

    // Allow pressing Enter in command input to send
    connect(ui->commandInput, &QLineEdit::returnPressed, this, &ServerListPage::onSendCommand);

    // Buttons must remain keyboard-accessible. Disable dialog-default styling
    // without removing them from the Tab focus chain, then give the page itself
    // the initial focus so no action appears preselected when the window opens.
    for (QPushButton *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    setFocusPolicy(Qt::StrongFocus);
    QTimer::singleShot(0, this, [this]() { setFocus(Qt::OtherFocusReason); });

    updateUI();
    if (qEnvironmentVariableIsSet("JLAUNCHER_PROFILE_UI")) {
        qInfo() << "[Performance] Constructed ServerListPage in"
                << performanceTimer.elapsed() << "ms";
    }
}

bool ServerListPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel
        && (qobject_cast<QAbstractSpinBox *>(watched) || qobject_cast<QComboBox *>(watched))) {
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ServerListPage::protectInputFromWheel(QWidget *scope)
{
    if (!scope) return;
    for (QAbstractSpinBox *input : scope->findChildren<QAbstractSpinBox *>()) input->installEventFilter(this);
    for (QComboBox *input : scope->findChildren<QComboBox *>()) input->installEventFilter(this);
}

ServerListPage::~ServerListPage()
{
    delete ui;
}

void ServerListPage::setupServerNavigation()
{
    // Keep the selected server visible while its operational tools switch in
    // the workspace. The dedicated rail avoids an overflowing tab strip and
    // gives every destination a stable icon and keyboard-accessible row.
    const QList<QPair<QWidget *, QString>> pages = {
        {ui->overviewTab, tr("Home")},
        {ui->consoleTab, tr("Console")},
        {ui->installedContentTab, tr("Mods")},
        {m_playersTab, tr("Players")},
        {ui->filesTab, tr("Files")},
        {ui->backupsTab, tr("Backups")},
        {m_maintenanceTab, tr("Tools")},
        {ui->settingsTab, tr("Settings")}
    };

    for(const auto &page : pages)
    {
        const int index = ui->serverTabs->indexOf(page.first);
        if(index >= 0)
        {
            ui->serverTabs->removeTab(index);
        }
    }
    for(const auto &page : pages)
    {
        ui->serverTabs->addTab(page.first, page.second);
    }

    const QList<QIcon> icons = {
        launcherIcon("server", QStyle::SP_ComputerIcon),
        launcherIcon("log", QStyle::SP_FileDialogDetailedView),
        launcherIcon("loadermods", QStyle::SP_FileIcon),
        launcherIcon("accounts", QStyle::SP_DirHomeIcon),
        launcherIcon("viewfolder", QStyle::SP_DirOpenIcon),
        launcherIcon("copy", QStyle::SP_DialogSaveButton),
        launcherIcon("custom-commands", QStyle::SP_ComputerIcon),
        launcherIcon("settings", QStyle::SP_FileDialogContentsView)
    };
    for (int index = 0; index < icons.size(); ++index) {
        ui->serverTabs->setTabIcon(index, icons.at(index));
    }

    ui->serverTabs->tabBar()->hide();
    ui->serverNavigationList->clear();
    for (int index = 0; index < ui->serverTabs->count(); ++index) {
        auto *item = new QListWidgetItem(ui->serverTabs->tabIcon(index),
                                         ui->serverTabs->tabText(index),
                                         ui->serverNavigationList);
        item->setData(Qt::UserRole, index);
        item->setSizeHint(QSize(126, 44));
    }

    connect(ui->serverNavigationList, &QListWidget::currentRowChanged, this,
            [this](int row) {
        if (row < 0 || row >= ui->serverTabs->count()) return;
        if (!ui->serverTabs->isTabEnabled(row)) {
            syncServerNavigation();
            return;
        }
        ui->serverTabs->setCurrentIndex(row);
    });
    connect(ui->serverTabs, &QTabWidget::currentChanged, this,
            [this](int) { syncServerNavigation(); });

    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->overviewTab),
                                   tr("Server status, live resource use, and quick statistics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->consoleTab),
                                   tr("Live server output and commands."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->installedContentTab),
                                   tr("Installed mods or plugins and downloads."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_playersTab),
                                   tr("Player access, operator levels, bans, and history."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->filesTab),
                                   tr("Browse server files and import or export server profiles."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->backupsTab),
                                   tr("Create, restore, and manage complete server backups."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_maintenanceTab),
                                   tr("Software updates, automation, monitoring thresholds, and crash diagnostics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->settingsTab),
                                   tr("Server properties, Java, memory, and launch settings."));
    ui->serverTabs->setCurrentWidget(ui->overviewTab);
    syncServerNavigation();

    m_maintenanceTab->setDocumentMode(false);
    m_maintenanceTab->setUsesScrollButtons(true);
    m_maintenanceTab->tabBar()->setExpanding(false);
}

void ServerListPage::syncServerNavigation()
{
    if (!ui->serverNavigationList) return;

    const QSignalBlocker blocker(ui->serverNavigationList);
    for (int index = 0; index < ui->serverTabs->count(); ++index) {
        QListWidgetItem *item = ui->serverNavigationList->item(index);
        if (!item) continue;
        item->setText(ui->serverTabs->tabText(index));
        item->setIcon(ui->serverTabs->tabIcon(index));
        Qt::ItemFlags flags = item->flags();
        if (ui->serverTabs->isTabEnabled(index)) {
            flags |= Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        } else {
            flags &= ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        item->setFlags(flags);
        item->setToolTip(ui->serverTabs->tabToolTip(index));
    }
    ui->serverNavigationList->setCurrentRow(ui->serverTabs->currentIndex());
}

void ServerListPage::applyServerVisualHierarchy()
{
    // Palette roles keep the workspace native to every launcher theme while
    // object-scoped styling supplies the quieter geometry and action hierarchy.
    QFont titleFont = ui->titleLabel->font();
    titleFont.setBold(true);
    ui->titleLabel->setFont(titleFont);

    QFont sectionFont = ui->serverListTitle->font();
    sectionFont.setBold(true);
    ui->serverListTitle->setFont(sectionFont);
    ui->detailTitleLabel->setFont(sectionFont);

    applyMutedLabelPalette(ui->serverStatsLabel);
    applyMutedLabelPalette(ui->selectedServerInfoLabel);

    ui->createFromModpackButton->setIcon(launcherIcon("centralmods", QStyle::SP_FileDialogNewFolder));
    ui->createServerButton->setIcon(launcherIcon("new", QStyle::SP_FileDialogNewFolder));
    ui->startServerButton->setIcon(launcherIcon("launch", QStyle::SP_MediaPlay));
    ui->stopServerButton->setIcon(launcherIcon("status-bad", QStyle::SP_MediaStop));
    ui->restartServerButton->setIcon(launcherIcon("refresh", QStyle::SP_BrowserReload));
    ui->deleteServerButton->setIcon(launcherIcon("delete", QStyle::SP_TrashIcon));
    ui->sendCommandButton->setIcon(launcherIcon("launch", QStyle::SP_ArrowForward));

    ui->createServerButton->setProperty("role", "primary");
    ui->startServerButton->setProperty("role", "primary");
    ui->deleteServerButton->setProperty("role", "danger");
    ui->sendCommandButton->setProperty("role", "primary");

    setStyleSheet(QStringLiteral(R"QSS(
        QWidget#ServerListPage {
            background: palette(window);
            color: palette(text);
        }
        QFrame#serverHeaderPanel {
            background: palette(base);
            border: none;
            border-bottom: 1px solid palette(midlight);
        }
        QFrame#serverListPanel {
            background: palette(alternate-base);
            border: none;
            border-right: 1px solid palette(midlight);
        }
        QFrame#serverDetailPanel {
            background: palette(window);
            border: none;
        }
        QFrame#serverCommandBar,
        QFrame#serverWorkspacePanel,
        QFrame#overviewMetricsPanel {
            background: palette(base);
            border: 1px solid palette(midlight);
            border-radius: 12px;
        }
        QLabel#titleLabel,
        QLabel#detailTitleLabel,
        QLabel#serverListTitle {
            color: palette(text);
        }
        QLineEdit,
        QComboBox,
        QSpinBox,
        QTimeEdit {
            min-height: 32px;
            padding: 2px 9px;
            background: palette(base);
            color: palette(text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
        QLineEdit:focus,
        QComboBox:focus,
        QSpinBox:focus,
        QTimeEdit:focus {
            border: 2px solid palette(highlight);
            padding: 1px 8px;
        }
        QPushButton {
            min-height: 30px;
            padding: 3px 12px;
            background: palette(button);
            color: palette(button-text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
        }
        QPushButton:hover {
            border-color: palette(highlight);
        }
        QPushButton:focus {
            border: 2px solid palette(highlight);
            padding: 2px 11px;
        }
        QPushButton[role="primary"] {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: palette(highlight);
            font-weight: 600;
        }
        QPushButton:disabled {
            background: palette(alternate-base);
            color: #7d7d7d;
            border-color: palette(midlight);
        }
        QListWidget#serverList {
            background: transparent;
            border: none;
            outline: none;
        }
        QListWidget#serverList::item {
            background: transparent;
            border: none;
        }
        QWidget#serverCard {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 10px;
        }
        QWidget#serverCard:hover {
            background: palette(base);
            border-color: palette(midlight);
        }
        QWidget#serverCard[selected="true"] {
            background: palette(base);
            border-color: palette(highlight);
        }
        QListWidget#serverNavigationList {
            background: palette(alternate-base);
            border: none;
            border-right: 1px solid palette(midlight);
            padding: 9px 7px;
            outline: none;
        }
        QListWidget#serverNavigationList::item {
            min-height: 40px;
            padding: 0 10px;
            border: 1px solid transparent;
            border-radius: 8px;
        }
        QListWidget#serverNavigationList::item:hover {
            background: palette(base);
        }
        QListWidget#serverNavigationList::item:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: palette(highlight);
        }
        QListWidget#serverNavigationList::item:disabled {
            color: palette(mid);
        }
        QTabWidget#serverTabs::pane {
            background: palette(base);
            border: none;
        }
        QGroupBox {
            margin-top: 10px;
            padding: 12px;
            background: palette(base);
            border: 1px solid palette(midlight);
            border-radius: 10px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QFrame#overviewMetricsPanel QGroupBox {
            margin-top: 0;
            padding: 14px 16px;
            border: none;
            border-right: 1px solid palette(midlight);
            border-radius: 0;
            background: transparent;
        }
        QPlainTextEdit#consoleOutput,
        QTreeWidget {
            background: palette(base);
            color: palette(text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
        QHeaderView::section {
            background: palette(alternate-base);
            color: palette(text);
            border: none;
            border-bottom: 1px solid palette(midlight);
            padding: 7px 8px;
            font-weight: 600;
        }
        QProgressBar {
            min-height: 12px;
            background: palette(alternate-base);
            border: 1px solid palette(midlight);
            border-radius: 6px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: palette(highlight);
            border-radius: 5px;
        }
    )QSS"));

    // Keep destructive actions unmistakable without sacrificing contrast in
    // either launcher appearance. A local rule wins over the page's generic
    // button text rule while leaving the native button surface untouched.
    const bool darkAppearance = palette().color(QPalette::Window).lightness() < 128;
    ui->deleteServerButton->setStyleSheet(QStringLiteral("color: %1;").arg(
        darkAppearance ? QStringLiteral("#ff8a80") : QStringLiteral("#b3261e")));
}

void ServerListPage::setServerManager(ServerManager *manager)
{
    QElapsedTimer performanceTimer;
    performanceTimer.start();
    if (m_serverTrackingContext) {
        m_serverTrackingContext->deleteLater();
    }
    m_serverTrackingContext = new QObject(this);
    m_serverManager = manager;
    if (m_serverManager) {
        for (const auto &server : m_serverManager->getAllServers()) {
            attachServerTracking(server);
        }
        connect(m_serverManager, &ServerManager::serverAdded, m_serverTrackingContext, [this](const QString &id) {
            if (m_serverManager) attachServerTracking(m_serverManager->getServer(id));
        });
    }
    updateServerList();
    if (qEnvironmentVariableIsSet("JLAUNCHER_PROFILE_UI")) {
        qInfo() << "[Performance] Populated ServerListPage in"
                << performanceTimer.elapsed() << "ms";
    }
}

void ServerListPage::attachServerTracking(const std::shared_ptr<ServerInstance> &server)
{
    if (!server || !m_serverTrackingContext) return;
    const QString serverId = server->id();
    connect(server.get(), &ServerInstance::serverCrashed, m_serverTrackingContext,
            [this, server, serverId](const QString &message, const QString &details) {
        if (m_currentConnectedServer.get() == server.get()) return;
        QSettings settings;
        const QString prefix = QString("ServerDiagnostics/%1/").arg(serverId);
        settings.setValue(prefix + "lastCrash", crashSummary(message, details));
        settings.setValue(prefix + "details", structuredCrashDetails(server, message, details));
        if (m_selectedServerId == serverId) refreshDiagnostics();
    });
    connect(server.get(), &ServerInstance::playerActivity, m_serverTrackingContext,
            [this, serverId](const QString &player, bool joined) {
        if (m_currentConnectedServer && m_currentConnectedServer->id() == serverId) return;
        QSettings settings;
        const QString key = QString("ServerPlayerHistory/%1/events").arg(serverId);
        QStringList events = settings.value(key).toStringList();
        events.append(QString("%1 - %2 %3").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"), player,
            joined ? tr("joined") : tr("left")));
        while (events.size() > 100) events.removeFirst();
        settings.setValue(key, events);
        if (m_selectedServerId == serverId) refreshPlayerList();
    });
    connect(server.get(), &ServerInstance::serverSoftwareDownloadFinished,
            m_serverTrackingContext,
            [this, server, serverId](const QString &targetVersion, bool success,
                                     bool cancelled, const QString &errorMessage) {
        if (success && m_serverManager) {
            m_serverManager->save();
        }
        if (m_selectedServerId != serverId || !m_updatesInfoLabel) return;
        if (success) {
            m_updatesInfoLabel->setText(server->loaderVersion().isEmpty()
                ? tr("Server software for Minecraft %1 was installed successfully. Restart the server to verify the world and content.")
                      .arg(targetVersion)
                : tr("Server software build %1 for Minecraft %2 was installed successfully. Restart the server to verify the world and content.")
                      .arg(server->loaderVersion(), targetVersion));
            updateSelectedServerInfo();
            updateServerList();
        } else if (cancelled) {
            m_updatesInfoLabel->setText(
                tr("Server software download was cancelled. The previous server file and version were kept."));
        } else {
            m_updatesInfoLabel->setText(
                tr("Server software update failed. The previous server file and version were kept: %1")
                    .arg(errorMessage));
        }
        updateUI();
    });
}

void ServerListPage::onCreateServer()
{
    if (!m_serverManager) {
        return;
    }

    CreateServerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QString name = dialog.serverName();
    QString version = dialog.mcVersion();
    QString type = dialog.serverType();

    auto server = m_serverManager->createServer(name, version, type, QString());
    if (server) {
        server->setPort(dialog.port());
        server->setMinMemory(dialog.minMemory());
        server->setMaxMemory(dialog.maxMemory());
        server->setEulaAccepted(dialog.eulaAccepted());
        m_serverManager->save();

        updateServerList();

        // Select the newly created server
        for (int i = 0; i < ui->serverList->count(); i++) {
            if (ui->serverList->item(i)->data(Qt::UserRole).toString() == server->id()) {
                ui->serverList->setCurrentRow(i);
                break;
            }
        }

        QMessageBox::information(this, tr("Server Created"),
                                 tr("Server '%1' has been created.\n"
                                    "Click 'Start' to install the server software and launch it.")
                                 .arg(name));
    } else {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to create server."));
    }
}

void ServerListPage::onStartServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && !server->isRunning() && server->status() != ServerStatus::Downloading) {
        if (!server->eulaAccepted()) {
            QMessageBox consent(this);
            consent.setWindowTitle(tr("Minecraft EULA"));
            consent.setIcon(QMessageBox::Information);
            consent.setTextFormat(Qt::RichText);
            consent.setText(tr("Starting this server requires accepting the "
                               "<a href=\"https://aka.ms/MinecraftEULA\">Minecraft End User License Agreement</a>."));
            consent.setInformativeText(tr("Accept the EULA for this server and continue?"));
            auto *acceptButton = consent.addButton(tr("Accept and Start"), QMessageBox::AcceptRole);
            consent.addButton(QMessageBox::Cancel);
            consent.exec();
            if (consent.clickedButton() != acceptButton) {
                return;
            }
            server->setEulaAccepted(true);
            if (!m_serverManager->save()) {
                server->setEulaAccepted(false);
                QMessageBox::warning(this, tr("Minecraft EULA"), tr("Could not save the EULA acceptance."));
                return;
            }
        }
        if (server->start()) {
            appendConsoleOutput("[INFO] Starting server...");
            updateUI();
            updateServerList();
        } else if (server->status() == ServerStatus::Error) {
            QString details;
            const QStringList logLines =
                server->consoleLog().split('\n', Qt::SkipEmptyParts);
            if (!logLines.isEmpty()) {
                details = logLines.constLast().trimmed();
                details.remove(QRegularExpression("^\\[ERROR\\]\\s*"));
            }
            QMessageBox::warning(
                this, tr("Server Could Not Start"),
                details.isEmpty()
                    ? tr("The server could not be started. Open the Console tab for details.")
                    : details);
        }
    }
}

void ServerListPage::onStopServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && server->status() == ServerStatus::Downloading) {
        server->cancelDownload();
        updateUI();
        updateServerList();
    } else if (server && (server->status() == ServerStatus::Running
                          || server->status() == ServerStatus::Starting)) {
        appendConsoleOutput("[INFO] Stopping server...");
        server->stop();
        updateUI();
        updateServerList();
    }
}

void ServerListPage::onRestartServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && (server->status() == ServerStatus::Running
                   || server->status() == ServerStatus::Starting)) {
        appendConsoleOutput("[INFO] Restarting server...");
        server->restart();
        updateUI();
        updateServerList();
    }
}

void ServerListPage::onDeleteServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Delete Server"),
        tr("Move server '%1' and all of its files to the recycle bin?\n"
           "You can undo this action while the launcher remains open.").arg(server->name()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        if (!m_serverManager->deleteServer(m_selectedServerId)) {
            QMessageBox permanentDeletePrompt(
                QMessageBox::Warning,
                tr("Delete Server"),
                tr("The server could not be moved to the recycle bin."),
                QMessageBox::NoButton,
                this);
            permanentDeletePrompt.setInformativeText(
                tr("Permanently delete '%1' and all of its files instead?\n\n"
                   "This cannot be undone.").arg(server->name()));
            auto *cancelButton = permanentDeletePrompt.addButton(QMessageBox::Cancel);
            auto *permanentDeleteButton = permanentDeletePrompt.addButton(
                tr("Delete Permanently"), QMessageBox::DestructiveRole);
            permanentDeletePrompt.setDefaultButton(qobject_cast<QPushButton *>(cancelButton));
            permanentDeletePrompt.exec();
            if (permanentDeletePrompt.clickedButton() != permanentDeleteButton) {
                return;
            }

            if (!m_serverManager->deleteServerPermanently(m_selectedServerId)) {
                QMessageBox::warning(
                    this,
                    tr("Delete Server"),
                    tr("J Launcher could not delete the server folder. Close any program using its files, then try again.\n\n"
                       "Folder: %1").arg(QDir::toNativeSeparators(server->serverDirectory())));
                return;
            }
        }

        // Keep the live connection intact until deletion has actually succeeded.
        if (m_currentConnectedServer && m_currentConnectedServer->id() == m_selectedServerId) {
            disconnect(m_currentConnectedServer.get(), nullptr, this, nullptr);
            m_currentConnectedServer = nullptr;
        }
        m_selectedServerId.clear();
        ui->consoleOutput->clear();
        updateServerList();
        updateUI();
    }
}

void ServerListPage::onUndoDelete()
{
    if (!m_serverManager) {
        return;
    }
    QString restoredId;
    if (!m_serverManager->restoreLastDeletedServer(&restoredId)) {
        QMessageBox::warning(this, tr("Undo Delete"), tr("The deleted server could not be restored."));
        return;
    }
    m_selectedServerId = restoredId;
    updateServerList();
    updateUI();
}

void ServerListPage::onOpenFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(server->serverDirectory()));
    }
}

void ServerListPage::onInstallModpack()
{
    if (!m_serverManager || !APPLICATION->instances()) {
        return;
    }

    const QString initialGroup =
        APPLICATION->settings()->get("LastUsedGroupForNewInstance").toString();
    NewInstanceDialog dialog(initialGroup, QString(), {}, this,
                             NewInstanceDialog::Mode::ServerModpack);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    APPLICATION->settings()->set("LastUsedGroupForNewInstance", dialog.instGroup());

    InstanceTask *creationTask = dialog.extractTask();
    if (!creationTask) {
        QMessageBox::warning(this, tr("Create from Modpack"),
                             tr("No modpack was selected."));
        return;
    }

    QSet<QString> existingInstanceIds;
    for (int index = 0; index < APPLICATION->instances()->count(); ++index) {
        existingInstanceIds.insert(APPLICATION->instances()->at(index)->id());
    }

    QString installedInstanceId;
    const QMetaObject::Connection selectionConnection =
        connect(APPLICATION->instances(), &InstanceList::instanceSelectRequest,
                this, [&installedInstanceId](const QString &id) {
                    installedInstanceId = id;
                });
    unique_qobject_ptr<Task> task(
        APPLICATION->instances()->wrapInstanceTask(creationTask));
    ProgressDialog progress(this);
    progress.setWindowTitle(tr("Downloading Modpack and Creating Instance"));
    progress.setSkipButton(true, tr("Abort"));
    progress.execWithTask(task.get());
    disconnect(selectionConnection);

    if (!task->wasSuccessful() || installedInstanceId.isEmpty()) {
        QMessageBox::critical(
            this, tr("Create from Modpack"),
            task->failReason().isEmpty()
                ? tr("The modpack instance could not be created.")
                : tr("The modpack instance could not be created:\n%1")
                      .arg(Privacy::sanitizeText(task->failReason())));
        return;
    }

    auto *instance = dynamic_cast<MinecraftInstance *>(
        APPLICATION->instances()->getInstanceById(installedInstanceId));
    if (!instance) {
        if (!existingInstanceIds.contains(installedInstanceId)) {
            APPLICATION->instances()->trashInstance(installedInstanceId);
        }
        QMessageBox::critical(
            this, tr("Create from Modpack"),
            tr("The downloaded pack is not a Minecraft instance."));
        return;
    }

    const ServerModpackInstallResult result =
        ServerModpackInstaller::createMatchingServer(
            m_serverManager, *instance, instance->name() + tr(" Server"));
    if (!result.isValid()) {
        const bool wasNewInstance = !existingInstanceIds.contains(installedInstanceId);
        if (wasNewInstance) {
            APPLICATION->instances()->trashInstance(installedInstanceId);
        }
        QMessageBox::critical(
            this, tr("Create from Modpack"),
            wasNewInstance
                ? tr("A compatible server could not be created, so the new client "
                     "instance was rolled back.\n\n%1")
                      .arg(result.error)
                : tr("A compatible server could not be created. The existing instance "
                     "was kept because it was updated rather than newly created.\n\n%1")
                      .arg(result.error));
        return;
    }

    m_selectedServerId = result.serverId;
    updateServerList();
    updateUI();

    QString details = tr("Created client instance \"%1\" and matching stopped server "
                         "\"%2\" with the same Minecraft and loader versions.")
                          .arg(instance->name(),
                               m_serverManager->getServer(result.serverId)->name());
    details += result.hasDedicatedServerPack
        ? tr("\n\nDedicated server version: available and used (%1).")
              .arg(result.provider)
        : tr("\n\nDedicated server version: not supplied by %1. J Launcher created a "
             "server projection from the client pack. "
             "The server loader performs the final version and runtime checks when it starts.")
              .arg(result.provider);
    if (!result.skippedClientFiles.isEmpty()) {
        details += tr("\n\nExcluded %1 client-only file(s) from the server.")
                       .arg(result.skippedClientFiles.size());
    }
    if (!result.warnings.isEmpty()) {
        details += tr("\n\nCompatibility note:\n%1")
                       .arg(result.warnings.join(QStringLiteral("\n")));
    }
    QMessageBox::information(this, tr("Modpack Ready"), details);
}

void ServerListPage::onBrowseMods()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) {
        return;
    }

    const QString loader = server->loaderType().toLower();
    const ServerContentType contentType = server->contentType();
    const bool pluginServer = contentType == ServerContentType::Plugin;
    const QMap<QString, QString> loaderComponents = {
        { QStringLiteral("fabric"), QStringLiteral("net.fabricmc.fabric-loader") },
        { QStringLiteral("forge"), QStringLiteral("net.minecraftforge") },
        { QStringLiteral("neoforge"), QStringLiteral("net.neoforged") },
        { QStringLiteral("quilt"), QStringLiteral("org.quiltmc.quilt-loader") },
    };
    const QString loaderComponent = loaderComponents.value(loader);
    if (contentType == ServerContentType::None
        || (!pluginServer && loaderComponent.isEmpty())) {
        QMessageBox::information(this, tr("Download Mods"),
                                 tr("The app downloader requires a Fabric, Forge, NeoForge, Quilt, Paper, or Purpur server."));
        return;
    }
    const ModPlatform::ResourceType resourceType =
        pluginServer ? ModPlatform::ResourceType::Plugin : ModPlatform::ResourceType::Mod;
    QStringList loaderNames;
    if (loader == "paper") {
        loaderNames = { QStringLiteral("paper"), QStringLiteral("spigot"), QStringLiteral("bukkit") };
    } else if (loader == "purpur") {
        loaderNames = { QStringLiteral("purpur"), QStringLiteral("paper"),
                        QStringLiteral("spigot"), QStringLiteral("bukkit") };
    }

    QTemporaryDir compatibilityRoot(QDir::tempPath() + "/jlauncher-server-content-download-XXXXXX");
    if (!compatibilityRoot.isValid()) {
        QMessageBox::warning(this, tr("Download Mods"),
                             tr("Could not prepare the compatibility data for the mod downloader."));
        return;
    }

    auto compatibilitySettings =
        std::make_unique<INISettingsObject>(QDir(compatibilityRoot.path()).filePath("instance.cfg"));
    MinecraftInstance compatibilityInstance(APPLICATION->settings(), std::move(compatibilitySettings),
                                             compatibilityRoot.path());
    compatibilityInstance.setName(server->name());
    PackProfile *profile = compatibilityInstance.getPackProfile();
    profile->buildingFromScratch();
    profile->setComponentVersion(QStringLiteral("net.minecraft"), server->version());
    if (!pluginServer) {
        profile->setComponentVersion(loaderComponent, QStringLiteral("server"));
    }
    // This profile exists only to provide compatibility filters to the instance
    // downloader, so it is not resolved through the normal component metadata
    // task. Populate the cached versions that downloader filters read directly.
    profile->getComponent(QStringLiteral("net.minecraft"))->m_cachedVersion = server->version();
    if (!pluginServer) {
        profile->getComponent(loaderComponent)->m_cachedVersion = QStringLiteral("server");
    }

    const QString contentDirectory = server->contentDirectory();
    ModFolderModel serverContent(QDir(contentDirectory), &compatibilityInstance, true, true);
    QEventLoop initialScan;
    connect(&serverContent, &ModFolderModel::updateFinished, &initialScan, &QEventLoop::quit);
    if (serverContent.update()) {
        initialScan.exec();
    }

    ResourceDownload::ModDownloadDialog dialog(this, &serverContent, &compatibilityInstance,
                                               false, resourceType, loaderNames);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const auto selectedDownloads = dialog.getTasks();
    if (selectedDownloads.isEmpty()) {
        return;
    }

    auto downloads = std::make_unique<ConcurrentTask>(
        pluginServer ? tr("Download Server Plugins") : tr("Download Server Mods"),
        APPLICATION->settings()->get("NumberOfConcurrentDownloads").toInt());
    QStringList downloadedNames;
    const QString serverId = server->id();
    for (const auto &download : selectedDownloads) {
        const QString filename = download->getFilename();
        const QString provider = download->getProvider() == ModPlatform::ResourceProvider::MODRINTH
            ? QStringLiteral("modrinth")
            : QStringLiteral("curseforge");
        const QString source = QString("%1:%2:%3")
            .arg(provider, download->getPack()->addonId.toString(),
                 download->getVersion().fileId.toString());
        connect(download.get(), &Task::succeeded, this, [serverId, filename, source]() {
            QSettings().setValue(QString("ServerContentSources/%1/%2").arg(serverId, filename), source);
        });
        downloadedNames.append(filename);
        downloads->addTask(download);
    }

    ProgressDialog progress(this);
    progress.setWindowTitle(pluginServer ? tr("Downloading Server Plugins") : tr("Downloading Server Mods"));
    progress.setSkipButton(true, tr("Abort"));
    progress.execWithTask(downloads.get());

    if (downloads->getState() == Task::State::Failed) {
        QMessageBox::critical(this, pluginServer ? tr("Download Plugins") : tr("Download Mods"),
                              tr("One or more files could not be downloaded:\n%1").arg(
                                  Privacy::sanitizeText(downloads->failReason())));
    } else if (downloads->getState() == Task::State::AbortedByUser) {
        QMessageBox::information(this, pluginServer ? tr("Download Plugins") : tr("Download Mods"),
                                 tr("Download stopped by user."));
    } else if (downloads->wasSuccessful()) {
        const QStringList warnings = downloads->warnings();
        if (!warnings.isEmpty()) {
            QMessageBox::warning(this, tr("Download Warnings"), warnings.join('\n'));
        }
        appendConsoleOutput(pluginServer
                                ? tr("[INFO] Downloaded server plugins: %1").arg(downloadedNames.join(", "))
                                : tr("[INFO] Downloaded server mods: %1").arg(downloadedNames.join(", ")));
    }
    // A cancelled or partially failed batch can still contain successfully
    // installed files, so always bring the server views back in sync.
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onAddLocalContent()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->contentType() == ServerContentType::None) {
        return;
    }
    const bool plugins = server->contentType() == ServerContentType::Plugin;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, plugins ? tr("Add Local Plugins") : tr("Add Local Mods"),
        QString(), tr("Java Archives (*.jar)"));
    if (paths.isEmpty()) {
        return;
    }
    QString error;
    if (!server->addContentFiles(paths, &error)) {
        QMessageBox::warning(
            this, plugins ? tr("Could Not Add Plugins") : tr("Could Not Add Mods"), error);
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onToggleInstalledContent()
{
    auto *item = ui->installedContentTree->currentItem();
    if (!item) return;
    const QString source = item->data(0, Qt::UserRole).toString();
    if (source.isEmpty()) return;

    const bool disabled = source.endsWith(".disabled", Qt::CaseInsensitive);
    const QString destination = disabled ? source.left(source.size() - QString(".disabled").size()) : source + ".disabled";
    if (QFile::exists(destination)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), tr("A file with the target name already exists."));
        return;
    }
    if (!QFile::rename(source, destination)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), tr("The selected file could not be renamed."));
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onRemoveInstalledContent()
{
    auto *item = ui->installedContentTree->currentItem();
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove Installed Content"),
                              tr("Remove %1 from this server?").arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!QFile::remove(path)) {
        QMessageBox::warning(this, tr("Could Not Remove Content"), tr("The selected file could not be removed."));
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onOpenContentFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString directory = server->contentDirectory();
    if (!directory.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    }
}

void ServerListPage::onCreateBackup()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    const QString requestedName = ui->backupNameInput->text().trimmed();
    if (requestedName.isEmpty()) {
        QMessageBox::information(this, tr("Backup Name Required"), tr("Enter a name for this backup before creating it."));
        return;
    }

    QString error;
    ServerBackupInfo backup;
    if (!m_serverManager->createServerBackup(
            m_selectedServerId, requestedName, &backup, &error)) {
        QMessageBox::warning(this, tr("Backup Failed"), error);
        return;
    }
    ui->backupNameInput->clear();
    refreshServerBackups();
    refreshOverview();
    QMessageBox::information(this, tr("Backup Created"), tr("Created backup: %1").arg(backup.name));
}

void ServerListPage::onRestoreBackup()
{
    auto *item = ui->backupsTree->currentItem();
    if (!item || !m_serverManager || m_selectedServerId.isEmpty()) return;
    const QString backupPath = item->data(0, Qt::UserRole).toString();
    if (backupPath.isEmpty()) return;

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;
    if (QMessageBox::question(this, tr("Restore Backup"),
                              tr("This replaces the server files, worlds, player data, and configuration with the selected backup. Continue?"),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString error;
    QString safetyBackup;
    if (!m_serverManager->restoreServerBackup(m_selectedServerId, backupPath, &error, &safetyBackup)) {
        QMessageBox::warning(this, tr("Restore Failed"), error);
        return;
    }
    refreshInstalledContent();
    refreshServerFiles();
    refreshOverview();
    QMessageBox::information(
        this, tr("Backup Restored"),
        tr("The selected backup has been restored.\nA safety backup of the previous state was saved as %1.")
            .arg(safetyBackup));
}

void ServerListPage::onOpenBackupsFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    QDir().mkpath(backupRoot);
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupRoot));
}

void ServerListPage::onRemoveBackup()
{
    auto *item = ui->backupsTree->currentItem();
    if (!item) return;
    const QString backupPath = item->data(0, Qt::UserRole).toString();
    if (backupPath.isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove Backup"),
                              tr("Permanently remove backup '%1'?").arg(item->text(0)),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_serverManager || !m_serverManager->deleteServerBackup(
            m_selectedServerId, backupPath, &error)) {
        QMessageBox::warning(this, tr("Could Not Remove Backup"), error);
        return;
    }
    refreshServerBackups();
    refreshOverview();
}

void ServerListPage::onExportProfile()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Server Profile"),
        server->name() + ".jserver.json", tr("J Launcher Server Profile (*.jserver.json)"));
    if (path.isEmpty()) return;
    QJsonObject profile = server->toJson();
    profile.remove("id");
    profile.remove("serverDirectory");
    profile["profileFormat"] = "JLauncherServerProfile";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(profile).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        QMessageBox::warning(this, tr("Export Profile"), tr("Could not save the server profile."));
        return;
    }
    QMessageBox::information(this, tr("Profile Exported"), tr("The server profile was exported without worlds, mods, plugins, or player data."));
}

void ServerListPage::onImportProfile()
{
    if (!m_serverManager) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Server Profile"), QString(),
        tr("J Launcher Server Profile (*.jserver.json);;JSON files (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Profile"), tr("Could not read the selected profile."));
        return;
    }
    const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
    if (profile.value("profileFormat").toString() != "JLauncherServerProfile") {
        QMessageBox::warning(this, tr("Import Profile"), tr("This is not a J Launcher server profile."));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Import Server Profile"), tr("Server name:"),
        QLineEdit::Normal, profile.value("name").toString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    const auto server = m_serverManager->createServer(name, profile.value("version").toString(), profile.value("loaderType").toString("vanilla"), profile.value("loaderVersion").toString());
    if (!server) {
        QMessageBox::warning(this, tr("Import Profile"), tr("Could not create a server from this profile."));
        return;
    }
    server->setPort(profile.value("port").toInt(25565));
    server->setMinMemory(profile.value("minMemory").toInt(1024));
    server->setMaxMemory(profile.value("maxMemory").toInt(2048));
    server->setJavaPath(profile.value("javaPath").toString());
    server->setExtraJvmArguments(profile.value("extraJvmArguments").toString());
    server->setAutoRestartOnCrash(profile.value("autoRestartOnCrash").toBool(false));
    m_serverManager->save();
    m_selectedServerId = server->id();
    updateServerList();
    QMessageBox::information(this, tr("Profile Imported"), tr("Created %1. Start it to download its server software.").arg(name));
}

void ServerListPage::refreshAutomation()
{
    if (!m_automationTab) return;
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        m_automationInfoLabel->setText(tr("Select a server to configure automated maintenance."));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    QSettings settings;
    const QString prefix = QString("ServerAutomation/%1/").arg(server->id());
    m_scheduleEnabledCheck->setChecked(settings.value(prefix + "enabled", false).toBool());
    const QString action = settings.value(prefix + "action", "start").toString();
    const int actionIndex = m_scheduleActionCombo->findData(action);
    m_scheduleActionCombo->setCurrentIndex(actionIndex >= 0 ? actionIndex : 0);
    const QTime time = QTime::fromString(settings.value(prefix + "time", "03:00").toString(), "HH:mm");
    m_scheduleTimeEdit->setTime(time.isValid() ? time : QTime(3, 0));
    m_backupRetentionSpin->setValue(settings.value(prefix + "retention", 0).toInt());
    m_gracefulStopTimeoutSpin->setValue(server->gracefulStopTimeoutSeconds());
    m_autoRestartCheck->setChecked(server->autoRestartOnCrash());
    const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
    m_cpuWarningSpin->setValue(settings.value(monitoringPrefix + "cpuWarning", 85).toInt());
    m_ramWarningSpin->setValue(settings.value(monitoringPrefix + "ramWarning", 90).toInt());
    m_diskWarningSpin->setValue(settings.value(monitoringPrefix + "diskWarningGb", 2).toInt());
    m_automationInfoLabel->setText(tr("Daily schedules are checked every 30 seconds. The server must be stopped for an automatic backup."));
    refreshAutomationHistory();
}

void ServerListPage::onSaveAutomation()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;
    QSettings settings;
    const QString prefix = QString("ServerAutomation/%1/").arg(server->id());
    settings.setValue(prefix + "enabled", m_scheduleEnabledCheck->isChecked());
    settings.setValue(prefix + "action", m_scheduleActionCombo->currentData().toString());
    settings.setValue(prefix + "time", m_scheduleTimeEdit->time().toString("HH:mm"));
    settings.setValue(prefix + "retention", m_backupRetentionSpin->value());
    const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
    settings.setValue(monitoringPrefix + "cpuWarning", m_cpuWarningSpin->value());
    settings.setValue(monitoringPrefix + "ramWarning", m_ramWarningSpin->value());
    settings.setValue(monitoringPrefix + "diskWarningGb", m_diskWarningSpin->value());
    server->setAutoRestartOnCrash(m_autoRestartCheck->isChecked());
    server->setGracefulStopTimeoutSeconds(m_gracefulStopTimeoutSpin->value());
    m_serverManager->save();
    m_automationInfoLabel->setText(tr("Automation saved for %1.").arg(server->name()));
}

void ServerListPage::runAutomation(const std::shared_ptr<ServerInstance> &server, const QString &action, int retentionLimit)
{
    if (!server) return;
    if (action == "start") {
        if (server->isRunning()) {
            recordAutomation(server, action, tr("Skipped — server is already running."));
        } else if (server->start()) {
            recordAutomation(server, action, tr("Start requested."));
        } else {
            recordAutomation(server, action, tr("Could not start the server."));
        }
        return;
    }
    if (action == "stop") {
        if (!server->isRunning()) {
            recordAutomation(server, action, tr("Skipped — server is already stopped."));
        } else if (server->stop()) {
            recordAutomation(server, action, tr("Stop requested."));
        } else {
            recordAutomation(server, action, tr("Could not stop the server."));
        }
        return;
    }
    if (action == "restart") {
        if (server->restart()) {
            recordAutomation(server, action, tr("Restart requested."));
        } else {
            recordAutomation(server, action, tr("Could not restart the server."));
        }
        return;
    }
    if (action != "backup") return;
    if (server->isRunning()) {
        recordAutomation(server, action, tr("Skipped — backups require a stopped server."));
        return;
    }
    const QString automaticPrefix = QStringLiteral("Automatic backup ");
    const QString name = automaticPrefix
        + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString error;
    ServerBackupInfo backup;
    if (!m_serverManager
        || !m_serverManager->createServerBackup(server->id(), name, &backup, &error)) {
        server->appendLog("[BACKUP ERROR] " + error);
        recordAutomation(server, action, tr("Backup failed: %1").arg(error));
        return;
    }
    server->appendLog("[BACKUP] Created automatic backup: " + backup.name);
    recordAutomation(server, action, tr("Created backup %1.").arg(backup.name));
    if (retentionLimit > 0
        && !m_serverManager->enforceServerBackupRetention(
            server->id(), automaticPrefix, retentionLimit, &error)) {
        server->appendLog("[BACKUP ERROR] " + error);
        recordAutomation(server, action,
                         tr("Backup was created, but retention failed: %1").arg(error));
    }
    if (server->id() == m_selectedServerId) {
        refreshServerBackups();
        refreshOverview();
    }
}

void ServerListPage::refreshAutomationHistory()
{
    if (!m_automationHistoryList) return;
    m_automationHistoryList->clear();
    if (m_selectedServerId.isEmpty()) {
        m_automationHistoryList->addItem(tr("Select a server to view automation activity."));
        return;
    }
    QSettings settings;
    const QStringList history = settings.value(QString("ServerAutomation/%1/history").arg(m_selectedServerId)).toStringList();
    if (history.isEmpty()) {
        m_automationHistoryList->addItem(tr("No automated actions have run for this server yet."));
        return;
    }
    m_automationHistoryList->addItems(history);
}

void ServerListPage::recordAutomation(const std::shared_ptr<ServerInstance> &server, const QString &action, const QString &result)
{
    if (!server) return;
    QSettings settings;
    const QString key = QString("ServerAutomation/%1/history").arg(server->id());
    QStringList history = settings.value(key).toStringList();
    history.prepend(QString("%1 — %2: %3")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), action.toUpper(), result));
    while (history.size() > 50) history.removeLast();
    settings.setValue(key, history);
    if (server->id() == m_selectedServerId) refreshAutomationHistory();
}

void ServerListPage::onRunAutomationNow()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    runAutomation(server, m_scheduleActionCombo->currentData().toString(), m_backupRetentionSpin->value());
    m_automationInfoLabel->setText(tr("Ran %1 action for %2.").arg(m_scheduleActionCombo->currentText(), server->name()));
    updateUI();
}

void ServerListPage::refreshDiagnostics()
{
    if (!m_diagnosticsLabel) return;
    if (m_selectedServerId.isEmpty()) {
        m_diagnosticsLabel->setText(tr("No server selected."));
        return;
    }
    QSettings settings;
    const QString crash = settings.value(QString("ServerDiagnostics/%1/lastCrash").arg(m_selectedServerId)).toString();
    m_diagnosticsLabel->setText(crash.isEmpty() ? tr("No crash report recorded for this server.") : tr("Latest crash: %1").arg(crash));
}

void ServerListPage::onViewCrashReport()
{
    if (m_selectedServerId.isEmpty()) return;
    QSettings settings;
    const QString details = settings.value(QString("ServerDiagnostics/%1/details").arg(m_selectedServerId)).toString();
    if (details.isEmpty()) return;
    QMessageBox dialog(this);
    dialog.setWindowTitle(tr("Latest Crash Report"));
    dialog.setIcon(QMessageBox::Critical);
    dialog.setText(m_diagnosticsLabel->text());
    dialog.setDetailedText(details);
    dialog.exec();
}

void ServerListPage::onFindConsole()
{
    const QString term = m_consoleSearchInput->text().trimmed();
    if (term.isEmpty()) return;
    if (!ui->consoleOutput->find(term)) {
        ui->consoleOutput->moveCursor(QTextCursor::Start);
        ui->consoleOutput->find(term);
    }
}

void ServerListPage::onCopyConsoleErrors()
{
    QStringList errors;
    for (const QString &line : ui->consoleOutput->toPlainText().split('\n')) {
        if (line.contains("[ERROR]", Qt::CaseInsensitive) || line.contains("exception", Qt::CaseInsensitive) || line.contains("[CRASH]", Qt::CaseInsensitive)) errors.append(line);
    }
    QApplication::clipboard()->setText(errors.isEmpty() ? tr("No errors found in this console log.") : errors.join('\n'));
}

void ServerListPage::onUpdateServerSoftware()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;

    if (server->loaderType().compare(QStringLiteral("vanilla"), Qt::CaseInsensitive) == 0) {
        QMessageBox::information(
            this, tr("Vanilla Server Builds"),
            tr("Mojang publishes one official server JAR for each Minecraft version, so Vanilla has no separate build numbers. "
               "J Launcher can download a fresh verified copy of the official JAR."));
        startServerSoftwareUpdate(server, server->version(), false);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Update Current Build"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        tr("Minecraft %1 · %2\nCurrent build: %3")
            .arg(server->version(), server->loaderType(),
                 server->loaderVersion().isEmpty() ? tr("Not recorded") : server->loaderVersion()),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    auto *build = new QComboBox(&dialog);
    build->setObjectName(QStringLiteral("updateServerBuildCombo"));
    form->addRow(tr("Provider build:"), build);
    layout->addLayout(form);
    auto *status = new QLabel(
        tr("Loading builds published by %1 for Minecraft %2...")
            .arg(server->loaderType(), server->version()), &dialog);
    status->setWordWrap(true);
    applyMutedLabelPalette(status);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Install Build"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    ServerDownloader downloader(&dialog);
    connect(&downloader, &ServerDownloader::buildsReady, &dialog,
            [&](const QStringList &builds) {
        build->clear();
        build->addItems(builds);
        const bool different = !build->currentText().isEmpty()
            && build->currentText() != server->loaderVersion();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(different);
        status->setText(different
            ? tr("%1 builds available. Newest provider build is selected.").arg(builds.size())
            : tr("%1 builds available. Select a build different from the installed build.").arg(builds.size()));
    });
    connect(&downloader, &ServerDownloader::buildsFailed, &dialog,
            [&](const QString &error) {
        status->setText(tr("Could not load builds: %1").arg(error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    });
    connect(build, &QComboBox::currentTextChanged, &dialog, [&](const QString &selected) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !selected.isEmpty() && selected != server->loaderVersion());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    downloader.fetchAvailableBuilds(server->version(), server->loaderType());

    if (dialog.exec() != QDialog::Accepted) return;
    const QString targetBuild = build->currentText().trimmed();
    if (targetBuild.isEmpty() || targetBuild == server->loaderVersion()) return;
    startServerSoftwareUpdate(server, server->version(), false, targetBuild);
}

void ServerListPage::onChangeMinecraftVersion()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;

    const QString loader = server->loaderType().trimmed().toLower();
    if (loader == QStringLiteral("fabric") || loader == QStringLiteral("forge")
        || loader == QStringLiteral("neoforge")) {
        QMessageBox::information(
            this, tr("Modded Version Change Blocked"),
            tr("J Launcher will not automatically move a modded server to another Minecraft version. "
               "The loader and every server mod must be compatible with the target version.\n\n"
               "Create a new compatible modpack server, verify it, and then migrate the world. "
               "Updating the current build remains available."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Change Minecraft Version"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(
        tr("Current version: %1. Changing Minecraft versions can make worlds or plugins incompatible. "
           "A rollback backup will be created before downloading.").arg(server->version()),
        &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *form = new QFormLayout();
    auto *channel = new QComboBox(&dialog);
    channel->setObjectName(QStringLiteral("upgradeVersionChannelCombo"));
    channel->addItem(tr("Releases"), static_cast<int>(ServerDownloader::VersionChannel::Release));
    channel->addItem(tr("Snapshots"), static_cast<int>(ServerDownloader::VersionChannel::Snapshot));
    channel->addItem(tr("Betas"), static_cast<int>(ServerDownloader::VersionChannel::Beta));
    channel->addItem(tr("All versions"), -1);
    auto *version = new QComboBox(&dialog);
    version->setObjectName(QStringLiteral("upgradeMinecraftVersionCombo"));
    form->addRow(tr("Version channel:"), channel);
    form->addRow(tr("Target version:"), version);
    layout->addLayout(form);

    auto *status = new QLabel(tr("Loading versions published by %1...").arg(server->loaderType()), &dialog);
    status->setWordWrap(true);
    applyMutedLabelPalette(status);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Change Version"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    QStringList availableVersions;
    const auto applyFilter = [&]() {
        const int selectedChannel = channel->currentData().toInt();
        const QString previous = version->currentText();
        QStringList filtered;
        for (const QString &candidate : std::as_const(availableVersions)) {
            if (selectedChannel < 0
                || static_cast<int>(ServerDownloader::versionChannel(candidate)) == selectedChannel) {
                filtered.append(candidate);
            }
        }
        const QSignalBlocker blocker(version);
        version->clear();
        version->addItems(filtered);
        if (filtered.contains(previous)) version->setCurrentText(previous);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !version->currentText().isEmpty() && version->currentText() != server->version());
        status->setText(tr("%1 versions shown. Select a version different from %2.")
                            .arg(filtered.size()).arg(server->version()));
    };

    ServerDownloader downloader(&dialog);
    connect(&downloader, &ServerDownloader::versionsReady, &dialog,
            [&](const QStringList &versions) {
        availableVersions = versions;
        applyFilter();
    });
    connect(&downloader, &ServerDownloader::versionsFailed, &dialog,
            [&](const QString &error) {
        status->setText(tr("Could not load versions: %1").arg(error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    });
    connect(channel, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { applyFilter(); });
    connect(version, &QComboBox::currentTextChanged, &dialog, [&](const QString &selected) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !selected.isEmpty() && selected != server->version());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    downloader.fetchAvailableVersions(server->loaderType());

    if (dialog.exec() != QDialog::Accepted) return;
    const QString targetVersion = version->currentText().trimmed();
    if (targetVersion.isEmpty() || targetVersion == server->version()) return;
    startServerSoftwareUpdate(server, targetVersion, true);
}

bool ServerListPage::startServerSoftwareUpdate(
    const std::shared_ptr<ServerInstance> &server, const QString &targetVersion,
    bool changeVersion, const QString &targetBuild)
{
    if (!server || server->isRunning() || targetVersion.trimmed().isEmpty()) return false;
    const QString prompt = changeVersion
        ? tr("Change this %1 server from Minecraft %2 to %3?\n\n"
             "A complete rollback backup will be created first. Verify the world and all plugins before deleting that backup.")
              .arg(server->loaderType(), server->version(), targetVersion)
        : (targetBuild.isEmpty()
            ? tr("Download a fresh %1 server for Minecraft %2?\n\n"
                 "A complete rollback backup will be created first.")
                  .arg(server->loaderType(), server->version())
            : tr("Install %1 build %2 for Minecraft %3?\n\n"
                 "A complete rollback backup will be created first.")
                  .arg(server->loaderType(), targetBuild, server->version()));
    if (QMessageBox::question(this,
                              changeVersion ? tr("Change Minecraft Version")
                                            : tr("Update Current Build"),
                              prompt, QMessageBox::Yes | QMessageBox::No)
        != QMessageBox::Yes) {
        return false;
    }

    QString backupError;
    ServerBackupInfo rollbackBackup;
    if (!m_serverManager->createServerBackup(
            server->id(), tr("Before update"), &rollbackBackup, &backupError)) {
        QMessageBox::warning(this, tr("Update Cancelled"), tr("Could not create the rollback backup:\n%1").arg(backupError));
        return false;
    }
    const QString backupFolderName = QFileInfo(rollbackBackup.path).fileName();
    QSettings historySettings;
    const QString historyKey = QString("ServerUpdates/%1/history").arg(server->id());
    QStringList history = historySettings.value(historyKey).toStringList();
    const QString updateSource = changeVersion ? server->version() : server->loaderVersion();
    const QString updateTarget = targetBuild.isEmpty() ? targetVersion : targetBuild;
    history.append(QString("%1 — %2 %3 -> %4; rollback backup %5")
                       .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"),
                            changeVersion ? tr("version change") : tr("build update"),
                            updateSource, updateTarget, backupFolderName));
    while (history.size() > 30) history.removeFirst();
    historySettings.setValue(historyKey, history);
    historySettings.setValue(QString("ServerUpdates/%1/latestRollbackBackup").arg(server->id()),
                             backupFolderName);
    historySettings.setValue(QString("ServerUpdates/%1/latestRollbackBackupPath").arg(server->id()),
                             rollbackBackup.path);
    const bool started = changeVersion
        ? server->downloadServerJarForVersion(targetVersion, server->javaPath(), false)
        : (targetBuild.isEmpty()
            ? server->downloadServerJar(server->javaPath(), false)
            : server->downloadServerBuild(targetBuild, server->javaPath(), false));
    if (started) {
        m_updatesInfoLabel->setText(
            targetBuild.isEmpty()
                ? tr("Created rollback backup %1. Downloading server software for Minecraft %2; follow progress in Console.")
                      .arg(rollbackBackup.name, targetVersion)
                : tr("Created rollback backup %1. Downloading %2 build %3; follow progress in Console.")
                      .arg(rollbackBackup.name, server->loaderType(), targetBuild));
        appendConsoleOutput(
            tr("[INFO] Created rollback backup %1, then downloading server software for Minecraft %2 without starting the server.")
                .arg(backupFolderName, targetVersion));
        refreshServerBackups();
        updateUI();
        updateServerList();
        return true;
    }
    m_updatesInfoLabel->setText(tr("The server software download could not be started. The rollback backup was kept."));
    return false;
}

void ServerListPage::onRestoreLatestUpdateBackup()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;
    QSettings updateSettings;
    QString backupPath = updateSettings.value(
        QString("ServerUpdates/%1/latestRollbackBackupPath").arg(server->id())).toString();
    if (backupPath.isEmpty()) {
        const QString backupName = updateSettings.value(
            QString("ServerUpdates/%1/latestRollbackBackup").arg(server->id())).toString();
        if (!backupName.isEmpty()) {
            backupPath = QDir(QDir(server->serverDirectory()).filePath("backups"))
                             .filePath(backupName);
        }
    }
    if (backupPath.isEmpty()) {
        QMessageBox::information(this, tr("No Update Backup"), tr("No rollback backup has been recorded for this server yet."));
        return;
    }
    if (!QFileInfo(backupPath).isDir()) {
        QMessageBox::warning(this, tr("Update Backup Missing"), tr("The latest recorded rollback backup could not be found."));
        return;
    }
    for (int index = 0; index < ui->backupsTree->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = ui->backupsTree->topLevelItem(index);
        if (QDir::cleanPath(item->data(0, Qt::UserRole).toString()) == QDir::cleanPath(backupPath)) {
            ui->backupsTree->setCurrentItem(item);
            onRestoreBackup();
            return;
        }
    }
    QMessageBox::warning(this, tr("Update Backup Missing"), tr("The latest rollback backup is not available in the backup list."));
}

void ServerListPage::onCheckContentUpdates()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;

    const QString loader = server->loaderType().toLower();
    if (server->contentType() == ServerContentType::None) return;

    m_contentUpdatesTree->clear();
    const QString contentDirectory = server->contentDirectory();
    const QFileInfoList files = QDir(contentDirectory).entryInfoList(QStringList() << "*.jar", QDir::Files);
    QSettings settings;
    const QString sourcePrefix = QString("ServerContentSources/%1/").arg(server->id());
    int requests = 0;
    int untracked = 0;
    int curseForgeNeedsKey = 0;
    int recoveredTracking = 0;
    for (const QFileInfo &installed : files) {
        QString source = settings.value(sourcePrefix + installed.fileName()).toString();
        if (source.isEmpty() && APPLICATION->instances()) {
            for (int instanceIndex = 0;
                 instanceIndex < APPLICATION->instances()->count(); ++instanceIndex) {
                MinecraftInstance *instance = APPLICATION->instances()->at(instanceIndex);
                if (!instance) continue;
                source = ServerModpackInstaller::contentTrackingSource(
                    instance->gameRoot(), installed.absoluteFilePath());
                if (!source.isEmpty()) {
                    settings.setValue(sourcePrefix + installed.fileName(), source);
                    ++recoveredTracking;
                    break;
                }
            }
        }
        const QString provider = source.section(':', 0, 0).toLower();
        const QString projectId = source.section(':', 1, 1);
        const QString installedVersionId = source.section(':', 2, 2);
        auto *item = new QTreeWidgetItem(m_contentUpdatesTree);
        item->setText(0, installed.fileName());
        item->setText(1, source.isEmpty() ? tr("Not tracked")
            : provider == QStringLiteral("curseforge") ? tr("CurseForge") : tr("Modrinth"));
        item->setData(0, Qt::UserRole + 1, installed.absoluteFilePath());
        item->setData(0, Qt::UserRole + 2, source);
        if (source.isEmpty() || projectId.isEmpty()) {
            item->setText(2, source.isEmpty()
                ? tr("Downloaded before update tracking")
                : tr("Check from the content browser"));
            ++untracked;
            continue;
        }
        if (provider == QStringLiteral("curseforge")
            && !(APPLICATION->capabilities() & Application::SupportsFlame)) {
            item->setText(2, tr("CurseForge API key required — use Set Up CurseForge"));
            ++curseForgeNeedsKey;
            continue;
        }
        if (provider != QStringLiteral("modrinth")
            && provider != QStringLiteral("curseforge")) {
            item->setText(2, tr("Unknown update provider"));
            ++untracked;
            continue;
        }

        QUrl url(provider == QStringLiteral("curseforge")
            ? QString(BuildConfig.FLAME_BASE_URL + "/mods/%1/files").arg(projectId)
            : QString("https://api.modrinth.com/v2/project/%1/version").arg(projectId));
        QUrlQuery query;
        if (provider == QStringLiteral("curseforge")) {
            query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("10000"));
            query.addQueryItem(QStringLiteral("gameVersion"), server->version());
        } else {
            query.addQueryItem("loaders", QString("[\"%1\"]").arg(loader));
            query.addQueryItem("game_versions", QString("[\"%1\"]").arg(server->version()));
        }
        url.setQuery(query);
        item->setText(2, tr("Checking…"));
        ++requests;
        QNetworkReply *reply = m_updatesNetwork->get(
            provider == QStringLiteral("curseforge")
                ? curseForgeUpdateRequest(url) : updateRequest(url));
        const QString installedName = installed.fileName();
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, source, provider, projectId, installedVersionId,
                 installedName, loader]() {
            QTreeWidgetItem *item = nullptr;
            for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
                QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
                if (candidate->text(0) == installedName && candidate->data(0, Qt::UserRole + 2).toString() == source) {
                    item = candidate;
                    break;
                }
            }
            if (!item) { reply->deleteLater(); return; }
            if (reply->error() != QNetworkReply::NoError) {
                item->setText(2, tr("Could not check — %1").arg(reply->errorString()));
                reply->deleteLater();
                return;
            }
            QString metadataError;
            ServerContentUpdateCandidate update = provider == QStringLiteral("curseforge")
                ? ServerContentUpdater::parseCurseForgeFilesResponse(
                      reply->readAll(), installedName, loader, &metadataError)
                : ServerContentUpdater::parseModrinthVersionResponse(
                      reply->readAll(), installedName, &metadataError);
            if (!installedVersionId.isEmpty() && update.versionId == installedVersionId) {
                update.available = false;
                update.upToDate = true;
            }
            if (!metadataError.isEmpty()) {
                item->setText(2, tr("Could not check — %1").arg(metadataError));
            } else if (provider == QStringLiteral("curseforge") && update.available
                       && update.url.isEmpty()) {
                item->setText(2, tr("Resolving CurseForge download…"));
                const QUrl downloadUrlEndpoint =
                    FlameAPI::fileDownloadUrlEndpoint(projectId, update.providerFileId);
                QNetworkReply *downloadReply = m_updatesNetwork->get(
                    curseForgeUpdateRequest(downloadUrlEndpoint));
                connect(downloadReply, &QNetworkReply::finished, this,
                        [this, downloadReply, source, installedName, update]() mutable {
                    QTreeWidgetItem *currentItem = nullptr;
                    for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
                        QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
                        if (candidate->text(0) == installedName
                            && candidate->data(0, Qt::UserRole + 2).toString() == source) {
                            currentItem = candidate;
                            break;
                        }
                    }
                    if (!currentItem) {
                        downloadReply->deleteLater();
                        return;
                    }
                    if (downloadReply->error() != QNetworkReply::NoError) {
                        currentItem->setText(
                            2, tr("CurseForge download unavailable — %1")
                                   .arg(downloadReply->errorString()));
                    } else {
                        QString urlError;
                        update.url = FlameAPI::loadFileDownloadUrl(
                            downloadReply->readAll(), &urlError);
                        if (update.url.isEmpty()) {
                            currentItem->setText(
                                2, tr("CurseForge download unavailable — %1").arg(urlError));
                        } else {
                            showContentUpdate(currentItem, update);
                        }
                    }
                    downloadReply->deleteLater();
                    updateUI();
                });
            } else {
                showContentUpdate(item, update);
            }
            reply->deleteLater();
            updateUI();
        });
    }
    if (requests) {
        m_updatesInfoLabel->setText(
            tr("Checking %1 tracked file(s) from Modrinth and CurseForge. Recovered tracking for %2 old file(s); %3 file(s) still need manual source selection; %4 CurseForge file(s) need an API key.")
                .arg(requests).arg(recoveredTracking).arg(untracked)
                .arg(curseForgeNeedsKey));
    } else if (curseForgeNeedsKey) {
        m_updatesInfoLabel->setText(
            tr("CurseForge updates need an API key. Use Set Up CurseForge, save a valid key in Services, then check again."));
    } else {
        m_updatesInfoLabel->setText(
            tr("No tracked files were found. Mods imported by new modpack servers and files downloaded from the Mods or Plugins tab are tracked automatically."));
    }
    updateUI();
}

void ServerListPage::onInstallContentUpdate()
{
    if (m_activeContentUpdater) {
        m_activeContentUpdater->cancel();
        m_installContentUpdateButton->setEnabled(false);
        m_updatesInfoLabel->setText(
            tr("Cancelling content update; the installed file will be kept."));
        return;
    }
    auto *item = m_contentUpdatesTree ? m_contentUpdatesTree->currentItem() : nullptr;
    if (!item || !m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;
    const QUrl url(item->data(0, Qt::UserRole).toString());
    const QString replacementName = item->data(0, Qt::UserRole + 3).toString();
    const QString oldPath = item->data(0, Qt::UserRole + 1).toString();
    const QString source = item->data(0, Qt::UserRole + 2).toString();
    const auto hashAlgorithm = static_cast<QCryptographicHash::Algorithm>(item->data(0, Qt::UserRole + 4).toInt());
    const QByteArray expectedHash = item->data(0, Qt::UserRole + 5).toByteArray();
    const QString versionId = item->data(0, Qt::UserRole + 6).toString();
    if (!url.isValid() || replacementName.isEmpty() || oldPath.isEmpty() || expectedHash.isEmpty()) return;

    const QString serverId = m_selectedServerId;
    auto *updater = new ServerContentUpdater(this);
    m_activeContentUpdater = updater;
    item->setText(2, tr("Downloading…"));
    m_installContentUpdateButton->setText(tr("Cancel Update"));
    m_installContentUpdateButton->setEnabled(true);
    connect(updater, &ServerContentUpdater::progress, this,
            [this, oldPath](qint64 received, qint64 total) {
        if (total <= 0) return;
        for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
            QTreeWidgetItem* candidate = m_contentUpdatesTree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == oldPath) {
                candidate->setText(
                    2, tr("Downloading… %1%").arg(received * 100 / total));
                break;
            }
        }
    });
    connect(updater, &ServerContentUpdater::finished, this,
            [this, updater, hashAlgorithm, expectedHash, versionId, oldPath, source,
             serverId](const ServerContentUpdateResult& result) {
        QTreeWidgetItem *target = nullptr;
        for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
            QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == oldPath) {
                target = candidate;
                break;
            }
        }
        if (result.success) {
            QSettings settings;
            const QString prefix = QString("ServerContentSources/%1/").arg(serverId);
            const QString updatedSource = QStringLiteral("%1:%2")
                .arg(source.section(':', 0, 1), versionId);
            settings.remove(prefix + QFileInfo(oldPath).fileName());
            settings.setValue(prefix + QFileInfo(result.destinationPath).fileName(), updatedSource);
            const QString metadataPrefix = QString("ServerContentMetadata/%1/%2/")
                .arg(serverId, QFileInfo(result.destinationPath).fileName());
            settings.setValue(metadataPrefix + "versionId", versionId);
            settings.setValue(metadataPrefix + "url", result.finalUrl.toString());
            settings.setValue(metadataPrefix + "hashAlgorithm", static_cast<int>(hashAlgorithm));
            settings.setValue(metadataPrefix + "hash", QString::fromLatin1(expectedHash.toHex()));
            settings.setValue(metadataPrefix + "installedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            if (target) {
                target->setText(0, QFileInfo(result.destinationPath).fileName());
                target->setText(2, tr("Updated — restart server to load it"));
                target->setData(0, Qt::UserRole, QString());
                target->setData(0, Qt::UserRole + 1, result.destinationPath);
            }
            if (m_selectedServerId == serverId) refreshInstalledContent();
        } else {
            if (target) target->setText(2, result.message);
        }
        m_updatesInfoLabel->setText(result.message);
        if (m_activeContentUpdater == updater) m_activeContentUpdater = nullptr;
        updater->deleteLater();
        m_installContentUpdateButton->setText(tr("Install Selected Update"));
        updateUI();
    });

    ServerContentUpdateRequest request;
    request.url = url;
    request.installedPath = oldPath;
    request.replacementName = replacementName;
    request.hashAlgorithm = hashAlgorithm;
    request.expectedHash = expectedHash;
    QString error;
    if (!updater->start(server, request, &error)) {
        m_activeContentUpdater = nullptr;
        updater->deleteLater();
        item->setText(2, tr("Update blocked — %1").arg(error));
        m_updatesInfoLabel->setText(error);
        m_installContentUpdateButton->setText(tr("Install Selected Update"));
        updateUI();
    }
}

void ServerListPage::onRefreshPlayers()
{
    refreshPlayerList();
}

void ServerListPage::onWhitelistPlayer()
{
    auto *item = m_playersTree ? m_playersTree->currentItem() : nullptr;
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->setPlayerWhitelistedLive(item->text(0), true, &error)) {
            QMessageBox::warning(this, tr("Whitelist"), error);
            return;
        }
        item->setText(2, tr("Yes"));
        m_playersInfoLabel->setText(
            tr("Sent live whitelist command for %1. The Players tab and selection were kept.")
                .arg(item->text(0)));
        updateUI();
        return;
    }
    if (!ServerPlayerAccess::setWhitelisted(server, uuid, item->text(0), true, &error)) {
        QMessageBox::warning(this, tr("Whitelist"), error);
        return;
    }
    refreshPlayerList();
}

void ServerListPage::onOpPlayer()
{
    auto *item = m_playersTree ? m_playersTree->currentItem() : nullptr;
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;

    if (server->status() == ServerStatus::Running) {
        QString error;
        if (!server->setPlayerOperatorLive(item->text(0), true, &error)) {
            QMessageBox::warning(this, tr("Operators"), error);
            return;
        }
        item->setText(3, tr("Yes (server default level)"));
        m_playersInfoLabel->setText(
            tr("Sent live operator command for %1. The permission level comes from server.properties while running.")
                .arg(item->text(0)));
        updateUI();
        return;
    }

    QMenu levelMenu(this);
    const QList<QPair<int, QString>> levels = {
        {1, tr("Level 1 — bypass spawn protection")},
        {2, tr("Level 2 — use command blocks")},
        {3, tr("Level 3 — manage players")},
        {4, tr("Level 4 — full server control")}
    };
    for (const auto &level : levels) {
        QAction *action = levelMenu.addAction(level.second);
        action->setData(level.first);
    }
    QAction *selected = levelMenu.exec(QCursor::pos());
    if (!selected) return;

    QString error;
    if (!ServerPlayerAccess::setOperator(server, uuid, item->text(0),
                                         selected->data().toInt(), &error)) {
        QMessageBox::warning(this, tr("Operators"), error);
        return;
    }
    refreshPlayerList();
}

void ServerListPage::onBanPlayer()
{
    auto *item = m_playersTree ? m_playersTree->currentItem() : nullptr;
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    if (QMessageBox::question(this, tr("Ban Player"),
                              tr("Ban %1 from this server?").arg(item->text(0)))
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->setPlayerBannedLive(item->text(0), true,
                                         tr("Banned from J Launcher"), &error)) {
            QMessageBox::warning(this, tr("Ban Player"), error);
            return;
        }
        item->setText(4, tr("Yes"));
        m_playersInfoLabel->setText(
            tr("Sent live ban command for %1. The Players tab and selection were kept.")
                .arg(item->text(0)));
        updateUI();
        return;
    }
    if (!ServerPlayerAccess::setBanned(server, uuid, item->text(0), true,
                                       tr("Banned from J Launcher"), &error)) {
        QMessageBox::warning(this, tr("Ban Player"), error);
        return;
    }
    refreshPlayerList();
}

void ServerListPage::onKickPlayer()
{
    auto *item = m_playersTree ? m_playersTree->currentItem() : nullptr;
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    bool accepted = false;
    const QString reason = QInputDialog::getText(
        this, tr("Kick Player"), tr("Reason:"), QLineEdit::Normal,
        tr("Removed by server operator"), &accepted);
    if (!accepted) return;

    QString error;
    if (!server->kickPlayer(item->text(0), reason, &error)) {
        QMessageBox::warning(this, tr("Kick Player"), error);
    }
}

void ServerListPage::onRemovePlayerAccess()
{
    auto *item = m_playersTree ? m_playersTree->currentItem() : nullptr;
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->clearPlayerAccessLive(item->text(0), &error)) {
            QMessageBox::warning(this, tr("Remove Access"), error);
            return;
        }
        item->setText(2, tr("No"));
        item->setText(3, tr("No"));
        item->setText(4, tr("No"));
        m_playersInfoLabel->setText(
            tr("Sent live whitelist removal, de-op, and pardon commands for %1.")
                .arg(item->text(0)));
        updateUI();
        return;
    }
    if (!ServerPlayerAccess::clearAccess(server, uuid, &error)) {
        QMessageBox::warning(this, tr("Remove Access"), error);
        return;
    }
    refreshPlayerList();
}

void ServerListPage::onViewPlayerHistory()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QStringList history = QSettings().value(QString("ServerPlayerHistory/%1/events").arg(server->id())).toStringList();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Player Activity - %1").arg(server->name()));
    dialog.setMinimumSize(620, 400);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Search player activity..."));
    auto *list = new QListWidget(&dialog);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    const auto populate = [list, history](const QString &term) {
        list->clear();
        for (const QString &event : history) {
            if (term.isEmpty() || event.contains(term, Qt::CaseInsensitive)) list->addItem(event);
        }
        if (list->count() == 0) list->addItem(QObject::tr("No matching activity."));
    };
    populate(QString());
    connect(filter, &QLineEdit::textChanged, &dialog, populate);
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void ServerListPage::onExportPlayerHistory()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QStringList history = QSettings().value(QString("ServerPlayerHistory/%1/events").arg(server->id())).toStringList();
    const QString suggested = QDir::home().filePath(server->name().simplified().replace(' ', '-') + "-player-history.json");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Player Activity"), suggested,
                                                       tr("JSON files (*.json);;CSV files (*.csv)"));
    if (path.isEmpty()) return;

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export Player Activity"), tr("Could not create the selected export file."));
        return;
    }
    if (path.endsWith(".csv", Qt::CaseInsensitive)) {
        auto quote = [](QString value) {
            value.replace('"', "\"\"");
            return '"' + value + '"';
        };
        QString csv = "event\n";
        for (const QString &event : history) csv += quote(event) + '\n';
        output.write(csv.toUtf8());
    } else {
        QJsonArray events;
        for (const QString &event : history) events.append(event);
        QJsonObject document{{"serverId", server->id()}, {"serverName", server->name()},
                             {"exportedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {"events", events}};
        output.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    }
    if (!output.commit()) {
        QMessageBox::warning(this, tr("Export Player Activity"), tr("Could not finish writing the export file."));
        return;
    }
    QMessageBox::information(this, tr("Player Activity Exported"), tr("Saved player activity to %1.").arg(QDir::toNativeSeparators(path)));
}

void ServerListPage::onImportServerPack()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) {
        return;
    }

    const QString archive = QFileDialog::getOpenFileName(
        this, tr("Import Server Pack"), QString(), tr("ZIP archives (*.zip)"));
    if (archive.isEmpty()) {
        return;
    }

    QString error;
    if (!server->importServerPack(archive, &error)) {
        QMessageBox::warning(this, tr("Could Not Import Server Pack"), error);
        return;
    }

    appendConsoleOutput(tr("[INFO] Imported server pack: %1").arg(archive));
    const bool recordSaved = m_serverManager->save();
    updateServerList();
    updateSelectedServerInfo();
    if (!recordSaved) {
        QMessageBox::warning(
            this, tr("Server Pack Imported With Warning"),
            tr("The server pack files were imported, but the updated server record could not be saved. "
               "The imported files are already present; check that the J Launcher data folder is writable, "
               "then save the server settings again."));
        return;
    }
    QMessageBox::information(this, tr("Server Pack Imported"),
                             tr("The server pack's mods and supported configuration files were imported. "
                                "Start or restart the server to load them."));
}

void ServerListPage::onSendCommand()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && server->isRunning()) {
        QString command = ui->commandInput->text().trimmed();
        if (!command.isEmpty()) {
            server->writeStdin(command);
            const QString safeCommand = Privacy::sanitizeCommandForDisplay(command);
            server->appendLog(safeCommand);
            appendConsoleOutput(safeCommand);
            ui->commandInput->clear();
        }
    }
}

void ServerListPage::onSettingsClicked()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Server Settings - %1").arg(server->name()));
    dialog.setMinimumSize(700, 680);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    ServerSettingsPage *settingsPage = new ServerSettingsPage(&dialog);
    settingsPage->setServer(server);
    layout->addWidget(settingsPage);

    connect(settingsPage, &ServerSettingsPage::settingsSaved, &dialog, [this, &dialog]() {
        if (m_serverManager && !m_serverManager->save()) {
            QMessageBox::warning(
                &dialog,
                tr("Could Not Save Server Settings"),
                tr("The server options were written, but the server settings record could not be saved. "
                   "Check that the J Launcher data folder is writable, then try again."));
            return;
        }
        updateServerList();
        dialog.accept();
    });
    connect(settingsPage, &ServerSettingsPage::cancelled, &dialog, &QDialog::reject);

    dialog.exec();
}

void ServerListPage::onServerSelectionChanged()
{
    // Disconnect previously selected server
    if (m_currentConnectedServer) {
        disconnect(m_currentConnectedServer.get(), nullptr, this, nullptr);
        m_currentConnectedServer = nullptr;
    }

    int row = ui->serverList->currentRow();
    if (row >= 0) {
        m_selectedServerId = ui->serverList->item(row)->data(Qt::UserRole).toString();

        if (m_serverManager) {
            m_currentConnectedServer = m_serverManager->getServer(m_selectedServerId);
            if (m_currentConnectedServer) {
                // Clear console and load cached log
                ui->consoleOutput->clear();
                ui->consoleOutput->setPlainText(
                    Privacy::sanitizeText(m_currentConnectedServer->consoleLog(),
                                          100000));
                // Scroll to bottom
                ui->consoleOutput->moveCursor(QTextCursor::End);

                // Connect signals
                connect(m_currentConnectedServer.get(), &ServerInstance::outputReceived, this, [this](const QString &line) {
                    appendConsoleOutput(line);
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::errorReceived, this, [this](const QString &line) {
                    appendConsoleOutput("[ERROR] " + line);
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::statusChanged, this, [this]() {
                    updateUI();
                    updateServerList();
                    QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::serverCrashed, this,
                        [this](const QString &message, const QString &details) {
                    if (m_selectedServerId.isEmpty()) return;
                    const auto failedServer = m_currentConnectedServer;
                    const QString failedServerId = m_selectedServerId;
                    QSettings settings;
                    const QString prefix = QString("ServerDiagnostics/%1/").arg(m_selectedServerId);
                    settings.setValue(prefix + "lastCrash", crashSummary(message, details));
                    settings.setValue(prefix + "details", structuredCrashDetails(m_currentConnectedServer, message, details));
                    refreshDiagnostics();
                    QTimer::singleShot(0, this,
                                       [this, failedServer, failedServerId, message, details]() {
                        if (!failedServer || m_selectedServerId != failedServerId) return;
                        showServerFailureDialog(this, failedServer, message, details);
                    });
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::playerActivity, this,
                        [this](const QString &player, bool joined) {
                    if (m_selectedServerId.isEmpty()) return;
                    QSettings settings;
                    const QString key = QString("ServerPlayerHistory/%1/events").arg(m_selectedServerId);
                    QStringList events = settings.value(key).toStringList();
                    events.append(QString("%1 — %2 %3").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"), player,
                        joined ? tr("joined") : tr("left")));
                    while (events.size() > 100) events.removeFirst();
                    settings.setValue(key, events);
                    if (ui->serverTabs->currentWidget() == m_playersTab) refreshPlayerList();
                });
            }
        }
    } else {
        m_selectedServerId.clear();
        ui->consoleOutput->clear();
    }
    updateSelectedServerInfo();
    updateUI();
    QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
    if (m_contentUpdatesTree) {
        m_contentUpdatesTree->clear();
        m_updatesInfoLabel->setText(m_selectedServerId.isEmpty()
            ? tr("Select a server to check for updates.")
            : tr("Check tracked Modrinth and CurseForge content for compatible updates. New modpack servers import mod tracking automatically."));
    }

    for (int i = 0; i < ui->serverList->count(); ++i) {
        QWidget *card = ui->serverList->itemWidget(ui->serverList->item(i));
        if (!card) continue;
        card->setProperty("selected", i == ui->serverList->currentRow());
        card->style()->unpolish(card);
        card->style()->polish(card);
    }
}

void ServerListPage::onServerStatusChanged(ServerInstance *server)
{
    if (server && server->id() == m_selectedServerId) {
        updateUI();
    }
}

void ServerListPage::updateUI()
{
    bool hasSelection = !m_selectedServerId.isEmpty();
    ServerStatus status = ServerStatus::Stopped;

    if (hasSelection && m_serverManager) {
        auto server = m_serverManager->getServer(m_selectedServerId);
        if (server) {
            status = server->status();
        }
    }

    const bool canStart = status == ServerStatus::Stopped || status == ServerStatus::Error;
    const bool canStop = status == ServerStatus::Starting || status == ServerStatus::Running
        || status == ServerStatus::Downloading;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    bool supportsContentBrowser = false;
    if (hasSelection && m_serverManager) {
        const auto server = m_serverManager->getServer(m_selectedServerId);
        if (server) {
            const ServerContentType contentType = server->contentType();
            supportsContentBrowser = contentType != ServerContentType::None;
        }
    }

    ui->startServerButton->setEnabled(hasSelection && canStart);
    ui->stopServerButton->setEnabled(hasSelection && canStop);
    ui->stopServerButton->setText(status == ServerStatus::Downloading ? tr("Cancel Download") : tr("Stop"));
    ui->stopServerButton->setIcon(status == ServerStatus::Downloading
                                      ? launcherIcon("status-bad", QStyle::SP_DialogCancelButton)
                                      : launcherIcon("status-bad", QStyle::SP_MediaStop));
    ui->restartServerButton->setEnabled(
        hasSelection && (status == ServerStatus::Starting || status == ServerStatus::Running));
    ui->startServerButton->setProperty("role", canStart ? "primary" : "secondary");
    ui->stopServerButton->setProperty("role", canStop ? "primary" : "secondary");
    for (QPushButton *button : {ui->startServerButton, ui->stopServerButton}) {
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    const bool canUndoDelete = m_serverManager && m_serverManager->hasDeletedServer();
    m_undoDeleteButton->setEnabled(canUndoDelete);
    m_undoDeleteButton->setVisible(canUndoDelete);
    m_emptyDetailWidget->setVisible(!hasSelection);
    ui->serverCommandBar->setVisible(hasSelection);
    ui->serverWorkspacePanel->setVisible(hasSelection);
    ui->selectedServerInfoLabel->setVisible(hasSelection);
    ui->startServerButton->setVisible(hasSelection);
    ui->stopServerButton->setVisible(hasSelection);
    ui->restartServerButton->setVisible(hasSelection);
    ui->deleteServerButton->setVisible(hasSelection);
    ui->serverTabs->setVisible(hasSelection);
    ui->browseModsButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(ui->backupsTab), hasSelection);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(ui->installedContentTab), supportsContentBrowser);
    if (hasSelection && m_serverManager) {
        const auto server = m_serverManager->getServer(m_selectedServerId);
        const bool pluginServer =
            server && server->contentType() == ServerContentType::Plugin;
        ui->browseModsButton->setText(pluginServer ? tr("Download Plugins") : tr("Download Mods"));
        ui->openContentFolderButton->setText(pluginServer ? tr("Open Plugins Folder") : tr("Open Mods Folder"));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab),
                                   pluginServer ? tr("Plugins") : tr("Mods"));
    } else {
        ui->browseModsButton->setText(tr("Download Mods"));
        ui->openContentFolderButton->setText(tr("Open Mods Folder"));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab), tr("Mods"));
    }
    ui->importPackButton->setEnabled(hasSelection && canEditFiles);
    ui->deleteServerButton->setEnabled(hasSelection && canEditFiles);
    ui->settingsScrollArea->setEnabled(hasSelection && canEditFiles);
    ui->openFolderButton->setEnabled(hasSelection);
    ui->refreshFilesButton->setEnabled(hasSelection);
    ui->refreshOverviewButton->setEnabled(hasSelection);
    const bool hasInstalledSelection = ui->installedContentTree->currentItem()
        && !ui->installedContentTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    ui->refreshInstalledButton->setEnabled(hasSelection && supportsContentBrowser);
    ui->addLocalContentButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    ui->toggleInstalledButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    ui->removeInstalledButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    ui->openContentFolderButton->setEnabled(hasSelection && supportsContentBrowser);
    const bool hasBackupSelection = ui->backupsTree->currentItem()
        && !ui->backupsTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    const bool hasValidBackupSelection = hasBackupSelection
        && ui->backupsTree->currentItem()->data(0, Qt::UserRole + 1).toBool();
    ui->backupNameInput->setEnabled(hasSelection && canEditFiles);
    ui->createBackupButton->setEnabled(hasSelection && canEditFiles);
    ui->refreshBackupsButton->setEnabled(hasSelection);
    ui->openBackupsFolderButton->setEnabled(hasSelection);
    ui->restoreBackupButton->setEnabled(hasValidBackupSelection && canEditFiles);
    ui->removeBackupButton->setEnabled(hasBackupSelection && canEditFiles);
    ui->sendCommandButton->setEnabled(hasSelection && status == ServerStatus::Running);
    ui->commandInput->setEnabled(hasSelection && status == ServerStatus::Running);
    m_exportProfileButton->setEnabled(hasSelection);
    m_importProfileButton->setEnabled(m_serverManager != nullptr);
    m_consoleSearchInput->setEnabled(hasSelection);
    m_findConsoleButton->setEnabled(hasSelection);
    m_copyConsoleErrorsButton->setEnabled(hasSelection);
    if (m_maintenanceTab) {
        ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_maintenanceTab), hasSelection);
        m_updateServerSoftwareButton->setEnabled(hasSelection && canEditFiles);
        m_changeMinecraftVersionButton->setEnabled(hasSelection && canEditFiles);
        bool hasRollbackBackup = false;
        if (hasSelection && m_serverManager) {
            const auto server = m_serverManager->getServer(m_selectedServerId);
            QSettings updateSettings;
            QString backupPath = server ? updateSettings.value(
                QString("ServerUpdates/%1/latestRollbackBackupPath").arg(server->id())).toString() : QString();
            if (backupPath.isEmpty() && server) {
                const QString backupName = updateSettings.value(
                    QString("ServerUpdates/%1/latestRollbackBackup").arg(server->id())).toString();
                if (!backupName.isEmpty()) {
                    backupPath = QDir(QDir(server->serverDirectory()).filePath("backups"))
                                     .filePath(backupName);
                }
            }
            hasRollbackBackup = !backupPath.isEmpty() && QFileInfo(backupPath).isDir();
        }
        m_restoreLatestUpdateBackupButton->setEnabled(hasRollbackBackup && canEditFiles);
        m_checkContentUpdatesButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
        m_setupCurseForgeButton->setVisible(
            !(APPLICATION->capabilities() & Application::SupportsFlame));
        m_setupCurseForgeButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
        const bool hasContentUpdate = m_contentUpdatesTree->currentItem()
            && !m_contentUpdatesTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
        if (m_activeContentUpdater) {
            m_installContentUpdateButton->setText(tr("Cancel Update"));
            m_installContentUpdateButton->setEnabled(true);
        } else {
            m_installContentUpdateButton->setText(tr("Install Selected Update"));
            m_installContentUpdateButton->setEnabled(
                hasContentUpdate && canEditFiles && supportsContentBrowser);
        }
    }
    if (m_playersTab) {
        ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_playersTab), hasSelection);
        m_refreshPlayersButton->setEnabled(hasSelection);
        const bool hasPlayer = m_playersTree->currentItem()
            && !m_playersTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
        const bool canManagePlayerAccess = canEditFiles || status == ServerStatus::Running;
        m_whitelistPlayerButton->setEnabled(hasPlayer && canManagePlayerAccess);
        m_opPlayerButton->setEnabled(hasPlayer && canManagePlayerAccess);
        m_banPlayerButton->setEnabled(hasPlayer && canManagePlayerAccess);
        m_kickPlayerButton->setEnabled(hasPlayer && status == ServerStatus::Running);
        m_removePlayerAccessButton->setEnabled(hasPlayer && canManagePlayerAccess);
        m_viewPlayerHistoryButton->setEnabled(hasSelection);
        m_exportPlayerHistoryButton->setEnabled(hasSelection);
    }
    if (m_automationTab) {
        m_scheduleEnabledCheck->setEnabled(hasSelection);
        m_scheduleActionCombo->setEnabled(hasSelection);
        m_scheduleTimeEdit->setEnabled(hasSelection);
        m_backupRetentionSpin->setEnabled(hasSelection);
        m_cpuWarningSpin->setEnabled(hasSelection && canEditFiles);
        m_ramWarningSpin->setEnabled(hasSelection && canEditFiles);
        m_diskWarningSpin->setEnabled(hasSelection && canEditFiles);
        m_autoRestartCheck->setEnabled(hasSelection && canEditFiles);
        m_saveAutomationButton->setEnabled(hasSelection && canEditFiles);
        m_runAutomationButton->setEnabled(hasSelection);
        m_viewCrashReportButton->setEnabled(hasSelection && !m_diagnosticsLabel->text().startsWith(tr("No crash")));
    }
    syncServerNavigation();
}

void ServerListPage::updateServerList()
{
    // Rebuilding the cards must not temporarily clear the selected server or
    // disturb the page the operator is currently using.
    const QString savedId = m_selectedServerId;
    QWidget *activeServerTab = ui->serverTabs->currentWidget();
    QWidget *activeMaintenanceTab = m_maintenanceTab ? m_maintenanceTab->currentWidget() : nullptr;
    const QSignalBlocker selectionBlocker(ui->serverList);

    ui->serverList->clear();

    if (!m_serverManager) {
        return;
    }

    auto servers = m_serverManager->getAllServers();
    const bool hasServers = !servers.isEmpty();
    m_emptyServerListWidget->setVisible(!hasServers);
    ui->serverList->setVisible(hasServers);
    ui->serverSearchInput->setVisible(hasServers);
    ui->serverListTitle->setVisible(hasServers);
    ui->createFromModpackButton->setVisible(hasServers);
    ui->createServerButton->setVisible(hasServers);

    // With no servers, use the full content width for the single create prompt
    // instead of showing a second empty-state illustration beside it.
    ui->serverDetailPanel->setVisible(hasServers);
    if (hasServers) {
        ui->serverListPanel->setMinimumWidth(264);
        ui->serverListPanel->setMaximumWidth(328);
        ui->serverSplitter->setSizes(QList<int>() << 292 << 888);
    } else {
        ui->serverListPanel->setMinimumWidth(0);
        ui->serverListPanel->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    const QString filter = ui->serverSearchInput->text().trimmed();
    int running = 0;
    int visible = 0;
    int selectRow = -1;
    for (int i = 0; i < servers.size(); i++) {
        auto &server = servers[i];
        if (server->status() == ServerStatus::Running) ++running;
        const QString searchable = QString("%1 %2 %3").arg(server->name(), server->version(), server->loaderType());
        if (!filter.isEmpty() && !searchable.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        QListWidgetItem *item = new QListWidgetItem();
        item->setData(Qt::UserRole, server->id());
        item->setSizeHint(QSize(0, 82));

        auto *card = new QWidget(ui->serverList);
        card->setObjectName(QStringLiteral("serverCard"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(11, 8, 11, 8);
        cardLayout->setSpacing(4);

        auto *topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(7);
        auto *serverIconLabel = new QLabel(card);
        serverIconLabel->setPixmap(serverCardIcon().pixmap(24, 24));
        serverIconLabel->setFixedSize(26, 26);
        serverIconLabel->setAlignment(Qt::AlignCenter);
        serverIconLabel->setToolTip(tr("Minecraft server"));
        auto *nameLabel = new QLabel(server->name(), card);
        QFont nameFont = nameLabel->font();
        nameFont.setBold(true);
        nameFont.setPointSize(nameFont.pointSize() + 1);
        nameLabel->setFont(nameFont);
        auto *statusLabel = new QLabel(getStatusString(static_cast<int>(server->status())), card);
        QColor statusColor;
        switch (server->status()) {
            case ServerStatus::Running: statusColor = QColor("#43a047"); break;
            case ServerStatus::Starting:
            case ServerStatus::Stopping:
            case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
            case ServerStatus::Error: statusColor = QColor("#e53935"); break;
            default: statusColor = QColor("#607d8b"); break;
        }
        auto *statusDot = new QLabel(card);
        statusDot->setFixedSize(8, 8);
        statusDot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(statusColor.name()));
        applyMutedLabelPalette(statusLabel);
        topRow->addWidget(serverIconLabel);
        topRow->addWidget(nameLabel);
        topRow->addStretch();
        topRow->addWidget(statusDot);
        topRow->addWidget(statusLabel);
        cardLayout->addLayout(topRow);

        const QString loader = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
        auto *detailsLabel = new QLabel(tr("%1  •  %2  •  Port %3").arg(loader, server->version()).arg(server->port()), card);
        applyMutedLabelPalette(detailsLabel);
        detailsLabel->setContentsMargins(33, 0, 0, 0);
        cardLayout->addWidget(detailsLabel);

        ui->serverList->addItem(item);
        ui->serverList->setItemWidget(item, card);

        if (server->id() == savedId) {
            selectRow = visible;
        }
        ++visible;
    }

    if (auto *emptyTitle = m_emptyServerListWidget->findChild<QLabel *>(QStringLiteral("emptyServerListTitle"))) {
        emptyTitle->setText(hasServers ? tr("No matching servers") : tr("No servers yet"));
    }
    if (auto *emptyHint = m_emptyServerListWidget->findChild<QLabel *>(QStringLiteral("emptyServerListHint"))) {
        emptyHint->setText(hasServers ? tr("Try a different name, version, or server type.")
                                      : tr("Create a server to manage it from the launcher."));
    }
    if (auto *emptyCreate = m_emptyServerListWidget->findChild<QPushButton *>(QStringLiteral("emptyCreateServerButton"))) {
        emptyCreate->setVisible(!hasServers);
    }
    const bool hasVisibleServers = visible > 0;
    m_emptyServerListWidget->setVisible(!hasVisibleServers);
    ui->serverList->setVisible(hasVisibleServers);

    const bool savedServerExists = !savedId.isEmpty() && m_serverManager->getServer(savedId);
    const bool selectedServerHidden = savedServerExists && selectRow < 0;
    const QString serverCount = servers.size() == 1
        ? tr("1 server")
        : tr("%1 servers").arg(servers.size());
    QString statsText = filter.isEmpty()
        ? tr("%1 • %2 running").arg(serverCount).arg(running)
        : tr("%1 of %2 shown • %3 running").arg(visible).arg(serverCount).arg(running);
    if (selectedServerHidden) {
        statsText += tr(" • selected server hidden by filter");
    }
    ui->serverStatsLabel->setText(statsText);
    ui->serverSearchInput->setToolTip(selectedServerHidden
        ? tr("The selected server remains active in the workspace but is hidden by this filter.")
        : tr("Filter servers by name, version, or server type."));

    // Restore selection. On first open, select the first visible server so the
    // dashboard immediately has useful information instead of an empty state.
    if (selectRow >= 0) {
        ui->serverList->setCurrentRow(selectRow);
    } else if ((!savedServerExists || savedId.isEmpty()) && visible > 0) {
        ui->serverList->setCurrentRow(0);
    } else {
        ui->serverList->setCurrentRow(-1);
    }

    const QString finalId = ui->serverList->currentItem()
        ? ui->serverList->currentItem()->data(Qt::UserRole).toString() : QString();
    if (!selectedServerHidden
        && (finalId != m_selectedServerId
            || (!finalId.isEmpty() && !m_currentConnectedServer))) {
        onServerSelectionChanged();
    } else {
        for (int index = 0; index < ui->serverList->count(); ++index) {
            QWidget *card = ui->serverList->itemWidget(ui->serverList->item(index));
            if (!card) continue;
            card->setProperty("selected", index == ui->serverList->currentRow());
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
    }

    if (activeServerTab && ui->serverTabs->indexOf(activeServerTab) >= 0
        && ui->serverTabs->isTabEnabled(ui->serverTabs->indexOf(activeServerTab))) {
        ui->serverTabs->setCurrentWidget(activeServerTab);
    }
    if (activeMaintenanceTab && m_maintenanceTab
        && m_maintenanceTab->indexOf(activeMaintenanceTab) >= 0) {
        m_maintenanceTab->setCurrentWidget(activeMaintenanceTab);
    }
}

void ServerListPage::refreshCurrentServerTab()
{
    QWidget *page = ui->serverTabs->currentWidget();
    if (page == ui->overviewTab) {
        refreshOverview();
        refreshLiveStatistics();
    } else if (page == ui->installedContentTab) {
        refreshInstalledContent();
    } else if (page == m_playersTab) {
        refreshPlayerList();
    } else if (page == ui->filesTab) {
        refreshServerFiles();
    } else if (page == ui->backupsTab) {
        refreshServerBackups();
    } else if (page == ui->settingsTab) {
        rebuildSettingsPage();
    } else if (page == m_maintenanceTab && m_maintenanceTab->currentWidget() == m_automationTab) {
        refreshAutomation();
        refreshDiagnostics();
    }
}

void ServerListPage::refreshOverview()
{
    const quint64 generation = ++m_overviewRefreshGeneration;
    const auto setEmpty = [this]() {
        ui->overviewStatusValue->setText("-");
        ui->overviewStatusValue->setStyleSheet({});
        ui->overviewMemoryValue->setText("-");
        ui->overviewContentValue->setText("-");
        if (m_overviewSummaryLabel) {
            m_overviewSummaryLabel->setText(tr("No server data available."));
            m_overviewSummaryLabel->setToolTip({});
        }
    };

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        setEmpty();
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        setEmpty();
        return;
    }

    const ServerContentType contentType = server->contentType();
    const bool pluginServer = contentType == ServerContentType::Plugin;
    const bool supportsContent = contentType != ServerContentType::None;
    const QString contentDirectory = server->contentDirectory();
    const int contentCount = supportsContent
        ? QDir(contentDirectory).entryInfoList(QStringList() << "*.jar" << "*.jar.disabled", QDir::Files).size()
        : 0;

    const QDir serverDirectory(server->serverDirectory());
    const QFileInfoList worldDirectories = serverDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    int worldCount = 0;
    QStringList worldPaths;
    for (const QFileInfo &directory : worldDirectories) {
        if (directory.fileName().startsWith("world", Qt::CaseInsensitive)) {
            ++worldCount;
            worldPaths.append(directory.absoluteFilePath());
        }
    }

    const QDir backupsDirectory(serverDirectory.filePath("backups"));
    int backupCount = 0;
    for (const QFileInfo &backup : backupsDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (backup.fileName().startsWith("server-", Qt::CaseInsensitive)) ++backupCount;
    }

    const QString status = getStatusString(static_cast<int>(server->status()));
    QColor statusColor;
    switch (server->status()) {
        case ServerStatus::Running: statusColor = QColor("#43a047"); break;
        case ServerStatus::Starting:
        case ServerStatus::Stopping:
        case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
        case ServerStatus::Error: statusColor = QColor("#e53935"); break;
        default: statusColor = QColor("#607d8b"); break;
    }

    ui->overviewStatusValue->setText(status);
    ui->overviewStatusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));
    ui->overviewMemoryValue->setText(server->status() == ServerStatus::Running ? tr("Starting...") : tr("Not running"));
    ui->overviewContentValue->setText(tr("Calculating..."));

    const QString serverType = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
    const QString contentSummary = supportsContent
        ? tr("%1 %2").arg(contentCount).arg(pluginServer ? tr("plugins") : tr("mods"))
        : tr("Not applicable");
    if (m_overviewSummaryLabel) {
        m_overviewSummaryLabel->setText(
            tr("<b>Version:</b> %1 &nbsp;&bull;&nbsp; <b>Type:</b> %2 &nbsp;&bull;&nbsp; <b>Port:</b> %3<br>"
               "<b>Content:</b> %4 &nbsp;&bull;&nbsp; <b>Backups:</b> %5 &nbsp;&bull;&nbsp; <b>Worlds:</b> %6 (%7)")
                .arg(server->version().toHtmlEscaped(), serverType.toHtmlEscaped())
                .arg(server->port())
                .arg(contentSummary.toHtmlEscaped())
                .arg(backupCount)
                .arg(worldCount)
                .arg(tr("Calculating...").toHtmlEscaped()));
        const QString java = server->javaPath().isEmpty()
            ? tr("Automatic")
            : Privacy::sanitizePath(server->javaPath());
        m_overviewSummaryLabel->setToolTip(
            tr("Server folder: %1\nJava: %2\nConfigured memory: %3-%4 MB")
                .arg(Privacy::sanitizePath(server->serverDirectory()), java)
                .arg(server->minMemory())
                .arg(server->maxMemory()));
    }

    const QString selectedServerId = server->id();
    const QString serverDirectoryPath = server->serverDirectory();
    auto *watcher = new QFutureWatcher<QPair<qint64, qint64>>(this);
    connect(watcher, &QFutureWatcher<QPair<qint64, qint64>>::finished, this,
            [this, watcher, generation, selectedServerId, serverType, contentSummary,
             backupCount, worldCount]() {
        const auto sizes = watcher->result();
        watcher->deleteLater();
        if (generation != m_overviewRefreshGeneration || selectedServerId != m_selectedServerId) return;
        if (!m_serverManager) return;
        const auto currentServer = m_serverManager->getServer(selectedServerId);
        if (!currentServer) return;
        ui->overviewContentValue->setText(formatByteSize(sizes.first));
        if (m_overviewSummaryLabel) {
            m_overviewSummaryLabel->setText(
                tr("<b>Version:</b> %1 &nbsp;&bull;&nbsp; <b>Type:</b> %2 &nbsp;&bull;&nbsp; <b>Port:</b> %3<br>"
                   "<b>Content:</b> %4 &nbsp;&bull;&nbsp; <b>Backups:</b> %5 &nbsp;&bull;&nbsp; <b>Worlds:</b> %6 (%7)")
                    .arg(currentServer->version().toHtmlEscaped(), serverType.toHtmlEscaped())
                    .arg(currentServer->port())
                    .arg(contentSummary.toHtmlEscaped())
                    .arg(backupCount)
                    .arg(worldCount)
                    .arg(formatByteSize(sizes.second).toHtmlEscaped()));
        }
    });
    watcher->setFuture(QtConcurrent::run([serverDirectoryPath, worldPaths]() {
        qint64 worldSize = 0;
        for (const QString &worldPath : worldPaths) worldSize += ServerListPage::directorySize(worldPath);
        return qMakePair(ServerListPage::directorySize(serverDirectoryPath), worldSize);
    }));
}

void ServerListPage::refreshLiveStatistics()
{
    const auto setInactive = [this](const QString &text) {
        if (!m_overviewCpuBar || !m_overviewRamBar || !m_overviewUpdatedLabel) return;
        m_overviewCpuBar->setValue(0);
        m_overviewCpuBar->setFormat(text);
        m_overviewRamBar->setValue(0);
        m_overviewRamBar->setFormat(text);
        m_overviewUpdatedLabel->setText(text);
    };

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->overviewMemoryValue->setText("-");
        setInactive(tr("No server selected"));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        ui->overviewMemoryValue->setText("-");
        setInactive(tr("Server unavailable"));
        return;
    }

    const QString status = getStatusString(static_cast<int>(server->status()));
    QColor statusColor;
    switch (server->status()) {
        case ServerStatus::Running: statusColor = QColor("#43a047"); break;
        case ServerStatus::Starting:
        case ServerStatus::Stopping:
        case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
        case ServerStatus::Error: statusColor = QColor("#e53935"); break;
        default: statusColor = QColor("#607d8b"); break;
    }
    ui->overviewStatusValue->setText(status);
    ui->overviewStatusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));

    if (server->status() != ServerStatus::Running || server->processId() <= 0) {
        ui->overviewMemoryValue->setText(tr("Not running"));
        m_liveStatisticsPid = 0;
        m_previousProcessCpuMs = 0;
        m_previousSampleTimeMs = 0;
        setInactive(tr("Not running"));
        return;
    }

    const QDateTime startedAt = server->startedAt();
    if (startedAt.isValid()) {
        const qint64 seconds = startedAt.secsTo(QDateTime::currentDateTime());
        const qint64 hours = seconds / 3600;
        const qint64 minutes = (seconds % 3600) / 60;
        ui->overviewMemoryValue->setText(hours > 0 ? tr("%1h %2m").arg(hours).arg(minutes) : tr("%1m").arg(minutes));
    } else {
        ui->overviewMemoryValue->setText(tr("This session"));
    }

#ifdef Q_OS_WIN
    ProcessSnapshot snapshot;
    const qint64 processId = server->processId();
    if (!readProcessSnapshot(processId, &snapshot)) {
        ServerHealthInput input;
        input.running = true;
        const ServerHealthAssessment assessment = ServerDiagnostics::assessHealth(input);
        setInactive(assessment.state == ServerHealthState::Unavailable
                        ? tr("Process data unavailable") : tr("Not running"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double cpu = -1.0;
    if (m_liveStatisticsPid == processId && m_previousSampleTimeMs > 0 && now > m_previousSampleTimeMs) {
        const int cores = qMax(1, QThread::idealThreadCount());
        cpu = (snapshot.cpuMilliseconds - m_previousProcessCpuMs) * 100.0
            / (now - m_previousSampleTimeMs) / cores;
    }
    const auto setBarColor = [](QProgressBar *bar, const QColor &color) {
        bar->setStyleSheet(QString("QProgressBar { text-align: center; } QProgressBar::chunk { background: %1; }")
            .arg(color.name()));
    };
    if (m_overviewCpuBar && m_overviewRamBar && m_overviewUpdatedLabel) {
        QSettings monitoringSettings;
        const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
        const int cpuWarning = monitoringSettings.value(monitoringPrefix + "cpuWarning", 85).toInt();
        const int ramWarning = monitoringSettings.value(monitoringPrefix + "ramWarning", 90).toInt();
        const int diskWarningGb = monitoringSettings.value(monitoringPrefix + "diskWarningGb", 2).toInt();
        const QStorageInfo storage(server->serverDirectory());
        ServerHealthInput healthInput;
        healthInput.running = true;
        healthInput.processMetricsAvailable = true;
        healthInput.cpuPercent = cpu;
        healthInput.workingSetBytes = snapshot.workingSetBytes;
        healthInput.configuredRamMiB = qMax(1, server->maxMemory());
        healthInput.diskAvailable = storage.isValid();
        healthInput.diskBytesAvailable = storage.bytesAvailable();
        healthInput.cpuWarningPercent = cpuWarning;
        healthInput.ramWarningPercent = ramWarning;
        healthInput.diskWarningGiB = diskWarningGb;
        const ServerHealthAssessment health = ServerDiagnostics::assessHealth(healthInput);
        const auto metricColor = [](ServerMetricLevel level) {
            switch (level) {
                case ServerMetricLevel::Warning: return QColor("#e53935");
                case ServerMetricLevel::Approaching: return QColor("#f9a825");
                case ServerMetricLevel::Normal: return QColor("#43a047");
                case ServerMetricLevel::Unavailable: return QColor("#607d8b");
            }
            return QColor("#607d8b");
        };
        const double visibleCpu = qMax(0.0, cpu);
        m_overviewCpuBar->setRange(0, 100);
        m_overviewCpuBar->setValue(qBound(0, qRound(visibleCpu), 100));
        m_overviewCpuBar->setFormat(cpu < 0.0 ? tr("Measuring...") : QString::number(visibleCpu, 'f', 1) + "%");
        setBarColor(m_overviewCpuBar, metricColor(health.cpu));

        const int ramMegabytes = qMax(0, qRound(snapshot.workingSetBytes / (1024.0 * 1024.0)));
        const int configuredRam = healthInput.configuredRamMiB;
        m_overviewRamBar->setRange(0, qMax(configuredRam, ramMegabytes));
        m_overviewRamBar->setValue(ramMegabytes);
        m_overviewRamBar->setFormat(tr("%1 MB / %2 MB configured").arg(ramMegabytes).arg(configuredRam));
        setBarColor(m_overviewRamBar, metricColor(health.ram));
        QStringList warnings;
        if (health.cpu == ServerMetricLevel::Warning)
            warnings << tr("CPU %1%").arg(cpu, 0, 'f', 1);
        if (health.ram == ServerMetricLevel::Warning)
            warnings << tr("RAM %1%").arg(health.ramPercent, 0, 'f', 1);
        if (health.disk == ServerMetricLevel::Warning) {
            warnings << tr("Disk %1 GB free").arg(storage.bytesAvailable() / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
        }
        if (health.state == ServerHealthState::Warning) {
            m_overviewUpdatedLabel->setText(tr("Warning: %1").arg(warnings.join(", ")));
            m_overviewUpdatedLabel->setStyleSheet("color: #e53935; font-weight: 600;");
        } else if (health.state == ServerHealthState::Measuring) {
            m_overviewUpdatedLabel->setText(tr("Measuring live process usage..."));
            m_overviewUpdatedLabel->setStyleSheet("color: palette(mid);");
        } else {
            m_overviewUpdatedLabel->setText(tr("Updated %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
            m_overviewUpdatedLabel->setStyleSheet("color: palette(mid);");
        }
    }
    m_liveStatisticsPid = processId;
    m_previousProcessCpuMs = snapshot.cpuMilliseconds;
    m_previousSampleTimeMs = now;
#else
    setInactive(tr("Live process metrics are available on Windows."));
#endif
}

void ServerListPage::refreshInstalledContent()
{
    ui->installedContentTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->installedContentInfoLabel->setText(tr("Select a modded or plugin server to view installed content."));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab), tr("Mods"));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    const QString loader = server->loaderType().toLower();
    const bool pluginServer = loader == "paper" || loader == "purpur";
    const bool supportsContent = pluginServer || loader == "fabric" || loader == "forge" || loader == "neoforge";
    const QString contentLabel = pluginServer ? tr("Plugins") : tr("Mods");
    ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab),
                               supportsContent ? contentLabel : tr("Mods"));

    if (!supportsContent) {
        ui->installedContentInfoLabel->setText(tr("This server type does not use launcher-managed mods or plugins."));
        return;
    }

    const QString directoryPath = server->contentDirectory();
    const QDir directory(directoryPath);
    const QFileInfoList files = directory.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled",
                                                         QDir::Files, QDir::Name | QDir::IgnoreCase);
    ui->installedContentInfoLabel->setText(tr("Installed %1 for %2 (%3).")
        .arg(contentLabel.toLower(), server->name().toHtmlEscaped(), directoryPath.toHtmlEscaped()));

    if (files.isEmpty()) {
        auto *emptyItem = new QTreeWidgetItem({tr("No %1 installed yet.").arg(contentLabel.toLower()), QString(), QString(), QString()});
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsSelectable);
        emptyItem->setForeground(0, palette().brush(QPalette::Mid));
        ui->installedContentTree->addTopLevelItem(emptyItem);
        return;
    }

    for (const QFileInfo &file : files) {
        const QStringList details = installedContentDetails(file);
        auto *item = new QTreeWidgetItem({details.at(0), details.at(1), details.at(2),
                                           QLocale().formattedDataSize(file.size())});
        item->setData(0, Qt::UserRole, file.absoluteFilePath());
        item->setIcon(0, launcherIcon("loadermods", QStyle::SP_FileIcon));
        item->setToolTip(0, file.fileName());
        if (details.at(2) == tr("Disabled")) {
            for (int column = 0; column < item->columnCount(); ++column) {
                item->setForeground(column, palette().brush(QPalette::Mid));
            }
        }
        ui->installedContentTree->addTopLevelItem(item);
    }
}

void ServerListPage::refreshPlayerList()
{
    if (!m_playersTree || !m_playersInfoLabel) return;
    const QString selectedUuid = m_playersTree->currentItem()
        ? m_playersTree->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const QSignalBlocker selectionBlocker(m_playersTree);
    m_playersTree->clear();
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        m_playersInfoLabel->setText(tr("Select a server to view known players and manage access."));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    QString error;
    const QList<ServerPlayerInfo> players = ServerPlayerAccess::listPlayers(server, &error);
    if (!error.isEmpty()) {
        m_playersInfoLabel->setText(tr("Could not read player access files: %1").arg(error));
        updateUI();
        return;
    }

    for (const ServerPlayerInfo &player : players) {
        auto *item = new QTreeWidgetItem(m_playersTree);
        item->setText(0, player.name.isEmpty() ? tr("Unknown player") : player.name);
        item->setText(1, player.uuid);
        item->setText(2, player.whitelisted ? tr("Yes") : tr("No"));
        item->setText(3, player.operatorEnabled
                         ? tr("Yes (level %1)").arg(player.operatorLevel) : tr("No"));
        item->setText(4, player.banned ? tr("Yes") : tr("No"));
        item->setData(0, Qt::UserRole, player.uuid);
        if (!selectedUuid.isEmpty() && player.uuid == selectedUuid) {
            m_playersTree->setCurrentItem(item);
        }
    }
    if (!m_playersTree->currentItem() && m_playersTree->topLevelItemCount() > 0) {
        m_playersTree->setCurrentItem(m_playersTree->topLevelItem(0));
    }
    m_playersTree->resizeColumnToContents(0);
    m_playersTree->resizeColumnToContents(2);
    m_playersTree->resizeColumnToContents(3);
    m_playersTree->resizeColumnToContents(4);
    const QStringList history = QSettings().value(QString("ServerPlayerHistory/%1/events").arg(server->id())).toStringList();
    const QString activity = history.isEmpty() ? QString() : tr(" Latest activity: %1").arg(history.last());
    const QString managementMode = server->status() == ServerStatus::Running
        ? tr(" Live actions are sent through the server console; operator level uses server.properties.")
        : tr(" Access files can be edited safely while the server is stopped.");
    m_playersInfoLabel->setText((players.isEmpty()
        ? tr("No known players yet. Players appear after they connect, or when listed in whitelist, ops, or bans.")
        : tr("%1 known player(s).").arg(players.size())) + managementMode + activity);
    updateUI();
}

void ServerListPage::refreshServerBackups()
{
    ui->backupsTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->backupsInfoLabel->setText(tr("Select a server to view its backups."));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    const auto sizeText = [](qint64 bytes) {
        if (bytes < 1024) return QString::number(bytes) + " B";
        if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    };

    int count = 0;
    for (const ServerBackupInfo &backup : m_serverManager->listServerBackups(m_selectedServerId)) {
        auto *item = new QTreeWidgetItem(ui->backupsTree);
        item->setText(0, backup.name);
        item->setText(1, backup.createdAt.isValid()
                             ? backup.createdAt.toLocalTime().toString("yyyy-MM-dd HH:mm")
                             : tr("Invalid"));
        item->setText(2, sizeText(backup.size));
        item->setData(0, Qt::UserRole, backup.path);
        item->setData(0, Qt::UserRole + 1, backup.valid);
        item->setToolTip(
            0, backup.valid
                   ? tr("Includes: %1").arg(backup.includedCategories.join(", "))
                   : backup.validationError);
        if (!backup.valid) {
            item->setForeground(0, palette().brush(QPalette::Mid));
        }
        ++count;
    }
    ui->backupsTree->resizeColumnToContents(1);
    ui->backupsTree->resizeColumnToContents(2);
    ui->backupsInfoLabel->setText(tr("%1 complete server backup(s) saved for %2. Enter a name to create a new backup.")
        .arg(count).arg(server->name()));
}

bool ServerListPage::copyDirectory(const QString &source, const QString &destination, QString *error,
                                   const QString &excludedTopLevel)
{
    if (!QDir().mkpath(destination)) {
        if (error) *error = tr("Could not create the backup folder.");
        return false;
    }
    const QDir sourceDir(source);
    if (!sourceDir.exists()) return true;

    const QFileInfoList entries = sourceDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        if (!excludedTopLevel.isEmpty() && entry.fileName().compare(excludedTopLevel, Qt::CaseInsensitive) == 0) continue;
        const QString targetPath = QDir(destination).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectory(entry.absoluteFilePath(), targetPath, error)) {
                return false;
            }
        } else if (!QFile::copy(entry.absoluteFilePath(), targetPath)) {
            if (error) *error = tr("Could not copy %1.").arg(entry.fileName());
            return false;
        }
    }
    return true;
}

qint64 ServerListPage::directorySize(const QString &directory)
{
    qint64 total = 0;
    QDirIterator iterator(directory, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

void ServerListPage::rebuildSettingsPage()
{
    while (QLayoutItem *layoutItem = ui->settingsContentLayout->takeAt(0)) {
        delete layoutItem->widget();
        delete layoutItem;
    }
    m_embeddedSettingsPage = nullptr;

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        auto *placeholder = new QLabel(tr("Select a stopped server to edit its settings."), ui->settingsContentContainer);
        placeholder->setAlignment(Qt::AlignCenter);
        ui->settingsContentLayout->addWidget(placeholder);
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    auto *settingsPage = new ServerSettingsPage(ui->settingsContentContainer);
    m_embeddedSettingsPage = settingsPage;
    settingsPage->setServer(server);
    ui->settingsContentLayout->addWidget(settingsPage);
    protectInputFromWheel(settingsPage);
    for (QPushButton *button : settingsPage->findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    connect(settingsPage, &ServerSettingsPage::settingsSaved, this, [this]() {
        if (m_serverManager && !m_serverManager->save()) {
            QMessageBox::warning(
                this,
                tr("Could Not Save Server Settings"),
                tr("The server options were written, but the server settings record could not be saved. "
                   "Check that the J Launcher data folder is writable, then try again."));
            return;
        }

        // The card displays the name and port from the saved server record.
        // Refresh it as well as the detail header, while updateServerList()
        // preserves the selected server and active tab.
        updateServerList();
        updateSelectedServerInfo();
    });
    connect(settingsPage, &ServerSettingsPage::cancelled, this, [settingsPage, server]() {
        settingsPage->setServer(server);
    });
}

void ServerListPage::refreshServerFiles()
{
    ui->serverFilesTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->filesInfoLabel->setText(tr("Select a server to browse its files."));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    const QString directoryPath = server->serverDirectory();
    const QFileInfo rootInfo(directoryPath);
    ui->filesInfoLabel->setText(tr("Server files: %1").arg(directoryPath));
    if (!rootInfo.isDir()) {
        ui->filesInfoLabel->setText(tr("The server folder does not exist yet: %1").arg(directoryPath));
        return;
    }

    auto *rootItem = new QTreeWidgetItem(ui->serverFilesTree);
    rootItem->setText(0, server->name());
    rootItem->setText(1, tr("Server folder"));
    rootItem->setData(0, Qt::UserRole, directoryPath);
    rootItem->setIcon(0, serverCardIcon());
    rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    populateServerFileItem(rootItem);
    rootItem->setExpanded(true);
    ui->serverFilesTree->resizeColumnToContents(1);
    ui->serverFilesTree->resizeColumnToContents(2);
}

void ServerListPage::populateServerFileItem(QTreeWidgetItem *item)
{
    if (!item || item->data(0, Qt::UserRole + 1).toBool()) {
        return;
    }

    const QFileInfo parentInfo(item->data(0, Qt::UserRole).toString());
    if (!parentInfo.isDir()) {
        return;
    }

    const QDir directory(parentInfo.absoluteFilePath());
    const QFileInfoList entries = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    const auto sizeText = [](qint64 bytes) {
        if (bytes < 1024) return QString::number(bytes) + " B";
        if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
        if (bytes < 1024ll * 1024 * 1024) return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
    };

    for (const QFileInfo &entry : entries) {
        auto *child = new QTreeWidgetItem(item);
        child->setText(0, entry.fileName());
        child->setText(1, entry.isDir() ? tr("Folder") : tr("File"));
        child->setText(2, entry.isDir() ? QString() : sizeText(entry.size()));
        child->setData(0, Qt::UserRole, entry.absoluteFilePath());
        child->setIcon(0, serverFileIcon(entry));
        if (entry.isDir()) {
            child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
    }
    item->setData(0, Qt::UserRole + 1, true);
}

void ServerListPage::updateSelectedServerInfo()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->detailTitleLabel->setText(tr("Select a server"));
        ui->selectedServerInfoLabel->setText(tr("Select a server to view its details and controls."));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        ui->detailTitleLabel->setText(tr("Select a server"));
        ui->selectedServerInfoLabel->setText(tr("Select a server to view its details and controls."));
        return;
    }
    QString type = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
    if (!type.isEmpty()) type[0] = type[0].toUpper();
    const auto memoryText = [](int memoryMiB) {
        return memoryMiB % 1024 == 0
            ? QObject::tr("%1 GB").arg(memoryMiB / 1024)
            : QObject::tr("%1 MB").arg(memoryMiB);
    };
    ui->detailTitleLabel->setText(server->name());
    ui->selectedServerInfoLabel->setText(tr("%1 %2  •  Port %3  •  %4–%5  •  <b>%6</b>")
        .arg(type.toHtmlEscaped(), server->version().toHtmlEscaped()).arg(server->port())
        .arg(memoryText(server->minMemory()), memoryText(server->maxMemory()))
        .arg(getStatusString(static_cast<int>(server->status()))));
}

void ServerListPage::appendConsoleOutput(const QString &text)
{
    ui->consoleOutput->appendPlainText(Privacy::sanitizeText(text, 8192));
    if (!m_pauseConsoleScrollCheck || !m_pauseConsoleScrollCheck->isChecked()) {
        QScrollBar *scrollBar = ui->consoleOutput->verticalScrollBar();
        scrollBar->setValue(scrollBar->maximum());
    }
}

QString ServerListPage::getStatusString(int status) const
{
    switch (static_cast<ServerStatus>(status)) {
        case ServerStatus::Stopped: return tr("Stopped");
        case ServerStatus::Starting: return tr("Starting");
        case ServerStatus::Running: return tr("Running");
        case ServerStatus::Stopping: return tr("Stopping");
        case ServerStatus::Error: return tr("Error");
        case ServerStatus::Downloading: return tr("Downloading");
        default: return tr("Unknown");
    }
}
