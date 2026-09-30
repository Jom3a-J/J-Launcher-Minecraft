// SPDX-License-Identifier: GPL-3.0-only

#include "ServerConsoleTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

#include "logs/Privacy.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "ServerPageStyle.h"

ServerConsoleTab::ServerConsoleTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("consoleTab"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(18, 16, 18, 18);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(tr("Live output"), this);
    title->setObjectName("consoleTitleLabel");
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *hint = new QLabel(tr("Server output and commands"), this);
    hint->setObjectName("consoleHintLabel");
    header->addWidget(title);
    header->addWidget(hint);
    header->addStretch();
    layout->addLayout(header);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("consoleOutput"));
    m_output->setReadOnly(true);
    QFont outputFont(QStringLiteral("Courier New"));
    outputFont.setPointSize(9);
    m_output->setFont(outputFont);
    m_output->document()->setMaximumBlockCount(5000);
    layout->addWidget(m_output);

    auto *tools = new QHBoxLayout();
    m_searchInput = new QLineEdit(this);
    m_searchInput->setObjectName("consoleSearchInput");
    m_searchInput->setPlaceholderText(tr("Search console output…"));
    m_findButton = new QPushButton(tr("Find Next"), this);
    m_findButton->setObjectName("findConsoleButton");
    m_copyErrorsButton = new QPushButton(tr("Copy Errors"), this);
    m_copyErrorsButton->setObjectName("copyConsoleErrorsButton");
    m_pauseScrollCheck = new QCheckBox(tr("Pause auto-scroll"), this);
    m_clearButton = new QPushButton(tr("Clear View"), this);
    m_exportButton = new QPushButton(tr("Export Log"), this);
    tools->addWidget(m_searchInput, 1);
    tools->addWidget(m_findButton);
    tools->addWidget(m_copyErrorsButton);
    tools->addWidget(m_pauseScrollCheck);
    tools->addWidget(m_clearButton);
    tools->addWidget(m_exportButton);
    layout->addLayout(tools);

    auto *command = new QHBoxLayout();
    command->setSpacing(8);
    m_commandInput = new QLineEdit(this);
    m_commandInput->setObjectName(QStringLiteral("commandInput"));
    m_commandInput->setMinimumHeight(36);
    m_commandInput->setPlaceholderText(tr("Enter a server command…"));
    m_sendButton = new QPushButton(tr("Send"), this);
    m_sendButton->setObjectName(QStringLiteral("sendCommandButton"));
    m_sendButton->setMinimumSize(76, 36);
    m_sendButton->setEnabled(false);
    m_sendButton->setIcon(ServerPageStyle::launcherIcon("launch", QStyle::SP_ArrowForward));
    m_sendButton->setProperty("role", "primary");
    command->addWidget(m_commandInput);
    command->addWidget(m_sendButton);
    layout->addLayout(command);

    connect(m_findButton, &QPushButton::clicked, this, &ServerConsoleTab::findNext);
    connect(m_searchInput, &QLineEdit::returnPressed, this, &ServerConsoleTab::findNext);
    connect(m_copyErrorsButton, &QPushButton::clicked, this, &ServerConsoleTab::copyErrors);
    connect(m_clearButton, &QPushButton::clicked, m_output, &QPlainTextEdit::clear);
    connect(m_exportButton, &QPushButton::clicked, this, &ServerConsoleTab::exportLog);
    connect(m_sendButton, &QPushButton::clicked, this, &ServerConsoleTab::sendCommand);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &ServerConsoleTab::sendCommand);
}

void ServerConsoleTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerConsoleTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerConsoleTab::showLog(const QString &log)
{
    m_output->clear();
    m_output->setPlainText(Privacy::sanitizeText(log, 100000));
    m_output->moveCursor(QTextCursor::End);
}

void ServerConsoleTab::clear()
{
    m_output->clear();
}

void ServerConsoleTab::appendLine(const QString &text)
{
    m_output->appendPlainText(Privacy::sanitizeText(text, 8192));
    if (!m_pauseScrollCheck->isChecked()) {
        QScrollBar *scrollBar = m_output->verticalScrollBar();
        scrollBar->setValue(scrollBar->maximum());
    }
}

void ServerConsoleTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = !m_serverId.isEmpty();
    const bool running = server && server->status() == ServerStatus::Running;
    m_sendButton->setEnabled(hasSelection && running);
    m_commandInput->setEnabled(hasSelection && running);
    m_searchInput->setEnabled(hasSelection);
    m_findButton->setEnabled(hasSelection);
    m_copyErrorsButton->setEnabled(hasSelection);
}

void ServerConsoleTab::findNext()
{
    const QString term = m_searchInput->text().trimmed();
    if (term.isEmpty()) return;
    if (!m_output->find(term)) {
        m_output->moveCursor(QTextCursor::Start);
        m_output->find(term);
    }
}

void ServerConsoleTab::copyErrors()
{
    QStringList errors;
    for (const QString &line : m_output->toPlainText().split('\n')) {
        if (line.contains("[ERROR]", Qt::CaseInsensitive) || line.contains("exception", Qt::CaseInsensitive) || line.contains("[CRASH]", Qt::CaseInsensitive)) errors.append(line);
    }
    QApplication::clipboard()->setText(errors.isEmpty() ? tr("No errors found in this console log.") : errors.join('\n'));
}

void ServerConsoleTab::exportLog()
{
    QString suggestedName = tr("server-console.log");
    if (m_serverManager && !m_serverId.isEmpty()) {
        const auto server = m_serverManager->getServer(m_serverId);
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
        || output.write(m_output->toPlainText().toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Export Console"), tr("Could not write the selected log file."));
    }
}

void ServerConsoleTab::sendCommand()
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_serverId);
    if (server && server->isRunning()) {
        QString command = m_commandInput->text().trimmed();
        if (!command.isEmpty()) {
            server->writeStdin(command);
            const QString safeCommand = Privacy::sanitizeCommandForDisplay(command);
            server->appendLog(safeCommand);
            appendLine(safeCommand);
            m_commandInput->clear();
        }
    }
}
