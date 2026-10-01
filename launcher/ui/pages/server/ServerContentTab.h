// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerInstance;
class ServerManager;
class QLabel;
class QPushButton;
class QTreeWidget;

/*! The Server Manager's "Mods" (or "Plugins") tab: installed content, downloads from the
 *  launcher's catalogs, local jars, and help finding mods a modpack server is missing.
 */
class ServerContentTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerContentTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads the installed content list.
    void refresh();
    /// Enables and names the actions for the server's type and state.
    void updateActions();
    /// Opens the catalog download dialog, optionally already searching for initialSearch.
    void browseContent(const QString &initialSearch = QString());
    /// Opens the catalog download dialog searching for the first of these mod ids.
    void findMissingMods(const QStringList &missingIds);
    /// Mods that a modpack server made by the launcher still needs; empty when complete.
    QStringList missingDependencies(ServerInstance *server) const;
    /// Explains the missing mods and offers to search for them.
    void offerDependencyRepair(const QStringList &missingIds, const QString &introduction);

signals:
    /// A line for the page's console view.
    void consoleMessage(const QString &text);
    /// Files were added, removed, enabled or disabled.
    void contentChanged();
    /// The tab's title should read this ("Mods" or "Plugins").
    void contentLabelChanged(const QString &label);

private:
    void addLocalContent();
    void toggleSelectedContent();
    void removeSelectedContent();
    void openContentFolder();

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_infoLabel = nullptr;
    QTreeWidget *m_contentTree = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_addLocalButton = nullptr;
    QPushButton *m_openFolderButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QPushButton *m_removeButton = nullptr;
};
