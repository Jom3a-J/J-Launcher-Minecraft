// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerManager;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

/// The Server Manager's "Console" tab: live server output, search, export, and commands.
class ServerConsoleTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerConsoleTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server whose console this is; empty for none.
    void setServerId(const QString &serverId);

    /// Replaces the view with a server's stored log.
    void showLog(const QString &log);
    void clear();
    /// Adds a line (redacted), scrolling to it unless auto-scroll is paused.
    void appendLine(const QString &text);
    /// Enables the controls that apply to the server's state.
    void updateActions();

private:
    void findNext();
    void copyErrors();
    void exportLog();
    void sendCommand();

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QPlainTextEdit *m_output = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QPushButton *m_findButton = nullptr;
    QPushButton *m_copyErrorsButton = nullptr;
    QCheckBox *m_pauseScrollCheck = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QLineEdit *m_commandInput = nullptr;
    QPushButton *m_sendButton = nullptr;
};
