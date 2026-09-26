// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2021-2022 Jamie Mansfield <jmansfield@cadixdev.org>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "TechnicPage.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/widgets/ProjectItem.h"
#include "ui_TechnicPage.h"

#include <QKeyEvent>

#include "ui/dialogs/NewInstanceDialog.h"

#include "BuildConfig.h"
#include "Json.h"
#include "StringUtils.h"
#include "TechnicModel.h"
#include "modplatform/technic/SingleZipPackInstallTask.h"
#include "modplatform/technic/SolderPackInstallTask.h"
#include "modplatform/ServerSupport.h"

#include "Application.h"
#include "modplatform/technic/SolderPackManifest.h"
#include "logs/Privacy.h"

#include "net/ApiDownload.h"

TechnicPage::TechnicPage(NewInstanceDialog* dialog, QWidget* parent)
    : QWidget(parent), ui(new Ui::TechnicPage), dialog(dialog), m_fetch_progress(this, false)
{
    ui->setupUi(this);
    ui->serverCompatibilityLabel->setOpenExternalLinks(true);
    ui->serverCompatibilityLabel->setWordWrap(true);
    model = new Technic::ListModel(this);
    model->setShowServerBadges(dialog->isServerModpackMode());
    ui->packView->setModel(model);
    ui->packView->setItemDelegate(new ProjectItemDelegate(this));
    ui->serverCompatibilityLabel->setVisible(
        dialog->isServerModpackMode());
    ui->searchEdit->installEventFilter(this);
    ui->versionSelectionBox->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    ui->versionSelectionBox->view()->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_search_timer.setTimerType(Qt::TimerType::CoarseTimer);
    m_search_timer.setSingleShot(true);

    connect(&m_search_timer, &QTimer::timeout, this, &TechnicPage::triggerSearch);

    m_fetch_progress.hideIfInactive(true);
    m_fetch_progress.setFixedHeight(24);
    m_fetch_progress.progressFormat("");

    ui->verticalLayout->insertWidget(1, &m_fetch_progress);

    connect(ui->packView->selectionModel(), &QItemSelectionModel::currentChanged, this, &TechnicPage::onSelectionChanged);
    connect(ui->versionSelectionBox, &QComboBox::currentTextChanged, this, &TechnicPage::onVersionSelectionChanged);

}

bool TechnicPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui->searchEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return) {
            triggerSearch();
            keyEvent->accept();
            return true;
        } else {
            if (m_search_timer.isActive())
                m_search_timer.stop();

            m_search_timer.start(350);
        }
    }
    return QWidget::eventFilter(watched, event);
}

TechnicPage::~TechnicPage()
{
    delete ui;
}

bool TechnicPage::shouldDisplay() const
{
    return true;
}

void TechnicPage::retranslate()
{
    ui->retranslateUi(this);
}

void TechnicPage::openedImpl()
{
    model->setShowServerBadges(dialog->isServerModpackMode());
    suggestCurrent();
    triggerSearch();
}

void TechnicPage::closedImpl()
{
    Technic::cancelPackDetailsRequests(this);
    model->setShowServerBadges(false);
}

void TechnicPage::triggerSearch()
{
    model->searchWithTerm(ui->searchEdit->text());
    m_fetch_progress.watch(model->activeSearchJob().get());
}

void TechnicPage::onSelectionChanged(QModelIndex first, [[maybe_unused]] QModelIndex second)
{
    ui->versionSelectionBox->clear();

    if (!first.isValid()) {
        ui->serverCompatibilityLabel->clear();
        if (isOpened) {
            dialog->setSuggestedPack();
        }
        return;
    }

    QVariant raw = model->data(first, Qt::UserRole);
    Q_ASSERT(raw.canConvert<Technic::Modpack>());
    current = raw.value<Technic::Modpack>();
    dialog->setServerSupport(ModPlatform::ServerSupport::Unknown, {}, "technic");
    suggestCurrent();
}

void TechnicPage::suggestCurrent()
{
    if (!isOpened) {
        return;
    }
    if (current.broken) {
        dialog->setSuggestedPack();
        return;
    }

    QString editedLogoName = "technic_" + current.logoName;
    model->getLogo(current.logoName, current.logoUrl,
                   [this, editedLogoName](QString logo) { dialog->setSuggestedIconFromFile(logo, editedLogoName); });

    if (current.metadataLoaded) {
        metadataLoaded();
        return;
    }

    QString slug = current.slug;
    Technic::requestPackDetails(APPLICATION->network(), slug, this, [this, slug](std::optional<QJsonObject> details) {
        if (!isOpened) return;
        if (current.slug != slug) {
            return;
        }
        if (!details) {
            CustomMessageBox::selectable(this, tr("Error"), tr("Could not load this Technic pack's details."), QMessageBox::Critical)->exec();
            return;
        }
        QJsonObject obj = *details;
        if (!obj.contains("url")) {
            qWarning() << "Json doesn't contain an url key";
            return;
        }
        QJsonValueRef url = obj["url"];
        if (url.isString()) {
            current.url = url.toString();
        } else {
            if (!obj.contains("solder")) {
                qWarning() << "Json doesn't contain a valid url or solder key";
                return;
            }
            QJsonValueRef solderUrl = obj["solder"];
            if (solderUrl.isString()) {
                current.url = solderUrl.toString();
                current.isSolder = true;
            } else {
                qWarning() << "Json doesn't contain a valid url or solder key";
                return;
            }
        }

        current.minecraftVersion = obj["minecraft"].toString();
        current.serverPackUrl = obj["serverPackUrl"].toString();
        current.websiteUrl = obj["platformUrl"].toString();
        current.author = obj["user"].toString();
        current.description = obj["description"].toString();
        current.currentVersion = obj["version"].toString();
        current.metadataLoaded = true;

        metadataLoaded();
    });
}

// expects current.metadataLoaded to be true
void TechnicPage::metadataLoaded()
{
    QString text = "";
    QString name = current.name;

    if (current.websiteUrl.isEmpty())
        text = name.toHtmlEscaped();
    else
        text = "<a href=\"" + current.websiteUrl.toHtmlEscaped() + "\">" + name.toHtmlEscaped() + "</a>";

    if (!current.author.isEmpty()) {
        text += "<br>" + tr(" by ") + current.author.toHtmlEscaped();
    }

    text += "<br><br>";

    ui->packDescription->setHtml(StringUtils::htmlListPatch(text + current.description));

    // Strip trailing forward-slashes from Solder URL's
    if (current.isSolder) {
        while (current.url.endsWith('/'))
            current.url.chop(1);
    }

    // Display versions from Solder
    if (!current.isSolder) {
        // If the pack isn't a Solder pack, it only has the single version
        ui->versionSelectionBox->addItem(current.currentVersion);
    } else if (current.versionsLoaded) {
        // reverse foreach, so that the newest versions are first
        for (auto i = current.versions.size(); i--;) {
            ui->versionSelectionBox->addItem(current.versions.at(i));
        }
        ui->versionSelectionBox->setCurrentText(current.recommended);
    } else {
        // For now, until the versions are pulled from the Solder instance, display the current
        // version so we can display something quicker
        ui->versionSelectionBox->addItem(current.currentVersion);

        auto netJob = makeShared<NetJob>(QString("Technic::SolderMeta(%1)").arg(current.name), APPLICATION->network());
        auto url = QString("%1/modpack/%2").arg(current.url, current.slug);
        auto [action, response] = Net::ApiDownload::makeByteArray(QUrl(url));
        netJob->addNetAction(action);

        connect(netJob.get(), &NetJob::succeeded, this, [this, response] { onSolderLoaded(response); });
        connect(jobPtr.get(), &NetJob::failed, this,
                [this](QString reason) { CustomMessageBox::selectable(this, tr("Error"), reason, QMessageBox::Critical)->exec(); });

        jobPtr = netJob;
        jobPtr->start();
    }

    selectVersion();
}

void TechnicPage::selectVersion()
{
    if (!isOpened) {
        return;
    }
    if (current.broken) {
        ui->serverCompatibilityLabel->clear();
        dialog->setServerSupport(ModPlatform::ServerSupport::Unknown, {}, "technic");
        dialog->setSuggestedPack();
        return;
    }

    if (dialog->isServerModpackMode()) {
        const QUrl serverUrl = selectedVersion == current.currentVersion ? QUrl(current.serverPackUrl) : QUrl();
        const auto support = ModPlatform::technicServerSupport(serverUrl);
        dialog->setServerSupport(support, serverUrl.toString(), "technic");
        if (support == ModPlatform::ServerSupport::Official) {
            ui->serverCompatibilityLabel->setText(tr("Official server pack"));
        } else if (support == ModPlatform::ServerSupport::Website) {
            ui->serverCompatibilityLabel->setText(tr("<a href=\"%1\">%2</a>")
                .arg(serverUrl.toString().toHtmlEscaped(), tr("Server files on the pack's website")));
        } else {
            ui->serverCompatibilityLabel->setText(tr("No official server pack — built from client files, may not work"));
        }
    }

    if (!current.isSolder) {
        dialog->setSuggestedPack(current.name, selectedVersion,
                                 new Technic::SingleZipPackInstallTask(current.url, current.minecraftVersion,
                                                                      QUrl(current.serverPackUrl)));
    } else {
        const QUrl serverPackUrl = selectedVersion == current.currentVersion ? QUrl(current.serverPackUrl) : QUrl();
        dialog->setSuggestedPack(current.name, selectedVersion,
                                 new Technic::SolderPackInstallTask(APPLICATION->network(), current.url, current.slug, selectedVersion,
                                                                    current.minecraftVersion, serverPackUrl));
    }
}

void TechnicPage::onSolderLoaded(QByteArray* responsePtr)
{
    // NOTE(TheKodeToad): moving the response out to avoid it from being destroyed by jobPtr.reset()
    QByteArray response = std::move(*responsePtr);
    jobPtr.reset();

    auto fallback = [this]() {
        current.versionsLoaded = true;

        current.versions.clear();
        current.versions.append(current.currentVersion);
    };

    current.versions.clear();

    QJsonParseError parse_error{};
    auto doc = QJsonDocument::fromJson(response, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        qWarning() << "Error while parsing JSON response from Solder at" << parse_error.offset << "reason:" << parse_error.errorString();
        qWarning() << "Response body excerpt:"
                   << Privacy::sanitizeResponseBody(response, 2048);
        fallback();
        return;
    }
    auto obj = doc.object();

    TechnicSolder::Pack pack;
    try {
        TechnicSolder::loadPack(pack, obj);
    } catch (const JSONValidationError& err) {
        qCritical() << "Couldn't parse Solder pack metadata:" << err.cause();
        fallback();
        return;
    }

    current.versionsLoaded = true;
    current.recommended = pack.recommended;
    current.versions.append(pack.builds);

    // Finally, let's reload :)
    ui->versionSelectionBox->clear();
    metadataLoaded();
}

void TechnicPage::onVersionSelectionChanged(QString version)
{
    if (version.isNull() || version.isEmpty()) {
        selectedVersion = "";
        ui->serverCompatibilityLabel->clear();
        dialog->setServerSupport(ModPlatform::ServerSupport::Unknown, {}, "technic");
        return;
    }

    selectedVersion = version;
    selectVersion();
}

void TechnicPage::setSearchTerm(QString term)
{
    ui->searchEdit->setText(term);
}

QString TechnicPage::getSerachTerm() const
{
    return ui->searchEdit->text();
}
