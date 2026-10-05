// SPDX-License-Identifier: GPL-3.0-only

#include "CurseForgeSetupPage.h"

#include "Application.h"

#include <QCoreApplication>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
/// The row's messages came from ResourceDownloadDialog, so they keep its translation context.
QString rowTr(const char* text)
{
    return QCoreApplication::translate("ResourceDownloadDialog", text);
}
}  // namespace

CurseForgeSetupPage::CurseForgeSetupPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);

    auto* title = new QLabel(tr("CurseForge needs an API key"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);

    auto* explanation = new QLabel(
        tr("This build does not include a shared CurseForge API key, so its catalog is unavailable. Add your own key in Settings → Services. J Launcher stores it securely in Windows Credential Manager."),
        this);
    explanation->setWordWrap(true);
    explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* setupButton = new QPushButton(tr("Open Services Settings"), this);
    setupButton->setObjectName(QStringLiteral("setupCurseForgeButton"));
    setupButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto* restartHint = new QLabel(
        tr("After saving a valid key, close and reopen this window to load CurseForge."),
        this);
    restartHint->setWordWrap(true);

    layout->addWidget(title);
    layout->addWidget(explanation);
    layout->addSpacing(8);
    layout->addWidget(setupButton);
    layout->addWidget(restartHint);
    layout->addStretch();

    connect(setupButton, &QPushButton::clicked, this,
            [this, setupButton, restartHint]() {
        APPLICATION->ShowGlobalSettings(this, QStringLiteral("apis"));
        if (APPLICATION->capabilities() & Application::SupportsFlame) {
            setupButton->setEnabled(false);
            restartHint->setText(
                tr("CurseForge is enabled. Close and reopen this window to load its catalog."));
        }
    });
}

CurseForgeSetupRow::CurseForgeSetupRow(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 0);
    auto* explanation = new QLabel(
        rowTr("CurseForge is hidden because this build has no CurseForge API key. Add your own key in Settings → Services to enable its catalog and updates."),
        this);
    explanation->setWordWrap(true);
    auto* setupButton = new QPushButton(rowTr("Set Up CurseForge"), this);
    setupButton->setObjectName(QStringLiteral("setupCurseForgeButton"));
    layout->addWidget(explanation, 1);
    layout->addWidget(setupButton);
    connect(setupButton, &QPushButton::clicked, this,
            [this, explanation, setupButton]() {
        APPLICATION->ShowGlobalSettings(this, QStringLiteral("apis"));
        if (APPLICATION->capabilities() & Application::SupportsFlame) {
            explanation->setText(
                rowTr("CurseForge is enabled. Close and reopen this download window to load its catalog."));
            setupButton->setEnabled(false);
        }
    });
}
